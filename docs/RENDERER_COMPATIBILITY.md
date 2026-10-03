# Nested renderer compatibility

## Physical laptop2 result

On Pop!_OS/COSMIC laptop2, the default nested GLES2 session developed stale
terminal pixels inside COSMIC System Monitor after resizing. The same Monitor,
four-pane layout, Dashboard/CPU switching, and repeated resizing stayed clean
with `WLR_RENDERER=pixman`. A second physical test kept GLES2 enabled and set
`WLR_EGL_NO_MODIFIERS=1`; those same actions also stayed clean. This identifies
the explicit DMA-BUF modifier path as the relevant compatibility boundary.

The wlroots 0.17.4 EGL code handles that setting in `render/egl.c`. It stops
querying explicit format modifiers and advertises implicit and linear formats
for EGL imports and renders. GLES2, GBM output allocation, and DMA-BUF remain
enabled. The setting changes **both** COSMIC client buffer import and Rack's
outer Wayland output buffers. The physical A/B test therefore does not yet
identify which side, driver, or exact modifier mishandles the pixels. Rack's
local wlroots patch does not change the renderer or damage-ring source. Sway
marks the whole output damaged and replaces its swapchain when the nested output
size changes, so changing its damage arithmetic without further evidence would
be speculative.

Rack defaults to `WLR_EGL_NO_MODIFIERS=1` in its own nested session. An explicit
`WLR_EGL_NO_MODIFIERS=0` retains the old path for diagnosis. The host-open
bridges restore the host's original value so applications opened outside Rack
do not inherit this compatibility setting. Pixman remains available through
`WLR_RENDERER=pixman`; Rack does not select it automatically.

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
python3 tests/render_damage_tests.py --renderer all --cycles 24
```

The three runs use normal GLES2, GLES2 without explicit modifiers, and Pixman.
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
