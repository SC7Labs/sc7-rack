"""Raw buffer snapshots use supported wlroots READ access and balanced locks.

Actual pinned wlroots SHM storage exercises all supported channel layouts.
Synthetic buffers exercise unavailable/rejected access, malformed layouts,
bounded capture, cleanup and repeated use without a GPU or raw-FD mapping.
"""
from __future__ import annotations

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RawSnapshotTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # System /tmp is noexec on the development machine.
        cls.work = tempfile.TemporaryDirectory(prefix=".sc7-raw-snapshot-", dir=ROOT)
        cls.directory = Path(cls.work.name)
        cls.client = cls.directory / "snapshot-client"
        cls.library = ROOT / "vendor/wlroots/build"
        if not (cls.library / "libwlroots.so").is_file():
            raise RuntimeError("build the pinned local wlroots before snapshot tests")
        cflags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "pixman-1", "wayland-server", "libdrm"], text=True))
        libs = shlex.split(subprocess.check_output(
            ["pkg-config", "--libs", "pixman-1", "wayland-server"], text=True))
        command = ["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-DWLR_USE_UNSTABLE", "-I", str(ROOT / "vendor/wlroots/include"),
                   "-I", str(ROOT / "vendor/wlroots/build/include"), *cflags,
                   str(ROOT / "tests/render_raw_snapshot.c"),
                   str(ROOT / "tests/render_raw_snapshot_harness.c"),
                   "-o", str(cls.client), "-L", str(cls.library), "-lwlroots", *libs]
        subprocess.run(command, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def run_case(self, case: str, path: Path | None = None):
        path = path or self.directory / (case + ".ppm")
        env = dict(os.environ, LD_LIBRARY_PATH=str(self.library), TMPDIR=str(self.directory))
        result = subprocess.run([str(self.client), case, str(path)], env=env,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("raw snapshot assertions passed", result.stdout)
        self.assertFalse(list(self.directory.glob("*.tmp-*")))
        return path, result.stdout

    def assert_pixels(self, path: Path, opaque=False):
        self.assertEqual(path.read_bytes(), b"P6\n2 2\n255\n" +
                         bytes.fromhex("102030 405060 708090 a0b0c0"))
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        alpha = Path(str(path) + ".alpha.pgm")
        expected_alpha = bytes([255] * 4) if opaque else bytes([0, 128, 192, 255])
        self.assertEqual(alpha.read_bytes(), b"P5\n2 2\n255\n" + expected_alpha)
        self.assertEqual(alpha.stat().st_mode & 0o777, 0o600)

    def test_actual_shm_argb_preserves_channels_without_alpha_correction(self):
        self.assert_pixels(self.run_case("shm-argb")[0])

    def test_actual_shm_xrgb_preserves_channels_ignoring_x_byte(self):
        self.assert_pixels(self.run_case("shm-xrgb")[0], opaque=True)

    def test_actual_shm_abgr_swaps_red_blue_correctly(self):
        self.assert_pixels(self.run_case("shm-abgr")[0])

    def test_actual_shm_xbgr_swaps_red_blue_correctly(self):
        self.assert_pixels(self.run_case("shm-xbgr")[0], opaque=True)

    def test_padded_unaligned_row_pitch_skips_padding(self):
        self.assert_pixels(self.run_case("pitch")[0])

    def test_no_supported_access_reports_unavailable_without_reading(self):
        path, output = self.run_case("unavailable")
        self.assertIn("data-pointer-implementation-unavailable", output)
        self.assertFalse(path.exists())

    def test_rejected_read_access_never_calls_end_or_releases_source(self):
        path, output = self.run_case("reject")
        self.assertIn("data-pointer-read-access-failed", output)
        self.assertFalse(path.exists())

    def test_unsupported_formats_end_read_access(self):
        for case in ("format", "big-endian-format"):
            with self.subTest(case=case):
                path, output = self.run_case(case)
                self.assertIn("unsupported-raw-pixel-format", output)
                self.assertFalse(path.exists())

    def test_invalid_layouts_end_read_access_without_output(self):
        for case in ("stride", "overflow", "null-data"):
            with self.subTest(case=case):
                path, output = self.run_case(case)
                self.assertIn("invalid-data-pointer-or-row-layout", output)
                self.assertFalse(path.exists())

    def test_invalid_bounds_lock_state_and_active_access_are_rejected(self):
        for case in ("dimensions", "zero-dimension", "busy", "unlocked",
                     "null-buffer", "empty-path"):
            with self.subTest(case=case):
                self.assertFalse(self.run_case(case)[0].exists())

    def test_open_and_rename_failures_cleanup_owned_temporary_file(self):
        missing = self.directory / "absent" / "frame.ppm"
        self.assertIn("snapshot-file-write-failed", self.run_case("file-failure", missing)[1])
        target_directory = self.directory / "rename-target"
        target_directory.mkdir()
        self.assertIn("snapshot-file-write-failed",
                      self.run_case("file-failure", target_directory)[1])
        self.assertTrue(target_directory.is_dir())

    def test_repeated_snapshots_preserve_lock_and_access_lifetime(self):
        self.assert_pixels(self.run_case("cycles")[0])

    def test_alpha_write_failure_does_not_publish_or_replace_rgb(self):
        # RGB temporary fits NAME_MAX, alpha temporary does not. Both files
        # must finish writing before either final path can change.
        path = self.directory / ("a" * 240)
        path.write_bytes(b"existing-rgb")
        self.assertIn("snapshot-file-write-failed", self.run_case("file-failure", path)[1])
        self.assertEqual(path.read_bytes(), b"existing-rgb")
        self.assertFalse(Path(str(path) + ".alpha.pgm").exists())

    def test_second_rename_failure_removes_new_rgb_capture(self):
        path = self.directory / "alpha-rename-failure.ppm"
        sidecar = Path(str(path) + ".alpha.pgm")
        sidecar.mkdir()
        self.assertIn("snapshot-file-write-failed", self.run_case("file-failure", path)[1])
        self.assertFalse(path.exists())
        self.assertTrue(sidecar.is_dir())


if __name__ == "__main__":
    unittest.main()
