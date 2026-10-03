/* Exercise real wlroots buffer/addon/swapchain lifetimes without a GPU. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/gles2.h>
#include <wlr/render/swapchain.h>
#include <wlr/util/addon.h>

#include "render_output_experiment.h"

static bool fail_allocation;
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t count, size_t size) {
    if (fail_allocation) {
        fail_allocation = false;
        return NULL;
    }
    return __real_calloc(count, size);
}

/* Only the renderer-type branch is stubbed; buffer and swapchain functions
 * below all come from the pinned real libwlroots. No GL import is claimed. */
static struct wlr_renderer gles2_renderer;
static struct wlr_renderer pixman_renderer;
bool wlr_renderer_is_gles2(struct wlr_renderer *renderer) {
    return renderer == &gles2_renderer;
}

static unsigned destroyed, released, allocated, cached_resources_destroyed;

struct fixture_buffer {
    struct wlr_buffer base;
    bool dmabuf;
    int fd;
    struct wl_listener release;
};

static void fixture_release(struct wl_listener *listener, void *data) {
    (void)listener;
    (void)data;
    ++released;
}

static void fixture_destroy(struct wlr_buffer *buffer) {
    struct fixture_buffer *fixture = wl_container_of(buffer, fixture, base);
    wl_list_remove(&fixture->release.link);
    close(fixture->fd);
    ++destroyed;
    free(fixture);
}

static bool fixture_dmabuf(struct wlr_buffer *buffer,
        struct wlr_dmabuf_attributes *attributes) {
    struct fixture_buffer *fixture = wl_container_of(buffer, fixture, base);
    if (!fixture->dmabuf) {
        return false;
    }
    *attributes = (struct wlr_dmabuf_attributes){
        .width = buffer->width, .height = buffer->height,
        .format = 0x34325258, .modifier = 0xabc,
        .n_planes = 1, .offset = {0}, .stride = {buffer->width * 4},
        .fd = {fixture->fd},
    };
    return true;
}

static const struct wlr_buffer_impl fixture_impl = {
    .destroy = fixture_destroy, .get_dmabuf = fixture_dmabuf,
};

static struct wlr_buffer *new_buffer(bool dmabuf) {
    struct fixture_buffer *fixture = calloc(1, sizeof(*fixture));
    assert(fixture);
    fixture->fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    assert(fixture->fd >= 0);
    fixture->dmabuf = dmabuf;
    wlr_buffer_init(&fixture->base, &fixture_impl, 192, 108);
    fixture->release.notify = fixture_release;
    wl_signal_add(&fixture->base.events.release, &fixture->release);
    ++allocated;
    return &fixture->base;
}

struct cached_resource {
    struct wlr_addon addon;
    struct wlr_buffer *source;
};

static void cache_destroy(struct wlr_addon *addon) {
    struct cached_resource *cache = wl_container_of(addon, cache, addon);
    /* Real GLES2 FBO/EGLImage addons must be destroyed before backing storage
     * is released. This asserts that exact boundary in wlr_buffer teardown. */
    assert(cache->source->n_locks > 0);
    assert(!cache->source->dropped || destroyed == 0);
    wlr_addon_finish(addon);
    ++cached_resources_destroyed;
}

static const struct wlr_addon_interface cache_impl = {
    .name = "test-render-target-cache", .destroy = cache_destroy,
};

static void observe_test(void) {
    const char *modes[] = {NULL, "observe", "fresh-client", "fresh-output"};
    struct wlr_buffer *source = new_buffer(true);
    for (size_t index = 0; index < sizeof(modes) / sizeof(modes[0]); ++index) {
        if (modes[index]) {
            setenv("SC7_RENDER_EXPERIMENT", modes[index], 1);
        } else {
            unsetenv("SC7_RENDER_EXPERIMENT");
        }
        struct wlr_buffer *target = sc7_render_fresh_target(&gles2_renderer, source);
        assert(target == source && source->n_locks == 0 && !source->dropped);
        sc7_render_target_drop(source, target);
        assert(!source->dropped);
    }
    setenv("SC7_RENDER_EXPERIMENT", "fresh-target", 1);
    assert(sc7_render_fresh_target(&pixman_renderer, source) == source);
    assert(sc7_render_fresh_target(NULL, source) == source);
    assert(sc7_render_fresh_target(&gles2_renderer, NULL) == NULL);
    sc7_render_target_drop(source, NULL);
    assert(source->n_locks == 0 && !source->dropped);
    wlr_buffer_drop(source);
    assert(destroyed == allocated);
}

