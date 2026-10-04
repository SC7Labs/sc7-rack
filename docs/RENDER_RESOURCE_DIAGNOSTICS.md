# Renderer resource diagnostics

## Confirmed physical evidence

Laptop2, Pop!_OS 24.04, COSMIC Monitor 1.9.0; resource experiments completed
at `eb5085ac40bcadbf52896b977b349d3d4689bf52`:

| Nested renderer | Physical result |
| --- | --- |
| GLES2 | Corrupt |
| GLES2, explicit modifiers disabled | Corrupt |
| GLES2, explicit modifiers disabled, full output repaint | Corrupt |
| Repeated ordinary GLES2 | Corrupt |
| GLES2 fresh input texture/import | Corrupt |
| GLES2 fresh target identity | Corrupt |
| GLES2 fresh output storage | Corrupt |
| Pixman | Clean |

The four expected clients remain alive. In fresh-output frame 447 the
pre-submit image is already corrupt, despite new output storage, age zero,
a new EGLImage and full damage. Ordinary output reuse and host submission
cannot explain that capture. The next test compares the existing sampled
client texture with the final image of the same frame; see
[input diagnostics and ownership audit](RENDER_INPUT_DIAGNOSTICS.md).

## Source audit

Rack uses its pinned wlroots 0.17.4 and local Sway. No system compositor files
are changed by these diagnostics.

* Sway reads a surface's current texture during composition. SHM updates can
  reuse a GLES2 texture and upload only client-reported damage. DMA-BUF imports
  have an addon cache keyed by source buffer and renderer; that cache retains
  its EGLImage and GL texture.
* Resize transactions can keep saved client buffers alive after their surface
  changes. Any import experiment must retain the original source until the
  saved texture is no longer displayed. Retaining duplicated DMA-BUF file
  descriptors alone does not prevent a producer from reusing released pixels.
* The normal output path replaces a swapchain when dimensions or format
  change. GLES2 FBO/renderbuffer/EGLImage resources are cached on each output
  buffer and destroyed with that buffer. Thus source inspection already
  predicts new target resources for new-sized storage.
* The nested Wayland backend locks submitted buffers until host
  `wl_buffer.release`. Swapchain slots remain acquired until all consumer
  locks are released. This is the intended protection against early reuse;
  its physical correctness has not yet been established.

The new experiments are a separate diagnostic preload. Normal Rack launches
do not load it or change resource behavior. Its constructor removes
`LD_PRELOAD` before Sway starts client or host applications.

## Live laptop2 procedure

Update the existing clone on laptop2 and close Rack before each trial:

```bash
cd ~/sc7-rack
git pull --ff-only origin main
./install.sh
./scripts/run-render-diagnostic.sh --renderer gles2 --mode observe --capture-inputs
```

The runner prints a persistent artifact directory. Reproduce the corruption
with the same outer-window resizing and Dashboard/CPU switching. While the
corruption remains visible, run this in another terminal on the host desktop:

```bash
cd ~/sc7-rack
./scripts/capture-render-frame.sh
```

Keep Rack visible and Monitor animating. The helper requests one composed
frame, saves its pixels as PPM and PNG, and records the exact Rack IPC tree,
outputs, PID, and selected renderer environment. It tries a host screenshot
with `grim` if available; otherwise take a desktop screenshot while the
corruption remains visible.

The capture helper verifies that the live Rack PID owns the selected diagnostic
directory before writing metadata or requesting pixels. An old run directory
cannot be combined with a new Rack process or overwrite that old run's evidence.

The pre-submit capture runs after Sway draws the scene and before the render
pass submits or the Wayland backend commits it to the host. Empty modeset/test
buffers are excluded. For GLES2 it verifies that the bound FBO is the active
pass's expected FBO, then reads that FBO. Pixman reads its active pass image.

Readback synchronizes GPU work and can change a timing race for the captured
frame. The host screenshot is taken later and may represent a different
frame. Compare persistent visible corruption and the corresponding trace;
one clean capture alone cannot prove the original path is clean.

