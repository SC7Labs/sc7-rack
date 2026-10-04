#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <drm_fourcc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include "render/allocator/shm.h"
#include "render_raw_snapshot.h"

struct source {
    struct wlr_buffer base;
    unsigned char pixels[48];
    uint32_t format;
    size_t stride;
    bool reject, null_data;
    int begins, ends, releases, destroys;
    struct wl_listener release;
};

static struct source *from_buffer(struct wlr_buffer *buffer) {
    return wl_container_of(buffer, (struct source *)NULL, base);
}

static void destroy(struct wlr_buffer *buffer) {
    from_buffer(buffer)->destroys++;
}

static bool begin(struct wlr_buffer *buffer, uint32_t flags,
        void **data, uint32_t *format, size_t *stride) {
    struct source *source = from_buffer(buffer);
    assert(flags == WLR_BUFFER_DATA_PTR_ACCESS_READ);
    assert(buffer->n_locks == 2);
    source->begins++;
    if (source->reject) {
        return false;
    }
    *data = source->null_data ? NULL : source->pixels;
    *format = source->format;
    *stride = source->stride;
    return true;
}

static void end(struct wlr_buffer *buffer) {
    struct source *source = from_buffer(buffer);
    assert(buffer->accessing_data_ptr);
    assert(buffer->n_locks == 2);
    source->ends++;
}

static void release(struct wl_listener *listener, void *unused) {
    (void)unused;
    struct source *source = wl_container_of(listener, source, release);
    source->releases++;
}

static const struct wlr_buffer_impl readable_impl = {
    .destroy = destroy,
    .begin_data_ptr_access = begin,
    .end_data_ptr_access = end,
};

static const struct wlr_buffer_impl unreadable_impl = {
    .destroy = destroy,
};

static void init_source(struct source *source,
        const struct wlr_buffer_impl *impl) {
    *source = (struct source){ .format = DRM_FORMAT_ARGB8888, .stride = 8 };
    wlr_buffer_init(&source->base, impl, 2, 2);
    source->release.notify = release;
    wl_signal_add(&source->base.events.release, &source->release);
    wlr_buffer_lock(&source->base);
}

static void finish_source(struct source *source) {
    assert(source->base.n_locks == 1);
    assert(!source->base.accessing_data_ptr);
    assert(source->releases == 0);
    wlr_buffer_drop(&source->base);
    assert(source->destroys == 0);
    wlr_buffer_unlock(&source->base);
    assert(source->destroys == 1);
    assert(source->releases == 1);
    wl_list_remove(&source->release.link);
}

static void check_status(enum sc7_raw_snapshot_status status) {
    assert(sc7_render_raw_snapshot_last_status() == status);
    assert(sc7_render_raw_snapshot_reason()[0]);
    printf("status=%s\n", sc7_render_raw_snapshot_reason());
}

static uint32_t format_named(const char *name) {
    if (strcmp(name, "argb") == 0) return DRM_FORMAT_ARGB8888;
    if (strcmp(name, "xrgb") == 0) return DRM_FORMAT_XRGB8888;
    if (strcmp(name, "abgr") == 0) return DRM_FORMAT_ABGR8888;
    if (strcmp(name, "xbgr") == 0) return DRM_FORMAT_XBGR8888;
    abort();
}

static void fill(void *data, size_t stride, uint32_t format) {
    const uint32_t rgb[] = {0x102030, 0x405060, 0x708090, 0xa0b0c0};
    const unsigned char alpha[] = {0, 128, 192, 255};
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            uint32_t value = rgb[y * 2 + x];
            if (format == DRM_FORMAT_ABGR8888 || format == DRM_FORMAT_XBGR8888) {
                value = ((value & 0xff) << 16) | (value & 0xff00) | (value >> 16);
            }
            /* Includes transparent and nonopaque pixels. Raw RGB is retained
             * exactly; its meaning depends on the corresponding alpha. */
            value |= (uint32_t)alpha[y * 2 + x] << 24;
            memcpy((unsigned char *)data + y * stride + x * 4, &value, 4);
        }
    }
}

static void actual_shm(const char *name, const char *path) {
    struct wlr_allocator *allocator = wlr_shm_allocator_create();
    assert(allocator);
    uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
    struct wlr_drm_format format = {
        .format = format_named(name), .len = 1, .modifiers = &modifier,
    };
    struct wlr_buffer *buffer = wlr_allocator_create_buffer(allocator, 2, 2, &format);
    assert(buffer);
    struct wlr_shm_attributes attributes;
    assert(wlr_buffer_get_shm(buffer, &attributes));
    assert(attributes.fd >= 0 && attributes.format == format.format);
    wlr_buffer_lock(buffer);
    void *data;
    uint32_t observed_format;
    size_t stride;
    assert(wlr_buffer_begin_data_ptr_access(buffer,
        WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &observed_format, &stride));
    assert(observed_format == format.format);
    fill(data, stride, format.format);
    wlr_buffer_end_data_ptr_access(buffer);
    assert(sc7_render_raw_snapshot(buffer, path));
    check_status(SC7_RAW_SNAPSHOT_OK);
    assert(buffer->n_locks == 1 && !buffer->accessing_data_ptr);
    wlr_buffer_unlock(buffer);
    wlr_buffer_drop(buffer);
    wlr_allocator_destroy(allocator);
}

