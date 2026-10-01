#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
echo "== Installing SC7 Rack from '$SCRIPT_DIR' =="

XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
XDG_BIN_HOME="${XDG_BIN_HOME:-$HOME/.local/bin}"

# Install user files as the calling desktop user. Only package installation
# needs sudo; running the whole installer as root would configure /root.
if (( EUID == 0 )); then
    echo "Run ./install.sh as your normal desktop user, without sudo." >&2
    echo "The installer will ask for your password when packages are needed." >&2
    exit 1
fi
PACKAGE_INSTALLER="$SCRIPT_DIR/scripts/install-packages.sh"
[[ -x "$PACKAGE_INSTALLER" ]] || {
    echo "ERROR: Package installer is missing: $PACKAGE_INSTALLER" >&2
    exit 1
}

# ─── Phase 1: Install and validate runtime dependencies ─────────────────────

echo ""
echo "── Checking runtime dependencies ──"

declare -A APT_PACKAGES=(
    [sway]="sway" [swaymsg]="sway"
    [cosmic-term]="cosmic-term" [cosmic-files]="cosmic-files"
    [cosmic-monitor]="cosmic-monitor" [htop]="htop"
    [python3]="python3" [xdg-open]="xdg-utils"
    [fuser]="psmisc" [pgrep]="procps"
)
MANDATORY_CMDS=(sway swaymsg cosmic-term cosmic-files cosmic-monitor htop python3 xdg-open fuser pgrep)
APT_INSTALLABLE=()
COSMIC_PRESENT=false
rack_desktop="${XDG_CURRENT_DESKTOP:-}"
if [[ "${rack_desktop^^}" == *COSMIC* ]] ||
        command -v cosmic-comp >/dev/null 2>&1 || command -v cosmic-session >/dev/null 2>&1; then
    COSMIC_PRESENT=true
fi
for cmd in "${MANDATORY_CMDS[@]}"; do
    if command -v "$cmd" >/dev/null 2>&1; then
        echo "  ✓ $cmd"
    else
        case "$cmd" in
            cosmic-term|cosmic-files|cosmic-monitor)
                if [[ "$COSMIC_PRESENT" != true ]]; then
                    echo "ERROR: SC7Labs Rack requires the COSMIC Wayland desktop." >&2
                    echo "Use Pop!_OS 24.04 with COSMIC, then rerun ./install.sh." >&2
                    exit 1
                fi
                ;;
        esac
        APT_INSTALLABLE+=("${APT_PACKAGES[$cmd]}")
    fi
