/* A real pinned Pixman pass emits buffer.release during submit. The diagnostic
 * frame accessor must still return THIS frame inside that signal callback. */
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/pass.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_damage_ring.h>

struct fixture {
    struct wlr_buffer base;
    uint32_t pixels[64 * 64];
    struct wl_listener release;
};

static uint64_t (*current_frame)(void);
static uint64_t expected_frame;
static unsigned releases, destroyed;

static void buffer_destroy(struct wlr_buffer *buffer) {
    struct fixture *fixture = wl_container_of(buffer, fixture, base);
    wl_list_remove(&fixture->release.link);
    ++destroyed;
    free(fixture);
}

static bool buffer_access(struct wlr_buffer *buffer, uint32_t flags,
        void **data, uint32_t *format, size_t *stride) {
    (void)flags;
    struct fixture *fixture = wl_container_of(buffer, fixture, base);
    *data = fixture->pixels;
    *format = 0x34325258; /* DRM_FORMAT_XRGB8888 */
    *stride = 64 * 4;
    return true;
}

static void buffer_access_end(struct wlr_buffer *buffer) { (void)buffer; }
static const struct wlr_buffer_impl buffer_impl = {
    .destroy = buffer_destroy,
    .begin_data_ptr_access = buffer_access,
    .end_data_ptr_access = buffer_access_end,
};

static void buffer_release(struct wl_listener *listener, void *data) {
    (void)data;
    struct fixture *fixture = wl_container_of(listener, fixture, release);
    assert(fixture->base.n_locks == 0 && !fixture->base.accessing_data_ptr);
    assert(current_frame() == expected_frame);
    ++releases;
    printf("release-callback frame=%" PRIu64 " expected=%" PRIu64 "\n",
        current_frame(), expected_frame);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    unsigned cycles = (unsigned)strtoul(argv[1], NULL, 10);
    assert(cycles > 0 && cycles <= 128);
    current_frame = dlsym(RTLD_DEFAULT, "sc7_render_trace_frame");
    assert(current_frame && current_frame() == 0);
    struct wlr_renderer *renderer = wlr_pixman_renderer_create();
    assert(renderer);
    struct fixture *fixture = calloc(1, sizeof(*fixture));
    assert(fixture);
    wlr_buffer_init(&fixture->base, &buffer_impl, 64, 64);
    fixture->release.notify = buffer_release;
    wl_signal_add(&fixture->base.events.release, &fixture->release);
    struct wlr_damage_ring ring;
    wlr_damage_ring_init(&ring);
    wlr_damage_ring_set_bounds(&ring, 64, 64);
    pixman_region32_t damage;
    pixman_region32_init(&damage);
    const char *late_trigger = getenv("SC7_TEST_LATE_TRIGGER");
    for (unsigned cycle = 1; cycle <= cycles; ++cycle) {
        expected_frame = cycle;
        struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(renderer,
            &fixture->base, NULL);
        assert(pass && fixture->base.n_locks == 1);
        assert(current_frame() == expected_frame);
        wlr_damage_ring_get_buffer_damage(&ring, 0, &damage);
        assert(current_frame() == expected_frame);
        if (late_trigger && cycle == 1) {
            FILE *request = fopen(late_trigger, "w");
            assert(request && fclose(request) == 0);
        }
        struct wlr_render_rect_options rectangle = {
            .box = {.x = 0, .y = 0, .width = 64, .height = 64},
            .color = {.r = 1, .g = 0, .b = 0, .a = 1},
        };
        wlr_render_pass_add_rect(pass, &rectangle);
        assert(wlr_render_pass_submit(pass));
        assert(current_frame() == expected_frame);
        assert(releases == cycle && fixture->base.n_locks == 0);
    }
    pixman_region32_fini(&damage);
    wlr_damage_ring_finish(&ring);
    wlr_buffer_drop(&fixture->base);
    assert(destroyed == 1);
    wlr_renderer_destroy(renderer);
    printf("frame correlation assertions passed cycles=%u\n", cycles);
    return 0;
}
