# Client input capture and ownership audit

## What laptop2 has established

The physical result at `eb5085ac40bcadbf52896b977b349d3d4689bf52` is:

| Diagnostic | Result |
| --- | --- |
| Ordinary GLES2, including without explicit modifiers | Corrupt |
| Full output repaint | Corrupt |
| Fresh client texture/import | Corrupt |
| Fresh render target identity | Corrupt |
| Fresh output storage | Corrupt |
| Pixman | Clean |

In `run-20261003-200458.N8qgREVm`, frame 447 contains the corruption in both
the desktop screenshot and the composed capture taken before output submit.
Its 1214×624 output has generation 596, age zero, a new DMA-BUF and EGLImage,
FBO 3, and full damage. Ordinary output storage reuse cannot explain this
capture. Investigation now concerns client contents, imported texture sampling,
and composition before host submission.

The Monitor source in that frame is PID 28318, surface `0x5d5a3185bf50`,
597×307, linear ARGB8888 DMA-BUF, source generation 484, texture
`0x5d5a3184ce40`, GL texture 12, EGLImage `0x5d5a31700150`, destination
`5,312,597,307`. The preceding `texture-update ok=0` is expected: wlroots 0.17.4
rejects in-place updates of immutable DMA-BUF textures and creates/imports a
replacement. It is not evidence of a failed import.

These are physical observations on AMD A6-5200/Radeon HD hardware. The precise
defect remains unresolved; this change prepares the next discriminating test.

## One next laptop2 test

Close Rack, then use the existing clone from a terminal on the host desktop:

```bash
cd ~/sc7-rack
git pull --ff-only origin main
./install.sh
./scripts/run-render-diagnostic.sh --renderer gles2 --mode observe --capture-inputs
```

Repeat the outer-window resizing and Dashboard/CPU switching that causes the
failure. When corruption is visible, keep Monitor animating and run from
another **host** terminal:

```bash
cd ~/sc7-rack
./scripts/capture-render-frame.sh
```

Save the complete artifact directory printed by the runner, plus a desktop
screenshot while corruption is visible. This is one observation experiment;
no further renderer matrix is requested. The helper selects the live Rack PID
and exact IPC socket, verifies that the run directory belongs to that PID, and
converts only input captures bearing the composed frame's number.

## Captures and interpretation

With `--capture-inputs`, a request is selected at the start of a scene. A request
arriving after that point waits for the next complete scene. Immediately before
each real client texture is passed to wlroots' draw operation, the diagnostic:

1. Resolves the source owned by that exact texture, including saved resize
   textures. It does not substitute the surface's newest pending buffer or a
   historical SHM wrapper source.
2. Attempts a supported CPU snapshot of that source.
3. For GLES2, samples the **existing GL texture/EGLImage** into a temporary
   native-sized RGBA target and reads it. There is no new DMA-BUF import.
4. Restores the changed GLES state before the original draw proceeds.
5. Captures the final composed output before the real pass submits.

Input filenames contain frame, PID, surface resource ID, sequence, and
`raw`/`gles`. The manifest `pre-submit-FRAME-inputs.json` includes the input
images and corresponding trace records. Each record identifies live
`wl_buffer` ID and lifetime generation, source generation, DMA-BUF format,
modifier, planes/stride/offset, GL texture, EGLImage, and availability/error
reason. A destroyed protocol resource has ID zero, even if the buffer remains
alive for a saved resize view.

RGB PPMs preserve sampled channel values; `.ppm.alpha.pgm` preserves alpha.
Input PNGs combine these into RGBA without unpremultiplying the recorded RGB.
Use RGB and alpha together for comparisons: RGB beneath alpha zero contributes
no visible pixels, and a generic PNG viewer can darken premultiplied colors.
Inputs are captured at native size without pane cropping, scaling, transform,
blending or scene scissor. Those operations occur in the subsequent original
draw. Match its `sample-texture` destination and the Sway tree when comparing
against the final frame.

| Same-frame result | Next area to investigate |
| --- | --- |
| Raw source and isolated GLES input corrupt | Source contents or producer/reuse timing |
| Raw source clean, isolated GLES input corrupt | EGLImage import, sampling, synchronization or driver |
| Isolated input clean, final output corrupt | Composition state, blend/scissor/viewport/transform |
| Raw unavailable, isolated input corrupt | Source versus import remains unresolved |

The GL readback synchronizes work and can hide a timing race. A clean captured
frame is inconclusive if corruption disappeared during capture. The later host
screenshot is not guaranteed to be the same frame. Saved traces and persistent
visible corruption matter. A GLES input capture is never labeled a raw snapshot.

## Safe raw access

The CPU helper requires an already consumer-locked source and uses only
`wlr_buffer_begin_data_ptr_access(READ)` / `end_data_ptr_access`, with a balanced
extra lock. It validates dimensions, pitch and ARGB/XRGB/ABGR/XBGR layouts.

Pinned wlroots 0.17.4's **client linux-dmabuf buffer implementation has no
data-pointer access callbacks** (`types/wlr_linux_dmabuf_v1.c`). Linear layout
and modifier zero do not provide that missing API. For laptop2's recorded
Monitor DMA-BUF, expect an explicit unavailable reason rather than a raw image.
A copied SHM GLES texture can also have no remaining source to snapshot; Pixman
retains its readable SHM source.

