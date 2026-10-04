"""Correlate actual pass-submit release callbacks with the composed frame."""
from __future__ import annotations

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

from render_damage_tests import build_trace

ROOT = Path(__file__).resolve().parents[1]


class RenderFrameCorrelationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix=".sc7-frame-correlation-", dir=ROOT)
        work = Path(cls.work.name)
        cls.trace = build_trace(work)
        cls.library = ROOT / "vendor/wlroots/build"
        cls.client = work / "frame-client"
        cflags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "pixman-1", "wayland-server"], text=True))
        libs = shlex.split(subprocess.check_output(
            ["pkg-config", "--libs", "pixman-1", "wayland-server"], text=True))
        subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-DWLR_USE_UNSTABLE", "-I", str(ROOT / "vendor/wlroots/include"),
                        "-I", str(ROOT / "vendor/wlroots/build/include"), *cflags,
                        str(ROOT / "tests/render_frame_correlation_harness.c"),
                        "-o", str(cls.client), "-L", str(cls.library), "-lwlroots",
                        *libs, "-ldl"], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def run_cycles(self, cycles, late_capture=False):
        log = self.trace.parent / f"frames-{cycles}.log"
        env = dict(os.environ, LD_PRELOAD="./" + self.trace.name,
                   LD_LIBRARY_PATH=str(self.library), SC7_RENDER_TRACE=str(log))
        for key in ("SC7_RENDER_EXPERIMENT", "SC7_RENDER_CAPTURE_TRIGGER",
                    "SC7_RENDER_CAPTURE_DIR", "SC7_RENDER_CAPTURE_INPUTS",
                    "SC7_TEST_LATE_TRIGGER", "SC7_RENDER_INPUT_CAPTURE_TRIGGER"):
            env.pop(key, None)
        if late_capture:
            captures = self.trace.parent / "late-capture"
            captures.mkdir()
            trigger = self.trace.parent / "late-trigger"
            env.update(SC7_RENDER_CAPTURE_INPUTS="1", SC7_RENDER_CAPTURE_DIR=str(captures),
                       SC7_RENDER_CAPTURE_TRIGGER=str(trigger), SC7_TEST_LATE_TRIGGER=str(trigger))
        run = subprocess.run([str(self.client), str(cycles)], env=env,
                             cwd=self.trace.parent, capture_output=True, text=True, timeout=15)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        callbacks = re.findall(r"release-callback frame=(\d+) expected=(\d+)", run.stdout)
        self.assertEqual(callbacks, [(str(i), str(i)) for i in range(1, cycles + 1)])
        lines = log.read_text().splitlines()
        begins = [int(re.search(r" frame=(\d+)", line)[1]) for line in lines
                  if " render-begin " in line]
        submits = [int(re.search(r" frame=(\d+)", line)[1]) for line in lines
                   if " render-submit " in line]
        self.assertEqual(begins, list(range(1, cycles + 1)))
        self.assertEqual(submits, list(range(1, cycles + 1)))
        if late_capture:
            self.assertFalse(trigger.exists())
            self.assertEqual([path.name for path in captures.glob("*.ppm")],
                             ["pre-submit-000000000002.ppm"])
            self.assertTrue(any(" pre-submit frame=2 " in line and " ok=1" in line
                                for line in lines))

    def test_release_during_submit_keeps_this_frame(self):
        self.run_cycles(1)

    def test_repeated_passes_keep_begin_submit_and_release_correlated(self):
        self.run_cycles(128)

    def test_input_request_after_scene_start_waits_for_next_whole_frame(self):
        self.run_cycles(2, late_capture=True)


if __name__ == "__main__":
    unittest.main()
