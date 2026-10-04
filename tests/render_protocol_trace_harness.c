/* Real Wayland socketpair requests/events and pinned wlroots SHM lifetimes.
 * The compositor has no renderer: these tests cover the release protocol,
 * not GPU synchronization. Everything runs on one event-loop thread. */
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_shm.h>
#include "render_protocol_trace.h"
#include "types/wlr_buffer.h"

static FILE *log_file;
static uint64_t counter;
static unsigned wire_releases, internal_releases, buffer_destroys;
static struct wlr_buffer *held;
static bool hold_mode;
static struct wl_display *server_display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_listener new_surface;

FILE *sc7_render_trace_file(void) { return log_file; }
uint64_t sc7_render_trace_time_us(void) { return ++counter; }
uint64_t sc7_render_trace_frame(void) { return 447; }

struct watched_buffer {
    struct wl_list link;
    struct wlr_buffer *buffer;
    struct wl_listener release, destroy;
    uint64_t generation;
};
static struct wl_list watched = {&watched, &watched};
static uint64_t generations;

static void watch_release(struct wl_listener *listener, void *data) {
    (void)data;
    struct watched_buffer *entry = wl_container_of(listener, entry, release);
    ++internal_releases;
    fprintf(log_file, "%" PRIu64 " buffer-release generation=%" PRIu64 "\n",
        sc7_render_trace_time_us(), entry->generation);
}

static void watch_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct watched_buffer *entry = wl_container_of(listener, entry, destroy);
    ++buffer_destroys;
    wl_list_remove(&entry->release.link);
    wl_list_remove(&entry->destroy.link);
    wl_list_remove(&entry->link);
    free(entry);
}

uint64_t sc7_render_watch_buffer(struct wlr_buffer *buffer) {
    struct watched_buffer *entry;
    wl_list_for_each(entry, &watched, link) {
        if (entry->buffer == buffer) {
            return entry->generation;
        }
    }
    entry = calloc(1, sizeof(*entry));
    assert(entry);
    entry->buffer = buffer;
    entry->generation = ++generations;
    entry->release.notify = watch_release;
    entry->destroy.notify = watch_destroy;
    wl_signal_add(&buffer->events.release, &entry->release);
    wl_signal_add(&buffer->events.destroy, &entry->destroy);
    wl_list_insert(&watched, &entry->link);
    return entry->generation;
}

/* The production combined tracer makes this exact mapping call. */
struct wlr_buffer *wlr_buffer_try_from_resource(struct wl_resource *resource) {
    static struct wlr_buffer *(*next)(struct wl_resource *);
    if (!next) {
        next = dlsym(RTLD_NEXT, "wlr_buffer_try_from_resource");
        assert(next);
    }
    struct wlr_buffer *buffer = next(resource);
    if (buffer) {
        sc7_render_watch_buffer(buffer);
    }
    sc7_render_protocol_resource_buffer(resource, buffer);
    return buffer;
}

struct tracked_surface {
    struct wl_listener commit, destroy;
    struct wlr_surface *surface;
};

static void surface_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct tracked_surface *tracked = wl_container_of(listener, tracked, commit);
    if (hold_mode && tracked->surface->current.buffer && !held) {
        held = wlr_buffer_lock(tracked->surface->current.buffer);
    }
}

static void surface_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct tracked_surface *tracked = wl_container_of(listener, tracked, destroy);
    wl_list_remove(&tracked->commit.link);
    wl_list_remove(&tracked->destroy.link);
    free(tracked);
}

static void surface_created(struct wl_listener *listener, void *data) {
    (void)listener;
    struct wlr_surface *surface = data;
    sc7_render_protocol_watch_surface(surface);
    sc7_render_protocol_watch_surface(surface); /* Must be idempotent. */
    struct tracked_surface *tracked = calloc(1, sizeof(*tracked));
    assert(tracked);
    tracked->surface = surface;
    tracked->commit.notify = surface_commit;
    tracked->destroy.notify = surface_destroy;
    wl_signal_add(&surface->events.commit, &tracked->commit);
    wl_signal_add(&surface->events.destroy, &tracked->destroy);
}

static void global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
            version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    }
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener registry_listener = {global, global_remove};

static void sync_done(void *data, struct wl_callback *callback, uint32_t serial) {
    (void)serial;
    *(bool *)data = true;
    wl_callback_destroy(callback);
}
static const struct wl_callback_listener sync_listener = {sync_done};

static void pump(struct wl_display *client) {
    bool done = false;
    struct wl_callback *sync = wl_display_sync(client);
    wl_callback_add_listener(sync, &sync_listener, &done);
    assert(wl_display_flush(client) >= 0);
    while (!done) {
        assert(wl_event_loop_dispatch(wl_display_get_event_loop(server_display), 0) >= 0);
        wl_display_flush_clients(server_display);
        assert(wl_display_dispatch(client) >= 0);
    }
}

static void buffer_release(void *data, struct wl_buffer *buffer) {
    (void)data; (void)buffer;
    ++wire_releases;
}
static const struct wl_buffer_listener buffer_listener = {buffer_release};

struct internal_buffer { struct wlr_buffer base; };
static void internal_destroy(struct wlr_buffer *buffer) {
    free(buffer);
}
static const struct wlr_buffer_impl internal_impl = {.destroy = internal_destroy};

