"""ZERO 3E raw SD image layout, validation, and explicitly confirmed writing."""

import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import time
import uuid
import zlib


SECTOR = 512
MIB = 1024 * 1024
IDB_LBA = 64
ROOT_UUID = "614e0000-0000-4b53-8000-1d28000054a9"
NAMES = ("uboot", "misc", "boot", "recovery", "backup", "rootfs", "oem", "userdata")
LINUX_GUID = "0fc63daf-8483-4772-8e79-3d69d8477de4"
CHUNK = 4 * MIB


def require(condition, message):
    if not condition:
        raise ValueError(message)


def regular_destination(path, forbidden=()):
    require(not path.is_symlink(), f"refusing a symlink destination: {path}")
    require(not path.exists() or stat.S_ISREG(path.stat().st_mode),
            f"output must be a regular file: {path}")
    require(path.resolve() not in {p.resolve() for p in forbidden},
            f"output would replace an input: {path}")


def file_identity(path):
    return stat_identity(path.stat())


def stat_identity(info):
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns


def run(*args, input=None):
    result = subprocess.run([str(arg) for arg in args], input=input, text=True,
                            capture_output=True)
    if result.returncode:
        raise RuntimeError(f"{' '.join(map(str, args))}:\n{result.stderr or result.stdout}")
    return result.stdout


def parse_parameter(text, disk_bytes):
    require(disk_bytes % SECTOR == 0, "disk size must be a multiple of 512 bytes")
    require(re.search(r"^TYPE:\s*GPT\s*$", text, re.M), "parameter must use GPT")
    match = re.search(r"^CMDLINE:\s*mtdparts=:(.+)$", text, re.M)
    require(match, "missing mtdparts in parameter.txt")
    parts = []
    for spec in match[1].strip().split(","):
        item = re.fullmatch(r"(0x[0-9a-fA-F]+|[0-9]+|-)@(0x[0-9a-fA-F]+|[0-9]+)\((\w+)(:grow)?\)", spec)
        require(item, f"unsupported partition specification: {spec}")
        size, start, name, grow = item.groups()
        start = int(start, 0)
        require((size == "-") == bool(grow), "only an explicit final :grow partition may fill the disk")
        count = disk_bytes // SECTOR - 33 - start if grow else int(size, 0)
        require(count > 0, f"image too small for {name}")
        parts.append({"name": name, "start": start, "size": count})
    require(tuple(p["name"] for p in parts) == NAMES, "unexpected ZERO 3E partition order")
    require(parts[-1]["name"] == "userdata" and grow, "userdata must be the final grow partition")
    for before, after in zip(parts, parts[1:]):
        require(before["start"] + before["size"] <= after["start"], "overlapping partitions")
    require(parts[0]["start"] > IDB_LBA, "no room for the SD preloader")
    require(parts[-1]["start"] + parts[-1]["size"] <= disk_bytes // SECTOR - 33,
            "partition exceeds the GPT usable range")
    root = re.search(r"^uuid:rootfs=([0-9a-fA-F-]+)\s*$", text, re.M)
    require(root and str(uuid.UUID(root[1])) == ROOT_UUID, "rootfs UUID differs from the kernel boot contract")
    parts[5]["uuid"] = ROOT_UUID
    return parts


def table_script(parts):
    lines = ["label: gpt", "unit: sectors", "first-lba: 2048", ""]
    for part in parts:
        line = f'start={part["start"]}, size={part["size"]}, type={LINUX_GUID}, name="{part["name"]}"'
        if part.get("uuid"):
            line += f', uuid={part["uuid"]}'
        lines.append(line)
    return "\n".join(lines) + "\n"


def read_at(stream, offset, size):
    stream.seek(offset)
    data = stream.read(size)
    require(len(data) == size, f"short read at byte {offset}")
    return data


