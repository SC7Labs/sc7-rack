#!/usr/bin/env bash
set -euo pipefail

if (( $# > 1 )); then
    echo "Usage: scripts/capture-render-frame.sh [diagnostic-artifact-directory]" >&2
    exit 2
fi
runtime_dir="${XDG_RUNTIME_DIR:-/run/user/$UID}"
artifact_dir="${1:-}"
if [[ -z "$artifact_dir" ]]; then
    marker="$runtime_dir/sc7-rack/render-diagnostic-dir"
    if [[ ! -f "$marker" ]]; then
        echo "Error: no live renderer diagnostic. Start scripts/run-render-diagnostic.sh first." >&2
        exit 1
    fi
    artifact_dir="$(cat "$marker")"
fi
if [[ ! -d "$artifact_dir/captures" ]]; then
    echo "Error: diagnostic capture directory is missing: $artifact_dir" >&2
    exit 1
fi
artifact_dir="$(readlink -f -- "$artifact_dir")"
pid_file="$runtime_dir/sc7-rack/sc7-rack.pid"
if [[ ! -f "$pid_file" ]]; then
    echo "Error: Rack PID file is missing." >&2
    exit 1
fi
rack_pid="$(cat "$pid_file")"
if [[ ! "$rack_pid" =~ ^[1-9][0-9]*$ ]] || ! kill -0 "$rack_pid" 2>/dev/null; then
    echo "Error: Rack is not running." >&2
    exit 1
fi
# Confirm this directory belongs to the live diagnostic before writing any
# trees, metadata, or capture requests into a possibly older run.
runtime_metadata="$(python3 - "$rack_pid" "$artifact_dir" <<'PY'
import json, pathlib, sys, time
proc = pathlib.Path('/proc') / sys.argv[1]
directory = pathlib.Path(sys.argv[2]).resolve()
try:
    environment = {}
    for item in (proc / 'environ').read_bytes().split(b'\0'):
        key, separator, value = item.partition(b'=')
        if separator:
            environment[key.decode(errors='replace')] = value.decode(errors='replace')
    expected = {
        'SC7_RENDER_CAPTURE_DIR': directory / 'captures',
        'SC7_RENDER_CAPTURE_TRIGGER': directory / 'capture-now',
        'SC7_RENDER_TRACE': directory / 'render-trace.log',
    }
    for name, requested in expected.items():
        value = environment.get(name)
        if not value:
            raise ValueError('live Rack is not running this renderer diagnostic')
        actual = pathlib.Path(value)
        if not actual.is_absolute():
            actual = (proc / 'cwd').resolve() / actual
        if actual.resolve() != requested.resolve():
            raise ValueError('artifact directory does not belong to the live renderer diagnostic')
except (OSError, ValueError) as error:
    raise SystemExit(f'Error: {error}.')
allowed = {'WLR_RENDERER', 'WLR_BACKENDS', 'WLR_EGL_NO_MODIFIERS',
           'SC7_RENDER_EXPERIMENT', 'SC7_RENDER_CAPTURE_INPUTS', 'SC7_RACK_FULL_REPAINT', *expected}
print(json.dumps({
    'pid': int(sys.argv[1]),
    'environment': {name: value for name, value in environment.items() if name in allowed},
    'capture_request_monotonic_ns': time.monotonic_ns(),
}, indent=2))
PY
)"
ipc=""
for candidate in "$runtime_dir"/sway-ipc.*."$rack_pid".sock; do
    if [[ -S "$candidate" ]]; then ipc="$candidate"; break; fi
done
if [[ -z "$ipc" ]]; then
    echo "Error: Rack's exact IPC socket is missing." >&2
    exit 1
fi
swaymsg -s "$ipc" -t get_tree -r > "$artifact_dir/tree-before-capture.json"
swaymsg -s "$ipc" -t get_outputs -r > "$artifact_dir/outputs-before-capture.json"
printf '%s\n' "$runtime_metadata" > "$artifact_dir/runtime.json"

capture_limit_reached() {
    for trace_chunk in "$artifact_dir/render-trace.log" "$artifact_dir/render-trace.log.previous"; do
        if [[ -f "$trace_chunk" ]] && grep -q ' pre-submit .*reason=capture-limit' "$trace_chunk"; then
            return 0
        fi
    done
    return 1
}
if capture_limit_reached; then
    echo "Capture limit reached (8 per run). Restart the diagnostic for more captures." >&2
    exit 2
fi
before=()
shopt -s nullglob
before=("$artifact_dir"/captures/pre-submit-*.ppm)
: > "$artifact_dir/capture-now"
capture=""
for (( attempt=0; attempt<100; ++attempt )); do
    current=("$artifact_dir"/captures/pre-submit-*.ppm)
    for candidate in "${current[@]}"; do
        seen=0
        for previous in "${before[@]}"; do
            if [[ "$candidate" == "$previous" ]]; then seen=1; break; fi
        done
        if (( seen == 0 )); then capture="$candidate"; break; fi
    done
    if [[ -n "$capture" ]]; then break; fi
    sleep 0.05
done
if [[ -z "$capture" ]]; then
    if capture_limit_reached; then
        echo "Capture limit reached (8 per run). Restart the diagnostic for more captures." >&2
        exit 2
    fi
    echo "Capture pending. Keep Rack visible and animate or resize it, then check $artifact_dir/captures." >&2
    exit 2
fi

# Encode the captured PPM as PNG using only Python's standard library.
png="${capture%.ppm}.png"
python3 - "$capture" "$png" "$artifact_dir" <<'PY'
import json, pathlib, re, struct, sys, zlib
def read_image(path, magic, channels):
    with path.open('rb') as source:
        if source.readline() != magic + b'\n':
            raise SystemExit('Invalid diagnostic PPM header')
        width, height = map(int, source.readline().split())
        if source.readline() != b'255\n' or not (0 < width <= 4096 and 0 < height <= 4096):
            raise SystemExit('Invalid diagnostic PPM dimensions')
        pixels = source.read()
    if len(pixels) != width * height * channels:
        raise SystemExit('Incomplete diagnostic capture')
    return width, height, pixels
def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
def encode(ppm, png, use_alpha=False):
    width, height, pixels = read_image(ppm, b'P6', 3)
    alpha_path = pathlib.Path(str(ppm) + '.alpha.pgm')
    has_alpha = use_alpha and alpha_path.is_file()
    channels = 4 if has_alpha else 3
    if has_alpha:
        aw, ah, alpha = read_image(alpha_path, b'P5', 1)
        if (aw, ah) != (width, height):
            raise SystemExit('Input RGB and alpha dimensions differ')
        rgba = bytearray(width * height * 4)
        rgba[0::4], rgba[1::4], rgba[2::4], rgba[3::4] = (
            pixels[0::3], pixels[1::3], pixels[2::3], alpha)
        pixels = rgba
    rows = b''.join(b'\0' + pixels[y * width * channels:(y + 1) * width * channels]
                    for y in range(height))
    png.write_bytes(b'\x89PNG\r\n\x1a\n' +
        chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6 if has_alpha else 2, 0, 0, 0)) +
        chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
    return {'file': png.name, 'size': [width, height], 'alpha': has_alpha}
