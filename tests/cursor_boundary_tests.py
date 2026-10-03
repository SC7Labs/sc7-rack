#!/usr/bin/env python3
"""Regress the Rack Wayland cursor crash during a host DnD boundary crossing.

The C client includes the actual wlroots pointer, DnD, and output callback
sources. Only host Wayland requests are intercepted; no desktop input is used.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
WLROOTS = ROOT / "vendor/wlroots"
BUILD = WLROOTS / "build"
CLIENT = ROOT / "tests/cursor_boundary_client.c"
FIX_PATCH = ROOT / "patches/wlroots-cursor-serial-fix.patch"


def pkg_config(*args: str) -> list[str]:
    result = subprocess.run(["pkg-config", *args], check=True,
                            capture_output=True, text=True)
    return shlex.split(result.stdout)


def build_client(work: Path, previous_dnd: Path | None = None) -> Path:
    binary = work / ("previous-cursor-boundary" if previous_dnd else
                     "cursor-boundary")
    flags = ([f'-DDND_BACKEND_SOURCE="{previous_dnd.resolve()}"']
             if previous_dnd else [])
    command = [
        "cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-ffunction-sections", "-fdata-sections",
        "-DWLR_USE_UNSTABLE", *flags,
        *(f"-I{path}" for path in (
            WLROOTS, WLROOTS / "include", BUILD, BUILD / "include",
            BUILD / "protocol")),
        *pkg_config("--cflags", "wayland-client", "wayland-server",
                    "pixman-1", "libdrm"),
        str(CLIENT), f"-L{BUILD}", f"-Wl,-rpath,{BUILD}",
        "-Wl,--gc-sections", "-lwlroots",
        *pkg_config("--libs", "wayland-client", "wayland-server",
                    "pixman-1", "libdrm"),
        "-o", str(binary),
    ]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"C harness compilation failed:\n{result.stderr[-6000:]}")
    return binary


def reconstruct_previous_dnd(work: Path) -> Path:
    """Reverse only the authenticated DnD cursor fix in a temp copy."""
    source = work / "backend/wayland/dnd.c"
    source.parent.mkdir(parents=True)
    source.write_bytes((WLROOTS / "backend/wayland/dnd.c").read_bytes())
    environment = os.environ.copy()
    environment["GIT_CEILING_DIRECTORIES"] = str(ROOT)
    result = subprocess.run(
        ["git", "apply", "--reverse", "--include=backend/wayland/dnd.c",
         str(FIX_PATCH)], cwd=work, env=environment,
        capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError("could not reconstruct the previous DnD source:\n"
                           f"{result.stderr[-4000:]}")
    return source


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cycles", type=int, default=128)
    parser.add_argument("--negative-control", action="store_true",
                        help="prove the previous DnD callback hits the "
                             "wlroots output cursor assertion")
    args = parser.parse_args()
    if not 100 <= args.cycles <= 10000:
        parser.error("--cycles must be 100..10000")
    if not (BUILD / "libwlroots.so").exists():
        parser.error("build Rack-local wlroots first: ./scripts/bootstrap-wlroots.sh")
    if args.negative_control and not FIX_PATCH.is_file():
        parser.error(f"missing incremental cursor fix patch: {FIX_PATCH}")

    # /tmp can be noexec; keep compiled binaries in a disposable repo folder.
    with tempfile.TemporaryDirectory(prefix=".sc7-cursor-boundary-", dir=ROOT) as temp:
        work = Path(temp)
        binary = build_client(work)
        result = subprocess.run([str(binary), str(args.cycles)],
                                capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError(f"cursor boundary regression failed "
                               f"({result.returncode}):\n"
                               f"{result.stdout[-2000:]}\n"
                               f"{result.stderr[-4000:]}")
        print(result.stdout.strip())

        if args.negative_control:
            old_source = reconstruct_previous_dnd(work)
            old_binary = build_client(work, old_source)
            old = subprocess.run([str(old_binary), "100"],
                                 capture_output=True, text=True, timeout=10)
            expected = "Assertion `output->enter_serial' failed"
            if old.returncode == 0 or expected not in old.stderr:
                raise RuntimeError("previous backend did not fail at the exact "
                                   "cursor assertion:\n"
                                   f"{old.stderr[-4000:]}")
            print("PASS: previous backend reproduces the output cursor "
                  "enter_serial assertion")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
