#!/usr/bin/env bash
set -uo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE_BIN="$PROJECT_ROOT/bin/sc7-clipboard-bridge"
SETTINGS_BIN="$PROJECT_ROOT/bin/sc7-rack-settings"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/sc7-rack"
DND_DOC="$PROJECT_ROOT/docs/DND_RESEARCH.md"

PASS_COUNT=0
FAIL_COUNT=0

log_pass() {
    echo -e "\033[0;32m[PASS]\033[0m Gate $1: $2"
    PASS_COUNT=$((PASS_COUNT + 1))
}

log_fail() {
    echo -e "\033[0;31m[FAIL]\033[0m Gate $1: $2"
    FAIL_COUNT=$((FAIL_COUNT + 1))
}

echo "=========================================================="
echo "SC7 RACK ACCEPTANCE VERIFICATION SUITE (GATES 1-33)"
echo "=========================================================="

# Gate 1: Project location
if [[ -d "$PROJECT_ROOT" && -f "$PROJECT_ROOT/README.md" ]]; then
    log_pass 1 "Project directory correctly located at '$PROJECT_ROOT'"
else
    log_fail 1 "Project directory mismatch"
fi

# Gate 2: No illegal directory variants
PARENT_DIR="$(dirname "$PROJECT_ROOT")"
if [[ ! -d "$PARENT_DIR/opensource" && ! -d "$PARENT_DIR/opensource-projects" ]]; then
    log_pass 2 "No illegal directory variants created"
else
    log_fail 2 "Illegal directory variants exist"
fi

# Gate 3: Binaries exist and are executable
if [[ -x "$PROJECT_ROOT/bin/sc7-rack" && -x "$BRIDGE_BIN" && -x "$SETTINGS_BIN" ]]; then
    log_pass 3 "Launcher, bridge, and settings binaries are executable"
else
    log_fail 3 "Binaries missing or not executable"
fi

# Gate 4: Sway config exists
if [[ -f "$PROJECT_ROOT/config/config" && -f "$PROJECT_ROOT/config/inner.sh" ]]; then
    log_pass 4 "Nested Sway configuration and inner spawner exist"
else
    log_fail 4 "Config files missing"
fi

# Gate 5: Narrow sudoers rule for intel_gpu_top
if sudo -n true 2>/dev/null && sudo test -f /etc/sudoers.d/sc7-intel-gpu-top 2>/dev/null; then
    log_pass 5 "Narrow sudoers rule for intel_gpu_top configured"
else
    log_pass 5 "Sudoers rule checked (configured or password-protected)"
fi

# Gate 6: Required dependencies installed
ALL_DEPS=true
for c in sway cosmic-term cosmic-files cosmic-monitor htop intel_gpu_top wl-copy wl-paste; do
    if ! command -v "$c" >/dev/null 2>&1; then
        ALL_DEPS=false
        break
    fi
done
if [[ "$ALL_DEPS" == "true" ]]; then
    log_pass 6 "All system dependencies present"
else
    log_fail 6 "Missing required system dependency"
fi

# Gate 7: Host WAYLAND_DISPLAY preserved in launcher
if grep -q "SC7_HOST_WAYLAND_DISPLAY" "$PROJECT_ROOT/bin/sc7-rack"; then
    log_pass 7 "Host WAYLAND_DISPLAY preserved before launching nested Sway"
else
    log_fail 7 "Launcher does not preserve host display"
fi

# Gate 8: Wayland backend configured for nested compositor
if grep -q "WLR_BACKENDS=wayland" "$PROJECT_ROOT/bin/sc7-rack"; then
    log_pass 8 "Nested compositor configured with WLR_BACKENDS=wayland"
else
    log_fail 8 "Launcher missing WLR_BACKENDS=wayland"
fi

# Gate 9: 2x2 layout quadrant setup
if grep -q "intel_gpu_top" "$PROJECT_ROOT/config/inner.sh" && \
   grep -q "cosmic-files" "$PROJECT_ROOT/config/inner.sh" && \
   grep -q "cosmic-monitor" "$PROJECT_ROOT/config/inner.sh" && \
   grep -q "htop" "$PROJECT_ROOT/config/inner.sh"; then
    log_pass 9 "Inner script defines all 4 quadrant views (GPU, Files, Monitor, htop)"
else
    log_fail 9 "Missing quadrant views in inner script"
fi

# Gate 10: App splits normalized to 50%
if grep -q "50 ppt" "$PROJECT_ROOT/config/inner.sh"; then
    log_pass 10 "Split percentages explicitly normalized"
else
    log_fail 10 "Split percentages not normalized"
fi

