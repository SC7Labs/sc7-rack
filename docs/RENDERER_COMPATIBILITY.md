# Nested renderer compatibility

## Physical laptop2 result

On Pop!_OS/COSMIC laptop2, the nested GLES2 session develops stale and
overlapping COSMIC System Monitor pixels after resizing or runtime. The same
four-pane layout, Dashboard/CPU switching, and repeated resizing stayed clean
with `WLR_RENDERER=pixman`. An early short run with
`WLR_EGL_NO_MODIFIERS=1` looked clean, but corruption later recurred with that
setting confirmed in the live Rack Sway process and Monitor 1.9.0 installed.
Disabling explicit modifiers is **not** a renderer fix. The live process tree
contains only the expected four clients, so duplicate windows do not explain
the observed pixels.

Physical testing at `268bd5236296f35af834cd823cfbd09e412849b9` also corrupted
under GLES2 with both `WLR_EGL_NO_MODIFIERS=1` and
`SC7_RACK_FULL_REPAINT=1`. Repeating ordinary GLES2 corrupted again; Pixman
remained clean. Full output repaint therefore does not correct this failure.
All three subsequent resource experiments also corrupted at `eb5085ac`:
fresh input imports, fresh target identity, and fresh output storage. Fresh-output
frame 447 was corrupt before host submission with new storage, age zero and
full damage. The isolated Monitor GLES input also corrupted at frame 462 in
the next physical capture. The missing A/B axis has now been tested on a fresh physical install:
GLES2 + DMA-BUF inputs corrupted immediately; the same GLES2 compositor with
SHM client inputs stayed clean. See the
[source/upstream audit](DMABUF_INPUT_AUDIT.md) and
[input diagnostics](RENDER_INPUT_DIAGNOSTICS.md) for the evidence.

## Production candidate policy

The authenticated private Sway patch hides `zwp_linux_dmabuf_v1` and legacy
`wl_drm` from every nested client, including explicit registry binds. `wl_shm`
remains available. This is part of the installed compositor and applies to
plain `rack`, without a preload or environment variable. GLES2 initialization,
output buffers, allocator, and the host Wayland connection are unchanged.
Host-opened applications connect to COSMIC and keep its normal protocols.
Sway’s existing security restrictions still apply to all other protocols.

There is no GPU heuristic and no experimental production DMA-BUF override.
This avoids exposing an input path that cannot yet be classified as safe.
The bootstrap authenticates the complete popup + SHM source tree and build
stamp. Exact old clean popup builds migrate locally; genuine dirty changes are
still rejected. The historical partial VM state still recovers before migration.

This is a mitigation candidate backed by the physical A/B result, **not a
proven DMA-BUF repair or a release-ready result**. A new dry laptop2 install and
the separate long-session Files validation remain required.

The wlroots 0.17.4 EGL code handles that setting in `render/egl.c`. It stops
querying explicit format modifiers and advertises implicit and linear formats
for EGL imports and renders. GLES2, GBM output allocation, and DMA-BUF remain
enabled. The setting changes **both** COSMIC client buffer import and Rack's
outer Wayland output buffers. The physical A/B test therefore does not
identify which side, driver, or exact buffer path mishandles the pixels. Rack's
local wlroots patch does not change the renderer or damage-ring source. Sway
marks the whole output damaged and replaces its swapchain when the nested output
size changes, so changing its damage arithmetic without further evidence would
be speculative.

Rack currently defaults to `WLR_EGL_NO_MODIFIERS=1` in its own nested session.
An explicit `WLR_EGL_NO_MODIFIERS=0` retains the old path for diagnosis. The host-open
bridges restore the host's original value so applications opened outside Rack
do not inherit this compatibility setting. Pixman remains available through
`WLR_RENDERER=pixman`; Rack does not select it automatically. The production candidate removes nested DMA-BUF inputs; Pixman remains
an explicit diagnostic control if the candidate still corrupts.

## Full-output repaint experiment

Sway has a built-in `-D damage=rerender` diagnostic. Rack exposes it only when
requested. It expands both the Wayland output damage and the actual render
damage to the full output for each scheduled composited frame in Rack's
four-pane layout. Fullscreen direct scanout bypasses this render path. The
diagnostic does not force extra frames or change client texture uploads.

On laptop2, close Rack between trials. In its existing clone, compare the same
Dashboard/CPU switching and outer-window resize sequence:

```bash
WLR_RENDERER=gles2 WLR_EGL_NO_MODIFIERS=1 ./bin/sc7-rack
SC7_RACK_FULL_REPAINT=1 WLR_RENDERER=gles2 WLR_EGL_NO_MODIFIERS=1 ./bin/sc7-rack
WLR_RENDERER=pixman ./bin/sc7-rack
```

The second trial prints `Rack diagnostic: full-output repaint enabled` when
the option is active. Laptop2 has completed baseline → full repaint → baseline:
all three GLES2 trials corrupted. Investigation now concerns GLES2 source
textures, render targets, and DMA-BUF import/synchronization. Pixman reads SHM client buffers
directly, while GLES2 can update a reused texture only in the client-reported
damage region; full output repaint cannot repair an already stale source
texture. This is a remaining hypothesis, not a proven source of corruption.

## Physical CPU comparison before choosing a renderer default

The development host has no accessible DRM render node, so it cannot provide a
valid GLES2 versus Pixman CPU comparison. On laptop2, use the same Rack window
size and four clients for each trial. Run the following measurement from a
terminal **outside** Rack, once while idle and once while repeatedly resizing
and switching Dashboard/CPU:

```bash
python3 scripts/measure-rack-renderer-cpu.py --seconds 60 --host-cosmic-comp
```

