#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <wayland-client.h>
#include "wlr-data-control-unstable-v1-client-protocol.h"

static volatile sig_atomic_t g_running = 1;

static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
}

#define MAX_MIMES 128

struct mime_list {
    char *types[MAX_MIMES];
    size_t count;
};

static void mime_list_clear(struct mime_list *l) {
    for (size_t i = 0; i < l->count; i++) {
        free(l->types[i]);
        l->types[i] = NULL;
    }
    l->count = 0;
}

static void mime_list_add(struct mime_list *l, const char *type) {
    if (l->count >= MAX_MIMES) return;
    for (size_t i = 0; i < l->count; i++) {
        if (strcmp(l->types[i], type) == 0) return;
    }
    l->types[l->count++] = strdup(type);
}

struct bridge_endpoint;

struct offer_info {
    struct zwlr_data_control_offer_v1 *offer;
    struct mime_list mimes;
    struct bridge_endpoint *endpoint;
    bool is_primary;
};

struct bridge_endpoint {
    const char *name;
    const char *display_name;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_seat *seat;
    struct zwlr_data_control_manager_v1 *manager;
    struct zwlr_data_control_device_v1 *device;

    // Regular selection
    struct zwlr_data_control_source_v1 *our_source;
    struct offer_info *current_offer;
    int pending_our_selections;
    bool is_our_offer;

    // Primary selection
    struct zwlr_data_control_source_v1 *our_primary_source;
    struct offer_info *current_primary_offer;
    int pending_our_primary_selections;
    bool is_our_primary_offer;

    struct bridge_endpoint *peer;
    bool ready;
    bool debug;
};

static void offer_handle_offer(void *data, struct zwlr_data_control_offer_v1 *offer, const char *mime_type) {
    (void)offer;
    struct offer_info *info = data;
    if (info) {
        mime_list_add(&info->mimes, mime_type);
    }
}

static const struct zwlr_data_control_offer_v1_listener offer_listener = {
    .offer = offer_handle_offer,
};

static void source_handle_send(void *data, struct zwlr_data_control_source_v1 *source,
                               const char *mime_type, int32_t fd) {
    struct bridge_endpoint *ep = data;
    bool is_primary = (source == ep->our_primary_source);
    struct offer_info *peer_offer = is_primary ? ep->peer->current_primary_offer : ep->peer->current_offer;

    if (ep->debug) {
        fprintf(stderr, "[%s] send request for MIME '%s' (fd=%d, primary=%d, peer_offer=%p)\n",
                ep->name, mime_type, fd, is_primary, (void*)peer_offer);
    }

    if (peer_offer && peer_offer->offer) {
        zwlr_data_control_offer_v1_receive(peer_offer->offer, mime_type, fd);
        while (wl_display_flush(ep->peer->display) == -1 && errno == EAGAIN) {
            struct pollfd pfd = { .fd = wl_display_get_fd(ep->peer->display), .events = POLLOUT };
            poll(&pfd, 1, 100);
        }
    }
    close(fd);
}

static void source_handle_cancelled(void *data, struct zwlr_data_control_source_v1 *source) {
    struct bridge_endpoint *ep = data;
    if (ep->debug) {
        fprintf(stderr, "[%s] source cancelled: %p\n", ep->name, (void*)source);
    }
    if (source == ep->our_source) {
        zwlr_data_control_source_v1_destroy(ep->our_source);
        ep->our_source = NULL;
        ep->is_our_offer = false;
    } else if (source == ep->our_primary_source) {
        zwlr_data_control_source_v1_destroy(ep->our_primary_source);
        ep->our_primary_source = NULL;
        ep->is_our_primary_offer = false;
    }
}

static const struct zwlr_data_control_source_v1_listener source_listener = {
    .send = source_handle_send,
    .cancelled = source_handle_cancelled,
};

