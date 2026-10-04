# Client DMA-BUF audit and SHM production candidate

Audit date: 2026-10-03. Rack HEAD before this work:
`3777f38f785ec0c3949b06e98756e2161e1a651d`. Private wlroots is upstream
0.17.4 (`2e461be6083fb1da9d4255d354af9b2e2621b148`) plus Rack patches.
This document records a compatibility candidate, not a declaration that
laptop2 or the release is fixed.

## Physical evidence and confidence

Laptop2 uses an AMD A6-5200 APU / Radeon HD Graphics. The user quarantined
the previous Rack installation and state, cloned the repository again and
ran `./install.sh`. Ordinary `rack` immediately corrupted Monitor. The same
installation with GLES2 and the verified `shm-input` diagnostic stayed clean.

| Path | Physical result |
| --- | --- |
| GLES2 with client DMA-BUF input | Corrupt |
| GLES2 with verified client SHM input | Clean |
| Pixman with SHM input | Clean |
| GLES2 with fresh input/import, fresh target, fresh output or full repaint | Corrupt |

At frame 462 of `run-20261003-221516.POqehP2D`, the isolated Monitor input
already contains the corruption, before normal Rack composition. Its current
buffer is still unreleased: buffer 97 attaches in frame 461, is sampled in 462,
and is released after buffer 103 attaches in 463. The source is ARGB8888,
597×307, modifier 0, one plane and stride 2560. Raw CPU inspection is unavailable
because this buffer implementation has no data-pointer access callback.

**High confidence:** the failing path includes client DMA-BUF production,
sharing, import or sampling; SHM negotiation avoids it while keeping Rack's
GLES2 renderer/output. Normal output damage/reuse is no longer the leading
explanation. **Unresolved:** whether the pixels are wrong before EGL import,
the importer/driver interprets them incorrectly, or synchronization fails.
Changing advertised protocols can also change the client's allocation and
rendering path. There is no producer fence capture or independent raw source
image that proves one particular Radeon/Mesa defect.

## Pinned source: DMA-BUF and SHM side by side

