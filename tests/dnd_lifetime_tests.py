#!/usr/bin/env python3
"""Run repeated DnD lifecycle callbacks against patched and legacy wlroots.

This compiles the real backend callback source in a temporary directory and
links it with the local wlroots build. The vendor checkout is never changed.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


REPO = Path(__file__).resolve().parents[1]
VENDOR = REPO / "vendor" / "wlroots"
INCREMENTAL = REPO / "patches" / "wlroots-dnd-lifetime-fix.patch"
POINTER_FIX = REPO / "patches" / "wlroots-pointer-release-fix.patch"
CURSOR_FIX = REPO / "patches" / "wlroots-cursor-serial-fix.patch"
FULL = REPO / "patches" / "wlroots-sc7labs-rack.patch"
BASE = (REPO / "patches" / "WLROOTS_BASE_REVISION").read_text().strip()


def run(*args: str, cwd: Path | None = None, input: bytes | None = None) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(args, cwd=cwd, input=input, capture_output=True, check=True)


def patch_id(patch: bytes) -> str:
    return run("git", "patch-id", "--stable", input=patch).stdout.decode().split()[0]


def prepared_legacy_source(root: Path) -> Path:
    source = root / "legacy"
    for name in ("backend/wayland/dnd.c", "include/backend/wayland.h"):
        target = source / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(VENDOR / name, target)
    environment = os.environ.copy()
    environment["GIT_CEILING_DIRECTORIES"] = str(REPO)
    # The old DnD patch predates the pointer and cursor fixes. Reverse the
    # migrations in order on disposable copies, without changing the checkout.
    for patch in (CURSOR_FIX, POINTER_FIX, INCREMENTAL):
        subprocess.run([
            "git", "apply", "-R",
            "--include=backend/wayland/dnd.c",
            "--include=include/backend/wayland.h", str(patch),
        ], cwd=source, env=environment, check=True, capture_output=True)
    return source


def compile_flags(entry: dict, source: Path) -> list[str]:
    args = entry.get("arguments") or shlex.split(entry["command"])
    if Path(args[0]).name == "ccache":
        args = args[1:]
    flags: list[str] = []
    index = 0
    while index < len(args):
        argument = args[index]
        if argument in ("-o", "-MQ", "-MF"):
            index += 2
            continue
        if argument in ("-c", "-MD") or argument.endswith("backend/wayland/dnd.c"):
            index += 1
            continue
        flags.append(argument)
        index += 1
    flags.insert(1, f"-I{source / 'include'}")
    dnd_source = source / "backend/wayland/dnd.c"
    flags.append(f'-DSC7_DND_SOURCE_PATH="{dnd_source}"')
    return flags


def compile_and_run(source: Path, harness: str, work: Path, entry: dict) -> str:
    build = VENDOR / "build"
    executable = work / Path(harness).stem
    flags = compile_flags(entry, source)
    command = flags + [
        str(REPO / "tests" / harness),
        f"-L{build}",
        f"-Wl,-rpath,{build}",
        "-lwlroots", "-lwayland-client", "-lwayland-server", "-lpixman-1", "-ldrm", "-pthread",
        "-o", str(executable),
    ]
    result = subprocess.run(command, cwd=entry["directory"], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{harness} compilation failed:\n{result.stderr[-6000:]}")
    environment = os.environ.copy()
    environment["LD_LIBRARY_PATH"] = str(build) + os.pathsep + environment.get("LD_LIBRARY_PATH", "")
    result = subprocess.run([str(executable)], capture_output=True, text=True,
                            timeout=30, env=environment)
    if result.returncode:
        raise RuntimeError(f"{harness} failed ({result.returncode}):\n"
                           f"{result.stdout[-3000:]}\n{result.stderr[-6000:]}")
    return result.stdout.strip()


def main() -> int:
    build = VENDOR / "build"
    database = build / "compile_commands.json"
    if not database.is_file() or not (build / "libwlroots.so.12").is_file():
        print("Build the local patched wlroots library before DnD lifetime tests.", file=sys.stderr)
        return 2
    run("git", "-C", str(VENDOR), "diff", "--quiet")
    run("git", "-C", str(VENDOR), "diff", "--cached", "--quiet")
    current = patch_id(run("git", "-C", str(VENDOR), "diff", "--binary", BASE, "HEAD").stdout)
    updated = patch_id(FULL.read_bytes())
    if current != updated:
        print(f"Unexpected local wlroots source patch ID: {current}", file=sys.stderr)
        return 2
    stamp = build / ".sc7-patch-id"
    if not stamp.is_file() or stamp.read_text().strip() != updated:
        print("Local wlroots source is updated but its library has not been verified for this patch; run ./scripts/bootstrap-wlroots.sh first.",
              file=sys.stderr)
        return 2
    entries = json.loads(database.read_text())
    entry = next(e for e in entries if e["file"].endswith("backend/wayland/dnd.c"))
    # Exercise both current callbacks and the historic leak in disposable
    # source copies.
    with tempfile.TemporaryDirectory(prefix="sc7-dnd-lifetime-", dir=REPO) as temporary:
        work = Path(temporary)
        old_source = prepared_legacy_source(work)
        print(compile_and_run(VENDOR, "dnd_lifetime_harness.c", work, entry))
        print(compile_and_run(old_source, "dnd_lifetime_negative_control.c", work, entry))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1) from exc
