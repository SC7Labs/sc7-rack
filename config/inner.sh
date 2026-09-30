#!/usr/bin/env bash
set -u

XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
XDG_BIN_HOME="${XDG_BIN_HOME:-$HOME/.local/bin}"
export PATH="$XDG_BIN_HOME:$HOME/bin:/usr/local/bin:/usr/bin:/bin:$PATH"
if [[ -n "${SC7_RACK_PRIVATE_BIN:-}" && -x "$SC7_RACK_PRIVATE_BIN/xdg-open" ]]; then
    export PATH="$SC7_RACK_PRIVATE_BIN:$PATH"
fi

SETTINGS_FILE="$XDG_CONFIG_HOME/sc7-rack/settings.json"
FILES_PATH="${FILES_PATH:-$HOME}"
SHARE_CLIPBOARD=true

if [[ -f "$SETTINGS_FILE" ]]; then
    PARSED_PATH="$(python3 -c "import json, os; p = json.load(open('$SETTINGS_FILE')).get('files_path', ''); print(os.path.expanduser(p) if p else '')" 2>/dev/null || true)"
    if [[ -n "$PARSED_PATH" && -d "$PARSED_PATH" ]]; then
        FILES_PATH="$PARSED_PATH"
    fi
    PARSED_CLIP="$(python3 -c "import json; print(json.load(open('$SETTINGS_FILE')).get('share_clipboard', True))" 2>/dev/null || true)"
    if [[ "$PARSED_CLIP" == "False" || "$PARSED_CLIP" == "false" ]]; then
        SHARE_CLIPBOARD=false
    fi
fi

RACK_RUN_DIR="${XDG_RUNTIME_DIR:-/run/user/$UID}/sc7-rack"
mkdir -p "$RACK_RUN_DIR"
INNER_PIDS_FILE="$RACK_RUN_DIR/inner_pids"
INNER_HELPER_PID_FILE="$RACK_RUN_DIR/inner_helper.pid"

record_pid() {
    local pid="$1"
    if [[ -n "$pid" ]]; then
        echo "$pid" >> "$INNER_PIDS_FILE"
    fi
}

# Record this short-lived helper separately from long-lived client PIDs. The
# start time lets the launcher distinguish this process from a reused PID.
HELPER_START_TIME="$(awk '{print $22}' "/proc/$$/stat" 2>/dev/null || true)"
if [[ "$HELPER_START_TIME" =~ ^[0-9]+$ ]]; then
    printf '%s %s\n' "$$" "$HELPER_START_TIME" > "$INNER_HELPER_PID_FILE"
fi
clear_helper_record() {
    local recorded_pid="" recorded_start=""
    if [[ -f "$INNER_HELPER_PID_FILE" ]]; then
        read -r recorded_pid recorded_start < "$INNER_HELPER_PID_FILE" || true
        if [[ "$recorded_pid" == "$$" && "$recorded_start" == "$HELPER_START_TIME" ]]; then
            rm -f "$INNER_HELPER_PID_FILE"
        fi
    fi
}
trap clear_helper_record EXIT

rack_swaymsg() {
    # A closed Rack invalidates SWAYSOCK. Stop this layout pass at the first
    # failed IPC command, instead of issuing further commands to a stale socket.
    swaymsg "$@" >/dev/null 2>&1 || exit 0
}