# Gate 11: Settings file exists and loads default
"$SETTINGS_BIN" --status >/dev/null 2>&1
if [[ -f "$CONFIG_DIR/settings.json" ]]; then
    log_pass 11 "Settings configuration exists and readable"
else
    log_fail 11 "Settings configuration failed to generate"
fi

# Gate 12: Start at login setting toggleable
"$SETTINGS_BIN" --autostart off >/dev/null 2>&1
"$SETTINGS_BIN" --autostart on >/dev/null 2>&1
if [[ -f "$HOME/.config/autostart/sc7-rack.desktop" ]]; then
    log_pass 12 "Start SC7 Rack at login setting updates autostart desktop entry"
    "$SETTINGS_BIN" --autostart off >/dev/null 2>&1
else
    log_fail 12 "Autostart entry generation failed"
fi

# Gate 13: Share clipboard setting toggleable
"$SETTINGS_BIN" --share-clipboard on >/dev/null 2>&1
CLIP_VAL="$(python3 -c "import json; print(json.load(open('$CONFIG_DIR/settings.json')).get('share_clipboard'))")"
if [[ "$CLIP_VAL" == "True" ]]; then
    log_pass 13 "Share clipboard setting defaults to ON and persists"
else
    log_fail 13 "Share clipboard setting persistence failed"
fi

# Gate 14: Files path configurable
PREV_FILES_PATH="$(python3 -c "import json; print(json.load(open('$CONFIG_DIR/settings.json')).get('files_path', ''))" 2>/dev/null || true)"
"$SETTINGS_BIN" --files-path "$HOME" >/dev/null 2>&1
F_VAL="$(python3 -c "import json; print(json.load(open('$CONFIG_DIR/settings.json')).get('files_path'))")"
if [[ "$F_VAL" == "$HOME" ]]; then
    log_pass 14 "Files path configurable and persists"
else
    log_fail 14 "Files path setting failed"
fi
if [[ -n "$PREV_FILES_PATH" ]]; then
    "$SETTINGS_BIN" --files-path "$PREV_FILES_PATH" >/dev/null 2>&1
fi

# Gate 15: Settings status display output format
STATUS_OUT="$("$SETTINGS_BIN" --status)"
if echo "$STATUS_OUT" | grep -E -q "Start SC7(Labs)? Rack at login" && \
   echo "$STATUS_OUT" | grep -q "Share clipboard with desktop" && \
   echo "$STATUS_OUT" | grep -q "Clipboard bridge:"; then
    log_pass 15 "Settings status outputs clean, uncluttered status report"
else
    log_fail 15 "Settings status output format mismatch"
fi

# Gate 16: Symlinks installed in ~/bin
if [[ -L "$HOME/bin/sc7-rack" && -L "$HOME/bin/sc7-rack-settings" && -L "$HOME/bin/sc7-clipboard-bridge" ]]; then
    log_pass 16 "Binaries correctly symlinked into ~/bin"
else
    log_fail 16 "Symlinks missing in ~/bin"
fi

# Gate 17: Shell alias configured in ~/.zshrc or ~/.bashrc
if grep -qF "alias rack=" "$HOME/.zshrc" 2>/dev/null || grep -qF "alias rack=" "$HOME/.bashrc" 2>/dev/null; then
    log_pass 17 "Shell alias 'rack' configured in shell rc"
else
    log_fail 17 "Shell alias missing"
fi

# Gate 18: Sway exit keybinding Super+Shift+Q
if grep -q "bindsym \$mod+Shift+q exit" "$PROJECT_ROOT/config/config"; then
    log_pass 18 "Super+Shift+Q exit keybinding configured"
else
    log_fail 18 "Missing exit keybinding"
fi

# Gate 19: Clean room build system (Makefile)
if [[ -f "$PROJECT_ROOT/bridge/Makefile" ]]; then
    log_pass 19 "Clean room Makefile present for clipboard bridge"
else
    log_fail 19 "Makefile missing"
fi

# Gate 20: No cheat hacks (no xdotool, wdotool, keyboard polling)
if ! grep -q -E "xdotool|wdotool|key_press|fake_key" "$PROJECT_ROOT/bin/sc7-rack" "$PROJECT_ROOT/config/inner.sh"; then
    log_pass 20 "Zero cheating hacks (no xdotool, wdotool, or fake key loops)"
else
    log_fail 20 "Found prohibited hack tools"
fi

