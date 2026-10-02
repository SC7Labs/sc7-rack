#!/usr/bin/env python3
"""Offline regression tests for interrupted patched-wlroots bootstraps."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
BOOTSTRAP = ROOT / "scripts/bootstrap-wlroots.sh"


def executable(path: Path, body: str) -> None:
    path.write_text(f"#!{sys.executable}\n{body}", encoding="utf-8")
    path.chmod(0o755)


class BootstrapRerunTests(unittest.TestCase):
    def setUp(self) -> None:
        # Build tools are executed from this fixture. Use the project filesystem
        # because some systems mount /tmp with noexec.
        self.temporary = tempfile.TemporaryDirectory(
            prefix=".sc7-bootstrap-test-", dir=ROOT
        )
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.scripts = self.root / "scripts"
        self.patches = self.root / "patches"
        self.wlroots = self.root / "vendor/wlroots"
        self.fake_bin = self.root / "fake-bin"
        for directory in (self.scripts, self.patches, self.wlroots, self.fake_bin):
            directory.mkdir(parents=True)
        shutil.copy2(BOOTSTRAP, self.scripts / BOOTSTRAP.name)
        self.log = self.root / "commands.log"

        self.git("init", "-q")
        self.git("config", "user.name", "SC7 test")
        self.git("config", "user.email", "sc7-test@example.invalid")
        (self.wlroots / ".gitignore").write_text("build/\n", encoding="utf-8")
        (self.wlroots / "backend.txt").write_text("upstream\n", encoding="utf-8")
        self.git("add", ".")
        self.git("commit", "-qm", "upstream base")
        self.base_sha = self.git("rev-parse", "HEAD").stdout.strip()
        self.git("checkout", "-qb", "working-dnd-transport")

        # Model three authenticated source generations. The two incremental
        # patches lead from legacy -> previous -> current.
        source = self.wlroots / "backend.txt"
        source.write_text("SC7 legacy transport\n", encoding="utf-8")
        legacy_patch = self.git("diff", "--", "backend.txt").stdout
        (self.patches / "wlroots-legacy.patch").write_text(
            legacy_patch, encoding="utf-8"
        )
        legacy_id = subprocess.run(
            ["git", "patch-id", "--stable"], input=legacy_patch,
            capture_output=True, text=True, check=True,
        ).stdout.split()[0]
        (self.patches / "WLROOTS_LEGACY_PATCH_ID").write_text(
            legacy_id + "\n", encoding="utf-8"
        )
        source.write_text("SC7 patched transport\n", encoding="utf-8")
        previous_patch = self.git("diff", "--", "backend.txt").stdout
        (self.patches / "wlroots-previous.patch").write_text(
            previous_patch, encoding="utf-8"
        )
        previous_id = subprocess.run(
            ["git", "patch-id", "--stable"], input=previous_patch,
            capture_output=True, text=True, check=True,
        ).stdout.split()[0]
        (self.patches / "WLROOTS_PREVIOUS_PATCH_ID").write_text(
            previous_id + "\n", encoding="utf-8"
        )
        self.current_text = "SC7 patched transport\nSC7 popup pointer release\n"
        source.write_text(self.current_text, encoding="utf-8")
        patch = self.git("diff", "--", "backend.txt").stdout
        (self.patches / "wlroots-sc7labs-rack.patch").write_text(
            patch, encoding="utf-8"
        )
        (self.patches / "wlroots-dnd-lifetime-fix.patch").write_text(
            "diff --git a/backend.txt b/backend.txt\n"
            "--- a/backend.txt\n"
            "+++ b/backend.txt\n"
            "@@ -1 +1 @@\n"
            "-SC7 legacy transport\n"
            "+SC7 patched transport\n",
            encoding="utf-8",
        )
        (self.patches / "wlroots-pointer-release-fix.patch").write_text(
            "diff --git a/backend.txt b/backend.txt\n"
            "--- a/backend.txt\n"
            "+++ b/backend.txt\n"
            "@@ -1 +1,2 @@\n"
            " SC7 patched transport\n"
            "+SC7 popup pointer release\n",
            encoding="utf-8",
        )
        self.git("checkout", "--", "backend.txt")
        (self.patches / "WLROOTS_BASE_REVISION").write_text(
            self.base_sha + "\n", encoding="utf-8"
        )

        # The dependency helper and build tools are fakes; tests never access
        # apt, upstream Git, the real vendor tree, or a real compiler.
        executable(
            self.scripts / "wlroots_build_deps.py",
            """import os
