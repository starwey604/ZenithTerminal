"""Offline SD-image tests. No loop devices, privileges, or physical disks."""

import copy
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import zero3e_sd as sd


PARAMETER = """TYPE: GPT
CMDLINE: mtdparts=:0x2000@0x4000(uboot),0x2000@0x6000(misc),0x20000@0x8000(boot),0x40000@0x28000(recovery),0x10000@0x68000(backup),0xc00000@0x78000(rootfs),0x40000@0xc78000(oem),-@0xcb8000(userdata:grow)
uuid:rootfs=614e0000-0000-4b53-8000-1d28000054a9
"""


def fixture_idb(path):
    # Minimal format-valid unit-test DDR/SPL payloads, not executable firmware.
    header = bytearray(2048)
    header[:4] = b"RKNS"
    struct.pack_into("<II", header, 8, (2 << 16) | 384, 1)
    payloads = [b"D" * 512, b"S" * 512]
    for index, payload in enumerate(payloads):
        entry = 120 + index * 88
        struct.pack_into("<I", header, entry, (1 << 16) | (4 + index))
        header[entry + 24:entry + 56] = hashlib.sha256(payload).digest()
    header[1536:1568] = hashlib.sha256(header[:1536]).digest()
    path.write_bytes(header + b"".join(payloads))


def fixture_parts():
    # Compact layout for fast tests; production layout comes from parameter.txt.
    return [dict(name=name, start=16384 + i * 2048, size=2048,
                 **({"uuid": sd.ROOT_UUID} if name == "rootfs" else {}))
            for i, name in enumerate(sd.NAMES)]


def fixture_firmware(directory):
    directory.mkdir()
    for name in sd.NAMES:
        if name == "backup":
            continue
        data = bytearray(4096)
        if name in ("uboot", "boot", "recovery"):
            data[:4] = b"\xd0\x0d\xfe\xed"
            struct.pack_into(">I", data, 4, 4096)
        if name in ("rootfs", "oem", "userdata"):
            data[1080:1082] = b"\x53\xef"
        (directory / f"{name}.img").write_bytes(data)