def read_gpt(stream, disk_bytes):
    sectors = disk_bytes // SECTOR
    require(disk_bytes % SECTOR == 0 and sectors > 67, "invalid disk length")
    mbr = read_at(stream, 0, SECTOR)
    require(mbr[510:512] == b"\x55\xaa" and mbr[450] == 0xEE, "missing protective MBR")
    tables = []
    headers = []
    for lba, alternate in ((1, sectors - 1), (sectors - 1, 1)):
        header = bytearray(read_at(stream, lba * SECTOR, SECTOR))
        require(header[:8] == b"EFI PART", f"missing GPT header at LBA {lba}")
        length, stored_crc = struct.unpack_from("<II", header, 12)
        require(92 <= length <= SECTOR, "invalid GPT header size")
        struct.pack_into("<I", header, 16, 0)
        # GPT CRCs cover the headers and partition-entry arrays.
        require(zlib.crc32(header[:length]) == stored_crc, "GPT header CRC mismatch")
        current, backup, first, last = struct.unpack_from("<QQQQ", header, 24)
        require((current, backup) == (lba, alternate), "GPT backup is not at the end of this disk")
        require(34 <= first <= last <= sectors - 34, "invalid GPT usable range")
        entry_lba, count, entry_size, crc = struct.unpack_from("<QIII", header, 72)
        require(1 <= count <= 128 and entry_size == 128, "unsupported GPT entry array")
        table_sectors = (count * entry_size + SECTOR - 1) // SECTOR
        require((lba == 1 and 2 <= entry_lba and entry_lba + table_sectors <= first) or
                (lba != 1 and last < entry_lba and entry_lba + table_sectors <= lba),
                "GPT entry array overlaps usable data")
        table = read_at(stream, entry_lba * SECTOR, count * entry_size)
        require(zlib.crc32(table) == crc, "GPT entry-array CRC mismatch")
        headers.append((bytes(header[56:72]), first, last))
        tables.append(table)
    require(headers[0] == headers[1] and tables[0] == tables[1], "primary and backup GPT disagree")
    parts = []
    for offset in range(0, len(tables[0]), 128):
        entry = tables[0][offset:offset + 128]
        if entry[:16] == bytes(16):
            continue
        start, end = struct.unpack_from("<QQ", entry, 32)
        require(headers[0][1] <= start <= end <= headers[0][2], "partition outside GPT usable range")
        parts.append({"name": entry[56:128].decode("utf-16le").rstrip("\0"),
                      "start": start, "size": end - start + 1,
                      "uuid": str(uuid.UUID(bytes_le=entry[16:32])),
                      "type": str(uuid.UUID(bytes_le=entry[:16]))})
    require(tuple(p["name"] for p in parts) == NAMES, "unexpected ZERO 3E GPT layout")
    for before, after in zip(parts, parts[1:]):
        require(before["start"] + before["size"] <= after["start"], "overlapping GPT partitions")
    require(all(p["type"] == LINUX_GUID for p in parts), "unexpected partition type")
    require(parts[5]["uuid"] == ROOT_UUID, "kernel root PARTUUID would not resolve")
    return parts


def check_idb(stream, first_partition_lba):
    base = IDB_LBA * SECTOR
    header = read_at(stream, base, 2048)
    require(header[:4] == b"RKNS", "missing RK35 new-IDB SD preloader at LBA 64")
    description, flags = struct.unpack_from("<II", header, 8)
    hash_offset = (description & 0xFFFF) * 4
    require(description >> 16 == 2 and hash_offset == 1536 and flags == 1,
            "unsupported RKNS header: expected DDR + SPL, SHA256")
    # BootROM consumes these hashes. Validate the boot-format integrity fields.
    require(hashlib.sha256(header[:hash_offset]).digest() == header[hash_offset:hash_offset + 32],
            "BootROM IDB header hash mismatch")
    end = 2048
    for number in range(2):
        entry = 120 + 88 * number
        packed = struct.unpack_from("<I", header, entry)[0]
        offset, size = (packed & 0xFFFF) * SECTOR, (packed >> 16) * SECTOR
        require(size and offset >= end and base + offset + size <= first_partition_lba * SECTOR,
                "IDB payload overlaps GPT/partition data")
        payload = read_at(stream, base + offset, size)
        require(hashlib.sha256(payload).digest() == header[entry + 24:entry + 56],
                f"BootROM IDB payload {number} hash mismatch")
        end = offset + size
    return end


def inspect_media(path, disk_bytes=None):
    if disk_bytes is None:
        require(stat.S_ISREG(path.stat().st_mode), "check expects a regular raw image file")
        disk_bytes = path.stat().st_size
    with path.open("rb") as stream:
        parts = read_gpt(stream, disk_bytes)
        idb_size = check_idb(stream, parts[0]["start"])
        for part in parts:
            base = part["start"] * SECTOR
            if part["name"] in ("uboot", "boot", "recovery"):
                require(read_at(stream, base, 4) == b"\xd0\x0d\xfe\xed",
                        f'{part["name"]} is not a FIT image')
                size = struct.unpack(">I", read_at(stream, base + 4, 4))[0]
                require(40 <= size <= part["size"] * SECTOR, "FIT exceeds partition")
            if part["name"] in ("rootfs", "oem", "userdata"):
                require(read_at(stream, base + 1024 + 56, 2) == b"\x53\xef",
                        f'{part["name"]} has no ext4 superblock')
    return {"image": str(path), "disk_bytes": disk_bytes, "sector_bytes": SECTOR,
            "idb_lba": IDB_LBA, "idb_bytes": idb_size, "partitions": parts,
            "validation": "PASS", "board_boot": "not tested"}


