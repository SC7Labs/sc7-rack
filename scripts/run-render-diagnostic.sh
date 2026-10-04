#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/run-render-diagnostic.sh [--renderer gles2|pixman]
       [--mode observe|fresh-input|fresh-target|fresh-output|hold-input|shm-input]
       [--capture-inputs]

Launch Rack with a bounded renderer trace and one-shot pre-submit capture.
Run this from a terminal on the host desktop. Stop any running Rack first.
EOF
}

renderer="${WLR_RENDERER:-gles2}"
experiment=observe
capture_inputs=0
while (( $# )); do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --capture-inputs) capture_inputs=1; shift ;;
        --renderer|--mode)
            if (( $# < 2 )); then usage >&2; exit 2; fi
            if [[ "$1" == --renderer ]]; then renderer="$2"; else experiment="$2"; fi
            shift 2 ;;
        *) usage >&2; exit 2 ;;
    esac
done
case "$renderer" in gles2|pixman) ;; *) usage >&2; exit 2 ;; esac
case "$experiment" in observe|fresh-input|fresh-target|fresh-output|hold-input|shm-input) ;; *) usage >&2; exit 2 ;; esac
if [[ ( "$experiment" == fresh-target || "$experiment" == hold-input || "$experiment" == shm-input ) && "$renderer" != gles2 ]]; then
    echo "Error: $experiment requires GLES2." >&2
    exit 2
fi

project_root="$(dirname "$(dirname "$(readlink -f "$0")")")"
launcher="$project_root/bin/sc7-rack"
trace_source="$project_root/tests/render_damage_trace.c"
runtime_dir="${XDG_RUNTIME_DIR:-/run/user/$UID}"

if [[ ! -d "$runtime_dir" || ! -w "$runtime_dir" || "$runtime_dir" =~ [[:space:]] ]]; then
    echo "Error: XDG runtime directory must exist, be writable, and have no whitespace for LD_PRELOAD: $runtime_dir" >&2
    exit 1
fi
if [[ ! -f "$trace_source" || ! -x "$launcher" ||
      ! -d "$project_root/vendor/wlroots/build/include" ]]; then
    echo "Error: Rack source or its local wlroots build is missing. Run ./install.sh first." >&2
    exit 1
fi
if [[ -n "${SC7_RACK_SWAY_BINARY:-}" ]]; then
    echo "Error: diagnostic launch requires Rack's pinned local Sway; unset SC7_RACK_SWAY_BINARY." >&2
    exit 2
fi
if "$launcher" --status >/dev/null 2>&1; then
    echo "Error: Rack is already running. Stop it before starting a renderer diagnostic." >&2
    exit 1
fi
if ! command -v cc >/dev/null 2>&1 || ! command -v pkg-config >/dev/null 2>&1; then
    echo "Error: cc and pkg-config are required to build the renderer diagnostic." >&2
    exit 1
fi

# The runtime library path must not contain spaces because LD_PRELOAD splits
# entries on whitespace. Captures and logs remain in persistent user storage.
umask 077
build_dir="$(mktemp -d "$runtime_dir/sc7-rack-render-diagnostic.XXXXXXXX")"
trace_lib="$build_dir/render-damage-trace.so"
launcher_pid=""
artifact_dir=""
active_marker="$runtime_dir/sc7-rack/render-diagnostic-dir"
cleanup() {
    if [[ -n "$launcher_pid" ]] && kill -0 "$launcher_pid" 2>/dev/null; then
        kill -TERM "$launcher_pid" 2>/dev/null || true
        wait "$launcher_pid" 2>/dev/null || true
    fi
    if [[ -n "$artifact_dir" && -f "$active_marker" &&
          "$(cat "$active_marker")" == "$artifact_dir" ]]; then
        rm -f -- "$active_marker"
    fi
    rm -f -- "$trace_lib"
    rmdir -- "$build_dir" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

packages=(pixman-1 wayland-server wayland-client libdrm egl glesv2)
if ! pkg-config --exists "${packages[@]}"; then
    echo "Error: renderer diagnostic build dependencies are missing." >&2
    exit 1
fi
pkg_cflags="$(pkg-config --cflags "${packages[@]}")"
pkg_libs="$(pkg-config --libs "${packages[@]}")"
read -r -a cflags <<< "$pkg_cflags"
read -r -a libs <<< "$pkg_libs"

cc -std=gnu11 -O2 -Wall -Wextra -Werror -fPIC -shared -DWLR_USE_UNSTABLE \
    -I "$project_root/vendor/wlroots/include" \
    -I "$project_root/vendor/wlroots/build/include" \
    "${cflags[@]}" "$trace_source" \
    "$project_root/tests/render_input_experiment.c" \
    "$project_root/tests/render_output_experiment.c" \
    "$project_root/tests/render_input_capture.c" \
    "$project_root/tests/render_raw_snapshot.c" \
    "$project_root/tests/render_protocol_trace.c" \
    "$project_root/tests/render_hold_input.c" \
    "$project_root/tests/render_shm_input.c" \
    -o "$trace_lib" "${libs[@]}" -ldl

state_home="${XDG_STATE_HOME:-$HOME/.local/state}"
artifact_parent="$state_home/sc7-rack/render-diagnostics"
mkdir -p -m 0700 -- "$artifact_parent"
artifact_dir="$(mktemp -d "$artifact_parent/run-$(date +%Y%m%d-%H%M%S).XXXXXXXX")"
capture_dir="$artifact_dir/captures"
mkdir -m 0700 -- "$capture_dir"
trace_log="$artifact_dir/render-trace.log"
trigger="$artifact_dir/capture-now"
: > "$trace_log"
mkdir -p -m 0700 -- "$runtime_dir/sc7-rack"
printf '%s\n' "$artifact_dir" > "$active_marker"

# This variable is set only in the Sway invocation by bin/sc7-rack. The
# tracer removes its own preload from Sway's environment before apps launch.
unset LD_PRELOAD
export SC7_RACK_RENDER_TRACE_LIB="$trace_lib"
export SC7_RENDER_TRACE="$trace_log"
export SC7_RENDER_CAPTURE_DIR="$capture_dir"
export SC7_RENDER_CAPTURE_TRIGGER="$trigger"
export SC7_RENDER_CAPTURE_INPUTS="$capture_inputs"
export WLR_RENDERER="$renderer"
export SC7_RENDER_EXPERIMENT="$experiment"
export SC7_RACK_FULL_REPAINT=0

printf 'Renderer diagnostic artifacts: %s\n' "$artifact_dir"
printf 'Renderer: %s; experiment: %s\n' "$renderer" "$experiment"
printf 'When corruption is visible, run in another host terminal:\n  %q\n' "$project_root/scripts/capture-render-frame.sh"
printf 'Keep Rack visible and Monitor animating so the next composed frame can be captured.\n'
if [[ "$experiment" == shm-input ]]; then
    printf 'Capture once to verify Monitor kind=shm with renderer=gles2, then exercise Dashboard/CPU and repeated resizing.\n'
fi
printf 'The trace log is bounded; find it at %s\n' "$trace_log"

"$launcher" &
launcher_pid=$!
set +e
wait "$launcher_pid"
result=$?
set -e
launcher_pid=""
exit "$result"