static void target_lifetime_test(void) {
    setenv("SC7_RENDER_EXPERIMENT", "fresh-target", 1);
    struct wlr_buffer *source = new_buffer(true);
    struct wlr_dmabuf_attributes original, wrapped;
    assert(wlr_buffer_get_dmabuf(source, &original));
    struct wlr_buffer *target = sc7_render_fresh_target(&gles2_renderer, source);
    assert(target && target != source && target->n_locks == 0);
    assert(source->n_locks == 1 && target->width == source->width &&
        target->height == source->height);
    assert(wlr_buffer_get_dmabuf(target, &wrapped));
    assert(wrapped.width == original.width && wrapped.height == original.height &&
        wrapped.format == original.format && wrapped.modifier == original.modifier &&
        wrapped.n_planes == original.n_planes && wrapped.fd[0] == original.fd[0] &&
        wrapped.offset[0] == original.offset[0] && wrapped.stride[0] == original.stride[0]);
    struct cached_resource cache = {.source = source};
    wlr_addon_init(&cache.addon, &target->addons, &cache, &cache_impl);
    /* begin_gles2_buffer_pass holds one target consumer until pass submit. */
    wlr_buffer_lock(target);
    sc7_render_target_drop(source, target);
    assert(target->dropped && source->n_locks == 1 && destroyed == 0);
    wlr_buffer_drop(source);
    assert(source->dropped && fcntl(original.fd[0], F_GETFD) >= 0);
    wlr_buffer_unlock(target);
    assert(cached_resources_destroyed == 1 && released == 1 && destroyed == 1);
    assert(fcntl(original.fd[0], F_GETFD) == -1 && errno == EBADF);
}

static void target_failure_test(void) {
    setenv("SC7_RENDER_EXPERIMENT", "fresh-target", 1);
    struct wlr_buffer *shm = new_buffer(false);
    assert(sc7_render_fresh_target(&gles2_renderer, shm) == NULL);
    assert(shm->n_locks == 0 && !shm->dropped);
    wlr_buffer_drop(shm);
    struct wlr_buffer *source = new_buffer(true);
    fail_allocation = true;
    assert(sc7_render_fresh_target(&gles2_renderer, source) == NULL);
    assert(source->n_locks == 0 && !source->dropped);
    struct wlr_buffer *target = sc7_render_fresh_target(&gles2_renderer, source);
    assert(target && target != source);
    struct cached_resource cache = {.source = source};
    wlr_addon_init(&cache.addon, &target->addons, &cache, &cache_impl);
    /* A failed begin has no pass lock. Producer drop must clean the cache and
     * backing lock immediately, without dropping source's producer ref. */
    sc7_render_target_drop(source, target);
    assert(source->n_locks == 0 && !source->dropped && cached_resources_destroyed == 1);
    struct wlr_dmabuf_attributes attributes;
    assert(wlr_buffer_get_dmabuf(source, &attributes));
    assert(fcntl(attributes.fd[0], F_GETFD) >= 0);
    wlr_buffer_drop(source);
    assert(destroyed == allocated);
}

static void target_repeated_test(void) {
    setenv("SC7_RENDER_EXPERIMENT", "fresh-target", 1);
    struct wlr_buffer *source = new_buffer(true);
    for (int cycle = 0; cycle < 128; ++cycle) {
        struct wlr_buffer *target = sc7_render_fresh_target(&gles2_renderer, source);
        assert(target && target != source && source->n_locks == 1);
        struct cached_resource cache = {.source = source};
        wlr_addon_init(&cache.addon, &target->addons, &cache, &cache_impl);
        wlr_buffer_lock(target);
        sc7_render_target_drop(source, target);
        wlr_buffer_unlock(target);
        assert(source->n_locks == 0 && !source->dropped);
    }
    assert(cached_resources_destroyed == 128 && released == 128 && destroyed == 0);
    wlr_buffer_drop(source);
    assert(destroyed == allocated);
}

