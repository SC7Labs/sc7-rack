#!/usr/bin/env python3
"""Run the installer with an empty user profile and isolated package tools.

The real installer, package helper, dependency probes, launcher writer and
configuration assets run in a disposable clone. Package management and the
compiler/bootstrap boundaries are simulated; no host packages or user files
are changed. Dedicated bootstrap suites cover source authentication/builds.
"""

import configparser
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent


def executable(path: Path, body: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"#!{sys.executable}\n{body}", encoding="utf-8")
    path.chmod(0o755)


FAKE_COMMAND = r'''
import json
import os
from pathlib import Path
import sys

name = Path(sys.argv[0]).name
args = sys.argv[1:]
root = Path(os.environ["SC7_TEST_ROOT"])
tools = Path(os.environ["SC7_TEST_TOOLS"])
with (root / "commands.jsonl").open("a") as output:
    output.write(json.dumps([name, *args]) + "\n")

def make_command(command):
    target = tools / command
    target.write_text(Path(__file__).read_text())
    target.chmod(0o755)

if name == "sudo":
    # Model a normal account that must be allowed to enter its password.
    if "-n" in args:
        print("sudo: a password is required", file=sys.stderr)
        sys.exit(1)
    if args == ["-v"]:
        sys.exit(0)
    os.execvp(args[0], args)
elif name == "id":
    print("1000" if args == ["-u"] else "fresh-rack-user")
elif name == "python3":
    if len(args) >= 2 and args[0] == "-c" and "import tkinter" in args[1]:
        sys.exit(0 if (root / "tk-installed").exists() else 1)
    os.execv(os.environ["SC7_TEST_PYTHON"], [os.environ["SC7_TEST_PYTHON"], *args])
elif name == "apt-get":
    operation = next((arg for arg in args if arg in ("update", "install")), "")
    if os.environ.get("SC7_TEST_APT_FAIL") == operation:
        print("fixture apt failure: " + operation, file=sys.stderr)
        sys.exit(42)
    if operation == "install":
        package_commands = {
            "sway": ("sway", "swaymsg"),
            "htop": ("htop",),
            "xdg-utils": ("xdg-open",),
            "psmisc": ("fuser",),
            "procps": ("pgrep",),
            "cosmic-term": ("cosmic-term",),
            "cosmic-terminal": ("cosmic-term",),
            "cosmic-files": ("cosmic-files",),
            "cosmic-monitor": ("cosmic-monitor",),
            "python3": ("python3",),
        }
        for package in args:
            if package == os.environ.get("SC7_TEST_APT_OMIT"):
                continue
            if package == "python3-tk":
                (root / "tk-installed").touch()
            for command in package_commands.get(package, ()):
                make_command(command)
elif name == "make":
    if os.environ.get("SC7_TEST_BUILD_FAIL") == "bridge":
        sys.exit(43)
    if os.environ.get("SC7_TEST_BUILD_OMIT") == "bridge":
        sys.exit(0)
    bridge = root / "bin/sc7-clipboard-bridge"
    bridge.write_text(Path(__file__).read_text())
    bridge.chmod(0o755)
elif name in ("bootstrap-wlroots.sh", "bootstrap-sway.sh"):
    component = "wlroots" if name == "bootstrap-wlroots.sh" else "sway"
    if os.environ.get("SC7_TEST_BUILD_FAIL") == component:
        sys.exit(44)
    if component == "wlroots":
        output = root / "vendor/wlroots/build/libwlroots.so.12"
        output.parent.mkdir(parents=True, exist_ok=True)
        output.touch()
    else:
        output = root / "vendor/sway/build/sway/sway"
        if args == ["--check"]:
            sys.exit(0 if output.is_file() else 1)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(Path(__file__).read_text())
        output.chmod(0o755)
'''


