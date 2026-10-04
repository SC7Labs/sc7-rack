#ifndef SC7_RENDER_RAW_SNAPSHOT_H
#define SC7_RENDER_RAW_SNAPSHOT_H

#include <stdbool.h>

struct wlr_buffer;

enum sc7_raw_snapshot_status {
    SC7_RAW_SNAPSHOT_OK,
    SC7_RAW_SNAPSHOT_INVALID_ARGUMENT,
    SC7_RAW_SNAPSHOT_SOURCE_NOT_LOCKED,
    SC7_RAW_SNAPSHOT_DIMENSION_LIMIT,
    SC7_RAW_SNAPSHOT_ACCESS_BUSY,
    SC7_RAW_SNAPSHOT_NO_DATA_PTR_IMPL,
    SC7_RAW_SNAPSHOT_ACCESS_FAILED,
    SC7_RAW_SNAPSHOT_UNSUPPORTED_FORMAT,
    SC7_RAW_SNAPSHOT_INVALID_LAYOUT,
    SC7_RAW_SNAPSHOT_OUT_OF_MEMORY,
    SC7_RAW_SNAPSHOT_FILE_FAILED,
};

/* Test-only raw READ access, never EGL sampling or guessed DMA-BUF mmap.
 * Caller must hold a source consumer lock. Writes P6 RGB plus a P5 alpha
 * sidecar `${path}.alpha.pgm`, with original row order and channel bytes
 * (no correction/unpremultiply; RGB at alpha zero is not meaningful alone).
 * ARGB/ABGR alpha is preserved; XRGB/XBGR alpha is 255. Both files are fully
 * written before publication. A failed second rename removes the new RGB
 * capture, so failure never leaves a half-published new pair. Use unique
 * capture paths: publishing is two renames, not a filesystem transaction.
 * ARGB8888/XRGB8888/ABGR8888/XBGR8888 only, at most 4096x4096.
 * Status/reason refer to this thread's most recent call, including failures. */
bool sc7_render_raw_snapshot(struct wlr_buffer *buffer, const char *path);
enum sc7_raw_snapshot_status sc7_render_raw_snapshot_last_status(void);
const char *sc7_render_raw_snapshot_reason(void);

#endif
