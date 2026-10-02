#!/usr/bin/env python3
"""Check that a real grabbed xdg_popup receives pointer hover and clicks.

The client uses wlroots' virtual-pointer protocol inside an isolated Sway,
then repeats a right-click/menu-selection cycle. No desktop input is touched.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

import popup_lifecycle_tests as lifecycle


ROOT = Path(__file__).resolve().parents[1]
CLIENT = ROOT / "tests/popup_input_client.c"
VIRTUAL_POINTER = ROOT / "vendor/wlroots/protocol/wlr-virtual-pointer-unstable-v1.xml"


def build_client(work: Path) -> Path:
    xdg_header = work / "xdg-shell-client-protocol.h"
    xdg_code = work / "xdg-shell-protocol.c"
    pointer_header = work / "wlr-virtual-pointer-unstable-v1-client-protocol.h"
    pointer_code = work / "wlr-virtual-pointer-unstable-v1-protocol.c"
    binary = work / "popup-input-client"
    lifecycle.run("wayland-scanner", "client-header", str(lifecycle.PROTOCOL),
                  str(xdg_header))
    lifecycle.run("wayland-scanner", "private-code", str(lifecycle.PROTOCOL),
                  str(xdg_code))
    lifecycle.run("wayland-scanner", "client-header", str(VIRTUAL_POINTER),
                  str(pointer_header))
    lifecycle.run("wayland-scanner", "private-code", str(VIRTUAL_POINTER),
                  str(pointer_code))
    cflags = shlex.split(lifecycle.run("pkg-config", "--cflags", "wayland-client",
                                      capture_output=True).stdout)
    libs = shlex.split(lifecycle.run("pkg-config", "--libs", "wayland-client",
                                    capture_output=True).stdout)
    lifecycle.run("cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                  "-I", str(work), *cflags, str(CLIENT), str(xdg_code),
                  str(pointer_code), "-o", str(binary), *libs)
    return binary


def nested_window(ipc: Path, runtime: Path, pid: int) -> dict[str, object]:
    environment = dict(os.environ, XDG_RUNTIME_DIR=str(runtime))
    tree = json.loads(lifecycle.run("swaymsg", "-s", str(ipc), "-t", "get_tree",
                                    env=environment, capture_output=True).stdout)

    def nodes(node: dict[str, object]):
        yield node
        for key in ("nodes", "floating_nodes"):
            for child in node.get(key, []):
                yield from nodes(child)

    matches = [node for node in nodes(tree)
               if node.get("type") in ("con", "floating_con") and
               node.get("pid") == pid]
    if len(matches) != 1:
        leaves = [(node.get("id"), node.get("pid"), node.get("app_id"),
                   node.get("name")) for node in nodes(tree)
                  if node.get("type") in ("con", "floating_con")]
        raise RuntimeError(f"expected one host Rack window for PID {pid}, "
                           f"found {len(matches)}; host windows: {leaves}")
    return matches[0]


def exercise_host_leave(binary: Path, cycles: int, sway_binary: str,
                        wlroots_library: Path, work: Path, runtime: Path,
                        environment: dict[str, str]) -> None:
    """Leave the nested Sway window while a host button is held."""
    if cycles < 2:
        raise RuntimeError("--host-leave needs at least two menu cycles")
    host_config = work / "host.conf"
    host_config.write_text("output * resolution 1200x900\nworkspace 1\n"
                           "xwayland disable\ndefault_border none\n")
    nested_config = work / "rack.conf"
    nested_config.write_text("\n".join(
        line for line in (ROOT / "config/config").read_text().splitlines()
        if not line.startswith("exec --no-startup-id")
    ) + "\n")
    host_log = work / "host.log"
    nested_log = work / "rack.log"
    before = lifecycle.socket_paths(runtime)
    with host_log.open("wb") as host_output:
        host = subprocess.Popen(["/usr/bin/sway", "-d", "-c", str(host_config)],
                                env=environment, stdout=host_output,
                                stderr=subprocess.STDOUT, start_new_session=True)
        try:
            host_socket = lifecycle.wait_for_socket(runtime, before, host, host_log)
            deadline = time.monotonic() + 8
            ipc_paths = []
            while time.monotonic() < deadline:
                ipc_paths = list(runtime.glob("sway-ipc.*.sock"))
                if ipc_paths:
                    break
                time.sleep(0.05)
            if len(ipc_paths) != 1:
                raise RuntimeError("host Sway IPC socket unavailable")
            ipc = ipc_paths[0]
            nested_env = dict(environment, WAYLAND_DISPLAY=host_socket.name,
                              WLR_BACKENDS="wayland",
                              LD_LIBRARY_PATH=str(wlroots_library.resolve()) +
                              os.pathsep + environment.get("LD_LIBRARY_PATH", ""))
            before_nested = lifecycle.socket_paths(runtime)
            with nested_log.open("wb") as nested_output:
                nested = subprocess.Popen([sway_binary, "-d", "-c",
                                           str(nested_config)], env=nested_env,
                                          stdout=nested_output,
                                          stderr=subprocess.STDOUT,
                                          start_new_session=True)
                try:
                    nested_socket = lifecycle.wait_for_socket(
                        runtime, before_nested, nested, nested_log)
                    deadline = time.monotonic() + 8
                    node = None
                    while time.monotonic() < deadline:
                        try:
                            node = nested_window(ipc, runtime, nested.pid)
                            break
                        except RuntimeError:
                            time.sleep(0.05)
                    if node is None:
                        raise RuntimeError("nested Rack window not mapped on host")
                    command_env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime))
                    for command in ("floating enable",
                                    "resize set width 800 px height 600 px",
                                    "move absolute position 100 px 100 px"):
                        reply = json.loads(lifecycle.run(
                            "swaymsg", "-s", str(ipc),
                            f"[con_id={node['id']}] {command}",
                            env=command_env, capture_output=True).stdout)
                        if not all(result.get("success") for result in reply):
                            raise RuntimeError(f"host Sway rejected {command}: {reply}")
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline:
                        node = nested_window(ipc, runtime, nested.pid)
                        rect = node["rect"]
                        if (rect["x"] >= 90 and rect["y"] >= 90 and
                                rect["width"] >= 700 and rect["height"] >= 500):
                            break
                        time.sleep(0.05)
                    if (rect["x"] <= 20 or rect["y"] <= 20 or
                            rect["width"] < 700 or rect["height"] < 500):
                        raise RuntimeError(f"unexpected host Rack window: {rect}")
                    client_env = dict(nested_env, WAYLAND_DISPLAY=nested_socket.name,
                                      SC7_POPUP_HOST_DISPLAY=host_socket.name,
                                      SC7_POPUP_HOST_WINDOW_X=str(rect["x"]),
                                      SC7_POPUP_HOST_WINDOW_Y=str(rect["y"]),
                                      SC7_POPUP_HOST_WINDOW_WIDTH=str(rect["width"]),
                                      SC7_POPUP_HOST_WINDOW_HEIGHT=str(rect["height"]),
                                      SC7_POPUP_HOST_OUTSIDE_X="10",
                                      SC7_POPUP_HOST_OUTSIDE_Y="10")
                    lifecycle.exercise(binary, cycles, client_env, nested_log)
                    if nested.poll() is not None or host.poll() is not None:
                        raise RuntimeError("a compositor exited during host leave test")
                finally:
                    lifecycle.stop_exact(nested)
        finally:
            lifecycle.stop_exact(host)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sway", default=str(ROOT / "vendor/sway/build/sway/sway"))
    parser.add_argument("--wlroots-library", type=Path,
                        default=ROOT / "vendor/wlroots/build")
    parser.add_argument("--cycles", type=int, default=100)
    parser.add_argument("--nested", action="store_true")
    parser.add_argument("--via-launcher", action="store_true")
    parser.add_argument("--host-leave", action="store_true",
                        help="in nested mode, leave Rack with a host button held")
    args = parser.parse_args()
    if not 1 <= args.cycles <= 1000:
        parser.error("--cycles must be 1..1000")
    if args.host_leave and (not args.nested or args.via_launcher):
        parser.error("--host-leave requires --nested and excludes --via-launcher")
    lifecycle.check_prerequisites()
    if not VIRTUAL_POINTER.is_file():
        raise RuntimeError(f"virtual pointer protocol XML missing: {VIRTUAL_POINTER}")
    if not shutil.which(args.sway):
        raise RuntimeError(f"Rack-local Sway missing: {args.sway}")
    if not args.wlroots_library.is_dir():
        raise RuntimeError(f"Rack-local wlroots missing: {args.wlroots_library}")

    with tempfile.TemporaryDirectory(prefix=".sc7-popup-input-", dir=ROOT) as temp, \
            tempfile.TemporaryDirectory(prefix="sc7-pop-input-runtime-") as runtime_temp:
        work = Path(temp)
        runtime = Path(runtime_temp)
        binary = build_client(work)
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
            if args.host_leave:
                exercise_host_leave(binary, args.cycles, args.sway,
                                    args.wlroots_library, work, runtime,
                                    environment)
            else:
                lifecycle.exercise_nested(binary, args.cycles, args.sway,
                                          args.wlroots_library, work, runtime,
                                          environment,
                                          via_launcher=args.via_launcher)
            return 0
        environment["LD_LIBRARY_PATH"] = str(args.wlroots_library.resolve()) + \
            os.pathsep + environment.get("LD_LIBRARY_PATH", "")
        before = lifecycle.socket_paths(runtime)
        with log.open("wb") as output:
            sway = subprocess.Popen([args.sway, "-d", "-c", str(config)],
                                    env=environment, stdout=output,
                                    stderr=subprocess.STDOUT, start_new_session=True)
            try:
                socket = lifecycle.wait_for_socket(runtime, before, sway, log)
                client_environment = dict(environment, WAYLAND_DISPLAY=socket.name)
                lifecycle.exercise(binary, args.cycles, client_environment, log)
                if sway.poll() is not None:
                    raise RuntimeError(f"Sway exited {sway.returncode} during input test")
            finally:
                lifecycle.stop_exact(sway)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"popup input test failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