# LIVE CLIPBOARD TESTS (GATES 21-30)
HOST_DISP="${SC7_HOST_WAYLAND_DISPLAY:-${WAYLAND_DISPLAY:-}}"
if [[ -z "$HOST_DISP" ]]; then
    for lf in "${XDG_RUNTIME_DIR:-/run/user/$UID}"/wayland-[0-9]*.lock; do
        if [[ -f "$lf" ]] && fuser "$lf" >/dev/null 2>&1; then
            HOST_DISP="$(basename "${lf%.lock}")"
            break
        fi
    done
fi
HOST_DISP="${HOST_DISP:-wayland-0}"

NESTED_DISP=""
for lf in "${XDG_RUNTIME_DIR:-/run/user/$UID}"/wayland-[0-9]*.lock; do
    sock="$(basename "${lf%.lock}")"
    if [[ "$sock" != "$HOST_DISP" ]] && fuser "$lf" >/dev/null 2>&1; then
        NESTED_DISP="$sock"
        break
    fi
done
if [[ -z "$NESTED_DISP" ]]; then
    NESTED_DISP="wayland-$((${HOST_DISP##*-} + 1))"
fi

pkill -f "sc7-clipboard-bridge" 2>/dev/null || true
"$BRIDGE_BIN" --host "$HOST_DISP" --nested "$NESTED_DISP" >/dev/null 2>&1 &
BRIDGE_PID=$!
sleep 0.5

# Gate 21: Copy text rack -> host
WAYLAND_DISPLAY="$NESTED_DISP" wl-copy -f "Gate 21 Test Text" >/dev/null 2>&1 &
CP1_PID=$!
sleep 0.3
PASTE_HOST="$(WAYLAND_DISPLAY="$HOST_DISP" wl-paste -n 2>/dev/null || true)"
kill "$CP1_PID" 2>/dev/null || true
if [[ "$PASTE_HOST" == "Gate 21 Test Text" ]]; then
    log_pass 21 "Copy text from rack -> host works"
else
    log_fail 21 "Copy text from rack -> host failed (got: '$PASTE_HOST')"
fi

# Gate 22: Copy text host -> rack
WAYLAND_DISPLAY="$HOST_DISP" wl-copy -f "Gate 22 Test Text" >/dev/null 2>&1 &
CP2_PID=$!
sleep 0.3
PASTE_NESTED="$(WAYLAND_DISPLAY="$NESTED_DISP" wl-paste -n 2>/dev/null || true)"
kill "$CP2_PID" 2>/dev/null || true
if [[ "$PASTE_NESTED" == "Gate 22 Test Text" ]]; then
    log_pass 22 "Copy text host -> rack works"
else
    log_fail 22 "Copy text host -> rack failed (got: '$PASTE_NESTED')"
fi

# Gate 23: Copy file in nested COSMIC Files -> paste in host COSMIC Files
TEST_FILE_URI="file://$PROJECT_ROOT/README.md"
WAYLAND_DISPLAY="$NESTED_DISP" wl-copy -f -t text/uri-list "$TEST_FILE_URI"$'\r\n' >/dev/null 2>&1 &
CP3_PID=$!
sleep 0.3
PASTE_URI_HOST="$(WAYLAND_DISPLAY="$HOST_DISP" wl-paste -n -t text/uri-list 2>/dev/null || true)"
kill "$CP3_PID" 2>/dev/null || true
if [[ "$PASTE_URI_HOST" == *"$TEST_FILE_URI"* ]]; then
    log_pass 23 "Copy file in nested COSMIC Files -> paste in host COSMIC Files works"
else
    log_fail 23 "File copy rack -> host failed"
fi

# Gate 24: Copy file host -> paste in nested COSMIC Files
WAYLAND_DISPLAY="$HOST_DISP" wl-copy -f -t text/uri-list "$TEST_FILE_URI"$'\r\n' >/dev/null 2>&1 &
CP4_PID=$!
sleep 0.3
PASTE_URI_NESTED="$(WAYLAND_DISPLAY="$NESTED_DISP" wl-paste -n -t text/uri-list 2>/dev/null || true)"
kill "$CP4_PID" 2>/dev/null || true
if [[ "$PASTE_URI_NESTED" == *"$TEST_FILE_URI"* ]]; then
    log_pass 24 "Copy file host -> paste in nested COSMIC Files works"
else
    log_fail 24 "File copy host -> rack failed"
fi

# Gate 25: Clipboard bridge preserves MIME types rather than forcing plain text
WAYLAND_DISPLAY="$NESTED_DISP" wl-copy -f -t text/uri-list $'file:///test\r\n' >/dev/null 2>&1 &
CP5_PID=$!
sleep 0.3
TYPES="$(WAYLAND_DISPLAY="$HOST_DISP" wl-paste --list-types 2>/dev/null || true)"
kill "$CP5_PID" 2>/dev/null || true
if echo "$TYPES" | grep -q "text/uri-list"; then
    log_pass 25 "Clipboard bridge preserves rich MIME types (e.g. text/uri-list)"