static void swapchain_observe_test(void) {
    struct wlr_buffer *buffer = new_buffer(true);
    struct wlr_swapchain swapchain = {0};
    swapchain.slots[0] = (struct wlr_swapchain_slot){.buffer = buffer, .age = 7};
    const char *modes[] = {"observe", "fresh-client", "fresh-target"};
    for (size_t index = 0; index < sizeof(modes) / sizeof(modes[0]); ++index) {
        setenv("SC7_RENDER_EXPERIMENT", modes[index], 1);
        assert(sc7_render_retire_released_outputs(&swapchain) == 0);
        assert(swapchain.slots[0].buffer == buffer && swapchain.slots[0].age == 7);
    }
    assert(sc7_render_retire_released_outputs(NULL) == 0);
    wlr_buffer_drop(buffer);
    assert(destroyed == allocated);
}

static void swapchain_safety_test(void) {
    setenv("SC7_RENDER_EXPERIMENT", "fresh-output", 1);
    struct wlr_buffer *buffers[4];
    struct wlr_swapchain swapchain = {0};
    for (int index = 0; index < 4; ++index) {
        buffers[index] = new_buffer(true);
        swapchain.slots[index] = (struct wlr_swapchain_slot){
            .buffer = buffers[index], .age = index + 2,
        };
    }
    swapchain.slots[1].acquired = true;
    wlr_buffer_lock(buffers[1]); /* host has not released */
    wlr_buffer_lock(buffers[2]); /* lock must protect even a false acquired flag */
    swapchain.slots[3].acquired = true; /* flag must protect even zero locks */
    assert(sc7_render_retire_released_outputs(&swapchain) == 1);
    assert(destroyed == 1 && swapchain.slots[0].buffer == NULL &&
        swapchain.slots[0].age == 0 && !swapchain.slots[0].acquired);
    for (int index = 1; index < 4; ++index) {
        assert(swapchain.slots[index].buffer == buffers[index] &&
            swapchain.slots[index].age == index + 2 && !buffers[index]->dropped);
    }
    assert(sc7_render_retire_released_outputs(&swapchain) == 0);
    wlr_buffer_unlock(buffers[1]);
    wlr_buffer_unlock(buffers[2]);
    for (int index = 1; index < 4; ++index) {
        wlr_buffer_drop(buffers[index]);
    }
    assert(destroyed == allocated);
}

static bool fail_allocator;
static struct wlr_buffer *allocator_create(struct wlr_allocator *allocator,
        int width, int height, const struct wlr_drm_format *format) {
    (void)allocator;
    (void)format;
    if (fail_allocator) {
        return NULL;
    }
    struct wlr_buffer *buffer = new_buffer(true);
    buffer->width = width;
    buffer->height = height;
    return buffer;
}

static void allocator_destroy(struct wlr_allocator *allocator) {
    (void)allocator;
}

static const struct wlr_allocator_interface allocator_impl = {
    .create_buffer = allocator_create, .destroy = allocator_destroy,
};

static struct wlr_swapchain *new_swapchain(struct wlr_allocator *allocator) {
    wlr_allocator_init(allocator, &allocator_impl, WLR_BUFFER_CAP_DMABUF);
    uint64_t modifier = 0;
    struct wlr_drm_format format = {
        .format = 0x34325258, .len = 1, .capacity = 1, .modifiers = &modifier,
    };
    struct wlr_swapchain *swapchain = wlr_swapchain_create(allocator, 192, 108, &format);
    assert(swapchain);
    return swapchain;
}

