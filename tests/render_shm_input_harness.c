/* Exercise diagnostic capability suppression over real Wayland sockets.
 * The DMA-BUF and wl_drm globals are minimal registry fixtures; wl_shm and
 * wl_surface requests, SHM storage, commits, and buffer releases are real. */
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_shm.h>

static const struct wl_interface dmabuf_interface = {
    .name = "zwp_linux_dmabuf_v1", .version = 4};
static const struct wl_interface drm_interface = {.name = "wl_drm", .version = 2};
static const struct wl_interface hidden_interface = {.name = "sc7_hidden", .version = 1};
static const struct wl_interface similar_interface = {
    .name = "zwp_linux_dmabuf_v10", .version = 1};

static FILE *log_file;
static uint64_t timestamp;
FILE *sc7_render_trace_file(void) { return log_file; }
uint64_t sc7_render_trace_time_us(void) { return ++timestamp; }
uint64_t sc7_render_trace_frame(void) { return 462; }

struct session {
    struct wl_display *server;
    struct wl_display *client;
    struct wl_client *server_client;
    struct wl_global *dmabuf_global, *drm_global;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_listener new_surface;
    struct wl_listener commit;
    struct wl_listener surface_destroy;
    struct wlr_surface *surface;
    bool hide_extra;
    unsigned dmabuf_count, drm_count, shm_count, extra_count, similar_count;
    unsigned commits, releases;
    uint32_t expected_pixel;
};

static void bind_fixture(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    const struct wl_interface *interface = data;
    assert(wl_resource_create(client, interface, version, id));
}

static bool existing_filter(const struct wl_client *client,
        const struct wl_global *global, void *data) {
    (void)client;
    bool *hide_extra = data;
    return !*hide_extra ||
        strcmp(wl_global_get_interface(global)->name, hidden_interface.name) != 0;
}

static void registry_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    struct session *session = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        session->compositor = wl_registry_bind(registry, name,
            &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        ++session->shm_count;
        session->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, dmabuf_interface.name) == 0) {
        ++session->dmabuf_count;
    } else if (strcmp(interface, drm_interface.name) == 0) {
        ++session->drm_count;
    } else if (strcmp(interface, hidden_interface.name) == 0) {
        ++session->extra_count;
    } else if (strcmp(interface, similar_interface.name) == 0) {
        ++session->similar_count;
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener registry_listener = {
    registry_global, registry_remove};

static void sync_done(void *data, struct wl_callback *callback, uint32_t serial) {
    (void)serial;
    *(bool *)data = true;
    wl_callback_destroy(callback);
}
static const struct wl_callback_listener sync_listener = {sync_done};

static void pump(struct session *session) {
    bool done = false;
    struct wl_callback *callback = wl_display_sync(session->client);
    wl_callback_add_listener(callback, &sync_listener, &done);
    assert(wl_display_flush(session->client) >= 0);
    while (!done) {
        assert(wl_event_loop_dispatch(wl_display_get_event_loop(session->server), 0) >= 0);
        wl_display_flush_clients(session->server);
        assert(wl_display_dispatch(session->client) >= 0);
    }
}

static void surface_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct session *session = wl_container_of(listener, session, commit);
    struct wlr_buffer *buffer = session->surface->current.buffer;
    assert(buffer && buffer->n_locks > 0);
    struct wlr_shm_attributes shm;
    struct wlr_dmabuf_attributes dmabuf;
    assert(wlr_buffer_get_shm(buffer, &shm));
    assert(!wlr_buffer_get_dmabuf(buffer, &dmabuf));
    void *pixels;
    uint32_t format;
    size_t stride;
    assert(wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ,
        &pixels, &format, &stride));
    assert(buffer->width == 8 && buffer->height == 8 && stride == 8 * 4);
    assert(((uint32_t *)pixels)[0] == session->expected_pixel);
    wlr_buffer_end_data_ptr_access(buffer);
    ++session->commits;
}

static void surface_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct session *session = wl_container_of(listener, session, surface_destroy);
    wl_list_remove(&session->commit.link);
    wl_list_remove(&session->surface_destroy.link);
    session->surface = NULL;
}

static void surface_created(struct wl_listener *listener, void *data) {
    struct session *session = wl_container_of(listener, session, new_surface);
    assert(!session->surface);
    session->surface = data;
    session->commit.notify = surface_commit;
    session->surface_destroy.notify = surface_destroy;
    wl_signal_add(&session->surface->events.commit, &session->commit);
    wl_signal_add(&session->surface->events.destroy, &session->surface_destroy);
}

static void buffer_release(void *data, struct wl_buffer *buffer) {
    (void)buffer;
    struct session *session = data;
    ++session->releases;
}
static const struct wl_buffer_listener buffer_listener = {buffer_release};

