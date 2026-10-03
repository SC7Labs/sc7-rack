/* Optional, test-only wlroots frame telemetry. Loaded only into private Sway. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <inttypes.h>
#include <pixman.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>

static FILE *trace_file(void) {
    static FILE *file;
    if (!file) {
        const char *path = getenv("SC7_RENDER_TRACE");
        if (path && *path) {
            file = fopen(path, "a");
            if (file) {
                setvbuf(file, NULL, _IOLBF, 0);
            }
        }
    }
    return file;
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

struct wlr_buffer *wlr_swapchain_acquire(struct wlr_swapchain *chain, int *age) {
    static struct wlr_buffer *(*next)(struct wlr_swapchain *, int *);
    if (!next) {
        next = dlsym(RTLD_NEXT, "wlr_swapchain_acquire");
    }
    struct wlr_buffer *buffer = next(chain, age);
    FILE *file = trace_file();
    if (file) {
        struct wlr_dmabuf_attributes dma;
        struct wlr_shm_attributes shm;
        const char *kind = "other";
        uint64_t modifier = 0;
        if (buffer && wlr_buffer_get_dmabuf(buffer, &dma)) {
            kind = "dmabuf";
            modifier = dma.modifier;
        } else if (buffer && wlr_buffer_get_shm(buffer, &shm)) {
            kind = "shm";
        }
        fprintf(file, "%" PRIu64 " acquire chain=%p size=%dx%d buffer=%p age=%d format=0x%08" PRIx32 " kind=%s modifier=0x%016" PRIx64 "\n",
            now_us(), (void *)chain, chain->width, chain->height,
            (void *)buffer, age ? *age : -1, chain->format.format,
            kind, modifier);
    }
    return buffer;
}

void wlr_damage_ring_get_buffer_damage(struct wlr_damage_ring *ring,
        int age, pixman_region32_t *damage) {
    static void (*next)(struct wlr_damage_ring *, int, pixman_region32_t *);
    if (!next) {
        next = dlsym(RTLD_NEXT, "wlr_damage_ring_get_buffer_damage");
    }
    next(ring, age, damage);
    FILE *file = trace_file();
    if (file) {
        int count;
        pixman_region32_rectangles(damage, &count);
        const pixman_box32_t *extents = pixman_region32_extents(damage);
        fprintf(file, "%" PRIu64 " damage ring=%p size=%dx%d age=%d rects=%d box=%d,%d,%d,%d\n",
            now_us(), (void *)ring, ring->width, ring->height, age,
            count, extents->x1, extents->y1, extents->x2, extents->y2);
    }
}

void wlr_swapchain_set_buffer_submitted(struct wlr_swapchain *chain,
        struct wlr_buffer *buffer) {
    static void (*next)(struct wlr_swapchain *, struct wlr_buffer *);
    if (!next) {
        next = dlsym(RTLD_NEXT, "wlr_swapchain_set_buffer_submitted");
    }
    FILE *file = trace_file();
    if (file) {
        fprintf(file, "%" PRIu64 " submit chain=%p size=%dx%d buffer=%p\n",
            now_us(), (void *)chain, chain->width, chain->height,
            (void *)buffer);
    }
    next(chain, buffer);
}

bool wlr_output_commit_state(struct wlr_output *output,
        const struct wlr_output_state *state) {
    static bool (*next)(struct wlr_output *, const struct wlr_output_state *);
    if (!next) {
        next = dlsym(RTLD_NEXT, "wlr_output_commit_state");
    }
    bool ok = next(output, state);
    FILE *file = trace_file();
    if (file) {
        int count = 0;
        const pixman_box32_t *extents = NULL;
        if (state->committed & WLR_OUTPUT_STATE_DAMAGE) {
            pixman_region32_rectangles(&state->damage, &count);
            extents = pixman_region32_extents((pixman_region32_t *)&state->damage);
        }
        fprintf(file, "%" PRIu64 " commit output=%s size=%dx%d seq=%" PRIu32
            " fields=0x%08" PRIx32 " buffer=%p damage_rects=%d"
            " box=%d,%d,%d,%d ok=%d\n",
            now_us(), output->name ? output->name : "<unnamed>",
            output->width, output->height, output->commit_seq,
            state->committed, (void *)state->buffer, count,
            extents ? extents->x1 : 0, extents ? extents->y1 : 0,
            extents ? extents->x2 : 0, extents ? extents->y2 : 0, ok);
    }
    return ok;
}

void wlr_surface_get_effective_damage(struct wlr_surface *surface,
        pixman_region32_t *damage) {
    static void (*next)(struct wlr_surface *, pixman_region32_t *);
    if (!next) {
        next = dlsym(RTLD_NEXT, "wlr_surface_get_effective_damage");
    }
    next(surface, damage);
    FILE *file = trace_file();
    if (file) {
        int count;
        pixman_region32_rectangles(damage, &count);
        const pixman_box32_t *extents = pixman_region32_extents(damage);
        fprintf(file, "%" PRIu64 " surface-damage surface=%p size=%dx%d"
            " rects=%d box=%d,%d,%d,%d\n", now_us(), (void *)surface,
            surface->current.width, surface->current.height, count,
            extents->x1, extents->y1, extents->x2, extents->y2);
    }
}
