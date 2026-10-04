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

At `66a3e7d64805fd1c7c3d8f5d9a1c521fa21f7789`, laptop2 reported another
critical result in `run-20261003-221516.POqehP2D`: frame 462's **isolated
Monitor GLES input is already corrupt**, as are its final pre-submit image and
desktop. The source is PID 29867, surface ID 32, buffer 97, generation 132,
597×307 linear ARGB8888 DMA-BUF, one plane, stride 2560, texture
`0x6009e9dd9280`, GL texture 5, EGLImage `0x6009e9c4d6b0`. Raw CPU access remains
unavailable.

Buffer 97 is attached at frame 461 and remains current at the corrupt sample
in frame 462. Its next release follows attachment of buffer 103 in frame 463.
The physical trace shows no obvious premature release for this sample. Normal
Rack composition is therefore a reduced suspect, and `hold-input` is not the
next experiment. Source contents versus GLES import/sampling remains unresolved.

These observations are on AMD A6-5200/Radeon HD hardware. The missing axis is
**GLES2 renderer/output with client SHM input**; Pixman had changed both axes.

## One next laptop2 test

Close Rack, then use the existing clone from a terminal on the host desktop:

```bash
cd ~/sc7-rack
git pull --ff-only origin main
./install.sh
./scripts/run-render-diagnostic.sh --renderer gles2 --mode shm-input --capture-inputs
```

First capture while Monitor is animating to verify the input transport. Then
repeat Dashboard/CPU switching and outer-window resizing, and capture again if
corruption appears. Use another **host** terminal:

```bash
cd ~/sc7-rack
./scripts/capture-render-frame.sh
```

Save the complete artifact directory printed by the runner, plus a desktop
screenshot while corruption is visible. This is one observation experiment;
no further renderer matrix is requested. The helper selects the live Rack PID
and exact IPC socket, verifies that the run directory belongs to that PID, and
converts only input captures bearing the composed frame's number.

The helper now prints `shm-input verified: Monitor pid=... kind=shm; ...
renderer=gles2` and saves `shm-input-frame-FRAME.json`. It requires a stable
Monitor PID in both captured trees, real `wl-buffer-attach` records whose kinds
are exclusively `shm`, a GLES2 render pass for the exact frame, and the isolated
Monitor input capture. A copied SHM GLES texture can say `kind=owned-upload`;
that is not substituted for attachment evidence. Missing Monitor, no fallback,
DMA-BUF/mixed input, a different renderer or missing evidence is **UNVERIFIED**
and makes the helper exit 3 after saving the captured artifacts. Do not treat
such a run as the requested GLES2/SHM axis.

If this verified axis stays clean, focus on client DMA-BUF contents,
linux-dmabuf → EGLImage → GLES sampling and implicit synchronization on the old
Radeon/Mesa path. If it corrupts, DMA-BUF input is not necessary and broader
GLES renderer/driver behavior remains relevant. Monitor refusing SHM is a
separate reported result; the runner does not replace it or select Pixman.

## How shm-input changes capability negotiation

Pinned Sway `sway/server.c:133` installs its security global filter before
creating protocol globals. `server.c:147` calls
`wlr_renderer_init_wl_shm`; `render/wlr_renderer.c:220` calls
`wlr_shm_create_with_renderer`, and `types/wlr_shm.c:531` creates `wl_shm`.
When DMA-BUF texture formats are available, `server.c:150–152` creates legacy
`wl_drm` and `zwp_linux_dmabuf_v1` version 4. Their actual global creation is in
`types/wlr_drm.c:249` and `types/wlr_linux_dmabuf_v1.c:958` respectively.

The diagnostic module `tests/render_shm_input.c` wraps the public
`wl_display_set_global_filter` API only with a nonempty trace and the exact
`SC7_RENDER_EXPERIMENT=shm-input`. It chains Sway's existing callback and data,
then rejects those two exact input protocol names at advertisement **and bind**.
`wl_drm` must also be hidden because its PRIME path can import DMA-BUFs. SHM,
other globals and security restrictions keep their normal behavior. Per-display
state handles subsequent filter replacements, removal and display destruction.
Protocol objects still initialize successfully; allocation failures are not
simulated. Policy allocation/API failure terminates the diagnostic explicitly.

