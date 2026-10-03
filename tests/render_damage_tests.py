#!/usr/bin/env python3
"""Visual A/B test for stale pane pixels during nested Wayland output resize.

Run an isolated headless Sway host with Rack-local Sway inside it. The actual
Rack layout helper opens COSMIC System Monitor plus three distinctly colored
terminal panes. At each host-window resize, capture both the nested output and
host output. A cross-pane pixel oracle checks whether the terminal colors leak
into System Monitor. No live Rack instance or desktop socket is used. GLES2
requires a host with a DRM render node; on a CPU-only development host, run
the Pixman control with --renderer pixman.

Requires system Sway, foot, COSMIC System Monitor, Pillow, and grim. A grim
binary may be passed with --grim if it is not on PATH. Test-only LD_PRELOAD
instrumentation records wlroots swapchain buffer identity/age and damage.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import time

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
LOCAL_SWAY = ROOT / "vendor/sway/build/sway/sway"
LOCAL_WLROOTS = ROOT / "vendor/wlroots/build"
TRACE_SOURCE = ROOT / "tests/render_damage_trace.c"
PALETTE = {
    "com.system76.CosmicTerm": (188, 0, 245),  # GPU pane, top-left
    "com.system76.CosmicFiles": (0, 174, 244),  # Files stand-in, top-right
    "htop": (245, 195, 0),  # bottom-right terminal
}
SIZES = ((1366, 768), (1120, 720), (1480, 880), (1230, 760))


def run(command: list[str], *, env: dict[str, str] | None = None,
        timeout: float = 15) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, env=env, text=True, capture_output=True,
                          timeout=timeout, check=True)


def sockets(runtime: Path) -> set[Path]:
    return {path for path in runtime.glob("wayland-*") if path.is_socket()}


def wait_socket(runtime: Path, previous: set[Path],
                process: subprocess.Popen, log: Path) -> Path:
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        found = sockets(runtime) - previous
        if found:
            return sorted(found)[0]
        if process.poll() is not None:
            raise RuntimeError(f"compositor exited {process.returncode}: "
                               f"{log.read_text(errors='replace')[-4000:]}")
        time.sleep(0.05)
    raise RuntimeError(f"Wayland socket missing: "
                       f"{log.read_text(errors='replace')[-4000:]}")


def wait_ipc(runtime: Path, pid: int, process: subprocess.Popen) -> Path:
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        found = next((path for path in runtime.glob(f"sway-ipc.*.{pid}.sock")
                      if path.is_socket()), None)
        if found:
            return found
        if process.poll() is not None:
            raise RuntimeError(f"Sway exited before IPC was ready: {process.returncode}")
        time.sleep(0.05)
    raise RuntimeError(f"Sway IPC for PID {pid} missing")


def wait_ipc_ready(ipc: Path, env: dict[str, str],
                   process: subprocess.Popen) -> None:
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            sway_query(ipc, "get_version", env=env)
            return
        except (subprocess.CalledProcessError, json.JSONDecodeError):
            if process.poll() is not None:
                raise RuntimeError(f"Sway exited before IPC was ready: "
                                   f"{process.returncode}")
            time.sleep(0.05)
    raise RuntimeError(f"Sway IPC did not become ready: {ipc}")


def sway_query(ipc: Path, command: str, *, env: dict[str, str]) -> object:
    return json.loads(run(["swaymsg", "-s", str(ipc), "-t", command, "-r"],
                          env=env).stdout)


def sway_command(ipc: Path, command: str, *, env: dict[str, str]) -> None:
    response = json.loads(run(["swaymsg", "-s", str(ipc), command],
                              env=env).stdout)
    if not all(item.get("success") for item in response):
        raise RuntimeError(f"Sway rejected {command}: {response}")


def descendants(node: dict):
    yield node
    for key in ("nodes", "floating_nodes"):
        for child in node.get(key, []):
            yield from descendants(child)


def find_host_window(host_ipc: Path, nested_pid: int,
                     env: dict[str, str]) -> dict:
    tree = sway_query(host_ipc, "get_tree", env=env)
    matches = [node for node in descendants(tree)
               if node.get("type") in ("con", "floating_con")
               and node.get("pid") == nested_pid]
    if len(matches) != 1:
        raise RuntimeError(f"expected one nested Sway window, found {len(matches)}")
    return matches[0]


def find_panes(nested_ipc: Path, env: dict[str, str]) -> dict[str, dict]:
    tree = sway_query(nested_ipc, "get_tree", env=env)
    leaves = [node for node in descendants(tree)
              if node.get("type") == "con" and node.get("app_id")]
    monitor = [node for node in leaves
               if node["app_id"] == "com.system76.CosmicMonitor"]
    gpu = [node for node in leaves
           if node["app_id"] == "com.system76.CosmicTerm"
           and node["rect"]["y"] < 400]
    bottom_term = [node for node in leaves
                   if node["app_id"] == "htop"]
    files = [node for node in leaves
             if node["app_id"] == "com.system76.CosmicFiles"]
    if not all(len(group) == 1 for group in (monitor, gpu, bottom_term, files)):
        found = [(node["app_id"], node["rect"]) for node in leaves]
        raise RuntimeError(f"expected four Rack panes, found {found}")
    return {"monitor": monitor[0]["rect"], "gpu": gpu[0]["rect"],
            "files": files[0]["rect"], "htop": bottom_term[0]["rect"]}


def build_trace(work: Path) -> Path:
    trace = work / "render-damage-trace.so"
    cflags = shlex.split(run(["pkg-config", "--cflags", "pixman-1",
                              "wayland-server", "libdrm"]).stdout)
    run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
         "-fPIC", "-shared", "-DWLR_USE_UNSTABLE",
         "-I", str(ROOT / "vendor/wlroots/include"),
         "-I", str(ROOT / "vendor/wlroots/build/include"),
         *cflags, str(TRACE_SOURCE), "-o", str(trace), "-ldl"])
    return trace


def make_wrappers(work: Path) -> tuple[Path, Path]:
    fake = work / "fake-bin"
    private = work / "app-bin/rack-private"
    fake.mkdir(parents=True)
    private.mkdir(parents=True)
    def executable(path: Path, body: str) -> None:
        path.write_text("#!/usr/bin/env bash\nset -e\n" + body)
        path.chmod(0o755)
    executable(fake / "cosmic-term", """
