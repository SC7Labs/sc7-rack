// Exercise the xdg_popup wire protocol against a real compositor.
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

struct globals {
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    uint32_t wm_base_version;
};

struct surface_state {
    unsigned configures;
    bool before_initial_commit;
    bool premature_configure;
};

struct popup_state {
    unsigned geometry_configures;
    unsigned done;
    uint32_t repositioned_token;
};

struct pixel_buffer {
    struct wl_buffer *buffer;
    void *pixels;
    size_t length;
};

static void registry_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    struct globals *globals = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        globals->compositor = wl_registry_bind(registry, name,
            &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        globals->shm = wl_registry_bind(registry, name,
            &wl_shm_interface, version < 1 ? version : 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        globals->wm_base_version = version < 3 ? version : 3;
        globals->wm_base = wl_registry_bind(registry, name,
            &xdg_wm_base_interface, globals->wm_base_version);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
        uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base,
        uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void surface_configure(void *data, struct xdg_surface *surface,
        uint32_t serial) {
    struct surface_state *state = data;
    state->configures++;
    if (state->before_initial_commit) {
        state->premature_configure = true;
    }
    xdg_surface_ack_configure(surface, serial);
}

static const struct xdg_surface_listener surface_listener = {
    .configure = surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
        int32_t width, int32_t height, struct wl_array *states) {
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)data;
    (void)toplevel;
    fprintf(stderr, "parent toplevel closed unexpectedly\n");
    exit(1);
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void popup_configure(void *data, struct xdg_popup *popup,
        int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)popup;
    (void)x;
    (void)y;
    struct popup_state *state = data;
    if (width <= 0 || height <= 0) {
        fprintf(stderr, "invalid popup geometry %dx%d\n", width, height);
        exit(1);
    }
    state->geometry_configures++;
}

static void popup_done(void *data, struct xdg_popup *popup) {
    (void)popup;
    struct popup_state *state = data;
    state->done++;
}

static void popup_repositioned(void *data, struct xdg_popup *popup,
        uint32_t token) {
    (void)popup;
    struct popup_state *state = data;
    state->repositioned_token = token;
}

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

static struct pixel_buffer create_buffer(struct globals *globals,
        int32_t width, int32_t height) {
    struct pixel_buffer result = {0};
    result.length = (size_t)width * (size_t)height * 4;
    int fd = memfd_create("sc7-popup-test", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)result.length) != 0) {
        perror("create shm buffer");
        exit(1);
    }
    result.pixels = mmap(NULL, result.length, PROT_READ | PROT_WRITE,
        MAP_SHARED, fd, 0);
    if (result.pixels == MAP_FAILED) {
        perror("map shm buffer");
        exit(1);
    }
    memset(result.pixels, 0x48, result.length);
    struct wl_shm_pool *pool = wl_shm_create_pool(globals->shm, fd,
        (int32_t)result.length);
    result.buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
        width * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return result;
}

static void destroy_buffer(struct pixel_buffer *buffer) {
    wl_buffer_destroy(buffer->buffer);
    munmap(buffer->pixels, buffer->length);
}

static void wait_for_event(struct wl_display *display, unsigned *counter,
        unsigned expected, const char *description) {
    for (unsigned attempts = 0; *counter < expected && attempts < 25;
            attempts++) {
        if (wl_display_dispatch_pending(display) < 0 ||
                wl_display_flush(display) < 0) {
            fprintf(stderr, "Wayland disconnect while waiting for %s: %s\n",
                description, strerror(wl_display_get_error(display)));
            exit(1);
        }
        if (*counter >= expected) {
            break;
        }
        struct pollfd pfd = {
            .fd = wl_display_get_fd(display),
            .events = POLLIN,
        };
        int ready = poll(&pfd, 1, 200);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0 || wl_display_dispatch(display) < 0) {
            fprintf(stderr, "timed out/disconnected while waiting for %s\n",
                description);
            exit(1);
        }
    }
    if (*counter < expected) {
        fprintf(stderr, "missing %s (%u < %u)\n", description,
            *counter, expected);
        exit(1);
    }
}

static void wait_for_reposition(struct wl_display *display,
        struct surface_state *surface, struct popup_state *popup,
        uint32_t token) {
    for (unsigned attempts = 0; attempts < 25; attempts++) {
        if (wl_display_dispatch_pending(display) < 0 ||
                wl_display_flush(display) < 0) {
            fprintf(stderr, "Wayland disconnect during popup reposition\n");
            exit(1);
        }
        if (surface->configures >= 2 && popup->geometry_configures >= 2 &&
                popup->repositioned_token == token) {
            return;
        }
        struct pollfd pfd = {
            .fd = wl_display_get_fd(display),
            .events = POLLIN,
        };
        int ready = poll(&pfd, 1, 200);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0 || wl_display_dispatch(display) < 0) {
            fprintf(stderr, "timed out/disconnected during popup reposition\n");
            exit(1);
        }
    }
    fprintf(stderr, "popup reposition did not configure and acknowledge token\n");
    exit(1);
}

