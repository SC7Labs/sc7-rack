/* Real wlroots buffer locks/signals with a fake GLES2 import/cache renderer.
 * This needs no GPU; it exercises the exact public DMA-BUF wrapper factory. */
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/gles2.h>
#include <wlr/render/interface.h>
#include <wlr/util/addon.h>
#include "types/wlr_buffer.h"

struct counts {
    int release, destroy, gpu_destroy, imports, updates;
    int gpu_destroy_order, release_order, destroy_order;
};
static struct counts textures;
static int sequence;
static bool fail_import;

/* Standalone logging helpers let the observe-mode regression check the actual
 * buffer used by an upload, independently of client_buffer->source. */
FILE *sc7_render_trace_file(void) {
    static FILE *file;
    const char *path = getenv("SC7_INPUT_TEST_LOG");
    if (!path) { return NULL; }
    if (!file) {
        file = fopen(path, "a");
        assert(file);
        setvbuf(file, NULL, _IOLBF, 0);
    }
    return file;
}

uint64_t sc7_render_watch_buffer(struct wlr_buffer *buffer) {
    return (uint64_t)(uintptr_t)buffer;
}

struct source {
    struct wlr_buffer base;
    struct wlr_dmabuf_attributes dma;
    struct counts *counts;
    struct wl_listener released, destroyed;
    bool is_dma;
    uint32_t pixels[4];
};

static struct source *source_from_buffer(struct wlr_buffer *base) {
    return wl_container_of(base, (struct source *)NULL, base);
}

static void source_destroy(struct wlr_buffer *base) {
    struct source *source = source_from_buffer(base);
    close(source->dma.fd[0]);
    free(source);
}

static bool source_dmabuf(struct wlr_buffer *base,
        struct wlr_dmabuf_attributes *attr) {
    struct source *source = source_from_buffer(base);
    if (!source->is_dma) {
        return false;
    }
    *attr = source->dma;
    return true;
}

static bool source_shm(struct wlr_buffer *base, struct wlr_shm_attributes *attr) {
    struct source *source = source_from_buffer(base);
    if (source->is_dma) {
        return false;
    }
    *attr = (struct wlr_shm_attributes){.fd = source->dma.fd[0],
        .width = 2, .height = 2, .stride = 8, .format = DRM_FORMAT_XRGB8888};
    return true;
}

static bool source_access(struct wlr_buffer *base, uint32_t flags,
        void **data, uint32_t *format, size_t *stride) {
    (void)flags;
    struct source *source = source_from_buffer(base);
    if (source->is_dma) {
        return false;
    }
    *data = source->pixels;
    *format = DRM_FORMAT_XRGB8888;
    *stride = 8;
    return true;
}

static void source_access_end(struct wlr_buffer *base) { (void)base; }
static const struct wlr_buffer_impl source_impl = {
    .destroy = source_destroy, .get_dmabuf = source_dmabuf,
    .get_shm = source_shm, .begin_data_ptr_access = source_access,
    .end_data_ptr_access = source_access_end,
};

static void released(struct wl_listener *listener, void *data) {
    (void)data;
    struct source *source = wl_container_of(listener, source, released);
    ++source->counts->release;
    source->counts->release_order = ++sequence;
}

static void destroyed(struct wl_listener *listener, void *data) {
    (void)data;
    struct source *source = wl_container_of(listener, source, destroyed);
    ++source->counts->destroy;
    source->counts->destroy_order = ++sequence;
}

static struct source *new_source(struct counts *counts, bool dma, uint32_t pixel) {
    struct source *source = calloc(1, sizeof(*source));
    assert(source);
    wlr_buffer_init(&source->base, &source_impl, 2, 2);
    source->is_dma = dma;
    source->counts = counts;
    int fd = memfd_create("fresh-input-lifetime", MFD_CLOEXEC);
    assert(fd >= 0 && ftruncate(fd, 16) == 0);
    source->dma = (struct wlr_dmabuf_attributes){.width = 2, .height = 2,
        .format = DRM_FORMAT_XRGB8888, .modifier = DRM_FORMAT_MOD_LINEAR,
        .n_planes = 1, .stride = {8}, .fd = {fd, -1, -1, -1}};
    for (size_t i = 0; i < 4; ++i) { source->pixels[i] = pixel; }
    source->released.notify = released;
    source->destroyed.notify = destroyed;
    wl_signal_add(&source->base.events.release, &source->released);
    wl_signal_add(&source->base.events.destroy, &source->destroyed);
    wlr_buffer_lock(&source->base); /* compositor's pending buffer ownership */
    return source;
}

