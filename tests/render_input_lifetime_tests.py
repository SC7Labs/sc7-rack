"""Fresh import experiment must retain the producer buffer, not just its FDs.

Tests use real wlroots buffer lifecycle/signals and its public temporary
DMA-BUF factory, with a fake renderer implementing the GLES2 addon cache.
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FreshInputLifetimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # The development host mounts its system temp directory noexec.
        cls.work = tempfile.TemporaryDirectory(prefix=".sc7-input-lifetime-", dir=ROOT)
        work = Path(cls.work.name)
        cls.preload = work / "fresh-input.so"
        cls.client = work / "lifetime-client"
        cls.library = ROOT / "vendor/wlroots/build"
        if not (cls.library / "libwlroots.so").is_file():
            raise RuntimeError("build the pinned local wlroots before running lifetime tests")
        cflags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "pixman-1", "wayland-server", "libdrm"], text=True))
        libs = shlex.split(subprocess.check_output(
            ["pkg-config", "--libs", "pixman-1", "wayland-server"], text=True))
        common = ["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                  "-DWLR_USE_UNSTABLE", "-I", str(ROOT / "vendor/wlroots/include"),
                  "-I", str(ROOT / "vendor/wlroots/build/include"), *cflags]
        subprocess.run([*common, "-fPIC", "-shared",
                        str(ROOT / "tests/render_input_experiment.c"),
                        "-o", str(cls.preload), "-ldl"], check=True)
        subprocess.run([*common, "-Wl,--export-dynamic",
                        str(ROOT / "tests/render_input_lifetime_client.c"),
                        "-o", str(cls.client), "-L", str(cls.library),
                        "-lwlroots", *libs], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def run_case(self, case: str, mode: str | None = "fresh-input", log: Path | None = None):
        # LD_PRELOAD tokenizes whitespace even inside absolute path strings.
        environment = dict(os.environ, LD_PRELOAD="./" + self.preload.name,
                           LD_LIBRARY_PATH=str(self.library))
        environment.pop("SC7_RENDER_EXPERIMENT", None)
        environment.pop("SC7_INPUT_TEST_LOG", None)
        if mode is not None:
            environment["SC7_RENDER_EXPERIMENT"] = mode
        if log is not None:
            environment["SC7_INPUT_TEST_LOG"] = str(log)
        result = subprocess.run([str(self.client), case], env=environment,
                                cwd=self.preload.parent,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("texture lifetime assertions passed", result.stdout)
        return result.stdout

    def test_dma_source_remains_owned_until_fresh_texture_destroy(self):
        self.run_case("success")

    def test_failed_import_returns_source_lock(self):
        self.run_case("failure")

    def test_same_source_receives_distinct_imports_and_releases_once(self):
        self.run_case("repeated")

    def test_saved_view_retains_client_wrapper_and_source(self):
        self.run_case("saved-view")

    def test_importing_client_wrapper_tears_down_sources_reentrantly(self):
        self.run_case("wrapper-import")

    def test_repeated_allocation_cycles_release_each_source_once(self):
        self.run_case("cycles")

    def test_shm_updates_become_full_uploads_without_extra_retention(self):
        self.run_case("shm")

    def test_unset_mode_preserves_cache_and_updates(self):
        self.run_case("disabled", None)

    def test_unrecognized_mode_preserves_cache_and_updates(self):
        self.run_case("disabled", "fresh-input-typo")

    def test_observe_logs_incoming_shm_upload_separately_from_cached_source(self):
        log = self.preload.parent / "observe-input.log"
        output = self.run_case("observe-upload", None, log)
        identities = re.search(r"cached_source=(\S+) incoming=(\S+) texture=(\S+)", output)
        self.assertIsNotNone(identities)
        cached, incoming, texture = identities.groups()
        self.assertNotEqual(cached, incoming)
        lines = log.read_text().splitlines()
        imports = [line for line in lines if " texture-import " in line]
        updates = [line for line in lines if " texture-update " in line]
        self.assertEqual(len(imports), 1, lines)
        self.assertEqual(len(updates), 1, lines)
        self.assertIn(f" source={cached} ", imports[0])
        self.assertIn(f" source={incoming} ", updates[0])
        self.assertIn(f" texture={texture} ", updates[0])
        self.assertIn(" kind=shm ", updates[0])
        self.assertIn(" format=0x34325258 ", updates[0])
        self.assertIn(" ok=1", updates[0])


if __name__ == "__main__":
    unittest.main()