The hook does not touch GLES2 initialization, DMA-BUF import implementation,
output allocation or the nested backend's connection to host COSMIC. No buffer
is converted after attachment. Only Rack Sway loads the preload, and its
constructor removes it before children launch. Normal launches and host-opened
applications load no policy hook. The runner rejects `shm-input` with Pixman.

`shm-input-policy` and `input-capability` trace records describe installation
and protocol filtering. `wl-buffer-attach ... pid=MONITOR_PID ... kind=shm`
describes the actual received buffer independently of those policy records.

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

## Input-capture baseline verification

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
cannot establish correctness on the Radeon hardware. Laptop2 has now completed
the per-input capture with the corrupt result above. Its new SHM-input axis,
sustained runtime and view switching remain to be performed.

## SHM-input validation

The eight focused capability tests use real Wayland registries, requests and
wlroots SHM storage/commits. DMA-BUF/legacy DRM globals are minimal registry
fixtures; they do not test Radeon allocation or actual DMA-BUF importing.
Controls cover normal, observe, misspelled mode, missing/empty trace and an
unpreloaded process. All continue advertising both GPU input protocols.
The exact diagnostic hides both, keeps SHM, rejects explicit binds to known
hidden global IDs, preserves original/replaced filters and callback data, and
survives independent display teardown/recreation. Repeated SHM commit/read/
release testing covers 256 commits across two displays.

Nine evidence-verifier tests reject DMA-BUF/mixed input, Pixman masquerading
as requested GLES2, changed/absent Monitor PIDs, missing attachment evidence,
another client's SHM buffers and input captures from a different frame.
Same-frame DMA-BUF/unknown/missing sample kinds are rejected even if older
attachment records say SHM. Rotated trace chunks remain usable. The runner and
capture-helper integration preserve GLES2, preload isolation and exact-frame
verification.

| Current check | Result |
| --- | --- |
| Complete Python discovery, including bootstrap/install and unit suites | 196/196 |
| SHM capability/real protocol tests | 8/8, included above |
| Transport evidence verifier, including sample-kind rejection | 9/9, included above |
| Diagnostic workflow | 17/17, included above |
| Acceptance, disposable settings and private sockets | 41/41 |
| Existing Files environment/source/CLI parity | 33/33 |
| Popup lifecycle and launcher/host-leave menu input | 100 cycles per path |
| Pointer backend and cursor/DnD boundary | 256 / 128 cycles plus historical negative controls |
| DnD lifetime | 240 selections, 480 cancellations/unfocused drops, 240 outgoing endings; retained transfer and negative control passed |
| Clipboard state sanitizer | 2,000 cycles, 4,029 offers reclaimed; ASan/UBSan/LSan clean |
| Host-open bridge | 11/11, including 100 repeated launch cycles |
| Pixman integration control, expanded preload | 4/4 outer resizes; real Monitor plus three terminal stand-ins; five input captures; no leakage/import failure |
| Pinned bootstrap verification | `--check` passed; tracked vendor trees clean |

The first host-leave menu test failed at cycle 78 while several compositor
suites ran concurrently; the identical test then passed 100 cycles alone.
A placement transaction applied near the failure and the harness caches its
parent coordinates. A fixture timing race is plausible, but its cause is not
proved. The failing and passing logs are retained; this diagnostic does not
modify the tested Sway/input binaries.

An intermediate capture-limit test exceeded its fixture timeout because the
helper continued polling after an already recorded exhausted budget. The
helper now reports that limit immediately without requesting another frame,
including when the limit record is in the previous trace chunk. The final
196-test discovery passes with that correction.

The development host has no accessible DRM render node. Its registry/SHM
tests and software EGL tests cannot prove COSMIC Monitor's physical GLES2/SHM
fallback or Radeon behavior. The single laptop2 command above supplies that
remaining evidence.