struct fake_texture {
    struct wlr_texture base;
    struct wlr_buffer *buffer;
    struct wlr_addon addon;
    uint32_t copied_pixel;
};

static void free_texture(struct fake_texture *texture) {
    ++textures.gpu_destroy;
    textures.gpu_destroy_order = ++sequence;
    free(texture);
}

static void addon_destroy(struct wlr_addon *addon) {
    struct fake_texture *texture = wl_container_of(addon, texture, addon);
    wlr_addon_finish(addon);
    free_texture(texture);
}

static const struct wlr_addon_interface addon_impl = {
    .name = "fake-gles2-cached-texture", .destroy = addon_destroy,
};

static void texture_destroy(struct wlr_texture *base) {
    struct fake_texture *texture = wl_container_of(base, texture, base);
    if (texture->buffer) {
        wlr_buffer_unlock(texture->buffer); /* same cache behavior as GLES2 */
    } else {
        free_texture(texture);
    }
}

static bool texture_update(struct wlr_texture *base, struct wlr_buffer *buffer,
        const pixman_region32_t *damage) {
    (void)base; (void)buffer; (void)damage;
    ++textures.updates;
    return true;
}

static const struct wlr_texture_impl texture_impl = {
    .destroy = texture_destroy, .update_from_buffer = texture_update,
};

static struct wlr_texture *texture_import(struct wlr_renderer *renderer,
        struct wlr_buffer *buffer) {
    struct wlr_dmabuf_attributes dma;
    bool is_dma = wlr_buffer_get_dmabuf(buffer, &dma);
    if (is_dma) {
        assert(dma.n_planes == 1 && fcntl(dma.fd[0], F_GETFD) != -1);
        struct wlr_addon *cached = wlr_addon_find(&buffer->addons, renderer,
            &addon_impl);
        if (cached) {
            struct fake_texture *texture = wl_container_of(cached, texture, addon);
            wlr_buffer_lock(buffer);
            return &texture->base;
        }
    }
    ++textures.imports;
    if (fail_import) {
        return NULL;
    }
    struct fake_texture *texture = calloc(1, sizeof(*texture));
    assert(texture);
    wlr_texture_init(&texture->base, renderer, &texture_impl,
        buffer->width, buffer->height);
    if (is_dma) {
        texture->buffer = wlr_buffer_lock(buffer);
        wlr_addon_init(&texture->addon, &buffer->addons, renderer, &addon_impl);
    } else {
        void *data;
        uint32_t format;
        size_t stride;
        assert(wlr_buffer_begin_data_ptr_access(buffer,
            WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride));
        assert(format == DRM_FORMAT_XRGB8888 && stride == 8);
        texture->copied_pixel = *(uint32_t *)data;
        wlr_buffer_end_data_ptr_access(buffer);
    }
    return &texture->base;
}

static const struct wlr_renderer_impl renderer_impl = {
    .texture_from_buffer = texture_import,
};

/* Exported by this executable for the preload, without lying about texture
 * layout: only renderer classification is faked; texture_is_gles2 remains
 * the real wlroots function and returns false for the fake texture. */
bool wlr_renderer_is_gles2(struct wlr_renderer *renderer) {
    return renderer->impl == &renderer_impl;
}

static void check_release_order(struct counts *counts) {
    assert(counts->release == 1 && counts->destroy == 1);
    assert(textures.gpu_destroy_order < counts->release_order);
    assert(counts->release_order < counts->destroy_order);
}

static void success(struct wlr_renderer *renderer) {
    struct counts counts = {0};
    struct source *source = new_source(&counts, true, 0);
    int original_fd = source->dma.fd[0];
    struct wlr_texture *texture = wlr_texture_from_buffer(renderer, &source->base);
    assert(texture && source->base.n_locks == 2);
    struct fake_texture *fake = (void *)texture;
    assert(fake->buffer != &source->base && fake->buffer->dropped);
    struct wlr_dmabuf_attributes wrapper;
    assert(wlr_buffer_get_dmabuf(fake->buffer, &wrapper));
    assert(wrapper.fd[0] != original_fd && fcntl(wrapper.fd[0], F_GETFD) != -1);
    wlr_buffer_unlock(&source->base);
    wlr_buffer_drop(&source->base);
    assert(counts.release == 0 && counts.destroy == 0);
    /* Destruction must release retained sources even after mode is disabled. */
    unsetenv("SC7_RENDER_EXPERIMENT");
    wlr_texture_destroy(texture);
    assert(textures.gpu_destroy == 1);
    check_release_order(&counts);
}

