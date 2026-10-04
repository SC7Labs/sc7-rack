#!/usr/bin/env python3
"""Exercise isolated sampling and state restoration on real software Mesa GLES."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "vendor/wlroots"


class RenderInputCaptureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (VENDOR / "build/include").exists():
            raise unittest.SkipTest("build pinned wlroots headers before GLES capture tests")
        cls.workspace = tempfile.TemporaryDirectory(prefix=".sc7-input-capture-", dir=ROOT)
        cls.binary = Path(cls.workspace.name) / "capture-harness"
        flags = shlex.split(subprocess.check_output([
            "pkg-config", "--cflags", "--libs", "egl", "glesv2", "pixman-1",
        ], text=True))
        command = [
            "cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-DWLR_USE_UNSTABLE", "-I", str(VENDOR / "include"),
            "-I", str(VENDOR / "build/include"),
            str(ROOT / "tests/render_input_capture.c"),
            str(ROOT / "tests/render_input_capture_harness.c"),
            *flags, "-Wl,--wrap=glCheckFramebufferStatus", "-Wl,--wrap=rename",
            "-o", str(cls.binary),
        ]
        result = subprocess.run(command, text=True, capture_output=True)
        if result.returncode:
            cls.workspace.cleanup()
            raise RuntimeError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def check_case(self, name, version="2.0"):
        environment = os.environ.copy()
        environment.pop("LD_PRELOAD", None)
        environment["LIBGL_ALWAYS_SOFTWARE"] = "1"
        environment["MESA_GLES_VERSION_OVERRIDE"] = version
        path = Path(self.workspace.name) / f"{name}-{version}.ppm"
        if name == "write-failure":
            path = Path(self.workspace.name) / "does-not-exist" / "capture.ppm"
        result = subprocess.run([str(self.binary), name, str(path)], env=environment,
                                text=True, capture_output=True, timeout=30)
        if result.returncode == 77:
            self.skipTest("surfaceless software EGL unavailable")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), "PASS")
        self.assertFalse(Path(f"{path}.tmp").exists())
        self.assertFalse(Path(f"{path}.alpha.pgm.tmp").exists())

    def test_exact_native_pixels_and_orientation(self):
        self.check_case("normal")

    def test_nondefault_gles2_state_restored(self):
        self.check_case("polluted")

    def test_gles3_sampler_pbos_separate_fbos_and_integer_vao_restored(self):
        self.check_case("polluted", "3.2")

    def test_external_eglimage_sampling_and_state_restoration(self):
        self.check_case("external")

    def test_argb_rgb_and_alpha_sidecar_preserve_transparent_and_premul_pixels(self):
        self.check_case("alpha")

    def test_rgbx_alpha_is_opaque_as_in_wlroots_rgbx_shader(self):
        self.check_case("rgbx-alpha")

    def test_first_publish_failure_leaves_no_snapshot_pair(self):
        self.check_case("publish-failure-1")

    def test_second_publish_failure_removes_published_alpha_and_all_temporaries(self):
        self.check_case("publish-failure-2")

    def test_incomplete_fbo_restores_state(self):
        self.check_case("fbo-failure")
        self.check_case("fbo-failure", "3.2")

    def test_image_write_failure_restores_state(self):
        self.check_case("write-failure")

    def test_dimensions_and_non_gles_rejected_without_state_change(self):
        self.check_case("bad-dimensions")
        self.check_case("not-gles")

    def test_preexisting_gl_error_reported_without_state_change(self):
        self.check_case("prior-error")

    def test_repeated_captures_keep_state_and_pixels(self):
        self.check_case("repeated")


if __name__ == "__main__":
    unittest.main()
