"""Ownership and bounded evidence regressions for the read-only Files snapshot."""
import importlib.util
from contextlib import redirect_stdout, redirect_stderr
import io
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/capture-files-session.py"
spec = importlib.util.spec_from_file_location("files_session_snapshot", SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def record(kind, payload=b"", *, sequence=1976, flags=0):
    size = 16 + len(payload)
    result = struct.pack("=IHHII", size, kind, flags, sequence, 0) + payload
    return result + b"\0" * ((-size) % 4)


def peer(inode, other):
    payload = struct.pack("=BBBBIII", socket.AF_UNIX, 1, 1, 0, inode, 0, 0)
    payload += struct.pack("=HHI", 8, 2, other)  # UNIX_DIAG_PEER from linux/unix_diag.h
    return record(20, payload)


class FilesSessionDiagnosticTests(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory(prefix="sc7-files-snapshot-", dir=ROOT)
        self.addCleanup(work.cleanup)
        self.work = Path(work.name)

    def proc(self, pid=42, name="cosmic-files", start=991, inode=123):
        proc = self.work / str(pid)
        proc.mkdir()
        (proc / "fd").mkdir()
        (proc / "task" / str(pid)).mkdir(parents=True)
        stat = f"{pid} ({name} with spaces) S " + " ".join(["0"] * 18 + [str(start)] + ["0"] * 30)
        (proc / "stat").write_text(stat)
        (proc / "comm").write_text(name + "\n")
        (proc / "status").write_text("Name:\tcosmic-files\nState:\tS (sleeping)\nThreads:\t1\nVmRSS:\t2048 kB\nUid:\tsecret\n")
        (proc / "cmdline").write_bytes(b"/usr/bin/cosmic-files\0/private/directory\0")
        (proc / "fd/7").symlink_to(f"socket:[{inode}]")
        (proc / "fd/8").symlink_to("anon_inode:inotify")
        thread = proc / "task" / str(pid)
        (thread / "stat").write_text(stat)
        (thread / "comm").write_text("worker\n")
        (thread / "wchan").write_text("futex_wait_queue\n")
        return proc

    def test_socket_peer_attributes_and_done_are_decoded(self):
        mapping, done = module.decode_peer_messages(peer(100, 200) + peer(300, 400) + record(3), 1976)
        self.assertEqual(mapping, {100: 200, 300: 400})
        self.assertTrue(done)

    def test_bad_or_interrupted_netlink_evidence_is_rejected(self):
        cases = [record(20, b"short"), record(2), record(3, sequence=1),
                 record(3, flags=0x10), struct.pack("=IHHII", 2, 20, 0, 1976, 0),
                 record(20, struct.pack("=BBBBIII", socket.AF_UNIX, 1, 1, 0, 100, 0, 0)
                        + struct.pack("=HH", 9, 3))]
        for data in cases:
            with self.subTest(data=data), self.assertRaises(RuntimeError):
                module.decode_peer_messages(data, 1976)

    def test_kernel_error_is_not_accepted_as_empty_success(self):
        with self.assertRaises(OSError):
            module.decode_peer_messages(record(2, struct.pack("=i", -13)), 1976)

    def test_peer_query_keeps_only_exact_compositor_connections(self):
        class FakeSocket:
            def __enter__(self): return self
            def __exit__(self, *_): pass
            def settimeout(self, _): pass
            def send(self, request): self.request = request
            def recv(self, _): return peer(100, 200) + peer(300, 400) + record(3)
        fake = FakeSocket()
        with patch.object(module.socket, "socket", return_value=fake):
            self.assertEqual(module.server_peer_inodes({100}), {200})
        self.assertEqual(struct.unpack_from("=IHHII", fake.request)[1:3], (20, 0x301))

    def test_host_files_with_same_name_but_other_socket_is_excluded(self):
        proc = self.proc()
        self.assertTrue(module.owned_client(proc, "cosmic-files", {123}))
        self.assertFalse(module.owned_client(proc, "cosmic-files", {987}))
        self.assertFalse(module.owned_client(proc, "cosmic-term", {123}))

    def test_daemonized_files_can_be_identified_without_dead_tree_pid(self):
        proc = self.proc(pid=43)
        self.assertTrue(module.owned_client(proc, "cosmic-files", {123}))
        self.assertFalse(module.owned_client(self.work / "42", "cosmic-files", {123}))

    def test_details_report_counters_without_environment_or_file_names(self):
        proc = self.proc()
        details = module.process_details(proc, {123}, "cosmic-files")
        self.assertEqual(details["start_tick"], 991)
        self.assertEqual(details["fd_count"], 2)
        self.assertEqual(details["fd_categories"]["inotify"], 1)
        self.assertEqual(details["threads"][0]["wait_channel"], "futex_wait_queue")
        self.assertNotIn("Uid", details["status"])
        self.assertNotIn("private/directory", json.dumps(details))

    def test_identity_change_or_lost_peer_is_rejected(self):
        proc = self.proc()
        with patch.object(module, "identity", side_effect=[991, 992]):
            with self.assertRaisesRegex(RuntimeError, "identity changed"):
                module.process_details(proc, {123}, "cosmic-files")
        with self.assertRaisesRegex(RuntimeError, "ownership changed"):
            module.process_details(proc, {987}, "cosmic-files")

    def test_wrong_local_binary_or_config_is_rejected(self):
        proc = self.proc(name="sway")
        actual = self.work / "sway"
        actual.touch()
        (proc / "exe").symlink_to(actual)
        config = self.work / "config"
        (proc / "cmdline").write_bytes(os.fsencode(actual) + b"\0--config\0" + os.fsencode(config) + b"\0")
        self.assertEqual(module.validate_compositor(proc, actual, config), 991)
        with self.assertRaisesRegex(RuntimeError, "local Sway"):
            module.validate_compositor(proc, self.work / "other", config)
        with self.assertRaisesRegex(RuntimeError, "installed Rack config"):
            module.validate_compositor(proc, actual, self.work / "wrong")

    def test_nested_floating_dialogs_and_focus_are_retained(self):
        tree = {"nodes": [{"app_id": "com.system76.CosmicFiles", "pid": 42, "focused": True}],
                "floating_nodes": [{"app_id": "com.system76.CosmicFilesDialog", "pid": 42}]}
        found = module.views(tree)
        self.assertEqual(len(found), 2)
        self.assertTrue(found[0]["focused"])
        self.assertEqual(found[1]["app_id"], "com.system76.CosmicFilesDialog")

    def test_rebuilt_local_binary_is_identifiable_but_other_deleted_binary_is_not(self):
        proc = self.proc(name="sway")
        actual = self.work / "sway"
        config = self.work / "config"
        (proc / "exe").symlink_to(str(actual) + " (deleted)")
        (proc / "cmdline").write_bytes(os.fsencode(actual) + b"\0--config\0" + os.fsencode(config) + b"\0")
        self.assertEqual(module.validate_compositor(proc, actual, config), 991)
        with self.assertRaisesRegex(RuntimeError, "local Sway"):
            module.validate_compositor(proc, self.work / "different", config)

    def test_absent_rack_does_not_create_misleading_snapshot(self):
        runtime = self.work / "runtime"
        runtime.mkdir()
        output = self.work / "output"
        result = subprocess.run(["python3", str(SCRIPT), "--phase", "before", "--output", str(output)],
                                env={**os.environ, "XDG_RUNTIME_DIR": str(runtime)}, capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertFalse(output.exists())
        self.assertIn("Snapshot unavailable", result.stderr)

    def test_cli_writes_private_verified_and_unverified_evidence(self):
        output = self.work / "output"
        for proven in (True, False):
            data = {"files": [{"pid": 43}] if proven else [], "ownership_errors": []}
            stdout, stderr = io.StringIO(), io.StringIO()
            with patch.object(module, "snapshot", return_value=data), \
                 patch.object(module.sys, "argv", [str(SCRIPT), "--phase", "before", "--output", str(output)]), \
                 redirect_stdout(stdout), redirect_stderr(stderr):
                result = module.main()
            destination = Path(stdout.getvalue().strip())
            self.assertEqual(result, 0 if proven else 3)
            self.assertEqual(json.loads(destination.read_text()), data)
            self.assertEqual(destination.stat().st_mode & 0o777, 0o600)
            self.assertEqual(output.stat().st_mode & 0o777, 0o700)
            if not proven:
                self.assertIn("UNVERIFIED", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
