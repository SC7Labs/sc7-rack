// Exercise a grabbed context menu and its pointer interaction over Wayland.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/input-event-codes.h>
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
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

enum { MENU_WIDTH = 120, MENU_HEIGHT = 80 };

struct app {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_output *output;
    struct xdg_wm_base *wm_base;
    struct zwlr_virtual_pointer_manager_v1 *virtual_manager;
    struct zwlr_virtual_pointer_v1 *virtual_pointer;
    int32_t output_width;
    int32_t output_height;
    struct wl_surface *parent_surface;
    struct wl_surface *popup_surface;
    struct wl_surface *focused_surface;
    struct xdg_surface *parent_xdg;
    struct xdg_surface *popup_xdg;
    struct xdg_popup *popup;
    unsigned parent_configures;
    unsigned popup_configures;
    unsigned popup_enters;
    unsigned popup_moves;
    unsigned popup_done;
    unsigned selected;
    uint32_t right_press_serial;
    double pointer_x;
    double pointer_y;
    int32_t menu_x;
    int32_t menu_y;
    int32_t menu_width;
    int32_t menu_height;
};

struct pixel_buffer {
    struct wl_buffer *buffer;
    void *pixels;
    size_t length;
};

static void output_geometry(void *data, struct wl_output *output,
        int32_t x, int32_t y, int32_t physical_width,
        int32_t physical_height, int32_t subpixel, const char *make,
        const char *model, int32_t transform) {
    (void)data;
    (void)output;
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)make;
    (void)model;
    (void)transform;
}

static void output_mode(void *data, struct wl_output *output,
        uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    (void)output;
    (void)refresh;
    struct app *app = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        app->output_width = width;
        app->output_height = height;
    }
}

static void output_done(void *data, struct wl_output *output) {
    (void)data;
    (void)output;
}