# Start clipboard bridge if enabled
if [[ "$SHARE_CLIPBOARD" != "false" ]]; then
    BRIDGE_BIN="$(command -v sc7-clipboard-bridge || true)"
    if [[ -z "$BRIDGE_BIN" || ! -x "$BRIDGE_BIN" ]]; then
        for cand in "$XDG_BIN_HOME/sc7-clipboard-bridge" "$HOME/bin/sc7-clipboard-bridge"; do
            if [[ -x "$cand" ]]; then
                BRIDGE_BIN="$cand"
                break
            fi
        done
    fi

    if [[ -n "$BRIDGE_BIN" && -x "$BRIDGE_BIN" ]]; then
        NESTED_DISP="${WAYLAND_DISPLAY:-}"
        HOST_DISP="${SC7_HOST_WAYLAND_DISPLAY:-}"
        if [[ -z "$HOST_DISP" && -n "$NESTED_DISP" ]]; then
            for lockfile in "${XDG_RUNTIME_DIR:-/run/user/$UID}"/wayland-[0-9]*.lock; do
                sock="$(basename "${lockfile%.lock}")"
                if [[ "$sock" != "$NESTED_DISP" ]] && fuser "$lockfile" >/dev/null 2>&1; then
                    HOST_DISP="$sock"
                    break
                fi
            done
        fi
        if [[ -n "$HOST_DISP" && -n "$NESTED_DISP" ]]; then
            "$BRIDGE_BIN" --host "$HOST_DISP" --nested "$NESTED_DISP" >> "$RACK_RUN_DIR/clipboard-bridge.log" 2>&1 &
            record_pid $!
        fi
    fi
fi

wait_views() {
    local want="$1"
    local timeout="${2:-100}"
    local i n tree_json

    for ((i=0; i<timeout; i++)); do
        # A failed get_tree means the compositor is gone. Do not interpret it
        # as zero views and keep polling its dead IPC socket.
        tree_json="$(swaymsg -t get_tree -r 2>/dev/null)" || exit 0
        n="$(
            python3 -c '
import json,sys
try:
    root=json.load(sys.stdin)
except Exception:
    print(0); raise SystemExit

def walk(n):
    c = 1 if (n.get("app_id") or n.get("window")) and n.get("type") == "con" else 0
    for k in ("nodes","floating_nodes"):
        for ch in n.get(k,[]):
            c += walk(ch)
    return c
print(walk(root))
' <<< "$tree_json" 2>/dev/null
        )"
        [[ "${n:-0}" -ge "$want" ]] && return 0
        sleep 0.05
    done
    return 1
}

rack_swaymsg workspace 1

# If 4 or more views already exist on workspace 1, do not re-tile
CURRENT_TREE="$(swaymsg -t get_tree -r 2>/dev/null)" || exit 0
CURRENT_VIEWS="$(
    python3 -c '
import json,sys
try:
    root=json.load(sys.stdin)
except Exception:
    print(0); raise SystemExit

def walk(n):
    c = 1 if (n.get("app_id") or n.get("window")) and n.get("type") == "con" else 0
    for k in ("nodes","floating_nodes"):
        for ch in n.get(k,[]):
            c += walk(ch)
    return c
print(walk(root))
' <<< "$CURRENT_TREE" 2>/dev/null || echo 0
)"
if [[ "${CURRENT_VIEWS:-0}" -ge 4 ]]; then
    exit 0
fi

# 1) TOP LEFT — GPU Monitor (configurable provider: auto | intel | nvidia | amd | custom)
GPU_PROVIDER="auto"
GPU_CUSTOM=""
if [[ -f "$SETTINGS_FILE" ]]; then
    GPU_PROVIDER="$(python3 -c "import json; print(json.load(open('$SETTINGS_FILE')).get('gpu_provider', 'auto'))" 2>/dev/null || echo "auto")"
    GPU_CUSTOM="$(python3 -c "import json; print(json.load(open('$SETTINGS_FILE')).get('gpu_custom_command', ''))" 2>/dev/null || true)"
fi

