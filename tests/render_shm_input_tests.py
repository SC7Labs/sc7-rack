"""Exercise diagnostic capability gating through real Wayland registries."""
from __future__ import annotations

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RenderShmInputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix=".sc7-shm-input-", dir=ROOT)
        work = Path(cls.work.name)
        cls.library = ROOT / "vendor/wlroots/build"
        cls.preload = work / "shm-input.so"
        cls.client = work / "shm-input-client"
        cflags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "wayland-server", "wayland-client", "pixman-1"],
            text=True))
        libs = shlex.split(subprocess.check_output(
            ["pkg-config", "--libs", "wayland-server", "wayland-client", "pixman-1"],
            text=True))
        common = ["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                  "-DWLR_USE_UNSTABLE", "-I", str(ROOT / "vendor/wlroots/include"),
                  "-I", str(ROOT / "vendor/wlroots/build/include"), *cflags]
        subprocess.run([*common, "-fPIC", "-shared",
                        str(ROOT / "tests/render_shm_input.c"),
                        "-o", str(cls.preload), "-ldl"], check=True)
        subprocess.run([*common, "-Wl,--export-dynamic",
                        str(ROOT / "tests/render_shm_input_harness.c"),
                        "-o", str(cls.client), "-L", str(cls.library), "-lwlroots",
                        *libs], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def run_case(self, mode=None, *, trace=True, empty_trace=False,
                 preload=True, suppressed=False, cycles=1):
        label = f"{mode}-{trace}-{empty_trace}-{preload}-{cycles}"
        log = self.preload.parent / f"{label}.log"
        env = dict(os.environ, LD_LIBRARY_PATH=str(self.library))
        for key in ("LD_PRELOAD", "SC7_RENDER_EXPERIMENT", "SC7_RENDER_TRACE"):
            env.pop(key, None)
        if mode is not None:
            env["SC7_RENDER_EXPERIMENT"] = mode
        if trace:
            env["SC7_RENDER_TRACE"] = "" if empty_trace else str(log)
        if preload:
            # The repo path contains spaces; the loader splits LD_PRELOAD on
            # whitespace even when Python passes the value without a shell.
            env["LD_PRELOAD"] = "./" + self.preload.name
        run = subprocess.run([str(self.client), "suppressed" if suppressed else "normal",
                              str(cycles), str(log)], env=env, cwd=self.preload.parent,
                             capture_output=True,
                             text=True, timeout=20)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("capability assertions passed", run.stdout)
        self.assertIn(f"dmabuf={0 if suppressed else 1} drm={0 if suppressed else 1} shm=1",
                      run.stdout)
        self.assertIn(f"commits={cycles * 2} releases={cycles * 2}", run.stdout)
        self.assertIn("displays=2 existing_filters=preserved", run.stdout)
        self.assertIn("filter_updates=preserved", run.stdout)
        self.assertIn(f"hidden_bind={'rejected' if suppressed else 'allowed'}", run.stdout)
        contents = log.read_text()
        if suppressed:
            self.assertIn(" interface=zwp_linux_dmabuf_v1 allowed=0", contents)
            self.assertIn(" interface=wl_drm allowed=0", contents)
            self.assertIn(" interface=wl_shm allowed=1", contents)
        else:
            self.assertNotIn(" input-capability ", contents)
        return contents, run.stdout

    def test_normal_mode_advertises_dmabuf_and_legacy_drm(self):
        self.run_case()

    def test_observe_mode_advertises_dmabuf_and_legacy_drm(self):
        self.run_case("observe")

    def test_exact_shm_input_mode_suppresses_dmabuf_without_removing_shm(self):
        self.run_case("shm-input", suppressed=True)

    def test_shm_input_remains_usable_for_repeated_real_buffer_commits(self):
        self.run_case("shm-input", suppressed=True, cycles=128)

    def test_misspelled_mode_keeps_normal_capabilities(self):
        self.run_case("shm-input-extra")

    def test_no_trace_environment_keeps_normal_capabilities(self):
        self.run_case("shm-input", trace=False)

    def test_empty_trace_environment_keeps_normal_capabilities(self):
        self.run_case("shm-input", empty_trace=True)

    def test_fresh_process_outside_preload_keeps_host_capabilities(self):
        self.run_case("shm-input", preload=False)


if __name__ == "__main__":
    unittest.main()