int main(int argc, char **argv) {
    assert(argc == 3);
    const char *mode = argv[1];
    bool enabled = getenv("SC7_RENDER_TRACE") != NULL;
    log_file = fopen(argv[2], "w");
    assert(log_file);
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    server_display = wl_display_create();
    assert(server_display);
    assert(wl_client_create(server_display, sockets[0]));
    struct wl_display *client = wl_display_connect_to_fd(sockets[1]);
    assert(client);
    struct wlr_compositor *server_compositor = wlr_compositor_create(server_display, 4, NULL);
    assert(server_compositor);
    uint32_t formats[] = {0x34325258, 0x34325241}; /* DRM XRGB8888, ARGB8888 */
    assert(wlr_shm_create(server_display, 1, formats, 2));
    new_surface.notify = surface_created;
    wl_signal_add(&server_compositor->events.new_surface, &new_surface);
    struct wl_registry *registry = wl_display_get_registry(client);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    pump(client);
    assert(compositor && shm);
    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    int fd = memfd_create("sc7-protocol-test", MFD_CLOEXEC);
    assert(fd >= 0 && ftruncate(fd, 64 * 64 * 4) == 0);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, 64 * 64 * 4);
    close(fd);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, 64, 64, 64 * 4,
        WL_SHM_FORMAT_XRGB8888);
    wl_buffer_add_listener(buffer, &buffer_listener, NULL);
    pump(client);

    unsigned cycles = strcmp(mode, "cycles") == 0 ? 128 : 1;
    bool identity_mode = strcmp(mode, "identity") == 0;
    bool destroy_held = strcmp(mode, "destroy-held") == 0 || identity_mode;
    hold_mode = strcmp(mode, "hold") == 0 || destroy_held;
    for (unsigned index = 0; index < cycles; ++index) {
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_damage(surface, 0, 0, 64, 64);
        wl_surface_commit(surface);
        pump(client);
        if (hold_mode) {
            assert(held && held->n_locks == 1);
            assert(wire_releases == 0 && internal_releases == 0);
        } else {
            assert(wire_releases == index + 1 && internal_releases == index + 1);
        }
    }
    struct wlr_renderer *identity_renderer = NULL;
    struct wlr_client_buffer *saved_wrapper = NULL;
    if (identity_mode) {
        assert(enabled);
        identity_renderer = wlr_pixman_renderer_create();
        assert(identity_renderer);
        saved_wrapper = wlr_client_buffer_create(held, identity_renderer);
        assert(saved_wrapper && held->n_locks == 2);
        struct sc7_render_protocol_identity original, saved;
        assert(sc7_render_protocol_buffer_identity(held, &original));
        assert(original.resource && original.id == wl_proxy_get_id((void *)buffer));
        assert(original.pid == getpid() && original.generation == 1 && original.source == held);
        for (unsigned repeat = 0; repeat < 128; ++repeat) {
            assert(sc7_render_protocol_buffer_identity(&saved_wrapper->base, &saved));
            assert(saved.resource == original.resource && saved.id == original.id &&
                saved.pid == original.pid && saved.generation == original.generation &&
                saved.source == held && held->n_locks == 2);
        }
        /* Defensive bound only: temporarily corrupt a test fixture's passive
         * source pointer, then restore it before wlroots teardown. */
        saved_wrapper->source = &saved_wrapper->base;
        assert(!sc7_render_protocol_buffer_identity(&saved_wrapper->base, &saved));
        assert(!saved.resource && !saved.source && saved.generation == 0);
        saved_wrapper->source = held;
    }
    if (destroy_held) {
        wl_buffer_destroy(buffer);
        buffer = NULL;
        pump(client);
        assert(held && buffer_destroys == 0);
        if (identity_mode) {
            struct sc7_render_protocol_identity stale;
            assert(!sc7_render_protocol_buffer_identity(held, &stale));
            assert(!sc7_render_protocol_buffer_identity(&saved_wrapper->base, &stale));
            assert(!stale.resource && !stale.source && stale.generation == 0);
            wlr_buffer_unlock(&saved_wrapper->base);
            assert(held->n_locks == 1);
            wlr_renderer_destroy(identity_renderer);
            puts("saved-wrapper identity assertions passed");
        }
    }
    if (held) {
        wlr_buffer_unlock(held);
        held = NULL;
        pump(client);
        assert(internal_releases == 1);
        assert(wire_releases == (destroy_held ? 0u : 1u));
    }
    /* This separate compositor-owned buffer emits an internal release signal,
     * but it has no protocol resource and cannot send wl_buffer.release. */
    struct internal_buffer *internal = calloc(1, sizeof(*internal));
    assert(internal);
    wlr_buffer_init(&internal->base, &internal_impl, 1, 1);
    sc7_render_watch_buffer(&internal->base);
    wlr_buffer_lock(&internal->base);
    wlr_buffer_unlock(&internal->base);
    wlr_buffer_drop(&internal->base);
    unsigned expected_wire = destroy_held ? 0 : cycles;
    assert(wire_releases == expected_wire);
    assert(internal_releases == cycles + 1);
    wl_surface_attach(surface, NULL, 0, 0);
    wl_surface_commit(surface);
    pump(client);
    wl_surface_destroy(surface);
    if (buffer) {
        wl_buffer_destroy(buffer);
    }
    wl_shm_pool_destroy(pool);
    wl_shm_destroy(shm);
    wl_compositor_destroy(compositor);
    wl_registry_destroy(registry);
    pump(client);
    wl_display_disconnect(client);
    wl_display_destroy_clients(server_display);
    wl_list_remove(&new_surface.link);
    wl_display_destroy(server_display);
    assert(wl_list_empty(&watched));
    fclose(log_file);
    printf("protocol assertions passed wire=%u internal=%u cycles=%u enabled=%d\n",
        wire_releases, internal_releases, cycles, enabled);
    return 0;
}