static void swapchain_cycles_test(void) {
    setenv("SC7_RENDER_EXPERIMENT", "fresh-output", 1);
    struct wlr_allocator allocator;
    struct wlr_swapchain *swapchain = new_swapchain(&allocator);
    for (int cycle = 0; cycle < 128; ++cycle) {
        int age = -1;
        assert(sc7_render_retire_released_outputs(swapchain) == (cycle ? 1 : 0));
        struct wlr_buffer *buffer = wlr_swapchain_acquire(swapchain, &age);
        assert(buffer && age == 0 && buffer->n_locks == 1);
        wlr_swapchain_set_buffer_submitted(swapchain, buffer);
        /* Host acquires a second lock. Rack's unlock must not make it eligible
         * for retirement; only the host release clears acquired. */
        wlr_buffer_lock(buffer);
        wlr_buffer_unlock(buffer);
        assert(sc7_render_retire_released_outputs(swapchain) == 0);
        wlr_buffer_unlock(buffer);
    }
    assert(allocated == 128 && destroyed == 127);
    assert(sc7_render_retire_released_outputs(swapchain) == 1);
    wlr_swapchain_destroy(swapchain);
    wlr_allocator_destroy(&allocator);
    assert(destroyed == allocated);
}

static void swapchain_failure_test(void) {
    setenv("SC7_RENDER_EXPERIMENT", "fresh-output", 1);
    struct wlr_allocator allocator;
    struct wlr_swapchain *swapchain = new_swapchain(&allocator);
    struct wlr_buffer *host_owned[WLR_SWAPCHAIN_CAP];
    int age = -1;
    for (int index = 0; index < WLR_SWAPCHAIN_CAP; ++index) {
        host_owned[index] = wlr_swapchain_acquire(swapchain, &age);
        assert(host_owned[index] && age == 0);
    }
    assert(sc7_render_retire_released_outputs(swapchain) == 0);
    assert(wlr_swapchain_acquire(swapchain, &age) == NULL);
    assert(allocated == WLR_SWAPCHAIN_CAP && destroyed == 0);
    wlr_buffer_unlock(host_owned[0]);
    assert(sc7_render_retire_released_outputs(swapchain) == 1);
    fail_allocator = true;
    assert(wlr_swapchain_acquire(swapchain, &age) == NULL);
    assert(allocated == WLR_SWAPCHAIN_CAP && destroyed == 1);
    unsigned occupied = 0;
    for (int index = 0; index < WLR_SWAPCHAIN_CAP; ++index) {
        if (swapchain->slots[index].buffer) {
            assert(swapchain->slots[index].acquired);
            assert(swapchain->slots[index].buffer->n_locks == 1);
            ++occupied;
        }
    }
    assert(occupied == WLR_SWAPCHAIN_CAP - 1);
    fail_allocator = false;
    struct wlr_buffer *recovered = wlr_swapchain_acquire(swapchain, &age);
    assert(recovered && age == 0 && allocated == WLR_SWAPCHAIN_CAP + 1);
    wlr_buffer_unlock(recovered);
    for (int index = 1; index < WLR_SWAPCHAIN_CAP; ++index) {
        wlr_buffer_unlock(host_owned[index]);
    }
    wlr_swapchain_destroy(swapchain);
    wlr_allocator_destroy(&allocator);
    assert(destroyed == allocated);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (strcmp(argv[1], "observe") == 0) observe_test();
    else if (strcmp(argv[1], "target-lifetime") == 0) target_lifetime_test();
    else if (strcmp(argv[1], "target-failure") == 0) target_failure_test();
    else if (strcmp(argv[1], "target-repeated") == 0) target_repeated_test();
    else if (strcmp(argv[1], "swapchain-observe") == 0) swapchain_observe_test();
    else if (strcmp(argv[1], "swapchain-safety") == 0) swapchain_safety_test();
    else if (strcmp(argv[1], "swapchain-cycles") == 0) swapchain_cycles_test();
    else if (strcmp(argv[1], "swapchain-failure") == 0) swapchain_failure_test();
    else return 2;
    puts("PASS");
    return 0;
}
