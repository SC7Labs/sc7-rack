#define _POSIX_C_SOURCE 200809L
#include <drm_fourcc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include "render_raw_snapshot.h"

#define MAX_DIMENSION 4096
#define MAX_PATH_LENGTH 4096

static _Thread_local enum sc7_raw_snapshot_status last_status =
    SC7_RAW_SNAPSHOT_INVALID_ARGUMENT;

enum sc7_raw_snapshot_status sc7_render_raw_snapshot_last_status(void) {
    return last_status;
}

const char *sc7_render_raw_snapshot_reason(void) {
    static const char *const reasons[] = {
        [SC7_RAW_SNAPSHOT_OK] = "ok",
        [SC7_RAW_SNAPSHOT_INVALID_ARGUMENT] = "invalid-argument",
        [SC7_RAW_SNAPSHOT_SOURCE_NOT_LOCKED] = "source-not-consumer-locked",
        [SC7_RAW_SNAPSHOT_DIMENSION_LIMIT] = "dimension-limit",
        [SC7_RAW_SNAPSHOT_ACCESS_BUSY] = "data-pointer-access-already-active",
        [SC7_RAW_SNAPSHOT_NO_DATA_PTR_IMPL] =
            "data-pointer-implementation-unavailable",
        [SC7_RAW_SNAPSHOT_ACCESS_FAILED] = "data-pointer-read-access-failed",
        [SC7_RAW_SNAPSHOT_UNSUPPORTED_FORMAT] = "unsupported-raw-pixel-format",
        [SC7_RAW_SNAPSHOT_INVALID_LAYOUT] = "invalid-data-pointer-or-row-layout",
        [SC7_RAW_SNAPSHOT_OUT_OF_MEMORY] = "allocation-failed",
        [SC7_RAW_SNAPSHOT_FILE_FAILED] = "snapshot-file-write-failed",
    };
    return reasons[last_status];
}

static bool write_temporary_pnm(char *temporary, const char *magic,
        int width, int height, const unsigned char *bytes, size_t size,
        bool *created) {
    int fd = mkstemp(temporary);
    *created = fd >= 0;
    FILE *file = fd < 0 ? NULL : fdopen(fd, "wb");
    bool written = false;
    if (file) {
        written = fprintf(file, "%s\n%d %d\n255\n", magic, width, height) > 0 &&
            fwrite(bytes, 1, size, file) == size;
        if (fclose(file) != 0) written = false;
    } else if (fd >= 0) {
        close(fd);
    }
    return written;
}