Measure plain `rack` first (GLES2 + SHM on laptop2), then an explicit Pixman
control after closing it. Historical DMA-BUF measurements require the previous
commit in a separate clone; the candidate intentionally has no DMA-BUF override.
Use an alternating sequence on the same window size/workload and record CPU
seconds for both Rack and the host. This tool does not measure the client’s
software rendering CPU, GPU time, energy, or frame latency: overall client/system
load and visible responsiveness must also be observed. No new physical CPU
numbers are available, and the main rig has no DRM render node.

SHM requires CPU-visible pixels and texture uploads. Clients may switch to
software rendering or copy GPU output to CPU memory; the trace does not measure
which fallback Monitor uses. A 597×307 ARGB frame contains 733,116 pixel bytes (715.9 KiB): a full upload at
60 Hz is about 44 MB/s; at 1 Hz it is 0.73 MB/s. This is an upload estimate for
one pane, not a benchmark or an upper bound on total memory traffic. Partial
texture updates can reduce it; resize can require full uploads. GLES2 still
composes on the GPU. Pixman additionally composes the output on the CPU.
Acceptable performance on the AMD A6 laptop remains part of the physical gate.

## Dry physical install gate

Close Rack first, including diagnostic sessions. Preserve previous files in a
backup; do not delete them. Run this from a host terminal on laptop2:

```bash
(
set -eu
: "${XDG_RUNTIME_DIR:?Run from the logged-in COSMIC host terminal}"
for record in "$XDG_RUNTIME_DIR/sc7-rack/sc7-rack.pid" "$XDG_RUNTIME_DIR/sc7-rack/inner_pids"; do
    if [ -f "$record" ]; then
        for pid in $(cat "$record"); do
            case "$pid" in ''|*[!0-9]*) echo "Invalid Rack PID record; stop here."; exit 1 ;; esac
            if kill -0 "$pid" 2>/dev/null; then
                echo "Rack or a recorded helper is still running ($pid); close Rack first."
                exit 1
            fi
        done
    fi
done
cd "$HOME"
backup=$(mktemp -d "$HOME/sc7-rack-before-shm.XXXXXXXX")
if [ -e "$HOME/.config/sc7-rack" ]; then
    mv "$HOME/.config/sc7-rack" "$backup/config"
fi
if [ -e "$XDG_RUNTIME_DIR/sc7-rack" ]; then
    mv "$XDG_RUNTIME_DIR/sc7-rack" "$backup/runtime"
fi
candidate="$backup/fresh-sc7-rack"
gh repo clone SC7Labs/sc7-rack "$candidate"
cd "$candidate"
./install.sh
)
```

Stop if installation fails. Open a **new host terminal**, then run only `rack`
(without `WLR_*`, `SC7_RENDER_*`, or diagnostic settings in that terminal).
The normal launch log must say `Rack client buffers: SHM`.
Check Dashboard → CPU → Dashboard, repeated resizing and an extended run;
menus, DnD both ways, clipboard both ways; Files New Folder, Copy/Paste, Move To,
Extract; host-open .txt/.md/PDF/HTML/images and space names. Close/reopen Rack
and confirm helpers terminate. Measure the plain session with the CPU tool
above from the new clone, without capture instrumentation.

The renderer candidate qualifies only if that dry install stays clean and
responsive through these checks. The Files mutation failure must also remain
absent through a sufficiently long session. Until those physical results arrive,
this is a pushed candidate, not a completed renderer fix or v0.1 readiness.

## Repeatable visual test

The optional `tests/render_damage_tests.py` runs a private headless Sway host
and Rack-local Sway, starts the real COSMIC System Monitor with three colored
pane sentinels, repeatedly resizes Rack's outer window, captures both the inner
and host image, and rejects any sentinel pixels in the Monitor pane. It records
output dimensions, swapchain buffer identities and ages, damage regions, and
DMA-BUF modifiers in the artifacts directory. An injected cross-pane pixel
pattern must fail the same detector before the compositor test begins.

Install the optional test tools (`foot`, `grim`, and Python Pillow), then on a
physical machine with a DRM render node run:

```bash
python3 tests/render_damage_tests.py --renderer all --cycles 24 --artifacts "$HOME/rack-damage-baseline"
python3 tests/render_damage_tests.py --renderer all --cycles 24 --full-repaint --artifacts "$HOME/rack-damage-full"
```

Each command now runs GLES2 + SHM, GLES2 + SHM without explicit output modifiers, and Pixman + SHM;
the second adds Sway's full-repaint diagnostic. The test records buffer ages,
identities, damage and commits, retaining two recent trace chunks of about
16 MiB each. It
checks that every nested output commit under full repaint covers the entire
output. The screenshot detector catches other-pane sentinel colors inside
Monitor; it cannot automatically recognize duplicated Monitor card pixels, so
inspect saved screenshots for that symptom.

On a CPU-only development host, only the Pixman control is available:

```bash
python3 tests/render_damage_tests.py --renderer pixman --cycles 12
```

Local CPU-only validation completed 12 Pixman resize cycles at four output
sizes with zero cross-pane pixels in both inner and host captures. It observed
252 frame commits and damage queries, 908 surface-damage queries, buffer ages
0 and 2, 23 buffer identities, and zero import failures. Physical
GLES2 testing remains necessary because that development host has no accessible
`/dev/dri` render node.

The full-repaint control completed 24 additional Pixman resize cycles
with zero cross-pane leakage; all 440 rendered frame commits carried full-output
damage, with buffer ages 0 and 2 and zero import failures. In separate one-size
traces, ordinary rendering committed 37 partial-damage frames and 16
full-damage frames; the diagnostic committed 49 full-damage frames and zero
partial-damage frames. This validates
that the switch changes actual output damage. It does not establish whether
the switch cures laptop2's GLES2 corruption.
