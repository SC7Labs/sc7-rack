#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
echo "== Installing SC7 Rack from '$SCRIPT_DIR' =="

mkdir -p "$HOME/bin" "$HOME/.config/sc7-rack"

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

# 4. Link binaries
ln -sf "$SCRIPT_DIR/bin/sc7-rack" "$HOME/bin/sc7-rack"
ln -sf "$SCRIPT_DIR/bin/sc7-rack-settings" "$HOME/bin/sc7-rack-settings"
ln -sf "$SCRIPT_DIR/bin/sc7-clipboard-bridge" "$HOME/bin/sc7-clipboard-bridge"

# 5. Add shell alias
for rc in "$HOME/.zshrc" "$HOME/.bashrc"; do
    if [[ -f "$rc" ]] && ! grep -qF "alias rack=" "$rc" 2>/dev/null; then
        printf "\nalias rack='\$HOME/bin/sc7-rack'\n" >> "$rc"
    fi
done

echo "SC7 Rack installed successfully."
echo "Binaries available in ~/bin:"
echo "  sc7-rack"
echo "  sc7-rack-settings"
echo "  sc7-clipboard-bridge"