* Corrupt pre-submit pixels locate the failure at or before composition:
  client texture/import or render target handling.
* A clean pre-submit image alongside persistent corrupt host display points
  toward export, synchronization, host presentation, or buffer ownership.
  Repeat this result because readback may itself synchronize the failure away.

The following resource experiments have already failed physically on laptop2.
They remain available for reproduction, but are not the next requested test:

```bash
./scripts/run-render-diagnostic.sh --renderer gles2 --mode fresh-input
./scripts/run-render-diagnostic.sh --renderer gles2 --mode fresh-target
./scripts/run-render-diagnostic.sh --renderer gles2 --mode fresh-output
./scripts/run-render-diagnostic.sh --renderer pixman --mode observe
```

`fresh-input` rejects the cached SHM update path, forcing a complete texture
upload. DMA-BUF input gets a unique temporary import wrapper, new EGLImage,
and new GL texture. The original source remains locked until that texture is
destroyed, including saved-view resize transactions.

`fresh-target` uses the same output DMA-BUF storage through a new temporary
buffer identity for every render pass. This recreates its FBO, renderbuffer,
and EGLImage while preserving output storage and host ownership. It is a
stronger test than resize-only recreation, which the normal path already
performs. The backing buffer remains locked until target addon teardown.

`fresh-output` retires only released swapchain slots with zero consumer locks.
It allocates replacement storage with age zero. Acquired/host-owned slots and
the four-slot limit remain intact. This experiment necessarily also causes
full repaint; the physical full-repaint control has already failed.

A requested experiment reports allocation/import failures rather than silently
falling back to observation. None of these modes is a production renderer fix.
All three resource experiments reproduced laptop2's corruption.

## Trace contents and bounds

The current and previous trace chunks retain about 32 MiB of recent data.
Trace rotation keeps later corruption observable during long sessions.
There are at most eight manual capture attempts per run, with images limited
to 4096 by 4096 pixels. Restart the diagnostic for more captures.

The trace records:

* Client PID, `wl_surface` resource ID, `wl_buffer` resource ID and attachment,
  source buffer pointer plus a monotonically increasing lifetime generation,
  SHM/DMA-BUF/other type, dimensions, format and modifier.
* Sampled texture pointer, GL texture name/target, EGLImage, and fresh-import
  generation when that experiment is selected.
* Buffer observation, all-consumers-released, destruction, and client
  `wl_buffer` resource destruction. An all-consumers release is not itself
  proof of a protocol callback. Separate `wl-buffer-release-sent` records
  observe the actual client release event at the server marshal boundary.
* Output acquire, swapchain pointer/slot, age, buffer generation and locks;
  render pass target/FBO/renderbuffer/EGLImage; frame and output commit
  sequence, dimensions, resize generation, damage and commit result.

Use the captured Sway tree to map client PIDs to pane app IDs. Actual laptop2
buffer types and object identities must come from this trace; application names
alone do not establish whether their buffers are SHM or DMA-BUF.

For a successful SHM partial upload, wlroots keeps the client wrapper's original
`source` pointer even when the uploaded pixels came from a newer buffer.
`input` records therefore label that pointer `cached_source`. Use
`wl-buffer-attach`, `texture-update`, and `texture-import` for the actual incoming
buffer. The regression mutates a real SHM client texture and checks this
distinction, so a cached wrapper cannot be mistaken for the latest attachment.

## CPU comparison

Measure production launches without diagnostic tracing or readback. On
laptop2 use the same window size and four clients, alternating GLES2, Pixman,
and GLES2 for comparable 60-second idle and active resize/view workloads:

```bash
WLR_RENDERER=gles2 WLR_EGL_NO_MODIFIERS=1 ./bin/sc7-rack
# Close Rack, then:
WLR_RENDERER=pixman ./bin/sc7-rack
```

During each run, use a host terminal:

```bash
python3 scripts/measure-rack-renderer-cpu.py --seconds 60 --host-cosmic-comp
```

