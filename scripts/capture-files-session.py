#!/usr/bin/env python3
"""Read a bounded snapshot of Rack's nested Files session; never send input."""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
SOCKET_LINK = re.compile(r"socket:\[(\d+)\]$")


def identity(proc):
    stat = (proc / "stat").read_text()
    fields = stat[stat.rfind(") ") + 2:].split()
    return int(fields[19])  # starttime, /proc stat field 22


def arguments(proc):
    return [os.fsdecode(arg) for arg in (proc / "cmdline").read_bytes().split(b"\0") if arg]


def fd_targets(proc):
    targets = {}
    for fd in (proc / "fd").iterdir():
        try:
            targets[fd.name] = os.readlink(fd)
        except OSError:
            continue
    return targets


def socket_inodes(targets):
    return {int(match.group(1)) for target in targets.values()
            if (match := SOCKET_LINK.fullmatch(target))}


def decode_peer_messages(data, sequence):
    """Decode Linux UNIX_DIAG_PEER attributes without recording other sockets."""
    peers, done = {}, False
    offset = 0
    while offset + 16 <= len(data):
        size, kind, flags, seq, _pid = struct.unpack_from("=IHHII", data, offset)
        if size < 16 or offset + size > len(data):
            raise RuntimeError("Malformed socket diagnostic reply")
        if seq != sequence:
            raise RuntimeError("Unexpected socket diagnostic reply sequence")
        if flags & 0x10:  # NLM_F_DUMP_INTR: do not accept an incomplete dump
            raise RuntimeError("Socket diagnostic snapshot was interrupted")
        if kind == 3:  # NLMSG_DONE
            done = True
        elif kind == 2:  # NLMSG_ERROR
            if size < 20:
                raise RuntimeError("Truncated socket diagnostic error")
            code = struct.unpack_from("=i", data, offset + 16)[0]
            if code:
                raise OSError(-code, "Socket diagnostic request failed")
        elif kind == 20:
            payload = data[offset + 16:offset + size]
            if len(payload) < 16:
                raise RuntimeError("Truncated socket diagnostic record")
            family, _type, _state, _pad, inode, _a, _b = struct.unpack_from("=BBBBIII", payload)
            position = 16
            while position + 4 <= len(payload):
                length, attribute = struct.unpack_from("=HH", payload, position)
                if length < 4 or position + length > len(payload):
                    raise RuntimeError("Malformed socket diagnostic attribute")
                if family == socket.AF_UNIX and attribute == 2 and length >= 8:  # UNIX_DIAG_PEER
                    peers[inode] = struct.unpack_from("=I", payload, position + 4)[0]
                position += (length + 3) & ~3
        offset += (size + 3) & ~3
    return peers, done


def server_peer_inodes(server_inodes):
    # Files can daemonize: the PID reported by Sway is the initial connector,
    # not necessarily the process which still owns that connection. Socket
    # peers provide ownership evidence without guessing by process name.
    sequence = 1976
    body = struct.pack("=BBHIIIII", socket.AF_UNIX, 0, 0, 0xffffffff,
                       0, 4, 0xffffffff, 0xffffffff)  # UDIAG_SHOW_PEER
    request = struct.pack("=IHHII", 16 + len(body), 20, 0x301, sequence, 0) + body
    peers = set()
    with socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, 4) as diagnostic:
        diagnostic.settimeout(2)
        diagnostic.send(request)
        for _ in range(128):
            mapping, done = decode_peer_messages(diagnostic.recv(1 << 20), sequence)
            peers.update(peer for inode, peer in mapping.items()
                         if inode in server_inodes and peer)
            if done:
                return peers
    raise RuntimeError("Socket diagnostic reply exceeded bounded limit")


def validate_compositor(proc, expected_binary, config):
    args = arguments(proc)
    if proc.stat().st_uid != os.getuid():
        raise RuntimeError("Rack PID belongs to another user")
    # A local rebuild unlinks the executable of an already-running Rack. Keep
    # its exact original checkout path verifiable, and report this fact below.
    executable = os.readlink(proc / "exe").removesuffix(" (deleted)")
    if Path(executable).resolve() != expected_binary.resolve():
        raise RuntimeError("Rack PID is not this checkout's local Sway")
    if not any(arg == "--config" and index + 1 < len(args)
               and Path(args[index + 1]).resolve() == config.resolve()
               for index, arg in enumerate(args)):
        raise RuntimeError("Rack PID is not using the installed Rack config")
    return identity(proc)


def views(node):
    result = []
    if node.get("app_id") or node.get("pid"):
        result.append({key: node.get(key) for key in
                       ("id", "pid", "app_id", "name", "focused", "visible", "rect", "type")})
    for key in ("nodes", "floating_nodes"):
        for child in node.get(key, []):
            result.extend(views(child))
    return result


