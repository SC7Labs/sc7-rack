#!/usr/bin/env python3
"""Exercise the Settings interpreter and GUI dependency probe as processes.

Every profile and executable fixture belongs to a disposable directory. These
checks neither open a host window nor change a host setting or installed package.
"""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SETTINGS = ROOT / "bin/sc7-rack-settings"
SYSTEM_PYTHON = "/usr/bin/python3"
TK_IMPORT = "import tkinter; from tkinter import ttk, filedialog, messagebox"

# Blocking imports makes missing-Tk behavior reproducible even on machines that
# have already installed the dependency. The production file runs unchanged.
CONTROLLED_RUNTIME = r'''
import builtins
import runpy
import subprocess
import sys
import types

script, capability, *arguments = sys.argv[1:]
attempts = []
def forbidden_process(*args, **kwargs):
    attempts.append(args)
    raise AssertionError("Settings must not select an alternate interpreter")
subprocess.run = forbidden_process
if capability == "missing":
    original_import = builtins.__import__
    def without_tk(name, *args, **kwargs):
        if name == "tkinter" or name.startswith("tkinter."):
            raise ModuleNotFoundError("fixture: tkinter is unavailable")
        return original_import(name, *args, **kwargs)
    builtins.__import__ = without_tk
elif capability == "present":
    tk = types.ModuleType("tkinter")
    for name in ("ttk", "filedialog", "messagebox"):
        setattr(tk, name, types.ModuleType("tkinter." + name))
    sys.modules["tkinter"] = tk
else:
    raise AssertionError("Unknown capability fixture")

namespace = runpy.run_path(script, run_name="sc7_settings_runtime_fixture")
assert not attempts, "Settings tried an alternate interpreter"
globals_ = namespace["main"].__globals__
globals_["get_rack_status"] = lambda: {"running": False, "pid": None}
globals_["get_bridge_status"] = lambda: {
    "running": False, "pid": None, "host": "fixture-host", "rack": "fixture-rack",
}
sys.argv = [script, *arguments]
if arguments == ["--check-gui"]:
    def forbidden_configuration():
        raise AssertionError("A dependency probe must not load user settings")
    globals_["load_settings"] = forbidden_configuration
sys.exit(namespace["main"]())
'''


