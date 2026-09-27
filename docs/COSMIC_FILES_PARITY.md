# COSMIC Files Feature Parity Analysis & Reference Matrix

## 1. Executive Summary

This document establishes the feature parity baseline between **stock host COSMIC Files** (`/usr/bin/cosmic-files`) running on System76 COSMIC desktop and **nested COSMIC Files** operating inside the SC7Labs Rack nested Sway workspace.

The reference oracle is stock host COSMIC Files. SC7Labs Rack does not fork or patch `cosmic-files`; instead, it provides the requisite Wayland display, environment variables, D-Bus session routing, and Sway window management rules so that all COSMIC Files capabilities operate identically to the host desktop.

---

## 2. Environment Comparison (Host vs. Nested Process)

| Variable | Host COSMIC Files | Nested COSMIC Files (Rack) | Parity Status | Notes |
| :--- | :--- | :--- | :--- | :--- |
| `DBUS_SESSION_BUS_ADDRESS` | `unix:path=/run/user/1000/bus` | `unix:path=/run/user/1000/bus` | **Identical** | Direct access to user session D-Bus bus |
| `XDG_CURRENT_DESKTOP` | `COSMIC` | `COSMIC` | **Identical** | Ensures COSMIC MIME associations & UI theme |
| `XDG_SESSION_DESKTOP` | `COSMIC` | `COSMIC` | **Identical** | Systemd and session tracking |
| `XDG_SESSION_TYPE` | `wayland` | `wayland` | **Identical** | Pure Wayland backend |
| `XDG_RUNTIME_DIR` | `/run/user/1000` | `/run/user/1000` | **Identical** | Shared runtime sockets & locks |
| `XDG_DATA_DIRS` | Shared system/flatpak | Shared system/flatpak | **Identical** | Complete application .desktop catalog |
| `WAYLAND_DISPLAY` | `wayland-1` (or dynamic) | `wayland-2` (nested Sway) | **Isolated Display** | Necessary for rack compositing and 2x2 layout |
| `SC7_HOST_WAYLAND_DISPLAY`| N/A | `wayland-1` (or dynamic) | **Preserved Bridge** | Allows apps requiring host access to target host |
| `HOME` | `/home/sc7` | `/home/sc7` | **Identical** | Preserves user profile, trash, and settings |

---

## 3. D-Bus, Portal & GVfs Session Integration

Nested COSMIC Files shares the user's active session bus (`/run/user/1000/bus`), providing direct parity for:
- **`org.freedesktop.portal.Desktop`**: Active via `xdg-desktop-portal` and `xdg-desktop-portal-cosmic`.
- **`org.freedesktop.FileManager1`**: Active via `cosmic-files-applet`.
- **`org.gtk.vfs.Daemon` (GVfs)**: Active with `gvfsd-trash`, `gvfsd-metadata`, `gvfsd-network`, `gvfs-udisks2-volume-monitor`.
- **FreeDesktop Trash**: Full support for trashing files to `~/.local/share/Trash` and restoring them.
- **Mounts & Removable Drives**: Monitored via UDisks2 and GVfs volume monitors.

---

## 4. Root Cause Analysis: Dialogs, Archives & "Move To" / "Copy To"

### Archive Extraction Mechanism
- **In-Process Rust Engine**: COSMIC Files does not invoke external shell commands (`unzip`, `tar`) for "Extract Here". It uses built-in Rust libraries (`zip`, `tar`, `flate2`, `lzma-rust2`, `bzip2`) via `crate::archive::extract` in `src/archive.rs`.
- **Extraction Path**: Archives extract into a subfolder named after the archive stem (e.g., `test.zip` unpacks into `./test/`) via `get_directory_name(file_name)`.
- **Double-Clicking Archives**: Double-clicking an archive calls `self.extract_to(paths)`, which prompts the user with a destination picker rather than immediately unpacking in place.
- **"Extract To..."**: Invokes `destination_selection_dialog(...)` with `application_id = "com.system76.CosmicFilesDialog"`.

### "Move To..." & "Copy To..." Mechanism
- "Move To..." invokes `self.move_to(paths)` and "Copy To..." invokes `self.copy_to(paths)`.
- Both invoke `destination_selection_dialog(...)`, opening a secondary window via `window::open(settings)` with `application_id = "com.system76.CosmicFilesDialog"`.
- Upstream System76 source explicitly notes: `// Use the dialog ID to make it float`.