class FreshInstallTests(unittest.TestCase):
    def setUp(self) -> None:
        # Keep executables on the project filesystem; /tmp may be noexec.
        self.temporary = tempfile.TemporaryDirectory(
            prefix=".sc7 fresh install ", dir=ROOT,
        )
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / "Rack clone with spaces"
        self.home = self.base / "new user home"
        self.fake_bin = self.base / "system bin"
        for directory in (self.root, self.home, self.fake_bin):
            directory.mkdir()
        for directory in ("config", "settings", "desktop", "assets/icons"):
            shutil.copytree(ROOT / directory, self.root / directory)
        for name in (
            "install.sh", "scripts/install.sh", "scripts/install-packages.sh",
            "scripts/install_launchers.py", "scripts/wlroots_build_deps.py",
        ):
            destination = self.root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / name, destination)
        (self.root / "bridge").mkdir()
        for name in ("bootstrap-wlroots.sh", "bootstrap-sway.sh"):
            executable(self.root / "scripts" / name, FAKE_COMMAND)
        for name in (
            "sc7-rack", "sc7-rack-settings", "sc7-rack-open-host", "sc7-rack-files",
        ):
            executable(self.root / "bin" / name, FAKE_COMMAND)
        # Neither the user's bin directories nor the host's application bin
        # directories are on PATH. Only the listed basic OS tools are exposed.
        for name in (
            "bash", "dirname", "mkdir", "cp", "chmod", "ln", "grep",
            "readlink", "cat", "sort", "sed", "awk", "realpath",
        ):
            source = shutil.which(name)
            self.assertIsNotNone(source, f"Required test tool: {name}")
            (self.fake_bin / name).symlink_to(source)
        for name in (
            "sudo", "id", "python3", "apt-get", "gcc", "pkg-config", "make",
            "cosmic-term", "cosmic-files",
        ):
            executable(self.fake_bin / name, FAKE_COMMAND)
        self.environment = {
            key: value for key, value in os.environ.items()
            if not key.startswith(("XDG_", "SC7_"))
            and key not in ("WAYLAND_DISPLAY", "SWAYSOCK", "LD_LIBRARY_PATH")
        }
        self.environment.update({
            "HOME": str(self.home),
            "USER": "fresh-rack-user",
            "LOGNAME": "fresh-rack-user",
            "PATH": str(self.fake_bin),
            "XDG_CURRENT_DESKTOP": "COSMIC",
            "XDG_SESSION_TYPE": "wayland",
            "SC7_TEST_ROOT": str(self.root),
            "SC7_TEST_TOOLS": str(self.fake_bin),
            "SC7_TEST_PYTHON": sys.executable,
        })

    def install(self, **overrides: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(self.root / "install.sh")], cwd=self.home,
            env={**self.environment, **overrides},
            capture_output=True, text=True, timeout=30,
        )

    def commands(self, name: str | None = None) -> list[list[str]]:
        log = self.root / "commands.jsonl"
        commands = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
        return [command for command in commands if name is None or command[0] == name]

    def assert_succeeded(self, result: subprocess.CompletedProcess) -> None:
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("installed successfully", result.stdout)

    def assert_failed_before_configuration(self, result: subprocess.CompletedProcess) -> None:
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("installed successfully", result.stdout)
        self.assertFalse((self.home / ".config/sc7-rack/settings.json").exists())

    def test_fresh_install_and_rerun_preserve_user_settings(self) -> None:
        self.assertEqual(list(self.home.iterdir()), [])
        self.assert_succeeded(self.install())
        installed = {
            argument
            for command in self.commands("apt-get") if "install" in command
            for argument in command[1:] if not argument.startswith("-")
        }
        self.assertTrue({"sway", "htop", "xdg-utils", "cosmic-monitor", "python3-tk", "psmisc", "procps"} <= installed)
        self.assertTrue(self.commands("sudo"), "Fresh ordinary user must request sudo")
        self.assertTrue(all("-n" not in command for command in self.commands("sudo")))
        self.assertTrue((self.root / "tk-installed").exists())
        self.assertTrue(self.commands("make"))
        self.assertTrue(all("-B" in command for command in self.commands("make")))
        configuration = self.home / ".config/sc7-rack"
        for name in ("config", "inner.sh"):
            self.assertEqual((configuration / name).read_bytes(), (ROOT / "config" / name).read_bytes())
        self.assertTrue(os.access(configuration / "inner.sh", os.X_OK))
        settings = configuration / "settings.json"
        self.assertEqual(settings.read_bytes(), (ROOT / "settings/settings.json").read_bytes())
        chosen_settings = '{"autostart": true, "files_path": "/home/new user/Documents"}\n'
        settings.write_text(chosen_settings)
        package_calls = self.commands("apt-get")
        self.assert_succeeded(self.install())
        self.assertEqual(settings.read_text(), chosen_settings)
        self.assertEqual(self.commands("apt-get"), package_calls, "Rerun should reuse available packages")
        for directory in (self.home / ".local/bin", self.home / "bin"):
            for name in ("sc7-rack", "sc7-rack-settings", "sc7-rack-files", "sc7-rack-open-host", "sc7-clipboard-bridge"):
                self.assertEqual((directory / name).resolve(), self.root / "bin" / name)
            self.assertEqual((directory / "rack").resolve(), self.root / "bin/sc7-rack")
        self.assertTrue((self.home / ".local/share/icons/hicolor/scalable/apps/sc7-rack.svg").exists())
        self.assertGreaterEqual(len(self.commands("bootstrap-sway.sh")), 4)

    def test_desktop_launchers_work_without_user_bin_in_path(self) -> None:
        self.assert_succeeded(self.install())
        applications = self.home / ".local/share/applications"
        launched = []
        for name in ("sc7-rack.desktop", "sc7-rack-settings.desktop"):
            launcher = applications / name
            parser = configparser.ConfigParser(interpolation=None)
            parser.read(launcher)
            for section in parser.sections():
                if "Exec" not in parser[section]:
                    continue
                command = shlex.split(parser[section]["Exec"])
                self.assertTrue(Path(command[0]).is_absolute(), command)
                self.assertEqual(Path(command[0]).parent, self.root / "bin")
                result = subprocess.run(command, env=self.environment, cwd=self.home,
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                launched.append([Path(command[0]).name, *command[1:]])
        self.assertCountEqual(launched, [
            ["sc7-rack"], ["sc7-rack-settings", "--gui"],
            ["sc7-rack-settings", "--gui"],
        ])
        self.assertEqual((applications / "dev.sc7labs.rack.desktop").resolve(), applications / "sc7-rack.desktop")
        for command in launched:
            self.assertIn(command, self.commands())

    def test_package_index_failure_stops_installation(self) -> None:
        self.assert_failed_before_configuration(self.install(SC7_TEST_APT_FAIL="update"))
        self.assertFalse(any("install" in command for command in self.commands("apt-get")))
        self.assertEqual(self.commands("bootstrap-wlroots.sh"), [])

    def test_package_install_failure_stops_installation(self) -> None:
        self.assert_failed_before_configuration(self.install(SC7_TEST_APT_FAIL="install"))
        self.assertEqual(self.commands("bootstrap-wlroots.sh"), [])

    def test_missing_command_after_apt_success_is_rejected(self) -> None:
        self.assert_failed_before_configuration(self.install(SC7_TEST_APT_OMIT="cosmic-monitor"))
        self.assertEqual(self.commands("bootstrap-wlroots.sh"), [])

    def test_missing_tk_after_apt_success_is_rejected(self) -> None:
        self.assert_failed_before_configuration(self.install(SC7_TEST_APT_OMIT="python3-tk"))
        self.assertTrue(any("python3-tk" in command for command in self.commands("apt-get")))
        self.assertEqual(self.commands("bootstrap-wlroots.sh"), [])

    def test_missing_cosmic_app_outside_cosmic_does_not_install_a_desktop(self) -> None:
        self.assert_failed_before_configuration(self.install(XDG_CURRENT_DESKTOP="GNOME"))
        self.assertEqual(self.commands("apt-get"), [])
        self.assertEqual(self.commands("bootstrap-wlroots.sh"), [])

    def test_wlroots_failure_stops_installation(self) -> None:
        self.assert_failed_before_configuration(self.install(SC7_TEST_BUILD_FAIL="wlroots"))
        self.assertEqual(self.commands("bootstrap-sway.sh"), [])

    def test_sway_failure_stops_installation(self) -> None:
        self.assert_failed_before_configuration(self.install(SC7_TEST_BUILD_FAIL="sway"))
        self.assertEqual(self.commands("make"), [])

    def test_bridge_failure_stops_installation(self) -> None:
        self.assert_failed_before_configuration(self.install(SC7_TEST_BUILD_FAIL="bridge"))
        self.assertTrue(self.commands("make"))

    def test_missing_bridge_output_after_make_success_is_rejected(self) -> None:
        self.assert_failed_before_configuration(self.install(SC7_TEST_BUILD_OMIT="bridge"))
        self.assertTrue(self.commands("make"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
