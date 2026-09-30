#!/usr/bin/env python3
"""Protocol-level xdg_popup lifecycle regression against an isolated Sway.

The C client performs real Wayland requests and asserts event ordering. This
runner also rejects wlroots' uninitialized-xdg-surface warning: a broken
compositor can discard an early configure rather than send it to the client.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
CLIENT = ROOT / "tests/popup_lifecycle_client.c"
PROTOCOL = Path("/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml")
WARNING = "A configure is scheduled for an uninitialized xdg_surface"


def run(*command: str, **kwargs: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, check=True, **kwargs)


def build_client(work: Path) -> Path:
    header = work / "xdg-shell-client-protocol.h"
    protocol_code = work / "xdg-shell-protocol.c"
    binary = work / "popup-lifecycle-client"
    run("wayland-scanner", "client-header", str(PROTOCOL), str(header))
    run("wayland-scanner", "private-code", str(PROTOCOL), str(protocol_code))
    cflags = shlex.split(run("pkg-config", "--cflags", "wayland-client",
                             capture_output=True).stdout)
    libs = shlex.split(run("pkg-config", "--libs", "wayland-client",
                           capture_output=True).stdout)
    run("cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", str(work), *cflags, str(CLIENT), str(protocol_code),
        "-o", str(binary), *libs)
    return binary


def socket_paths(runtime: Path) -> set[Path]:
    return {path for path in runtime.glob("wayland-*") if path.is_socket()}


def wait_for_socket(runtime: Path, original: set[Path],
                    sway: subprocess.Popen[bytes], log: Path) -> Path:
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        sockets = socket_paths(runtime) - original
        if sockets:
            return sorted(sockets)[0]
        if sway.poll() is not None:
            raise RuntimeError(f"Sway exited {sway.returncode}:\n"
                               f"{log.read_text(errors='replace')[-6000:]}")
        time.sleep(0.05)
    raise RuntimeError(f"Sway did not open a private socket:\n"
                       f"{log.read_text(errors='replace')[-6000:]}")


def stop_exact(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def exercise(binary: Path, cycles: int, environment: dict[str, str],
             log: Path | None) -> None:
    result = subprocess.run([str(binary), str(cycles)], env=environment,
                            capture_output=True, text=True, timeout=30)
    server_log = log.read_text(errors="replace") if log else ""
    if result.returncode != 0 or WARNING in server_log:
        raise RuntimeError(
            f"popup client exited {result.returncode}; "
            f"uninitialized configure warning={WARNING in server_log}\n"
            f"client stdout:\n{result.stdout[-2000:]}\n"
            f"client stderr:\n{result.stderr[-4000:]}\n"
            f"compositor log tail:\n{server_log[-6000:]}"
        )
    print(result.stdout.strip())


def check_prerequisites() -> None:
    for name in ("wayland-scanner", "pkg-config", "cc"):
        if not shutil.which(name):
            raise RuntimeError(f"{name} is required")
    if not PROTOCOL.is_file():
        raise RuntimeError(f"xdg-shell protocol XML missing: {PROTOCOL}")


def exercise_nested(binary: Path, cycles: int, sway_binary: str,
                    wlroots_library: Path, work: Path, runtime: Path,
                    host_environment: dict[str, str],
                    via_launcher: bool = False) -> None:
    """Run the client inside private Sway-on-Sway, with Rack's real config."""
    host_config = work / "host.conf"
    host_config.write_text("output * resolution 800x600\nworkspace 1\nxwayland disable\n")
    # The Rack layout helper would open actual desktop applications. Omit only
    # that exec line, keeping all of Rack's compositor and window rules.
    nested_config = work / "rack.conf"
    nested_config.write_text("\n".join(
        line for line in (ROOT / "config/config").read_text().splitlines()
        if not line.startswith("exec --no-startup-id")
    ) + "\n")
    host_log = work / "host.log"
    nested_log = work / "rack.log"
    before = socket_paths(runtime)
    with host_log.open("wb") as host_output:
        host = subprocess.Popen(["/usr/bin/sway", "-d", "-c", str(host_config)],
                                env=host_environment, stdout=host_output,
                                stderr=subprocess.STDOUT, start_new_session=True)
        try:
            host_socket = wait_for_socket(runtime, before, host, host_log)
            nested_environment = dict(host_environment)
            nested_environment.update(
                WAYLAND_DISPLAY=host_socket.name, WLR_BACKENDS="wayland",
                LD_LIBRARY_PATH=str(wlroots_library.resolve()) + os.pathsep +
                host_environment.get("LD_LIBRARY_PATH", ""),
            )
            if via_launcher:
                private_home = work / "home"
                private_config = work / "config"
                rack_config = private_config / "sc7-rack"
                private_home.mkdir()
                rack_config.mkdir(parents=True)
                (rack_config / "config").write_text(nested_config.read_text())
                nested_environment.update(HOME=str(private_home),
                                          XDG_CONFIG_HOME=str(private_config))
                nested_environment.pop("SC7_RACK_SWAY_BINARY", None)
                nested_command = [str(ROOT / "bin/sc7-rack")]
            else:
                nested_command = [sway_binary, "-d", "-c", str(nested_config)]
            before_nested = socket_paths(runtime)
            with nested_log.open("wb") as nested_output:
                nested = subprocess.Popen(
                    nested_command,
                    env=nested_environment, stdout=nested_output,
                    stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    nested_socket = wait_for_socket(
                        runtime, before_nested, nested, nested_log)
                    if via_launcher:
                        pid_file = runtime / "sc7-rack/sc7-rack.pid"
                        nested_pid = int(pid_file.read_text().strip())
                        executable = Path(f"/proc/{nested_pid}/exe").resolve()
                        expected = ROOT / "vendor/sway/build/sway/sway"
                        if executable != expected.resolve():
                            raise RuntimeError(
                                f"launcher selected {executable}, expected {expected}")
                    client_environment = dict(nested_environment,
                                              WAYLAND_DISPLAY=nested_socket.name)
                    exercise(binary, cycles, client_environment, nested_log)
                    if nested.poll() is not None or host.poll() is not None:
                        raise RuntimeError("A compositor exited during nested popup test")
                finally:
                    stop_exact(nested)
        finally:
            stop_exact(host)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sway", default="sway", help="Sway executable")
    parser.add_argument("--wlroots-library", type=Path,
                        help="prepend Rack-local wlroots build to LD_LIBRARY_PATH")
    parser.add_argument("--cycles", type=int, default=32)
    parser.add_argument("--client-only", action="store_true",
                        help="use current XDG_RUNTIME_DIR and WAYLAND_DISPLAY")
    parser.add_argument("--nested", action="store_true",
                        help="run inside a private headless host plus Rack Wayland backend")
    parser.add_argument("--via-launcher", action="store_true",
                        help="start nested Rack through bin/sc7-rack and verify local Sway")
    args = parser.parse_args()
    if not 1 <= args.cycles <= 1000:
        parser.error("--cycles must be 1..1000")
    if (args.nested or args.via_launcher) and (args.client_only or not args.wlroots_library):
        parser.error("nested modes require --wlroots-library and exclude --client-only")
    check_prerequisites()

    # Put compiled binaries under the executable workspace, not a possibly
    # noexec /tmp mount. Temporary files are removed after the test.
    with tempfile.TemporaryDirectory(prefix=".sc7-popup-lifecycle-", dir=ROOT) as temp, \
            tempfile.TemporaryDirectory(prefix="sc7-pop-runtime-") as runtime_temp:
        work = Path(temp)
        binary = build_client(work)
        if args.client_only:
            if not os.environ.get("XDG_RUNTIME_DIR") or not os.environ.get("WAYLAND_DISPLAY"):
                raise RuntimeError("client-only requires XDG_RUNTIME_DIR and WAYLAND_DISPLAY")
            exercise(binary, args.cycles, os.environ.copy(), None)
            return 0

        # Sway's IPC socket embeds XDG_RUNTIME_DIR in a fixed-size sun_path.
        runtime = Path(runtime_temp)
        config = work / "sway.conf"
        config.write_text("output * resolution 800x600\nworkspace 1\nxwayland disable\n")
        log = work / "sway.log"
        environment = os.environ.copy()
        environment.update(XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(work),
                           WLR_BACKENDS="headless", WLR_RENDERER="pixman",
                           WLR_LIBINPUT_NO_DEVICES="1")
        for name in ("WAYLAND_DISPLAY", "DISPLAY", "SWAYSOCK", "I3SOCK"):
            environment.pop(name, None)
        if args.nested or args.via_launcher:
            exercise_nested(binary, args.cycles, args.sway,
                            args.wlroots_library, work, runtime, environment,
                            via_launcher=args.via_launcher)
            return 0
        if args.wlroots_library:
            lib = args.wlroots_library.resolve()
            environment["LD_LIBRARY_PATH"] = str(lib) + os.pathsep + \
                environment.get("LD_LIBRARY_PATH", "")
        before = socket_paths(runtime)
        with log.open("wb") as output:
            sway = subprocess.Popen([args.sway, "-d", "-c", str(config)],
                                    env=environment, stdout=output,
                                    stderr=subprocess.STDOUT,
                                    start_new_session=True)
            try:
                socket = wait_for_socket(runtime, before, sway, log)
                client_env = dict(environment, WAYLAND_DISPLAY=socket.name)
                exercise(binary, args.cycles, client_env, log)
                if sway.poll() is not None:
                    raise RuntimeError(f"Sway exited {sway.returncode} during popup test")
            finally:
                stop_exact(sway)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"popup lifecycle test failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
