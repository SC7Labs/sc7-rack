/* Test-only texture-cache isolation, loaded into the private Sway process.
 * This is an experiment, not a renderer fix. No behavior changes unless the
 * exact SC7_RENDER_EXPERIMENT=fresh-input mode is selected. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/render/gles2.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include "render/gles2.h"
#include "types/wlr_buffer.h"

/* The combined telemetry library provides bounded logging. Standalone
 * lifetime regressions intentionally do not need a log file. */
extern FILE *sc7_render_trace_file(void) __attribute__((weak));
extern uint64_t sc7_render_watch_buffer(struct wlr_buffer *) __attribute__((weak));
extern uint64_t sc7_render_trace_time_us(void) __attribute__((weak));

struct retained_input {
    struct wl_list link;
    struct wlr_texture *texture;
    struct wlr_buffer *source;
    uint64_t import_generation;
    uint64_t source_generation;
};

static struct wl_list retained_inputs = {&retained_inputs, &retained_inputs};
static uint64_t import_generation;
static _Thread_local unsigned importing_factory;

static bool fresh_input_enabled(void) {
    const char *mode = getenv("SC7_RENDER_EXPERIMENT");
    return mode && strcmp(mode, "fresh-input") == 0;
}

static FILE *experiment_log(void) {
    return sc7_render_trace_file ? sc7_render_trace_file() : NULL;
}

