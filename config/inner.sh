#!/usr/bin/env bash
set -u

XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
XDG_BIN_HOME="${XDG_BIN_HOME:-$HOME/.local/bin}"
export PATH="$XDG_BIN_HOME:$HOME/bin:/usr/local/bin:/usr/bin:/bin:$PATH"

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

# Start clipboard bridge if enabled
if [[ "$SHARE_CLIPBOARD" != "false" ]]; then
    pkill -f "sc7-clipboard-bridge" 2>/dev/null || true
    
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
            "$BRIDGE_BIN" --host "$HOST_DISP" --nested "$NESTED_DISP" >/dev/null 2>&1 &
        fi
    fi
fi

wait_views() {
    local want="$1"
    local timeout="${2:-100}"
    local i n

    for ((i=0; i<timeout; i++)); do
        n="$(
            swaymsg -t get_tree -r 2>/dev/null |
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
' 2>/dev/null
        )"
        [[ "${n:-0}" -ge "$want" ]] && return 0
        sleep 0.05
    done
    return 1
}

swaymsg workspace 1 >/dev/null

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
                echo "echo 'intel_gpu_top not found'; sleep 5"
            fi
            ;;
        nvidia)
            if command -v nvidia-smi >/dev/null 2>&1; then
                echo "nvidia-smi -l 1"
            elif command -v nvtop >/dev/null 2>&1; then
                echo "nvtop"
            else
                echo "echo 'NVIDIA monitoring tools (nvidia-smi / nvtop) not found'; sleep 5"
            fi
            ;;
        amd)
            if command -v radeontop >/dev/null 2>&1; then
                echo "radeontop"
            else
                echo "echo 'radeontop not found'; sleep 5"
            fi
            ;;
        custom)
            if [[ -n "${GPU_CUSTOM:-}" ]]; then
                echo "$GPU_CUSTOM"
            else
                echo "echo 'No custom GPU command configured'; sleep 5"
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
                echo "echo 'No supported GPU monitor found (intel_gpu_top / nvidia-smi / nvtop / radeontop)'; sleep 5"
            fi
            ;;
    esac
}

GPU_EXEC="$(resolve_gpu_command "$GPU_PROVIDER")"
cosmic-term --no-daemon -e bash -lc "exec $GPU_EXEC" >/dev/null 2>&1 &
wait_views 1 || true

# 2) TOP RIGHT — files path
swaymsg split h >/dev/null
cosmic-files "$FILES_PATH" >/dev/null 2>&1 &
wait_views 2 || true

# 3) BOTTOM LEFT — COSMIC Monitor
swaymsg focus left >/dev/null
swaymsg split v >/dev/null
cosmic-monitor >/dev/null 2>&1 &
wait_views 3 || true

# 4) BOTTOM RIGHT — htop
swaymsg focus up >/dev/null
swaymsg focus right >/dev/null
swaymsg split v >/dev/null
cosmic-term --no-daemon -e bash -lc 'exec htop' >/dev/null 2>&1 &
wait_views 4 || true

# Explicitly normalize the row/column split percentages.
swaymsg '[app_id="com.system76.CosmicFiles"] resize set width 50 ppt' >/dev/null 2>&1 || true
swaymsg '[app_id="com.system76.CosmicMonitor"] resize set height 50 ppt' >/dev/null 2>&1 || true