static struct xdg_positioner *positioner(struct globals *globals,
        int32_t anchor_x) {
    struct xdg_positioner *positioner =
        xdg_wm_base_create_positioner(globals->wm_base);
    xdg_positioner_set_size(positioner, 120, 80);
    xdg_positioner_set_anchor_rect(positioner, anchor_x, 25, 24, 24);
    xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
    xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(positioner,
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    return positioner;
}

int main(int argc, char **argv) {
    unsigned cycles = 32;
    if (argc > 1) {
        char *end = NULL;
        unsigned long parsed = strtoul(argv[1], &end, 10);
        if (!end || *end != '\0' || parsed == 0 || parsed > 1000) {
            fprintf(stderr, "usage: %s [cycles: 1..1000]\n", argv[0]);
            return 2;
        }
        cycles = (unsigned)parsed;
    }

    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        perror("connect to isolated Wayland display");
        return 1;
    }
    struct globals globals = {0};
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &globals);
    if (wl_display_roundtrip(display) < 0 || !globals.compositor ||
            !globals.shm || !globals.wm_base) {
        fprintf(stderr, "compositor, shm or xdg_wm_base unavailable\n");
        return 1;
    }
    xdg_wm_base_add_listener(globals.wm_base, &wm_base_listener, NULL);

    struct wl_surface *parent_surface =
        wl_compositor_create_surface(globals.compositor);
    struct xdg_surface *parent_xdg =
        xdg_wm_base_get_xdg_surface(globals.wm_base, parent_surface);
    struct surface_state parent_state = {0};
    xdg_surface_add_listener(parent_xdg, &surface_listener, &parent_state);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(parent_xdg);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "SC7 popup lifecycle test");
    wl_surface_commit(parent_surface);
    wait_for_event(display, &parent_state.configures, 1, "parent configure");
    struct pixel_buffer parent_buffer = create_buffer(&globals, 640, 480);
    wl_surface_attach(parent_surface, parent_buffer.buffer, 0, 0);
    wl_surface_damage(parent_surface, 0, 0, 640, 480);
    wl_surface_commit(parent_surface);
    if (wl_display_roundtrip(display) < 0) {
        fprintf(stderr, "parent disconnected before popup creation\n");
        return 1;
    }

    for (unsigned cycle = 0; cycle < cycles; cycle++) {
        struct wl_surface *surface =
            wl_compositor_create_surface(globals.compositor);
        struct xdg_surface *xdg_surface =
            xdg_wm_base_get_xdg_surface(globals.wm_base, surface);
        struct surface_state state = {.before_initial_commit = true};
        struct popup_state popup_state = {0};
        xdg_surface_add_listener(xdg_surface, &surface_listener, &state);
        struct xdg_positioner *initial = positioner(&globals, 20);
        struct xdg_popup *popup =
            xdg_surface_get_popup(xdg_surface, parent_xdg, initial);
        xdg_popup_add_listener(popup, &popup_listener, &popup_state);
        xdg_positioner_destroy(initial);

        // The compositor has processed popup creation, but the popup's
        // initial empty surface commit has not happened yet.
        if (wl_display_roundtrip(display) < 0 ||
                wl_display_roundtrip(display) < 0 ||
                state.premature_configure || state.configures != 0 ||
                popup_state.geometry_configures != 0) {
            fprintf(stderr, "cycle %u: popup configured before initial commit\n",
                cycle + 1);
            return 1;
        }

        state.before_initial_commit = false;
        wl_surface_commit(surface);
        wait_for_event(display, &state.configures, 1,
            "first popup xdg_surface.configure");
        if (popup_state.geometry_configures == 0) {
            fprintf(stderr, "cycle %u: first popup geometry missing\n",
                cycle + 1);
            return 1;
        }
        struct pixel_buffer buffer = create_buffer(&globals, 120, 80);
        wl_surface_attach(surface, buffer.buffer, 0, 0);
        wl_surface_damage(surface, 0, 0, 120, 80);
        wl_surface_commit(surface);

        struct xdg_positioner *moved = positioner(&globals, 80);
        uint32_t token = cycle + 1;
        xdg_popup_reposition(popup, moved, token);
        xdg_positioner_destroy(moved);
        wait_for_reposition(display, &state, &popup_state, token);
        if (popup_state.repositioned_token != token ||
                popup_state.geometry_configures < 2 || state.configures < 2 ||
                popup_state.done != 0) {
            fprintf(stderr, "cycle %u: popup reposition failed\n", cycle + 1);
            return 1;
        }
        wl_surface_commit(surface);
        xdg_popup_destroy(popup);
        xdg_surface_destroy(xdg_surface);
        wl_surface_destroy(surface);
        destroy_buffer(&buffer);
        if (wl_display_roundtrip(display) < 0) {
            fprintf(stderr, "cycle %u: disconnected on popup destroy\n",
                cycle + 1);
            return 1;
        }
    }

    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(parent_xdg);
    wl_surface_destroy(parent_surface);
    destroy_buffer(&parent_buffer);
    xdg_wm_base_destroy(globals.wm_base);
    wl_shm_destroy(globals.shm);
    wl_compositor_destroy(globals.compositor);
    wl_registry_destroy(registry);
    if (wl_display_roundtrip(display) < 0) {
        fprintf(stderr, "disconnected during final cleanup\n");
        return 1;
    }
    wl_display_disconnect(display);
    printf("%u popup cycles: create, no pre-commit configure, first commit, "
        "reposition, destroy\n", cycles);
    return 0;
}