capture, png, directory = map(pathlib.Path, sys.argv[1:])
encode(capture, png)
frame_string = re.fullmatch(r'pre-submit-(\d+)\.ppm', capture.name)[1]
frame = int(frame_string)
inputs = [encode(path, path.with_suffix('.png'), True)
          for path in sorted(capture.parent.glob(f'frame-{frame_string}-*-input-*.ppm'))]
records = []
for suffix in ('.previous', ''):
    trace = directory / ('render-trace.log' + suffix)
    if trace.is_file():
        for line in trace.read_text(errors='replace').splitlines():
            if (' input-capture' in line and re.search(rf'\bframe={frame}\b', line)):
                records.append(line)
manifest = capture.with_name(f'pre-submit-{frame_string}-inputs.json')
manifest.write_text(json.dumps({'frame': frame, 'input_images': inputs,
                               'input_trace': records}, indent=2) + '\n')
if inputs or records:
    print(f'Input snapshots and exact frame metadata: {manifest}')
PY
swaymsg -s "$ipc" -t get_tree -r > "$artifact_dir/tree-at-capture.json"
printf 'Pre-submit frame: %s\n' "$png"
if command -v grim >/dev/null 2>&1 && grim "$artifact_dir/host-at-capture.png" 2> "$artifact_dir/host-capture.log"; then
    printf 'Later host display: %s\n' "$artifact_dir/host-at-capture.png"
else
    printf 'Take a desktop screenshot while the corruption remains visible; save it in %s\n' "$artifact_dir"
fi
printf 'Trace and client structure: %s\n' "$artifact_dir"
if python3 - "$artifact_dir/runtime.json" <<'PY'
import json, sys
raise SystemExit(0 if json.load(open(sys.argv[1])).get('environment', {}).get(
    'SC7_RENDER_EXPERIMENT') == 'shm-input' else 1)
PY
then
    frame_name="${capture##*/pre-submit-}"
    frame_name="${frame_name%.ppm}"
    python3 "$(dirname "$(readlink -f "$0")")/verify-render-input-transport.py" \
        "$artifact_dir" "$((10#$frame_name))"
fi
