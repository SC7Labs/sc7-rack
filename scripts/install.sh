#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
echo "== Installing SC7 Rack from '$SCRIPT_DIR' =="

XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
XDG_BIN_HOME="${XDG_BIN_HOME:-$HOME/.local/bin}"

mkdir -p "$HOME/bin" "$XDG_BIN_HOME" "$XDG_CONFIG_HOME/sc7-rack" "$XDG_DATA_HOME/applications"

# 1. Check Python and Tkinter dependencies
echo "Checking Python dependencies..."
if ! command -v python3 >/dev/null 2>&1; then
    echo "Error: Python 3 is required but not installed." >&2
    exit 1
fi

if ! /usr/bin/python3 -c "import tkinter" >/dev/null 2>&1; then
    echo "Notice: Python Tkinter (python3-tk) is not installed."
    echo "The graphical settings window requires python3-tk."
    if command -v apt-get >/dev/null 2>&1; then
        echo "Attempting to install python3-tk via apt..."
        if sudo apt-get update -qq && sudo apt-get install -y python3-tk; then
            echo "python3-tk installed successfully."
        else
            echo "Notice: Automatic installation of python3-tk was skipped or failed."
            echo "Please install it manually when convenient: sudo apt install python3-tk"
            echo "Settings can still be managed via CLI: sc7-rack-settings --status"
        fi
    elif command -v dnf >/dev/null 2>&1; then
        echo "Please install python3-tkinter: sudo dnf install python3-tkinter"
    elif command -v pacman >/dev/null 2>&1; then
        echo "Please install tk: sudo pacman -S tk"
    fi
else
    echo "Python Tkinter support: OK"
fi

# 2. Build clipboard bridge
echo "Building clipboard bridge..."
make -C "$SCRIPT_DIR/bridge"

# 3. Setup sudo rule for intel_gpu_top if passwordless sudo is configured
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

# 4. Copy config files (do NOT overwrite existing settings.json)
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

# 5. Link binaries to ~/.local/bin and ~/bin
for bindir in "$XDG_BIN_HOME" "$HOME/bin"; do
    mkdir -p "$bindir"
    ln -sf "$SCRIPT_DIR/bin/sc7-rack" "$bindir/sc7-rack"
    ln -sf "$SCRIPT_DIR/bin/sc7-rack-settings" "$bindir/sc7-rack-settings"
    ln -sf "$SCRIPT_DIR/bin/sc7-clipboard-bridge" "$bindir/sc7-clipboard-bridge"
done

# 6. Install icons into hicolor theme
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

# 7. Install .desktop launchers
cp "$SCRIPT_DIR/desktop/sc7-rack.desktop" "$XDG_DATA_HOME/applications/sc7-rack.desktop"
cp "$SCRIPT_DIR/desktop/sc7-rack-settings.desktop" "$XDG_DATA_HOME/applications/sc7-rack-settings.desktop"
chmod 644 "$XDG_DATA_HOME/applications/sc7-rack*.desktop"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$XDG_DATA_HOME/applications" 2>/dev/null || true
fi

# 8. Add shell alias
for rc in "$HOME/.zshrc" "$HOME/.bashrc"; do
    if [[ -f "$rc" ]] && ! grep -qF "alias rack=" "$rc" 2>/dev/null; then
        printf "\nalias rack='sc7-rack'\n" >> "$rc"
    fi
done

echo "SC7 Rack installed successfully."
echo "Launch via COSMIC App Menu: 'SC7 Rack' or 'SC7 Rack Settings'"
echo "CLI Commands:"
echo "  sc7-rack             - Launch SC7 Rack"
echo "  sc7-rack-settings    - Manage settings (GUI / CLI)"
echo "  rack                 - Shell shortcut"