done
if (( ${#APT_INSTALLABLE[@]} > 0 )); then
    mapfile -t APT_INSTALLABLE < <(printf '%s\n' "${APT_INSTALLABLE[@]}" | sort -u)
    "$PACKAGE_INSTALLER" "${APT_INSTALLABLE[@]}" || {
        echo "ERROR: Runtime package installation failed. Rerun ./install.sh after resolving the error above." >&2
        exit 1
    }
fi
for cmd in "${MANDATORY_CMDS[@]}"; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "ERROR: Required command '$cmd' is still missing after package installation." >&2
        exit 1
    fi
done
echo "All runtime dependencies satisfied."

# ─── Phase 1b: Install the graphical Settings dependency ────────────────────

echo ""
echo "── Checking graphical Settings support ──"
if ! python3 -c "import tkinter" >/dev/null 2>&1; then
    "$PACKAGE_INSTALLER" python3-tk || {
        echo "ERROR: Could not install the graphical Settings dependency." >&2
        exit 1
    }
fi
if ! python3 -c "import tkinter" >/dev/null 2>&1; then
    echo "ERROR: Python Tkinter is still unavailable; graphical Settings cannot start." >&2
    exit 1
fi
echo "  ✓ Python Tkinter support: OK"

# ─── Phase 1c: Report optional GPU tools ────────────────────────────────────

echo ""
echo "── Checking optional GPU tools ──"
OPTIONAL_GPU_CMDS=(intel_gpu_top nvidia-smi nvtop radeontop)
for cmd in "${OPTIONAL_GPU_CMDS[@]}"; do
    if command -v "$cmd" >/dev/null 2>&1; then
        echo "  ✓ $cmd (available)"
    else
        echo "  ○ $cmd (not found — the rack will use fallback messaging)"
    fi
done

# ─── Phase 2: Bootstrap patched wlroots (SC7Labs DnD + lifecycle) ───────────

echo ""
echo "── Bootstrapping patched wlroots (required for SC7Labs DnD) ──"

WLROOTS_BUILD="$SCRIPT_DIR/vendor/wlroots/build"
WLROOTS_LIB="$WLROOTS_BUILD/libwlroots.so.12"

if [[ ! -x "$SCRIPT_DIR/scripts/bootstrap-wlroots.sh" ]]; then
    echo "  ERROR: scripts/bootstrap-wlroots.sh is missing or not executable." >&2
    exit 1
fi
"$SCRIPT_DIR/scripts/bootstrap-wlroots.sh" --install-deps || {
    echo "" >&2
    echo "════════════════════════════════════════════════════════════" >&2
    echo "  ERROR: Patched wlroots bootstrap failed." >&2
    echo "  SC7Labs Rack DnD requires a local patched wlroots build." >&2
    echo "  System wlroots was NOT modified." >&2
    echo "  Fix the build error above and re-run ./install.sh" >&2
    echo "════════════════════════════════════════════════════════════" >&2
    exit 1
}

# Final verification — must have the patched library
if [[ ! -f "$WLROOTS_LIB" ]]; then
    echo "" >&2
    echo "════════════════════════════════════════════════════════════" >&2
    echo "  ERROR: $WLROOTS_LIB is still missing after bootstrap." >&2
    echo "  SC7Labs Rack will NOT silently fall back to system wlroots." >&2
    echo "════════════════════════════════════════════════════════════" >&2
    exit 1
fi

echo "  ✓ Patched wlroots: $WLROOTS_LIB"

# ─── Phase 2b: Build Rack-local Sway with the popup lifecycle fix ────────────

echo ""
echo "── Bootstrapping Rack-local Sway (required for context menus) ──"

SWAY_BOOTSTRAP="$SCRIPT_DIR/scripts/bootstrap-sway.sh"
RACK_SWAY="$SCRIPT_DIR/vendor/sway/build/sway/sway"
if [[ ! -x "$SWAY_BOOTSTRAP" ]]; then
    echo "  ERROR: scripts/bootstrap-sway.sh is missing or not executable." >&2
    exit 1
fi
"$SWAY_BOOTSTRAP" --install-deps || {
    echo "  ERROR: Rack-local Sway bootstrap failed. System Sway was not modified." >&2
    exit 1
}
if [[ ! -x "$RACK_SWAY" ]]; then
    echo "  ERROR: Rack-local Sway is missing after bootstrap: $RACK_SWAY" >&2
    exit 1
fi
echo "  ✓ Rack-local Sway: $RACK_SWAY"

# ─── Phase 3: Build clipboard bridge ────────────────────────────────────────

echo ""
echo "── Building clipboard bridge ──"
"$SCRIPT_DIR/scripts/wlroots_build_deps.py" --ensure --bridge-only || {
    echo "  ERROR: Clipboard bridge build dependencies are unavailable." >&2
    exit 1
}
# The repository can contain a prebuilt bridge with checkout timestamps newer
# than its source. Always build for this machine and its installed Wayland ABI.
make -B -C "$SCRIPT_DIR/bridge"
if [[ ! -x "$SCRIPT_DIR/bin/sc7-clipboard-bridge" ]]; then
    echo "ERROR: Clipboard bridge build did not produce an executable." >&2
    exit 1
fi

# ─── Phase 4: Setup sudo rule for intel_gpu_top (optional) ──────────────────

IGT="$(command -v intel_gpu_top || true)"
if [[ -n "$IGT" ]] && sudo -n true 2>/dev/null; then
    SUDOERS="/etc/sudoers.d/sc7-intel-gpu-top"
    RULE="$USER ALL=(root) NOPASSWD: $IGT \"\""
    if ! sudo test -f "$SUDOERS" 2>/dev/null; then
        echo "Configuring sudo rule for intel_gpu_top..."
        printf '%s\n' "$RULE" | sudo tee "$SUDOERS" >/dev/null
        sudo chmod 440 "$SUDOERS"
        sudo visudo -cf "$SUDOERS" >/dev/null
    fi
fi

# ─── Phase 5: Copy config files (preserve existing settings) ─────────────────

mkdir -p "$HOME/bin" "$XDG_BIN_HOME" "$XDG_CONFIG_HOME/sc7-rack" "$XDG_DATA_HOME/applications"

cp "$SCRIPT_DIR/config/config" "$XDG_CONFIG_HOME/sc7-rack/config"
cp "$SCRIPT_DIR/config/inner.sh" "$XDG_CONFIG_HOME/sc7-rack/inner.sh"
chmod +x "$XDG_CONFIG_HOME/sc7-rack/inner.sh"

SETTINGS_DEST="$XDG_CONFIG_HOME/sc7-rack/settings.json"
if [[ ! -f "$SETTINGS_DEST" ]]; then
    cp "$SCRIPT_DIR/settings/settings.json" "$SETTINGS_DEST"
    echo "Created default settings at $SETTINGS_DEST"
else
    echo "Preserved existing user settings at $SETTINGS_DEST"
fi

# ─── Phase 6: Link binaries to ~/.local/bin and ~/bin ────────────────────────

for bindir in "$XDG_BIN_HOME" "$HOME/bin"; do
    mkdir -p "$bindir"
    ln -sf "$SCRIPT_DIR/bin/sc7-rack" "$bindir/sc7-rack"
    ln -sf "$SCRIPT_DIR/bin/sc7-rack-settings" "$bindir/sc7-rack-settings"
    ln -sf "$SCRIPT_DIR/bin/sc7-clipboard-bridge" "$bindir/sc7-clipboard-bridge"
    ln -sf "$SCRIPT_DIR/bin/sc7-rack-open-host" "$bindir/sc7-rack-open-host"
    ln -sf "$SCRIPT_DIR/bin/sc7-rack-files" "$bindir/sc7-rack-files"
    # Create 'rack' symlink pointing to sc7-rack
    ln -sf "$SCRIPT_DIR/bin/sc7-rack" "$bindir/rack"
done

# ─── Phase 7: Install icons into hicolor theme ───────────────────────────────

HICOLOR="$XDG_DATA_HOME/icons/hicolor"
mkdir -p "$HICOLOR/scalable/apps"
cp "$SCRIPT_DIR/assets/icons/sc7-rack.svg" "$HICOLOR/scalable/apps/sc7-rack.svg"
chmod 644 "$HICOLOR/scalable/apps/sc7-rack.svg"

for s in 512 256 128 64 48 32; do
    size_dir="$HICOLOR/${s}x${s}/apps"
    mkdir -p "$size_dir"
    png_file="$SCRIPT_DIR/assets/icons/sc7-rack-${s}.png"
    if [[ -f "$png_file" ]]; then
        cp "$png_file" "$size_dir/sc7-rack.png"
        chmod 644 "$size_dir/sc7-rack.png"
    fi
done

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t "$HICOLOR" 2>/dev/null || true
fi

# ─── Phase 8: Install .desktop launchers ─────────────────────────────────────

# Absolute Exec paths work immediately in an existing desktop session whose
# PATH does not yet include the newly created user bin directories.
python3 "$SCRIPT_DIR/scripts/install_launchers.py"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$XDG_DATA_HOME/applications" 2>/dev/null || true
fi

# ─── Phase 10: Post-install runtime validation ──────────────────────────────

echo ""
echo "── Post-install runtime validation ──"
VALIDATION_CMDS=(sway swaymsg cosmic-term cosmic-files cosmic-monitor htop xdg-open sc7-rack sc7-rack-settings sc7-rack-open-host sc7-rack-files)
VALIDATION_FAILED=false

# Ensure newly installed symlinks are findable
export PATH="$XDG_BIN_HOME:$HOME/bin:$PATH"

for cmd in "${VALIDATION_CMDS[@]}"; do
    resolved="$(command -v "$cmd" 2>/dev/null || true)"
    if [[ -n "$resolved" ]]; then
        echo "  ✓ $cmd → $resolved"
    else
        echo "  ✗ $cmd NOT FOUND" >&2
        VALIDATION_FAILED=true
    fi
done

# Verify rack symlink
for bindir in "$XDG_BIN_HOME" "$HOME/bin"; do
    if [[ -L "$bindir/rack" ]]; then
        echo "  ✓ $bindir/rack → $(readlink "$bindir/rack")"
    else
        echo "  ✗ $bindir/rack symlink missing" >&2
        VALIDATION_FAILED=true
    fi
done

# Verify patched wlroots library (mandatory — no silent fallback)
if [[ -f "$WLROOTS_LIB" ]]; then
    echo "  ✓ Patched wlroots library present: $WLROOTS_LIB"
else
    echo "  ✗ Patched wlroots library MISSING: $WLROOTS_LIB" >&2
    echo "    SC7Labs DnD will not function without this library." >&2
    VALIDATION_FAILED=true
fi
if [[ -x "$RACK_SWAY" ]] && "$SWAY_BOOTSTRAP" --check >/dev/null 2>&1; then
    echo "  ✓ Rack-local Sway verified: $RACK_SWAY"
else
    echo "  ✗ Rack-local Sway missing or stale: $RACK_SWAY" >&2
    VALIDATION_FAILED=true
fi

if [[ "$VALIDATION_FAILED" == "true" ]]; then
    echo "" >&2
    echo "════════════════════════════════════════════════════════════" >&2
    echo "  ERROR: Post-install validation failed." >&2
    echo "  SC7Labs Rack was NOT installed successfully." >&2
    echo "  Review the errors above and re-run ./install.sh" >&2
    echo "════════════════════════════════════════════════════════════" >&2
    exit 1
fi

echo ""
echo "SC7Labs Rack installed successfully."
echo "Launch now with ./bin/sc7-rack, or via COSMIC App Menu: 'SC7Labs Rack' or 'SC7Labs Rack Settings'"
echo "CLI Commands:"
echo "  sc7-rack             - Launch SC7Labs Rack"
echo "  sc7-rack-settings    - Manage settings (GUI / CLI)"
echo "  rack                 - Shell shortcut (symlink + alias)"
