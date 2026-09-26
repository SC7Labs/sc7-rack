#!/usr/bin/env bash
set -u

export PATH="$HOME/.local/bin:$HOME/.cargo/bin:/home/sc7/projects/opensource projects/sc7-rack/bin:$HOME/bin:/usr/local/bin:/usr/bin:/bin:$PATH"

SETTINGS_FILE="$HOME/.config/sc7-rack/settings.json"
FILES_PATH="$HOME/projects/portfolio"
SHARE_CLIPBOARD=true

if [[ -f "$SETTINGS_FILE" ]]; then
    PARSED_PATH="$(python3 -c "import json; print(json.load(open('$SETTINGS_FILE')).get('files_path', ''))" 2>/dev/null || true)"
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
    
    BRIDGE_BIN="/home/sc7/projects/opensource projects/sc7-rack/bin/sc7-clipboard-bridge"
    if [[ ! -x "$BRIDGE_BIN" ]]; then
        BRIDGE_BIN="$HOME/bin/sc7-clipboard-bridge"
    fi

    if [[ -x "$BRIDGE_BIN" ]]; then
        HOST_DISP="${SC7_HOST_WAYLAND_DISPLAY:-wayland-1}"
        NESTED_DISP="${WAYLAND_DISPLAY:-wayland-2}"
        "$BRIDGE_BIN" --host "$HOST_DISP" --nested "$NESTED_DISP" >/dev/null 2>&1 &
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

# 1) TOP LEFT — intel_gpu_top
cosmic-term --no-daemon -e bash -lc 'exec sudo -n "$(command -v intel_gpu_top)"' \
    >/dev/null 2>&1 &
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