static void propagate_selection(struct bridge_endpoint *src_ep, bool is_primary) {
    struct bridge_endpoint *dst_ep = src_ep->peer;
    if (!dst_ep || !dst_ep->ready || !dst_ep->manager || !dst_ep->device) return;

    struct offer_info *src_offer = is_primary ? src_ep->current_primary_offer : src_ep->current_offer;

    if (src_offer == NULL || src_offer->mimes.count == 0) {
        return; // Do not clear destination clipboard on empty transient source
    }

    if (src_ep->debug) {
        fprintf(stderr, "[%s -> %s] Propagating %s selection (%zu mime types):\n",
                src_ep->name, dst_ep->name, is_primary ? "PRIMARY" : "REGULAR",
                src_offer->mimes.count);
        for (size_t i = 0; i < src_offer->mimes.count; i++) {
            fprintf(stderr, "   - %s\n", src_offer->mimes.types[i]);
        }
    }

    if (is_primary) {
        if (dst_ep->our_primary_source) {
            zwlr_data_control_source_v1_destroy(dst_ep->our_primary_source);
            dst_ep->our_primary_source = NULL;
        }

        struct zwlr_data_control_source_v1 *source =
            zwlr_data_control_manager_v1_create_data_source(dst_ep->manager);
        dst_ep->our_primary_source = source;
        zwlr_data_control_source_v1_add_listener(source, &source_listener, dst_ep);

        for (size_t i = 0; i < src_offer->mimes.count; i++) {
            zwlr_data_control_source_v1_offer(source, src_offer->mimes.types[i]);
        }

        dst_ep->pending_our_primary_selections++;
        zwlr_data_control_device_v1_set_primary_selection(dst_ep->device, source);
        wl_display_flush(dst_ep->display);
    } else {
        if (dst_ep->our_source) {
            zwlr_data_control_source_v1_destroy(dst_ep->our_source);
            dst_ep->our_source = NULL;
        }

        struct zwlr_data_control_source_v1 *source =
            zwlr_data_control_manager_v1_create_data_source(dst_ep->manager);
        dst_ep->our_source = source;
        zwlr_data_control_source_v1_add_listener(source, &source_listener, dst_ep);

        for (size_t i = 0; i < src_offer->mimes.count; i++) {
            zwlr_data_control_source_v1_offer(source, src_offer->mimes.types[i]);
        }

        dst_ep->pending_our_selections++;
        zwlr_data_control_device_v1_set_selection(dst_ep->device, source);
        wl_display_flush(dst_ep->display);
    }
}

static void device_handle_data_offer(void *data, struct zwlr_data_control_device_v1 *device,
                                     struct zwlr_data_control_offer_v1 *offer) {
    (void)device;
    struct bridge_endpoint *ep = data;
    struct offer_info *info = calloc(1, sizeof(*info));
    info->offer = offer;
    info->endpoint = ep;
    zwlr_data_control_offer_v1_set_user_data(offer, info);
    zwlr_data_control_offer_v1_add_listener(offer, &offer_listener, info);
}

static void device_handle_selection(void *data, struct zwlr_data_control_device_v1 *device,
                                    struct zwlr_data_control_offer_v1 *offer) {
    (void)device;
    struct bridge_endpoint *ep = data;
    if (ep->debug) {
        fprintf(stderr, "[%s] selection event: offer=%p, pending_our=%d\n",
                ep->name, (void*)offer, ep->pending_our_selections);
    }

    if (offer == NULL) {
        return;
    }

    struct offer_info *info = zwlr_data_control_offer_v1_get_user_data(offer);
    if (!info) return;

    if (ep->pending_our_selections > 0) {
        ep->pending_our_selections--;
        ep->is_our_offer = true;
        if (ep->debug) {
            fprintf(stderr, "[%s] Ignored echo of our own selection\n", ep->name);
        }
        return;
    }

    if (ep->current_offer && ep->current_offer != info) {
        zwlr_data_control_offer_v1_destroy(ep->current_offer->offer);
        mime_list_clear(&ep->current_offer->mimes);
        free(ep->current_offer);
    }
    ep->current_offer = info;
    ep->is_our_offer = false;

    propagate_selection(ep, false);
}

static void device_handle_primary_selection(void *data, struct zwlr_data_control_device_v1 *device,
                                            struct zwlr_data_control_offer_v1 *offer) {
    (void)device;
    struct bridge_endpoint *ep = data;
    if (ep->debug) {
        fprintf(stderr, "[%s] primary selection event: offer=%p, pending_our_primary=%d\n",
                ep->name, (void*)offer, ep->pending_our_primary_selections);
    }

    if (offer == NULL) {
        return;
    }

    struct offer_info *info = zwlr_data_control_offer_v1_get_user_data(offer);
    if (!info) return;

    if (ep->pending_our_primary_selections > 0) {
        ep->pending_our_primary_selections--;
        ep->is_our_primary_offer = true;
        if (ep->debug) {
            fprintf(stderr, "[%s] Ignored echo of our own primary selection\n", ep->name);
        }
        return;
    }

    if (ep->current_primary_offer && ep->current_primary_offer != info) {
        zwlr_data_control_offer_v1_destroy(ep->current_primary_offer->offer);
        mime_list_clear(&ep->current_primary_offer->mimes);
        free(ep->current_primary_offer);
    }
    ep->current_primary_offer = info;
    ep->is_our_primary_offer = false;

    propagate_selection(ep, true);
}

static void device_handle_finished(void *data, struct zwlr_data_control_device_v1 *device) {
    (void)data;
    (void)device;
}