def compare_region(image, source, offset):
    with image.open("rb") as dest, source.open("rb") as src:
        dest.seek(offset)
        while data := src.read(CHUNK):
            require(dest.read(len(data)) == data, f"image payload differs from {source}")


def assemble(output, disk_bytes, parts, idb, firmware):
    require(idb.stat().st_size + IDB_LBA * SECTOR <= parts[0]["start"] * SECTOR,
            "IDB exceeds the bootloader gap")
    sources = [(idb, IDB_LBA * SECTOR)]
    for part in parts:
        if part["name"] == "backup":  # SDK package-file marks this partition RESERVED.
            continue
        source = firmware / (part["name"] + ".img")
        require(source.is_file() and 0 < source.stat().st_size <= part["size"] * SECTOR,
                f"missing or oversized partition image: {source}")
        sources.append((source, part["start"] * SECTOR))
    with output.open("xb") as stream:
        stream.truncate(disk_bytes)  # Large gaps remain sparse, but logically contain zeros.
    run("sfdisk", "--no-reread", "--no-tell-kernel", "--wipe", "never",
        "--wipe-partitions", "never", output, input=table_script(parts))
    with output.open("r+b") as dest:
        for source, offset in sources:
            dest.seek(offset)
            with source.open("rb") as src:
                shutil.copyfileobj(src, dest, CHUNK)
        dest.flush()
        os.fsync(dest.fileno())
    report = inspect_media(output)
    for source, offset in sources:
        compare_region(output, source, offset)
    for expected, actual in zip(parts, report["partitions"]):
        require(all(expected[key] == actual[key] for key in ("name", "start", "size")),
                "partition differs from SDK parameter.txt")
    report["payload_comparison"] = "PASS (IDB and every included partition image)"
    report["payloads"] = [{"source": str(source), "offset": offset,
                           "bytes": source.stat().st_size} for source, offset in sources]
    return report


def build_image(sdk, output, size_mib):
    firmware = sdk / "output/firmware"
    config = (sdk / "output/.config").read_text()
    require('RK_KERNEL_DTS_NAME="rk3566-radxa-zero-3e"' in config, "SDK is not configured for ZERO 3E")
    regular_destination(output, firmware.glob("*"))
    idb_output = output.with_suffix(".idbloader.img")
    regular_destination(idb_output, firmware.glob("*"))
    disk_bytes = size_mib * MIB
    parts = parse_parameter((firmware / "parameter.txt").read_text(), disk_bytes)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".zero3e-sd-", dir=output.parent) as tmp:
        work = Path(tmp)
        idb = work / "idbloader.img"
        # Convert the exact built USB-loader package; do not dd it as an IDB.
        conversion = run(sdk / "rkbin/tools/boot_merger", "idb", "-l", "-n", "-o",
                         idb.resolve(), firmware / "MiniLoaderAll.bin")
        require(idb.is_file(), f"boot_merger did not generate an IDB:\n{conversion}")
        report = assemble(work / "sd.img", disk_bytes, parts, idb, firmware)
        os.replace(work / "sd.img", output)
        os.replace(idb, idb_output)
    report.update(image=str(output), sdk=str(sdk), idb_file=str(idb_output),
                  allocated_bytes=output.stat().st_blocks * SECTOR, physical_write="not performed")
    report["payloads"][0]["source"] = str(idb_output)
    return report


def walk_devices(device):
    yield device
    for child in device.get("children", []):
        yield from walk_devices(child)


def validate_target_tree(device, image_bytes):
    require(device["type"] == "disk", "target must be a whole physical disk, not a partition/loop/mapper")
    require(not device["ro"], "target is read-only")
    require(device["size"] >= image_bytes, "target is smaller than the raw image")
    require(device["log-sec"] == SECTOR, "target must expose 512-byte logical sectors")
    for node in walk_devices(device):
        require(not any(node.get("mountpoints") or []), f'mounted or swap device: {node["name"]}')
        require(node["type"] in ("disk", "part"), f'active stacked device: {node["name"]}')


def target_state(device, image_bytes):
    require(stat.S_ISBLK(device.stat().st_mode), "target is not a block device")
    target = json.loads(run("lsblk", "--json", "--bytes", "--paths", "--output",
                            "NAME,TYPE,SIZE,MODEL,SERIAL,TRAN,RM,RO,LOG-SEC,MOUNTPOINTS", device))["blockdevices"]
    require(len(target) == 1, "ambiguous device identity")
    validate_target_tree(target[0], image_bytes)
    return {"device": str(device), "device_id": device.stat().st_rdev, "target": target[0]}


