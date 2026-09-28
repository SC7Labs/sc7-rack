# SC7Labs Rack

An integrated developer workstation rack for the **COSMIC Desktop Environment**.

SC7Labs Rack presents **one outer COSMIC window** containing a native, nested Wayland compositor (**Sway / wlroots**) tiling four essential developer views in a crisp 2x2 grid:

```
+-----------------------------------+-----------------------------------+
| Top-Left: GPU Diagnostics         | Top-Right: Workspace Files        |
| intel_gpu_top (or custom provider)| COSMIC Files ($HOME)              |
+-----------------------------------+-----------------------------------+
| Bottom-Left: System Monitor       | Bottom-Right: Process Monitor     |
| COSMIC Monitor                    | htop                              |
+-----------------------------------+-----------------------------------+
```

---

## Key Features

1. **Native 2x2 Tiled Grid in a Single Window**
   - The entire rack is treated as a single window by the host COSMIC compositor.
   - Resizing, maximizing, moving workspaces, or minimizing preserves the exact 50/50 quadrant split without any position polling loops or hacks.
2. **App-by-App Startup Experience**
   - Each tool tiles sequentially on startup with zero jitter.
3. **Seamless Bidirectional Clipboard Bridge (`sc7-clipboard-bridge`)**
   - Out-of-the-box copy/paste between host COSMIC applications and rack apps.
   - Text copy into and out of the rack.
   - Full file copy/paste: copy files in nested COSMIC Files (`Ctrl+C`) and paste in host COSMIC Files (`Ctrl+V`), and vice-versa.
   - Rich MIME support: preserves `text/plain`, `text/plain;charset=utf-8`, `text/uri-list`, `image/png`, and custom formats.
   - True zero-copy: pipes are directly plumbed across compositors via Wayland file descriptor passing (`SCM_RIGHTS`).
4. **Security & Privacy by Design**
   - **Zero logging of clipboard payloads**: The bridge never inspects, parses, or logs clipboard data.
   - **Zero disk persistence**: Data streams entirely through ephemeral kernel pipe buffers in RAM.
   - **User control**: Clipboard sharing can be enabled or disabled at any time via settings.
5. **Configurable Settings & Autostart**
   - Configure start-at-login (`[x] Start SC7 Rack at login`).
   - Toggle clipboard integration (`[x] Share clipboard with desktop`).
   - Customize default Files folder path (defaults to `$HOME`).
   - Select GPU diagnostics provider (`auto`, `intel`, `nvidia`, `amd`, `custom`).

---

## Architecture

```
Host Desktop: System76 COSMIC Desktop Environment (smithay)
  └─ Host Wayland Socket: $WAYLAND_DISPLAY (dynamically discovered)
       │
       ├─ SC7 Rack Outer Window (wlroots Wayland Backend)
       │    └─ Nested Sway Compositor ($WAYLAND_DISPLAY)
       │         ├─ Top-Left: GPU monitor (in cosmic-term)
       │         ├─ Top-Right: cosmic-files ($FILES_PATH, defaults to $HOME)
       │         ├─ Bottom-Left: cosmic-monitor
       │         └─ Bottom-Right: htop (in cosmic-term)
       │
       └─ SC7 Clipboard Bridge Daemon (sc7-clipboard-bridge)
            ├─ Connects to Host ($SC7_HOST_WAYLAND_DISPLAY) via zwlr_data_control_manager_v1
            └─ Connects to Rack ($WAYLAND_DISPLAY) via zwlr_data_control_manager_v1
```

---

## Clipboard & File Transfer Integration

### Text Copy / Paste
- **Rack -> Host**: Select text in nested terminal or editor -> `Ctrl+Shift+C` -> paste with `Ctrl+V` into any host application.
- **Host -> Rack**: Copy text in host browser, terminal, or document -> `Ctrl+V` inside nested rack terminal or files.

### File Copy / Paste
- **Rack -> Host**: Inside nested COSMIC Files, select file(s) -> `Ctrl+C` -> click host COSMIC Files window or desktop -> `Ctrl+V`. Files are copied/transferred normally via standard `text/uri-list` protocol.
- **Open files**: Double-click a document in Rack's COSMIC Files to open its associated application on the host desktop. You can move that application between monitors; COSMIC Files stays in Rack.
- **Host -> Rack**: In host COSMIC Files, select file(s) -> `Ctrl+C` -> click nested COSMIC Files -> `Ctrl+V`.

---

## Drag-and-Drop (DnD) Across the Rack Boundary

A deep technical audit was conducted regarding cross-compositor Drag-and-Drop between the host COSMIC desktop and the nested rack window.

### Findings Summary
- Wayland Drag-and-Drop (`wl_data_device.start_drag`) is strictly bound to pointer seat grabs, window surfaces, and input event serials.
- Neither `zwlr_data_control_manager_v1` nor `ext_data_control_manager_v1` support DnD (by design, data-control protocols manage only clipboard and primary selection).
- Stock wlroots' Wayland backend (`backend/wayland/backend.c`) does **not** bind the host's `wl_data_device_manager` and has no proxying logic for DnD sessions.
- As a result, transparent cross-boundary DnD cannot work with stock Sway/wlroots without maintaining a custom wlroots fork.
- Full details, protocol traces, and proposed upstream architectural patches are documented in [`docs/DND_RESEARCH.md`](docs/DND_RESEARCH.md).

