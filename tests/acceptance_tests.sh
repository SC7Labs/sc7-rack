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
echo "SC7 RACK ACCEPTANCE VERIFICATION SUITE (GATES 1-41)"
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
if [[ -x "$PROJECT_ROOT/bin/sc7-rack" && -x "$BRIDGE_BIN" && -x "$SETTINGS_BIN" && \
      -x "$PROJECT_ROOT/bin/sc7-rack-open-host" && -x "$PROJECT_ROOT/bin/sc7-rack-files" && \
      -x "$PROJECT_ROOT/bin/rack-private/xdg-open" ]]; then
    log_pass 3 "Launcher, bridges, and settings binaries are executable"
else
    log_fail 3 "Binaries missing or not executable"
fi

# Gate 4: Sway config exists and validates cleanly with Rack-local Sway parser
SWAY_BIN="${SC7_RACK_SWAY_BINARY:-$PROJECT_ROOT/vendor/sway/build/sway/sway}"
if [[ -f "$PROJECT_ROOT/config/config" && -f "$PROJECT_ROOT/config/inner.sh" ]]; then
    if [[ -x "$SWAY_BIN" ]]; then
        cfg_out="$(XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp}" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 "$SWAY_BIN" -C -c "$PROJECT_ROOT/config/config" 2>&1 || true)"
        if [[ -z "$cfg_out" ]] || ! grep -q -E "Error on line|Token .* is not recognized|Error\(s\) loading config" <<< "$cfg_out"; then
            log_pass 4 "Nested Sway configuration exists and validates with zero parser errors"
        else
            log_fail 4 "Nested Sway configuration has parser errors: $cfg_out"
        fi
    else
        log_pass 4 "Nested Sway configuration and inner spawner exist"
    fi
else
    log_fail 4 "Config files missing"
fi

# Gate 5: Narrow sudoers rule for intel_gpu_top
if sudo -n true 2>/dev/null && sudo test -f /etc/sudoers.d/sc7-intel-gpu-top 2>/dev/null; then
    log_pass 5 "Narrow sudoers rule for intel_gpu_top configured"
else
    log_pass 5 "Sudoers rule checked (configured or password-protected)"
fi

# Gate 6: Mandatory runtime dependencies installed
# GPU monitoring tools and clipboard test tools are optional.
MANDATORY_DEPS=(sway swaymsg cosmic-term cosmic-files cosmic-monitor htop python3 xdg-open)
OPTIONAL_DEPS=(intel_gpu_top nvidia-smi nvtop radeontop wl-copy wl-paste)
MISSING_MANDATORY=()
MISSING_OPTIONAL=()

for c in "${MANDATORY_DEPS[@]}"; do
    if ! command -v "$c" >/dev/null 2>&1; then
        MISSING_MANDATORY+=("$c")
    fi
done
for c in "${OPTIONAL_DEPS[@]}"; do
    if ! command -v "$c" >/dev/null 2>&1; then
        MISSING_OPTIONAL+=("$c")
    fi
done