def process_details(proc, owned_peers, expected_name):
    start = identity(proc)
    targets = fd_targets(proc)
    if ((proc / "comm").read_text().strip() != expected_name
            or proc.stat().st_uid != os.getuid()
            or not (socket_inodes(targets) & owned_peers)):
        raise RuntimeError("Nested client ownership changed during snapshot")
    status = {}
    allowed = {"Name", "State", "PPid", "Threads", "VmRSS", "VmSize", "FDSize"}
    for line in (proc / "status").read_text().splitlines():
        key, _, value = line.partition(":")
        if key in allowed:
            status[key] = value.strip()
    threads = []
    for thread in sorted((proc / "task").iterdir(), key=lambda path: int(path.name))[:256]:
        try:
            stat = (thread / "stat").read_text()
            fields = stat[stat.rfind(") ") + 2:].split()
            threads.append({"tid": int(thread.name), "state": fields[0],
                            "name": (thread / "comm").read_text().strip(),
                            "wait_channel": (thread / "wchan").read_text().strip()})
        except OSError:
            continue
    if identity(proc) != start:
        raise RuntimeError("Process identity changed during snapshot")
    categories = {"socket": 0, "pipe": 0, "inotify": 0, "eventfd": 0, "other": 0}
    for target in targets.values():
        category = next((kind for kind in categories if kind != "other" and kind in target), "other")
        categories[category] += 1
    return {"pid": int(proc.name), "start_tick": start, "status": status,
            "fd_count": len(targets), "fd_categories": categories, "threads": threads,
            "nested_peer_inodes": sorted(socket_inodes(targets) & owned_peers)}


def owned_client(proc, name, peer_inodes):
    try:
        return (proc.stat().st_uid == os.getuid()
                and (proc / "comm").read_text().strip() == name
                and bool(socket_inodes(fd_targets(proc)) & peer_inodes))
    except OSError:
        return False


def ipc(socket_path, kind):
    result = subprocess.run(["swaymsg", "-s", str(socket_path), "-t", kind, "-r"],
                            capture_output=True, timeout=4, check=True)
    if len(result.stdout) > 8 * 1024 * 1024:
        raise RuntimeError("Rack IPC response exceeded bounded limit")
    return json.loads(result.stdout)


def snapshot(runtime, config, phase):
    run = runtime / "sc7-rack"
    raw = (run / "sc7-rack.pid").read_text().strip()
    if not raw.isdecimal() or int(raw) < 1:
        raise RuntimeError("Invalid Rack PID record")
    compositor = Path("/proc") / raw
    start = validate_compositor(compositor, ROOT / "vendor/sway/build/sway/sway", config)
    sockets = [path for path in runtime.glob(f"sway-ipc.*.{raw}.sock") if path.is_socket()]
    if len(sockets) != 1:
        raise RuntimeError("Cannot identify one Rack IPC socket")
    targets = fd_targets(compositor)
    nested = [str(Path(target).with_suffix("")) for target in targets.values()
              if Path(target).parent == runtime and re.fullmatch(r"wayland-\d+\.lock", Path(target).name)]
    if len(set(nested)) != 1 or not Path(nested[0]).is_socket():
        raise RuntimeError("Cannot identify Rack's nested Wayland socket")
    result = {"phase": phase, "captured_utc": datetime.now(timezone.utc).isoformat(),
              "rack_pid": int(raw), "rack_start_tick": start, "nested_socket": nested[0],
              "local_binary_replaced": os.readlink(compositor / "exe").endswith(" (deleted)"),
              "sway_version": ipc(sockets[0], "get_version"),
              "views": views(ipc(sockets[0], "get_tree")),
              "seats": ipc(sockets[0], "get_seats"), "files": [], "bridges": [],
              "ownership_errors": [], "interpretation": "Evidence only; not a Files mutation or long-run test."}
    try:
        peers = server_peer_inodes(socket_inodes(targets))
        tracked = set((run / "inner_pids").read_text().split()) if (run / "inner_pids").exists() else set()
        for proc in Path("/proc").iterdir():
            if not proc.name.isdecimal():
                continue
            try:
                if owned_client(proc, "cosmic-files", peers):
                    result["files"].append(process_details(proc, peers, "cosmic-files"))
                elif proc.name in tracked and owned_client(proc, "sc7-clipboard-b", peers):
                    args = arguments(proc)
                    if (args and Path(args[0]).resolve() == (ROOT / "bin/sc7-clipboard-bridge").resolve()
                            and "--nested" in args and
                            args[args.index("--nested") + 1] in (nested[0], Path(nested[0]).name)):
                        result["bridges"].append(process_details(proc, peers, "sc7-clipboard-b"))
            except (OSError, IndexError, ValueError, RuntimeError) as error:
                result["ownership_errors"].append(f"PID {proc.name}: {error}")
    except (OSError, RuntimeError) as error:
        result["ownership_errors"].append(f"Socket ownership unavailable: {error}")
    # This existing bridge log records MIME/ownership metadata, never payloads.
    # Bound it, and only read it if a live owned bridge was positively identified.
    log = run / "clipboard-bridge.log"
    if result["bridges"] and log.is_file():
        with log.open("rb") as source:
            source.seek(max(0, log.stat().st_size - 65536))
            result["bridge_log_tail"] = source.read(65536).decode(errors="replace")
    if identity(compositor) != start or (run / "sc7-rack.pid").read_text().strip() != raw:
        raise RuntimeError("Rack restarted during snapshot; retry")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phase", choices=("before", "failure", "after-reopen"), required=True)
    parser.add_argument("--output", type=Path,
                        default=Path.home() / ".local/state/sc7-rack/files-diagnostics")
    args = parser.parse_args()
    runtime = Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"))
    config = Path(os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config"))) / "sc7-rack/config"
    try:
        data = snapshot(runtime, config, args.phase)
        args.output.mkdir(mode=0o700, parents=True, exist_ok=True)
        destination = args.output / f"{args.phase}-{uuid.uuid4().hex[:12]}.json"
        with open(destination, "x", opener=lambda path, flags: os.open(path, flags, 0o600)) as output:
            json.dump(data, output, indent=2)
            output.write("\n")
        print(destination)
        if not data["files"]:
            print("UNVERIFIED: no live nested Files process was proven; see ownership_errors.", file=sys.stderr)
            return 3
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Snapshot unavailable: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
