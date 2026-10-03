/* Optional, test-only wlroots frame telemetry. Loaded only into private Sway. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <GLES2/gl2.h>
#include <inttypes.h>
#include <limits.h>
#include <pixman.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wlr/render/gles2.h>
#include <wlr/render/pass.h>
#include <wlr/render/pixman.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include "render/gles2.h"
#include "render/egl.h"
#include "render/pixman.h"
#include "render_output_experiment.h"

static FILE *trace_file(void);
static uint64_t now_us(void);

/* The diagnostic is loaded into Sway only. Its children must not preload it. */
__attribute__((constructor)) static void isolate_preload(void) {
    if (getenv("SC7_RENDER_TRACE")) {
        unsetenv("LD_PRELOAD");
    }
}

static struct wlr_render_pass *active_pass;
static struct wlr_renderer *active_renderer;
static struct wlr_buffer *active_buffer;
static bool active_scene_frame;
static uint64_t render_frame;
static unsigned int capture_attempts;

static void capture_pre_submit(uint64_t frame) {
    const char *trigger = getenv("SC7_RENDER_CAPTURE_TRIGGER");
    const char *directory = getenv("SC7_RENDER_CAPTURE_DIR");
    if (!trigger || !*trigger || !directory || !*directory ||
            access(trigger, F_OK) != 0 || !active_renderer || !active_buffer ||
            !active_scene_frame) {
        return;
    }
    /* One trigger captures one completed render pass before output commit. */
    unlink(trigger);
    uint32_t width = active_buffer->width, height = active_buffer->height;
    FILE *log = trace_file();
    if (capture_attempts++ >= 8) {
        if (log) {
            fprintf(log, "%" PRIu64 " pre-submit frame=%" PRIu64
                " buffer=%p ok=0 reason=capture-limit\n",
                now_us(), frame, (void *)active_buffer);
        }
        return;
    }
    if (width == 0 || height == 0 || width > 4096 || height > 4096 ||
            width > UINT32_MAX / 4) {
        if (log) {
            fprintf(log, "%" PRIu64 " pre-submit frame=%" PRIu64
                " buffer=%p size=%ux%u ok=0 reason=dimensions\n",
                now_us(), frame, (void *)active_buffer, width, height);
        }
        return;
    }
    uint32_t stride = width * 4;
    unsigned char *pixels = malloc((size_t)stride * height);
    if (!pixels) {
        if (log) {
            fprintf(log, "%" PRIu64 " pre-submit frame=%" PRIu64
                " buffer=%p size=%ux%u ok=0 reason=allocation\n",
                now_us(), frame, (void *)active_buffer, width, height);
        }
        return;
    }
    bool bottom_up = wlr_renderer_is_gles2(active_renderer);
    bool ok = false;
    unsigned int target = 0;
    if (bottom_up) {
        struct wlr_gles2_render_pass *pass = (void *)active_pass;
        void (*read_pixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) =
            dlsym(RTLD_DEFAULT, "glReadPixels");
        void (*get_integer)(GLenum, GLint *) = dlsym(RTLD_DEFAULT, "glGetIntegerv");
        void (*pixel_store)(GLenum, GLint) = dlsym(RTLD_DEFAULT, "glPixelStorei");
        GLenum (*get_error)(void) = dlsym(RTLD_DEFAULT, "glGetError");
        if (read_pixels && get_integer && pixel_store && get_error) {
            GLint fbo = 0;
            get_integer(GL_FRAMEBUFFER_BINDING, &fbo);
            target = (unsigned int)fbo;
            GLenum prior_error = GL_NO_ERROR;
            bool errors_drained = false;
            for (int i = 0; i < 16; ++i) {
                GLenum error = get_error();
                if (error == GL_NO_ERROR) {
                    errors_drained = true;
                    break;
                }
                prior_error = error;
                if (log) {
                    fprintf(log, "%" PRIu64 " pre-submit-gl-error frame=%" PRIu64
                        " error=0x%x\n", now_us(), frame, error);
                }
            }
            if (target != 0 && target == pass->buffer->fbo && errors_drained) {
                GLint pack_alignment = 4;
                get_integer(GL_PACK_ALIGNMENT, &pack_alignment);
                pixel_store(GL_PACK_ALIGNMENT, 1);
                read_pixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                ok = get_error() == GL_NO_ERROR;
                pixel_store(GL_PACK_ALIGNMENT, pack_alignment);
            } else if (log) {
                fprintf(log, "%" PRIu64 " pre-submit-invalid-target frame=%" PRIu64
                    " bound=%u expected=%u drained=%d prior_error=0x%x\n",
                    now_us(), frame, target, pass->buffer->fbo,
                    errors_drained, prior_error);
            }
        }
    } else if (wlr_renderer_is_pixman(active_renderer)) {
        struct wlr_pixman_render_pass *pass = (void *)active_pass;
        pixman_image_t *image = pass->buffer->image;
        pixman_format_code_t format = pixman_image_get_format(image);
        if ((format == PIXMAN_x8r8g8b8 || format == PIXMAN_a8r8g8b8) &&
                pixman_image_get_width(image) == (int)width &&
                pixman_image_get_height(image) == (int)height &&
                pixman_image_get_stride(image) >= (int)stride) {
            const unsigned char *source = (void *)pixman_image_get_data(image);
            int source_stride = pixman_image_get_stride(image);
            for (uint32_t y = 0; y < height; ++y) {
                for (uint32_t x = 0; x < width; ++x) {
                    const unsigned char *src = source + (size_t)y * source_stride + x * 4;
                    unsigned char *dst = pixels + (size_t)y * stride + x * 4;
                    dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3];
                }
            }
            ok = true;
        }
    }
    char path[PATH_MAX];
    int name_len = snprintf(path, sizeof(path), "%s/pre-submit-%012" PRIu64 ".ppm",
        directory, frame);
    if (ok && name_len > 0 && (size_t)name_len < sizeof(path)) {
        char temporary[PATH_MAX];
        int temporary_len = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
        FILE *image = temporary_len > 0 && (size_t)temporary_len < sizeof(temporary)
            ? fopen(temporary, "wb") : NULL;
        if (image) {
            ok = fprintf(image, "P6\n%u %u\n255\n", width, height) > 0;
            unsigned char *rgb_row = malloc((size_t)width * 3);
            ok = ok && rgb_row != NULL;
            for (uint32_t y = 0; ok && y < height; ++y) {
                uint32_t source_y = bottom_up ? height - y - 1 : y;
                const unsigned char *row = pixels + (size_t)source_y * stride;
                for (uint32_t x = 0; x < width; ++x) {
                    memcpy(rgb_row + (size_t)x * 3, row + (size_t)x * 4, 3);
                }
                ok = fwrite(rgb_row, 3, width, image) == width;
            }
            free(rgb_row);
            if (fclose(image) != 0) {
                ok = false;
            }
            if (ok && rename(temporary, path) != 0) {
                ok = false;
            }
            if (!ok) {
                unlink(temporary);
            }
        } else {
            ok = false;
        }
    } else {
        ok = false;
    }
    if (log) {
        fprintf(log, "%" PRIu64 " pre-submit frame=%" PRIu64
            " buffer=%p size=%ux%u renderer=%s target=%u path=%s ok=%d\n",
            now_us(), frame, (void *)active_buffer, width, height,
            bottom_up ? "gles2" : "pixman", target,
            ok ? path : "<none>", ok);
    }
    free(pixels);
}

