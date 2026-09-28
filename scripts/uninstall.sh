#!/usr/bin/env bash
set -euo pipefail

PURGE=false
for arg in "$@"; do
    if [[ "$arg" == "--purge" ]]; then
        PURGE=true
    elif [[ "$arg" == "-h" || "$arg" == "--help" ]]; then
        echo "Usage: $0 [--purge]"
        echo "  Uninstalls SC7 Rack application, desktop launchers, and binaries."
        echo "  --purge  Also remove user configuration and settings ($HOME/.config/sc7-rack)"
        exit 0
    fi
done

echo "== Uninstalling SC7 Rack =="

XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
XDG_BIN_HOME="${XDG_BIN_HOME:-$HOME/.local/bin}"
RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$UID}"

# 1. Stop any running SC7 Rack instance
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -x "$SCRIPT_DIR/bin/sc7-rack" ]]; then
    "$SCRIPT_DIR/bin/sc7-rack" --stop 2>/dev/null || true
fi

# 2. Remove desktop launchers
echo "Removing desktop entries..."
rm -f "$XDG_DATA_HOME/applications/sc7-rack.desktop"
rm -f "$XDG_DATA_HOME/applications/sc7-rack-settings.desktop"
rm -f "$XDG_CONFIG_HOME/autostart/sc7-rack.desktop"

# 3. Remove icons
echo "Removing icons..."
HICOLOR="$XDG_DATA_HOME/icons/hicolor"
rm -f "$HICOLOR/scalable/apps/sc7-rack.svg"
for s in 512 256 128 64 48 32; do
    rm -f "$HICOLOR/${s}x${s}/apps/sc7-rack.png"
done

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t "$HICOLOR" 2>/dev/null || true
fi

# 4. Remove binaries / symlinks
echo "Removing binaries and symlinks..."
for bindir in "$XDG_BIN_HOME" "$HOME/bin"; do
    for name in sc7-rack sc7-rack-settings sc7-clipboard-bridge sc7-rack-open-host sc7-rack-files rack; do
        target="$bindir/$name"
        source_name="$name"
        [[ "$name" == rack ]] && source_name=sc7-rack
        if [[ -L "$target" && "$(readlink -f -- "$target")" == "$SCRIPT_DIR/bin/$source_name" ]]; then
            rm -f "$target"
        fi
    done
done

# 5. Remove sudoers rule if present
SUDOERS="/etc/sudoers.d/sc7-intel-gpu-top"
if [[ -f "$SUDOERS" ]] && sudo -n true 2>/dev/null; then
    echo "Removing sudoers rule $SUDOERS..."
    sudo rm -f "$SUDOERS" 2>/dev/null || true
fi

# 6. Clean runtime directory
rm -rf "$RUNTIME_DIR/sc7-rack" 2>/dev/null || true

# 7. Config files / Purge
CONFIG_DIR="$XDG_CONFIG_HOME/sc7-rack"
if [[ "$PURGE" == "true" ]]; then
    echo "Purging configuration directory $CONFIG_DIR..."
    rm -rf "$CONFIG_DIR"
    echo "✓ SC7 Rack completely uninstalled and purged."
else
    if [[ -d "$CONFIG_DIR" ]]; then
        echo "Preserved configuration at $CONFIG_DIR (use --purge to remove)."
    fi
    echo "✓ SC7 Rack uninstalled successfully."
fi