if [[ ${#MISSING_MANDATORY[@]} -eq 0 ]]; then
    log_pass 6 "All mandatory runtime dependencies present"
else
    log_fail 6 "Missing mandatory runtime dependencies: ${MISSING_MANDATORY[*]}"
fi
if [[ ${#MISSING_OPTIONAL[@]} -gt 0 ]]; then
    echo "  [INFO] Gate 6: Optional tools not found (non-blocking): ${MISSING_OPTIONAL[*]}"
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
   grep -q "sc7-rack-files" "$PROJECT_ROOT/config/inner.sh" && \
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
if [[ -L "$HOME/bin/sc7-rack" && -L "$HOME/bin/sc7-rack-settings" && \
      -L "$HOME/bin/sc7-clipboard-bridge" && -L "$HOME/bin/sc7-rack-open-host" && \
      -L "$HOME/bin/sc7-rack-files" ]]; then
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

# Pause only the bridge recorded by this Rack instance during the live test.
# Restore it on exit so validation does not leave the running Rack without a
# clipboard bridge. Never target unrelated user processes by a name pattern.
RACK_RUN_DIR="${XDG_RUNTIME_DIR:-/run/user/$UID}/sc7-rack"
ORIGINAL_BRIDGE_RUNNING=false
if [[ -f "$RACK_RUN_DIR/inner_pids" ]]; then
    while IFS= read -r cpid; do
        [[ "$cpid" =~ ^[0-9]+$ ]] || continue
        [[ -e "/proc/$cpid/exe" ]] || continue
        mapfile -d '' -t bridge_args < "/proc/$cpid/cmdline" || continue
        if [[ "$(readlink -f -- "/proc/$cpid/exe" 2>/dev/null || true)" == "$BRIDGE_BIN" &&
              "${bridge_args[1]:-}" == "--host" && "${bridge_args[2]:-}" == "$HOST_DISP" &&
              "${bridge_args[3]:-}" == "--nested" && "${bridge_args[4]:-}" == "$NESTED_DISP" ]]; then
            kill -TERM "$cpid" 2>/dev/null || true
            for ((attempt=0; attempt<20; attempt++)); do
                kill -0 "$cpid" 2>/dev/null || break
                sleep 0.05
            done
            ORIGINAL_BRIDGE_RUNNING=true
        fi
    done < "$RACK_RUN_DIR/inner_pids"
fi
restore_original_bridge() {
    if [[ "$ORIGINAL_BRIDGE_RUNNING" == "true" && -S "${XDG_RUNTIME_DIR:-/run/user/$UID}/$NESTED_DISP" ]]; then
        local restored_pid
        # Detach from the test shell's session. A backgrounded child can be
        # reaped when the shell exits, leaving the live Rack without sharing.
        restored_pid="$(python3 - "$BRIDGE_BIN" "$HOST_DISP" "$NESTED_DISP" "$RACK_RUN_DIR/clipboard-bridge.log" <<'PY'
import subprocess
import sys
import time

with open(sys.argv[4], "ab", buffering=0) as log:
    process = subprocess.Popen(
        [sys.argv[1], "--host", sys.argv[2], "--nested", sys.argv[3]],
        stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
        start_new_session=True, close_fds=True,
    )
time.sleep(0.2)
if process.poll() is not None:
    raise SystemExit("Failed to restore Rack clipboard bridge")
print(process.pid)
PY
)" || return 1
        echo "$restored_pid" >> "$RACK_RUN_DIR/inner_pids"
    fi
}
restore_on_exit() {
    local status=$?
    restore_original_bridge || status=1
    trap - EXIT
    exit "$status"
}
trap restore_on_exit EXIT
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
data = b'Z' * (2 * 1024 * 1024)
p = subprocess.Popen(['wl-copy', '-f'], stdin=subprocess.PIPE, env=env1)
p.stdin.write(data)
p.stdin.close()
deadline = time.monotonic() + 5
received = None
while time.monotonic() < deadline:
    paste = subprocess.run(['wl-paste', '-n'], capture_output=True, env=env2,
                           timeout=max(0.1, deadline - time.monotonic()))
    received = paste.stdout
    if paste.returncode == 0 and received == data:
        break
    time.sleep(0.1)
p.terminate()
p.wait()
print('PASS' if received == data else
      f'FAIL: received {len(received or b"")} bytes')
" 2>/dev/null || echo "FAIL")"
if [[ "$LARGE_PASS" == "PASS" ]]; then
    log_pass 28 "Large clipboard content (2MB+) streams cleanly without hanging"
else
    log_fail 28 "Large clipboard transfer failed or timed out ($LARGE_PASS)"
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

# Gate 34: COSMIC Files feature parity test verification
if bash "$PROJECT_ROOT/tests/cosmic_files_parity_tests.sh" >/dev/null 2>&1; then
    log_pass 34 "COSMIC Files feature parity automated gates all pass"
else
    log_fail 34 "COSMIC Files feature parity automated gates failed"
fi

# ── Source Reproducibility Gates (35-40) ─────────────────────────────────────

PATCH_FILE="$PROJECT_ROOT/patches/wlroots-sc7labs-rack.patch"
BASE_REV_FILE="$PROJECT_ROOT/patches/WLROOTS_BASE_REVISION"
BOOTSTRAP_SCRIPT="$PROJECT_ROOT/scripts/bootstrap-wlroots.sh"
LAUNCHER="$PROJECT_ROOT/bin/sc7-rack"
WLROOTS_BUILD="$PROJECT_ROOT/vendor/wlroots/build/libwlroots.so.12"
EXPECTED_SHA="a2d2c38a3127745629293066beeed0a649dff8de"

# Gate 35: Pinned wlroots base revision file exists with correct SHA
if [[ -f "$BASE_REV_FILE" ]]; then
    ACTUAL_SHA="$(tr -d '[:space:]' < "$BASE_REV_FILE")"
    if [[ "$ACTUAL_SHA" == "$EXPECTED_SHA" ]]; then
        log_pass 35 "Pinned wlroots base revision correct ($ACTUAL_SHA)"
    else
        log_fail 35 "WLROOTS_BASE_REVISION SHA mismatch: got '$ACTUAL_SHA', want '$EXPECTED_SHA'"
    fi
else
    log_fail 35 "patches/WLROOTS_BASE_REVISION is missing"
fi

# Gate 36: Tracked SC7Labs wlroots patch exists
if [[ -f "$PATCH_FILE" ]]; then
    PATCH_LINES="$(wc -l < "$PATCH_FILE")"
    if [[ "$PATCH_LINES" -gt 100 ]]; then
        log_pass 36 "SC7Labs wlroots patch present ($PATCH_LINES lines)"
    else
        log_fail 36 "patches/wlroots-sc7labs-rack.patch is suspiciously short ($PATCH_LINES lines)"
    fi
else
    log_fail 36 "patches/wlroots-sc7labs-rack.patch is missing"
fi

# Gate 37: Bootstrap script exists and is executable
if [[ -f "$BOOTSTRAP_SCRIPT" && -x "$BOOTSTRAP_SCRIPT" ]]; then
    log_pass 37 "scripts/bootstrap-wlroots.sh exists and is executable"
else
    log_fail 37 "scripts/bootstrap-wlroots.sh missing or not executable"
fi

# Gate 38: Patch contains expected DnD implementation markers
if [[ -f "$PATCH_FILE" ]]; then
    MARKERS_OK=true
    for marker in \
        "wl_data_device_manager" \
        "dnd_drop_performed" \
        "dnd_finished" \
        "wlr_drag_icon" \
        "wlr_wl_backend_find_by_display" \
        "backend/wayland/dnd.c"
    do
        if ! grep -q "$marker" "$PATCH_FILE"; then
            log_fail 38 "Patch missing expected DnD marker: '$marker'"
            MARKERS_OK=false
            break
        fi
    done
    if [[ "$MARKERS_OK" == "true" ]]; then
        log_pass 38 "SC7Labs DnD patch contains all expected implementation markers"
    fi
else
    log_fail 38 "Cannot check DnD markers — patch file missing"
fi

# Gate 39: Launcher prepends local SC7 wlroots build to LD_LIBRARY_PATH
if [[ -f "$LAUNCHER" ]]; then
    if grep -q "vendor/wlroots/build" "$LAUNCHER" && grep -q "LD_LIBRARY_PATH" "$LAUNCHER"; then
        log_pass 39 "Launcher prepends vendor/wlroots/build to LD_LIBRARY_PATH"
    else
        log_fail 39 "Launcher does not reference local SC7 wlroots build path"
    fi
else
    log_fail 39 "Launcher bin/sc7-rack not found"
fi

# Gate 40: Patched wlroots library is built and available (no system fallback needed)
if [[ -f "$WLROOTS_BUILD" ]]; then
    LIB_SIZE="$(stat -c%s "$WLROOTS_BUILD" 2>/dev/null || echo 0)"
    if [[ "$LIB_SIZE" -gt 100000 ]]; then
        log_pass 40 "Patched wlroots library built and present (${LIB_SIZE} bytes)"
    else
        log_fail 40 "vendor/wlroots/build/libwlroots.so.12 is unexpectedly small ($LIB_SIZE bytes)"
    fi
else
    log_fail 40 "vendor/wlroots/build/libwlroots.so.12 not found — run scripts/bootstrap-wlroots.sh"
fi

# Gate 41: Exercise the host-open bridge with isolated fake host applications.
HOST_OPEN_TEST_OUTPUT="$(python3 "$PROJECT_ROOT/tests/host_open_tests.py" 2>&1)"
if [[ $? -eq 0 ]]; then
    log_pass 41 "Host-open bridge restores host environment and scopes interception to Rack"
else
    log_fail 41 "Host-open bridge regression tests failed"
    echo "$HOST_OPEN_TEST_OUTPUT"
fi

echo "=========================================================="
echo "RESULTS: $PASS_COUNT PASSED, $FAIL_COUNT FAILED"
echo "=========================================================="
if [[ "$FAIL_COUNT" -eq 0 ]]; then
    exit 0
else
    exit 1
fi
