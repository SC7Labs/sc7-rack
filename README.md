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
Use a normal user terminal on **Pop!_OS 24.04 with the COSMIC Wayland desktop**.
You need internet access and permission to install packages with `sudo`.
On a fresh system, install Git, clone the repository, then run Rack's installer:

```bash
sudo apt update
sudo apt install -y git
git clone https://github.com/SC7Labs/sc7-rack.git
cd sc7-rack
./install.sh
./bin/sc7-rack
```

Run `./install.sh` without `sudo`; it asks for your password when system packages
are missing. No Git name, email, signing key, or manual development-package setup
is required. Keep the clone in place: the installed launchers use its local builds.

This will:
1. Install missing runtime tools, COSMIC application packages when the desktop is present, and the Python Tkinter dependency for graphical Settings. If COSMIC System Monitor is older than 1.9.0, upgrade it from the configured Pop!_OS packages so the Dashboard shows its CPU, Memory, and Disk cards first.
2. Install missing build requirements, including C headers and EGL/GLES2/GBM development packages, and verify them before compiling. Build patched wlroots locally for SC7Labs DnD and compositor lifecycle, then build a pinned local Sway with the popup lifecycle fix. System wlroots and `/usr/bin/sway` are **never replaced**. Rerunning `./install.sh` resumes verified partial builds.
3. Rebuild `sc7-clipboard-bridge` from source for the current machine.
4. Link binaries to `~/.local/bin` and `~/bin` (`sc7-rack`, `sc7-rack-settings`, `sc7-clipboard-bridge`).
5. Setup Sway configuration in `~/.config/sc7-rack/`.
6. Install FreeDesktop hicolor application icons and `.desktop` launchers for COSMIC App Library.
7. Configure the `rack` shell alias in existing Bash/Zsh settings, preserving a user-defined alias.

The COSMIC App Library entries work immediately, even if your current session
does not yet include `~/.local/bin` in `PATH`. Use `./bin/sc7-rack` from the clone
for the first terminal launch; open a new terminal to load the `rack` alias.
GPU monitoring utilities are optional: Rack keeps all four panes when none is installed.

After installation, verify the local compositor with:

```bash
./scripts/bootstrap-sway.sh --check
```

To update or retry an interrupted installation, run `./install.sh` again.
Existing user settings are preserved.
The installer checks the actual `cosmic-monitor` executable after the package
step and stops with an explanation if version 1.9.0 or newer is unavailable.
See [Monitor version parity](docs/MONITOR_VERSION_PARITY.md) for the laptop2
retest.

> **Note:** SC7Labs Rack targets the **COSMIC Desktop Environment on Wayland** (System76 COSMIC).
> Generic Ubuntu/Wayland support is not guaranteed.

The clean build and fresh-user installer checks are recorded in
[`docs/FRESH_INSTALL_VALIDATION.md`](docs/FRESH_INSTALL_VALIDATION.md).


### Launching SC7Labs Rack
Launch via the COSMIC App Menu by clicking **SC7Labs Rack**, or from terminal:
```bash
sc7-rack
# or simply:
rack
```

Rack currently keeps the accelerated GLES2 renderer with explicit EGL DMA-BUF
modifiers disabled inside its nested compositor. **Corruption has still occurred
on laptop2 with this setting active.** `WLR_RENDERER=pixman ./bin/sc7-rack`
is the known-clean control there. Full-output repaint also failed the physical
GLES2 test. Fresh input, target and output experiments also corrupted, with
corruption already present before host submission. Experimental
[input diagnostics](docs/RENDER_INPUT_DIAGNOSTICS.md) now compare each sampled
client texture with the final composed frame and trace actual buffer releases.
The physical A/B evidence and resize test are recorded in
[`docs/RENDERER_COMPATIBILITY.md`](docs/RENDERER_COMPATIBILITY.md).

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
