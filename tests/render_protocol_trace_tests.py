"""Actual Wayland release events must not be inferred from internal signals."""
from __future__ import annotations

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RenderProtocolTraceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix=".sc7-protocol-trace-", dir=ROOT)
        work = Path(cls.work.name)
        cls.library = ROOT / "vendor/wlroots/build"
        cflags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "pixman-1", "wayland-server", "wayland-client"], text=True))
        libs = shlex.split(subprocess.check_output(
            ["pkg-config", "--libs", "pixman-1", "wayland-server", "wayland-client"], text=True))
        common = ["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                  "-DWLR_USE_UNSTABLE", "-I", str(ROOT / "vendor/wlroots/include"),
                  "-I", str(ROOT / "vendor/wlroots/build/include"), *cflags]
        cls.preload = work / "protocol-trace.so"
        cls.client = work / "protocol-client"
        subprocess.run([*common, "-fPIC", "-shared",
                        str(ROOT / "tests/render_protocol_trace.c"),
                        "-o", str(cls.preload), "-ldl"], check=True)
        subprocess.run([*common, "-Wl,--export-dynamic",
                        str(ROOT / "tests/render_protocol_trace_harness.c"),
                        "-o", str(cls.client), "-L", str(work), "-l:protocol-trace.so",
                        "-L", str(cls.library), "-lwlroots", *libs, "-ldl"], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def run_case(self, case, enabled=True):
        log = self.preload.parent / f"{case}-{enabled}.log"
        env = dict(os.environ, LD_LIBRARY_PATH=os.pathsep.join(
            [str(self.preload.parent), str(self.library)]))
        env.pop("SC7_RENDER_TRACE", None)
        if enabled:
            env["SC7_RENDER_TRACE"] = str(log)
        run = subprocess.run([str(self.client), case, str(log)], env=env,
                             capture_output=True, text=True, timeout=15)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("protocol assertions passed", run.stdout)
        return log.read_text().splitlines(), run.stdout

    def test_exact_wire_release_is_separate_from_internal_signal(self):
        lines, output = self.run_case("single")
        self.assertIn("wire=1 internal=2", output)
        wire = [line for line in lines if " wl-buffer-release-sent " in line]
        internal = [line for line in lines if " buffer-release " in line]
        self.assertEqual(len(wire), 1, lines)
        self.assertEqual(len(internal), 2, lines)
        self.assertIn(" frame=447 ", wire[0])
        self.assertIn(" generation=1", wire[0])
        self.assertRegex(wire[0], r"pid=\d+ .*buffer_id=\d+ source=0x[0-9a-f]+")

    def test_source_lock_defers_actual_release_until_final_unlock(self):
        lines, output = self.run_case("hold")
        self.assertIn("wire=1 internal=2", output)
        applied = [i for i, line in enumerate(lines) if " wlroots-applied-commit " in line]
        releases = [i for i, line in enumerate(lines) if " wl-buffer-release-sent " in line]
        self.assertEqual(len(releases), 1, lines)
        self.assertGreater(releases[0], applied[0])

    def test_destroyed_protocol_resource_suppresses_wire_event_but_not_signal(self):
        lines, output = self.run_case("destroy-held")
        self.assertIn("wire=0 internal=2", output)
        self.assertFalse(any(" wl-buffer-release-sent " in line for line in lines), lines)
        self.assertEqual(sum(" wl-buffer-destroy-request " in line for line in lines), 1)
        self.assertEqual(sum(" buffer-release " in line for line in lines), 2)

    def test_repeated_reattach_commit_release_cycles_are_correlated(self):
        lines, output = self.run_case("cycles")
        self.assertIn("wire=128 internal=129", output)
        attaches = [line for line in lines if " wl-surface-attach-request " in line]
        commits = [line for line in lines if " wl-surface-commit-request " in line]
        applied = [line for line in lines if " wlroots-applied-commit " in line]
        wire = [line for line in lines if " wl-buffer-release-sent " in line]
        self.assertEqual(len(attaches), 129, lines)  # includes final NULL attach
        self.assertEqual(len(commits), 129, lines)
        self.assertEqual(len(applied), 129, lines)  # duplicate watch is idempotent
        self.assertEqual(len(wire), 128, lines)
        self.assertTrue(all(" generation=1" in line for line in wire))
        self.assertIn(" buffer_id=0 ", attaches[-1])
        self.assertIn(" attach_pending=1 ", commits[-1])
        sequences = [int(re.search(r" commit=(\d+)", line)[1]) for line in commits]
        self.assertEqual(sequences, list(range(1, 130)))
        resource_ids = [re.search(r" buffer_id=(\d+)", line)[1] for line in wire]
        self.assertEqual(len(set(resource_ids)), 1)

    def test_unset_diagnostic_env_preserves_protocol_and_omits_telemetry(self):
        lines, output = self.run_case("single", False)
        self.assertIn("wire=1 internal=2", output)
        self.assertFalse(any(" wl-buffer-release-sent " in line for line in lines), lines)
        self.assertFalse(any(" wl-surface-" in line for line in lines), lines)

    def test_saved_view_identity_walk_is_passive_bounded_and_rejects_destroyed_resource(self):
        lines, output = self.run_case("identity")
        self.assertIn("saved-wrapper identity assertions passed", output)
        self.assertIn("wire=0 internal=2", output)
        self.assertFalse(any(" wl-buffer-release-sent " in line for line in lines), lines)


if __name__ == "__main__":
    unittest.main()