class SettingsRuntimeTests(unittest.TestCase):
    def setUp(self):
        # Executable fixtures live on the project filesystem; /tmp can be noexec.
        self.temporary = tempfile.TemporaryDirectory(prefix=".sc7-settings-runtime-", dir=ROOT)
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.home = self.work / "home"
        self.home.mkdir()
        self.config = self.work / "config"
        self.runtime = self.work / "runtime"
        self.runtime.mkdir()
        self.fake_bin = self.work / "bin"
        self.fake_bin.mkdir()
        self.marker = self.work / "caller-python-used"
        self.shadow = self.work / "shadow-python-modules"
        self.shadow.mkdir()
        self.fake_python = self.fake_bin / "python3"
        self.fake_python.write_text(
            "#!/usr/bin/python3 -I\n"
            "import os, sys\n"
            "from pathlib import Path\n"
            f"Path({str(self.marker)!r}).write_text('called')\n"
            "os.execv('/usr/bin/python3', ['/usr/bin/python3', *sys.argv[1:]])\n"
        )
        self.fake_python.chmod(0o755)
        self.env = {
            "HOME": str(self.home),
            "XDG_CONFIG_HOME": str(self.config),
            "XDG_RUNTIME_DIR": str(self.runtime),
            "PATH": str(self.fake_bin) + ":/usr/bin:/bin",
            "LC_ALL": "C.UTF-8",
        }

    def launch(self, *arguments, **environment):
        return subprocess.run(
            [str(SETTINGS), *arguments], env={**self.env, **environment},
            capture_output=True, text=True, timeout=10,
        )

    def system_tk_probe(self):
        return subprocess.run(
            [SYSTEM_PYTHON, "-I", "-c", TK_IMPORT], env=self.env,
            capture_output=True, text=True, timeout=10,
        )

    def controlled(self, capability, *arguments):
        return subprocess.run(
            [SYSTEM_PYTHON, "-I", "-c", CONTROLLED_RUNTIME,
             str(SETTINGS), capability, *arguments], env=self.env,
            capture_output=True, text=True, timeout=10,
        )

    def assert_probe_matches_system(self, result):
        expected = 0 if self.system_tk_probe().returncode == 0 else 1
        self.assertEqual(result.returncode, expected, result.stderr)
        self.assertFalse(self.marker.exists(), "Settings executed PATH's caller Python")
        self.assertFalse(self.config.exists(), "The dependency probe created user configuration")
        if expected:
            self.assertIn("/usr/bin/python3", result.stderr)
            self.assertIn("python3-tk", result.stderr)

    def test_executable_probe_uses_the_system_interpreter(self):
        self.assert_probe_matches_system(self.launch("--check-gui"))

    def test_caller_python_with_fake_tk_cannot_satisfy_settings_dependency(self):
        (self.shadow / "tkinter.py").write_text(
            "from types import SimpleNamespace\n"
            "ttk = filedialog = messagebox = SimpleNamespace()\n"
        )
        caller = subprocess.run(
            ["python3", "-c", TK_IMPORT],
            env={**self.env, "PYTHONPATH": str(self.shadow)},
            capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(caller.returncode, 0, caller.stderr)
        self.assertTrue(self.marker.exists(), "Caller Python fixture was not executed")
        self.marker.unlink()
        self.assert_probe_matches_system(
            self.launch("--check-gui", PYTHONPATH=str(self.shadow)),
        )

    def test_pythonpath_cannot_break_the_real_tk_import(self):
        (self.shadow / "tkinter.py").write_text(
            "raise AssertionError('SHADOW_TK_IMPORTED')\n",
        )
        result = self.launch("--check-gui", PYTHONPATH=str(self.shadow))
        self.assert_probe_matches_system(result)
        self.assertNotIn("SHADOW_TK_IMPORTED", result.stderr)

    def test_pythonhome_cannot_redirect_the_runtime(self):
        result = self.launch("--check-gui", PYTHONHOME=str(self.work / "nonexistent-python"))
        self.assert_probe_matches_system(result)
        self.assertNotIn("Fatal Python error", result.stderr)
        self.assertNotIn("init_fs_encoding", result.stderr)

    def test_missing_tk_probe_fails_without_loading_or_writing_configuration(self):
        result = self.controlled("missing", "--check-gui")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("/usr/bin/python3", result.stderr)
        self.assertIn("python3-tk", result.stderr)
        self.assertFalse(self.config.exists())

    def test_available_tk_probe_succeeds_without_loading_or_writing_configuration(self):
        result = self.controlled("present", "--check-gui")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertEqual(result.stderr, "")
        self.assertFalse(self.config.exists())

    def test_status_still_works_without_tk(self):
        result = self.controlled("missing", "--status")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("SC7Labs Rack Settings", result.stdout)
        self.assertIn("Stopped", result.stdout)
        self.assertIn("fixture-host", result.stdout)
        self.assertFalse((self.config / "sc7-rack/settings.json").exists())

    def test_cli_can_save_a_file_preference_without_tk(self):
        files = self.work / "Documents with spaces"
        files.mkdir()
        result = self.controlled("missing", "--files-path", str(files))
        self.assertEqual(result.returncode, 0, result.stderr)
        settings = json.loads((self.config / "sc7-rack/settings.json").read_text())
        self.assertEqual(settings["files_path"], str(files))
        self.assertTrue(settings["share_clipboard"])
        self.assertFalse(settings["autostart"])
        self.assertIn(str(files), result.stdout)

    def test_help_still_works_without_tk(self):
        result = self.controlled("missing", "--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--files-path", result.stdout)
        self.assertIn("--share-clipboard", result.stdout)


if __name__ == "__main__":
    unittest.main()