---

## Installation & Quickstart

### Installation
Clone the repository and run the installer script:
```bash
git clone https://github.com/SC7Labs/sc7-rack.git
cd sc7-rack
./install.sh
```

This will:
1. Check mandatory dependencies (`sway`, `swaymsg`, COSMIC desktop components, `htop`, `python3`).
2. Check the pinned wlroots build requirements. On Pop!_OS, Ubuntu, and Debian, the installer uses `sudo` to install only missing required apt packages, then checks them again. Build the patched wlroots locally for SC7Labs DnD and compositor lifecycle. System wlroots and `/usr/bin/sway` are **never replaced**. Rerunning `./install.sh` resumes a verified partial build.
3. Build `sc7-clipboard-bridge` from clean C source.
4. Link binaries to `~/.local/bin` and `~/bin` (`sc7-rack`, `sc7-rack-settings`, `sc7-clipboard-bridge`).
5. Setup Sway configuration in `~/.config/sc7-rack/`.
6. Install FreeDesktop hicolor application icons and `.desktop` launchers for COSMIC App Library.
7. Configure the `rack` shell alias.

> **Note:** SC7Labs Rack targets the **COSMIC Desktop Environment on Wayland** (System76 COSMIC).
> Generic Ubuntu/Wayland support is not guaranteed.


### Launching SC7Labs Rack
Launch via the COSMIC App Menu by clicking **SC7Labs Rack**, or from terminal:
```bash
sc7-rack
# or simply:
rack
```

### Exiting
Press **`Super+Shift+Q`** to close the entire rack window and terminate all nested processes cleanly.

---

## Settings & Status

SC7Labs Rack includes a dedicated settings utility `sc7-rack-settings`:

### View Status
```bash
sc7-rack-settings --status
```
Example Output:
```
========================================
        SC7Labs Rack Settings
========================================
Status:             Running (PID 2133265)
[ ] Start at login:     Disabled
[x] Share clipboard:    Enabled
Files path:         /home/user
GPU Provider:       auto
----------------------------------------
Clipboard Bridge:   Connected (PID 2133295)
Host Wayland:       $HOST_DISPLAY (host socket)
Rack Wayland:       $RACK_DISPLAY (nested socket)
========================================
```

### CLI Configuration
```bash
# Toggle / configure autostart at login
sc7-rack-settings --autostart on
sc7-rack-settings --autostart off

# Toggle / configure desktop clipboard bridge
sc7-rack-settings --share-clipboard on
sc7-rack-settings --share-clipboard off

# Change default Files path
sc7-rack-settings --files-path ~/projects

# Select GPU monitor provider
sc7-rack-settings --gpu-provider intel
sc7-rack-settings --gpu-provider auto
sc7-rack-settings --gpu-provider custom --gpu-command "nvtop"
```

### GUI Dialog
Launch via the COSMIC App Menu by clicking **SC7 Rack Settings**, or from terminal:
```bash
sc7-rack-settings --gui
```

---

## GPU Diagnostics & Hardware Providers

The top-left quadrant hosts hardware GPU diagnostics. SC7 Rack supports configurable GPU providers:

| Provider | Command / Tool | Status |
|---|---|---|
| `auto` | Auto-detects Intel / NVIDIA / AMD | Default (recommended) |
| `intel` | `sudo -n intel_gpu_top` | **Hardware tested & validated** (Intel Arc) |
| `nvidia` | `nvidia-smi -l 1` or `nvtop` | Provider abstraction ready (future validation required) |
| `amd` | `radeontop` | Provider abstraction ready (future validation required) |
| `custom` | Configurable via `--gpu-command` | Supported |

---

## Acceptance Verification Suite

Run the automated test suite covering all 41 acceptance gates:
```bash
cd sc7-rack
./tests/acceptance_tests.sh
```
All 41 gates are verified automated live:
- Gates 1–20: Base architecture, layout, binaries, narrow sudo rule, no cheat tools.
- Gates 21–22: Bidirectional text copy/paste.
- Gates 23–24: Bidirectional file copy/paste (`text/uri-list`).
- Gate 25: Rich MIME preservation.
- Gate 26: Zero clipboard payload logging.
- Gate 27: Feedback loop suppression under rapid copying.
- Gate 28: Large payload streaming (5MB+) without hanging.
- Gates 29–30: Clean lifecycle termination and automatic re-establishment.
- Gates 31–33: Drag-and-drop investigation, blocker documentation, and architecture report.
- Gate 34: Complete COSMIC Files feature parity verification (see [`docs/COSMIC_FILES_PARITY.md`](docs/COSMIC_FILES_PARITY.md)).
- Gates 35–40: **Source reproducibility** — pinned wlroots base SHA, tracked patch, bootstrap script, DnD markers, launcher LD_LIBRARY_PATH, built library present.
- Gate 41: Host opening bridge regression tests, including direct MIME launches from COSMIC Files.
