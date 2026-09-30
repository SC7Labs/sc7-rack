#!/usr/bin/env python3
"""Exercise the real Rack layout in an isolated headless Sway session."""

import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
RACK_SWAY = ROOT / "vendor/sway/build/sway/sway"
WLROOTS_BUILD = ROOT / "vendor/wlroots/build"


def write_executable(path: Path, content: str) -> None:
    path.write_text(content)
    path.chmod(0o755)


@unittest.skipUnless(RACK_SWAY.is_file() and shutil.which("foot"),
                     "Rack-local Sway and foot required")
class NoGpuLayoutTests(unittest.TestCase):
    def test_no_gpu_terminal_persists_in_four_quadrants(self):
        subprocess.run([str(ROOT / "scripts/bootstrap-sway.sh"), "--check"],
                       check=True, capture_output=True)
        with tempfile.TemporaryDirectory(prefix="sc7-no-gpu-layout-") as tmp:
            base = Path(tmp)
            fake_bin = base / "fake-bin"
            private_bin = base / "app-bin" / "rack-private"
            config_dir = base / "config" / "sc7-rack"
            for directory in (fake_bin, private_bin, config_dir):
                directory.mkdir(parents=True)

            (config_dir / "settings.json").write_text(json.dumps({
                "gpu_provider": "auto",
                "share_clipboard": False,
                "files_path": tmp,
            }))
            sway_config = base / "sway.conf"
            sway_config.write_text(
                "output * resolution 800x600\n"
                "workspace 1\n"
                "focus_follows_mouse no\n"
            )
            # This test host has GPU utilities. Simulate the fresh VM where all
            # four optional providers are absent without changing host PATH.
            bash_env = base / "no-gpu.bashenv"
            bash_env.write_text(
                "command() {\n"
                "  if [[ ${1:-} == -v ]]; then\n"
                "    case ${2:-} in intel_gpu_top|nvidia-smi|nvtop|radeontop) return 1;; esac\n"
                "  fi\n"
                "  builtin command \"$@\"\n"
                "}\n"
            )
            payload_file = base / "gpu-command.txt"
            write_executable(fake_bin / "cosmic-term", """#!/usr/bin/env bash
if [[ "${*: -1}" == 'exec htop' ]]; then
    exec foot --app-id=htop -e sleep 30
fi
printf '%s' "${*: -1}" > "$SC7_TEST_GPU_COMMAND"
exec foot --app-id=gpu -e bash -lc "${*: -1}"
""")
            write_executable(fake_bin / "cosmic-monitor", """#!/usr/bin/env bash
exec foot --app-id=monitor -e sleep 30
""")
            write_executable(private_bin.parent / "sc7-rack-files", """#!/usr/bin/env bash
exec foot --app-id=files -e sleep 30
""")

            env = os.environ.copy()
            env.update({
                "XDG_RUNTIME_DIR": tmp,
                "XDG_CONFIG_HOME": str(base / "config"),
                "XDG_BIN_HOME": str(fake_bin),
                "SC7_RACK_PRIVATE_BIN": str(private_bin),
                "SC7_TEST_GPU_COMMAND": str(payload_file),
                "BASH_ENV": str(bash_env),
                "WLR_BACKENDS": "headless",
                "WLR_RENDERER": "pixman",
                "WLR_LIBINPUT_NO_DEVICES": "1",
                "LD_LIBRARY_PATH": str(WLROOTS_BUILD),
            })
            for key in ("WAYLAND_DISPLAY", "DISPLAY", "SWAYSOCK"):
                env.pop(key, None)

            sway_log = (base / "sway.log").open("w")
            inner_log = (base / "inner.log").open("w")
            sway = subprocess.Popen(
                [str(RACK_SWAY), "-c", str(sway_config)], env=env,
                stdout=sway_log, stderr=subprocess.STDOUT, start_new_session=True,
            )
            inner = None
            try:
                try:
                    ipc = self.wait_for_socket(base, "sway-ipc.*.sock", sway)
                except AssertionError:
                    sway_log.flush()
                    if "Unable to open wayland socket" in (base / "sway.log").read_text():
                        self.skipTest("sandbox blocks Wayland socket creation; run this test with local socket access")
                    raise
                wayland = self.wait_for_socket(base, "wayland-*", sway)
                env["SWAYSOCK"] = str(ipc)
                env["WAYLAND_DISPLAY"] = wayland.name

                inner = subprocess.Popen(
                    ["bash", str(ROOT / "config" / "inner.sh")], env=env,
                    stdout=inner_log, stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
                inner.wait(timeout=22)
                inner_log.flush()
                self.assertEqual(inner.returncode, 0, (base / "inner.log").read_text() + (base / "sway.log").read_text())

                # The former fallback closed immediately because it began with
                # `exec echo`; even a delayed exit would re-tile after five sec.
                time.sleep(6)
                tree = self.sway_tree(env)
                leaves = self.views(tree)
                self.assertEqual(set(leaves), {"gpu", "files", "monitor", "htop"})
                self.assertEqual(len(leaves), 4)
                command = payload_file.read_text()
                self.assertIn("No supported GPU monitor found", command)
                self.assertIn("exec bash -i", command)

                gpu, files = leaves["gpu"], leaves["files"]
                monitor, htop = leaves["monitor"], leaves["htop"]
                self.assertEqual(gpu["x"], monitor["x"])
                self.assertEqual(files["x"], htop["x"])
                self.assertLess(gpu["x"], files["x"])
                self.assertEqual(gpu["y"], files["y"])
                self.assertEqual(monitor["y"], htop["y"])
                self.assertLess(gpu["y"], monitor["y"])
                self.assertGreater(min(r["width"] for r in leaves.values()), 0)
                self.assertGreater(min(r["height"] for r in leaves.values()), 0)
            finally:
                if inner is not None:
                    try:
                        os.killpg(inner.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                sway.terminate()
                try:
                    sway.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    sway.kill()
                    sway.wait(timeout=3)
                sway_log.close()
                inner_log.close()

    @staticmethod
    def wait_for_socket(base: Path, pattern: str, sway: subprocess.Popen) -> Path:
        for _ in range(100):
            for candidate in base.glob(pattern):
                if candidate.is_socket():
                    return candidate
            if sway.poll() is not None:
                raise AssertionError(f"headless Sway exited with {sway.returncode}")
            time.sleep(0.05)
        raise AssertionError(f"headless Sway did not create {pattern}")

    @staticmethod
    def sway_tree(env: dict) -> dict:
        return json.loads(subprocess.check_output(
            ["swaymsg", "-t", "get_tree", "-r"], env=env, text=True,
        ))

    @staticmethod
    def views(tree: dict) -> dict:
        result = {}

        def visit(node):
            if node.get("app_id"):
                result[node["app_id"]] = node["rect"]
            for child in node.get("nodes", []) + node.get("floating_nodes", []):
                visit(child)

        visit(tree)
        return result


if __name__ == "__main__":
    unittest.main()
