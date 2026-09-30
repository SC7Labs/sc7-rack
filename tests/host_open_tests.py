#!/usr/bin/env python3
"""Offline regression tests for Rack's host file-association bridge."""

import errno
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parent.parent
LAUNCHER = ROOT / "bin/sc7-rack"
INNER = ROOT / "config/inner.sh"
SHIM = ROOT / "bin/rack-private/xdg-open"
HELPER = ROOT / "bin/sc7-rack-open-host"
FILES_BRIDGE = ROOT / "bin/sc7-rack-files"


def write_executable(path, body):
    path.write_text(f"#!{sys.executable}\n" + body, encoding="utf-8")
    path.chmod(0o755)


def read_json_lines(path):
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


class HostOpenTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="sc7-host-open-test-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.base = Path(cls.temporary.name)
        cls.home = cls.base / "home"
        cls.host_bin = cls.base / "host-bin"
        cls.runtime = cls.base / "host-runtime"
        cls.nested_runtime = cls.base / "nested-runtime"
        cls.config = cls.base / "config"
        for directory in (cls.home, cls.host_bin, cls.runtime, cls.nested_runtime, cls.config):
            directory.mkdir()

        cls.sway_capture = cls.base / "sway.json"
        cls.inner_capture = cls.base / "inner.jsonl"
        cls.open_capture = cls.base / "open.jsonl"
        cls.files_capture = cls.base / "files.json"
        cls.mime_capture = cls.base / "mime.json"
        cls.host_opener = cls.host_bin / "xdg-open"
        write_executable(
            cls.host_opener,
            """import json
import os
import sys

keys = ("WAYLAND_DISPLAY", "DISPLAY", "XDG_RUNTIME_DIR",
        "DBUS_SESSION_BUS_ADDRESS", "SWAYSOCK", "I3SOCK", "PATH",
        "LD_LIBRARY_PATH", "WLR_BACKENDS", "WLR_LIBINPUT_NO_DEVICES",
        "WAYLAND_SOCKET", "XDG_ACTIVATION_TOKEN")
record = {"pid": os.getpid(), "argv": sys.argv[1:],
          "env": {key: os.environ[key] for key in keys if key in os.environ}}
with open(os.environ["SC7_TEST_OPEN_CAPTURE"], "a", encoding="utf-8") as output:
    output.write(json.dumps(record, ensure_ascii=False) + "\\n")
""",
        )
        write_executable(
            cls.host_bin / "sway",
            """import json
import os
import shutil

keep = {"HOME", "XDG_BIN_HOME", "XDG_CONFIG_HOME", "XDG_RUNTIME_DIR",
        "WAYLAND_DISPLAY", "DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "SWAYSOCK",
        "I3SOCK", "PATH", "LD_LIBRARY_PATH", "WLR_BACKENDS",
        "WLR_LIBINPUT_NO_DEVICES", "SC7_RACK_PRIVATE_BIN",
        "SC7_TEST_SWAY_CAPTURE", "SC7_TEST_INNER_CAPTURE",
        "SC7_TEST_OPEN_CAPTURE"}
record = {"env": {key: value for key, value in os.environ.items()
                  if key in keep or key.startswith("SC7_HOST_")},
          "resolved_open": shutil.which("xdg-open")}
with open(os.environ["SC7_TEST_SWAY_CAPTURE"], "w", encoding="utf-8") as output:
    json.dump(record, output)
""",
        )
        write_executable(
            cls.host_bin / "swaymsg",
            """import json
import os
import shutil
import sys

record = {"argv": sys.argv[1:], "path": os.environ.get("PATH", ""),
          "resolved_open": shutil.which("xdg-open")}
with open(os.environ["SC7_TEST_INNER_CAPTURE"], "a", encoding="utf-8") as output:
    output.write(json.dumps(record) + "\\n")
if sys.argv[1:3] == ["-t", "get_tree"]:
    print(json.dumps({"nodes": [{"type": "con", "app_id": str(i)}
                               for i in range(4)]}))
""",
        )
        write_executable(
            cls.host_bin / "cosmic-files",
            """import json
import os
import socket
import subprocess
import sys

fd = int(os.environ["WAYLAND_SOCKET"])
connection = socket.socket(fileno=fd)
connection.sendall(b"files-connected-to-nested-wayland")
record = {"argv": sys.argv[1:], "env": dict(os.environ), "socket_fd": fd}
with open(os.environ["SC7_TEST_FILES_CAPTURE"], "w", encoding="utf-8") as output:
    json.dump(record, output)

# Mirror wayland-client: consume WAYLAND_SOCKET before Files launches an app.
os.environ.pop("WAYLAND_SOCKET")
connection.close()
subprocess.run(["arbitrary-mime-viewer", *sys.argv[1:]], check=True)
""",
        )
        write_executable(
            cls.host_bin / "arbitrary-mime-viewer",
            """import json
import os
import sys

record = {"pid": os.getpid(), "argv": sys.argv[1:], "env": dict(os.environ)}
with open(os.environ["SC7_TEST_MIME_CAPTURE"], "w", encoding="utf-8") as output:
    json.dump(record, output)
""",
        )

        cls.host_path = f"{cls.host_bin}:/usr/bin:/bin"
        cls.host_ld_path = str(cls.base / "host-libraries")
        cls.host_bus = f"unix:path={cls.runtime}/bus"
        cls.host_sway_socket = str(cls.runtime / "host-sway.sock")
        cls.host_i3_socket = str(cls.runtime / "host-i3.sock")
        cls.host_env = os.environ.copy()
        for key in tuple(cls.host_env):
            if key.startswith("SC7_HOST_") or key.startswith("SC7_RACK_"):
                del cls.host_env[key]
        cls.host_env.update(
            HOME=str(cls.home),
            XDG_BIN_HOME=str(cls.host_bin),
            XDG_CONFIG_HOME=str(cls.config),
            XDG_RUNTIME_DIR=str(cls.runtime),
            WAYLAND_DISPLAY="wayland-sc7-host-test",
            DISPLAY=":77",
            DBUS_SESSION_BUS_ADDRESS=cls.host_bus,
            SWAYSOCK=cls.host_sway_socket,
            I3SOCK=cls.host_i3_socket,
            PATH=cls.host_path,
            LD_LIBRARY_PATH=cls.host_ld_path,
            WLR_BACKENDS="host-backend",
            WLR_LIBINPUT_NO_DEVICES="host-libinput",
            SC7_TEST_SWAY_CAPTURE=str(cls.sway_capture),
            SC7_TEST_INNER_CAPTURE=str(cls.inner_capture),
            SC7_TEST_OPEN_CAPTURE=str(cls.open_capture),
            SC7_TEST_FILES_CAPTURE=str(cls.files_capture),
            SC7_TEST_MIME_CAPTURE=str(cls.mime_capture),
            SC7_RACK_SWAY_BINARY=str(cls.host_bin / "sway"),
        )

        # Simulate an existing install whose copied inner.sh predates the shim.
        cls.installed_inner = cls.config / "sc7-rack/inner.sh"
        cls.installed_inner.parent.mkdir()
        cls.installed_inner.write_text("#!/usr/bin/env bash\nexit 1\n", encoding="utf-8")
        cls.installed_inner.chmod(0o755)

        launched = subprocess.run(
            [str(LAUNCHER)], env=cls.host_env, cwd=cls.home,
            capture_output=True, text=True, timeout=15, check=False,
        )
        if launched.returncode != 0 or not cls.sway_capture.exists():
            raise AssertionError(
                f"Fake-Sway launcher failed ({launched.returncode}): "
                f"{launched.stdout}\n{launched.stderr}"
            )
        cls.sway_record = json.loads(cls.sway_capture.read_text(encoding="utf-8"))
        cls.sway_env = cls.sway_record["env"]

        # A populated tree makes inner.sh stop before spawning any Rack views.
        settings = cls.config / "sc7-rack/settings.json"
        settings.write_text(json.dumps({"share_clipboard": False}), encoding="utf-8")
        inner_env = cls.sway_env.copy()
        inner_env["WAYLAND_DISPLAY"] = "wayland-sc7-nested-test"
        inner = subprocess.run(
            [str(cls.config / "sc7-rack/inner.sh")], env=inner_env, cwd=cls.home,
            capture_output=True, text=True, timeout=15, check=False,
        )
        if inner.returncode != 0 or not cls.inner_capture.exists():
            raise AssertionError(
                f"Fake-inner session failed ({inner.returncode}): "
                f"{inner.stdout}\n{inner.stderr}"
            )
        cls.inner_records = read_json_lines(cls.inner_capture)
        tree_records = [r for r in cls.inner_records if r["argv"][:2] == ["-t", "get_tree"]]
        if not tree_records:
            raise AssertionError("inner.sh never queried the fake Sway tree")
        cls.nested_path = tree_records[-1]["path"]

        cls.nested_env = cls.sway_env.copy()
        cls.nested_env.update(
            WAYLAND_DISPLAY="wayland-sc7-nested-test",
            DISPLAY=":99",
            XDG_RUNTIME_DIR=str(cls.nested_runtime),
            DBUS_SESSION_BUS_ADDRESS="unix:path=/nested/test-bus",
            SWAYSOCK=str(cls.nested_runtime / "nested-sway.sock"),
            I3SOCK=str(cls.nested_runtime / "nested-i3.sock"),
            PATH=cls.nested_path,
            LD_LIBRARY_PATH=str(cls.base / "nested-only-libraries"),
            WLR_BACKENDS="nested-backend",
            WLR_LIBINPUT_NO_DEVICES="nested-libinput",
            WAYLAND_SOCKET="nested-wayland-fd",
            XDG_ACTIVATION_TOKEN="nested-activation-token",
            SC7_TEST_FILES_CAPTURE=str(cls.files_capture),
            SC7_TEST_MIME_CAPTURE=str(cls.mime_capture),
        )

    def setUp(self):
        self.open_capture.unlink(missing_ok=True)
        self.files_capture.unlink(missing_ok=True)
        self.mime_capture.unlink(missing_ok=True)

    def invoke(self, args, env=None, executable=SHIM):
        return subprocess.run(
            [str(executable), *map(str, args)],
            env=env or self.nested_env, cwd=self.home,
            capture_output=True, text=True, timeout=5, check=False,
        )

    def wait_for_records(self, count):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            records = read_json_lines(self.open_capture)
            if len(records) >= count:
                return records
            time.sleep(0.02)
        self.fail(f"Expected {count} host opener calls, got {read_json_lines(self.open_capture)}")

    def fixture_processes(self):
        """Find only helpers launched from this test's private executable paths."""
        names = (str(self.host_opener), str(self.host_bin / "arbitrary-mime-viewer"))
        running = set()
        for entry in Path("/proc").iterdir():
            if not entry.name.isdecimal():
                continue
            try:
                arguments = entry.joinpath("cmdline").read_bytes().split(b"\0")
            except (OSError, PermissionError):
                continue
            if any(os.fsencode(name) in arguments for name in names):
                running.add(int(entry.name))
        return running

    def wait_for_fixture_processes_to_exit(self):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            running = self.fixture_processes()
            if not running:
                return
            time.sleep(0.02)
        self.fail(f"Host-open test helpers did not exit: {sorted(running)}")

    def test_launcher_preserves_host_environment_and_scopes_private_path(self):
        expected = {
            "SC7_HOST_WAYLAND_DISPLAY": self.host_env["WAYLAND_DISPLAY"],
            "SC7_HOST_DISPLAY": self.host_env["DISPLAY"],
            "SC7_HOST_XDG_RUNTIME_DIR": self.host_env["XDG_RUNTIME_DIR"],
            "SC7_HOST_DBUS_SESSION_BUS_ADDRESS": self.host_bus,
            "SC7_HOST_SWAYSOCK": self.host_sway_socket,
            "SC7_HOST_I3SOCK": self.host_i3_socket,
            "SC7_HOST_PATH": self.host_path,
            "SC7_HOST_LD_LIBRARY_PATH": self.host_ld_path,
            "SC7_HOST_XDG_OPEN": str(self.host_opener.resolve()),
        }
        for key, value in expected.items():
            with self.subTest(variable=key):
                self.assertEqual(self.sway_env.get(key), value)
        self.assertEqual(self.sway_record["resolved_open"], str(SHIM))
        self.assertEqual(self.host_env["PATH"], self.host_path)

    def test_inner_session_keeps_private_opener_first(self):
        self.assertEqual(self.installed_inner.read_bytes(), INNER.read_bytes())
        self.assertEqual(self.inner_records[-1]["resolved_open"], str(SHIM))
        self.assertEqual(self.nested_path.split(":", 1)[0], str(SHIM.parent))
        self.assertEqual(self.sway_env["SC7_HOST_PATH"], self.host_path)

    def test_helper_restores_host_environment_and_exact_arguments(self):
        unusual = self.home / "report with spaces;$(touch SHOULD_NOT_EXIST)&.pdf"
        unusual.write_bytes(b"test document")
        args = [str(unusual), unusual.as_uri(), "https://example.test/a?q=one%20two&x=$value"]
        mime_file = self.config / "mimeapps.list"
        mime_before = b"[Default Applications]\napplication/pdf=unchanged.desktop;\n"
        mime_file.write_bytes(mime_before)
        mime_files_before = sorted(self.home.rglob("mimeapps.list")) + sorted(
            self.config.rglob("mimeapps.list")
        )

        result = self.invoke(args)

        self.assertEqual(result.returncode, 0, result.stderr)
        records = self.wait_for_records(len(args))
        self.assertEqual(sorted(record["argv"][0] for record in records), sorted(args))
        self.assertTrue(all(len(record["argv"]) == 1 for record in records))
        self.assertFalse((self.home / "SHOULD_NOT_EXIST").exists())
        expected = {
            "WAYLAND_DISPLAY": self.host_env["WAYLAND_DISPLAY"],
            "DISPLAY": self.host_env["DISPLAY"],
            "XDG_RUNTIME_DIR": str(self.runtime),
            "DBUS_SESSION_BUS_ADDRESS": self.host_bus,
            "SWAYSOCK": self.host_sway_socket,
            "I3SOCK": self.host_i3_socket,
            "PATH": self.host_path,
            "LD_LIBRARY_PATH": self.host_ld_path,
            "WLR_BACKENDS": self.host_env["WLR_BACKENDS"],
            "WLR_LIBINPUT_NO_DEVICES": self.host_env["WLR_LIBINPUT_NO_DEVICES"],
        }
        for record in records:
            for key, value in expected.items():
                with self.subTest(variable=key):
                    self.assertEqual(record["env"].get(key), value)
            self.assertNotIn("WAYLAND_SOCKET", record["env"])
            self.assertNotIn("XDG_ACTIVATION_TOKEN", record["env"])
        self.assertEqual(mime_file.read_bytes(), mime_before)
        self.assertEqual(
            sorted(self.home.rglob("mimeapps.list")) + sorted(self.config.rglob("mimeapps.list")),
            mime_files_before,
        )

    def test_unset_host_values_do_not_leak_nested_values(self):
        env = self.nested_env.copy()
        for key in ("DISPLAY", "SWAYSOCK", "I3SOCK", "LD_LIBRARY_PATH"):
            env[f"SC7_HOST_{key}"] = ""
            env[f"SC7_HOST_{key}_SET"] = ""
        result = self.invoke(["https://example.test/no-host-display"], env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        child_env = self.wait_for_records(1)[0]["env"]
        for key in ("DISPLAY", "SWAYSOCK", "I3SOCK", "LD_LIBRARY_PATH"):
            with self.subTest(variable=key):
                self.assertNotIn(key, child_env)

    def test_recursive_opener_is_rejected(self):
        env = self.nested_env.copy()
        env["SC7_HOST_XDG_OPEN"] = str(SHIM)
        result = self.invoke(["https://example.test/recursion"], env=env, executable=HELPER)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("recursive", result.stderr.lower())
        self.assertEqual(read_json_lines(self.open_capture), [])

    def test_slow_host_opener_does_not_block_files(self):
        slow_opener = self.host_bin / "xdg-open-slow"
        write_executable(slow_opener, "import time\ntime.sleep(1.5)\n")
        env = self.nested_env.copy()
        env["SC7_HOST_XDG_OPEN"] = str(slow_opener)
        started = time.monotonic()
        result = self.invoke(["https://example.test/slow"], env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertLess(time.monotonic() - started, 0.8)

    def test_directory_paths_stay_out_of_host_opener(self):
        directory = self.home / "folder with spaces"
        directory.mkdir(exist_ok=True)
        local_host_uri = directory.as_uri().replace(
            "file://", f"file://{socket.gethostname()}", 1
        )
        result = self.invoke([str(directory), directory.as_uri(), local_host_uri])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(read_json_lines(self.open_capture), [])

    def test_files_uses_nested_socket_but_direct_mime_apps_use_host(self):
        nested_socket = self.nested_runtime / self.nested_env["WAYLAND_DISPLAY"]
        documents = [self.home / "notes with spaces.md", self.home / "unknown.weird"]
        for document in documents:
            document.write_text("test", encoding="utf-8")

        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
            try:
                listener.bind(str(nested_socket))
            except PermissionError as error:
                if error.errno == errno.EPERM:
                    self.skipTest("sandbox forbids local UNIX socket listeners")
                raise
            listener.listen(1)
            listener.settimeout(3)
            result = self.invoke(documents, executable=FILES_BRIDGE)
            self.assertEqual(result.returncode, 0, result.stderr)
            with listener.accept()[0] as connection:
                self.assertEqual(
                    connection.recv(128), b"files-connected-to-nested-wayland"
                )

        files = json.loads(self.files_capture.read_text(encoding="utf-8"))
        mime = json.loads(self.mime_capture.read_text(encoding="utf-8"))
        self.assertEqual(files["argv"], list(map(str, documents)))
        self.assertEqual(mime["argv"], list(map(str, documents)))
        self.assertEqual(int(files["env"]["WAYLAND_SOCKET"]), files["socket_fd"])
        self.assertEqual(files["env"]["WAYLAND_DISPLAY"], self.host_env["WAYLAND_DISPLAY"])
        self.assertEqual(mime["env"]["WAYLAND_DISPLAY"], self.host_env["WAYLAND_DISPLAY"])
        self.assertNotIn("WAYLAND_SOCKET", mime["env"])

        for name in (
            "DISPLAY", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS", "SWAYSOCK",
            "I3SOCK", "PATH", "LD_LIBRARY_PATH", "WLR_BACKENDS",
            "WLR_LIBINPUT_NO_DEVICES",
        ):
            with self.subTest(variable=name):
                self.assertEqual(files["env"].get(name), self.host_env[name])
                self.assertEqual(mime["env"].get(name), self.host_env[name])
        for name in ("SC7_HOST_WAYLAND_DISPLAY", "SC7_RACK_PRIVATE_BIN", "XDG_ACTIVATION_TOKEN"):
            with self.subTest(stripped=name):
                self.assertNotIn(name, files["env"])
                self.assertNotIn(name, mime["env"])

    def test_files_bridge_rejects_missing_session(self):
        variations = (
            ("missing host display", {"SC7_HOST_WAYLAND_DISPLAY": None}),
            ("missing nested display", {"WAYLAND_DISPLAY": None}),
            ("missing host path", {"SC7_HOST_PATH": None}),
            ("same display", {"WAYLAND_DISPLAY": self.host_env["WAYLAND_DISPLAY"]}),
            ("missing nested socket", {"WAYLAND_DISPLAY": "wayland-absent-test"}),
        )
        for label, changes in variations:
            with self.subTest(case=label):
                env = self.nested_env.copy()
                for name, value in changes.items():
                    if value is None:
                        env.pop(name, None)
                    else:
                        env[name] = value
                result = self.invoke([self.home / "test.txt"], env=env, executable=FILES_BRIDGE)
                self.assertNotEqual(result.returncode, 0, label)
                self.assertFalse(self.files_capture.exists(), label)
                self.assertFalse(self.mime_capture.exists(), label)

    def test_repeated_host_open_and_direct_mime_launches_leave_no_helpers(self):
        """Exercise 100 cycles of both launch paths and verify session isolation."""
        nested_socket = self.nested_runtime / self.nested_env["WAYLAND_DISPLAY"]
        nested_socket.unlink(missing_ok=True)
        document = self.home / "long run test.md"
        document.write_text("test", encoding="utf-8")
        expected_uris = [f"https://example.test/host-open/{i}" for i in range(100)]

        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                listener.bind(str(nested_socket))
                listener.listen(1)
                listener.settimeout(5)
                for i, uri in enumerate(expected_uris):
                    result = self.invoke([uri])
                    self.assertEqual(result.returncode, 0, (i, result.stderr))

                    result = self.invoke([document], executable=FILES_BRIDGE)
                    self.assertEqual(result.returncode, 0, (i, result.stderr))
                    with listener.accept()[0] as connection:
                        self.assertEqual(
                            connection.recv(128), b"files-connected-to-nested-wayland"
                        )

                    files = json.loads(self.files_capture.read_text(encoding="utf-8"))
                    mime = json.loads(self.mime_capture.read_text(encoding="utf-8"))
                    self.assertEqual(files["argv"], [str(document)])
                    self.assertEqual(mime["argv"], [str(document)])
                    self.assertEqual(files["env"]["WAYLAND_DISPLAY"], self.host_env["WAYLAND_DISPLAY"])
                    self.assertEqual(mime["env"]["WAYLAND_DISPLAY"], self.host_env["WAYLAND_DISPLAY"])
                    self.assertEqual(int(files["env"]["WAYLAND_SOCKET"]), files["socket_fd"])
                    self.assertNotIn("WAYLAND_SOCKET", mime["env"])
                    self.assertEqual(mime["env"].get("DBUS_SESSION_BUS_ADDRESS"), self.host_bus)
        except PermissionError as error:
            if error.errno == errno.EPERM:
                self.skipTest("sandbox forbids local UNIX socket listeners")
            raise

        records = self.wait_for_records(100)
        self.assertEqual(sorted(record["argv"][0] for record in records), sorted(expected_uris))
        for record in records:
            self.assertEqual(record["env"]["WAYLAND_DISPLAY"], self.host_env["WAYLAND_DISPLAY"])
            self.assertEqual(record["env"].get("DBUS_SESSION_BUS_ADDRESS"), self.host_bus)
            self.assertNotIn("WAYLAND_SOCKET", record["env"])
        self.wait_for_fixture_processes_to_exit()


if __name__ == "__main__":
    unittest.main(verbosity=2)