static void create_session(struct session *session, bool suppressed, bool hide_extra) {
    memset(session, 0, sizeof(*session));
    session->hide_extra = hide_extra;
    session->server = wl_display_create();
    assert(session->server);
    wl_display_set_global_filter(session->server, hide_extra ? existing_filter : NULL,
        hide_extra ? &session->hide_extra : NULL);
    struct wlr_compositor *compositor = wlr_compositor_create(session->server, 4, NULL);
    assert(compositor);
    session->new_surface.notify = surface_created;
    wl_signal_add(&compositor->events.new_surface, &session->new_surface);
    uint32_t formats[] = {0x34325258, 0x34325241}; /* DRM XRGB8888, ARGB8888 */
    assert(wlr_shm_create(session->server, 1, formats, 2));
    struct wl_global *dmabuf = wl_global_create(session->server, &dmabuf_interface,
        4, (void *)&dmabuf_interface, bind_fixture);
    struct wl_global *drm = wl_global_create(session->server, &drm_interface,
        2, (void *)&drm_interface, bind_fixture);
    assert(dmabuf && drm); /* Suppression must not fail wlroots global creation. */
    session->dmabuf_global = dmabuf;
    session->drm_global = drm;
    assert(wl_global_create(session->server, &hidden_interface, 1,
        (void *)&hidden_interface, bind_fixture));
    assert(wl_global_create(session->server, &similar_interface, 1,
        (void *)&similar_interface, bind_fixture));
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    session->server_client = wl_client_create(session->server, sockets[0]);
    assert(session->server_client);
    session->client = wl_display_connect_to_fd(sockets[1]);
    assert(session->client);
    struct wl_registry *registry = wl_display_get_registry(session->client);
    wl_registry_add_listener(registry, &registry_listener, session);
    pump(session);
    assert(session->compositor && session->shm && session->shm_count == 1);
    assert(session->dmabuf_count == (suppressed ? 0u : 1u));
    assert(session->drm_count == (suppressed ? 0u : 1u));
    assert(session->extra_count == (hide_extra ? 0u : 1u));
    assert(session->similar_count == 1);
    wl_registry_destroy(registry);
}

static void probe_existing_display(struct session *original, bool suppressed,
        bool hide_extra) {
    /* A new registry/client observes the currently installed filter, rather
     * than assuming the setter retroactively sends global_remove events. */
    struct session probe = {.server = original->server};
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    assert(wl_client_create(probe.server, sockets[0]));
    probe.client = wl_display_connect_to_fd(sockets[1]);
    assert(probe.client);
    struct wl_registry *registry = wl_display_get_registry(probe.client);
    wl_registry_add_listener(registry, &registry_listener, &probe);
    pump(&probe);
    assert(probe.compositor && probe.shm && probe.shm_count == 1);
    assert(probe.dmabuf_count == (suppressed ? 0u : 1u));
    assert(probe.drm_count == (suppressed ? 0u : 1u));
    assert(probe.extra_count == (hide_extra ? 0u : 1u));
    assert(probe.similar_count == 1);
    wl_registry_destroy(registry);
    wl_shm_destroy(probe.shm);
    wl_compositor_destroy(probe.compositor);
    wl_display_disconnect(probe.client);
    assert(wl_event_loop_dispatch(wl_display_get_event_loop(probe.server), 0) >= 0);
}

static void bind_known_global(struct session *original, struct wl_global *global,
        const struct wl_interface *interface, bool suppressed) {
    /* Binding a hidden global by its numeric name must not bypass the filter.
     * Use a disposable client because libwayland posts a protocol error. */
    struct session probe = {.server = original->server};
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    struct wl_client *server_client = wl_client_create(probe.server, sockets[0]);
    assert(server_client);
    probe.client = wl_display_connect_to_fd(sockets[1]);
    assert(probe.client);
    struct wl_registry *registry = wl_display_get_registry(probe.client);
    wl_registry_add_listener(registry, &registry_listener, &probe);
    pump(&probe);
    uint32_t name;
    if (suppressed) {
        /* The public name getter also honors filters. Acquire a known name
         * while the policy is temporarily off, then reinstall the policy
         * before sending bind, like a client retaining an older global ID. */
        const char *mode = getenv("SC7_RENDER_EXPERIMENT");
        assert(mode);
        char *saved_mode = strdup(mode);
        assert(saved_mode);
        assert(unsetenv("SC7_RENDER_EXPERIMENT") == 0);
        wl_display_set_global_filter(original->server, NULL, NULL);
        name = wl_global_get_name(global, server_client);
        assert(setenv("SC7_RENDER_EXPERIMENT", saved_mode, 1) == 0);
        free(saved_mode);
        wl_display_set_global_filter(original->server, existing_filter,
            &original->hide_extra);
    } else {
        name = wl_global_get_name(global, server_client);
    }
    assert(name);
    struct wl_proxy *bound = wl_registry_bind(registry, name, interface, 1);
    assert(bound);
    assert(wl_display_flush(probe.client) >= 0);
    assert(wl_event_loop_dispatch(wl_display_get_event_loop(probe.server), 0) >= 0);
    wl_display_flush_clients(probe.server);
    if (suppressed) {
        assert(wl_display_dispatch(probe.client) == -1);
        const struct wl_interface *error_interface = NULL;
        uint32_t object_id = 0;
        uint32_t code = wl_display_get_protocol_error(probe.client,
            &error_interface, &object_id);
        assert(error_interface == &wl_registry_interface &&
            code == WL_DISPLAY_ERROR_INVALID_OBJECT && object_id);
    } else {
        pump(&probe);
        assert(wl_display_get_error(probe.client) == 0);
    }
    wl_proxy_destroy(bound);
    wl_registry_destroy(registry);
    wl_shm_destroy(probe.shm);
    wl_compositor_destroy(probe.compositor);
    wl_display_disconnect(probe.client);
    assert(wl_event_loop_dispatch(wl_display_get_event_loop(probe.server), 0) >= 0);
}