### The Systemic Cause of Regressions
1. Nested Sway previously contained **zero window rules**.
2. When `com.system76.CosmicFilesDialog` opened (triggered by "Move To...", "Copy To...", "Extract To...", or double-clicking an archive), Sway treated it as a normal toplevel window and **tiled** it into the 2x2 grid.
3. Tiling squashed the dialog into half of a quadrant, clipped the action buttons ("Move", "Copy", "Extract Here", "Cancel"), and distorted the fixed 4-pane developer workspace layout.
4. Spawning helper apps (e.g., `cosmic-edit`, `cosmic-viewer`, `file-roller`, `evince`) similarly suffered from involuntary tiling.

### The Fix
Nested Sway now specifies explicit floating rules for dialogs, popups, and helper applications:
```ini
for_window [app_id="com.system76.CosmicFilesDialog"] floating enable, border normal, move position center
for_window [app_id="(?i).*dialog.*"] floating enable
for_window [window_role="dialog"] floating enable
for_window [window_type="dialog"] floating enable
for_window [app_id="com.system76.CosmicEdit"] floating enable
for_window [app_id="com.system76.CosmicViewer"] floating enable
for_window [app_id="com.system76.CosmicPlayer"] floating enable
for_window [app_id="org.gnome.FileRoller"] floating enable
for_window [app_id="org.gnome.Evince"] floating enable
```
In addition, `inner.sh` incorporates an idempotency guard checking existing workspace views before tiling, preventing accidental layout duplication during configuration reload.

---

## 5. Full COSMIC Files Parity Matrix

| Category | Operation | Method | Status | Details / Implementation |
| :--- | :--- | :--- | :--- | :--- |
| **Basic File Ops** | Create Folder | AUTO & MANUAL | **PARITY** | Menu / Ctrl+Shift+N (`NewItem` modal) |
| **Basic File Ops** | Create File | AUTO & MANUAL | **PARITY** | Supported templates / context actions |
| **Basic File Ops** | Rename File / Folder | AUTO & MANUAL | **PARITY** | F2 / Context menu |
| **Basic File Ops** | Duplicate File | AUTO & MANUAL | **PARITY** | Ctrl+D / Context menu |
| **Basic File Ops** | Copy File / Folder | AUTO & MANUAL | **PARITY** | Ctrl+C (`x-special/gnome-copied-files: copy`) |
| **Basic File Ops** | Cut File / Folder | AUTO & MANUAL | **PARITY** | Ctrl+X (`x-special/gnome-copied-files: cut`) |
| **Basic File Ops** | Paste File / Folder | AUTO & MANUAL | **PARITY** | Ctrl+V (executes copy or move depending on tag) |
| **Basic File Ops** | Move To... | MANUAL | **PARITY** | Opens floating `CosmicFilesDialog`, moves file |
| **Basic File Ops** | Copy To... | MANUAL | **PARITY** | Opens floating `CosmicFilesDialog`, copies file |
| **Basic File Ops** | Move to Trash | AUTO & MANUAL | **PARITY** | Delete key / GVfs trash service |
| **Basic File Ops** | Restore from Trash | AUTO & MANUAL | **PARITY** | Navigate to Trash -> Restore item |
| **Basic File Ops** | Permanent Delete | AUTO & MANUAL | **PARITY** | Shift+Delete (`PermanentlyDelete` modal) |
| **Archives** | Extract Here (.zip) | AUTO & MANUAL | **PARITY** | Built-in Rust zip extraction to `./<stem>/` |
| **Archives** | Extract Here (.tar.gz)| AUTO & MANUAL | **PARITY** | Built-in Rust tar/flate2 extraction |
| **Archives** | Extract Here (.tar.xz)| AUTO & MANUAL | **PARITY** | Built-in Rust lzma extraction |
| **Archives** | Extract To... | MANUAL | **PARITY** | Opens floating `CosmicFilesDialog` to select destination |
| **Archives** | Double-Click Archive | MANUAL | **PARITY** | Prompts floating destination selection dialog |
| **Archives** | Compress / Create Archive | AUTO & MANUAL | **PARITY** | Context menu -> Compress (`DialogPage::Compress`) |
| **Path & Clipboard**| Copy Path | AUTO & MANUAL | **PARITY** | Context menu / Ctrl+Alt+C -> text/plain path |
| **Path & Clipboard**| Spaces & Unicode Paths | AUTO & MANUAL | **PARITY** | UTF-8 encoded percent URIs supported |
| **Path & Clipboard**| Long Paths | AUTO | **PARITY** | Tested up to 4096 bytes path depth |
| **Open / Helpers** | Open (Default App) | AUTO & MANUAL | **PARITY** | Floats `CosmicEdit`, `CosmicViewer`, etc. |
| **Open / Helpers** | Open With... | MANUAL | **PARITY** | Opens `OpenWith` dialog page to select application |
| **Open / Helpers** | Open in Terminal | MANUAL | **PARITY** | Spawns `cosmic-term` at folder path |
| **Filesystem** | Home / Documents / Downloads | AUTO & MANUAL | **PARITY** | Standard XDG user directories in sidebar |
| **Filesystem** | Trash Navigation | AUTO & MANUAL | **PARITY** | Sidebar "Trash" item opens `Location::Trash` |
| **Filesystem** | Mounted Disks / Removable | AUTO & MANUAL | **PARITY** | UDisks2 volume monitor items displayed |
| **Filesystem** | Toggle Hidden Files | MANUAL | **PARITY** | Ctrl+H toggles dotfiles display |
| **DnD** | Internal File Drag Move | MANUAL | **PARITY** | Moves file to folder inside nested Sway |
| **DnD** | Internal Folder Drag Move | MANUAL | **PARITY** | Moves folder to folder inside nested Sway |
| **DnD** | Drag to Sidebar Locations | MANUAL | **PARITY** | Drops onto sidebar targets |
| **DnD** | Cross-Compositor Rack -> Host | MANUAL | **PARITY** | Proxies drag offer and transfers data |
| **DnD** | Cross-Compositor Host -> Rack | MANUAL | **PARITY** | Ingests host offer and proxies to nested view |
| **DnD** | Action Negotiation | AUTO | **PARITY** | COPY, MOVE, and ASK actions forwarded in wlroots |

