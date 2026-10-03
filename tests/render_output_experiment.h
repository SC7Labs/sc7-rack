#ifndef SC7_RENDER_OUTPUT_EXPERIMENT_H
#define SC7_RENDER_OUTPUT_EXPERIMENT_H

#include <stddef.h>

struct wlr_buffer;
struct wlr_renderer;
struct wlr_swapchain;

/*
 * Test-only, opt-in helpers for the pinned wlroots 0.17.4 diagnostic preload.
 * The returned target is source unless fresh-target is selected for GLES2.
 * A new target owns a producer reference and a consumer lock on source. NULL
 * means that the requested experiment failed, rather than silently observing.
 */
struct wlr_buffer *sc7_render_fresh_target(struct wlr_renderer *renderer,
    struct wlr_buffer *source);

/* Call after begin_buffer_pass, including its failure path. The pass retains
 * the wrapper on success. Never drops the original producer reference. */
void sc7_render_target_drop(struct wlr_buffer *source, struct wlr_buffer *target);

/* In fresh-output mode, retire released swapchain storage before acquire.
 * Never removes an acquired slot or a buffer with any consumer lock. */
size_t sc7_render_retire_released_outputs(struct wlr_swapchain *swapchain);

#endif