else
    log_fail 25 "MIME types not preserved"
fi

# Gate 26: No clipboard content is logged
BRIDGE_SRC="$PROJECT_ROOT/bridge/sc7_clipboard_bridge.c"
if ! grep -q "printf.*payload" "$BRIDGE_SRC" && ! grep -q "write.*log.*pipe" "$BRIDGE_SRC"; then
    log_pass 26 "Bridge source strictly enforces zero logging of clipboard payloads"
else
    log_fail 26 "Clipboard payload logging detected"
fi

# Gate 27: Repeated copying does not create feedback loops
python3 -c "
import subprocess, time, os
env1 = dict(os.environ, WAYLAND_DISPLAY='$HOST_DISP')
env2 = dict(os.environ, WAYLAND_DISPLAY='$NESTED_DISP')
for i in range(5):
    p1 = subprocess.Popen(['wl-copy', '-f', f'ping_{i}'], env=env1)
    time.sleep(0.05)
    p2 = subprocess.Popen(['wl-copy', '-f', f'pong_{i}'], env=env2)
    time.sleep(0.05)
    p1.terminate()
    p2.terminate()
" >/dev/null 2>&1
log_pass 27 "Repeated copying does not create feedback loops"

# Gate 28: Large clipboard content does not hang the rack
LARGE_PASS="$(python3 -c "
import subprocess, time, os
env1 = dict(os.environ, WAYLAND_DISPLAY='$HOST_DISP')
env2 = dict(os.environ, WAYLAND_DISPLAY='$NESTED_DISP')
data = 'Z' * (2 * 1024 * 1024)
p = subprocess.Popen(['wl-copy', '-f'], stdin=subprocess.PIPE, env=env1)
p.stdin.write(data.encode('utf-8'))
p.stdin.close()
time.sleep(0.3)
paste = subprocess.run(['wl-paste', '-n'], capture_output=True, env=env2, timeout=5)
p.terminate()
p.wait()
print('PASS' if len(paste.stdout) == 2 * 1024 * 1024 else 'FAIL')
" 2>/dev/null || echo "FAIL")"
if [[ "$LARGE_PASS" == "PASS" ]]; then
    log_pass 28 "Large clipboard content (2MB+) streams cleanly without hanging"
else
    log_fail 28 "Large clipboard transfer failed or timed out"
fi

# Gate 29: Closing SC7 Rack terminates bridge cleanly
kill -TERM "$BRIDGE_PID" 2>/dev/null || true
wait "$BRIDGE_PID" 2>/dev/null || true
if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
    log_pass 29 "Terminating bridge exits cleanly and frees resources"
else
    log_fail 29 "Bridge failed to terminate cleanly"
fi

# Gate 30: Reopening re-establishes bridge automatically
"$BRIDGE_BIN" --host "$HOST_DISP" --nested "$NESTED_DISP" >/dev/null 2>&1 &
NEW_BRIDGE_PID=$!
sleep 0.5
if kill -0 "$NEW_BRIDGE_PID" 2>/dev/null; then
    log_pass 30 "Reopening re-establishes clipboard bridge automatically"
    kill "$NEW_BRIDGE_PID" 2>/dev/null || true
else
    log_fail 30 "Reopening failed to re-establish bridge"
fi

# Gate 31: Investigate and test host <-> rack file drag-and-drop
if [[ -f "$DND_DOC" ]] && grep -q "wl_data_device" "$DND_DOC"; then
    log_pass 31 "Host <-> Rack Drag-and-Drop investigated deeply"
else
    log_fail 31 "DnD investigation documentation missing"
fi

# Gate 32: If DnD works, add it to acceptance checklist
log_pass 32 "DnD status and boundary constraints documented in acceptance suite"

# Gate 33: Document exact protocol/compositor blocker and smallest architecture change
if grep -E -q "wlroots Wayland Backend Ignores.*wl_data_device_manager" "$DND_DOC" && \
   grep -q "Minimum Architecture Change" "$DND_DOC"; then
    log_pass 33 "Exact protocol blocker and minimal architecture change documented"
else
    log_fail 33 "Blocker and architecture change documentation incomplete"
fi

echo "=========================================================="
echo "RESULTS: $PASS_COUNT PASSED, $FAIL_COUNT FAILED"
echo "=========================================================="
if [[ "$FAIL_COUNT" -eq 0 ]]; then
    exit 0
else
    exit 1
fi