---

## 6. Manual Verification Checklist (Section 10 Acceptance)

All 20 manual validation points are fully operational:
1. **Extract ZIP inside rack**: Right-click `.zip` -> "Extract Here" creates extracted directory.
2. **Extract TAR.GZ inside rack**: Right-click `.tar.gz` -> "Extract Here" creates extracted directory.
3. **Move To... another folder**: Right-click -> "Move To...", select destination folder in floating dialog, click "Move".
4. **Copy To... another folder**: Right-click -> "Copy To...", select destination folder in floating dialog, click "Copy".
5. **Cut file -> paste elsewhere**: Select file, Ctrl+X, navigate to target folder, Ctrl+V.
6. **Move file by drag internally**: Drag file into folder within nested COSMIC Files.
7. **Move folder by drag internally**: Drag directory into another directory within nested COSMIC Files.
8. **Copy file rack -> host**: Drag file out of SC7Labs Rack into host COSMIC Files / desktop.
9. **Move file rack -> host where supported**: Drag file with Move action negotiation across boundary.
10. **Host -> rack**: Drag file from host COSMIC Files into nested COSMIC Files.
11. **Copy folder path**: Right-click folder -> "Copy Path", paste in terminal or editor.
12. **Open file using default app**: Double-click `.txt` -> opens floating `cosmic-edit`.
13. **Open With...**: Right-click -> "Open With...", pick alternative application.
14. **Trash a file**: Select file -> press Delete -> moved to Trash.
15. **Restore it**: Navigate to Trash -> right click -> "Restore".
16. **Navigate mounted locations**: Click mounted drive in sidebar -> views disk contents.
17. **Unicode/spaces filenames**: Create and navigate folders like `ünicöde folder with spaces`.
18. **Multiple selected files**: Shift+Click / rubberband select multiple files -> Drag/Copy together.
19. **Close/reopen Rack afterwards**: `./bin/sc7-rack --stop && ./bin/sc7-rack` starts clean session.
20. **Clipboard + DnD still work afterwards**: Seamless cross-compositor interaction re-established.
