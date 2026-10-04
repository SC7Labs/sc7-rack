"""Opt-in source locks remain held until the owning GPU context finishes.

Uses real pinned wlroots locks/release/destroy signals. GPU finish is a small
mock boundary asserting every retained source is still alive before release.
"""
from __future__ import annotations

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class HoldInputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix=".sc7-hold-input-", dir=ROOT)
        cls.directory = Path(cls.work.name)
        cls.client = cls.directory / "hold-client"
        cls.library = ROOT / "vendor/wlroots/build"
        if not (cls.library / "libwlroots.so").is_file():
            raise RuntimeError("build the pinned local wlroots before hold input tests")
        cflags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "pixman-1", "wayland-server", "libdrm"], text=True))
        libs = shlex.split(subprocess.check_output(
            ["pkg-config", "--libs", "pixman-1", "wayland-server"], text=True))
        command = ["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-DWLR_USE_UNSTABLE", "-I", str(ROOT / "vendor/wlroots/include"),
                   "-I", str(ROOT / "vendor/wlroots/build/include"), *cflags,
                   str(ROOT / "tests/render_hold_input.c"),
                   str(ROOT / "tests/render_hold_input_harness.c"),
                   "-o", str(cls.client), "-L", str(cls.library), "-lwlroots", *libs]
        subprocess.run(command, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def run_case(self, case: str, mode: str | None = "hold-input"):
        env = dict(os.environ, LD_LIBRARY_PATH=str(self.library), TMPDIR=str(self.directory))
        env.pop("SC7_RENDER_EXPERIMENT", None)
        if mode is not None:
            env["SC7_RENDER_EXPERIMENT"] = mode
        result = subprocess.run([str(self.client), case], env=env,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("hold input assertions passed", result.stdout)
        return result.stdout

    def test_source_alive_until_gpu_finish_and_released_after(self):
        output = self.run_case("success")
        self.assertLess(output.index("hold-input-gpu-finish-end"),
                        output.index("hold-input-unlock"))

    def test_missing_gpu_finish_retains_sources_and_allows_safe_retry(self):
        self.assertIn("gpu-finish-unavailable", self.run_case("missing-finish"))

    def test_duplicate_source_retained_only_once_and_capacity_is_bounded(self):
        self.assertIn("capacity-64-sources", self.run_case("capacity"))

    def test_rejected_non_dmabuf_does_not_change_locks(self):
        self.assertIn("source-is-not-dmabuf", self.run_case("reject"))

    def test_already_released_source_cannot_be_reclaimed_as_a_sample(self):
        self.assertIn("source-not-consumer-locked", self.run_case("unlocked"))

    def test_finish_still_cleans_pending_sources_after_failed_submission(self):
        self.run_case("failed-submit")

    def test_finish_cleans_pending_sources_even_when_mode_changes(self):
        self.run_case("mode-changed")

    def test_reentrant_hold_and_finish_are_rejected_during_completion(self):
        self.assertIn("finish-in-progress", self.run_case("reentry"))

    def test_repeated_passes_return_every_buffer_lock(self):
        self.run_case("cycles")

    def test_unset_mode_does_not_retain_sources_or_finish_gpu(self):
        self.run_case("disabled", None)

    def test_unknown_mode_does_not_retain_sources_or_finish_gpu(self):
        self.run_case("disabled", "hold-input-typo")


if __name__ == "__main__":
    unittest.main()
