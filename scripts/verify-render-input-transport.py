#!/usr/bin/env python3
"""Verify the missing GLES2/SHM axis using real Monitor attachment telemetry."""
import argparse
import json
from pathlib import Path
import re
import sys


def monitor_pids(tree):
    pending = [tree]
    result = set()
    while pending:
        node = pending.pop()
        if node.get("app_id") == "com.system76.CosmicMonitor" and node.get("pid"):
            result.add(int(node["pid"]))
        pending.extend(node.get("nodes", []))
        pending.extend(node.get("floating_nodes", []))
    return result


def verify(directory, frame):
    runtime = json.loads((directory / "runtime.json").read_text())
    environment = runtime.get("environment", {})
    if environment.get("SC7_RENDER_EXPERIMENT") != "shm-input":
        raise ValueError("live diagnostic mode is not shm-input")
    if environment.get("WLR_RENDERER") != "gles2":
        raise ValueError("live diagnostic does not request GLES2")
    before = monitor_pids(json.loads((directory / "tree-before-capture.json").read_text()))
    after = monitor_pids(json.loads((directory / "tree-at-capture.json").read_text()))
    if len(before) != 1 or before != after:
        raise ValueError("one stable Monitor PID was not present; Monitor may not have fallen back")
    pid = next(iter(before))
    lines = []
    for suffix in (".previous", ""):
        path = directory / ("render-trace.log" + suffix)
        if path.is_file():
            lines.extend(path.read_text(errors="replace").splitlines())
    renderer = [line for line in lines if " render-begin " in line and
                re.search(rf"\bframe={frame}\b", line)]
    if not renderer or not all(" renderer=gles2 " in line for line in renderer):
        raise ValueError("captured frame has no verified GLES2 render pass")
    attachments = [line for line in lines if " wl-buffer-attach " in line and
                   re.search(rf"\bpid={pid}\b", line)]
    kinds = sorted({match[1] for line in attachments
                    if (match := re.search(r"\bkind=(\S+)", line))})
    if kinds != ["shm"]:
        raise ValueError(f"Monitor input transport is not verified SHM: {kinds or 'no attachments'}")
    samples = [line for line in lines if " input-capture " in line and
               re.search(rf"\bframe={frame}\b", line) and re.search(rf"\bpid={pid}\b", line)]
    sampled_kinds = {match[1] if (match := re.search(r"\bkind=(\S+)", line)) else "missing"
                     for line in samples}
    if sampled_kinds - {"owned-upload", "shm"}:
        raise ValueError(f"Monitor's sampled input conflicts with SHM attachments: {sorted(sampled_kinds)}")
    if not samples or not any(" gles_status=1 " in line for line in samples):
        raise ValueError("Monitor's isolated GLES input was not captured in this frame")
    return {"verified": True, "frame": frame, "monitor_pid": pid,
            "renderer": "gles2", "monitor_input_kind": "shm",
            "attachment_evidence": attachments[-8:], "render_evidence": renderer,
            "sample_evidence": samples}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact_directory", type=Path)
    parser.add_argument("frame", type=int)
    args = parser.parse_args()
    destination = args.artifact_directory / f"shm-input-frame-{args.frame:012d}.json"
    try:
        result = verify(args.artifact_directory, args.frame)
    except (OSError, ValueError, TypeError, KeyError) as error:
        result = {"verified": False, "frame": args.frame, "reason": str(error)}
    destination.write_text(json.dumps(result, indent=2) + "\n")
    if not result["verified"]:
        print(f"shm-input UNVERIFIED: {result['reason']}. Keep the artifacts; "
              "do not interpret this run as GLES2 + SHM.", file=sys.stderr)
        return 3
    print(f"shm-input verified: Monitor pid={result['monitor_pid']} kind=shm; "
          f"frame={args.frame} renderer=gles2. Evidence: {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