static FILE *trace_file(void) {
    static FILE *file;
    static bool stopped;
    if (!file && !stopped) {
        const char *path = getenv("SC7_RENDER_TRACE");
        if (path && *path) {
            file = fopen(path, "a");
            if (file) {
                setvbuf(file, NULL, _IOLBF, 0);
            } else {
                stopped = true;
            }
        }
    }
    if (file && ftello(file) >= (off_t)(16 * 1024 * 1024)) {
        const char *path = getenv("SC7_RENDER_TRACE");
        char previous[PATH_MAX];
        int length = snprintf(previous, sizeof(previous), "%s.previous", path);
        fclose(file);
        file = NULL;
        if (length > 0 && (size_t)length < sizeof(previous) &&
                rename(path, previous) == 0) {
            file = fopen(path, "w");
            if (file) {
                setvbuf(file, NULL, _IOLBF, 0);
                fputs("trace-rotated chunk_limit=16777216\n", file);
            }
        }
        stopped = file == NULL;
    }
    return file;
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

struct watched_buffer {
    struct wl_list link;
    struct wlr_buffer *buffer;
    uint64_t generation;
    struct wl_listener release;
    struct wl_listener destroy;
};

static struct wl_list watched_buffers = {&watched_buffers, &watched_buffers};
static uint64_t buffer_generation;

static void buffer_released(struct wl_listener *listener, void *data) {
    (void)data;
    struct watched_buffer *watch = wl_container_of(listener, watch, release);
    FILE *file = trace_file();
    if (file) {
        fprintf(file, "%" PRIu64 " buffer-release buffer=%p generation=%" PRIu64
            " locks=%zu\n", now_us(), (void *)watch->buffer,
            watch->generation, watch->buffer->n_locks);
    }
}

static void buffer_destroyed(struct wl_listener *listener, void *data) {
    (void)data;
    struct watched_buffer *watch = wl_container_of(listener, watch, destroy);
    FILE *file = trace_file();
    if (file) {
        fprintf(file, "%" PRIu64 " buffer-destroy buffer=%p generation=%" PRIu64 "\n",
            now_us(), (void *)watch->buffer, watch->generation);
    }
    wl_list_remove(&watch->release.link);
    wl_list_remove(&watch->destroy.link);
    wl_list_remove(&watch->link);
    free(watch);
}

static uint64_t watch_buffer(struct wlr_buffer *buffer) {
    if (!buffer) {
        return 0;
    }
    struct watched_buffer *watch;
    wl_list_for_each(watch, &watched_buffers, link) {
        if (watch->buffer == buffer) {
            return watch->generation;
        }
    }
    watch = calloc(1, sizeof(*watch));
    if (!watch) {
        return 0;
    }
    watch->buffer = buffer;
    watch->generation = ++buffer_generation;
    watch->release.notify = buffer_released;
    watch->destroy.notify = buffer_destroyed;
    wl_signal_add(&buffer->events.release, &watch->release);
    wl_signal_add(&buffer->events.destroy, &watch->destroy);
    wl_list_insert(&watched_buffers, &watch->link);
    FILE *file = trace_file();
    if (file) {
        fprintf(file, "%" PRIu64 " buffer-observe buffer=%p generation=%" PRIu64
            " size=%dx%d locks=%zu\n", now_us(), (void *)buffer,
            watch->generation, buffer->width, buffer->height, buffer->n_locks);
    }
    return watch->generation;
}

FILE *sc7_render_trace_file(void) { return trace_file(); }
uint64_t sc7_render_watch_buffer(struct wlr_buffer *buffer) { return watch_buffer(buffer); }
uint64_t sc7_render_trace_time_us(void) { return now_us(); }

struct watched_resource {
    struct wl_listener destroy;
    struct wl_resource *resource;
    uint32_t id;
    pid_t pid;
};

static void resource_destroyed(struct wl_listener *listener, void *data) {
    (void)data;
    struct watched_resource *watch = wl_container_of(listener, watch, destroy);
    FILE *file = trace_file();
    if (file) {
        fprintf(file, "%" PRIu64 " wl-buffer-destroy resource=%p id=%u pid=%ld\n",
            now_us(), (void *)watch->resource, watch->id, (long)watch->pid);
    }
    wl_list_remove(&watch->destroy.link);
    free(watch);
}

struct wlr_buffer *wlr_buffer_try_from_resource(struct wl_resource *resource) {
    static struct wlr_buffer *(*next)(struct wl_resource *);
    if (!next) { next = dlsym(RTLD_NEXT, "wlr_buffer_try_from_resource"); }
    struct wlr_buffer *buffer = next(resource);
    uint64_t generation = watch_buffer(buffer);
    const char *kind = buffer ? "other" : "none";
    uint32_t format = 0;
    uint64_t modifier = 0;
    struct wlr_dmabuf_attributes dma;
    struct wlr_shm_attributes shm;
    if (buffer && wlr_buffer_get_dmabuf(buffer, &dma)) {
        kind = "dmabuf"; format = dma.format; modifier = dma.modifier;
    } else if (buffer && wlr_buffer_get_shm(buffer, &shm)) {
        kind = "shm"; format = shm.format;
    }
    pid_t pid = 0;
    wl_client_get_credentials(wl_resource_get_client(resource), &pid, NULL, NULL);
    if (!wl_resource_get_destroy_listener(resource, resource_destroyed)) {
        struct watched_resource *watch = calloc(1, sizeof(*watch));
        if (watch) {
            watch->resource = resource;
            watch->id = wl_resource_get_id(resource);
            watch->pid = pid;
            watch->destroy.notify = resource_destroyed;
            wl_resource_add_destroy_listener(resource, &watch->destroy);
        }
    }
    FILE *file = trace_file();
    if (file) {
        fprintf(file, "%" PRIu64 " wl-buffer-attach resource=%p id=%u pid=%ld"
            " buffer=%p generation=%" PRIu64 " kind=%s size=%dx%d"
            " format=0x%08" PRIx32 " modifier=0x%016" PRIx64 "\n",
            now_us(), (void *)resource, wl_resource_get_id(resource), (long)pid,
            (void *)buffer, generation, kind, buffer ? buffer->width : 0,
            buffer ? buffer->height : 0, format, modifier);
    }
    return buffer;
}

struct wlr_texture *wlr_surface_get_texture(struct wlr_surface *surface) {
    static struct wlr_texture *(*next)(struct wlr_surface *);
    if (!next) {
        next = dlsym(RTLD_NEXT, "wlr_surface_get_texture");
    }
    struct wlr_texture *texture = next(surface);
    struct wlr_buffer *source = surface->buffer ? surface->buffer->source : NULL;
    uint64_t generation = watch_buffer(source);
    FILE *file = trace_file();
    if (file) {
        const char *kind = source ? "other" : "none";
        uint32_t format = 0;
        uint64_t modifier = 0;
        struct wlr_dmabuf_attributes dma;
        struct wlr_shm_attributes shm;
        if (source && wlr_buffer_get_dmabuf(source, &dma)) {
            kind = "dmabuf"; format = dma.format; modifier = dma.modifier;
        } else if (source && wlr_buffer_get_shm(source, &shm)) {
            kind = "shm"; format = shm.format;
        }
        pid_t pid = 0;
        wl_client_get_credentials(wl_resource_get_client(surface->resource), &pid, NULL, NULL);
        unsigned int gl_texture = 0, gl_target = 0;
        void *image = NULL;
        if (texture && wlr_texture_is_gles2(texture)) {
            struct wlr_gles2_texture *gles = (void *)texture;
            gl_texture = gles->tex; gl_target = gles->target; image = gles->image;
        }
        fprintf(file, "%" PRIu64 " input surface=%p resource=%u pid=%ld"
            " cached_source=%p cached_generation=%" PRIu64 " cached_kind=%s size=%dx%d"
            " format=0x%08" PRIx32 " modifier=0x%016" PRIx64
            " texture=%p gl_texture=%u gl_target=0x%x egl_image=%p\n",
            now_us(), (void *)surface, wl_resource_get_id(surface->resource), (long)pid,
            (void *)source, generation, kind, source ? source->width : 0,
            source ? source->height : 0, format, modifier,
            (void *)texture, gl_texture, gl_target, image);
    }
    return texture;
}

EGLImageKHR wlr_egl_create_image_from_dmabuf(struct wlr_egl *egl,
        struct wlr_dmabuf_attributes *attributes, bool *external_only) {
    static EGLImageKHR (*next)(struct wlr_egl *, struct wlr_dmabuf_attributes *, bool *);
    if (!next) { next = dlsym(RTLD_NEXT, "wlr_egl_create_image_from_dmabuf"); }
    EGLImageKHR image = next(egl, attributes, external_only);
    FILE *file = trace_file();
    if (file) {
        fprintf(file, "%" PRIu64 " egl-image-create egl=%p image=%p size=%dx%d"
            " format=0x%08" PRIx32 " modifier=0x%016" PRIx64
            " planes=%d external=%d ok=%d\n", now_us(), (void *)egl, (void *)image,
            attributes->width, attributes->height, attributes->format,
            attributes->modifier, attributes->n_planes,
            image != EGL_NO_IMAGE_KHR && external_only ? *external_only : -1,
            image != EGL_NO_IMAGE_KHR);
    }
    return image;
}

bool wlr_egl_destroy_image(struct wlr_egl *egl, EGLImageKHR image) {
    static bool (*next)(struct wlr_egl *, EGLImageKHR);
    if (!next) { next = dlsym(RTLD_NEXT, "wlr_egl_destroy_image"); }
    bool ok = next(egl, image);
    FILE *file = trace_file();
    if (file) {
        fprintf(file, "%" PRIu64 " egl-image-destroy egl=%p image=%p ok=%d\n",
            now_us(), (void *)egl, (void *)image, ok);
    }
    return ok;
}

void wlr_render_pass_add_texture(struct wlr_render_pass *pass,
        const struct wlr_render_texture_options *options) {
    static void (*next)(struct wlr_render_pass *, const struct wlr_render_texture_options *);
    if (!next) { next = dlsym(RTLD_NEXT, "wlr_render_pass_add_texture"); }
    struct wlr_texture *texture = options->texture;
    struct wlr_gles2_texture *gles = texture && wlr_texture_is_gles2(texture)
        ? (void *)texture : NULL;
    uint64_t generation = watch_buffer(gles ? gles->buffer : NULL);
    struct wlr_box destination;
    wlr_render_texture_options_get_dst_box(options, &destination);
    FILE *file = trace_file();
    if (file) {
        fprintf(file, "%" PRIu64 " sample-texture frame=%" PRIu64
            " pass=%p texture=%p source=%p generation=%" PRIu64
            " gl_texture=%u gl_target=0x%x egl_image=%p dst=%d,%d,%d,%d\n",
            now_us(), render_frame + 1, (void *)pass, (void *)texture,
            gles ? (void *)gles->buffer : NULL, generation,
            gles ? gles->tex : 0, gles ? gles->target : 0,
            gles ? (void *)gles->image : NULL, destination.x, destination.y,
            destination.width, destination.height);
    }
    next(pass, options);
}

struct wlr_render_pass *wlr_renderer_begin_buffer_pass(struct wlr_renderer *renderer,
        struct wlr_buffer *buffer, const struct wlr_buffer_pass_options *options) {
    static struct wlr_render_pass *(*next)(struct wlr_renderer *,
        struct wlr_buffer *, const struct wlr_buffer_pass_options *);
    if (!next) {
        next = dlsym(RTLD_NEXT, "wlr_renderer_begin_buffer_pass");
    }
    struct wlr_buffer *target = sc7_render_fresh_target(renderer, buffer);
    if (!target) { return NULL; }
    struct wlr_render_pass *pass = next(renderer, target, options);
    sc7_render_target_drop(buffer, target);
    if (pass) {
        active_pass = pass;
        active_renderer = renderer;
        active_buffer = buffer;
        active_scene_frame = false;
        uint64_t generation = watch_buffer(buffer);
        FILE *file = trace_file();
        if (file && wlr_renderer_is_gles2(renderer)) {
            struct wlr_gles2_render_pass *gles = (void *)pass;
            fprintf(file, "%" PRIu64 " render-begin frame=%" PRIu64
                " renderer=gles2 buffer=%p generation=%" PRIu64
                " size=%dx%d target=%p fbo=%u rbo=%u egl_image=%p\n",
                now_us(), render_frame + 1, (void *)buffer, generation,
                buffer->width, buffer->height, (void *)gles->buffer,
                gles->buffer->fbo, gles->buffer->rbo, (void *)gles->buffer->image);
        } else if (file) {
            fprintf(file, "%" PRIu64 " render-begin frame=%" PRIu64
                " renderer=pixman buffer=%p generation=%" PRIu64 " size=%dx%d\n",
                now_us(), render_frame + 1, (void *)buffer, generation,
                buffer->width, buffer->height);
        }
    }
    return pass;
}

bool wlr_render_pass_submit(struct wlr_render_pass *pass) {
    static bool (*next)(struct wlr_render_pass *);
    if (!next) {
        next = dlsym(RTLD_NEXT, "wlr_render_pass_submit");
    }
    if (pass == active_pass) {
        capture_pre_submit(++render_frame);
    }
    bool ok = next(pass);
    if (pass == active_pass) {
        active_pass = NULL;
        active_renderer = NULL;
        active_buffer = NULL;
        active_scene_frame = false;
    }
    return ok;
}

struct wlr_buffer *wlr_swapchain_acquire(struct wlr_swapchain *chain, int *age) {
    static struct wlr_buffer *(*next)(struct wlr_swapchain *, int *);
    if (!next) {
        next = dlsym(RTLD_NEXT, "wlr_swapchain_acquire");
    }
    sc7_render_retire_released_outputs(chain);
    struct wlr_buffer *buffer = next(chain, age);
    uint64_t generation = watch_buffer(buffer);
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
        int slot = -1;
        for (int i = 0; i < WLR_SWAPCHAIN_CAP; ++i) {
            if (chain->slots[i].buffer == buffer) { slot = i; break; }
        }
        fprintf(file, "%" PRIu64 " acquire chain=%p size=%dx%d buffer=%p age=%d format=0x%08" PRIx32 " kind=%s modifier=0x%016" PRIx64 " generation=%" PRIu64 " slot=%d locks=%zu\n",
            now_us(), (void *)chain, chain->width, chain->height,
            (void *)buffer, age ? *age : -1, chain->format.format,
            kind, modifier, generation, slot, buffer ? buffer->n_locks : 0);
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
    /* Sway queries scene damage after starting its composed output pass.
     * wlroots' empty modeset/test passes do not query this damage ring. */
    if (active_pass) {
        active_scene_frame = true;
    }
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
    static int last_width, last_height;
    static uint64_t resize_generation;
    if (ok && (output->width != last_width || output->height != last_height)) {
        last_width = output->width; last_height = output->height;
        ++resize_generation;
    }
    uint64_t generation = watch_buffer(state->buffer);
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
            " box=%d,%d,%d,%d ok=%d frame=%" PRIu64
            " resize_generation=%" PRIu64 " buffer_generation=%" PRIu64 "\n",
            now_us(), output->name ? output->name : "<unnamed>",
            output->width, output->height, output->commit_seq,
            state->committed, (void *)state->buffer, count,
            extents ? extents->x1 : 0, extents ? extents->y1 : 0,
            extents ? extents->x2 : 0, extents ? extents->y2 : 0, ok,
            render_frame, resize_generation, generation);
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