int main(int argc, char **argv) {
    assert(argc == 3);
    const char *test = argv[1], *path = argv[2];
    if (strncmp(test, "shm-", 4) == 0) {
        actual_shm(test + 4, path);
        puts("raw snapshot assertions passed");
        return 0;
    }
    struct source source;
    init_source(&source, strcmp(test, "unavailable") == 0 ?
        &unreadable_impl : &readable_impl);
    if (strcmp(test, "pitch") == 0 || strcmp(test, "cycles") == 0) {
        source.stride = 13;
        memset(source.pixels, 0xee, sizeof(source.pixels));
        fill(source.pixels, source.stride, source.format);
        int cycles = strcmp(test, "cycles") == 0 ? 128 : 1;
        for (int i = 0; i < cycles; i++) {
            assert(sc7_render_raw_snapshot(&source.base, path));
            check_status(SC7_RAW_SNAPSHOT_OK);
        }
        assert(source.begins == cycles && source.ends == cycles);
    } else {
        enum sc7_raw_snapshot_status expected = SC7_RAW_SNAPSHOT_INVALID_ARGUMENT;
        if (strcmp(test, "unavailable") == 0) {
            expected = SC7_RAW_SNAPSHOT_NO_DATA_PTR_IMPL;
        } else if (strcmp(test, "reject") == 0) {
            source.reject = true;
            expected = SC7_RAW_SNAPSHOT_ACCESS_FAILED;
        } else if (strcmp(test, "format") == 0) {
            source.format = DRM_FORMAT_RGB565;
            expected = SC7_RAW_SNAPSHOT_UNSUPPORTED_FORMAT;
        } else if (strcmp(test, "big-endian-format") == 0) {
            source.format |= DRM_FORMAT_BIG_ENDIAN;
            expected = SC7_RAW_SNAPSHOT_UNSUPPORTED_FORMAT;
        } else if (strcmp(test, "stride") == 0) {
            source.stride = 7;
            expected = SC7_RAW_SNAPSHOT_INVALID_LAYOUT;
        } else if (strcmp(test, "overflow") == 0) {
            source.stride = SIZE_MAX;
            expected = SC7_RAW_SNAPSHOT_INVALID_LAYOUT;
        } else if (strcmp(test, "null-data") == 0) {
            source.null_data = true;
            expected = SC7_RAW_SNAPSHOT_INVALID_LAYOUT;
        } else if (strcmp(test, "busy") == 0) {
            source.base.accessing_data_ptr = true;
            expected = SC7_RAW_SNAPSHOT_ACCESS_BUSY;
        } else if (strcmp(test, "dimensions") == 0) {
            source.base.width = 4097;
            expected = SC7_RAW_SNAPSHOT_DIMENSION_LIMIT;
        } else if (strcmp(test, "zero-dimension") == 0) {
            source.base.height = 0;
            expected = SC7_RAW_SNAPSHOT_DIMENSION_LIMIT;
        } else if (strcmp(test, "file-failure") == 0) {
            expected = SC7_RAW_SNAPSHOT_FILE_FAILED;
        } else if (strcmp(test, "unlocked") == 0) {
            wlr_buffer_unlock(&source.base);
            assert(!sc7_render_raw_snapshot(&source.base, path));
            check_status(SC7_RAW_SNAPSHOT_SOURCE_NOT_LOCKED);
            assert(source.begins == 0 && source.ends == 0);
            assert(source.releases == 1);
            wlr_buffer_drop(&source.base);
            assert(source.destroys == 1);
            wl_list_remove(&source.release.link);
            puts("raw snapshot assertions passed");
            return 0;
        } else if (strcmp(test, "null-buffer") == 0) {
            assert(!sc7_render_raw_snapshot(NULL, path));
            check_status(expected);
            finish_source(&source);
            puts("raw snapshot assertions passed");
            return 0;
        } else if (strcmp(test, "empty-path") == 0) {
            path = "";
        } else {
            abort();
        }
        assert(!sc7_render_raw_snapshot(&source.base, path));
        check_status(expected);
        source.base.accessing_data_ptr = false;
        int accesses = expected == SC7_RAW_SNAPSHOT_ACCESS_FAILED ||
            expected == SC7_RAW_SNAPSHOT_UNSUPPORTED_FORMAT ||
            expected == SC7_RAW_SNAPSHOT_INVALID_LAYOUT ||
            expected == SC7_RAW_SNAPSHOT_FILE_FAILED;
        assert(source.begins == accesses);
        assert(source.ends == accesses - (expected == SC7_RAW_SNAPSHOT_ACCESS_FAILED));
    }
    finish_source(&source);
    puts("raw snapshot assertions passed");
    return 0;
}
