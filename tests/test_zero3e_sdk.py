"""Patch-stack tests: python3 -m unittest discover -s tests -p test_zero3e_sdk.py."""

import importlib.machinery
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("zero3e_sdk", str(ROOT / "tools/zero3e-sdk"))
spec = importlib.util.spec_from_loader(loader.name, loader)
sdk_tool = importlib.util.module_from_spec(spec)
loader.exec_module(sdk_tool)


class PatchStackTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.sdk = self.root / "sdk"
        (self.sdk / ".repo").mkdir(parents=True)
        self.repo = self.sdk / "buildroot"
        self.repo.mkdir()
        self.git("init", "-q")
        self.git("config", "user.name", "Test")
        self.git("config", "user.email", "test@example.invalid")
        (self.repo / "existing").write_text("base\n")
        self.git("add", ".")
        self.git("commit", "-qm", "baseline")
        self.base = self.git("rev-parse", "HEAD").strip()
        self.patches = self.root / "patches"
        self.patches.mkdir()
        # Patch two depends on the new file from patch one.
        (self.repo / "new").write_text("first\n")
        self.git("add", ".")
        (self.patches / "0001.patch").write_text(self.git("diff", "--cached"))
        self.git("commit", "-qm", "first")
        (self.repo / "new").write_text("second\n")
        (self.patches / "0002.patch").write_text(self.git("diff"))
        # This is a disposable fixture repository, not the real SDK.
        self.git("reset", "--hard", self.base)
        self.series = self.root / "series.tsv"
        self.series.write_text(f"buildroot|{self.base}|patches\n")

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.repo), *args], text=True)

    def inspect(self):
        return sdk_tool.inspect(self.sdk, self.series)

    def test_sequential_stack_and_idempotence(self):
        self.assertEqual(self.inspect()[0][2], "ready")
        self.assertFalse((self.repo / "new").exists())
        for patch in sorted(self.patches.glob("*.patch")):
            self.git("apply", str(patch))
        self.assertEqual(self.inspect()[0][2], "applied")
        self.assertEqual(self.git("diff", "--cached"), "")

    def test_untracked_collision_is_preserved(self):
        (self.repo / "new").write_text("someone else's work\n")
        with self.assertRaises(ValueError):
            self.inspect()
        self.assertEqual((self.repo / "new").read_text(), "someone else's work\n")

    def test_partial_stack_is_rejected(self):
        self.git("apply", str(self.patches / "0001.patch"))
        with self.assertRaises(ValueError):
            self.inspect()

    def test_staged_collision_is_rejected(self):
        for patch in sorted(self.patches.glob("*.patch")):
            self.git("apply", str(patch))
        self.git("add", "new")
        with self.assertRaises(ValueError):
            self.inspect()

    def test_unrelated_work_is_untouched(self):
        (self.repo / "existing").write_text("local change\n")
        (self.repo / "unrelated").write_text("local file\n")
        self.assertEqual(self.inspect()[0][2], "ready")
        self.assertEqual((self.repo / "existing").read_text(), "local change\n")

    def test_baseline_mismatch_is_rejected(self):
        self.git("commit", "--allow-empty", "-qm", "other baseline")
        with self.assertRaises(ValueError):
            self.inspect()

    def test_export_includes_new_files_without_touching_real_index(self):
        (self.repo / "new").write_text("export me\n")
        (self.repo / "unrelated").write_text("not exported\n")
        output = self.root / "export.patch"
        sdk_tool.export_patch(self.sdk, self.series, "buildroot", ["new"], output)
        self.assertIn("+export me", output.read_text())
        self.assertNotIn("unrelated", output.read_text())
        self.assertEqual(self.git("diff", "--cached"), "")
        with self.assertRaises(FileExistsError):
            sdk_tool.export_patch(self.sdk, self.series, "buildroot", ["new"], output)


class BringupOverlayTest(unittest.TestCase):
    def test_project_gating_handles_vendor_exported_quotes(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp) / "buildroot"
            repo.mkdir()
            subprocess.run(["git", "init", "-q", str(repo)], check=True)
            subprocess.run(["git", "-C", str(repo), "apply", str(ROOT /
                "patches/radxa-zero3e/buildroot/0001-zenith-zero3e-bringup.patch")], check=True)
            script = repo / "board/rockchip/common/overlays/90-zenith-zero3e/prepare.sh"
            for name, quoted, expected in (
                ("rockchip_rk3566_zenith", False, 0),
                ("rockchip_rk3566_zenith", True, 0),
                ("rockchip_rk3566", True, 1),
                ("rockchip_rk3566_recovery", True, 1),
            ):
                with self.subTest(name=name, quoted=quoted):
                    target = Path(tmp) / name
                    marker = target / "etc/init.d/S05async-commit.sh"
                    marker.parent.mkdir(parents=True, exist_ok=True)
                    marker.touch()
                    config = f"/sdk/configs/{name}_defconfig"
                    if quoted:
                        config = f'"{config}"'
                    result = subprocess.run([str(script)], env={
                        "PATH": "/usr/bin:/bin", "TARGET_DIR": str(target),
                        "BR2_DEFCONFIG": config,
                    })
                    self.assertEqual(result.returncode, expected)
                    self.assertEqual(marker.exists(), expected != 0)


if __name__ == "__main__":
    unittest.main()
