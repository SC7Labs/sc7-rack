#!/usr/bin/env python3
"""Exercise repeated clipboard handoffs in an isolated nested Wayland Rack."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
BRIDGE = ROOT / "bin/sc7-clipboard-bridge"
WLROOTS_BUILD = ROOT / "vendor/wlroots/build"
RACK_SWAY = ROOT / "vendor/sway/build/sway/sway"
CYCLES = 140


def wayland_sockets(runtime: Path) -> set[Path]:
    return {path for path in runtime.glob("wayland-*") if path.is_socket()}


def wait_for_new_socket(runtime: Path, before: set[Path], process: subprocess.Popen) -> Path:
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        found = wayland_sockets(runtime) - before
        if found:
            return sorted(found)[0]
        if process.poll() is not None:
            raise AssertionError(f"isolated Sway exited with {process.returncode}")
        time.sleep(0.05)
    raise AssertionError("isolated Sway did not open its Wayland socket")


def process_metrics(pid: int) -> tuple[int, int]:
    proc = Path("/proc") / str(pid)
    descriptors = len(list((proc / "fd").iterdir()))
    rss = 0
    for line in (proc / "status").read_text().splitlines():
        if line.startswith("VmRSS:"):
            rss = int(line.split()[1])
            break
    return descriptors, rss


def isolated_process_count(runtime: Path) -> int:
    """Count processes using only this test's private Wayland runtime."""
    expected = os.fsencode(f"XDG_RUNTIME_DIR={runtime}")
    count = 0
    for entry in Path("/proc").iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            environment = (entry / "environ").read_bytes().split(b"\0")
        except (OSError, PermissionError):
            continue
        count += expected in environment
    return count


def stop_exact(process: subprocess.Popen) -> None:
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