static void output_scale(void *data, struct wl_output *output, int32_t scale) {
    (void)data;
    (void)output;
    (void)scale;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

static void registry_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    struct app *app = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app->compositor = wl_registry_bind(registry, name,
            &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && !app->seat) {
        app->seat = wl_registry_bind(registry, name,
            &wl_seat_interface, version < 5 ? version : 5);
    } else if (strcmp(interface, wl_output_interface.name) == 0 && !app->output) {
        app->output = wl_registry_bind(registry, name,
            &wl_output_interface, version < 2 ? version : 2);
        wl_output_add_listener(app->output, &output_listener, app);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app->wm_base = wl_registry_bind(registry, name,
            &xdg_wm_base_interface, version < 3 ? version : 3);
    } else if (strcmp(interface,
            zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        app->virtual_manager = wl_registry_bind(registry, name,
            &zwlr_virtual_pointer_manager_v1_interface,
            version < 2 ? version : 2);
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

static void pointer_enter(void *data, struct wl_pointer *pointer,
        uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    (void)pointer;
    (void)serial;
    struct app *app = data;
    app->focused_surface = surface;
    app->pointer_x = wl_fixed_to_double(x);
    app->pointer_y = wl_fixed_to_double(y);
    if (surface == app->popup_surface) {
        app->popup_enters++;
    }
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
        uint32_t serial, struct wl_surface *surface) {
    (void)pointer;
    (void)serial;
    struct app *app = data;
    if (app->focused_surface == surface) {
        app->focused_surface = NULL;
    }
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
        uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    (void)pointer;
    (void)time;
    struct app *app = data;
    app->pointer_x = wl_fixed_to_double(x);
    app->pointer_y = wl_fixed_to_double(y);
    if (app->focused_surface == app->popup_surface) {
        app->popup_moves++;
    }
}

static void pointer_button(void *data, struct wl_pointer *pointer,
        uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    (void)pointer;
    (void)time;
    struct app *app = data;
    if (button == BTN_RIGHT && state == WL_POINTER_BUTTON_STATE_PRESSED &&
            app->focused_surface == app->parent_surface) {
        app->right_press_serial = serial;
    }
    if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED &&
            app->focused_surface == app->popup_surface &&
            app->pointer_x >= 0 && app->pointer_x < app->menu_width &&
            app->pointer_y >= 0 && app->pointer_y < app->menu_height) {
        app->selected++;
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
        uint32_t time, uint32_t axis, wl_fixed_t value) {
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
    (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *pointer) {
    (void)data;
    (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer,
        uint32_t source) {
    (void)data;
    (void)pointer;
    (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
        uint32_t time, uint32_t axis) {
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
        uint32_t axis, int32_t discrete) {
    (void)data;
    (void)pointer;
    (void)axis;
    (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
        uint32_t capabilities) {
    struct app *app = data;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !app->pointer) {
        app->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app->pointer, &pointer_listener, app);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
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
    struct app *app = data;
    if (surface == app->parent_xdg) {
        app->parent_configures++;
    } else if (surface == app->popup_xdg) {
        app->popup_configures++;
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
    struct app *app = data;
    app->menu_x = x;
    app->menu_y = y;
    app->menu_width = width;
    app->menu_height = height;
}

static void popup_done(void *data, struct xdg_popup *popup) {
    (void)popup;
    struct app *app = data;
    app->popup_done++;
}

static void popup_repositioned(void *data, struct xdg_popup *popup,
        uint32_t token) {
    (void)data;
    (void)popup;
    (void)token;
}

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

static struct pixel_buffer create_buffer(struct app *app,
        int32_t width, int32_t height) {
    struct pixel_buffer result = {0};
    result.length = (size_t)width * (size_t)height * 4;
    int fd = memfd_create("sc7-popup-input", MFD_CLOEXEC);
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
    struct wl_shm_pool *pool = wl_shm_create_pool(app->shm, fd,
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

static void await_count(struct app *app, unsigned *counter,
        unsigned expected, const char *description) {
    for (unsigned tries = 0; *counter < expected && tries < 20; tries++) {
        if (wl_display_dispatch_pending(app->display) < 0 ||
                wl_display_flush(app->display) < 0) {
            fprintf(stderr, "Wayland disconnected awaiting %s\n", description);
            exit(1);
        }
        if (*counter >= expected) {
            break;
        }
        struct pollfd pollfd = {.fd = wl_display_get_fd(app->display),
            .events = POLLIN};
        int ready = poll(&pollfd, 1, 200);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0 || wl_display_dispatch(app->display) < 0) {
            fprintf(stderr, "timed out/disconnected awaiting %s\n", description);
            exit(1);
        }
    }
    if (*counter < expected) {
        fprintf(stderr, "missing %s (%u < %u)\n", description,
            *counter, expected);
        exit(1);
    }
}

static void move_pointer(struct app *app, int x, int y, uint32_t time) {
    zwlr_virtual_pointer_v1_motion_absolute(app->virtual_pointer, time,
        (uint32_t)x, (uint32_t)y, app->output_width, app->output_height);
    zwlr_virtual_pointer_v1_frame(app->virtual_pointer);
    if (wl_display_roundtrip(app->display) < 0) {
        fprintf(stderr, "Wayland disconnected moving pointer\n");
        exit(1);
    }
}

static void click_button(struct app *app, uint32_t button,
        uint32_t state, uint32_t time) {
    zwlr_virtual_pointer_v1_button(app->virtual_pointer, time, button, state);
    zwlr_virtual_pointer_v1_frame(app->virtual_pointer);
    if (wl_display_roundtrip(app->display) < 0) {
        fprintf(stderr, "Wayland disconnected clicking pointer\n");
        exit(1);
    }
}

static void fail_cycle(unsigned cycle, const char *reason) {
    fprintf(stderr, "cycle %u: %s\n", cycle + 1, reason);
    exit(1);
}

static int required_coordinate(const char *name) {
    const char *value = getenv(name);
    if (!value) {
        fprintf(stderr, "missing %s for host pointer leave test\n", name);
        exit(1);
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (!end || *end != '\0' || parsed < 0 || parsed > 10000) {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        exit(1);
    }
    return (int)parsed;
}

static void inject_host_leave(struct app *nested, int parent_origin_x,
        int parent_origin_y) {
    const char *host_display = getenv("SC7_POPUP_HOST_DISPLAY");
    if (!host_display) {
        return;
    }
    struct app host = {0};
    host.display = wl_display_connect(host_display);
    if (!host.display) {
        perror("connect to isolated host Wayland display");
        exit(1);
    }
    struct wl_registry *registry = wl_display_get_registry(host.display);
    wl_registry_add_listener(registry, &registry_listener, &host);
    if (wl_display_roundtrip(host.display) < 0 || !host.seat || !host.output ||
            !host.virtual_manager) {
        fprintf(stderr, "host virtual pointer globals unavailable\n");
        exit(1);
    }
    wl_seat_add_listener(host.seat, &seat_listener, &host);
    if (wl_proxy_get_version((struct wl_proxy *)host.virtual_manager) >= 2) {
        host.virtual_pointer =
            zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
                host.virtual_manager, host.seat, host.output);
    } else {
        host.virtual_pointer =
            zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
                host.virtual_manager, host.seat);
    }
    if (wl_display_roundtrip(host.display) < 0) {
        fprintf(stderr, "host virtual pointer disconnected\n");
        exit(1);
    }

    int window_x = required_coordinate("SC7_POPUP_HOST_WINDOW_X");
    int window_y = required_coordinate("SC7_POPUP_HOST_WINDOW_Y");
    int window_width = required_coordinate("SC7_POPUP_HOST_WINDOW_WIDTH");
    int window_height = required_coordinate("SC7_POPUP_HOST_WINDOW_HEIGHT");
    int inside_x = window_x + (parent_origin_x + 45) * window_width /
        nested->output_width;
    int inside_y = window_y + (parent_origin_y + 45) * window_height /
        nested->output_height;
    int outside_x = required_coordinate("SC7_POPUP_HOST_OUTSIDE_X");
    int outside_y = required_coordinate("SC7_POPUP_HOST_OUTSIDE_Y");
    int width = host.output_width;
    int height = host.output_height;
    if (width <= 0 || height <= 0) {
        fprintf(stderr, "host output size unavailable\n");
        exit(1);
    }
    for (unsigned attempts = 0; attempts < 20 &&
            nested->focused_surface != nested->parent_surface; attempts++) {
        zwlr_virtual_pointer_v1_motion_absolute(host.virtual_pointer,
            attempts + 1, inside_x + (attempts % 2),
            inside_y + (attempts % 2), width, height);
        zwlr_virtual_pointer_v1_frame(host.virtual_pointer);
        if (wl_display_roundtrip(host.display) < 0 ||
                wl_display_roundtrip(nested->display) < 0) {
            fprintf(stderr, "host pointer failed to enter nested compositor\n");
            exit(1);
        }
        usleep(10000);
    }
    if (nested->focused_surface != nested->parent_surface) {
        fprintf(stderr, "host pointer at %d,%d did not enter nested parent "
            "(output %dx%d, parent origin %d,%d)\n", inside_x, inside_y,
            nested->output_width, nested->output_height,
            parent_origin_x, parent_origin_y);
        exit(1);
    }
    nested->right_press_serial = 0;
    zwlr_virtual_pointer_v1_button(host.virtual_pointer, 2, BTN_RIGHT,
        WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(host.virtual_pointer);
    if (wl_display_roundtrip(host.display) < 0 ||
            wl_display_roundtrip(nested->display) < 0) {
        fprintf(stderr, "host pointer press disconnected compositor\n");
        exit(1);
    }
    if (!nested->right_press_serial) {
        fprintf(stderr, "host right press did not reach nested parent\n");
        exit(1);
    }
    zwlr_virtual_pointer_v1_motion_absolute(host.virtual_pointer, 3,
        outside_x, outside_y, width, height);
    zwlr_virtual_pointer_v1_frame(host.virtual_pointer);
    if (wl_display_roundtrip(host.display) < 0 ||
            wl_display_roundtrip(nested->display) < 0) {
        fprintf(stderr, "host pointer leave disconnected compositor\n");
        exit(1);
    }
    zwlr_virtual_pointer_v1_destroy(host.virtual_pointer);
    if (wl_display_roundtrip(host.display) < 0 ||
            wl_display_roundtrip(nested->display) < 0) {
        fprintf(stderr, "host pointer removal disconnected compositor\n");
        exit(1);
    }
    zwlr_virtual_pointer_manager_v1_destroy(host.virtual_manager);
    if (host.pointer) {
        wl_pointer_release(host.pointer);
    }
    wl_seat_release(host.seat);
    wl_output_release(host.output);
    if (host.compositor) {
        wl_compositor_destroy(host.compositor);
    }
    if (host.shm) {
        wl_shm_destroy(host.shm);
    }
    if (host.wm_base) {
        xdg_wm_base_destroy(host.wm_base);
    }
    wl_registry_destroy(registry);
    wl_display_disconnect(host.display);
}

int main(int argc, char **argv) {
    unsigned cycles = 32;
    if (argc > 1) {
        char *end = NULL;
        unsigned long value = strtoul(argv[1], &end, 10);
        if (!end || *end != '\0' || value < 1 || value > 1000) {
            fprintf(stderr, "usage: %s [cycles: 1..1000]\n", argv[0]);
            return 2;
        }
        cycles = (unsigned)value;
    }
    struct app app = {0};
    app.display = wl_display_connect(NULL);
    if (!app.display) {
        perror("connect to Wayland display");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(registry, &registry_listener, &app);
    if (wl_display_roundtrip(app.display) < 0 || !app.compositor || !app.shm ||
            !app.seat || !app.wm_base || !app.virtual_manager || !app.output) {
        fprintf(stderr, "required Wayland globals unavailable\n");
        return 1;
    }
    wl_seat_add_listener(app.seat, &seat_listener, &app);
    xdg_wm_base_add_listener(app.wm_base, &wm_base_listener, NULL);
    if (wl_display_roundtrip(app.display) < 0) {
        fprintf(stderr, "Wayland disconnected reading seat capabilities\n");
        return 1;
    }
    if (app.output_width <= 0 || app.output_height <= 0) {
        fprintf(stderr, "nested output size unavailable\n");
        return 1;
    }
    if (wl_proxy_get_version((struct wl_proxy *)app.virtual_manager) >= 2) {
        app.virtual_pointer =
            zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
                app.virtual_manager, app.seat, app.output);
    } else {
        app.virtual_pointer =
            zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
                app.virtual_manager, app.seat);
    }
    if (wl_display_roundtrip(app.display) < 0 || !app.pointer) {
        fprintf(stderr, "virtual pointer did not create a seat pointer\n");
        return 1;
    }

    app.parent_surface = wl_compositor_create_surface(app.compositor);
    app.parent_xdg = xdg_wm_base_get_xdg_surface(app.wm_base,
        app.parent_surface);
    xdg_surface_add_listener(app.parent_xdg, &surface_listener, &app);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(app.parent_xdg);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, &app);
    xdg_toplevel_set_title(toplevel, "SC7 popup input test");
    wl_surface_commit(app.parent_surface);
    await_count(&app, &app.parent_configures, 1, "parent configure");
    struct pixel_buffer parent_buffer = create_buffer(&app, 640, 480);
    wl_surface_attach(app.parent_surface, parent_buffer.buffer, 0, 0);
    wl_surface_damage(app.parent_surface, 0, 0, 640, 480);
    wl_surface_commit(app.parent_surface);
    if (wl_display_roundtrip(app.display) < 0) {
        fprintf(stderr, "Wayland disconnected after parent map\n");
        return 1;
    }

    int parent_origin_x = -1;
    int parent_origin_y = -1;
    for (int y = 40; y < app.output_height && parent_origin_x < 0; y += 80) {
        for (int x = 40; x < app.output_width; x += 80) {
            move_pointer(&app, x, y, 1);
            if (app.focused_surface == app.parent_surface) {
                parent_origin_x = x - (int)app.pointer_x;
                parent_origin_y = y - (int)app.pointer_y;
                break;
            }
        }
    }
    if (parent_origin_x < 0) {
        fprintf(stderr, "virtual pointer never entered parent surface\n");
        return 1;
    }

    for (unsigned cycle = 0; cycle < cycles; cycle++) {
        if (cycle == 1 && getenv("SC7_POPUP_HOST_DISPLAY")) {
            inject_host_leave(&app, parent_origin_x, parent_origin_y);
        }
        uint32_t time = 10 + cycle * 10;
        app.right_press_serial = 0;
        app.popup_configures = 0;
        app.popup_enters = 0;
        app.popup_moves = 0;
        app.popup_done = 0;
        app.menu_width = 0;
        app.menu_height = 0;
        unsigned previous_selected = app.selected;

        move_pointer(&app, parent_origin_x + 45,
            parent_origin_y + 45, time);
        if (app.focused_surface != app.parent_surface) {
            fail_cycle(cycle, "pointer did not return to parent");
        }
        click_button(&app, BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED,
            time + 1);
        if (!app.right_press_serial) {
            fail_cycle(cycle, "right click did not reach parent");
        }

        app.popup_surface = wl_compositor_create_surface(app.compositor);
        app.popup_xdg = xdg_wm_base_get_xdg_surface(app.wm_base,
            app.popup_surface);
        xdg_surface_add_listener(app.popup_xdg, &surface_listener, &app);
        struct xdg_positioner *positioner =
            xdg_wm_base_create_positioner(app.wm_base);
        xdg_positioner_set_size(positioner, MENU_WIDTH, MENU_HEIGHT);
        xdg_positioner_set_anchor_rect(positioner, 40, 40, 1, 1);
        xdg_positioner_set_anchor(positioner,
            XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
        xdg_positioner_set_gravity(positioner,
            XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        xdg_positioner_set_constraint_adjustment(positioner,
            XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
            XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
        app.popup = xdg_surface_get_popup(app.popup_xdg, app.parent_xdg,
            positioner);
        xdg_popup_add_listener(app.popup, &popup_listener, &app);
        xdg_popup_grab(app.popup, app.seat, app.right_press_serial);
        xdg_positioner_destroy(positioner);
        wl_surface_commit(app.popup_surface);
        await_count(&app, &app.popup_configures, 1, "popup configure");
        if (app.menu_width != MENU_WIDTH || app.menu_height != MENU_HEIGHT) {
            fail_cycle(cycle, "unexpected popup geometry");
        }
        struct pixel_buffer menu_buffer = create_buffer(&app,
            MENU_WIDTH, MENU_HEIGHT);
        wl_surface_attach(app.popup_surface, menu_buffer.buffer, 0, 0);
        wl_surface_damage(app.popup_surface, 0, 0,
            MENU_WIDTH, MENU_HEIGHT);
        wl_surface_commit(app.popup_surface);
        if (wl_display_roundtrip(app.display) < 0) {
            fail_cycle(cycle, "Wayland disconnect after popup map");
        }
        click_button(&app, BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED,
            time + 2);

        int target_x = parent_origin_x + app.menu_x + MENU_WIDTH / 2;
        int target_y = parent_origin_y + app.menu_y + MENU_HEIGHT / 2;
        move_pointer(&app, target_x, target_y, time + 3);
        move_pointer(&app, target_x + 1, target_y + 1, time + 4);
        if (app.focused_surface != app.popup_surface ||
                app.popup_enters == 0 || app.popup_moves == 0 ||
                app.pointer_x < 0 || app.pointer_x >= MENU_WIDTH ||
                app.pointer_y < 0 || app.pointer_y >= MENU_HEIGHT) {
            fail_cycle(cycle, "popup did not receive pointer enter/motion");
        }
        if (app.popup_done) {
            fail_cycle(cycle, "popup dismissed before menu selection");
        }
        click_button(&app, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED,
            time + 5);
        click_button(&app, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED,
            time + 6);
        if (app.selected != previous_selected + 1) {
            fail_cycle(cycle, "menu click did not trigger selection");
        }
        xdg_popup_destroy(app.popup);
        xdg_surface_destroy(app.popup_xdg);
        wl_surface_destroy(app.popup_surface);
        destroy_buffer(&menu_buffer);
        app.popup = NULL;
        app.popup_xdg = NULL;
        app.popup_surface = NULL;
        app.focused_surface = NULL;
        if (wl_display_roundtrip(app.display) < 0) {
            fail_cycle(cycle, "Wayland disconnect after popup destroy");
        }
    }

    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(app.parent_xdg);
    wl_surface_destroy(app.parent_surface);
    destroy_buffer(&parent_buffer);
    zwlr_virtual_pointer_v1_destroy(app.virtual_pointer);
    zwlr_virtual_pointer_manager_v1_destroy(app.virtual_manager);
    wl_pointer_release(app.pointer);
    wl_seat_release(app.seat);
    wl_output_release(app.output);
    xdg_wm_base_destroy(app.wm_base);
    wl_shm_destroy(app.shm);
    wl_compositor_destroy(app.compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(app.display);
    printf("%u context-menu cycles: right click, popup pointer motion, "
        "left click selection, destroy\n", cycles);
    return 0;
}