static void failure(struct wlr_renderer *renderer) {
    struct counts counts = {0};
    struct source *source = new_source(&counts, true, 0);
    fail_import = true;
    assert(wlr_texture_from_buffer(renderer, &source->base) == NULL);
    assert(source->base.n_locks == 1 && counts.release == 0);
    wlr_buffer_unlock(&source->base);
    wlr_buffer_drop(&source->base);
    assert(counts.release == 1 && counts.destroy == 1 && textures.gpu_destroy == 0);
}

static void repeated(struct wlr_renderer *renderer) {
    struct counts counts = {0};
    struct source *source = new_source(&counts, true, 0);
    struct wlr_texture *first = wlr_texture_from_buffer(renderer, &source->base);
    struct wlr_texture *second = wlr_texture_from_buffer(renderer, &source->base);
    assert(first && second && first != second && textures.imports == 2);
    assert(source->base.n_locks == 3);
    wlr_buffer_unlock(&source->base);
    wlr_buffer_drop(&source->base);
    wlr_texture_destroy(first);
    assert(counts.release == 0 && counts.destroy == 0);
    wlr_texture_destroy(second);
    assert(textures.gpu_destroy == 2);
    check_release_order(&counts);
}

static void saved_view(struct wlr_renderer *renderer) {
    struct counts counts = {0};
    struct source *source = new_source(&counts, true, 0);
    struct wlr_client_buffer *client = wlr_client_buffer_create(&source->base, renderer);
    assert(client && source->base.n_locks == 2);
    wlr_buffer_unlock(&source->base);
    /* Pinned Sway saves surface->buffer during a resize transaction. Its saved
     * view owns another client-buffer lock and reuses that client's texture. */
    struct wlr_buffer *saved = wlr_buffer_lock(&client->base);
    wlr_buffer_unlock(&client->base); /* current view replaced */
    wlr_buffer_drop(&source->base);
    assert(client->base.n_locks == 1 && client->source == &source->base);
    assert(counts.release == 0 && counts.destroy == 0);
    wlr_buffer_unlock(saved);
    assert(textures.gpu_destroy == 1);
    check_release_order(&counts);
}

static void wrapper_import(struct wlr_renderer *renderer) {
    struct counts counts = {0};
    struct source *source = new_source(&counts, true, 0);
    struct wlr_client_buffer *client = wlr_client_buffer_create(&source->base, renderer);
    assert(client && source->base.n_locks == 2);
    wlr_buffer_unlock(&source->base);
    struct wlr_texture *saved = wlr_texture_from_buffer(renderer, &client->base);
    assert(saved && client->base.n_locks == 2);
    wlr_buffer_unlock(&client->base); /* current view destroyed, saved view stays */
    wlr_buffer_drop(&source->base);
    assert(counts.release == 0 && counts.destroy == 0 && client->base.n_locks == 1);
    wlr_texture_destroy(saved); /* reentrant client -> original texture teardown */
    assert(textures.gpu_destroy == 2);
    check_release_order(&counts);
}

static void cycles(struct wlr_renderer *renderer) {
    for (int cycle = 0; cycle < 128; ++cycle) {
        struct counts counts = {0};
        struct source *source = new_source(&counts, true, 0);
        struct wlr_texture *texture = wlr_texture_from_buffer(renderer, &source->base);
        assert(texture && source->base.n_locks == 2);
        wlr_buffer_unlock(&source->base);
        wlr_buffer_drop(&source->base);
        assert(counts.release == 0 && counts.destroy == 0);
        wlr_texture_destroy(texture);
        check_release_order(&counts);
    }
    assert(textures.imports == 128 && textures.gpu_destroy == 128);
}