bool sc7_render_raw_snapshot(struct wlr_buffer *buffer, const char *path) {
    last_status = SC7_RAW_SNAPSHOT_INVALID_ARGUMENT;
    if (!buffer || !buffer->impl || !path || !path[0] ||
            strnlen(path, MAX_PATH_LENGTH + 1) > MAX_PATH_LENGTH) {
        return false;
    }
    if (buffer->n_locks == 0) {
        last_status = SC7_RAW_SNAPSHOT_SOURCE_NOT_LOCKED;
        return false;
    }
    if (buffer->width < 1 || buffer->height < 1 ||
            buffer->width > MAX_DIMENSION || buffer->height > MAX_DIMENSION) {
        last_status = SC7_RAW_SNAPSHOT_DIMENSION_LIMIT;
        return false;
    }
    if (buffer->accessing_data_ptr) {
        last_status = SC7_RAW_SNAPSHOT_ACCESS_BUSY;
        return false;
    }
    if (!buffer->impl->begin_data_ptr_access ||
            !buffer->impl->end_data_ptr_access) {
        last_status = SC7_RAW_SNAPSHOT_NO_DATA_PTR_IMPL;
        return false;
    }

    int width = buffer->width, height = buffer->height;
    size_t alpha_size = (size_t)width * (size_t)height;
    size_t rgb_size = alpha_size * 3;
    unsigned char *rgb = malloc(rgb_size);
    unsigned char *alpha = malloc(alpha_size);
    if (!rgb || !alpha) {
        last_status = SC7_RAW_SNAPSHOT_OUT_OF_MEMORY;
        free(rgb);
        free(alpha);
        return false;
    }

    void *data = NULL;
    uint32_t format = DRM_FORMAT_INVALID;
    size_t stride = 0;
    wlr_buffer_lock(buffer);
    if (!wlr_buffer_begin_data_ptr_access(buffer,
            WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
        last_status = SC7_RAW_SNAPSHOT_ACCESS_FAILED;
        wlr_buffer_unlock(buffer);
        free(rgb);
        free(alpha);
        return false;
    }

    bool swap_red_blue = format == DRM_FORMAT_ABGR8888 ||
        format == DRM_FORMAT_XBGR8888;
    bool supported = swap_red_blue || format == DRM_FORMAT_ARGB8888 ||
        format == DRM_FORMAT_XRGB8888;
    bool has_alpha = format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_ABGR8888;
    if (!supported) {
        last_status = SC7_RAW_SNAPSHOT_UNSUPPORTED_FORMAT;
    } else if (!data || stride < (size_t)width * 4 ||
            stride > SIZE_MAX / (size_t)height) {
        last_status = SC7_RAW_SNAPSHOT_INVALID_LAYOUT;
    } else {
        for (int y = 0; y < height; y++) {
            const unsigned char *row = (const unsigned char *)data + y * stride;
            unsigned char *out = rgb + (size_t)y * (size_t)width * 3;
            for (int x = 0; x < width; x++) {
                /* These DRM formats have little-endian packed channel bytes;
                 * byte access is independent of host endianness/alignment. */
                const unsigned char *pixel = row + (size_t)x * 4;
                out[x * 3] = pixel[swap_red_blue ? 0 : 2];
                out[x * 3 + 1] = pixel[1];
                out[x * 3 + 2] = pixel[swap_red_blue ? 2 : 0];
                alpha[(size_t)y * (size_t)width + (size_t)x] = has_alpha ? pixel[3] : 255;
            }
        }
        last_status = SC7_RAW_SNAPSHOT_OK;
    }
    wlr_buffer_end_data_ptr_access(buffer);
    wlr_buffer_unlock(buffer);
    if (last_status != SC7_RAW_SNAPSHOT_OK) {
        free(rgb);
        free(alpha);
        return false;
    }

    size_t path_length = strlen(path);
    char *alpha_path = malloc(path_length + sizeof(".alpha.pgm"));
    char *rgb_temporary = malloc(path_length + sizeof(".tmp-XXXXXX"));
    char *alpha_temporary = malloc(path_length + sizeof(".alpha.pgm.tmp-XXXXXX"));
    if (!alpha_path || !rgb_temporary || !alpha_temporary) {
        last_status = SC7_RAW_SNAPSHOT_OUT_OF_MEMORY;
        free(alpha_path);
        free(rgb_temporary);
        free(alpha_temporary);
        free(rgb);
        free(alpha);
        return false;
    }
    snprintf(alpha_path, path_length + sizeof(".alpha.pgm"), "%s.alpha.pgm", path);
    snprintf(rgb_temporary, path_length + sizeof(".tmp-XXXXXX"), "%s.tmp-XXXXXX", path);
    snprintf(alpha_temporary, path_length + sizeof(".alpha.pgm.tmp-XXXXXX"),
        "%s.alpha.pgm.tmp-XXXXXX", path);
    bool rgb_created = false, alpha_created = false, rgb_published = false;
    bool rgb_written = write_temporary_pnm(rgb_temporary, "P6", width, height,
        rgb, rgb_size, &rgb_created);
    bool alpha_written = rgb_written && write_temporary_pnm(alpha_temporary,
        "P5", width, height, alpha, alpha_size, &alpha_created);
    last_status = SC7_RAW_SNAPSHOT_FILE_FAILED;
    if (rgb_written && alpha_written && rename(rgb_temporary, path) == 0) {
        rgb_published = true;
        if (rename(alpha_temporary, alpha_path) == 0) {
            last_status = SC7_RAW_SNAPSHOT_OK;
        }
    }
    if (last_status != SC7_RAW_SNAPSHOT_OK) {
        if (rgb_published) unlink(path);
        if (rgb_created) unlink(rgb_temporary);
        if (alpha_created) unlink(alpha_temporary);
    }
    free(alpha_path);
    free(rgb_temporary);
    free(alpha_temporary);
    free(rgb);
    free(alpha);
    return last_status == SC7_RAW_SNAPSHOT_OK;
}