static uint64_t timestamp(void) {
    if (sc7_render_trace_time_us) {
        return sc7_render_trace_time_us();
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static uint64_t source_generation(struct wlr_buffer *source) {
    return sc7_render_watch_buffer ? sc7_render_watch_buffer(source) : 0;
}

struct input_attributes {
    const char *kind;
    uint32_t format;
    uint64_t modifier;
    int planes;
};

static struct input_attributes input_attributes(struct wlr_buffer *buffer) {
    struct wlr_dmabuf_attributes dma;
    struct wlr_shm_attributes shm;
    if (wlr_buffer_get_dmabuf(buffer, &dma)) {
        return (struct input_attributes){"dmabuf", dma.format, dma.modifier,
            dma.n_planes};
    }
    if (wlr_buffer_get_shm(buffer, &shm)) {
        return (struct input_attributes){"shm", shm.format, 0, 0};
    }
    return (struct input_attributes){"data/other", 0, 0, 0};
}

static void log_import(struct wlr_buffer *source, struct wlr_texture *texture,
        struct wlr_renderer *renderer, uint64_t generation, uint64_t source_id,
        const char *event, const struct input_attributes *attributes) {
    FILE *file = experiment_log();
    if (!file) {
        return;
    }
    struct wlr_gles2_texture *gles = NULL;
    if (texture && wlr_texture_is_gles2(texture)) {
        gles = (void *)texture;
    }
    fprintf(file, "%" PRIu64 " %s import=%" PRIu64
        " source=%p source_generation=%" PRIu64
        " kind=%s renderer=%s size=%dx%d texture=%p wrapper=%p tex=%u target=0x%x"
        " egl_image=%p format=0x%08" PRIx32 " modifier=0x%016" PRIx64
        " planes=%d source_locks=%zu ok=%d\n",
        timestamp(), event, generation, (void *)source, source_id, attributes->kind,
        wlr_renderer_is_gles2(renderer) ? "gles2" :
        wlr_renderer_is_pixman(renderer) ? "pixman" : "other",
        source->width, source->height, (void *)texture,
        gles ? (void *)gles->buffer : NULL, gles ? gles->tex : 0,
        gles ? gles->target : 0, gles ? (void *)gles->image : NULL,
        attributes->format, attributes->modifier, attributes->planes,
        source->n_locks, texture != NULL);
}

bool wlr_client_buffer_apply_damage(struct wlr_client_buffer *client,
        struct wlr_buffer *buffer, const pixman_region32_t *damage) {
    static bool (*next)(struct wlr_client_buffer *, struct wlr_buffer *,
        const pixman_region32_t *);
    if (!next) {
        next = dlsym(RTLD_NEXT, "wlr_client_buffer_apply_damage");
    }
    /* client->source is not updated when SHM texture mutation succeeds. Log
     * the incoming buffer directly; that is the source of this upload. */
    uint64_t source_id = source_generation(buffer);
    struct input_attributes attributes = input_attributes(buffer);
    if (fresh_input_enabled()) {
        /* Returning false uses wlroots' existing full re-creation path. SHM
         * receives a complete upload; DMA-BUF receives a new import below. */
        /* Watching a new buffer may rotate/close the shared trace file. Acquire
         * the FILE only after that helper has finished writing to the log. */
        FILE *file = experiment_log();
        if (file) {
            fprintf(file, "%" PRIu64 " fresh-input-reject-update client=%p"
                " texture=%p source=%p source_generation=%" PRIu64
                " kind=%s format=0x%08" PRIx32 " modifier=0x%016" PRIx64 "\n",
                timestamp(), (void *)client, (void *)client->texture,
                (void *)buffer, source_id, attributes.kind,
                attributes.format, attributes.modifier);
        }
        return false;
    }
    bool ok = next(client, buffer, damage);
    FILE *file = experiment_log();
    if (file) {
        fprintf(file, "%" PRIu64 " texture-update client=%p texture=%p"
            " source=%p source_generation=%" PRIu64 " kind=%s size=%dx%d"
            " format=0x%08" PRIx32 " modifier=0x%016" PRIx64 " ok=%d\n",
            timestamp(), (void *)client, (void *)client->texture, (void *)buffer,
            source_id, attributes.kind, buffer->width, buffer->height,
            attributes.format, attributes.modifier, ok);
    }
    return ok;
}

struct wlr_texture *wlr_texture_from_buffer(struct wlr_renderer *renderer,
        struct wlr_buffer *buffer) {
    static struct wlr_texture *(*next)(struct wlr_renderer *, struct wlr_buffer *);
    if (!next) {
        next = dlsym(RTLD_NEXT, "wlr_texture_from_buffer");
    }
    if (importing_factory) {
        return next(renderer, buffer);
    }

    uint64_t generation = ++import_generation;
    uint64_t source_id = source_generation(buffer);
    struct input_attributes attributes = input_attributes(buffer);
    if (!fresh_input_enabled() || !wlr_renderer_is_gles2(renderer)) {
        ++importing_factory;
        struct wlr_texture *texture = next(renderer, buffer);
        --importing_factory;
        log_import(buffer, texture, renderer, generation, source_id,
            "texture-import", &attributes);
        return texture;
    }
    struct wlr_dmabuf_attributes dma;
    if (!wlr_buffer_get_dmabuf(buffer, &dma)) {
        /* The GLES2 SHM path allocates a texture and copies all pixels. With
         * apply_damage rejected above, it has no cached partial upload. */
        ++importing_factory;
        struct wlr_texture *texture = next(renderer, buffer);
        --importing_factory;
        log_import(buffer, texture, renderer, generation, source_id,
            "fresh-input-import", &attributes);
        return texture;
    }

    struct retained_input *retained = calloc(1, sizeof(*retained));
    if (!retained) {
        log_import(buffer, NULL, renderer, generation, source_id,
            "fresh-input-import", &attributes);
        return NULL;
    }
    retained->source = wlr_buffer_lock(buffer);
    retained->source_generation = source_id;
    retained->import_generation = generation;

    /* This public factory creates a unique temporary wlr_dmabuf_buffer. The
     * GLES2 addon cache therefore cannot find the original buffer's texture.
     * The factory duplicates DMA-BUF FDs when dropping its wrapper. Retaining
     * the ORIGINAL source is equally necessary: FDs alone would let wl_buffer
     * release reach the producer while the fresh texture still reads it.
     * A saved-view client wrapper may itself be this original source. */
    ++importing_factory;
    struct wlr_texture *texture = wlr_texture_from_dmabuf(renderer, &dma);
    --importing_factory;
    log_import(buffer, texture, renderer, generation, source_id,
        "fresh-input-import", &attributes);
    if (!texture) {
        wlr_buffer_unlock(retained->source);
        free(retained);
        return NULL;
    }
    retained->texture = texture;
    wl_list_insert(&retained_inputs, &retained->link);
    return texture;
}

void wlr_texture_destroy(struct wlr_texture *texture) {
    static void (*next)(struct wlr_texture *);
    if (!next) {
        next = dlsym(RTLD_NEXT, "wlr_texture_destroy");
    }
    struct retained_input *entry, *retained = NULL;
    wl_list_for_each(entry, &retained_inputs, link) {
        if (entry->texture == texture) {
            retained = entry;
            wl_list_remove(&retained->link);
            break;
        }
    }
    if (retained) {
        FILE *file = experiment_log();
        if (file) {
            fprintf(file, "%" PRIu64 " fresh-input-destroy import=%" PRIu64
                " texture=%p source=%p source_generation=%" PRIu64
                " source_locks=%zu\n", timestamp(), retained->import_generation,
                (void *)texture, (void *)retained->source,
                retained->source_generation, retained->source->n_locks);
        }
    }
    /* Delete/unreference the imported GPU resource before making its original
     * buffer reusable. Removing the entry first permits reentrant destruction
     * when releasing a saved-view wrapper destroys its own source texture. */
    next(texture);
    if (retained) {
        wlr_buffer_unlock(retained->source);
        free(retained);
    }
}
