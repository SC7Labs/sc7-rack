#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
echo "== Installing SC7 Rack from '$SCRIPT_DIR' =="

XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
XDG_BIN_HOME="${XDG_BIN_HOME:-$HOME/.local/bin}"

INSTALL_FAILED=false

# ─── Phase 1: Validate mandatory runtime dependencies ───────────────────────

echo ""
echo "── Checking mandatory runtime dependencies ──"

# Mandatory commands and their apt package mappings (empty = no known apt package)
declare -A APT_PACKAGES=(
    [sway]="sway"
    [swaymsg]=""        # provided by sway package
    [cosmic-term]=""    # COSMIC desktop component
    [cosmic-files]=""   # COSMIC desktop component
    [cosmic-monitor]="" # COSMIC desktop component
    [htop]="htop"
    [python3]="python3"
    [xdg-open]="xdg-utils"
)

MANDATORY_CMDS=(sway swaymsg cosmic-term cosmic-files cosmic-monitor htop python3 xdg-open)
MISSING_CMDS=()
APT_INSTALLABLE=()
COSMIC_MISSING=()

for cmd in "${MANDATORY_CMDS[@]}"; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        MISSING_CMDS+=("$cmd")
        case "$cmd" in
            cosmic-term|cosmic-files|cosmic-monitor)
                COSMIC_MISSING+=("$cmd")
                ;;
            *)
                pkg="${APT_PACKAGES[$cmd]:-}"
                if [[ -n "$pkg" ]]; then
                    APT_INSTALLABLE+=("$pkg")
                fi
                ;;
        esac
    else
        echo "  ✓ $cmd"
    fi
done

