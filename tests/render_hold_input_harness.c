#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <drm_fourcc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include "render_hold_input.h"

struct source {
    struct wlr_buffer base;
    bool dma, expect_finish;
    int releases, destroys;
    struct wl_listener release;
};

static struct source sources[65];
static size_t sources_count;
static int finish_calls;
static FILE *log_file;
static bool reentry;

FILE *sc7_render_trace_file(void) {
    if (!log_file) {
        log_file = tmpfile();
        assert(log_file);
    }
    return log_file;
}

uint64_t sc7_render_watch_buffer(struct wlr_buffer *buffer) {
    assert(buffer);
    return 99;
}

uint64_t sc7_render_trace_time_us(void) {
    return 777;
}

static struct source *from_buffer(struct wlr_buffer *buffer) {
    return wl_container_of(buffer, (struct source *)NULL, base);
}

static void destroy(struct wlr_buffer *buffer) {
    struct source *source = from_buffer(buffer);
    if (source->expect_finish) assert(finish_calls > 0);
    source->destroys++;
}

static bool get_dmabuf(struct wlr_buffer *buffer,
        struct wlr_dmabuf_attributes *attributes) {
    struct source *source = from_buffer(buffer);
    if (!source->dma) return false;
    *attributes = (struct wlr_dmabuf_attributes){
        .width = 2, .height = 2, .format = DRM_FORMAT_ARGB8888,
        .modifier = DRM_FORMAT_MOD_LINEAR, .n_planes = 1,
        .fd = {-1}, .stride = {8},
    };
    return true;
}

static const struct wlr_buffer_impl impl = {
    .destroy = destroy,
    .get_dmabuf = get_dmabuf,
};

static void release(struct wl_listener *listener, void *unused) {
    (void)unused;
    struct source *source = wl_container_of(listener, source, release);
    if (source->expect_finish) assert(finish_calls > 0);
    source->releases++;
    if (reentry) {
        assert(!sc7_render_hold_input(&sources[sources_count - 1].base));
        assert(sc7_render_finish_held_inputs(NULL) == 0);
    }
}

static struct source *init_source(bool dma) {
    assert(sources_count < 65);
    struct source *source = &sources[sources_count++];
    *source = (struct source){ .dma = dma };
    wlr_buffer_init(&source->base, &impl, 2, 2);
    source->release.notify = release;
    wl_signal_add(&source->base.events.release, &source->release);
    wlr_buffer_lock(&source->base);
    return source;
}

static void gpu_finish(void) {
    assert(sc7_render_held_input_count() > 0);
    assert(finish_calls == 0);
    for (size_t i = 0; i < sources_count; i++) {
        struct source *source = &sources[i];
        if (source->expect_finish) {
            assert(source->base.n_locks > 0);
            assert(source->releases == 0 && source->destroys == 0);
        }
    }
    if (reentry) {
        assert(!sc7_render_hold_input(&sources[0].base));
        assert(sc7_render_finish_held_inputs(gpu_finish) == 0);
    }
    finish_calls++;
}

static void drop_producer_and_caller(struct source *source) {
    wlr_buffer_drop(&source->base);
    wlr_buffer_unlock(&source->base);
}

static void check_destroyed(struct source *source) {
    assert(source->releases == 1 && source->destroys == 1);
    assert(source->base.n_locks == 0);
    wl_list_remove(&source->release.link);
}

static void finish_logs(void) {
    if (!log_file) return;
    rewind(log_file);
    char line[512];
    while (fgets(line, sizeof(line), log_file)) fputs(line, stdout);
    fclose(log_file);
    log_file = NULL;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *test = argv[1];
    if (strcmp(test, "disabled") == 0) {
        struct source *source = init_source(true);
        assert(sc7_render_hold_input(&source->base));
        assert(sc7_render_hold_input(NULL));
        assert(source->base.n_locks == 1);
        assert(sc7_render_held_input_count() == 0);
        assert(sc7_render_finish_held_inputs(gpu_finish) == 0);
        assert(finish_calls == 0);
        drop_producer_and_caller(source);
        check_destroyed(source);
    } else if (strcmp(test, "reject") == 0) {
        assert(!sc7_render_hold_input(NULL));
        struct source *source = init_source(false);
        assert(!sc7_render_hold_input(&source->base));
        assert(source->base.n_locks == 1);
        assert(sc7_render_held_input_count() == 0);
        drop_producer_and_caller(source);
        check_destroyed(source);
    } else if (strcmp(test, "unlocked") == 0) {
        struct source *source = init_source(true);
        wlr_buffer_unlock(&source->base);
        assert(source->base.n_locks == 0 && source->releases == 1);
        assert(!sc7_render_hold_input(&source->base));
        assert(sc7_render_held_input_count() == 0);
        wlr_buffer_drop(&source->base);
        check_destroyed(source);
    } else if (strcmp(test, "capacity") == 0) {
        for (int i = 0; i < 65; i++) {
            struct source *source = init_source(true);
            assert(sc7_render_hold_input(&source->base) == (i < 64));
            source->expect_finish = i < 64;
            assert(source->base.n_locks == (size_t)(i < 64 ? 2 : 1));
        }
        assert(sc7_render_held_input_count() == 64);
        assert(sc7_render_hold_input(&sources[0].base));
        for (size_t i = 0; i < sources_count; i++) {
            drop_producer_and_caller(&sources[i]);
        }
        assert(sc7_render_finish_held_inputs(gpu_finish) == 64);
        assert(sc7_render_held_input_count() == 0 && finish_calls == 1);
        for (size_t i = 0; i < sources_count; i++) check_destroyed(&sources[i]);
    } else {
        int cycles = strcmp(test, "cycles") == 0 ? 128 : 1;
        for (int i = 0; i < cycles; i++) {
            sources_count = 0;
            finish_calls = 0;
            struct source *source = init_source(true);
            source->expect_finish = true;
            assert(sc7_render_hold_input(&source->base));
            assert(source->base.n_locks == 2);
            assert(sc7_render_hold_input(&source->base));
            assert(source->base.n_locks == 2);
            assert(sc7_render_held_input_count() == 1);
            if (strcmp(test, "reentry") == 0) {
                reentry = true;
                init_source(true);
            }
            drop_producer_and_caller(source);
            assert(source->base.n_locks == 1);
            assert(source->releases == 0 && source->destroys == 0);
            if (strcmp(test, "missing-finish") == 0) {
                assert(sc7_render_finish_held_inputs(NULL) == 0);
                assert(sc7_render_held_input_count() == 1);
                assert(source->base.n_locks == 1 && source->releases == 0);
            } else if (strcmp(test, "mode-changed") == 0) {
                unsetenv("SC7_RENDER_EXPERIMENT");
            } else if (strcmp(test, "failed-submit") == 0) {
                /* The caller must finish after a false submission result;
                 * success/failure cannot be inferred inside this helper. */
                bool submitted = false;
                assert(!submitted);
            } else {
                assert(strcmp(test, "success") == 0 || strcmp(test, "cycles") == 0 ||
                    strcmp(test, "reentry") == 0);
            }
            assert(sc7_render_finish_held_inputs(gpu_finish) == 1);
            assert(sc7_render_held_input_count() == 0 && finish_calls == 1);
            check_destroyed(source);
            assert(sc7_render_finish_held_inputs(NULL) == 0);
            if (reentry) {
                reentry = false;
                drop_producer_and_caller(&sources[1]);
                check_destroyed(&sources[1]);
            }
        }
    }
    finish_logs();
    puts("hold input assertions passed");
    return 0;
}
