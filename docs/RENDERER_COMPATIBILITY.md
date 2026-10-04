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
full damage. The next single test captures each sampled client input and the
final composed frame. See [input diagnostics](RENDER_INPUT_DIAGNOSTICS.md) for
the physical procedure and ownership audit, and
[resource diagnostics](RENDER_RESOURCE_DIAGNOSTICS.md) for the earlier experiments.

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
`WLR_RENDERER=pixman`; Rack does not select it automatically. Until a reliable
GLES2 correction or renderer policy is validated, use Pixman when corruption
appears.

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

First launch Rack with `WLR_RENDERER=gles2 WLR_EGL_NO_MODIFIERS=1`, then close
it and repeat with `WLR_RENDERER=pixman`. An alternating GLES2/Pixman/GLES2
sequence helps expose background-load variation. Record CPU seconds and
one-core CPU percentages for Rack Sway and host `cosmic-comp`, plus whether any
corruption occurred. Do not collect screenshots or instrumented traces during
the timed CPU sample; run those separately.

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

Each command runs normal GLES2, GLES2 without explicit modifiers, and Pixman;
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
