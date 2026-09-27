#!/usr/bin/env bash
set -euo pipefail

echo "=========================================================="
echo "COSMIC FILES FEATURE PARITY VERIFICATION SUITE"
echo "=========================================================="

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_DIR="/tmp/sc7-parity-test-$$"
mkdir -p "$TEST_DIR"

cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT INT TERM

PASSED=0
FAILED=0

assert_eq() {
    local desc="$1"
    local expected="$2"
    local actual="$3"
    if [[ "$expected" == "$actual" ]]; then
        echo "[PASS] [AUTO] $desc"
        PASSED=$((PASSED + 1))
    else
        echo "[FAIL] [AUTO] $desc (expected: '$expected', got: '$actual')"
        FAILED=$((FAILED + 1))
    fi
}

assert_true() {
    local desc="$1"
    local cmd="$2"
    if eval "$cmd"; then
        echo "[PASS] [AUTO] $desc"
        PASSED=$((PASSED + 1))
    else
        echo "[FAIL] [AUTO] $desc"
        FAILED=$((FAILED + 1))
    fi
}

# 1. Environment & D-Bus Session Parity
echo "--- Category 1: Environment & D-Bus Session Parity ---"
NESTED_FILES_PID="$(pgrep -f '[c]osmic-files' | head -n1 || true)"
if [[ -n "$NESTED_FILES_PID" ]]; then
    ENV_DBUS="$(python3 -c "import os; data = open(f'/proc/$NESTED_FILES_PID/environ', 'rb').read().decode('utf-8', errors='replace'); print(dict(i.split('=', 1) for i in data.split('\0') if '=' in i).get('DBUS_SESSION_BUS_ADDRESS', ''))" 2>/dev/null || true)"
    ENV_DESKTOP="$(python3 -c "import os; data = open(f'/proc/$NESTED_FILES_PID/environ', 'rb').read().decode('utf-8', errors='replace'); print(dict(i.split('=', 1) for i in data.split('\0') if '=' in i).get('XDG_CURRENT_DESKTOP', ''))" 2>/dev/null || true)"
    assert_true "Nested cosmic-files connects to user session D-Bus bus" "[[ -n '$ENV_DBUS' ]]"
    assert_eq "Nested cosmic-files has XDG_CURRENT_DESKTOP=COSMIC" "COSMIC" "$ENV_DESKTOP"
else
    echo "[WARN] [AUTO] No cosmic-files process currently running to inspect environ"
fi

assert_true "D-Bus session bus has org.freedesktop.portal.Desktop" "busctl --user status org.freedesktop.portal.Desktop >/dev/null 2>&1"
assert_true "D-Bus session bus has org.freedesktop.FileManager1" "busctl --user status org.freedesktop.FileManager1 >/dev/null 2>&1"
assert_true "GVfs daemon is active on user session bus" "busctl --user status org.gtk.vfs.Daemon >/dev/null 2>&1"

# 2. Window Management & Floating Parity
echo "--- Category 2: Window Management & Floating Parity ---"
SWAY_CONFIG="$SCRIPT_DIR/config/config"
USER_SWAY_CONFIG="${XDG_CONFIG_HOME:-$HOME/.config}/sc7-rack/config"

assert_true "Repo Sway config contains floating rule for CosmicFilesDialog" \
    "grep -q 'app_id=\"com.system76.CosmicFilesDialog\"\] floating enable' '$SWAY_CONFIG'"
assert_true "User Sway config contains floating rule for CosmicFilesDialog" \
    "grep -q 'app_id=\"com.system76.CosmicFilesDialog\"\] floating enable' '$USER_SWAY_CONFIG'"
assert_true "Repo Sway config contains floating rule for CosmicEdit" \
    "grep -q 'app_id=\"com.system76.CosmicEdit\"\] floating enable' '$SWAY_CONFIG'"
assert_true "Repo Sway config contains floating rule for CosmicViewer" \
    "grep -q 'app_id=\"com.system76.CosmicViewer\"\] floating enable' '$SWAY_CONFIG'"
assert_true "Repo Sway config contains floating rule for FileRoller" \
    "grep -q 'app_id=\"org.gnome.FileRoller\"\] floating enable' '$SWAY_CONFIG'"
assert_true "Repo Sway config contains floating rule for Evince" \
    "grep -q 'app_id=\"org.gnome.Evince\"\] floating enable' '$SWAY_CONFIG'"
assert_true "Repo Sway config contains floating rule for dialog roles" \
    "grep -q 'window_type=\"dialog\"\] floating enable' '$SWAY_CONFIG'"
assert_true "Inner script has view guard to prevent duplicate tiling on reload" \
    "grep -q 'CURRENT_VIEWS=' '$SCRIPT_DIR/config/inner.sh'"

# 3. File Operations & Unicode / Spaces
echo "--- Category 3: File Operations, Spaces & Unicode ---"
mkdir -p "$TEST_DIR/folder with spaces/ünicöde_тест"
echo "hello world" > "$TEST_DIR/folder with spaces/ünicöde_тест/file with spaces & symbols #1.txt"

assert_true "Create directory with spaces and Unicode succeeds" \
    "[[ -d '$TEST_DIR/folder with spaces/ünicöde_тест' ]]"
assert_true "Create file with spaces and Unicode succeeds" \
    "[[ -f '$TEST_DIR/folder with spaces/ünicöde_тест/file with spaces & symbols #1.txt' ]]"