class ParameterTest(unittest.TestCase):
    def test_preserves_vendor_offsets_and_kernel_uuid(self):
        parts = sd.parse_parameter(PARAMETER, 7 * 1024**3)
        self.assertEqual(parts[5]["start"], 0x78000)
        self.assertEqual(parts[5]["size"], 0xC00000)
        self.assertEqual(parts[5]["uuid"], sd.ROOT_UUID)
        self.assertEqual(parts[-1]["start"] + parts[-1]["size"], 7 * 1024**3 // 512 - 33)

    def test_rejects_insufficient_size_overlap_and_wrong_uuid(self):
        for text, size in (
            (PARAMETER, 6 * 1024**3),
            (PARAMETER.replace("0x6000(misc)", "0x4000(misc)"), 7 * 1024**3),
            (PARAMETER.replace("614e0000", "614f0000"), 7 * 1024**3),
            (PARAMETER.replace("userdata:grow", "userdata"), 7 * 1024**3),
        ):
            with self.subTest(text=text, size=size), self.assertRaises(ValueError):
                sd.parse_parameter(text, size)

    def test_output_cannot_replace_input_or_nonregular_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "source.img"
            source.touch()
            link = Path(tmp) / "link.img"
            link.symlink_to(source)
            for path, forbidden in ((link, []), (source, [source]), (Path('/dev/null'), [])):
                with self.subTest(path=path), self.assertRaises(ValueError):
                    sd.regular_destination(path, forbidden)


@unittest.skipUnless(shutil.which("sfdisk"), "Linux sfdisk is required")
class ImageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.image = root / "sd.img"
        self.idb = root / "idb.img"
        fixture_idb(self.idb)
        self.firmware = root / "firmware"
        fixture_firmware(self.firmware)
        self.parts = fixture_parts()
        self.report = sd.assemble(self.image, 32 * sd.MIB, self.parts, self.idb, self.firmware)

    def test_assembled_payloads_and_both_gpts(self):
        self.assertEqual(self.report["payload_comparison"].split()[0], "PASS")
        self.assertIn("No errors detected", sd.run("sfdisk", "--verify", self.image))

    def test_gpt_and_bootrom_corruption_are_rejected(self):
        cases = (512 + 16, self.image.stat().st_size - 512 + 16,
                 sd.IDB_LBA * 512 + 13, sd.IDB_LBA * 512 + 2048)
        for offset in cases:
            with self.subTest(offset=offset), self.image.open("r+b") as stream:
                stream.seek(offset)
                byte = stream.read(1)
                stream.seek(offset)
                stream.write(bytes([byte[0] ^ 1]))
                stream.flush()
                with self.assertRaises(ValueError):
                    sd.inspect_media(self.image)
                stream.seek(offset)
                stream.write(byte)

    def test_larger_card_relocates_gpt_and_grows_only_userdata(self):
        with self.image.open("r+b") as stream:
            stream.truncate(40 * sd.MIB)
        result = sd.finalize_geometry(self.image, 40 * sd.MIB, self.report["partitions"])
        self.assertEqual(result["partitions"][-1]["start"] + result["partitions"][-1]["size"],
                         40 * sd.MIB // 512 - 33)
        self.assertEqual(result["partitions"][:-1], self.report["partitions"][:-1])
        self.assertEqual(result["partitions"][-1]["uuid"], self.report["partitions"][-1]["uuid"])
        for item in self.report["payloads"]:
            sd.compare_region(self.image, Path(item["source"]), item["offset"])
        self.assertIn("No errors detected", sd.run("sfdisk", "--verify", self.image))

    def test_complete_transfer_and_readback_using_only_a_regular_file(self):
        card = self.image.parent / "card-file.img"
        with card.open("wb") as stream:
            stream.truncate(40 * sd.MIB)
        state = {"device": str(card), "device_id": 0, "target": {"size": card.stat().st_size}}
        plan = {"image_bytes": self.image.stat().st_size,
                "image_partitions": self.report["partitions"], **state}
        # Only hardware identity / ioctl / interactive input are simulated.
        # Copy, full readback, GPT relocation, and userdata growth run for real.
        with patch.object(sd, "device_plan", return_value=plan), \
             patch.object(sd, "target_state", return_value=state), \
             patch.object(sd.os, "geteuid", return_value=0), \
             patch.object(sd.sys.stdin, "isatty", return_value=True), \
             patch("builtins.input", return_value=f"ERASE {card}"), \
             patch("builtins.print"), \
             patch.object(sd.fcntl, "ioctl", return_value=struct.pack("Q", card.stat().st_size)):
            result = sd.write_confirmed(self.image, card)
        self.assertTrue(result["full_image_readback"].startswith("PASS"))
        self.assertEqual(result["disk_bytes"], 40 * sd.MIB)
        for item in self.report["payloads"]:
            sd.compare_region(card, Path(item["source"]), item["offset"])


class WriteGuardTest(unittest.TestCase):
    def setUp(self):
        self.disk = {"name": "/dev/test-only", "type": "disk", "size": 8 * 1024**3,
                     "ro": False, "log-sec": 512, "mountpoints": [None],
                     "children": [{"name": "/dev/test-only1", "type": "part", "mountpoints": [None]}]}

    def test_rejects_mounted_swap_partition_mapper_readonly_small_and_4k(self):
        variants = []
        for key, value in (("type", "part"), ("type", "loop"), ("ro", True),
                           ("size", 1), ("log-sec", 4096)):
            disk = copy.deepcopy(self.disk)
            disk[key] = value
            variants.append(disk)
        for mount in ("/", "/home", "[SWAP]"):
            disk = copy.deepcopy(self.disk)
            disk["children"][0]["mountpoints"] = [mount]
            variants.append(disk)
        disk = copy.deepcopy(self.disk)
        disk["children"][0]["type"] = "crypt"
        variants.append(disk)
        for disk in variants:
            with self.subTest(disk=disk), self.assertRaises(ValueError):
                sd.validate_target_tree(disk, 7 * 1024**3)
        sd.validate_target_tree(self.disk, 7 * 1024**3)

    def test_regular_file_is_never_a_production_write_target(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "not-a-card"
            path.write_bytes(b"keep me")
            with self.assertRaises(ValueError):
                sd.device_plan(path, path)
            self.assertEqual(path.read_bytes(), b"keep me")

    def test_no_writable_open_without_interactive_exact_confirmation(self):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "source.img"
            image.touch()
            for interactive, answer in ((False, ""), (True, ""), (True, "ERASE /dev/wrong")):
                with self.subTest(interactive=interactive, answer=answer), \
                     patch.object(sd, "device_plan", return_value={}), \
                     patch.object(sd.os, "geteuid", return_value=0), \
                     patch.object(sd.sys.stdin, "isatty", return_value=interactive), \
                     patch("builtins.input", return_value=answer), \
                     patch("builtins.print"), \
                     patch.object(sd.os, "open") as writable_open:
                    with self.assertRaises(ValueError):
                        sd.write_confirmed(image, Path("/dev/test-only"))
                    writable_open.assert_not_called()

    def test_changed_device_after_confirmation_is_not_opened_for_writing(self):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "source.img"
            image.touch()
            device = Path("/dev/test-only")
            plan = {"device_id": 1, "device": str(device), "target": {}, "image_bytes": 0}
            with patch.object(sd, "device_plan", return_value=plan), \
                 patch.object(sd, "target_state", return_value={"device_id": 2}), \
                 patch.object(sd.os, "geteuid", return_value=0), \
                 patch.object(sd.sys.stdin, "isatty", return_value=True), \
                 patch("builtins.input", return_value=f"ERASE {device}"), \
                 patch("builtins.print"), \
                 patch.object(sd.os, "open") as writable_open:
                with self.assertRaises(ValueError):
                    sd.write_confirmed(image, device)
                writable_open.assert_not_called()


if __name__ == "__main__":
    unittest.main()
