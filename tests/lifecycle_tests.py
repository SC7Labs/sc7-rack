#!/usr/bin/env python3
"""Behavioral checks for the nested layout helper's Sway IPC lifecycle."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
INNER = ROOT / "config" / "inner.sh"


class InnerLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="sc7-rack-lifecycle-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        (self.root / "home").mkdir()
        (self.root / "run").mkdir()
        (self.root / "config").mkdir()
        self.calls = self.root / "ipc-calls.jsonl"
        self.launches = self.root / "launches.txt"

        fake_swaymsg = self.bin / "swaymsg"
        fake_swaymsg.write_text(
            "#!/usr/bin/env python3\n"
            "import json, os, sys\n"
            "from pathlib import Path\n"
            "call = sys.argv[1:]\n"
            "with open(os.environ['SC7_CALLS'], 'a') as out:\n"
            "    out.write(json.dumps(call) + '\\n')\n"
            "if call == ['workspace', '1']:\n"
            "    sys.exit(0)\n"
            "if ' '.join(call) == os.environ.get('SC7_FAIL_COMMAND', ''):\n"
            "    print('Unable to connect to stale socket', file=sys.stderr)\n"
            "    sys.exit(1)\n"
            "if call == ['-t', 'get_tree', '-r']:\n"
            "    count = sum(json.loads(line) == call for line in "
            "Path(os.environ['SC7_CALLS']).read_text().splitlines())\n"
            "    if count >= int(os.environ['SC7_FAIL_TREE_AT']):\n"
            "        print('Unable to connect to stale socket', file=sys.stderr)\n"
            "        sys.exit(1)\n"
            "    views = int(os.environ.get('SC7_TREE_VIEWS', '0'))\n"
            "    nodes = [{'type': 'con', 'app_id': 'gpu'} for _ in range(views)]\n"
            "    print(json.dumps({'type': 'root', 'nodes': nodes, 'floating_nodes': []}))\n"
            "    sys.exit(0)\n"
            "sys.exit(0)\n"
        )
        fake_swaymsg.chmod(0o755)

        fake_term = self.bin / "cosmic-term"
        fake_term.write_text(
            "#!/bin/sh\n"
            "printf '%s\\n' term >> \"$SC7_LAUNCHES\"\n"
        )
        fake_term.chmod(0o755)

        self.env = os.environ.copy()
        self.env.update(
            HOME=str(self.root / "home"),
            XDG_BIN_HOME=str(self.bin),
            XDG_CONFIG_HOME=str(self.root / "config"),
            XDG_RUNTIME_DIR=str(self.root / "run"),
            SC7_RACK_PRIVATE_BIN=str(self.root / "rack-private"),
            SC7_HOST_WAYLAND_DISPLAY="",
            WAYLAND_DISPLAY="",
            SC7_CALLS=str(self.calls),
            SC7_LAUNCHES=str(self.launches),
        )

    def run_inner(self, fail_tree_at, **settings):
        env = self.env | {"SC7_FAIL_TREE_AT": str(fail_tree_at)} | settings
        return subprocess.run(
            ["bash", str(INNER)], env=env, text=True,
            capture_output=True, timeout=3, check=False,
        )

    def ipc_calls(self):
        return [json.loads(line) for line in self.calls.read_text().splitlines()]

    def test_disconnect_before_layout_stops_without_retry(self):
        result = self.run_inner(fail_tree_at=1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.ipc_calls(),
            [["workspace", "1"], ["-t", "get_tree", "-r"]],
        )
        self.assertNotIn("Unable to connect", result.stderr)
        self.assertFalse(self.launches.exists())
        self.assertFalse((self.root / "run" / "sc7-rack" / "inner_helper.pid").exists())

    def test_disconnect_during_view_wait_stops_remaining_layout(self):
        result = self.run_inner(fail_tree_at=3)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.ipc_calls(),
            [["workspace", "1"], ["-t", "get_tree", "-r"],
             ["-t", "get_tree", "-r"], ["-t", "get_tree", "-r"]],
        )
        self.assertEqual(self.launches.read_text().splitlines(), ["term"])
        self.assertNotIn("Unable to connect", result.stderr)
        self.assertFalse((self.root / "run" / "sc7-rack" / "inner_helper.pid").exists())

    def test_disconnect_on_layout_command_stops_before_next_pane(self):
        result = self.run_inner(
            fail_tree_at=99, SC7_TREE_VIEWS="1", SC7_FAIL_COMMAND="split h",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.ipc_calls(),
            [["workspace", "1"], ["-t", "get_tree", "-r"],
             ["-t", "get_tree", "-r"], ["split", "h"]],
        )
        self.assertEqual(self.launches.read_text().splitlines(), ["term"])
        self.assertNotIn("Unable to connect", result.stderr)

    def test_startup_does_not_kill_unrelated_bridge_process(self):
        foreign_bridge = self.bin / "sc7-clipboard-bridge"
        foreign_bridge.write_text("#!/bin/sh\nwhile :; do sleep 1; done\n")
        foreign_bridge.chmod(0o755)
        foreign = subprocess.Popen(
            [str(foreign_bridge)], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            result = self.run_inner(fail_tree_at=1)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIsNone(foreign.poll(), "Rack stopped an unrelated bridge")
        finally:
            foreign.terminate()
            try:
                foreign.wait(timeout=2)
            except subprocess.TimeoutExpired:
                foreign.kill()
                foreign.wait(timeout=2)


if __name__ == "__main__":
    unittest.main()