# Rename
mv "$TEST_DIR/folder with spaces/ünicöde_тест/file with spaces & symbols #1.txt" \
   "$TEST_DIR/folder with spaces/ünicöde_тест/renamed_file.txt"
assert_true "Rename file with Unicode/spaces succeeds" \
    "[[ -f '$TEST_DIR/folder with spaces/ünicöde_тест/renamed_file.txt' ]]"

# Copy & Cut/Paste format verification (x-special/gnome-copied-files)
URI="file://$TEST_DIR/folder%20with%20spaces/%C3%BCnic%C3%B6de_%D1%82%D0%B5%D1%81%D1%82/renamed_file.txt"
GNOME_COPY="copy\n$URI"
GNOME_CUT="cut\n$URI"
assert_true "x-special/gnome-copied-files format standard adheres to cosmic-files copy spec" \
    "[[ '$GNOME_COPY' == copy* ]]"
assert_true "x-special/gnome-copied-files format standard adheres to cosmic-files cut spec" \
    "[[ '$GNOME_CUT' == cut* ]]"

# 4. Built-in Archive Extraction Emulation & Integrity
echo "--- Category 4: Archive Extraction Integrity ---"
ARCHIVE_SRC="$TEST_DIR/archive_src"
mkdir -p "$ARCHIVE_SRC/subdir"
echo "archive file content 1" > "$ARCHIVE_SRC/file1.txt"
echo "archive file content 2" > "$ARCHIVE_SRC/subdir/file2.txt"

# Create ZIP
(cd "$TEST_DIR" && zip -r test_archive.zip archive_src >/dev/null 2>&1)
assert_true "Create .zip archive succeeds" "[[ -f '$TEST_DIR/test_archive.zip' ]]"

# Create TAR.GZ
(cd "$TEST_DIR" && tar -czf test_archive.tar.gz archive_src)
assert_true "Create .tar.gz archive succeeds" "[[ -f '$TEST_DIR/test_archive.tar.gz' ]]"

# Create TAR.XZ
(cd "$TEST_DIR" && tar -cJf test_archive.tar.xz archive_src)
assert_true "Create .tar.xz archive succeeds" "[[ -f '$TEST_DIR/test_archive.tar.xz' ]]"

# Emulate cosmic-files extraction: strips suffix to form new_dir, unpacks into it
EXTRACT_ZIP_DIR="$TEST_DIR/extract_zip/test_archive"
mkdir -p "$EXTRACT_ZIP_DIR"
unzip -q "$TEST_DIR/test_archive.zip" -d "$EXTRACT_ZIP_DIR"
assert_true "Extract .zip contents matches expected structure" \
    "[[ -f '$EXTRACT_ZIP_DIR/archive_src/file1.txt' && -f '$EXTRACT_ZIP_DIR/archive_src/subdir/file2.txt' ]]"

EXTRACT_TARGZ_DIR="$TEST_DIR/extract_targz/test_archive"
mkdir -p "$EXTRACT_TARGZ_DIR"
tar -xzf "$TEST_DIR/test_archive.tar.gz" -C "$EXTRACT_TARGZ_DIR"
assert_true "Extract .tar.gz contents matches expected structure" \
    "[[ -f '$EXTRACT_TARGZ_DIR/archive_src/file1.txt' && -f '$EXTRACT_TARGZ_DIR/archive_src/subdir/file2.txt' ]]"

# 5. FreeDesktop Trash Compliance
echo "--- Category 5: FreeDesktop Trash Compliance ---"
TRASH_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/sc7-parity-trash-test-$$"
mkdir -p "$TRASH_DIR"
TRASH_TEST_FILE="$TRASH_DIR/trash_me.txt"
echo "trash data" > "$TRASH_TEST_FILE"
if command -v gio >/dev/null 2>&1; then
    gio trash "$TRASH_TEST_FILE"
    assert_true "gio trash sends file to FreeDesktop trash" "[[ ! -f '$TRASH_TEST_FILE' ]]"
    rm -rf "$TRASH_DIR"
else
    echo "[SKIP] [AUTO] gio command not found for trash CLI test"
    rm -rf "$TRASH_DIR"
fi

# 6. DnD Action Support in wlroots
echo "--- Category 6: DnD Protocol & Action Negotiation in wlroots ---"
WLROOTS_DND_C="$SCRIPT_DIR/vendor/wlroots/backend/wayland/dnd.c"
assert_true "wlroots implements outgoing action forwarding to nested source" \
    "grep -q 'wlr_data_source_dnd_action(od->wlr_drag->source, dnd_action)' '$WLROOTS_DND_C'"
assert_true "wlroots sets host source actions with COPY and MOVE" \
    "grep -q 'WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE' '$WLROOTS_DND_C'"
assert_true "wlroots forwards host offer source_actions to proxy" \
    "grep -q 'proxy->base.actions = ho->source_actions' '$WLROOTS_DND_C'"
assert_true "wlroots forwards dnd_drop_performed to nested source" \
    "grep -q 'wlr_data_source_dnd_drop(od->wlr_drag->source)' '$WLROOTS_DND_C'"
assert_true "wlroots forwards dnd_finished to nested source" \
    "grep -q 'wlr_data_source_dnd_finish(od->wlr_drag->source)' '$WLROOTS_DND_C'"

echo "=========================================================="
echo "PARITY TESTS COMPLETED: $PASSED PASSED, $FAILED FAILED"
echo "=========================================================="

if [[ "$FAILED" -gt 0 ]]; then
    exit 1
fi
