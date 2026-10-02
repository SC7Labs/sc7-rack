#!/usr/bin/env python3
"""Run the real wlroots Wayland pointer callbacks through repeated host leaves.

The C harness includes backend/wayland/pointer.c and emits actual wlr_pointer
signals. It checks balanced right-button events after pointer leave, cursor
cleanup, motion after re-enter, and the DnD finish/cancel release helper.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests/pointer_backend_client.c"
WLROOTS = ROOT / "vendor/wlroots"
BUILD = WLROOTS / "build"
FIX_PATCH = ROOT / "patches/wlroots-pointer-release-fix.patch"


def pkg_config(*args: str) -> list[str]:
    result = subprocess.run(["pkg-config", *args], text=True,
                            capture_output=True, check=True)
    return shlex.split(result.stdout)


def build_client(work: Path, previous_source: Path | None = None) -> Path:
    binary = work / ("previous-pointer-backend" if previous_source else
                     "pointer-backend")
    flags = []
    if previous_source:
        flags += [f'-DPOINTER_BACKEND_SOURCE="{previous_source.resolve()}"',
                  "-DTEST_PREVIOUS_BACKEND"]
    command = [
        "cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-ffunction-sections", "-fdata-sections",
        "-DWLR_USE_UNSTABLE", *flags,
        *(f"-I{path}" for path in (
            WLROOTS, WLROOTS / "include", BUILD, BUILD / "include",
            BUILD / "protocol")),
        *pkg_config("--cflags", "wayland-client", "wayland-server",
                    "pixman-1", "libdrm"),
        str(SOURCE), f"-L{BUILD}", f"-Wl,-rpath,{BUILD}",
        "-Wl,--gc-sections", "-lwlroots",
        *pkg_config("--libs", "wayland-client", "wayland-server",
                    "pixman-1", "libdrm"),
        "-o", str(binary),
    ]
    subprocess.run(command, check=True, capture_output=True, text=True)
    return binary


def reconstruct_previous_source(work: Path) -> Path:
    """Reverse the authenticated incremental fix in an isolated temp tree."""
    source = work / "backend/wayland/pointer.c"
    source.parent.mkdir(parents=True)
    shutil.copy2(WLROOTS / "backend/wayland/pointer.c", source)
    subprocess.run(
        ["git", "apply", "--reverse", "--include=backend/wayland/pointer.c",
         str(FIX_PATCH)], cwd=work, check=True, capture_output=True, text=True)
    return source


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cycles", type=int, default=256)
    parser.add_argument("--negative-control", action="store_true",
                        help="reverse the tracked pointer fix in a temporary "
                             "tree and prove the old backend fails")
    args = parser.parse_args()
    if not 1 <= args.cycles <= 10000:
        parser.error("--cycles must be 1..10000")
    if not (BUILD / "libwlroots.so").exists():
        parser.error("build Rack-local wlroots first: ./scripts/bootstrap-wlroots.sh")
    if args.negative_control and not FIX_PATCH.is_file():
        parser.error(f"missing incremental fix patch: {FIX_PATCH}")

    # /tmp can be mounted noexec. Keep only compiled fixtures in the workspace.
    with tempfile.TemporaryDirectory(prefix=".sc7-pointer-backend-", dir=ROOT) as temp, \
            tempfile.TemporaryDirectory(prefix="sc7-previous-pointer-") as previous_temp:
        work = Path(temp)
        binary = build_client(work)
        result = subprocess.run([str(binary), str(args.cycles)],
                                capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError(
                f"pointer backend regression failed ({result.returncode}):\n"
                f"{result.stdout[-2000:]}\n{result.stderr[-4000:]}")
        print(result.stdout.strip())

        if args.negative_control:
            previous_source = reconstruct_previous_source(Path(previous_temp))
            old_binary = build_client(work, previous_source)
            old = subprocess.run([str(old_binary), "1"], capture_output=True,
                                 text=True, timeout=10)
            markers = ("leave retained cursor pointer",
                       "release after leave was not forwarded exactly once")
            if old.returncode == 0 or not any(
                    marker in old.stderr for marker in markers):
                raise RuntimeError(
                    "previous backend did not fail at the expected pointer "
                    f"invariant ({old.returncode}):\n{old.stderr[-4000:]}")
            print("PASS: previous backend fails the host leave/release invariant")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