import sys
with open(os.environ['SC7_TEST_COMMAND_LOG'], 'a', encoding='utf-8') as log:
    log.write('build-deps ' + ' '.join(sys.argv[1:]) + '\\n')
""",
        )
        executable(
            self.fake_bin / "meson",
            """import os
from pathlib import Path
import sys
with open(os.environ['SC7_TEST_COMMAND_LOG'], 'a', encoding='utf-8') as log:
    log.write('meson ' + ' '.join(sys.argv[1:]) + '\\n')
build = Path(sys.argv[-1] if '--reconfigure' in sys.argv else sys.argv[-2])
if (build / 'meson-private/SC7_TEST_FAIL_FIRST').exists() and '--wipe' not in sys.argv:
    sys.exit(2)
if (build / 'meson-private/SC7_TEST_NO_GRAPH').exists() and '--wipe' not in sys.argv:
    sys.exit(0)
build.mkdir(parents=True, exist_ok=True)
(build / 'build.ninja').write_text('fake build\\n', encoding='utf-8')
""",
        )
        executable(
            self.fake_bin / "ninja",
            """import os
from pathlib import Path
import sys
with open(os.environ['SC7_TEST_COMMAND_LOG'], 'a', encoding='utf-8') as log:
    log.write('ninja ' + ' '.join(sys.argv[1:]) + '\\n')
build = Path(sys.argv[sys.argv.index('-C') + 1])
if os.getenv('SC7_TEST_NINJA_FAIL'):
    sys.exit(2)
(build / 'libwlroots.so.12').write_text(
    'valid-symbol full-renderer\\n' if not os.getenv('SC7_TEST_RENDERERLESS_BUILD')
    else 'valid-symbol\\n', encoding='utf-8')
""",
        )
        executable(
            self.fake_bin / "nm",
            """from pathlib import Path
import sys
library = Path(sys.argv[-1]).read_text(encoding='utf-8')
if 'valid-symbol' in library:
    for name in ('wlr_wl_backend_find_by_display', 'wlr_wl_backend_create',
                 'wlr_pixman_renderer_create'):
        print('0000000000000000 T ' + name)
if 'full-renderer' in library:
    for name in ('wlr_egl_create_with_context', 'wlr_gles2_renderer_create',
                 'wlr_gbm_allocator_create'):
        print('0000000000000000 T ' + name)