The source links below identify the audited development-host files. Equivalent
unmodified code is in the [upstream 0.17.4 tree](https://gitlab.freedesktop.org/wlroots/wlroots/-/tree/0.17.4).

| Stage | DMA-BUF input | SHM input |
| --- | --- | --- |
| Protocol capability | Sway constructs `wl_drm` and `zwp_linux_dmabuf_v1` when the renderer reports DMA-BUF formats. Their globals are created in [wlr_drm.c:249](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/types/wlr_drm.c:249>) and [wlr_linux_dmabuf_v1.c:958](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/types/wlr_linux_dmabuf_v1.c:958>). | `wlr_renderer_init_wl_shm()` calls the renderer-backed SHM manager; `wl_shm` is created in [wlr_shm.c:531](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/types/wlr_shm.c:531>). |
| Resource → surface | Both use [resource.c:42](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/types/buffer/resource.c:42>), which returns a locked source, then [wlr_compositor.c:49](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/types/wlr_compositor.c:49>) stores it in pending state. Commit moves the reference into current state. | Same surface/refcount path. |
| Storage access | [wlr_linux_dmabuf_v1.c:109](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/types/wlr_linux_dmabuf_v1.c:109>) returns the protocol's FD/offset/stride/format/modifier attributes. The implementation supports `get_dmabuf`, not CPU data-pointer reads. | [wlr_shm.c:224](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/types/wlr_shm.c:224>) accesses a retained pool mapping, adds the buffer offset and returns format/stride, with SIGBUS protection and matching access teardown. |
| Texture creation | [texture.c:253](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/render/gles2/texture.c:253>) imports an EGLImage and binds it through `glEGLImageTargetTexture2DOES`. [texture.c:322](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/render/gles2/texture.c:322>) locks the original source and caches image/texture as its addon. | [texture.c:352](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/render/gles2/texture.c:352>) starts CPU read access and calls the pixel upload path. [texture.c:239](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/render/gles2/texture.c:239>) sets row length and uploads into an owned `GL_TEXTURE_2D` with `glTexImage2D`, then ends source access. |
| Repeated commits | DMA-backed textures reject in-place updates ([texture.c:36](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/render/gles2/texture.c:36>)); the replacement import/cache path rebinds non-external EGLImages. External textures rely on GL's external-image visibility semantics ([texture.c:110](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/render/gles2/texture.c:110>)). `texture-update ok=0` is expected here, not proof of an import failure. | When format and reference conditions permit, [texture.c:32](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/render/gles2/texture.c:32>) uploads damaged rectangles with `glTexSubImage2D`, respecting stride/skipped rows/pixels and resetting that state. Otherwise a new owned texture is uploaded. |
| Current source after commit | The current temporary source lock is released after commit listeners ([wlr_compositor.c:502](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/types/wlr_compositor.c:502>)); the active imported texture's source lock still protects the DMA-BUF. | The temporary source lock can reach zero immediately after upload/commit: the compositor texture contains a copy, so the client can reuse its SHM buffer without changing that texture. |
| Sampling / submission | [pass.c:145](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/render/gles2/pass.c:145>) binds/draws the imported texture. [pass.c:39](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/render/gles2/pass.c:39>) submits with `glFlush`; no explicit input fence wait is present. | Same GLES2 drawing/output path, using the owned upload. Upload and subsequent draw are ordered in Rack's GLES context. |
| Release / destruction | [client.c:25](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/types/buffer/client.c:25>) destroys the wrapper's texture reference. [texture.c:159](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/render/gles2/texture.c:159>) unlocks the source but can keep an idle cached image. Zero source locks emit release ([buffer.c:52](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/types/buffer/buffer.c:52>)), which sends the real protocol event ([wlr_linux_dmabuf_v1.c:122](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/types/wlr_linux_dmabuf_v1.c:122>)). Dropped buffers with zero locks destroy addons/image/texture. | The same zero-lock event sends release ([wlr_shm.c:302](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/types/wlr_shm.c:302>)). The uploaded GL texture is independently deleted when its wrapper is destroyed. |

An idle cached EGLImage is not an active buffer lock. Conversely, a valid
active lock prevents protocol reuse but does not prove producer GPU work has
finished. There is no audited premature release for the corrupt sample.

## Import attributes and synchronization

[egl.c:700](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/render/egl.c:700>) forwards width, height, DRM FourCC, each plane's FD,
offset and pitch to `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)`. Explicit modifier
halves are supplied when supported and the modifier is not INVALID; LINEAR is
accepted even without the modifier extension. `EGL_IMAGE_PRESERVED_KHR=TRUE`
prevents import from intentionally discarding contents. The source does not
replace stride 2560 with the tight width of 2388 bytes. Both are compatible
with a 597-pixel ARGB8888 row; padding alone is not an error.

DMA import keeps the DRM FourCC for EGL. SHM's [ARGB/XRGB mapping](</home/sc7/projects/opensource projects/sc7-rack/vendor/wlroots/render/gles2/pixel_format.c:6>) uses BGRA unsigned bytes on little-endian systems, with the correct alpha
distinction. No proven format/channel/stride bug was found. LINEAR describes
layout, not fence completion. The attribute contract comes from
[EGL_EXT_image_dma_buf_import](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import.txt).

The linux-dmabuf protocol requires implicit reservation fences unless another
extension specifies otherwise. The pinned GLES2 path relies on EGL/Mesa/kernel
implicit synchronization; it does not export an acquire sync-file, perform an
EGL native-fence wait, or wait for producer completion on the CPU. `glFlush`
submits Rack's commands; it is not a completion wait for another process.
This absence is not itself proof that GLES2 is wrong: an implicit consumer is
expected to order accesses through the driver. SHM avoids importing shared GPU
storage and its producer/consumer fence interaction.

The [kernel DMA-BUF documentation](https://docs.kernel.org/driver-api/dma-buf.html#implicit-fence-poll-support) distinguishes writer/read fences and CPU coherence. A proposed
`poll(POLLIN)` can observe only fences actually attached to the reservation;
it cannot repair a producer that omitted them. `DMA_BUF_IOCTL_SYNC` handles CPU
cache coherence and does not substitute for GPU ordering. A speculative wait,
`glFinish`, or FD mapping is therefore not being backported as a claimed fix.

## Rack patch influence

`git -C vendor/wlroots diff 0.17.4 -- render types/buffer
types/wlr_compositor.c types/wlr_linux_dmabuf_v1.c` is empty for the existing
Rack patch set. Its changes concern nested input/DnD, host output behavior,
branding and Wayland display dispatch, not client texture import or fences.
The physical SHM control leaves the GLES2 output path running. Rack integration
exposes an incompatible path on laptop2; attribution to wlroots, Mesa, kernel
or Monitor's DMA-BUF producer remains unresolved.

## Exact upstream changes assessed

Official upstream history was inspected through master
`c5c57cd316581c0779a51309f1ae90b7e1abd8c4` (2026-10-03) in a temporary research
repository. Dates below are **landing/committer dates**, not old author dates.

| Upstream change | Landing date | Applicability / backport decision |
| --- | --- | --- |
| [cb5f67431b782ab20a9ea8dd68471740e5d20f7b — Don't double import dmabuf](https://gitlab.freedesktop.org/wlroots/wlroots/-/commit/cb5f67431b782ab20a9ea8dd68471740e5d20f7b), followed by [9bf51e744e257398baaf5c42dace7e933c1a780d — Don't attach texture as buffer addon](https://gitlab.freedesktop.org/wlroots/wlroots/-/commit/9bf51e744e257398baaf5c42dace7e933c1a780d) | 2023-11-30, on the newer development branch | Unifies texture/render-target import storage. This concerns using one buffer as both kinds of target. Monitor's client input is not Rack's output target, and fresh-import diagnostics still corrupt. Refactor, not a demonstrated narrow repair. |
| [19ffbfe3565f95fff313d240da4d7193795be04d — EGL explicit sync extensions](https://gitlab.freedesktop.org/wlroots/wlroots/-/commit/19ffbfe3565f95fff313d240da4d7193795be04d) and [d2374b3e4ed802b8676029631e480d6cf75edb13 — GLES2 explicit sync API](https://gitlab.freedesktop.org/wlroots/wlroots/-/commit/d2374b3e4ed802b8676029631e480d6cf75edb13) | 2024-08-06; first tagged release 0.19.0 | Adds native-fence wait when a texture has a supplied `wait_timeline`, and completion signaling when a pass has `signal_timeline`. Needs render/timeline/protocol/scene integration. Implicit clients without a supplied timeline retain the implicit path; cherry-picking these is not a fix for this case. |
| [e9a6b3b85dfb3f4c43cc901b6936a7c0e27ea0ee — Wayland backend explicit sync](https://gitlab.freedesktop.org/wlroots/wlroots/-/commit/e9a6b3b85dfb3f4c43cc901b6936a7c0e27ea0ee) | 2024-11-11 | Changes host output acquire/release timeline handling. The corrupt isolated input precedes this path. Not the leading repair, with substantial dependencies. |
| [43b37e34d662ca893ebaae6e813634ee216c352f — Vulkan implicit read fence](https://gitlab.freedesktop.org/wlroots/wlroots/-/commit/43b37e34d662ca893ebaae6e813634ee216c352f) and [2367d78c3c1de2428cbbf9444d6eff632698baf6 — Vulkan implicit write fence](https://gitlab.freedesktop.org/wlroots/wlroots/-/commit/2367d78c3c1de2428cbbf9444d6eff632698baf6) | 2026-02-23 | Repairs wlroots' Vulkan renderer fence publication/waits. Rack uses GLES2. Monitor's actual producer implementation is not established by the trace, so this cannot be attributed to it either. Do not backport unrelated renderer changes. |
| [e8c983808d8b98a71f991c16c24367494386958b — Reset surface buffer after texture creation failure](https://gitlab.freedesktop.org/wlroots/wlroots/-/commit/e8c983808d8b98a71f991c16c24367494386958b) | 2026-07-15 | Small genuine fix for retaining an old surface buffer after a failed import, especially with changed sizes/viewports. The reported corrupt input has a successfully imported/sampled texture. No matching import failure is proven; applying it cannot be presented as the observed corruption fix. |

No inspected upstream change is a proven safe narrow fix for the physical
failure. The current decision is to keep the pins, avoid a speculative
renderer backport, and make the physically successful SHM negotiation the
production candidate.

## Production policy and performance limits

The authenticated private Sway patch rejects the two DMA-BUF factory globals
through its existing global filter, including explicit binds. `wl_shm`, the
remaining security policy, renderer selection and output allocator remain
active. Plain `rack` uses this policy without diagnostic flags or a preload.
Host-opened applications connect to the host compositor and keep its normal
protocols. This is deterministic for v0.1; it does not guess which Radeon or
Mesa combinations are safe.

SHM adds a texture upload that DMA-BUF sharing can avoid. It preserves GLES2
composition, unlike switching the entire compositor to Pixman. No measured
CPU ranking is claimed: the main rig has no accessible DRM render node, so
its software-rendered tests cannot reproduce laptop2's three hardware paths.

| Arithmetic estimate | Value |
| --- | --- |
| Tight 597×307 ARGB8888 payload | 733,116 bytes = 0.699 MiB/frame |
| Reported DMA source allocation rows, stride 2560 | 785,920 bytes = 0.750 MiB/frame, excluding offset/allocation overhead |
| One same-sized pane fully uploaded at 60 Hz | 41.95 MiB/s of pixel payload |
| Four such panes fully uploaded at 60 Hz | 167.80 MiB/s of pixel payload |

These are payload calculations, not measured memory traffic, bus transfers,
frame rate or CPU usage. Idle panes and damage-only updates reduce uploads;
resizes and incompatible format/size changes may require full uploads.
The integrated APU shares system memory, and client rendering/allocation costs
may also change with SHM. Use the existing
[CPU measurement tool](</home/sc7/projects/opensource projects/sc7-rack/scripts/measure-rack-renderer-cpu.py>) on laptop2 with equal durations, sizes and interaction workload for
GLES2/SHM and Pixman/SHM. A GLES2/DMA-BUF cost baseline would require a separately
preserved previous build: the new production policy also applies to ordinary
diagnostic sessions, so `observe` is not a DMA-BUF opt-in. No current three-way
measurement is claimed. The tool measures Rack and, optionally, host COSMIC
CPU; it does not measure the clients or GPU usage.

## Required physical gate and remaining blockers

Another dry physical install must run **plain `rack`**, with fresh/quarantined
Rack state and a new terminal after installation. Dashboard ↔ CPU, repeated
outer resizing and a long Monitor session must stay clean. Menus, DnD in both
directions, clipboard in both directions, host-open for common file types,
Files New Folder/Copy/Move To/Extract, closing/reopening Rack and helper cleanup
must also pass. Automated SHM protocol tests and main-rig controls do not
replace that gate.

The separately reported **long-session Files mutation failure** remains a
release blocker until reproduced/diagnosed or cleared by meaningful validation.
Closing/reopening restoring Files operations is evidence to retain, not a
renderer fix. The underlying DMA-BUF/Radeon path and laptop2 performance remain
open investigations. No tag, release or v0.1-ready claim follows from this
candidate alone.