static void commit_shm(struct session *session, unsigned cycles) {
    struct wl_surface *surface = wl_compositor_create_surface(session->compositor);
    int fd = memfd_create("sc7-shm-input-test", MFD_CLOEXEC);
    const size_t size = 8 * 8 * 4;
    assert(fd >= 0 && ftruncate(fd, size) == 0);
    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(pixels != MAP_FAILED);
    struct wl_shm_pool *pool = wl_shm_create_pool(session->shm, fd, (int)size);
    close(fd);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, 8, 8, 8 * 4,
        WL_SHM_FORMAT_ARGB8888);
    wl_buffer_add_listener(buffer, &buffer_listener, session);
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        session->expected_pixel = 0xffabcdefu ^ cycle;
        pixels[0] = session->expected_pixel;
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_damage(surface, 0, 0, 8, 8);
        wl_surface_commit(surface);
        pump(session);
        assert(session->commits == cycle + 1);
        assert(session->releases == cycle + 1);
    }
    wl_buffer_destroy(buffer);
    wl_shm_pool_destroy(pool);
    wl_surface_destroy(surface);
    pump(session);
    assert(session->surface == NULL);
    munmap(pixels, size);
}

static void destroy_session(struct session *session) {
    wl_shm_destroy(session->shm);
    wl_compositor_destroy(session->compositor);
    wl_display_disconnect(session->client);
    wl_display_destroy_clients(session->server);
    wl_list_remove(&session->new_surface.link);
    wl_display_destroy(session->server);
}

int main(int argc, char **argv) {
    assert(argc == 4);
    bool suppressed = strcmp(argv[1], "suppressed") == 0;
    unsigned cycles = (unsigned)strtoul(argv[2], NULL, 10);
    assert(cycles > 0 && cycles <= 128);
    log_file = fopen(argv[3], "w");
    assert(log_file);
    struct session first, second;
    create_session(&first, suppressed, true);
    create_session(&second, suppressed, false);
    bool replacement_data = false;
    wl_display_set_global_filter(first.server, existing_filter, &replacement_data);
    probe_existing_display(&first, suppressed, false);
    probe_existing_display(&second, suppressed, false);
    replacement_data = true;
    wl_display_set_global_filter(first.server, existing_filter, &replacement_data);
    probe_existing_display(&first, suppressed, true);
    probe_existing_display(&second, suppressed, false);
    wl_display_set_global_filter(first.server, NULL, NULL);
    probe_existing_display(&first, suppressed, false);
    wl_display_set_global_filter(first.server, existing_filter, &first.hide_extra);
    probe_existing_display(&first, suppressed, true);
    bind_known_global(&first, first.dmabuf_global, &dmabuf_interface, suppressed);
    bind_known_global(&first, first.drm_global, &drm_interface, suppressed);
    commit_shm(&first, cycles);
    commit_shm(&second, cycles);
    printf("capability assertions passed dmabuf=%u drm=%u shm=%u "
        "commits=%u releases=%u displays=2 existing_filters=preserved "
        "filter_updates=preserved hidden_bind=%s\n",
        first.dmabuf_count, first.drm_count, first.shm_count,
        first.commits + second.commits, first.releases + second.releases,
        suppressed ? "rejected" : "allowed");
    destroy_session(&second);
    probe_existing_display(&first, suppressed, true);
    destroy_session(&first);
    create_session(&first, suppressed, true);
    commit_shm(&first, 1);
    destroy_session(&first);
    fclose(log_file);
    return 0;
}
