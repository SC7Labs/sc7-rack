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

        # Generate a real Git patch against the base without altering the
        # fixture's checkout until an individual test applies it.
        source = self.wlroots / "backend.txt"
        source.write_text("SC7 patched transport\n", encoding="utf-8")
        patch = self.git("diff", "--", "backend.txt").stdout
        (self.patches / "wlroots-sc7labs-rack.patch").write_text(
            patch, encoding="utf-8"
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
build = Path(sys.argv[-2])
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
(build / 'libwlroots.so.12').write_text('valid-symbol\\n', encoding='utf-8')
""",
        )
        executable(
            self.fake_bin / "nm",
            """from pathlib import Path
import sys
if 'valid-symbol' in Path(sys.argv[-1]).read_text(encoding='utf-8'):
    print('0000000000000000 T wlr_wl_backend_find_by_display')
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
            self.assertEqual(library.read_text(encoding="utf-8"), "valid-symbol\n")
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
