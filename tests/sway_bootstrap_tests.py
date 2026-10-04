#!/usr/bin/env python3
"""Offline source-state and rerun checks for the Rack-local Sway bootstrap.

The fixture uses a tiny Git checkout with the same three-file patch shape as
the pinned Sway change. Fake build tools exercise the complete bootstrap and
its build stamp without accessing upstream Git or changing system packages.
"""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
BOOTSTRAP = ROOT / "scripts/bootstrap-sway.sh"


def executable(path: Path, body: str) -> None:
    path.write_text(f"#!{sys.executable}\n{body}", encoding="utf-8")
    path.chmod(0o755)


def patch_id(patch: str) -> str:
    result = subprocess.run(
        ["git", "patch-id", "--stable"], input=patch,
        capture_output=True, text=True, check=True,
    )
    return result.stdout.split()[0]


class SwayBootstrapTests(unittest.TestCase):
    def setUp(self) -> None:
        # Build tools execute from this directory; /tmp can be noexec.
        self.temporary = tempfile.TemporaryDirectory(
            prefix=".sc7-sway-bootstrap-test-", dir=ROOT,
        )
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.scripts = self.root / "scripts"
        self.patches = self.root / "patches"
        self.sway = self.root / "vendor/sway"
        self.wlroots_build = self.root / "vendor/wlroots/build"
        self.fake_bin = self.root / "fake-bin"
        self.deps = self.root / "deps"
        for directory in (
            self.scripts, self.patches, self.sway, self.wlroots_build,
            self.fake_bin, self.deps / "usr/include",
        ):
            directory.mkdir(parents=True)
        shutil.copy2(BOOTSTRAP, self.scripts / BOOTSTRAP.name)
        (self.deps / "usr/include/libinput.h").write_text(
            "/* fixture */\n", encoding="utf-8",
        )
        self.log = self.root / "commands.log"

        self.git("init", "-q")
        self.git("config", "user.name", "SC7 test")
        self.git("config", "user.email", "sc7-test@example.invalid")
        (self.sway / ".gitignore").write_text(
            "build/\n.sc7-pkgconfig/\n", encoding="utf-8",
        )
        self.view = self.sway / "include/sway/tree/view.h"
        self.shell = self.sway / "sway/desktop/xdg_shell.c"
        self.server = self.sway / "sway/server.c"
        for source in (self.view, self.shell, self.server):
            source.parent.mkdir(parents=True, exist_ok=True)
        self.view.write_text(
            "struct sway_xdg_popup {\n"
            "    int existing;\n"
            "};\n", encoding="utf-8",
        )
        self.shell.write_text(
            "static void popup_create(void) {\n"
            "    popup_unconstrain();\n"
            "}\n", encoding="utf-8",
        )
        self.server.write_text(
            "#define SWAY_XDG_SHELL_VERSION 2\n"
            "#define SWAY_LAYER_SHELL_VERSION 4\n", encoding="utf-8",
        )
        self.git("add", ".")
        self.git("commit", "-qm", "pinned Sway base")
        self.base_sha = self.git("rev-parse", "HEAD").stdout.strip()
        (self.patches / "SWAY_BASE_REVISION").write_text(
            self.base_sha + "\n", encoding="utf-8",
        )

        self.view.write_text(
            "struct sway_xdg_popup {\n"
            "    int existing;\n"
            "    int surface_commit_listener;\n"
            "    int reposition_listener;\n"
            "};\n", encoding="utf-8",
        )
        self.shell.write_text(
            "static void popup_create(void) {\n"
            "    add_surface_commit_listener();\n"
            "    add_reposition_listener();\n"
            "}\n", encoding="utf-8",
        )
        self.server.write_text(
            "#define SWAY_XDG_SHELL_VERSION 3\n"
            "#define SWAY_LAYER_SHELL_VERSION 4\n", encoding="utf-8",
        )
        self.popup_patch = self.git("diff", "--binary").stdout
        self.legacy_patch = self.git(
            "diff", "--binary", "--", "include/sway/tree/view.h",
            "sway/desktop/xdg_shell.c",
        ).stdout
        self.popup_id = patch_id(self.popup_patch)
        self.legacy_id = patch_id(self.legacy_patch)
        (self.patches / "sway-popup-lifecycle.patch").write_text(
            self.popup_patch, encoding="utf-8",
        )
        # Authenticate both the previous complete popup state and the new
        # production input policy. Incremental and full patches must agree.
        self.git("add", ".")
        self.popup_tree = self.git("write-tree").stdout.strip()
        self.server.write_text(self.server.read_text() +
                               "/* Rack client inputs: SHM; output unchanged */\n")
        shm_patch = self.git("diff", "--binary").stdout
        (self.patches / "sway-shm-input.patch").write_text(shm_patch)
        self.full_patch = self.git("diff", "--binary", self.base_sha).stdout
        self.full_id = patch_id(self.full_patch)
        (self.patches / "sway-sc7labs-rack.patch").write_text(self.full_patch)
        self.git("restore", "--staged", ".")
        self.git("restore", ".")

        wlroots_patch = (
            "diff --git a/backend.c b/backend.c\n"
            "--- a/backend.c\n+++ b/backend.c\n"
            "@@ -1 +1 @@\n-old\n+new\n"
        )
        (self.patches / "wlroots-sc7labs-rack.patch").write_text(
            wlroots_patch, encoding="utf-8",
        )
        self.wlroots_id = patch_id(wlroots_patch)
        self.wlroots_lib = self.wlroots_build / "libwlroots.so.12"
        self.wlroots_lib.write_text("private fixture library\n", encoding="utf-8")
        (self.wlroots_build / ".sc7-patch-id").write_text(
            self.wlroots_id + "\n", encoding="utf-8",
        )
        uninstalled = self.wlroots_build / "meson-uninstalled/wlroots-uninstalled.pc"
        private = self.wlroots_build / "meson-private/wlroots.pc"
        uninstalled.parent.mkdir()
        private.parent.mkdir()
        uninstalled.write_text("Name: wlroots\n", encoding="utf-8")
        private.write_text("have_xwayland=false\n", encoding="utf-8")

        executable(
            self.fake_bin / "meson",
            """import os
from pathlib import Path
import sys
with open(os.environ['SC7_TEST_COMMAND_LOG'], 'a', encoding='utf-8') as log:
    log.write('meson ' + ' '.join(sys.argv[1:]) + '\\n')
args = sys.argv[1:]
build = Path(args[2] if '--reconfigure' in args or '--wipe' in args else args[1])
if (build / 'meson-private/SC7_TEST_FAIL_FIRST').exists() and '--wipe' not in args:
    sys.exit(2)
if (build / 'meson-private/SC7_TEST_NO_GRAPH').exists() and '--wipe' not in args:
    sys.exit(0)
build.mkdir(parents=True, exist_ok=True)
(build / 'build.ninja').write_text('fake build graph\\n', encoding='utf-8')
""",
        )
        executable(
            self.fake_bin / "ninja",
            """import os
from pathlib import Path
import sys
with open(os.environ['SC7_TEST_COMMAND_LOG'], 'a', encoding='utf-8') as log:
    log.write('ninja ' + ' '.join(sys.argv[1:]) + '\\n')
if '-t' in sys.argv:
    sys.exit(0)
build = Path(sys.argv[sys.argv.index('-C') + 1])
binary = build / 'sway/sway'
binary.parent.mkdir(parents=True, exist_ok=True)
binary.write_text('#!' + sys.executable + '\\n'
                  "print('sway version 1.9-sc7-test')\\n", encoding='utf-8')
binary.chmod(0o755)
""",
        )
        executable(
            self.fake_bin / "ldd",
            """import os
print('libwlroots.so.12 => ' + os.environ['SC7_TEST_WLROOTS_LIB'])
""",
        )
        executable(self.fake_bin / "pkg-config", """import os
from pathlib import Path
import sys
marker = os.getenv('SC7_TEST_MISSING_DEP')
sys.exit(1 if marker and Path(marker).exists() and 'json-c' in sys.argv else 0)
""")
        executable(self.scripts / "install-packages.sh", """import os
from pathlib import Path
import sys
with open(os.environ['SC7_TEST_COMMAND_LOG'], 'a', encoding='utf-8') as log:
    log.write('install-packages ' + ' '.join(sys.argv[1:]) + '\\n')
if os.getenv('SC7_TEST_APT_FAIL'):
    sys.exit(100)
marker = os.getenv('SC7_TEST_MISSING_DEP')
if marker:
    Path(marker).unlink()
""")
        executable(self.fake_bin / "wayland-scanner", "")
        real_git = shutil.which("git")
        assert real_git is not None
        executable(
            self.fake_bin / "git",
            f"""import os
import sys
if 'clone' in sys.argv[1:] or 'fetch' in sys.argv[1:]:
    with open(os.environ['SC7_TEST_COMMAND_LOG'], 'a', encoding='utf-8') as log:
        log.write('blocked-network-git ' + ' '.join(sys.argv[1:]) + '\\n')
    sys.exit(98)
os.execv({real_git!r}, [{real_git!r}, *sys.argv[1:]])
""",
        )
        self.env = os.environ.copy()
        self.env["PATH"] = f"{self.fake_bin}{os.pathsep}{self.env['PATH']}"
        self.env["SC7_TEST_COMMAND_LOG"] = str(self.log)
        self.env["SC7_TEST_WLROOTS_LIB"] = str(self.wlroots_lib)
        self.env["SC7_SWAY_DEP_PREFIX"] = str(self.deps)

    def git(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["git", "-C", str(self.sway), *args],
            capture_output=True, text=True, check=True,
        )

    def run_bootstrap(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["bash", str(self.scripts / "bootstrap-sway.sh"), *args],
            env=self.env, capture_output=True, text=True, timeout=30,
        )

    def commands(self) -> list[str]:
        return self.log.read_text(encoding="utf-8").splitlines() if self.log.exists() else []

    def tracked_status(self) -> str:
        return self.git("status", "--porcelain", "--untracked-files=no").stdout

    def actual_patch_id(self) -> str:
        diff = self.git("diff", "--binary", self.base_sha, "HEAD").stdout
        return patch_id(diff)

    def commit_full_patch(self) -> str:
        self.git("apply", str(self.patches / "sway-sc7labs-rack.patch"))
        self.git("add", "--", "include/sway/tree/view.h",
                 "sway/desktop/xdg_shell.c", "sway/server.c")
        self.git("commit", "-qm", "complete Rack patch")
        return self.git("rev-parse", "HEAD").stdout.strip()

    def commit_legacy_patch(self) -> str:
        legacy_file = self.patches / "legacy.patch"
        legacy_file.write_text(self.legacy_patch, encoding="utf-8")
        self.git("apply", str(legacy_file))
        self.git("add", "--", "include/sway/tree/view.h",
                 "sway/desktop/xdg_shell.c")
        self.git("commit", "-qm", "previous incomplete Rack patch")
        return self.git("rev-parse", "HEAD").stdout.strip()

    def assert_verified(self) -> None:
        self.assertEqual(self.tracked_status(), "")
        self.assertEqual(self.actual_patch_id(), self.full_id)
        self.assertIn("SWAY_XDG_SHELL_VERSION 3", self.server.read_text())
        self.assertIn("Rack client inputs: SHM; output unchanged", self.server.read_text())
        self.assertEqual(
            (self.sway / "build/.sc7-patch-id").read_text().strip(),
            f"{self.full_id} {self.wlroots_id}",
        )
        check = self.run_bootstrap("--check")
        self.assertEqual(check.returncode, 0, check.stdout + check.stderr)
        self.assertEqual(self.tracked_status(), "")
        self.assertFalse(any(
            line.startswith("blocked-network-git ") for line in self.commands()
        ))

    def test_fresh_bootstrap_commits_complete_patch_and_second_install_passes(self) -> None:
        result = self.run_bootstrap()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotEqual(self.git("rev-parse", "HEAD").stdout.strip(), self.base_sha)
        self.assert_verified()
        self.assertIn("sway/server.c", self.git("show", "--format=", "--name-only", "HEAD").stdout)

        before = self.commands()
        second = self.run_bootstrap("--install-deps")
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(self.commands(), before)
        self.assert_verified()

    def test_existing_complete_patch_head_is_recognized(self) -> None:
        expected_head = self.commit_full_patch()
        result = self.run_bootstrap()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), expected_head)
        self.assert_verified()

    def commit_previous_complete_patch(self) -> str:
        self.git("apply", str(self.patches / "sway-popup-lifecycle.patch"))
        self.git("add", ".")
        self.git("commit", "-qm", "previous complete popup patch")
        return self.git("rev-parse", "HEAD").stdout.strip()

    def test_previous_complete_popup_build_migrates_and_reruns_cleanly(self) -> None:
        previous_head = self.commit_previous_complete_patch()
        build = self.sway / "build"
        build.mkdir()
        (build / ".sc7-patch-id").write_text(f"{self.popup_id} {self.wlroots_id}\n")
        check = self.run_bootstrap("--check")
        self.assertNotEqual(check.returncode, 0)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), previous_head)
        result = self.run_bootstrap()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_verified()
        self.assertIn("Adding authenticated SHM input policy", result.stdout)
        self.assertIn("sway/server.c", self.git("show", "--format=", "--name-only", "HEAD").stdout)
        before = self.commands()
        self.assertEqual(self.run_bootstrap().returncode, 0)
        self.assertEqual(self.commands(), before)

    def test_previous_popup_state_with_unrelated_server_edit_is_rejected(self) -> None:
        previous_head = self.commit_previous_complete_patch()
        with self.server.open("a") as stream:
            stream.write("/* genuine dirty source */\n")
        result = self.run_bootstrap()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("uncommitted tracked source changes", result.stderr)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), previous_head)
        self.assertEqual(self.tracked_status().strip(), "M sway/server.c")
        self.assertEqual(self.commands(), [])

    def test_mismatched_incremental_policy_patch_is_rejected(self) -> None:
        policy = self.patches / "sway-shm-input.patch"
        policy.write_text(policy.read_text().replace("output unchanged", "tampered policy"))
        result = self.run_bootstrap()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match the complete Rack Sway patch", result.stderr)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), self.base_sha)
        self.assertEqual(self.tracked_status(), "")
        self.assertEqual(self.commands(), [])

    def test_expected_policy_as_uncommitted_mutation_is_rejected(self) -> None:
        previous_head = self.commit_previous_complete_patch()
        self.git("apply", str(self.patches / "sway-shm-input.patch"))
        result = self.run_bootstrap()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("uncommitted tracked source changes", result.stderr)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), previous_head)
        self.assertEqual(self.commands(), [])

    def test_missing_sway_dependency_uses_package_helper_and_rechecks_prefix(self) -> None:
        marker = self.root / "missing-json-c"
        marker.touch()
        self.env["SC7_TEST_MISSING_DEP"] = str(marker)
        result = self.run_bootstrap("--install-deps")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("install-packages libjson-c-dev", self.commands())
        self.assertFalse(marker.exists())
        self.assert_verified()

    def test_dependency_install_failure_does_not_start_build(self) -> None:
        marker = self.root / "missing-json-c"
        marker.touch()
        self.env["SC7_TEST_MISSING_DEP"] = str(marker)
        self.env["SC7_TEST_APT_FAIL"] = "1"
        result = self.run_bootstrap("--install-deps")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Could not install Sway build dependencies", result.stderr)
        self.assertFalse(any(line.startswith("meson ") for line in self.commands()))
        self.assertEqual(self.tracked_status(), "")

    def test_interrupted_meson_configuration_recovers(self) -> None:
        for marker_name in ("SC7_TEST_FAIL_FIRST", "SC7_TEST_NO_GRAPH"):
            with self.subTest(marker=marker_name):
                build = self.sway / "build"
                if build.exists():
                    shutil.rmtree(build)
                metadata = build / "meson-private"
                metadata.mkdir(parents=True)
                (metadata / marker_name).touch()
                result = self.run_bootstrap()
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("Recovering incomplete Sway Meson configuration", result.stdout)
                self.assertTrue(any(line.startswith("meson setup --wipe ")
                                    for line in self.commands()))
                self.assert_verified()

    def test_exact_vm_partial_state_recovers_without_reclone(self) -> None:
        old_head = self.commit_legacy_patch()
        self.assertEqual(self.actual_patch_id(), self.legacy_id)
        self.server.write_text(
            "#define SWAY_XDG_SHELL_VERSION 3\n"
            "#define SWAY_LAYER_SHELL_VERSION 4\n", encoding="utf-8",
        )
        self.assertEqual(self.tracked_status().strip(), "M sway/server.c")

        precheck = self.run_bootstrap("--check")
        self.assertNotEqual(precheck.returncode, 0)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), old_head)

        result = self.run_bootstrap()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotEqual(self.git("rev-parse", "HEAD").stdout.strip(), old_head)
        self.assert_verified()

        before = self.commands()
        second = self.run_bootstrap("--install-deps")
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(self.commands(), before)

    def test_unrelated_server_change_remains_rejected(self) -> None:
        old_head = self.commit_legacy_patch()
        self.server.write_text(
            "#define SWAY_XDG_SHELL_VERSION 4\n"
            "#define SWAY_LAYER_SHELL_VERSION 4\n", encoding="utf-8",
        )
        result = self.run_bootstrap()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("uncommitted tracked source changes", result.stderr)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), old_head)
        self.assertEqual(self.tracked_status().strip(), "M sway/server.c")
        self.assertNotIn("ninja ", "\n".join(self.commands()))

    def test_expected_server_change_plus_extra_edit_remains_rejected(self) -> None:
        old_head = self.commit_legacy_patch()
        self.server.write_text(
            "#define SWAY_XDG_SHELL_VERSION 3\n"
            "#define SWAY_LAYER_SHELL_VERSION 4\n"
            "/* unrelated user edit */\n", encoding="utf-8",
        )
        result = self.run_bootstrap()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("uncommitted tracked source changes", result.stderr)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), old_head)
        self.assertEqual(self.tracked_status().strip(), "M sway/server.c")

    def test_whitespace_modified_legacy_commit_cannot_be_recovered(self) -> None:
        legacy_file = self.patches / "legacy.patch"
        legacy_file.write_text(self.legacy_patch, encoding="utf-8")
        self.git("apply", str(legacy_file))
        self.view.write_text(
            self.view.read_text(encoding="utf-8").replace(
                "    int surface_commit_listener;",
                "\tint surface_commit_listener;",
            ),
            encoding="utf-8",
        )
        self.git("add", "--", "include/sway/tree/view.h",
                 "sway/desktop/xdg_shell.c")
        self.git("commit", "-qm", "lookalike partial patch")
        lookalike_head = self.git("rev-parse", "HEAD").stdout.strip()
        # A stable patch ID deliberately ignores this whitespace difference.
        # The exact expected Git tree must still reject this source state.
        self.assertEqual(self.actual_patch_id(), self.legacy_id)
        self.server.write_text(
            "#define SWAY_XDG_SHELL_VERSION 3\n"
            "#define SWAY_LAYER_SHELL_VERSION 4\n", encoding="utf-8",
        )

        result = self.run_bootstrap()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("uncommitted tracked source changes", result.stderr)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), lookalike_head)
        self.assertEqual(self.tracked_status().strip(), "M sway/server.c")
        self.assertFalse((self.sway / "build/.sc7-patch-id").exists())

    def test_whitespace_modified_completed_commit_is_not_authenticated(self) -> None:
        self.git("apply", str(self.patches / "sway-sc7labs-rack.patch"))
        self.view.write_text(
            self.view.read_text(encoding="utf-8").replace(
                "    int surface_commit_listener;",
                "\tint surface_commit_listener;",
            ),
            encoding="utf-8",
        )
        self.git("add", "--", "include/sway/tree/view.h",
                 "sway/desktop/xdg_shell.c", "sway/server.c")
        self.git("commit", "-qm", "lookalike complete patch")
        lookalike_head = self.git("rev-parse", "HEAD").stdout.strip()
        self.assertEqual(self.actual_patch_id(), self.full_id)
        self.assertEqual(self.tracked_status(), "")

        result = self.run_bootstrap()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source does not match the tracked Rack patch", result.stderr)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), lookalike_head)
        self.assertFalse((self.sway / "build/.sc7-patch-id").exists())


if __name__ == "__main__":
    unittest.main()