if [[ "${*: -1}" == 'exec htop' ]]; then
    exec foot --app-id=htop -o colors.background=f5c300 -o colors.foreground=1b1b1b \
        -e bash -lc 'printf "HTOP STATIC PANE\\n"; exec sleep 180'
fi
exec foot --app-id=com.system76.CosmicTerm -o colors.background=bc00f5 \
    -o colors.foreground=ffffff -e bash -lc \
    'printf "GPU FALLBACK TERMINAL\\n"; exec sleep 180'
""")
    executable(fake / "cosmic-monitor",
               'exec /usr/bin/cosmic-monitor >> "$SC7_MONITOR_LOG" 2>&1\n')
    executable(private.parent / "sc7-rack-files", """
exec foot --app-id=com.system76.CosmicFiles -o colors.background=00aef4 \
    -o colors.foreground=111111 -e bash -lc \
    'printf "FILES STATIC PANE\\n"; exec sleep 180'
""")
    return fake, private


def stop_exact(process: subprocess.Popen | None) -> None:
    if process is None:
        return
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def shot(grim: Path, path: Path, env: dict[str, str]) -> Image.Image:
    run([str(grim), str(path)], env=env, timeout=15)
    return Image.open(path).convert("RGB")


def inner(rect: dict, image: Image.Image, *, offset=(0, 0),
          margin=25) -> Image.Image:
    x = rect["x"] + offset[0]
    y = rect["y"] + offset[1]
    width, height = rect["width"], rect["height"]
    box = (x + margin, y + margin,
           x + width - margin, y + height - margin)
    if box[2] <= box[0] or box[3] <= box[1]:
        raise RuntimeError(f"pane too small for visual sampling: {rect}")
    if box[0] < 0 or box[1] < 0 or box[2] > image.width or box[3] > image.height:
        raise RuntimeError(f"pane outside screenshot: {box}, image={image.size}")
    return image.crop(box)


def count_color(image: Image.Image, color: tuple[int, int, int],
                tolerance: int = 18) -> int:
    pixels = (image.get_flattened_data() if hasattr(image, "get_flattened_data")
              else image.getdata())
    return sum(all(abs(pixel[channel] - color[channel]) <= tolerance
                   for channel in range(3))
               for pixel in pixels)


def check_frame(image: Image.Image, panes: dict[str, dict], *,
                offset=(0, 0)) -> dict[str, float]:
    source_colors = {"gpu": PALETTE["com.system76.CosmicTerm"],
                     "files": PALETTE["com.system76.CosmicFiles"],
                     "htop": PALETTE["htop"]}
    result = {}
    for name, color in source_colors.items():
        pane = inner(panes[name], image, offset=offset)
        ratio = count_color(pane, color) / (pane.width * pane.height)
        result[f"{name}_own_color"] = round(ratio, 5)
        if ratio < 0.35:
            raise AssertionError(f"{name} sentinel color missing: {ratio:.2%}")
    monitor = inner(panes["monitor"], image, offset=offset)
    for name, color in source_colors.items():
        leaked = count_color(monitor, color)
        result[f"{name}_pixels_in_monitor"] = leaked
        if leaked > 32:
            raise AssertionError(f"{leaked} pixels from {name} leaked into Monitor")
    return result


def check_detector() -> None:
    """A copied terminal rectangle must fail the same pixel oracle."""
    image = Image.new("RGB", (400, 400), (20, 20, 20))
    from PIL import ImageDraw
    draw = ImageDraw.Draw(image)
    panes = {
        "gpu": {"x": 0, "y": 0, "width": 200, "height": 200},
        "files": {"x": 200, "y": 0, "width": 200, "height": 200},
        "monitor": {"x": 0, "y": 200, "width": 200, "height": 200},
        "htop": {"x": 200, "y": 200, "width": 200, "height": 200},
    }
    for name, color in (("gpu", PALETTE["com.system76.CosmicTerm"]),
                        ("files", PALETTE["com.system76.CosmicFiles"]),
                        ("htop", PALETTE["htop"])):
        rect = panes[name]
        draw.rectangle((rect["x"], rect["y"],
                        rect["x"] + rect["width"] - 1,
                        rect["y"] + rect["height"] - 1), fill=color)
    check_frame(image, panes)
    draw.rectangle((60, 260, 95, 295),
                   fill=PALETTE["com.system76.CosmicTerm"])
    try:
        check_frame(image, panes)
    except AssertionError as error:
        if "leaked into Monitor" not in str(error):
            raise
    else:
        raise AssertionError("visual oracle failed to detect injected cross-pane pixels")


def dimensions(ipc: Path, env: dict[str, str]) -> tuple[int, int]:
    outputs = sway_query(ipc, "get_outputs", env=env)
    active = [output for output in outputs if output.get("active")]
    if len(active) != 1:
        raise RuntimeError(f"expected one active nested output: {outputs}")
    rect = active[0]["rect"]
    return rect["width"], rect["height"]


def wait_output(ipc: Path, env: dict[str, str],
                process: subprocess.Popen) -> None:
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            dimensions(ipc, env)
            return
        except (RuntimeError, subprocess.CalledProcessError,
                json.JSONDecodeError):
            if process.poll() is not None:
                raise RuntimeError(f"Sway exited before output was ready: "
                                   f"{process.returncode}")
            time.sleep(0.05)
    raise RuntimeError(f"Sway output did not become ready: {ipc}")


def summarize_trace(path: Path) -> dict:
    lines = path.read_text(errors="replace").splitlines() if path.exists() else []
    acquire_lines = [line for line in lines if " acquire " in line]
    ages = [int(match.group(1)) for line in lines
            if (match := re.search(r" acquire .* age=(-?\d+)\b", line))
            and int(match.group(1)) >= 0]
    sizes = sorted(set(re.findall(r"size=(\d+x\d+)",
                                  "\n".join(acquire_lines))))
    buffers = set(re.findall(r" buffer=(0x[0-9a-f]+)", "\n".join(lines)))
    buffer_kinds = sorted(set(re.findall(r" acquire .* kind=(\w+)",
                                         "\n".join(lines))))
    modifiers = sorted(set(re.findall(r" acquire .* modifier=(0x[0-9a-f]+)",
                                      "\n".join(lines))))
    return {"acquires": len(ages), "buffer_ages": sorted(set(ages)),
            "buffer_identities": len(buffers), "sizes": sizes,
            "buffer_kinds": buffer_kinds, "modifiers": modifiers,
            "damage_queries": sum(" damage " in line for line in lines),
            "surface_damage_queries": sum(" surface-damage " in line
                                          for line in lines),
            "output_commits": sum(" commit " in line and "ok=1" in line
                                  for line in lines),
            "frame_commits": sum(" commit " in line and "buffer=0x" in line
                                 and "ok=1" in line for line in lines),
            "submissions": sum(" submit " in line for line in lines)}


def exercise(renderer: str, args: argparse.Namespace, work: Path,
             runtime: Path, grim: Path, tracer: Path) -> dict:
    work.mkdir(parents=True, exist_ok=True)
    out = args.artifacts / renderer
    out.mkdir(parents=True, exist_ok=True)
    host_config = work / "host.conf"
    host_config.write_text(
        "output * resolution 1800x1100\nworkspace 1\nxwayland disable\n"
        "default_border none\n"
        'for_window [app_id="dev.sc7labs.rack"] floating enable, border none\n')
    nested_config = work / "rack.conf"
    nested_config.write_text("\n".join(
        line for line in (ROOT / "config/config").read_text().splitlines()
        if not line.startswith("exec --no-startup-id")
    ) + "\n")
    fake, private = make_wrappers(work)
    settings = work / "config/sc7-rack/settings.json"
    settings.parent.mkdir(parents=True)
    settings.write_text(json.dumps({
        "share_clipboard": False, "files_path": str(work),
        "gpu_provider": "custom", "gpu_custom_command": "printf 'GPU fallback\\n'",
    }))
    host_env = os.environ.copy()
    host_env.update(XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(work),
                    WLR_BACKENDS="headless",
                    WLR_RENDERER=args.host_renderer,
                    WLR_LIBINPUT_NO_DEVICES="1",
                    LD_LIBRARY_PATH=str(LOCAL_WLROOTS.resolve()))
    for key in ("WAYLAND_DISPLAY", "SWAYSOCK", "I3SOCK", "DISPLAY",
                "LD_PRELOAD", "WLR_EGL_NO_MODIFIERS"):
        host_env.pop(key, None)
    host_log = out / "host.log"
    nested_log = out / "nested.log"
    inner_log = out / "inner.log"
    monitor_log = out / "monitor.log"
    trace = out / "render-trace.log"
    host = nested = inner_process = None
    logs = []
    frames = []
    try:
        before = sockets(runtime)
        logs.append(host_log.open("w"))
        host = subprocess.Popen([str(args.host_sway), "-c", str(host_config)],
                                env=host_env, stdout=logs[-1],
                                stderr=subprocess.STDOUT, start_new_session=True)
        host_socket = wait_socket(runtime, before, host, host_log)
        host_ipc = wait_ipc(runtime, host.pid, host)
        wait_ipc_ready(host_ipc, host_env, host)
        wait_output(host_ipc, host_env, host)
        nested_env = dict(host_env)
        nested_env.pop("WLR_EGL_NO_MODIFIERS", None)
        nested_env.update(WAYLAND_DISPLAY=host_socket.name,
                          WLR_BACKENDS="wayland",
                          WLR_RENDERER="gles2" if renderer == "gles2-no-modifiers"
                          else renderer,
                          LD_LIBRARY_PATH=str(LOCAL_WLROOTS.resolve()),
                          LD_PRELOAD=str(tracer), SC7_RENDER_TRACE=str(trace))
        if renderer == "gles2-no-modifiers":
            nested_env["WLR_EGL_NO_MODIFIERS"] = "1"
        before = sockets(runtime)
        logs.append(nested_log.open("w"))
        nested = subprocess.Popen([str(LOCAL_SWAY), "-d", "-c", str(nested_config)],
                                  env=nested_env, stdout=logs[-1],
                                  stderr=subprocess.STDOUT, start_new_session=True)
        nested_socket = wait_socket(runtime, before, nested, nested_log)
        nested_ipc = wait_ipc(runtime, nested.pid, nested)
        wait_ipc_ready(nested_ipc, nested_env, nested)
        wait_output(nested_ipc, nested_env, nested)
        client_env = dict(nested_env)
        client_env.pop("LD_PRELOAD")
        client_env.update(WAYLAND_DISPLAY=nested_socket.name,
                          SWAYSOCK=str(nested_ipc),
                          XDG_CONFIG_HOME=str(work / "config"),
                          XDG_BIN_HOME=str(fake),
                          SC7_RACK_PRIVATE_BIN=str(private),
                          SC7_MONITOR_LOG=str(monitor_log))
        logs.append(inner_log.open("w"))
        inner_process = subprocess.Popen(["bash", str(ROOT / "config/inner.sh")],
                                         env=client_env, stdout=logs[-1],
                                         stderr=subprocess.STDOUT,
                                         start_new_session=True)
        inner_process.wait(timeout=28)
        if inner_process.returncode != 0:
            raise RuntimeError(f"Rack layout failed: {inner_log.read_text()[-3000:]}")
        host_node = find_host_window(host_ipc, nested.pid, host_env)
        sway_command(host_ipc,
                     f"[con_id={host_node['id']}] floating enable",
                     env=host_env)
        sway_command(host_ipc,
                     f"[con_id={host_node['id']}] move absolute position 40 px 40 px",
                     env=host_env)
        time.sleep(1.5)
        for cycle in range(args.cycles):
            width, height = SIZES[cycle % len(SIZES)]
            sway_command(host_ipc,
                         f"[con_id={host_node['id']}] resize set width "
                         f"{width} px height {height} px", env=host_env)
            # Sway resizes floating windows around their center, which can
            # move a larger test frame partly off the capture output.
            sway_command(host_ipc,
                         f"[con_id={host_node['id']}] move absolute position "
                         "40 px 40 px", env=host_env)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                actual = dimensions(nested_ipc, client_env)
                if abs(actual[0] - width) <= 4 and abs(actual[1] - height) <= 4:
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError(f"nested output did not resize to {width}x{height}: "
                                   f"actual={actual}")
            time.sleep(args.settle)
            panes = find_panes(nested_ipc, client_env)
            host_node = find_host_window(host_ipc, nested.pid, host_env)
            rect = host_node["rect"]
            nested_png = out / f"{cycle:03d}-nested.png"
            host_png = out / f"{cycle:03d}-host.png"
            nested_image = shot(grim, nested_png, client_env)
            host_capture_env = dict(host_env, WAYLAND_DISPLAY=host_socket.name)
            host_image = shot(grim, host_png, host_capture_env)
            nested_result = check_frame(nested_image, panes)
            host_result = check_frame(
                host_image, panes, offset=(rect["x"], rect["y"]))
            frame = {
                "cycle": cycle, "requested": [width, height],
                "nested_dimensions": list(actual), "host_window": rect,
                "panes": panes, "nested": nested_result, "host": host_result,
            }
            frames.append(frame)
            print(f"{renderer} {cycle + 1}/{args.cycles}: "
                  f"{actual[0]}x{actual[1]}, "
                  f"host/nested GPU leakage "
                  f"{host_result['gpu_pixels_in_monitor']}/"
                  f"{nested_result['gpu_pixels_in_monitor']}", flush=True)
            if nested.poll() is not None or host.poll() is not None:
                raise RuntimeError("a compositor exited during resize")
        telemetry = summarize_trace(trace)
        compositor_log = nested_log.read_text(errors="replace")
        telemetry["renderer_path"] = (
            "gles2" if "Creating GLES2 renderer" in compositor_log else
            "pixman" if "Creating pixman renderer" in compositor_log else
            "unknown")
        telemetry["import_failures"] = len(re.findall(
            r"Failed to (?:create EGL image from DMA-BUF|upload buffer|"
            r"create texture|create FBO|allocate buffer)", compositor_log))
        expected_kind = "shm" if renderer == "pixman" else "dmabuf"
        expected_renderer = "pixman" if renderer == "pixman" else "gles2"
        if (expected_kind not in telemetry["buffer_kinds"] or
                telemetry["renderer_path"] != expected_renderer or
                not telemetry["damage_queries"] or
                not telemetry["frame_commits"] or
                telemetry["import_failures"]):
            raise AssertionError(f"missing {expected_kind} buffer or damage telemetry: "
                                 f"{telemetry}")
        if renderer == "gles2-no-modifiers" and any(
                value not in ("0x0000000000000000", "0xffffffffffffffff")
                for value in telemetry["modifiers"]):
            raise AssertionError(f"explicit output modifier remained active: "
                                 f"{telemetry['modifiers']}")
        result = {
            "renderer": renderer, "host_renderer": args.host_renderer,
            "frames": frames, "trace": telemetry,
        }
        (out / "report.json").write_text(json.dumps(result, indent=2) + "\n")
        return result
    finally:
        stop_exact(inner_process)
        stop_exact(nested)
        stop_exact(host)
        for log in logs:
            log.close()
        if not (out / "report.json").exists():
            (out / "failure.json").write_text(
                json.dumps({"frames_completed": frames,
                            "trace": summarize_trace(trace)}, indent=2) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer",
                        choices=("all", "both", "gles2", "gles2-no-modifiers",
                                 "pixman"),
                        default="both")
    parser.add_argument("--host-renderer", choices=("auto", "gles2", "pixman"),
                        default="auto")
    parser.add_argument("--host-sway", type=Path, default=LOCAL_SWAY,
                        help="Sway binary for the private host compositor")
    parser.add_argument("--cycles", type=int, default=12)
    parser.add_argument("--settle", type=float, default=0.35)
    parser.add_argument("--grim", type=Path,
                        default=Path(shutil.which("grim") or "/nonexistent/grim"))
    parser.add_argument("--artifacts", type=Path,
                        default=Path("/tmp/sc7-render-damage-results"))
    args = parser.parse_args()
    if args.host_renderer == "auto":
        args.host_renderer = ("pixman" if args.renderer == "pixman"
                              else "gles2")
    if args.host_renderer == "gles2" and not list(Path("/dev/dri").glob("renderD*")):
        parser.error("GLES2 test needs an accessible DRM render node; "
                     "use --renderer pixman for the CPU-only control")
    if not 1 <= args.cycles <= 100:
        parser.error("--cycles must be 1..100")
    for command in ("cc", "pkg-config", "swaymsg", "foot",
                    "cosmic-monitor"):
        if not shutil.which(command):
            parser.error(f"{command} is required")
    if not args.host_sway.is_file() or not LOCAL_SWAY.is_file():
        parser.error("host and Rack-local Sway builds are required")
    if not args.grim.is_file():
        parser.error("grim is required (install grim or pass --grim PATH)")
    check_detector()
    print("cross-pane detector negative control passed", flush=True)
    run([str(ROOT / "scripts/bootstrap-sway.sh"), "--check"], timeout=40)
    args.artifacts.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".sc7-render-damage-", dir=ROOT) as tmp, \
            tempfile.TemporaryDirectory(prefix="sc7-render-runtime-") as rt, \
            tempfile.TemporaryDirectory(prefix="sc7-render-trace-link-") as link_dir:
        work, runtime = Path(tmp), Path(rt)
        grim = work / "grim"
        shutil.copyfile(args.grim, grim)
        grim.chmod(0o755)
        tracer_target = build_trace(work)
        # glibc splits LD_PRELOAD on spaces. The repository path may contain
        # spaces, so use a short symlink to the executable workspace file.
        tracer = Path(link_dir) / "trace.so"
        tracer.symlink_to(tracer_target)
        choices = (("gles2", "gles2-no-modifiers", "pixman")
                   if args.renderer == "all" else
                   ("gles2", "pixman") if args.renderer == "both" else
                   (args.renderer,))
        results = [exercise(renderer, args, work / renderer, runtime, grim, tracer)
                   for renderer in choices]
    summary = {item["renderer"]: {
        "frames": len(item["frames"]), "trace": item["trace"]}
        for item in results}
    (args.artifacts / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    print(f"artifacts: {args.artifacts}")


if __name__ == "__main__":
    main()
