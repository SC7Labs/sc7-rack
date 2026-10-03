#!/usr/bin/env python3
"""Check diagnostic resource experiments against pinned real wlroots lifetimes."""

from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "vendor/wlroots"


class RenderOutputExperimentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (VENDOR / "build/libwlroots.so").exists():
            raise unittest.SkipTest("build pinned local wlroots before lifetime tests")
        # Temporary mounts may be noexec; match the other compiled harnesses.
        cls.workspace = tempfile.TemporaryDirectory(prefix=".sc7-render-output-", dir=ROOT)
        cls.binary = Path(cls.workspace.name) / "resource-harness"
        flags = shlex.split(subprocess.check_output([
            "pkg-config", "--cflags", "--libs", "pixman-1", "wayland-server",
        ], text=True))
        command = [
            "cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-DWLR_USE_UNSTABLE", "-I", str(VENDOR / "include"),
            "-I", str(VENDOR / "build/include"),
            str(ROOT / "tests/render_output_experiment.c"),
            str(ROOT / "tests/render_output_experiment_harness.c"),
            "-L", str(VENDOR / "build"),
            f"-Wl,-rpath,{VENDOR / 'build'}", "-Wl,--wrap=calloc",
            "-lwlroots", *flags, "-o", str(cls.binary),
        ]
        compiled = subprocess.run(command, text=True, capture_output=True)
        if compiled.returncode:
            cls.workspace.cleanup()
            raise RuntimeError(f"resource harness compilation failed:\n{compiled.stderr}")

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def check_case(self, name):
        environment = os.environ.copy()
        environment.pop("LD_PRELOAD", None)
        environment["LD_LIBRARY_PATH"] = str(VENDOR / "build")
        result = subprocess.run([str(self.binary), name], env=environment,
                                text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), "PASS")

    def test_default_and_non_gles2_are_passthrough(self):
        self.check_case("observe")

    def test_wrapper_keeps_source_until_target_cache_destroy(self):
        self.check_case("target-lifetime")

    def test_failed_import_allocation_and_begin_release_source(self):
        self.check_case("target-failure")

    def test_repeated_target_recreation_releases_every_backing_lock(self):
        self.check_case("target-repeated")

    def test_other_modes_preserve_swapchain_storage(self):
        self.check_case("swapchain-observe")

    def test_acquired_and_locked_slots_are_never_retired(self):
        self.check_case("swapchain-safety")

    def test_repeated_real_acquire_host_release_recreate_has_age_zero(self):
        self.check_case("swapchain-cycles")

    def test_full_swapchain_and_allocation_failure_preserve_host_storage(self):
        self.check_case("swapchain-failure")


if __name__ == "__main__":
    unittest.main()