# Attempt to install apt-available packages on apt-based systems
if [[ ${#APT_INSTALLABLE[@]} -gt 0 ]] && command -v apt-get >/dev/null 2>&1; then
    echo ""
    echo "The following packages can be installed via apt: ${APT_INSTALLABLE[*]}"

    CAN_SUDO=false
    if sudo -n true 2>/dev/null; then
        CAN_SUDO=true
    fi

    if [[ "$CAN_SUDO" == "true" ]]; then
        echo "Installing: ${APT_INSTALLABLE[*]}"
        sudo apt-get update -qq
        sudo apt-get install -y -qq "${APT_INSTALLABLE[@]}"

        # Re-check which commands are now available
        NEW_MISSING=()
        for cmd in "${MISSING_CMDS[@]}"; do
            case "$cmd" in
                cosmic-term|cosmic-files|cosmic-monitor)
                    NEW_MISSING+=("$cmd")
                    ;;
                *)
                    if command -v "$cmd" >/dev/null 2>&1; then
                        echo "  ✓ $cmd (installed)"
                    else
                        NEW_MISSING+=("$cmd")
                    fi
                    ;;
            esac
        done
        MISSING_CMDS=("${NEW_MISSING[@]}")

        # After installing sway, verify swaymsg is also available
        if command -v sway >/dev/null 2>&1 && ! command -v swaymsg >/dev/null 2>&1; then
            echo "  ✗ swaymsg is still missing after installing sway" >&2
            if [[ ! " ${MISSING_CMDS[*]} " =~ " swaymsg " ]]; then
                MISSING_CMDS+=("swaymsg")
            fi
        fi
    else
        echo ""
        echo "Cannot install packages automatically (no passwordless sudo)."
        echo "Please install manually:"
        for pkg in "${APT_INSTALLABLE[@]}"; do
            echo "  sudo apt install $pkg"
        done
    fi
fi

# Stop if COSMIC components are missing
if [[ ${#COSMIC_MISSING[@]} -gt 0 ]]; then
    echo "" >&2
    echo "════════════════════════════════════════════════════════════" >&2
    echo "  ERROR: Missing COSMIC desktop components" >&2
    echo "════════════════════════════════════════════════════════════" >&2
    echo "" >&2
    echo "  SC7Labs Rack currently requires a COSMIC desktop environment." >&2
    echo "" >&2
    echo "  Missing commands:" >&2
    for cmd in "${COSMIC_MISSING[@]}"; do
        echo "    ✗ $cmd" >&2
    done
    echo "" >&2
    echo "  These are part of the COSMIC desktop (https://system76.com/cosmic)." >&2
    echo "  Please install COSMIC and try again." >&2
    echo "════════════════════════════════════════════════════════════" >&2
    exit 1
fi

# Stop if any other mandatory dependencies are still missing
NON_COSMIC_MISSING=()
for cmd in "${MISSING_CMDS[@]}"; do
    case "$cmd" in
        cosmic-term|cosmic-files|cosmic-monitor) ;; # already handled above
        *) NON_COSMIC_MISSING+=("$cmd") ;;
    esac
done

if [[ ${#NON_COSMIC_MISSING[@]} -gt 0 ]]; then
    echo "" >&2
    echo "════════════════════════════════════════════════════════════" >&2
    echo "  ERROR: Missing mandatory runtime dependencies" >&2
    echo "════════════════════════════════════════════════════════════" >&2
    echo "" >&2
    echo "  The following required commands are not available:" >&2
    for cmd in "${NON_COSMIC_MISSING[@]}"; do
        echo "    ✗ $cmd" >&2
    done
    echo "" >&2
    if command -v apt-get >/dev/null 2>&1; then
        echo "  Install with:  sudo apt install sway htop python3 xdg-utils" >&2
    elif command -v dnf >/dev/null 2>&1; then
        echo "  Install with:  sudo dnf install sway htop python3" >&2
    elif command -v pacman >/dev/null 2>&1; then
        echo "  Install with:  sudo pacman -S sway htop python3" >&2
    fi
    echo "════════════════════════════════════════════════════════════" >&2
    exit 1
fi

echo ""
echo "All mandatory runtime dependencies satisfied."

# ─── Phase 1b: Check Python Tkinter (optional but helpful) ──────────────────

echo ""
echo "── Checking Python Tkinter ──"
if ! python3 -c "import tkinter" >/dev/null 2>&1; then
    echo "Notice: Python Tkinter (python3-tk) is not installed."
    echo "The graphical settings window requires python3-tk."
    if sudo -n true 2>/dev/null && command -v apt-get >/dev/null 2>&1; then
        echo "Attempting non-interactive install of python3-tk via apt..."
        sudo apt-get install -y -qq python3-tk || true
    fi
    if ! python3 -c "import tkinter" >/dev/null 2>&1; then
        echo "To use the GUI settings window, please install python3-tk:"
        if command -v apt-get >/dev/null 2>&1; then
            echo "  sudo apt install python3-tk"
        elif command -v dnf >/dev/null 2>&1; then
            echo "  sudo dnf install python3-tkinter"
        elif command -v pacman >/dev/null 2>&1; then
            echo "  sudo pacman -S tk"
        fi
        echo "Settings can still be managed via CLI: sc7-rack-settings --status"
    else
        echo "  ✓ python3-tk installed successfully."
    fi
else
    echo "  ✓ Python Tkinter support: OK"
fi

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
make -C "$SCRIPT_DIR/bridge"

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

cp "$SCRIPT_DIR/desktop/sc7-rack.desktop" "$XDG_DATA_HOME/applications/sc7-rack.desktop"
cp "$SCRIPT_DIR/desktop/sc7-rack-settings.desktop" "$XDG_DATA_HOME/applications/sc7-rack-settings.desktop"
ln -sf sc7-rack.desktop "$XDG_DATA_HOME/applications/dev.sc7labs.rack.desktop"
chmod 644 "$XDG_DATA_HOME/applications/sc7-rack.desktop" "$XDG_DATA_HOME/applications/sc7-rack-settings.desktop"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$XDG_DATA_HOME/applications" 2>/dev/null || true
fi

# ─── Phase 9: Add shell alias ─────────────────────────────────────────────────

for rc in "$HOME/.zshrc" "$HOME/.bashrc"; do
    if [[ -f "$rc" ]] && ! grep -qF "alias rack=" "$rc" 2>/dev/null; then
        printf "\nalias rack='sc7-rack'\n" >> "$rc"
    fi
done

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
echo "Launch via COSMIC App Menu: 'SC7Labs Rack' or 'SC7Labs Rack Settings'"
echo "CLI Commands:"
echo "  sc7-rack             - Launch SC7Labs Rack"
echo "  sc7-rack-settings    - Manage settings (GUI / CLI)"
echo "  rack                 - Shell shortcut (symlink + alias)"
