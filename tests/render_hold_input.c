#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_buffer.h>
#include "render_hold_input.h"

#define MAX_HELD_INPUTS 64

extern FILE *sc7_render_trace_file(void) __attribute__((weak));
extern uint64_t sc7_render_watch_buffer(struct wlr_buffer *) __attribute__((weak));
extern uint64_t sc7_render_trace_time_us(void) __attribute__((weak));
extern uint64_t sc7_render_trace_frame(void) __attribute__((weak));

static struct wlr_buffer *held[MAX_HELD_INPUTS];
static size_t held_count;
static bool finishing;

static bool enabled(void) {
    const char *mode = getenv("SC7_RENDER_EXPERIMENT");
    return mode && strcmp(mode, "hold-input") == 0;
}

static void trace(const char *event, struct wlr_buffer *buffer,
        const char *reason, size_t count) {
    uint64_t generation = buffer && sc7_render_watch_buffer ?
        sc7_render_watch_buffer(buffer) : 0;
    /* Watch may rotate the log; acquire FILE only after watching. */
    FILE *file = sc7_render_trace_file ? sc7_render_trace_file() : NULL;
    if (!file) {
        if (strcmp(event, "hold-input-failed") == 0) {
            fprintf(stderr, "[sc7-render] %s: %s (held=%zu)\n", event, reason, count);
        }
        return;
    }
    uint64_t time = sc7_render_trace_time_us ? sc7_render_trace_time_us() : 0;
    fprintf(file, "time_us=%" PRIu64 " %s frame=%" PRIu64 " source=%p generation=%" PRIu64
        " held=%zu reason=%s\n", time, event,
        sc7_render_trace_frame ? sc7_render_trace_frame() : 0, (void *)buffer,
        generation, count, reason);
    fflush(file);
}

size_t sc7_render_held_input_count(void) {
    return held_count;
}

bool sc7_render_hold_input(struct wlr_buffer *buffer) {
    if (!enabled()) {
        return true;
    }
    if (finishing) {
        trace("hold-input-failed", buffer, "finish-in-progress", held_count);
        return false;
    }
    if (!buffer || buffer->n_locks == 0) {
        trace("hold-input-failed", buffer, "source-not-consumer-locked", held_count);
        return false;
    }
    struct wlr_dmabuf_attributes attributes;
    if (!wlr_buffer_get_dmabuf(buffer, &attributes)) {
        trace("hold-input-failed", buffer, "source-is-not-dmabuf", held_count);
        return false;
    }
    for (size_t i = 0; i < held_count; i++) {
        if (held[i] == buffer) {
            return true;
        }
    }
    if (held_count == MAX_HELD_INPUTS) {
        trace("hold-input-failed", buffer, "capacity-64-sources", held_count);
        return false;
    }
    held[held_count++] = wlr_buffer_lock(buffer);
    trace("hold-input-retain", buffer, "until-gpu-finish", held_count);
    return true;
}

size_t sc7_render_finish_held_inputs(void (*gpu_finish)(void)) {
    if (held_count == 0) {
        return 0;
    }
    if (!gpu_finish || finishing) {
        trace("hold-input-failed", NULL,
            gpu_finish ? "finish-in-progress" : "gpu-finish-unavailable", held_count);
        return 0;
    }
    finishing = true;
    size_t count = held_count;
    trace("hold-input-gpu-finish-begin", NULL, "before-source-unlock", count);
    gpu_finish();
    trace("hold-input-gpu-finish-end", NULL, "safe-to-unlock", count);
    /* Clear each slot before unlock: destroy/release listeners can execute
     * reentrantly, and must never see a dangling source in this array. */
    while (held_count > 0) {
        struct wlr_buffer *buffer = held[--held_count];
        held[held_count] = NULL;
        trace("hold-input-unlock", buffer, "after-gpu-finish", held_count);
        wlr_buffer_unlock(buffer);
    }
    finishing = false;
    return count;
}