def device_plan(image, device):
    require(stat.S_ISBLK(device.stat().st_mode), "target is not a block device")
    report = inspect_media(image)
    state = target_state(device, report["disk_bytes"])
    return {"image": str(image), "image_bytes": report["disk_bytes"],
            "image_partitions": report["partitions"], **state,
            "action": "replace partition table and image area, read back, relocate GPT, expand userdata"}


def finalize_geometry(path, disk_bytes, original, parent_locked=False):
    """Production caller holds O_EXCL + flock across both GPT operations."""
    # sfdisk's initial in-use test can reread partitions too. Suppress both
    # rereads while the caller's exclusive claim prevents new mounts. Its lock
    # must stay held across both subprocesses; do not reacquire it in the child.
    options = ["--no-reread", "--no-tell-kernel", "--lock=no" if parent_locked else "--lock=yes"]
    run("sfdisk", *options, "--relocate", "gpt-bak-std", path)
    last = original[-1]
    count = disk_bytes // SECTOR - 33 - last["start"]
    run("sfdisk", *options, "--wipe", "never", "--wipe-partitions", "never",
        "-N", str(len(original)), path, input=f'start={last["start"]}, size={count}\n')
    result = inspect_media(path, disk_bytes)
    for before, after in zip(original, result["partitions"]):
        keys = ("name", "start", "uuid", "type") if before is last else ("name", "start", "size", "uuid", "type")
        require(all(before[key] == after[key] for key in keys), "GPT repair changed another partition")
    require(result["partitions"][-1]["size"] == count, "userdata was not extended to disk capacity")
    return result


def write_confirmed(image, device):
    source_stat = file_identity(image)
    plan = device_plan(image, device)
    require(file_identity(image) == source_stat, "image changed while inspecting it; start again")
    print(json.dumps(plan, indent=2), flush=True)
    require(os.geteuid() == 0, "writing requires root; plan is read-only")
    require(sys.stdin.isatty(), "writing requires an interactive device confirmation")
    options = run("sfdisk", "--help")
    require(all(flag in options for flag in ("--relocate", "--lock", "--no-tell-kernel")),
            "sfdisk lacks the required GPT relocation/locking options")
    phrase = f"ERASE {device}"
    answer = input(f"ALL DATA WILL BE LOST. Type exactly {phrase}: ")
    require(answer == phrase, "cancelled; no disk was written")
    expected_target = {key: plan[key] for key in ("device", "device_id", "target")}
    require(target_state(device, plan["image_bytes"]) == expected_target,
            "device changed after confirmation; start again")
    require(file_identity(image) == source_stat, "image changed after confirmation; start again")
    with image.open("rb") as src:
        require(stat_identity(os.fstat(src.fileno())) == source_stat,
                "image was replaced before open; start again")
        fd = os.open(device, os.O_RDWR | os.O_EXCL)
        try:
            require(os.fstat(fd).st_rdev == plan["device_id"], "device identity changed before open")
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            size = struct.unpack("Q", fcntl.ioctl(fd, 0x80081272, bytes(8)))[0]  # BLKGETSIZE64
            require(size == plan["target"]["size"], "device capacity changed before write")
            with os.fdopen(fd, "r+b", closefd=False) as dest:
                copied = 0
                last_print = 0
                while data := src.read(CHUNK):
                    dest.write(data)
                    copied += len(data)
                    now = time.monotonic()
                    if now - last_print >= 1:
                        print(f"Writing {copied}/{plan['image_bytes']} bytes", flush=True)
                        last_print = now
                dest.flush()
                os.fsync(fd)
                require(file_identity(image) == source_stat, "image changed during write; do not boot this card")
                print("Reading back the complete written image...", flush=True)
                fcntl.ioctl(fd, 0x1261)  # BLKFLSBUF: invalidate block cache before real card readback.
                src.seek(0)
                dest.seek(0)
                while data := src.read(CHUNK):
                    require(dest.read(len(data)) == data, "readback mismatch; do not boot this card")
                require(file_identity(image) == source_stat, "image changed during readback; do not boot this card")
            require(target_state(device, plan["image_bytes"]) == expected_target,
                    "target state changed before GPT adjustment; do not boot this card")
            report = finalize_geometry(device, plan["target"]["size"], plan["image_partitions"], parent_locked=True)
        finally:
            os.close(fd)
    report.update(full_image_readback="PASS (before GPT capacity adjustment)", physical_write="completed")
    return report
