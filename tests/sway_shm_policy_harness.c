/* Compile the authenticated Sway filter separately from renderer setup, then
 * exercise advertisement, binding and SHM commits through real Wayland clients.
 * Only Sway's security/Xwayland context objects are fixtures. */
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
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

void rack_test_install_policy(struct wl_display *display);
void rack_test_set_context(struct wl_client *sandboxed, struct wl_global *privileged,
    struct wl_global *xway_shell, struct wl_client *xway_client, bool xway_server);

static const struct wl_interface dmabuf_interface = {.name = "zwp_linux_dmabuf_v1", .version = 4};
static const struct wl_interface drm_interface = {.name = "wl_drm", .version = 2};
static const struct wl_interface privileged_interface = {.name = "sc7_privileged", .version = 1};
static const struct wl_interface xway_interface = {.name = "xwayland_shell_v1", .version = 1};
static const struct wl_interface ordinary_interface = {.name = "sc7_other", .version = 1};
static const struct wl_interface similar_interface = {.name = "zwp_linux_dmabuf_v10", .version = 1};

struct session {
    struct wl_display *server, *client;
    struct wl_client *server_client;
    struct wl_global *privileged_global, *xway_global;
    bool xway_authorized, xway_server;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_listener new_surface, commit, destroy;
    struct wlr_surface *surface;
    unsigned dmabuf, drm, shm_count, privileged, xway, ordinary, similar;
    unsigned commits, releases;
    uint32_t expected_pixel;
};

static void bind_fixture(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    assert(wl_resource_create(client, data, version, id));
}

static void registry_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    struct session *s = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        s->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        ++s->shm_count;
        s->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, dmabuf_interface.name) == 0) { ++s->dmabuf;
    } else if (strcmp(interface, drm_interface.name) == 0) { ++s->drm;
    } else if (strcmp(interface, privileged_interface.name) == 0) { ++s->privileged;
    } else if (strcmp(interface, xway_interface.name) == 0) { ++s->xway;
    } else if (strcmp(interface, ordinary_interface.name) == 0) { ++s->ordinary;
    } else if (strcmp(interface, similar_interface.name) == 0) { ++s->similar; }
}
static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener registry_listener = {registry_global, registry_remove};
static void sync_done(void *data, struct wl_callback *callback, uint32_t serial) {
    (void)serial;
    *(bool *)data = true;
    wl_callback_destroy(callback);
}
static const struct wl_callback_listener sync_listener = {sync_done};
static void pump(struct session *s) {
    bool done = false;
    struct wl_callback *callback = wl_display_sync(s->client);
    wl_callback_add_listener(callback, &sync_listener, &done);
    assert(wl_display_flush(s->client) >= 0);
    while (!done) {
        assert(wl_event_loop_dispatch(wl_display_get_event_loop(s->server), 0) >= 0);
        wl_display_flush_clients(s->server);
        assert(wl_display_dispatch(s->client) >= 0);
    }
}
static void connect_client(struct session *s) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    s->server_client = wl_client_create(s->server, sockets[0]);
    assert(s->server_client);
    s->client = wl_display_connect_to_fd(sockets[1]);
    assert(s->client);
}
static void discover(struct session *s) {
    struct wl_registry *registry = wl_display_get_registry(s->client);
    wl_registry_add_listener(registry, &registry_listener, s);
    pump(s);
    wl_registry_destroy(registry);
}
static void disconnect_client(struct session *s) {
    if (s->shm) { wl_shm_destroy(s->shm); }
    if (s->compositor) { wl_compositor_destroy(s->compositor); }
    wl_display_disconnect(s->client);
    assert(wl_event_loop_dispatch(wl_display_get_event_loop(s->server), 0) >= 0);
}

static void check_known_bind(struct session *original, struct wl_global *global,
        const struct wl_interface *interface, bool allowed, bool sandboxed) {
    struct session probe = {.server = original->server};
    connect_client(&probe);
    rack_test_set_context(sandboxed ? probe.server_client : NULL, original->privileged_global,
        original->xway_global, original->xway_authorized ? probe.server_client : NULL,
        original->xway_server);
    /* Acquire the numeric name with policy temporarily disabled, then restore
     * it before bind to represent a client knowing an otherwise hidden ID. */
    wl_display_set_global_filter(probe.server, NULL, NULL);
    uint32_t name = wl_global_get_name(global, probe.server_client);
    assert(name);
    rack_test_install_policy(probe.server);
    struct wl_registry *registry = wl_display_get_registry(probe.client);
    wl_registry_add_listener(registry, &registry_listener, &probe);
    pump(&probe);
    struct wl_proxy *bound = wl_registry_bind(registry, name, interface, 1);
    assert(bound && wl_display_flush(probe.client) >= 0);
    assert(wl_event_loop_dispatch(wl_display_get_event_loop(probe.server), 0) >= 0);
    wl_display_flush_clients(probe.server);
    if (allowed) {
        pump(&probe);
        assert(wl_display_get_error(probe.client) == 0);
    } else {
        assert(wl_display_dispatch(probe.client) == -1);
        const struct wl_interface *error_interface = NULL;
        uint32_t id = 0;
        uint32_t code = wl_display_get_protocol_error(probe.client, &error_interface, &id);
        assert(error_interface == &wl_registry_interface &&
            code == WL_DISPLAY_ERROR_INVALID_OBJECT && id);
    }
    wl_proxy_destroy(bound);
    wl_registry_destroy(registry);
    disconnect_client(&probe);
}