resolve_gpu_command() {
    local provider="$1"
    case "$provider" in
        intel)
            if command -v intel_gpu_top >/dev/null 2>&1; then
                echo "sudo -n \"$(command -v intel_gpu_top)\""
            else
                echo "printf '%s\\n' 'intel_gpu_top is not installed.' 'This pane is available as a terminal.'"
            fi
            ;;
        nvidia)
            if command -v nvidia-smi >/dev/null 2>&1; then
                echo "nvidia-smi -l 1"
            elif command -v nvtop >/dev/null 2>&1; then
                echo "nvtop"
            else
                echo "printf '%s\\n' 'NVIDIA monitoring tools (nvidia-smi / nvtop) are not installed.' 'This pane is available as a terminal.'"
            fi
            ;;
        amd)
            if command -v radeontop >/dev/null 2>&1; then
                echo "radeontop"
            else
                echo "printf '%s\\n' 'radeontop is not installed.' 'This pane is available as a terminal.'"
            fi
            ;;
        custom)
            if [[ -n "${GPU_CUSTOM:-}" ]]; then
                echo "$GPU_CUSTOM"
            else
                echo "printf '%s\\n' 'No custom GPU command is configured.' 'This pane is available as a terminal.'"
            fi
            ;;
        auto|*)
            if command -v intel_gpu_top >/dev/null 2>&1; then
                echo "sudo -n \"$(command -v intel_gpu_top)\""
            elif command -v nvidia-smi >/dev/null 2>&1; then
                echo "nvidia-smi -l 1"
            elif command -v nvtop >/dev/null 2>&1; then
                echo "nvtop"
            elif command -v radeontop >/dev/null 2>&1; then
                echo "radeontop"
            else
                echo "printf '%s\\n' 'No supported GPU monitor found (intel_gpu_top / nvidia-smi / nvtop / radeontop).' 'This pane is available as a terminal.'"
            fi
            ;;
    esac
}

GPU_EXEC="$(resolve_gpu_command "$GPU_PROVIDER")"
# Keep the pane after a missing or exited monitor command. The user gets an
# interactive shell instead of a terminal that vanishes and retiles the rack.
cosmic-term --no-daemon -e bash -lc "$GPU_EXEC; gpu_status=\$?; printf '\nGPU monitor finished (status %s). Terminal ready.\n' \"\$gpu_status\"; exec bash -i" >/dev/null 2>&1 &
record_pid $!
wait_views 1 300 || { echo "GPU pane did not open" >&2; exit 1; }

# 2) TOP RIGHT — files path
rack_swaymsg split h
TARGET_DIR="${FILES_PATH:-$HOME}"
[[ -z "$TARGET_DIR" || ! -d "$TARGET_DIR" ]] && TARGET_DIR="$HOME"
FILES_LAUNCHER="$(dirname "$SC7_RACK_PRIVATE_BIN")/sc7-rack-files"
(cd "$TARGET_DIR" && exec "$FILES_LAUNCHER" "$TARGET_DIR") >/dev/null 2>&1 &
record_pid $!
wait_views 2 300 || { echo "Files pane did not open" >&2; exit 1; }

# 3) BOTTOM LEFT — COSMIC Monitor
rack_swaymsg focus left
rack_swaymsg split v
cosmic-monitor >/dev/null 2>&1 &
record_pid $!
wait_views 3 300 || { echo "System Monitor pane did not open" >&2; exit 1; }

# 4) BOTTOM RIGHT — htop
rack_swaymsg focus up
rack_swaymsg focus right
rack_swaymsg split v
cosmic-term --no-daemon -e bash -lc 'exec htop' >/dev/null 2>&1 &
record_pid $!
wait_views 4 300 || { echo "htop pane did not open" >&2; exit 1; }

# Explicitly normalize the row/column split percentages.
swaymsg '[app_id="com.system76.CosmicFiles"] resize set width 50 ppt' >/dev/null 2>&1 || true
swaymsg '[app_id="com.system76.CosmicMonitor"] resize set height 50 ppt' >/dev/null 2>&1 || true

# Shutdown monitor: if output window is closed, cleanly terminate nested Sway
(
    swaymsg -t subscribe '["output"]' -m 2>/dev/null | while IFS= read -r _; do
        # The subscription can deliver its final event during shutdown. A
        # disconnected socket ends this monitor without another IPC retry.
        outputs="$(swaymsg -t get_outputs 2>/dev/null)" || break
        num_outputs="$(python3 -c 'import json,sys; print(len(json.load(sys.stdin)))' <<< "$outputs" 2>/dev/null || echo "1")"
        if [[ "$num_outputs" -eq 0 ]]; then
            swaymsg exit >/dev/null 2>&1 || true
            break
        fi
    done
) &
record_pid $!
