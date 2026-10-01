#!/usr/bin/env python3
"""Exercise desktop launchers without relying on a refreshed session PATH."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]


class InstalledLauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="sc7-launcher-")
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.repo = self.work / 'Rack space \'quote "double $cash `tick %percent =equals \\slash'
        for directory in ("scripts", "desktop", "bin"):
            (self.repo / directory).mkdir(parents=True)
        shutil.copy2(ROOT / "scripts/install_launchers.py", self.repo / "scripts")
        for name in ("sc7-rack.desktop", "sc7-rack-settings.desktop"):
            shutil.copy2(ROOT / "desktop" / name, self.repo / "desktop")
        for name in ("sc7-rack", "sc7-rack-settings"):
            binary = self.repo / "bin" / name
            binary.write_text(
                "#!/usr/bin/python3\nimport json, os, sys\n"
                "with open(os.environ['SC7_TEST_LAUNCHES'], 'a') as out:\n"
                "    out.write(json.dumps(sys.argv) + '\\n')\n"
            )
            binary.chmod(0o755)
        self.env = os.environ.copy()
        self.env.update(
            HOME=str(self.work / "home"),
            XDG_DATA_HOME=str(self.work / "data"),
            XDG_CONFIG_HOME=str(self.work / "config"),
            XDG_RUNTIME_DIR=str(self.work / "runtime"),
            PATH="/usr/bin:/bin",
            SC7_TEST_LAUNCHES=str(self.work / "launches.jsonl"),
        )
        for name in ("HOME", "XDG_RUNTIME_DIR"):
            Path(self.env[name]).mkdir()
        self.apps = Path(self.env["XDG_DATA_HOME"]) / "applications"

    def install(self):
        subprocess.run(
            ["/usr/bin/python3", str(self.repo / "scripts/install_launchers.py")],
            env=self.env, check=True, capture_output=True, text=True,
        )

    def gio(self, script, *arguments):
        probe = subprocess.run(
            ["/usr/bin/python3", "-c", "from gi.repository import Gio"],
            capture_output=True,
        )
        if probe.returncode:
            self.skipTest("Gio Python bindings are needed for a real desktop-entry launch")
        subprocess.run(
            ["/usr/bin/python3", "-c", "from gi.repository import Gio; import sys; " + script,
             *map(str, arguments)], env=self.env, check=True, capture_output=True, text=True,
        )

    def launches(self, count):
        log = Path(self.env["SC7_TEST_LAUNCHES"])
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if log.exists():
                lines = log.read_text().splitlines()
                if len(lines) >= count:
                    return [json.loads(line) for line in lines]
            time.sleep(0.02)
        self.fail("Desktop entry did not launch its executable")

    def test_desktop_and_settings_action_launch_without_user_bin_in_path(self):
        self.install()
        self.gio("Gio.DesktopAppInfo.new_from_filename(sys.argv[1]).launch([], None)",
                 self.apps / "sc7-rack.desktop")
        self.assertEqual(self.launches(1), [[str(self.repo / "bin/sc7-rack")]])
        self.gio("Gio.DesktopAppInfo.new_from_filename(sys.argv[1]).launch_action('Settings', None)",
                 self.apps / "sc7-rack.desktop")
        self.assertEqual(self.launches(2)[1], [str(self.repo / "bin/sc7-rack-settings"), "--gui"])
        self.gio("Gio.DesktopAppInfo.new_from_filename(sys.argv[1]).launch([], None)",
                 self.apps / "sc7-rack-settings.desktop")
        self.assertEqual(self.launches(3)[2], [str(self.repo / "bin/sc7-rack-settings"), "--gui"])

    def test_second_install_is_identical_and_app_id_resolves(self):
        self.install()
        first = {path.name: path.read_bytes() for path in self.apps.iterdir()}
        self.install()
        self.assertEqual(first, {path.name: path.read_bytes() for path in self.apps.iterdir()})
        self.assertEqual((self.apps / "dev.sc7labs.rack.desktop").resolve(), self.apps / "sc7-rack.desktop")
        for name in ("sc7-rack.desktop", "sc7-rack-settings.desktop"):
            self.assertEqual((self.apps / name).stat().st_mode & 0o777, 0o644)

    def test_autostart_uses_real_binary_without_session_path_changes(self):
        settings = self.repo / "bin/sc7-rack-settings"
        shutil.copy2(ROOT / "bin/sc7-rack-settings", settings)
        subprocess.run(
            ["/usr/bin/python3", "-c",
             "import runpy, sys; runpy.run_path(sys.argv[1])['sync_autostart'](True)",
             str(settings)], env=self.env, check=True, capture_output=True, text=True,
        )
        autostart = Path(self.env["XDG_CONFIG_HOME"]) / "autostart/sc7-rack.desktop"
        self.gio("Gio.DesktopAppInfo.new_from_filename(sys.argv[1]).launch([], None)", autostart)
        self.assertEqual(self.launches(1), [[str(self.repo / "bin/sc7-rack")]])

    def test_managed_alias_launches_with_minimal_path_and_uninstalls_cleanly(self):
        bashrc = Path(self.env["HOME"]) / ".bashrc"
        original = "# Existing shell preferences\nexport SC7_KEEP_ME=yes\n"
        bashrc.write_text(original)
        self.install()
        first = bashrc.read_text()
        self.install()
        self.assertEqual(first, bashrc.read_text())
        subprocess.run(
            ["bash", "--noprofile", "--norc", "-c",
             "shopt -s expand_aliases; source \"$HOME/.bashrc\"; eval rack"],
            env=self.env, check=True, capture_output=True, text=True,
        )
        self.assertEqual(self.launches(1), [[str(self.repo / "bin/sc7-rack")]])
        subprocess.run(
            ["/usr/bin/python3", str(self.repo / "scripts/install_launchers.py"), "--remove-aliases"],
            env=self.env, check=True, capture_output=True, text=True,
        )
        self.assertEqual(bashrc.read_text(), original)

    def test_existing_user_alias_is_preserved_during_install_and_removal(self):
        bashrc = Path(self.env["HOME"]) / ".bashrc"
        original = "alias rack='my-own-command'\n"
        bashrc.write_text(original)
        self.install()
        subprocess.run(
            ["/usr/bin/python3", str(self.repo / "scripts/install_launchers.py"), "--remove-aliases"],
            env=self.env, check=True, capture_output=True, text=True,
        )
        self.assertEqual(bashrc.read_text(), original)


class RuntimeLauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="sc7-launch-runtime-")
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.env = os.environ.copy()
        self.env.update(
            HOME=str(self.work / "home"),
            XDG_CONFIG_HOME=str(self.work / "config"),
            XDG_RUNTIME_DIR=str(self.work / "runtime"),
            XDG_BIN_HOME=str(self.work / "bin"),
            WAYLAND_DISPLAY="wayland-host",
        )
        for name in ("HOME", "XDG_RUNTIME_DIR", "XDG_BIN_HOME"):
            Path(self.env[name]).mkdir()

    def test_missing_fuser_preserves_unrelated_socket_and_lock(self):
        socket_path = Path(self.env["XDG_RUNTIME_DIR"]) / "wayland-27"
        lock = socket_path.with_suffix(".lock")
        socket_path.write_text("unrelated compositor")
        lock.write_text("unrelated owner")
        result = subprocess.run(
            ["bash", "-c",
             "command() { if [[ \"$*\" == '-v fuser' ]]; then return 1; fi; builtin command \"$@\"; }; "
             "export -f command; exec bash \"$1\"", "launcher-test", str(ROOT / "bin/sc7-rack")],
            env=self.env, capture_output=True, text=True, timeout=5,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("fuser is unavailable", result.stderr)
        self.assertEqual(socket_path.read_text(), "unrelated compositor")
        self.assertEqual(lock.read_text(), "unrelated owner")

    def test_failed_compositor_reports_failure_and_cleans_pid(self):
        sway = self.work / "sway-fail"
        sway.write_text("#!/bin/sh\nexit 42\n")
        sway.chmod(0o755)
        result = subprocess.run(
            ["bash", str(ROOT / "bin/sc7-rack")],
            env=self.env | {"SC7_RACK_SWAY_BINARY": str(sway)},
            capture_output=True, text=True, timeout=5,
        )
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertFalse((Path(self.env["XDG_RUNTIME_DIR"]) / "sc7-rack/sc7-rack.pid").exists())

    def test_normal_compositor_exit_still_succeeds_and_cleans_pid(self):
        sway = self.work / "sway-normal"
        sway.write_text("#!/bin/sh\nexit 0\n")
        sway.chmod(0o755)
        result = subprocess.run(
            ["bash", str(ROOT / "bin/sc7-rack")],
            env=self.env | {"SC7_RACK_SWAY_BINARY": str(sway)},
            capture_output=True, text=True, timeout=5,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((Path(self.env["XDG_RUNTIME_DIR"]) / "sc7-rack/sc7-rack.pid").exists())


if __name__ == "__main__":
    unittest.main()