static void shm_upload(struct wlr_renderer *renderer) {
    struct counts counts = {0}, next_counts = {0};
    struct source *source = new_source(&counts, false, 0x12abcdef);
    struct source *next = new_source(&next_counts, false, 0x456789ab);
    struct wlr_client_buffer *client = wlr_client_buffer_create(&source->base, renderer);
    assert(client && source->base.n_locks == 1);
    pixman_region32_t damage;
    pixman_region32_init_rect(&damage, 0, 0, 1, 1);
    assert(!wlr_client_buffer_apply_damage(client, &next->base, &damage));
    assert(textures.updates == 0);
    struct wlr_client_buffer *replacement = wlr_client_buffer_create(&next->base, renderer);
    assert(replacement && textures.imports == 2);
    assert(((struct fake_texture *)client->texture)->copied_pixel == 0x12abcdef);
    assert(((struct fake_texture *)replacement->texture)->copied_pixel == 0x456789ab);
    wlr_buffer_unlock(&source->base);
    wlr_buffer_drop(&source->base);
    wlr_buffer_unlock(&next->base);
    wlr_buffer_drop(&next->base);
    assert(counts.release == 1 && counts.destroy == 1);
    assert(next_counts.release == 1 && next_counts.destroy == 1);
    assert(client->source == NULL && replacement->source == NULL);
    wlr_buffer_unlock(&client->base);
    wlr_buffer_unlock(&replacement->base);
    assert(textures.gpu_destroy == 2);
    pixman_region32_fini(&damage);
}

static void disabled(struct wlr_renderer *renderer) {
    struct counts counts = {0};
    struct source *source = new_source(&counts, true, 0);
    struct wlr_texture *first = wlr_texture_from_buffer(renderer, &source->base);
    struct wlr_texture *second = wlr_texture_from_buffer(renderer, &source->base);
    assert(first && first == second && textures.imports == 1);
    struct wlr_client_buffer client = {.texture = first, .base = {.n_locks = 1}};
    pixman_region32_t damage;
    pixman_region32_init_rect(&damage, 0, 0, 2, 2);
    assert(wlr_client_buffer_apply_damage(&client, &source->base, &damage));
    assert(textures.updates == 1);
    wlr_texture_destroy(first);
    wlr_texture_destroy(second);
    wlr_buffer_unlock(&source->base);
    wlr_buffer_drop(&source->base);
    assert(counts.release == 1 && counts.destroy == 1 && textures.gpu_destroy == 1);
    pixman_region32_fini(&damage);
}

static void observe_upload(struct wlr_renderer *renderer) {
    struct counts counts = {0}, next_counts = {0};
    struct source *source = new_source(&counts, false, 0x12abcdef);
    struct source *next = new_source(&next_counts, false, 0x456789ab);
    struct wlr_client_buffer *client = wlr_client_buffer_create(&source->base, renderer);
    assert(client);
    pixman_region32_t damage;
    pixman_region32_init_rect(&damage, 0, 0, 1, 1);
    assert(wlr_client_buffer_apply_damage(client, &next->base, &damage));
    assert(client->source == &source->base && textures.imports == 1 && textures.updates == 1);
    printf("cached_source=%p incoming=%p texture=%p\n", (void *)&source->base,
        (void *)&next->base, (void *)client->texture);
    wlr_buffer_unlock(&source->base);
    wlr_buffer_drop(&source->base);
    wlr_buffer_unlock(&next->base);
    wlr_buffer_drop(&next->base);
    wlr_buffer_unlock(&client->base);
    assert(counts.release == 1 && counts.destroy == 1);
    assert(next_counts.release == 1 && next_counts.destroy == 1);
    assert(textures.gpu_destroy == 1);
    pixman_region32_fini(&damage);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    struct wlr_renderer renderer = {.impl = &renderer_impl};
    if (!strcmp(argv[1], "success")) { success(&renderer); }
    else if (!strcmp(argv[1], "failure")) { failure(&renderer); }
    else if (!strcmp(argv[1], "repeated")) { repeated(&renderer); }
    else if (!strcmp(argv[1], "saved-view")) { saved_view(&renderer); }
    else if (!strcmp(argv[1], "wrapper-import")) { wrapper_import(&renderer); }
    else if (!strcmp(argv[1], "cycles")) { cycles(&renderer); }
    else if (!strcmp(argv[1], "shm")) { shm_upload(&renderer); }
    else if (!strcmp(argv[1], "disabled")) { disabled(&renderer); }
    else if (!strcmp(argv[1], "observe-upload")) { observe_upload(&renderer); }
    else { abort(); }
    puts("texture lifetime assertions passed");
    return 0;
}