""",
        )
        executable(self.fake_bin / "pkg-config", "import sys\nsys.exit(0)\n")
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

    def git(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["git", "-C", str(self.wlroots), *args],
            check=True,
            capture_output=True,
            text=True,
        )

    def commit_patch(self) -> str:
        self.git("apply", str(self.patches / "wlroots-sc7labs-rack.patch"))
        self.git("add", "backend.txt")
        self.git("commit", "-qm", "build(wlroots): apply SC7Labs Rack patches")
        return self.git("rev-parse", "HEAD").stdout.strip()

    def commit_legacy_patch(self) -> str:
        self.git("apply", str(self.patches / "wlroots-legacy.patch"))
        self.git("add", "backend.txt")
        self.git("commit", "-qm", "build(wlroots): apply previous SC7Labs Rack patch")
        return self.git("rev-parse", "HEAD").stdout.strip()

    def commit_previous_patch(self) -> str:
        self.git("apply", str(self.patches / "wlroots-previous.patch"))
        self.git("add", "backend.txt")
        self.git("commit", "-qm", "build(wlroots): apply prior SC7Labs Rack patch")
        return self.git("rev-parse", "HEAD").stdout.strip()

    def run_bootstrap(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["bash", str(self.scripts / "bootstrap-wlroots.sh"), *args],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=30,
        )

    def commands(self) -> list[str]:
        if not self.log.exists():
            return []
        return self.log.read_text(encoding="utf-8").splitlines()

    def remove_git_identity(self) -> None:
        self.git("config", "--unset", "user.name")
        self.git("config", "--unset", "user.email")
        self.git("config", "commit.gpgSign", "true")
        self.env["GIT_CONFIG_GLOBAL"] = os.devnull
        self.env["GIT_CONFIG_NOSYSTEM"] = "1"
        for name in ("GIT_AUTHOR_NAME", "GIT_AUTHOR_EMAIL", "GIT_COMMITTER_NAME",
                     "GIT_COMMITTER_EMAIL", "EMAIL"):
            self.env.pop(name, None)

    def test_fresh_user_needs_no_git_identity_or_signing_setup(self) -> None:
        self.remove_git_identity()
        untracked = self.wlroots / "keep-local-notes.txt"
        untracked.write_text("preserve this file\n", encoding="utf-8")
        result = self.run_bootstrap("--install-deps")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.git("log", "-1", "--format=%an <%ae>").stdout.strip(),
                         "SC7 Rack build <build@sc7.invalid>")
        self.assertEqual(self.git("status", "--porcelain", "--untracked-files=no").stdout, "")
        self.assertEqual((self.wlroots / "backend.txt").read_text(), self.current_text)
        self.assertEqual(untracked.read_text(), "preserve this file\n")
        self.assertNotIn("keep-local-notes.txt", self.git("ls-files").stdout)
        before = self.commands()
        second = self.run_bootstrap("--install-deps")
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(self.commands(), before)

    def test_exact_staged_fresh_patch_recovers_after_old_identity_failure(self) -> None:
        self.remove_git_identity()
        self.git("apply", "--index", str(self.patches / "wlroots-sc7labs-rack.patch"))
        result = self.run_bootstrap("--install-deps")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Completing the authenticated patch", result.stdout)
        self.assertEqual(self.git("status", "--porcelain").stdout, "")

    def test_staged_patch_with_unrelated_change_is_rejected(self) -> None:
        self.git("apply", "--index", str(self.patches / "wlroots-sc7labs-rack.patch"))
        (self.wlroots / "backend.txt").write_text(self.current_text + "user edit\n")
        self.git("add", "backend.txt")
        result = self.run_bootstrap("--install-deps")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("uncommitted tracked source changes", result.stderr)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), self.base_sha)

    def test_legacy_upgrade_needs_no_git_identity(self) -> None:
        self.commit_legacy_patch()
        self.remove_git_identity()
        result = self.run_bootstrap("--install-deps")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.git("status", "--porcelain").stdout, "")

    def test_committed_patch_without_build_resumes_without_reapplying(self) -> None:
        patched_sha = self.commit_patch()
        result = self.run_bootstrap()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), patched_sha)
        self.assertEqual(
            self.git("symbolic-ref", "--short", "HEAD").stdout.strip(),
            "working-dnd-transport",
        )
        self.assertNotIn("cloning from upstream", result.stdout)
        self.assertNotIn("Applying SC7Labs patch", result.stdout)
        self.assertTrue((self.wlroots / "build/libwlroots.so.12").is_file())
        self.assertTrue(any(command.startswith("meson ") for command in self.commands()))
        self.assertTrue(any(
            "-Drenderers=gles2" in command and "-Dallocators=gbm" in command
            for command in self.commands() if command.startswith("meson ")
        ))
        self.assertTrue(any(command.startswith("ninja ") for command in self.commands()))
        self.assertLess(
            next(i for i, command in enumerate(self.commands()) if command == "build-deps --check"),
            next(i for i, command in enumerate(self.commands()) if command.startswith("meson ")),
        )
        self.assertFalse(any(command.startswith("blocked-network-git ") for command in self.commands()))
        self.assertEqual(self.git("status", "--porcelain").stdout, "")

        # The next run uses a verified cache without rebuilding.
        before = self.commands()
        second = self.run_bootstrap()
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(self.commands(), before)

    def test_legacy_patched_checkout_upgrades_through_both_fixes(self) -> None:
        legacy_sha = self.commit_legacy_patch()
        build = self.wlroots / "build"
        build.mkdir()
        (build / "build.ninja").write_text("old graph\n", encoding="utf-8")
        (build / "libwlroots.so.12").write_text(
            "valid-symbol full-renderer\n", encoding="utf-8"
        )

        result = self.run_bootstrap("--install-deps")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Upgrading the authentic legacy SC7Labs DnD patch", result.stdout)
        self.assertIn("Legacy DnD patch upgraded without recloning", result.stdout)
        self.assertEqual(
            (self.wlroots / "backend.txt").read_text(encoding="utf-8"),
            self.current_text,
        )
        self.assertNotEqual(self.git("rev-parse", "HEAD").stdout.strip(), legacy_sha)
        self.assertEqual(self.git("status", "--porcelain").stdout, "")
        self.assertTrue(any(command.startswith("meson setup --wipe")
                            for command in self.commands()))
        self.assertTrue(any(command.startswith("ninja ") for command in self.commands()))
        self.assertFalse(any(command.startswith("blocked-network-git ")
                             for command in self.commands()))

        before = self.commands()
        second = self.run_bootstrap("--install-deps")
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(self.commands(), before)

    def test_previous_patched_checkout_upgrades_in_place(self) -> None:
        previous_sha = self.commit_previous_patch()
        build = self.wlroots / "build"
        build.mkdir()
        (build / "build.ninja").write_text("old graph\n", encoding="utf-8")
        (build / "libwlroots.so.12").write_text(
            "valid-symbol full-renderer\n", encoding="utf-8"
        )
        result = self.run_bootstrap("--install-deps")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Upgrading the authentic previous SC7Labs patch", result.stdout)
        self.assertEqual((self.wlroots / "backend.txt").read_text(), self.current_text)
        self.assertNotEqual(self.git("rev-parse", "HEAD").stdout.strip(), previous_sha)
        self.assertEqual(self.git("status", "--porcelain").stdout, "")
        self.assertTrue(any(command.startswith("ninja ") for command in self.commands()))
        self.assertFalse(any(command.startswith("blocked-network-git ")
                             for command in self.commands()))
        before = self.commands()
        second = self.run_bootstrap("--install-deps")
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(self.commands(), before)

    def test_unrelated_tracked_change_to_previous_patch_is_rejected(self) -> None:
        previous_sha = self.commit_previous_patch()
        (self.wlroots / "backend.txt").write_text(
            "SC7 patched transport\nuser modification\n", encoding="utf-8"
        )
        result = self.run_bootstrap("--install-deps")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("uncommitted tracked source changes", result.stderr)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), previous_sha)
        self.assertEqual((self.wlroots / "backend.txt").read_text(),
                         "SC7 patched transport\nuser modification\n")

    def test_invalid_previous_upgrade_rolls_back_before_commit(self) -> None:
        previous_sha = self.commit_previous_patch()
        migration = self.patches / "wlroots-pointer-release-fix.patch"
        migration.write_text(
            migration.read_text(encoding="utf-8").replace(
                "SC7 popup pointer release", "SC7 unrelated release"
            ),
            encoding="utf-8",
        )
        result = self.run_bootstrap()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not produce the current SC7Labs patch", result.stderr)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), previous_sha)
        self.assertEqual(self.git("status", "--porcelain").stdout, "")
        self.assertEqual((self.wlroots / "backend.txt").read_text(),
                         "SC7 patched transport\n")

    def test_interrupted_legacy_upgrade_never_accepts_old_library(self) -> None:
        self.commit_legacy_patch()
        build = self.wlroots / "build"
        build.mkdir()
        (build / "build.ninja").write_text("old graph\n", encoding="utf-8")
        library = build / "libwlroots.so.12"
        library.write_text("valid-symbol full-renderer\n", encoding="utf-8")
        self.env["SC7_TEST_NINJA_FAIL"] = "1"

        first = self.run_bootstrap()
        self.assertNotEqual(first.returncode, 0)
        self.assertEqual(
            (self.wlroots / "backend.txt").read_text(encoding="utf-8"),
            self.current_text,
        )
        self.assertEqual(library.read_text(encoding="utf-8"),
                         "valid-symbol full-renderer\n")
        self.assertFalse((build / ".sc7-patch-id").exists())

        self.env.pop("SC7_TEST_NINJA_FAIL")
        before = len([cmd for cmd in self.commands() if cmd.startswith("ninja ")])
        second = self.run_bootstrap()
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertNotIn("skipping rebuild", second.stdout.lower())
        self.assertEqual(
            len([cmd for cmd in self.commands() if cmd.startswith("ninja ")]),
            before + 1,
        )
        self.assertEqual(
            (build / ".sc7-patch-id").read_text(encoding="utf-8").strip(),
            subprocess.run(
                ["git", "patch-id", "--stable"],
                input=(self.patches / "wlroots-sc7labs-rack.patch").read_text(),
                capture_output=True, text=True, check=True,
            ).stdout.split()[0],
        )

    def test_invalid_legacy_upgrade_rolls_back_before_commit(self) -> None:
        legacy_sha = self.commit_legacy_patch()
        migration = self.patches / "wlroots-dnd-lifetime-fix.patch"
        migration.write_text(
            migration.read_text(encoding="utf-8").replace(
                "SC7 patched transport", "SC7 wrong transport"
            ),
            encoding="utf-8",
        )

        result = self.run_bootstrap()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not produce the authenticated previous source", result.stderr)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), legacy_sha)
        self.assertEqual(self.git("status", "--porcelain").stdout, "")
        self.assertEqual(
            (self.wlroots / "backend.txt").read_text(encoding="utf-8"),
            "SC7 legacy transport\n",
        )

    def test_unrelated_descendant_is_not_accepted_as_patch(self) -> None:
        (self.wlroots / "backend.txt").write_text("unrelated change\n", encoding="utf-8")
        self.git("add", "backend.txt")
        self.git("commit", "-qm", "unrelated descendant")
        unrelated_sha = self.git("rev-parse", "HEAD").stdout.strip()
        build = self.wlroots / "build"
        build.mkdir()
        (build / "libwlroots.so.12").write_text("valid-symbol\n", encoding="utf-8")

        result = self.run_bootstrap()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), unrelated_sha)
        self.assertFalse(any(command.startswith("ninja ") for command in self.commands()))
        self.assertFalse(any(command.startswith("blocked-network-git ") for command in self.commands()))
        self.assertNotIn("skipping rebuild", result.stdout.lower())

    def test_invalid_cached_library_cannot_silently_pass(self) -> None:
        self.commit_patch()
        build = self.wlroots / "build"
        build.mkdir()
        library = build / "libwlroots.so.12"
        library.write_text("invalid library\n", encoding="utf-8")

        result = self.run_bootstrap()
        if result.returncode == 0:
            self.assertTrue(
                any(command.startswith("ninja ") for command in self.commands()),
                result.stdout + result.stderr,
            )
            self.assertEqual(library.read_text(encoding="utf-8"),
                             "valid-symbol full-renderer\n")
        else:
            self.assertNotIn("skipping rebuild", result.stdout.lower())
            self.assertRegex(result.stdout + result.stderr, r"(?i)library|symbol|cache")

    def test_installer_mode_ensures_dependencies_before_build(self) -> None:
        self.commit_patch()
        result = self.run_bootstrap("--install-deps")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        commands = self.commands()
        self.assertIn("build-deps --ensure", commands)
        self.assertLess(
            commands.index("build-deps --ensure"),
            next(i for i, command in enumerate(commands) if command.startswith("meson ")),
        )

    def test_rendererless_cache_reconfigures_after_installing_dependencies(self) -> None:
        patched_sha = self.commit_patch()
        build = self.wlroots / "build"
        build.mkdir()
        (build / "build.ninja").write_text("old graph\n", encoding="utf-8")
        library = build / "libwlroots.so.12"
        library.write_text("valid-symbol\n", encoding="utf-8")
        current_patch_id = subprocess.run(
            ["git", "patch-id", "--stable"],
            input=(self.patches / "wlroots-sc7labs-rack.patch").read_text(),
            capture_output=True, text=True, check=True,
        ).stdout.split()[0]
        (build / ".sc7-patch-id").write_text(current_patch_id + "\n", encoding="utf-8")

        result = self.run_bootstrap("--install-deps")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("wlr_egl_create_with_context", result.stdout)
        self.assertIn("reconfiguring and rebuilding", result.stdout)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), patched_sha)
        self.assertEqual(self.git("status", "--porcelain").stdout, "")
        self.assertEqual(library.read_text(encoding="utf-8"),
                         "valid-symbol full-renderer\n")
        commands = self.commands()
        self.assertIn("build-deps --ensure", commands)
        refresh = next(command for command in commands
                       if command.startswith("meson setup --wipe"))
        self.assertIn("-Drenderers=gles2", refresh)
        self.assertIn("-Dallocators=gbm", refresh)
        self.assertLess(commands.index("build-deps --ensure"), commands.index(refresh))
        self.assertFalse(any(command.startswith("blocked-network-git ")
                             for command in commands))

        # A successful rebuild is accepted as a cache on the following run.
        second = self.run_bootstrap("--install-deps")
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(self.commands(), commands)

    def test_old_build_graph_without_library_redetects_renderer_inputs(self) -> None:
        self.commit_patch()
        build = self.wlroots / "build"
        build.mkdir()
        (build / "build.ninja").write_text("old rendererless graph\n", encoding="utf-8")
        result = self.run_bootstrap("--install-deps")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        commands = self.commands()
        refresh = next(command for command in commands
                       if command.startswith("meson setup --wipe"))
        self.assertIn("-Drenderers=gles2", refresh)
        self.assertIn("-Dallocators=gbm", refresh)
        self.assertLess(commands.index("build-deps --ensure"), commands.index(refresh))
        self.assertTrue((build / "libwlroots.so.12").is_file())
        self.assertFalse(any(command.startswith("blocked-network-git ")
                             for command in commands))

    def test_force_rebuild_refreshes_renderer_dependency_cache(self) -> None:
        self.commit_patch()
        first = self.run_bootstrap()
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        before = self.commands()
        forced = self.run_bootstrap("--force-rebuild")
        self.assertEqual(forced.returncode, 0, forced.stdout + forced.stderr)
        added = self.commands()[len(before):]
        self.assertTrue(any(command.startswith("meson setup --wipe")
                            for command in added))
        self.assertTrue(any(command.startswith("ninja ") for command in added))

    def test_missing_renderer_after_build_is_a_hard_failure(self) -> None:
        self.commit_patch()
        self.env["SC7_TEST_RENDERERLESS_BUILD"] = "1"
        result = self.run_bootstrap()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("wlr_egl_create_with_context", result.stdout)
        self.assertIn("missing required Wayland, Pixman, EGL, GLES2, GBM", result.stderr)

    def test_interrupted_meson_configuration_is_recovered(self) -> None:
        self.commit_patch()
        metadata = self.wlroots / "build/meson-private"
        metadata.mkdir(parents=True)
        (metadata / "SC7_TEST_FAIL_FIRST").touch()
        result = self.run_bootstrap()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Recovering incomplete Meson configuration", result.stdout)
        self.assertTrue(any("--wipe" in command for command in self.commands()))
        self.assertTrue((self.wlroots / "build/libwlroots.so.12").is_file())

    def test_meson_success_without_build_graph_is_recovered(self) -> None:
        self.commit_patch()
        metadata = self.wlroots / "build/meson-private"
        metadata.mkdir(parents=True)
        (metadata / "SC7_TEST_NO_GRAPH").touch()
        result = self.run_bootstrap()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Recovering incomplete Meson configuration", result.stdout)
        self.assertTrue(any("--wipe" in command for command in self.commands()))
        self.assertTrue((self.wlroots / "build/libwlroots.so.12").is_file())


if __name__ == "__main__":
    unittest.main()
