# Deep Investigation & Architectural Report: Drag-and-Drop Across Nested Wayland Compositors

## Executive Summary

Cross-compositor Drag-and-Drop (DnD) between the host COSMIC desktop and the nested SC7 Rack (Sway/wlroots) cannot function with stock Sway and stock wlroots without patching the compositor backend.

While selection synchronization (Ctrl+C / Ctrl+V text and file transfers) operates out-of-band via data control protocols (`zwlr_data_control_manager_v1`), Wayland Drag-and-Drop is an interactive, pointer-grabbed protocol inherently tied to the Wayland seat, input event serials, and window surface hierarchies.

Stock wlroots' Wayland backend (`backend/wayland`) does not bind the host's `wl_data_device_manager`, does not proxy DnD sessions across the parent surface boundary, and does not forward implicit seat grab serials.

This document details the exact protocol mechanisms, source-code blockers in wlroots and Sway, and proposes the minimal architecture change required to enable transparent cross-compositor DnD in a future release.

---

## 1. Wayland Drag-and-Drop Protocol Fundamentals

Drag-and-Drop in Wayland is governed by `wl_data_device`, `wl_data_source`, and `wl_data_offer` (`wayland.xml`).

### Protocol Flow:
1. **Initiation (`start_drag`)**:
   ```xml
   <request name="start_drag">
     <arg name="source" type="object" interface="wl_data_source" allow-null="true"/>
     <arg name="origin" type="object" interface="wl_surface"/>
     <arg name="icon" type="object" interface="wl_surface" allow-null="true"/>
     <arg name="serial" type="uint" summary="serial number of the implicit grab on the origin"/>
   </request>
   ```
   - A drag **must** be initiated in response to a user input event (such as `wl_pointer.button` press).
   - The client must pass the exact `serial` of that button press.
   - The compositor verifies that the client holds an active implicit grab on the `origin` surface with that `serial`.
2. **Motion and Enter (`enter`, `motion`)**:
   - As the pointer moves across surfaces, the compositor sends `wl_data_device.enter` with a `wl_data_offer` to the surface underneath the pointer.
   - Pointer motion events are delivered as `wl_data_device.motion` rather than normal `wl_pointer.motion`.
3. **Drop and Data Transfer (`drop`, `receive`, `send`)**:
   - Releasing the button sends `wl_data_device.drop` to the destination client.
   - The destination client calls `wl_data_offer.receive(mime_type, fd)`.
   - The origin client receives `wl_data_source.send(mime_type, fd)` and writes the data to the pipe.
   - Finally, `wl_data_offer.finish()` and `wl_data_source.dnd_finished()` close the session.

---

## 2. Why Clipboard Bridging Succeeds But DnD Does Not

Clipboard operations succeed because Wayland provides privileged data-control extensions (`zwlr_data_control_manager_v1` and `ext_data_control_manager_v1`). These protocols allow an authorized bridge daemon to:
- Monitor selection changes without possessing keyboard focus or pointer grabs.
- Announce data sources and set seat selections asynchronously.
- Connect file descriptors directly between sender and receiver via unix pipes.

**Crucially, neither `zwlr_data_control_manager_v1` nor `ext_data_control_manager_v1` includes any mechanism for Drag-and-Drop.** Both extensions explicitly restrict their interface to `set_selection` and `set_primary_selection`. Wayland has no data-control protocol for DnD because DnD is inherently synchronized with live pointer grabs and window surfaces.

---

## 3. The Root Blockers in Stock Nested Sway & wlroots

### Blocker 1: wlroots Wayland Backend Ignores `wl_data_device_manager`
Inspection of `wlroots/backend/wayland/backend.c` reveals that wlroots binds the following parent globals in `registry_global`:
- `wl_compositor`
- `wl_seat`
- `xdg_wm_base`
- `zxdg_decoration_manager_v1`
- `zwp_pointer_gestures_v1`
- `wp_presentation`
- `zwp_tablet_manager_v2`
- `zwp_linux_dmabuf_v1`
- `zwp_relative_pointer_manager_v1`
- `wl_drm`
- `wl_shm`
- `xdg_activation_v1`
- `wl_subcompositor`
- `wp_viewporter`

**`wl_data_device_manager` is completely absent.** The wlroots Wayland backend treats the outer window strictly as a rendering canvas and input consumer. It never requests or manages a `wl_data_device` on the host compositor.

### Blocker 2: Outgoing Drag Serialization & Surface Mismatch
When a user clicks a file inside nested COSMIC Files and starts dragging:
1. Nested COSMIC Files calls `wl_data_device.start_drag(source, origin, icon, serial)`.
2. Sway validates `origin` (a nested surface) and `serial` (from Sway's virtual seat).
3. Sway creates an internal drag grab on `seat0` inside Sway.
4. When the user drags the mouse outside the SC7 Rack window:
   - Nested Sway receives pointer leave events from the host.
   - The host compositor (COSMIC) sees a normal cursor moving over desktop windows with no active drag session from SC7 Rack.
   - In order for COSMIC to start a drag, SC7 Rack would have had to invoke `wl_data_device.start_drag` on the host compositor using the host's surface and the host's button press serial.
   - By the time nested COSMIC Files decides to start a drag, the host button press serial has already expired or was never bound to a host `start_drag` request.

### Blocker 3: Incoming Drag Ingestion
When a user drags a file from the host desktop into the SC7 Rack window:
1. Host COSMIC sends `wl_data_device.enter` to the SC7 Rack window surface.
2. Because the wlroots backend has no `wl_data_device` listener, this event is either discarded or not delivered.
3. Even if delivered, Sway has no mechanism to translate a host `wl_data_offer` into an internal virtual `wlr_drag` and synthesize `enter`/`motion`/`drop` events to the nested child window underneath the virtual pointer.

---

## 4. Minimum Architecture Change Required to Enable Cross-Compositor DnD

To support true drag-and-drop across the rack boundary without abandoning the nested compositor architecture, the following upstream/downstream changes would be necessary:

### 1. wlroots Wayland Backend Patch (`backend/wayland`):
- Bind `wl_data_device_manager` on the parent connection.
- Instantiate a parent `wl_data_device` for each seat.
- Cache the latest pointer button press serial received from the parent compositor.
- Expose a `wlr_wl_backend_start_parent_drag(backend, wlr_drag, parent_serial)` hook.

### 2. Nested Sway Hook:
- When Sway receives `wlr_seat_request_start_drag`:
  - Along with starting the internal drag grab, call into the backend to start a corresponding drag on the host window surface.
  - Proxy `wlr_data_source` MIME types to a parent `wl_data_source`.
- When the pointer moves outside the nested boundary, the host compositor continues the drag session transparently.
- When the parent compositor delivers a drop event on the SC7 Rack surface, Sway maps the parent drop to the focused internal surface and streams the pipe.

### 3. Alternative Clean Room Approach:
- A custom Wayland proxy / bridge layer between the host compositor and Sway's socket that sits on the Wayland IPC stream and translates `wl_data_device` events bidirectionally.

---

## 5. Conclusion for v0.1.0

Because stock nested Sway and wlroots lack `wl_data_device` support in their Wayland backend:
- Transparent mouse drag-and-drop across the window boundary requires a patched wlroots build.
- **Selection bridging (Ctrl+C / Ctrl+V text and file transfers) is fully working, native, and production-ready in SC7 Rack v0.1.0.**
- Drag-and-drop inside the rack (e.g., between nested Files and another nested target) functions normally within the nested compositor boundary.