static const struct zwlr_data_control_device_v1_listener device_listener = {
    .data_offer = device_handle_data_offer,
    .selection = device_handle_selection,
    .finished = device_handle_finished,
    .primary_selection = device_handle_primary_selection,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface, uint32_t version) {
    struct bridge_endpoint *ep = data;
    if (strcmp(interface, wl_seat_interface.name) == 0) {
        if (!ep->seat) {
            ep->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        }
    } else if (strcmp(interface, zwlr_data_control_manager_v1_interface.name) == 0) {
        uint32_t target_version = version < 2 ? version : 2;
        ep->manager = wl_registry_bind(registry, name, &zwlr_data_control_manager_v1_interface, target_version);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

static int connect_endpoint(struct bridge_endpoint *ep, const char *name, const char *display_name, bool debug) {
    ep->name = name;
    ep->display_name = display_name;
    ep->debug = debug;

    ep->display = wl_display_connect(display_name);
    if (!ep->display) {
        fprintf(stderr, "[%s] Failed to connect to Wayland display '%s'\n", name, display_name ? display_name : "(default)");
        return -1;
    }

    ep->registry = wl_display_get_registry(ep->display);
    wl_registry_add_listener(ep->registry, &registry_listener, ep);
    wl_display_roundtrip(ep->display);

    if (!ep->seat) {
        fprintf(stderr, "[%s] No wl_seat found on '%s'\n", name, display_name);
        return -1;
    }
    if (!ep->manager) {
        fprintf(stderr, "[%s] zwlr_data_control_manager_v1 not supported on '%s'\n", name, display_name);
        return -1;
    }

    return 0;
}

static int setup_endpoint_device(struct bridge_endpoint *ep) {
    ep->device = zwlr_data_control_manager_v1_get_data_device(ep->manager, ep->seat);
    zwlr_data_control_device_v1_add_listener(ep->device, &device_listener, ep);
    wl_display_roundtrip(ep->display);
    ep->ready = true;
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    const char *host_disp = getenv("SC7_HOST_WAYLAND_DISPLAY");
    if (!host_disp || !*host_disp) {
        host_disp = "wayland-1";
    }
    const char *nested_disp = getenv("WAYLAND_DISPLAY");
    if (!nested_disp || !*nested_disp) {
        nested_disp = "wayland-2";
    }
    bool debug = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host_disp = argv[++i];
        } else if (strcmp(argv[i], "--nested") == 0 && i + 1 < argc) {
            nested_disp = argv[++i];
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: sc7-clipboard-bridge [--host <display>] [--nested <display>] [--debug]\n");
            return 0;
        }
    }

    struct bridge_endpoint host, nested;
    memset(&host, 0, sizeof(host));
    memset(&nested, 0, sizeof(nested));

    // Connect both displays first
    if (connect_endpoint(&host, "HOST", host_disp, debug) < 0) return 1;
    if (connect_endpoint(&nested, "NESTED", nested_disp, debug) < 0) {
        wl_display_disconnect(host.display);
        return 1;
    }

    // Cross-link peers before data devices start receiving events
    host.peer = &nested;
    nested.peer = &host;

    // Set up devices
    if (setup_endpoint_device(&host) < 0 || setup_endpoint_device(&nested) < 0) {
        return 1;
    }

    // Synchronize initial selection from host if available
    if (host.current_offer && !host.is_our_offer) {
        propagate_selection(&host, false);
    } else if (nested.current_offer && !nested.is_our_offer) {
        propagate_selection(&nested, false);
    }

    if (debug) {
        printf("SC7 Rack Clipboard Bridge connected (Host: %s, Nested: %s)\n", host_disp, nested_disp);
        fflush(stdout);
    }

    struct pollfd fds[2];
    fds[0].fd = wl_display_get_fd(host.display);
    fds[0].events = POLLIN;
    fds[1].fd = wl_display_get_fd(nested.display);
    fds[1].events = POLLIN;

    while (g_running) {
        while (wl_display_prepare_read(host.display) != 0) {
            wl_display_dispatch_pending(host.display);
        }
        while (wl_display_prepare_read(nested.display) != 0) {
            wl_display_dispatch_pending(nested.display);
        }

        wl_display_flush(host.display);
        wl_display_flush(nested.display);

        int ret = poll(fds, 2, 250);
        if (ret < 0 && errno != EINTR) {
            wl_display_cancel_read(host.display);
            wl_display_cancel_read(nested.display);
            break;
        }

        if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
            if (wl_display_read_events(host.display) < 0) {
                if (debug) fprintf(stderr, "Host read error or disconnect: %d\n", wl_display_get_error(host.display));
                break;
            }
        } else {
            wl_display_cancel_read(host.display);
        }

        if (fds[1].revents & (POLLIN | POLLERR | POLLHUP)) {
            if (wl_display_read_events(nested.display) < 0) {
                if (debug) fprintf(stderr, "Nested read error or disconnect: %d\n", wl_display_get_error(nested.display));
                break;
            }
        } else {
            wl_display_cancel_read(nested.display);
        }

        wl_display_dispatch_pending(host.display);
        wl_display_dispatch_pending(nested.display);
    }

    if (debug) {
        printf("SC7 Rack Clipboard Bridge shutting down...\n");
    }

    wl_display_disconnect(host.display);
    wl_display_disconnect(nested.display);
    return 0;
}
