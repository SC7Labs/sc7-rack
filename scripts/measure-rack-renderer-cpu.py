#!/usr/bin/env python3
"""Measure CPU used by the live Rack compositor during a manual render test.

This tool only reads the Rack PID file and Linux /proc counters. Run it while
resizing Rack or reproducing the visual issue on the same machine.
"""

import argparse
import os
from pathlib import Path
import sys
import time


def rack_pid(pid_file: Path) -> int:
    try:
        value = pid_file.read_text(encoding="ascii").strip()
    except OSError as exc:
        raise RuntimeError(f"Cannot read Rack PID file {pid_file}: {exc}") from exc
    if not value.isdecimal() or int(value) < 1:
        raise RuntimeError(f"Invalid Rack PID in {pid_file}: {value!r}")
    return int(value)


def process_sample(pid: int) -> tuple[int, int, float]:
    """Return process start tick, total CPU ticks, and sample time."""
    try:
        stat = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
    except OSError as exc:
        raise RuntimeError(f"Cannot sample PID {pid}: {exc}") from exc
    # The parenthesized command name may contain spaces or ')' characters.
    end_name = stat.rfind(") ")
    if end_name < 0:
        raise RuntimeError(f"Malformed /proc/{pid}/stat")
    fields = stat[end_name + 2 :].split()
    if len(fields) < 20 or fields[0] == "Z":
        raise RuntimeError(f"PID {pid} exited or has malformed process counters")
    try:
        return int(fields[19]), int(fields[11]) + int(fields[12]), time.monotonic()
    except ValueError as exc:
        raise RuntimeError(f"Malformed /proc/{pid}/stat counters") from exc


def verify_rack_sway(pid: int) -> None:
    try:
        arguments = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
    except OSError as exc:
        raise RuntimeError(f"Cannot inspect Rack PID {pid}: {exc}") from exc
    args = [os.fsdecode(item) for item in arguments if item]
    rack_config = any(
        arg == "--config"
        and index + 1 < len(args)
        and Path(args[index + 1]).parts[-2:] == ("sc7-rack", "config")
        for index, arg in enumerate(args)
    )
    if not args or Path(args[0]).name != "sway" or not rack_config:
        raise RuntimeError(
            f"PID {pid} is not Rack's nested Sway (--config .../sc7-rack/config)"
        )


def host_compositor() -> tuple[int | None, str]:
    """Select only a unique cosmic-comp process owned by the current user."""
    matches = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            if (entry / "comm").read_text(encoding="ascii").strip() != "cosmic-comp":
                continue
            status = (entry / "status").read_text(encoding="ascii")
            uid_line = next(line for line in status.splitlines() if line.startswith("Uid:"))
            if int(uid_line.split()[1]) == os.getuid():
                matches.append(int(entry.name))
        except (OSError, StopIteration, ValueError):
            continue  # Process disappeared or cannot be inspected.
    if len(matches) == 1:
        return matches[0], ""
    return None, f"found {len(matches)} matching processes owned by this user"


def report(label: str, pid: int, before: tuple[int, int, float],
           after: tuple[int, int, float], ticks_per_second: int) -> None:
    if before[0] != after[0]:
        raise RuntimeError(f"{label} PID {pid} was replaced during the measurement")
    elapsed = after[2] - before[2]
    cpu_seconds = (after[1] - before[1]) / ticks_per_second
    if elapsed <= 0 or cpu_seconds < 0:
        raise RuntimeError(f"Invalid CPU measurement for {label} PID {pid}")
    print(f"{label} (PID {pid}): elapsed {elapsed:.2f} s, "
          f"CPU {cpu_seconds:.2f} s, one-core CPU {cpu_seconds / elapsed * 100:.1f}%")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=60, metavar="30-120",
                        help="measurement length in seconds (default: 60)")
    parser.add_argument("--host-cosmic-comp", action="store_true",
                        help="also measure host cosmic-comp if exactly one is found")
    args = parser.parse_args()
    if not 30 <= args.seconds <= 120:
        parser.error("--seconds must be between 30 and 120")

    runtime = Path(os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}")
    pid_file = runtime / "sc7-rack" / "sc7-rack.pid"
    try:
        pid = rack_pid(pid_file)
        verify_rack_sway(pid)
        rack_before = process_sample(pid)
        host_pid = None
        host_before = None
        if args.host_cosmic_comp:
            host_pid, reason = host_compositor()
            if host_pid is None:
                print(f"Host cosmic-comp skipped: {reason}", file=sys.stderr)
            else:
                try:
                    host_before = process_sample(host_pid)
                except RuntimeError as exc:
                    print(f"Host cosmic-comp skipped: {exc}", file=sys.stderr)
                    host_pid = None
        print(f"Measuring Rack Sway PID {pid} for {args.seconds} s; "
              "resize Rack or reproduce the corruption now.", flush=True)
        deadline = time.monotonic() + args.seconds
        time.sleep(max(0.0, deadline - time.monotonic()))
        rack_after = process_sample(pid)
        if rack_pid(pid_file) != pid:
            raise RuntimeError("Rack PID file changed during the measurement")
        verify_rack_sway(pid)
        ticks_per_second = os.sysconf("SC_CLK_TCK")
        report("Rack nested Sway", pid, rack_before, rack_after, ticks_per_second)
        if host_pid is not None and host_before is not None:
            try:
                host_after = process_sample(host_pid)
                report("Host cosmic-comp", host_pid, host_before, host_after,
                       ticks_per_second)
            except RuntimeError as exc:
                print(f"Host cosmic-comp unavailable: {exc}", file=sys.stderr)
    except RuntimeError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