static void surface_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct session *s = wl_container_of(listener, s, commit);
    struct wlr_buffer *buffer = s->surface->current.buffer;
    struct wlr_shm_attributes shm;
    struct wlr_dmabuf_attributes dma;
    assert(buffer && wlr_buffer_get_shm(buffer, &shm) && !wlr_buffer_get_dmabuf(buffer, &dma));
    void *pixels;
    uint32_t format;
    size_t stride;
    assert(wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ,
        &pixels, &format, &stride));
    assert(stride == 32 && buffer->width == 8 && buffer->height == 8);
    assert(((uint32_t *)pixels)[0] == s->expected_pixel);
    wlr_buffer_end_data_ptr_access(buffer);
    ++s->commits;
}
static void surface_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct session *s = wl_container_of(listener, s, destroy);
    wl_list_remove(&s->commit.link);
    wl_list_remove(&s->destroy.link);
    s->surface = NULL;
}
static void surface_created(struct wl_listener *listener, void *data) {
    struct session *s = wl_container_of(listener, s, new_surface);
    assert(!s->surface);
    s->surface = data;
    s->commit.notify = surface_commit;
    s->destroy.notify = surface_destroy;
    wl_signal_add(&s->surface->events.commit, &s->commit);
    wl_signal_add(&s->surface->events.destroy, &s->destroy);
}
static void buffer_release(void *data, struct wl_buffer *buffer) {
    (void)buffer;
    ++((struct session *)data)->releases;
}
static const struct wl_buffer_listener buffer_listener = {buffer_release};
static void commit_shm(struct session *s, unsigned cycles) {
    struct wl_surface *surface = wl_compositor_create_surface(s->compositor);
    int fd = memfd_create("rack-production-shm", MFD_CLOEXEC);
    assert(fd >= 0 && ftruncate(fd, 256) == 0);
    uint32_t *pixels = mmap(NULL, 256, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(pixels != MAP_FAILED);
    struct wl_shm_pool *pool = wl_shm_create_pool(s->shm, fd, 256);
    close(fd);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, 8, 8, 32, WL_SHM_FORMAT_ARGB8888);
    wl_buffer_add_listener(buffer, &buffer_listener, s);
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        pixels[0] = s->expected_pixel = 0xff123456 ^ cycle;
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_damage(surface, 0, 0, 8, 8);
        wl_surface_commit(surface);
        pump(s);
        assert(s->commits == cycle + 1 && s->releases == cycle + 1);
    }
    wl_buffer_destroy(buffer);
    wl_shm_pool_destroy(pool);
    wl_surface_destroy(surface);
    pump(s);
    assert(!s->surface);
    munmap(pixels, 256);
}

int main(int argc, char **argv) {
    assert(argc == 5);
    bool production = strcmp(argv[1], "production") == 0;
    bool sandboxed = strcmp(argv[2], "sandboxed") == 0;
    bool authorized_xway = strcmp(argv[2], "xway-authorized") == 0;
    bool xway_server = strcmp(argv[2], "xway-absent") != 0;
    bool has_xwayland = strcmp(argv[3], "1") == 0;
    unsigned cycles = (unsigned)strtoul(argv[4], NULL, 10);
    assert(cycles > 0 && cycles <= 128);
    struct session s = {0};
    s.server = wl_display_create();
    assert(s.server);
    rack_test_install_policy(s.server);
    struct wlr_compositor *compositor = wlr_compositor_create(s.server, 4, NULL);
    assert(compositor);
    s.new_surface.notify = surface_created;
    wl_signal_add(&compositor->events.new_surface, &s.new_surface);
    uint32_t formats[] = {0x34325258, 0x34325241};
    assert(wlr_shm_create(s.server, 1, formats, 2));
    const struct wl_interface *interfaces[] = {&dmabuf_interface, &drm_interface,
        &privileged_interface, &xway_interface, &ordinary_interface, &similar_interface};
    struct wl_global *globals[6];
    for (unsigned i = 0; i < 6; ++i) {
        globals[i] = wl_global_create(s.server, interfaces[i], interfaces[i]->version,
            (void *)interfaces[i], bind_fixture);
        assert(globals[i]);
    }
    connect_client(&s);
    s.privileged_global = globals[2];
    s.xway_global = globals[3];
    s.xway_authorized = authorized_xway;
    s.xway_server = xway_server;
    rack_test_set_context(sandboxed ? s.server_client : NULL, globals[2], globals[3],
        authorized_xway ? s.server_client : NULL, xway_server);
    discover(&s);
    assert(s.compositor && s.shm && s.shm_count == 1 && s.ordinary == 1 && s.similar == 1);
    assert(s.dmabuf == (production ? 0u : 1u) && s.drm == (production ? 0u : 1u));
    assert(s.privileged == (sandboxed ? 0u : 1u));
    assert(s.xway == (!has_xwayland || (authorized_xway && xway_server) ? 1u : 0u));
    commit_shm(&s, cycles);
    check_known_bind(&s, globals[0], &dmabuf_interface, !production, false);
    check_known_bind(&s, globals[1], &drm_interface, !production, false);
    check_known_bind(&s, globals[2], &privileged_interface, !sandboxed, sandboxed);
    check_known_bind(&s, globals[3], &xway_interface,
        !has_xwayland || (authorized_xway && xway_server), sandboxed);
    check_known_bind(&s, globals[4], &ordinary_interface, true, sandboxed);
    printf("production policy assertions passed dmabuf=%u drm=%u shm=%u ordinary=%u "
        "privileged=%u xway=%u commits=%u releases=%u environment=not-required\n",
        s.dmabuf, s.drm, s.shm_count, s.ordinary, s.privileged, s.xway, s.commits, s.releases);
    disconnect_client(&s);
    wl_display_destroy_clients(s.server);
    wl_list_remove(&s.new_surface.link);
    wl_display_destroy(s.server);
    return 0;
}
