#ifndef SC7_RENDER_INPUT_CAPTURE_H
#define SC7_RENDER_INPUT_CAPTURE_H

#include <stddef.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>

enum sc7_render_input_capture_result {
    SC7_RENDER_INPUT_CAPTURE_ERROR = -1,
    SC7_RENDER_INPUT_CAPTURE_UNSUPPORTED = 0,
    SC7_RENDER_INPUT_CAPTURE_OK = 1,
};

/* Test-only: sample the exact texture in the already-current GLES context.
 * No EGL context switch or texture reimport. Restores every GL state it changes.
 * glReadPixels synchronizes the GPU and can change the timing of a race. GL error
 * flags cannot be restored: a pre-existing error is reported and capture skipped.
 * Writes an RGB PPM and `${path}.alpha.pgm` P5 alpha map, with texture coordinate
 * y=0 at the top of both images. Both files are prepared before publishing; PPM
 * is published last as the completion marker. Failed publication removes the
 * pair. Alpha is the sampled ARGB alpha or 255 for RGBX, matching wlroots.
 * The caller owns one-shot triggering, limits and source/frame correlation. */
enum sc7_render_input_capture_result sc7_render_capture_input_gles2(
    struct wlr_renderer *renderer, struct wlr_texture *texture, const char *path,
    char *reason, size_t reason_size);

#endif
