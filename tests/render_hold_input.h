#ifndef SC7_RENDER_HOLD_INPUT_H
#define SC7_RENDER_HOLD_INPUT_H

#include <stdbool.h>
#include <stddef.h>

struct wlr_buffer;

/* Test-only, exact SC7_RENDER_EXPERIMENT=hold-input mode. The caller must
 * already hold the actual sampled DMA-BUF source and use the source's GLES2
 * context for finish. No-op success outside the selected mode. A requested
 * failure is false and is traced, without acquiring any new reference.
 * At most 64 unique sources are retained once per output pass. */
bool sc7_render_hold_input(struct wlr_buffer *buffer);

/* Call after the real pass submission, even if submission failed. The GPU
 * finish must complete this context's queued texture reads BEFORE the extra
 * locks are released. NULL is rejected: pending locks remain owned so a
 * caller can retry after making the valid renderer context current.
 * Returns the number released, not the number left pending. */
size_t sc7_render_finish_held_inputs(void (*gpu_finish)(void));
size_t sc7_render_held_input_count(void);

#endif