@unittest.skipUnless(
    all(shutil.which(name) for name in ("sway", "wl-copy", "wl-paste"))
    and BRIDGE.is_file() and (WLROOTS_BUILD / "libwlroots.so.12").is_file()
    and RACK_SWAY.is_file(),
    "Sway, wl-clipboard, bridge, and patched Rack builds required",
)
class IsolatedClipboardReliabilityTests(unittest.TestCase):
    def test_140_bidirectional_ownership_swaps_plateau(self) -> None:
        subprocess.run([str(ROOT / "scripts/bootstrap-sway.sh"), "--check"],
                       check=True, capture_output=True)
        # Both compositors and every clipboard owner use private sockets; the
        # host desktop's clipboard and the user's running Rack are untouched.
        with tempfile.TemporaryDirectory(prefix="sc7-reliability-") as temp:
            runtime = Path(temp)
            config = runtime / "sway.conf"
            config.write_text(
                "output * resolution 800x600\nworkspace 1\nxwayland disable\n"
            )
            base_env = os.environ.copy()
            base_env.update(
                XDG_RUNTIME_DIR=temp,
                XDG_CONFIG_HOME=temp,
                WLR_RENDERER="pixman",
                WLR_LIBINPUT_NO_DEVICES="1",
                LD_LIBRARY_PATH=str(WLROOTS_BUILD),
            )
            for name in ("WAYLAND_DISPLAY", "DISPLAY", "SWAYSOCK", "I3SOCK"):
                base_env.pop(name, None)

            processes: list[subprocess.Popen] = []
            owners: list[subprocess.Popen] = []
            logs = [open(runtime / name, "w") for name in ("host.log", "nested.log", "bridge.log")]
            try:
                host_env = dict(base_env, WLR_BACKENDS="headless")
                host = subprocess.Popen(
                    ["sway", "-c", str(config)], env=host_env,
                    stdout=logs[0], stderr=subprocess.STDOUT, start_new_session=True,
                )
                processes.append(host)
                before = wayland_sockets(runtime)
                try:
                    host_socket = wait_for_new_socket(runtime, before, host)
                except AssertionError:
                    logs[0].flush()
                    if "Unable to open wayland socket" in (runtime / "host.log").read_text():
                        self.skipTest("sandbox blocks local Wayland sockets")
                    raise

                nested_env = dict(base_env, WLR_BACKENDS="wayland", WAYLAND_DISPLAY=host_socket.name)
                before = wayland_sockets(runtime)
                nested = subprocess.Popen(
                    [str(RACK_SWAY), "-c", str(config)], env=nested_env,
                    stdout=logs[1], stderr=subprocess.STDOUT, start_new_session=True,
                )
                processes.append(nested)
                nested_socket = wait_for_new_socket(runtime, before, nested)

                bridge = subprocess.Popen(
                    [str(BRIDGE), "--host", host_socket.name, "--nested", nested_socket.name],
                    env=base_env, stdout=logs[2], stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
                processes.append(bridge)
                time.sleep(0.3)
                self.assertIsNone(bridge.poll(), "clipboard bridge exited during setup")

                host_copy_env = dict(base_env, WAYLAND_DISPLAY=host_socket.name)
                nested_copy_env = dict(base_env, WAYLAND_DISPLAY=nested_socket.name)
                checkpoints: dict[int, tuple[tuple[int, int], tuple[int, int], tuple[int, int], int]] = {}
                for cycle in range(CYCLES):
                    source, destination = (
                        (host_copy_env, nested_copy_env) if cycle % 2 == 0
                        else (nested_copy_env, host_copy_env)
                    )
                    expected = f"sc7-reliability-cycle-{cycle:03d}".encode()
                    owner = subprocess.Popen(
                        ["wl-copy", "-f"], env=source, stdin=subprocess.PIPE,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    )
                    owners.append(owner)
                    assert owner.stdin is not None
                    owner.stdin.write(expected)
                    owner.stdin.close()

                    deadline = time.monotonic() + 3
                    matched = False
                    while time.monotonic() < deadline:
                        try:
                            paste = subprocess.run(
                                ["wl-paste", "-n"], env=destination,
                                capture_output=True, timeout=0.75,
                            )
                        except subprocess.TimeoutExpired:
                            continue
                        if paste.returncode == 0 and paste.stdout == expected:
                            matched = True
                            break
                        time.sleep(0.05)
                    self.assertTrue(matched, f"clipboard handoff failed at cycle {cycle + 1}")
                    self.assertIsNone(bridge.poll(), f"clipboard bridge exited at cycle {cycle + 1}")
                    self.assertIsNone(nested.poll(), f"nested Sway exited at cycle {cycle + 1}")
                    if cycle in (19, 69, CYCLES - 1):
                        checkpoints[cycle] = (
                            process_metrics(bridge.pid), process_metrics(nested.pid),
                            process_metrics(host.pid),
                            isolated_process_count(runtime),
                        )

                early_bridge, early_sway, early_host, early_processes = checkpoints[19]
                late_bridge, late_sway, late_host, late_processes = checkpoints[CYCLES - 1]
                print(
                    f"isolated {CYCLES} swaps: bridge fd {early_bridge[0]}->{late_bridge[0]}, "
                    f"RSS {early_bridge[1]}->{late_bridge[1]} KiB; "
                    f"nested Sway fd {early_sway[0]}->{late_sway[0]}, "
                    f"RSS {early_sway[1]}->{late_sway[1]} KiB; "
                    f"host Sway fd {early_host[0]}->{late_host[0]}, "
                    f"RSS {early_host[1]}->{late_host[1]} KiB; "
                    f"runtime processes {early_processes}->{late_processes}"
                )
                self.assertLessEqual(late_bridge[0], early_bridge[0] + 8)
                self.assertLessEqual(late_sway[0], early_sway[0] + 8)
                self.assertLessEqual(late_host[0], early_host[0] + 8)
                self.assertLessEqual(late_bridge[1], early_bridge[1] + 8192)
                self.assertLessEqual(late_sway[1], early_sway[1] + 8192)
                self.assertLessEqual(late_host[1], early_host[1] + 8192)
                self.assertLessEqual(late_processes, early_processes + 2)
            finally:
                for owner in owners:
                    stop_exact(owner)
                for process in reversed(processes):
                    stop_exact(process)
                for log in logs:
                    log.close()


if __name__ == "__main__":
    unittest.main()