No direct DMA-BUF FD mapping is attempted. CPU cache coherency and GPU ordering
are separate obligations in the [kernel DMA-BUF documentation](https://docs.kernel.org/driver-api/dma-buf.html#dma-buffer-ioctls).

## Ownership and synchronization findings

The audit uses the pinned wlroots 0.17.4 sources:

| Stage | Ownership behavior |
| --- | --- |
| `types/buffer/resource.c`, compositor attach/commit | The pending attachment owns a lock; commit moves it to current state |
| `render/gles2/texture.c`, DMA-BUF import/cache hit | An active texture locks the original source buffer |
| `types/buffer/client.c`, saved views | The client wrapper owns its texture; retained views keep that texture alive |
| GLES texture unref | Unlocks the original source; an idle cached EGLImage/GL texture can remain as an addon |
| `types/buffer/buffer.c`, last consumer unlock | Emits the internal all-consumers release signal |
| `types/wlr_linux_dmabuf_v1.c`, release listener | Sends `wl_buffer.release` if the protocol resource is still alive |
| `render/gles2/pass.c`, submit | Flushes GLES commands and unlocks the output; does not wait for GPU completion |

An active texture holds ownership. An **idle cached EGLImage alone does not**
hold a consumer lock or prevent legal client reuse. After the actual
[Wayland release event](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_buffer-event-release),
the client can reuse the buffer, subject to GPU synchronization. This audit has
not found a concrete premature unlock of a still-active client texture.

The new public Wayland protocol logger records `wl-buffer-release-sent` at the
actual server event marshal boundary. It also records incoming attach, commit,
destroy, wlroots pending/applied commits, import, sample, pass submit and output
commit. An internal `buffer-release` record remains separate. Marshal time is
not the time at which the client receives the event. Tracing adds no source locks.

The linux-dmabuf protocol requires implicit synchronization. This GLES2 path
relies on the EGL/driver/kernel implicit DMA-BUF fences, rather than performing
an explicit per-input native-fence wait. `glFlush` is not GPU completion; this
does not by itself demonstrate missing synchronization. The
[kernel implicit fence description](https://docs.kernel.org/driver-api/dma-buf.html#implicit-fence-poll-support)
distinguishes waiting for a writer from waiting for all readers/writers. A bare
`glFinish` before sampling in Rack's context would not establish completion of
the client's separate GPU context, so no speculative `sync-input` is added.

## Optional ownership experiment

`--mode hold-input` is available only with GLES2. It adds one source lock per
unique sampled DMA-BUF, finishes Rack's same GLES context after the real pass
submit, then unlocks those sources. It uses no sleep, never retires host-owned
output buffers, and reports failures. Failed submit/renderer destruction paths
also preserve safe release ordering. This diagnostic changes the source hold
interval and necessarily waits for GPU completion, so an improvement would
implicate ownership/synchronization timing without proving a particular defect.

This mode is **not the selected next physical test**. No `copy-input` is added:
copying would first sample the same suspect import. Normal Rack launches load
none of this diagnostic preload and retain their existing renderer policy.

## Bounds and local verification

Trace storage remains two chunks of about 16 MiB. There are at most eight
manual requests per run, eight distinct client textures and eight million
input pixels per captured frame, and 4096 pixels per dimension. Limit/error
records are explicit. The tracer is loaded only into Sway; control programs
and client descendants do not inherit it.

Validation on the development host:

| Check | Result |
| --- | --- |
| Combined Python regression discovery | 177/177 |
| Acceptance with disposable settings and private compositor sockets | 41/41 |
| Existing Files environment/source/CLI parity | 33/33; COSMIC/session bus probes were read-only |
| GLES2/GLES3 software EGL capture and state restoration, external EGLImage, alpha | 13/13 |
| Supported raw access, channel layouts, unavailable access, lock/error cleanup | 14/14 |
| Opt-in source hold, GPU-finish ordering, failure/reentrancy/cleanup | 11/11 |
| Real Wayland protocol release/attach/commit/destroy and live identity | 6/6 |
| Actual pass/release frame correlation, including a late capture request | 3/3 |
| Runner/capture workflow, exact-frame PNG/alpha/manifest, preload isolation | 15/15 |
| Four-pane Pixman observation with per-input capture | 24/24 resizes; 298 scene submits, 339 wire releases; no cross-pane leakage or import failure |
| Final capture-request timing change, four-pane Pixman | 4/4 resizes; five exact-frame raw inputs including Monitor; no leakage or import failure |
| Popup lifecycle through Rack launcher | 100 creation/first-commit/reposition/destruction cycles |
| Popup input through launcher and after host leave | 100 cycles per path |
| Pointer backend | 256 press/leave/release and drag cycles; old-backend negative control passed |
| DnD cursor boundary | 128 boundary cycles; historical cursor assertion reproduced by negative control |
| DnD ownership lifetime | 240 selections, 480 cancellations/unfocused drops, 240 outgoing endings; retained transfer and leak negative control passed |
| Clipboard state lifetime | 2,000 cycles; 4,029 offers reclaimed; ASan/UBSan/LSan clean |
| Clipboard reliability, included in discovery | 140 bidirectional swaps; stable descriptors, memory and process count |
| Host-open bridge | 11/11; includes 100 repeated host-open/direct-MIME cycles |
| Pinned local Sway verification | `bootstrap-sway.sh --check` passed; tracked vendor trees clean |

The Pixman workload used real COSMIC Monitor and three colored terminal
stand-ins. Its four panes used SHM; this validates the capture integration and
control, not laptop2's DMA-BUF/Mesa path. Local EGL tests use software Mesa and
cannot establish correctness on the Radeon hardware. Laptop2's new per-input
capture, sustained Monitor runtime and view switching remain to be performed.
