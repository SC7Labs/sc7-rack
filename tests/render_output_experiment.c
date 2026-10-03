/* Test-only resource lifetime experiments; never linked into normal Rack. */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/gles2.h>
#include <wlr/render/swapchain.h>

#include "render_output_experiment.h"

/* Optional logging supplied by the diagnostic tracer. */
extern FILE *sc7_render_trace_file(void) __attribute__((weak));
extern uint64_t sc7_render_watch_buffer(struct wlr_buffer *) __attribute__((weak));
extern uint64_t sc7_render_trace_time_us(void) __attribute__((weak));

struct fresh_target {
    struct wlr_buffer base;
    struct wlr_buffer *source;
    struct wlr_dmabuf_attributes attributes;
};

static bool experiment_is(const char *wanted) {
    const char *mode = getenv("SC7_RENDER_EXPERIMENT");
    return mode && strcmp(mode, wanted) == 0;
}

static FILE *trace_file(void) {
    return sc7_render_trace_file ? sc7_render_trace_file() : NULL;
}

static uint64_t trace_time(void) {
    return sc7_render_trace_time_us ? sc7_render_trace_time_us() : 0;
}

static uint64_t watch_buffer(struct wlr_buffer *buffer) {
    return sc7_render_watch_buffer ? sc7_render_watch_buffer(buffer) : 0;
}

static void target_destroy(struct wlr_buffer *buffer) {
    struct fresh_target *target = wl_container_of(buffer, target, base);
    FILE *file = trace_file();
    if (file) {
        fprintf(file, "%" PRIu64 " fresh-target-destroy target=%p source=%p"
            " source_locks=%zu\n", trace_time(), (void *)buffer,
            (void *)target->source, target->source->n_locks);
    }
    /* wlr_buffer destroys all target addons (FBO/EGLImage) before invoking
     * this callback. Keep the backing storage locked until those are gone.
     * The borrowed DMA-BUF descriptors belong to source and are not closed. */
    wlr_buffer_unlock(target->source);
    free(target);
}

static bool target_get_dmabuf(struct wlr_buffer *buffer,
        struct wlr_dmabuf_attributes *attributes) {
    struct fresh_target *target = wl_container_of(buffer, target, base);
    *attributes = target->attributes;
    return true;
}

static const struct wlr_buffer_impl target_impl = {
    .destroy = target_destroy,
    .get_dmabuf = target_get_dmabuf,
};

struct wlr_buffer *sc7_render_fresh_target(struct wlr_renderer *renderer,
        struct wlr_buffer *source) {
    if (!experiment_is("fresh-target") || !renderer || !source ||
            !wlr_renderer_is_gles2(renderer)) {
        return source;
    }
    struct wlr_dmabuf_attributes attributes = {0};
    if (!wlr_buffer_get_dmabuf(source, &attributes)) {
        FILE *file = trace_file();
        if (file) {
            fprintf(file, "%" PRIu64 " fresh-target-failed source=%p reason=no-dmabuf\n",
                trace_time(), (void *)source);
        }
        return NULL;
    }
    struct fresh_target *target = calloc(1, sizeof(*target));
    if (!target) {
        FILE *file = trace_file();
        if (file) {
            fprintf(file, "%" PRIu64 " fresh-target-failed source=%p reason=allocation\n",
                trace_time(), (void *)source);
        }
        return NULL;
    }
    wlr_buffer_init(&target->base, &target_impl, source->width, source->height);
    target->source = wlr_buffer_lock(source);
    target->attributes = attributes;
    uint64_t source_generation = watch_buffer(source);
    uint64_t target_generation = watch_buffer(&target->base);
    FILE *file = trace_file();
    if (file) {
        fprintf(file, "%" PRIu64 " fresh-target source=%p source_generation=%" PRIu64
            " target=%p target_generation=%" PRIu64 " size=%dx%d source_locks=%zu\n",
            trace_time(), (void *)source, source_generation, (void *)&target->base,
            target_generation, source->width, source->height, source->n_locks);
    }
    return &target->base;
}

void sc7_render_target_drop(struct wlr_buffer *source, struct wlr_buffer *target) {
    if (target && target != source) {
        wlr_buffer_drop(target);
    }
}

size_t sc7_render_retire_released_outputs(struct wlr_swapchain *swapchain) {
    if (!experiment_is("fresh-output") || !swapchain) {
        return 0;
    }
    size_t retired = 0;
    for (size_t index = 0; index < WLR_SWAPCHAIN_CAP; ++index) {
        struct wlr_swapchain_slot *slot = &swapchain->slots[index];
        struct wlr_buffer *buffer = slot->buffer;
        if (!buffer || slot->acquired || buffer->n_locks != 0) {
            continue;
        }
        uint64_t generation = watch_buffer(buffer);
        FILE *file = trace_file();
        if (file) {
            fprintf(file, "%" PRIu64 " fresh-output-retire swapchain=%p slot=%zu"
                " buffer=%p generation=%" PRIu64 " size=%dx%d age=%d locks=%zu\n",
                trace_time(), (void *)swapchain, index, (void *)buffer,
                generation, buffer->width, buffer->height, slot->age, buffer->n_locks);
        }
        /* slot_handle_release already unlinked the listener. Clear the slot
         * before dropping storage so destruction callbacks see consistent
         * state. A replacement allocation starts at age zero/full damage. */
        *slot = (struct wlr_swapchain_slot){0};
        wlr_buffer_drop(buffer);
        ++retired;
    }
    return retired;
}
