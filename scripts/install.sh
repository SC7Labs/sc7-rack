#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
echo "== Installing SC7 Rack from '$SCRIPT_DIR' =="

mkdir -p "$HOME/bin" "$HOME/.local/bin" "$HOME/.config/sc7-rack" "$HOME/.local/share/applications"

# 1. Build clipboard bridge
echo "Building clipboard bridge..."
make -C "$SCRIPT_DIR/bridge"

# 2. Setup sudo rule for intel_gpu_top if passwordless sudo is configured
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

# 3. Copy/link config files
cp "$SCRIPT_DIR/config/config" "$HOME/.config/sc7-rack/config"
cp "$SCRIPT_DIR/config/inner.sh" "$HOME/.config/sc7-rack/inner.sh"
chmod +x "$HOME/.config/sc7-rack/inner.sh"

if [[ ! -f "$HOME/.config/sc7-rack/settings.json" ]]; then
    cp "$SCRIPT_DIR/settings/settings.json" "$HOME/.config/sc7-rack/settings.json"
fi

# 4. Link binaries to ~/bin and ~/.local/bin
for bindir in "$HOME/bin" "$HOME/.local/bin"; do
    mkdir -p "$bindir"
    ln -sf "$SCRIPT_DIR/bin/sc7-rack" "$bindir/sc7-rack"
    ln -sf "$SCRIPT_DIR/bin/sc7-rack-settings" "$bindir/sc7-rack-settings"
    ln -sf "$SCRIPT_DIR/bin/sc7-clipboard-bridge" "$bindir/sc7-clipboard-bridge"
done

# 5. Install icons into hicolor theme
HICOLOR="$HOME/.local/share/icons/hicolor"
mkdir -p "$HICOLOR/scalable/apps"
cp "$SCRIPT_DIR/assets/icons/sc7-rack.svg" "$HICOLOR/scalable/apps/sc7-rack.svg"

for s in 512 256 128 64 48 32; do
    size_dir="$HICOLOR/${s}x${s}/apps"
    mkdir -p "$size_dir"
    png_file="$SCRIPT_DIR/assets/icons/sc7-rack-${s}.png"
    if [[ -f "$png_file" ]]; then
        cp "$png_file" "$size_dir/sc7-rack.png"
    fi
done

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t "$HICOLOR" 2>/dev/null || true
fi

# 6. Install .desktop launchers
cp "$SCRIPT_DIR/desktop/sc7-rack.desktop" "$HOME/.local/share/applications/sc7-rack.desktop"
cp "$SCRIPT_DIR/desktop/sc7-rack-settings.desktop" "$HOME/.local/share/applications/sc7-rack-settings.desktop"
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true
fi

# 7. Add shell alias
for rc in "$HOME/.zshrc" "$HOME/.bashrc"; do
    if [[ -f "$rc" ]] && ! grep -qF "alias rack=" "$rc" 2>/dev/null; then
        printf "\nalias rack='\$HOME/bin/sc7-rack'\n" >> "$rc"
    fi
done

echo "SC7 Rack installed successfully."
echo "Launch via COSMIC App Menu: 'SC7 Rack' or 'SC7 Rack Settings'"
echo "CLI Commands:"
echo "  sc7-rack             - Launch SC7 Rack"
echo "  sc7-rack-settings    - Manage settings (GUI / CLI)"
echo "  rack                 - Shell shortcut"