Record Rack and host CPU, responsiveness, resize smoothness, and Monitor
animation smoothness. The main rig has no accessible DRM render node, so it
cannot supply a valid physical GLES2/Pixman comparison. Pixman remains the
physically clean laptop2 control; a default-policy decision requires these
measurements. No visual-corruption detector is assumed.

## Isolated regression

The existing four-pane visual harness can exercise the same diagnostic modes:

```bash
python3 tests/render_damage_tests.py --renderer all --cycles 24 --capture-presubmit --experiment observe
python3 tests/render_damage_tests.py --renderer all --cycles 24 --experiment fresh-input
python3 tests/render_damage_tests.py --renderer gles2-no-modifiers --cycles 24 --experiment fresh-target
python3 tests/render_damage_tests.py --renderer all --cycles 24 --experiment fresh-output
```

It requires the optional `foot`, `grim`, and Python Pillow tools. On a host
without a DRM render node, only the Pixman control is available. The one-shot
capture regression verifies a populated readable frame; the existing spatial
sentinel detector checks cross-pane leakage in nested and host screenshots.
Physical GLES2 testing remains necessary.

## Resource-diagnostic baseline validation, 2026-10-03

These totals cover the preceding resource experiments. Current input-capture
validation and the new 177-test total are recorded in
[input diagnostics](RENDER_INPUT_DIAGNOSTICS.md#bounds-and-local-verification).

| Check | Result |
| --- | --- |
| Existing Python regression suites | 97/97 |
| New input lifetime and incoming SHM telemetry | 10/10 |
| New target/output lifetime and allocation failure handling | 8/8 |
| New diagnostic runner, capture association, PNG, preload scope, log rotation | 14/14 |
| Combined Python discovery | 129/129 |
| Acceptance with disposable settings and private compositor sockets | 41/41 |
| Files environment/source/CLI parity | 33/33; existing COSMIC/session bus probes were read-only |
| Popup lifecycle through the Rack launcher | 100 creation/first-commit/reposition/destruction cycles |
| Popup input after host pointer leave and through the launcher | 100 cycles per path |
| Pointer backend and historical negative control | 256 press/leave/release and drag cycles; old backend fails |
| DnD cursor boundary and historical negative control | 128 boundary cycles; old backend reproduces the cursor assertion |
| DnD lifetime | 240 selections, 480 cancellations/unfocused drops, 240 outgoing endings; retained drops and historic leak control passed |
| Clipboard ownership lifetime | 2,000 cycles; 4,029 offers reclaimed; ASan/UBSan/LSan clean |
| Clipboard reliability | 140 bidirectional swaps; stable descriptors, memory and process count |
| Pixman observation, real Monitor and three colored terminal stand-ins | 24/24 resizes; no cross-pane sentinel pixels |
| Pixman fresh output storage, same four-pane workload | 24/24 resizes; every acquired buffer age zero; no cross-pane sentinel pixels |
| Pixman fresh input upload, same four-pane workload | 4/4 resizes; cached updates rejected; no cross-pane sentinel pixels |
| Pre-submit capture in each of those three runs | One complete, populated four-pane frame; no import failure |
| Pinned local Sway verification | `bootstrap-sway.sh --check` passed; tracked vendor trees clean |

All four final harness pane PIDs attached SHM buffers under this isolated Pixman
control. The three static panes are representative terminals, and the host is
headless Sway. This does not establish the buffer types or presentation behavior
of real COSMIC clients under laptop2's GLES2/COSMIC session.

The target experiment has real wlroots buffer/addon lifetime tests and a mock
GLES2 factory, including retained host ownership, failed allocation/import,
and repeated destruction. The main rig has no DRM render node, so its real
GLES2 FBO/EGLImage path remains untested here. Laptop2 has now supplied the
pre-submit comparison and all three resource results, with corruption in each.
The new per-input comparison and the 60-second CPU comparison remain pending.
The exact defective resource and a production renderer fix/default policy are
not yet established.
