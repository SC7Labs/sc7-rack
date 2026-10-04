#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <pthread.h>
#include <wayland-client.h>
#include "wlr-data-control-unstable-v1-client-protocol.h"

static volatile sig_atomic_t g_running = 1;

static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
}

#define MAX_MIMES 128
#define OWNER_MIME_PREFIX "application/x-sc7-rack-clipboard-bridge-"
#define OWNER_MIME_RANDOM_BYTES 16
#define OWNER_MIME_CAPACITY (sizeof(OWNER_MIME_PREFIX) + OWNER_MIME_RANDOM_BYTES * 2)

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
    char *copy = strdup(type);
    if (!copy) return;
    l->types[l->count++] = copy;
}

static bool mime_list_has(const struct mime_list *l, const char *type) {
    for (size_t i = 0; i < l->count; i++) {
        if (strcmp(l->types[i], type) == 0) return true;
    }
    return false;
}

static const char *find_matching_text_mime(const struct mime_list *l) {
    if (mime_list_has(l, "text/plain;charset=utf-8")) return "text/plain;charset=utf-8";
    if (mime_list_has(l, "UTF8_STRING")) return "UTF8_STRING";
    if (mime_list_has(l, "text/plain")) return "text/plain";
    if (mime_list_has(l, "STRING")) return "STRING";
    if (mime_list_has(l, "TEXT")) return "TEXT";
    return NULL;
}

static bool is_text_mime(const char *type) {
    if (!type) return false;
    return strcmp(type, "text/plain;charset=utf-8") == 0 ||
           strcmp(type, "UTF8_STRING") == 0 ||
           strcmp(type, "text/plain") == 0 ||
           strcmp(type, "STRING") == 0 ||
           strcmp(type, "TEXT") == 0;
}

struct bridge_endpoint;

struct pending_owner_marker {
    char mime[OWNER_MIME_CAPACITY];
    struct zwlr_data_control_source_v1 *source;
    struct bridge_endpoint *endpoint;
    struct wl_callback *cleanup_sync;
    struct pending_owner_marker *next;
    bool is_primary;
    bool linked;
};

struct offer_info {
    struct zwlr_data_control_offer_v1 *offer;
    struct mime_list mimes;
    struct bridge_endpoint *endpoint;
    bool is_primary;
    char own_regular_marker[OWNER_MIME_CAPACITY];
    char own_primary_marker[OWNER_MIME_CAPACITY];
};

struct bridge_endpoint {
    const char *name;
    const char *display_name;
    bool is_host;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_seat *seat;
    struct zwlr_data_control_manager_v1 *manager;
    struct zwlr_data_control_device_v1 *device;

    // Regular selection
    struct zwlr_data_control_source_v1 *our_source;
    struct offer_info *current_offer;
    bool is_our_offer;

    // Primary selection
    struct zwlr_data_control_source_v1 *our_primary_source;
    struct offer_info *current_primary_offer;
    bool is_our_primary_offer;

    struct pending_owner_marker *pending_markers;

    struct bridge_endpoint *peer;
    bool ready;
    bool debug;
};

static bool is_owner_marker(const char *type) {
    return strncmp(type, OWNER_MIME_PREFIX, sizeof(OWNER_MIME_PREFIX) - 1) == 0;
}

static bool endpoint_is_host(const struct bridge_endpoint *ep) {
    if (!ep) return false;
    if (ep->is_host) return true;
    if (ep->name && strcasecmp(ep->name, "HOST") == 0) return true;
    return false;
}

static int generate_owner_mime(char *out, size_t capacity) {
    unsigned char random_bytes[OWNER_MIME_RANDOM_BYTES];
    size_t got = 0;
    while (got < sizeof(random_bytes)) {
        ssize_t n = getrandom(random_bytes + got, sizeof(random_bytes) - got, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        got += (size_t)n;
    }

    int pos = snprintf(out, capacity, "%s", OWNER_MIME_PREFIX);
    if (pos < 0 || (size_t)pos + sizeof(random_bytes) * 2 >= capacity) return -1;
    for (size_t i = 0; i < sizeof(random_bytes); i++) {
        snprintf(out + pos + i * 2, capacity - (size_t)pos - i * 2,
                 "%02x", random_bytes[i]);
    }
    return 0;
}

static struct pending_owner_marker *find_pending_marker(
        struct bridge_endpoint *ep, const char *mime, bool is_primary) {
    for (struct pending_owner_marker *marker = ep->pending_markers;
         marker; marker = marker->next) {
        if (marker->is_primary == is_primary && strcmp(marker->mime, mime) == 0) {
            return marker;
        }
    }
    return NULL;
}

static struct pending_owner_marker *find_pending_source(
        struct bridge_endpoint *ep, struct zwlr_data_control_source_v1 *source) {
    for (struct pending_owner_marker *marker = ep->pending_markers;
         marker; marker = marker->next) {
        if (marker->source == source) return marker;
    }
    return NULL;
}

static void unlink_pending_marker(struct pending_owner_marker *marker) {
    if (!marker->linked) return;
    struct pending_owner_marker **cursor = &marker->endpoint->pending_markers;
    while (*cursor && *cursor != marker) cursor = &(*cursor)->next;
    if (*cursor) *cursor = marker->next;
    marker->linked = false;
    marker->next = NULL;
}

static void marker_sync_done(void *data, struct wl_callback *callback, uint32_t serial) {
    (void)serial;
    struct pending_owner_marker *marker = data;
    unlink_pending_marker(marker);
    wl_callback_destroy(callback);
    free(marker);
}

static const struct wl_callback_listener marker_sync_listener = {
    .done = marker_sync_done,
};

static void retire_pending_source(struct bridge_endpoint *ep,
                                  struct zwlr_data_control_source_v1 *source) {
    struct pending_owner_marker *marker = find_pending_source(ep, source);
    if (!marker || marker->cleanup_sync) return;

    // A queued echo can arrive after we destroy its source. Keep the marker
    // until the server processes all requests preceding this sync callback.
    struct wl_callback *sync = wl_display_sync(ep->display);
    if (!sync || wl_callback_add_listener(sync, &marker_sync_listener, marker) != 0) {
        if (sync) wl_callback_destroy(sync);
        unlink_pending_marker(marker);
        free(marker);
        return;
    }
    marker->cleanup_sync = sync;
}

static struct pending_owner_marker *track_pending_source(
        struct bridge_endpoint *ep, struct zwlr_data_control_source_v1 *source,
        bool is_primary) {
    struct pending_owner_marker *marker = calloc(1, sizeof(*marker));
    if (!marker) return NULL;
    if (generate_owner_mime(marker->mime, sizeof(marker->mime)) != 0) {
        free(marker);
        return NULL;
    }
    marker->source = source;
    marker->endpoint = ep;
    marker->is_primary = is_primary;
    marker->linked = true;
    marker->next = ep->pending_markers;
    ep->pending_markers = marker;
    return marker;
}

static void consume_pending_marker(struct pending_owner_marker *marker) {
    unlink_pending_marker(marker);
    if (!marker->cleanup_sync) free(marker);
}

static void destroy_owned_source(struct bridge_endpoint *ep,
                                 struct zwlr_data_control_source_v1 *source) {
    zwlr_data_control_source_v1_destroy(source);
    retire_pending_source(ep, source);
}

static void offer_info_destroy(struct offer_info *info) {
    if (!info) return;
    zwlr_data_control_offer_v1_destroy(info->offer);
    mime_list_clear(&info->mimes);
    free(info);
}

static void offer_handle_offer(void *data, struct zwlr_data_control_offer_v1 *offer, const char *mime_type) {
    (void)offer;
    struct offer_info *info = data;
    if (info) {
        // Preserve ownership identity even when MAX_MIMES ordinary MIME slots
        // are full. The bridge marker is deliberately advertised last.
        if (is_owner_marker(mime_type)) {
            struct pending_owner_marker *regular =
                find_pending_marker(info->endpoint, mime_type, false);
            struct pending_owner_marker *primary =
                find_pending_marker(info->endpoint, mime_type, true);
            if (regular) strcpy(info->own_regular_marker, regular->mime);
            if (primary) strcpy(info->own_primary_marker, primary->mime);
        }
        mime_list_add(&info->mimes, mime_type);
    }
}

static const struct zwlr_data_control_offer_v1_listener offer_listener = {
    .offer = offer_handle_offer,
};

struct uri_to_text_ctx {
    int read_fd;
    int write_fd;
    bool is_uri_to_text;
};

static void *convert_worker(void *arg) {
    struct uri_to_text_ctx *ctx = arg;
    char buf[16384];
    ssize_t n = 0;
    size_t total = 0;
    while ((n = read(ctx->read_fd, buf + total, sizeof(buf) - 1 - total)) > 0) {
        total += n;
        if (total >= sizeof(buf) - 1) break;
    }
    close(ctx->read_fd);
    buf[total] = '\0';

    if (ctx->is_uri_to_text) {
        // Strip file:// prefix and decode %20 URL encoding
        char *line = strtok(buf, "\r\n");
        bool first = true;
        while (line) {
            while (*line == ' ') line++;
            if (strncmp(line, "file://", 7) == 0) {
                line += 7;
            }
            if (*line) {
                char decoded[4096];
                size_t d = 0;
                for (size_t s = 0; line[s] && d < sizeof(decoded) - 2; s++) {
                    if (line[s] == '%' && line[s+1] && line[s+2]) {
                        char hex[3] = { line[s+1], line[s+2], '\0' };
                        decoded[d++] = (char)strtol(hex, NULL, 16);
                        s += 2;
                    } else {
                        decoded[d++] = line[s];
                    }
                }
                decoded[d] = '\0';
                if (!first) {
                    ssize_t w = write(ctx->write_fd, "\n", 1);
                    (void)w;
                }
                ssize_t w = write(ctx->write_fd, decoded, strlen(decoded));
                (void)w;
                first = false;
            }
            line = strtok(NULL, "\r\n");
        }
    } else {
        // Plain text -> text/uri-list (file://<path>\r\n)
        char *line = strtok(buf, "\r\n");
        while (line) {
            while (*line == ' ') line++;
            if (*line == '/') {
                char uri[4096];
                snprintf(uri, sizeof(uri), "file://%s\r\n", line);
                ssize_t w = write(ctx->write_fd, uri, strlen(uri));
                (void)w;
            } else if (strncmp(line, "file://", 7) == 0) {
                char uri[4096];
                snprintf(uri, sizeof(uri), "%s\r\n", line);
                ssize_t w = write(ctx->write_fd, uri, strlen(uri));
                (void)w;
            }
            line = strtok(NULL, "\r\n");
        }
    }
    close(ctx->write_fd);
    free(ctx);
    return NULL;
}

static bool start_convert_worker(int read_fd, int write_fd, bool is_uri_to_text) {
    struct uri_to_text_ctx *ctx = malloc(sizeof(*ctx));
    if (!ctx) return false;
    ctx->read_fd = read_fd;
    ctx->write_fd = write_fd;
    ctx->is_uri_to_text = is_uri_to_text;

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        free(ctx);
        return false;
    }
    int err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (err == 0) {
        pthread_t th;
        err = pthread_create(&th, &attr, convert_worker, ctx);
    }
    pthread_attr_destroy(&attr);
    if (err != 0) {
        free(ctx);
        return false;
    }
    return true;
}

static void source_handle_send(void *data, struct zwlr_data_control_source_v1 *source,
                               const char *mime_type, int32_t fd) {
    struct bridge_endpoint *ep = data;
    bool is_primary = (source == ep->our_primary_source);
    struct offer_info *peer_offer = is_primary ? ep->peer->current_primary_offer : ep->peer->current_offer;

    if (ep->debug) {
        fprintf(stderr, "[%s] send request for MIME '%s' (fd=%d, primary=%d, peer_offer=%p)\n",
                ep->name, mime_type, fd, is_primary, (void*)peer_offer);
    }

    // The marker is only an ownership signal. Never transfer clipboard data
    // under that private MIME type, including through the text fallback.
    if (is_owner_marker(mime_type)) {
        close(fd);
        return;
    }

    if (!peer_offer || !peer_offer->offer) {
        close(fd);
        return;
    }

    // Direct match
    if (mime_list_has(&peer_offer->mimes, mime_type)) {
        zwlr_data_control_offer_v1_receive(peer_offer->offer, mime_type, fd);
        while (wl_display_flush(ep->peer->display) == -1 && errno == EAGAIN) {
            struct pollfd pfd = { .fd = wl_display_get_fd(ep->peer->display), .events = POLLOUT };
            poll(&pfd, 1, 100);
        }
        close(fd);
        return;
    }

    // Target wants text/plain, but peer only has text/uri-list
    if (is_text_mime(mime_type) &&
        mime_list_has(&peer_offer->mimes, "text/uri-list")) {
        int p[2];
        if (pipe(p) == 0) {
            if (!start_convert_worker(p[0], fd, true)) {
                close(p[0]);
                close(p[1]);
                close(fd);
                return;
            }

            zwlr_data_control_offer_v1_receive(peer_offer->offer, "text/uri-list", p[1]);
            while (wl_display_flush(ep->peer->display) == -1 && errno == EAGAIN) {
                struct pollfd pfd = { .fd = wl_display_get_fd(ep->peer->display), .events = POLLOUT };
                poll(&pfd, 1, 100);
            }
            close(p[1]);
            return;
        }
    }

    // Target wants text/uri-list, but peer only has plain text.
    // Allowed only Host -> Rack (ep is nested, not host).
    // Rack -> host: never synthesize text/uri-list from plain text.
    if (!endpoint_is_host(ep) && strcmp(mime_type, "text/uri-list") == 0) {
        const char *alt = find_matching_text_mime(&peer_offer->mimes);
        if (alt) {
            int p[2];
            if (pipe(p) == 0) {
                if (!start_convert_worker(p[0], fd, false)) {
                    close(p[0]);
                    close(p[1]);
                    close(fd);
                    return;
                }

                zwlr_data_control_offer_v1_receive(peer_offer->offer, alt, p[1]);
                while (wl_display_flush(ep->peer->display) == -1 && errno == EAGAIN) {
                    struct pollfd pfd = { .fd = wl_display_get_fd(ep->peer->display), .events = POLLOUT };
                    poll(&pfd, 1, 100);
                }
                close(p[1]);
                return;
            }
        }
    }

    // Target wants some text form (e.g. text/plain), but peer has another text form (e.g. UTF8_STRING)
    if (is_text_mime(mime_type)) {
        const char *alt_text = find_matching_text_mime(&peer_offer->mimes);
        if (alt_text) {
            zwlr_data_control_offer_v1_receive(peer_offer->offer, alt_text, fd);
            while (wl_display_flush(ep->peer->display) == -1 && errno == EAGAIN) {
                struct pollfd pfd = { .fd = wl_display_get_fd(ep->peer->display), .events = POLLOUT };
                poll(&pfd, 1, 100);
            }
            close(fd);
            return;
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
        ep->our_source = NULL;
        ep->is_our_offer = false;
    } else if (source == ep->our_primary_source) {
        ep->our_primary_source = NULL;
        ep->is_our_primary_offer = false;
    }
    destroy_owned_source(ep, source);
}

static const struct zwlr_data_control_source_v1_listener source_listener = {
    .send = source_handle_send,
    .cancelled = source_handle_cancelled,
};

static void offer_all_synthesized_mimes(struct bridge_endpoint *dst_ep,
                                        struct zwlr_data_control_source_v1 *source,
                                        struct offer_info *src_offer) {
    for (size_t i = 0; i < src_offer->mimes.count; i++) {
        // Markers identify bridge-owned selections. Never forward one from
        // an observed offer to the other display.
        if (!is_owner_marker(src_offer->mimes.types[i])) {
            zwlr_data_control_source_v1_offer(source, src_offer->mimes.types[i]);
        }
    }

    if (mime_list_has(&src_offer->mimes, "text/uri-list")) {
        if (!mime_list_has(&src_offer->mimes, "text/plain;charset=utf-8")) {
            zwlr_data_control_source_v1_offer(source, "text/plain;charset=utf-8");
        }
        if (!mime_list_has(&src_offer->mimes, "UTF8_STRING")) {
            zwlr_data_control_source_v1_offer(source, "UTF8_STRING");
        }
        if (!mime_list_has(&src_offer->mimes, "text/plain")) {
            zwlr_data_control_source_v1_offer(source, "text/plain");
        }
    }

    const char *text_mime = find_matching_text_mime(&src_offer->mimes);
    if (text_mime) {
        if (!mime_list_has(&src_offer->mimes, "text/plain;charset=utf-8")) {
            zwlr_data_control_source_v1_offer(source, "text/plain;charset=utf-8");
        }
        if (!mime_list_has(&src_offer->mimes, "UTF8_STRING")) {
            zwlr_data_control_source_v1_offer(source, "UTF8_STRING");
        }
        if (!mime_list_has(&src_offer->mimes, "text/plain")) {
            zwlr_data_control_source_v1_offer(source, "text/plain");
        }
        // Rack -> host: never synthesize text/uri-list from plain text.
        // Host -> Rack: retain text -> URI synthesis for file-transfer parity.
        if (!endpoint_is_host(dst_ep)) {
            if (!mime_list_has(&src_offer->mimes, "text/uri-list")) {
                zwlr_data_control_source_v1_offer(source, "text/uri-list");
            }
        }
    }
}

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
            destroy_owned_source(dst_ep, dst_ep->our_primary_source);
            dst_ep->our_primary_source = NULL;
        }

        struct zwlr_data_control_source_v1 *source =
            zwlr_data_control_manager_v1_create_data_source(dst_ep->manager);
        if (!source) return;
        struct pending_owner_marker *marker =
            track_pending_source(dst_ep, source, true);
        if (!marker) {
            zwlr_data_control_source_v1_destroy(source);
            return;
        }
        dst_ep->our_primary_source = source;
        zwlr_data_control_source_v1_add_listener(source, &source_listener, dst_ep);

        offer_all_synthesized_mimes(dst_ep, source, src_offer);
        zwlr_data_control_source_v1_offer(source, marker->mime);

        zwlr_data_control_device_v1_set_primary_selection(dst_ep->device, source);
        wl_display_flush(dst_ep->display);
    } else {
        if (dst_ep->our_source) {
            destroy_owned_source(dst_ep, dst_ep->our_source);
            dst_ep->our_source = NULL;
        }

        struct zwlr_data_control_source_v1 *source =
            zwlr_data_control_manager_v1_create_data_source(dst_ep->manager);
        if (!source) return;
        struct pending_owner_marker *marker =
            track_pending_source(dst_ep, source, false);
        if (!marker) {
            zwlr_data_control_source_v1_destroy(source);
            return;
        }
        dst_ep->our_source = source;
        zwlr_data_control_source_v1_add_listener(source, &source_listener, dst_ep);

        offer_all_synthesized_mimes(dst_ep, source, src_offer);
        zwlr_data_control_source_v1_offer(source, marker->mime);

        zwlr_data_control_device_v1_set_selection(dst_ep->device, source);
        wl_display_flush(dst_ep->display);
    }
}

static void revoke_mirrored_selection(struct bridge_endpoint *src_ep, bool is_primary) {
    struct bridge_endpoint *dst_ep = src_ep->peer;
    if (!dst_ep) return;

    // Destroy only the source we created. If it is still the compositor's
    // selection, the compositor clears it; if another client replaced it,
    // that client's selection is left untouched. Do not send set_selection
    // with NULL, which could erase a newer selection on the other display.
    struct zwlr_data_control_source_v1 **source = is_primary ?
        &dst_ep->our_primary_source : &dst_ep->our_source;
    if (*source) {
        destroy_owned_source(dst_ep, *source);
        *source = NULL;
        if (is_primary) {
            dst_ep->is_our_primary_offer = false;
        } else {
            dst_ep->is_our_offer = false;
        }
        wl_display_flush(dst_ep->display);
    }
}

static void device_handle_data_offer(void *data, struct zwlr_data_control_device_v1 *device,
                                     struct zwlr_data_control_offer_v1 *offer) {
    (void)device;
    struct bridge_endpoint *ep = data;
    struct offer_info *info = calloc(1, sizeof(*info));
    if (!info) {
        // Keep the proxy dispatchable. The selection callback will destroy
        // this untracked offer without trying to use or forward it.
        zwlr_data_control_offer_v1_add_listener(offer, &offer_listener, NULL);
        return;
    }
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
        fprintf(stderr, "[%s] selection event: offer=%p\n", ep->name, (void*)offer);
    }

    struct offer_info *info = offer ? zwlr_data_control_offer_v1_get_user_data(offer) : NULL;
    if (offer && !info &&
        (!ep->current_offer || ep->current_offer->offer != offer)) {
        zwlr_data_control_offer_v1_destroy(offer);
    }

    // Each selection event invalidates the previous offer, including an echo
    // of our own source and a NULL selection. The protocol requires clients
    // to destroy that offer before using the new one.
    if (ep->current_offer && ep->current_offer != info) {
        offer_info_destroy(ep->current_offer);
    }
    ep->current_offer = NULL;

    if (!info) {
        ep->is_our_offer = false;
        revoke_mirrored_selection(ep, false);
        return;
    }

    struct pending_owner_marker *marker = info->own_regular_marker[0] ?
        find_pending_marker(ep, info->own_regular_marker, false) : NULL;
    if (marker) {
        consume_pending_marker(marker);
        ep->is_our_offer = true;
        offer_info_destroy(info);
        if (ep->debug) {
            fprintf(stderr, "[%s] Ignored echo of our own selection\n", ep->name);
        }
        return;
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
        fprintf(stderr, "[%s] primary selection event: offer=%p\n", ep->name, (void*)offer);
    }

    struct offer_info *info = offer ? zwlr_data_control_offer_v1_get_user_data(offer) : NULL;
    if (offer && !info &&
        (!ep->current_primary_offer || ep->current_primary_offer->offer != offer)) {
        zwlr_data_control_offer_v1_destroy(offer);
    }

    if (ep->current_primary_offer && ep->current_primary_offer != info) {
        offer_info_destroy(ep->current_primary_offer);
    }
    ep->current_primary_offer = NULL;

    if (!info) {
        ep->is_our_primary_offer = false;
        revoke_mirrored_selection(ep, true);
        return;
    }

    struct pending_owner_marker *marker = info->own_primary_marker[0] ?
        find_pending_marker(ep, info->own_primary_marker, true) : NULL;
    if (marker) {
        consume_pending_marker(marker);
        ep->is_our_primary_offer = true;
        offer_info_destroy(info);
        if (ep->debug) {
            fprintf(stderr, "[%s] Ignored echo of our own primary selection\n", ep->name);
        }
        return;
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

static int connect_endpoint(struct bridge_endpoint *ep, const char *name, const char *display_name, bool is_host, bool debug) {
    ep->name = name;
    ep->display_name = display_name;
    ep->is_host = is_host;
    ep->debug = debug;

    for (int attempt = 0; attempt < 30; attempt++) {
        ep->display = wl_display_connect(display_name);
        if (ep->display) {
            ep->registry = wl_display_get_registry(ep->display);
            wl_registry_add_listener(ep->registry, &registry_listener, ep);
            wl_display_roundtrip(ep->display);

            if (ep->seat && ep->manager) {
                return 0;
            }
            if (ep->manager) {
                zwlr_data_control_manager_v1_destroy(ep->manager);
                ep->manager = NULL;
            }
            if (ep->seat) {
                wl_seat_destroy(ep->seat);
                ep->seat = NULL;
            }
            if (ep->registry) {
                wl_registry_destroy(ep->registry);
                ep->registry = NULL;
            }
            wl_display_disconnect(ep->display);
            ep->display = NULL;
        }
        usleep(100000); // 100ms
    }

    fprintf(stderr, "[%s] Failed to connect to Wayland display '%s' (seat=%p, manager=%p)\n",
            name, display_name ? display_name : "(default)", (void*)ep->seat, (void*)ep->manager);
    return -1;
}

static int setup_endpoint_device(struct bridge_endpoint *ep) {
    ep->device = zwlr_data_control_manager_v1_get_data_device(ep->manager, ep->seat);
    zwlr_data_control_device_v1_add_listener(ep->device, &device_listener, ep);
    wl_display_roundtrip(ep->display);
    ep->ready = true;
    return 0;
}

static bool prepare_endpoint_read(struct wl_display *display) {
    while (wl_display_prepare_read(display) != 0) {
        // prepare_read also fails on a broken connection. Dispatching it
        // repeatedly without checking the error would spin forever.
        if (wl_display_dispatch_pending(display) < 0) return false;
    }
    return true;
}

static char *detect_wayland_display(const char *exclude) {
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    char path[1024];
    if (!runtime_dir || !*runtime_dir) {
        snprintf(path, sizeof(path), "/run/user/%d", (int)getuid());
        runtime_dir = path;
    }

    DIR *d = opendir(runtime_dir);
    if (!d) return NULL;

    struct dirent *ent;
    char found[256] = {0};
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "wayland-", 8) == 0 && !strstr(ent->d_name, ".lock")) {
            if (exclude && strcmp(ent->d_name, exclude) == 0) {
                continue;
            }
            char sock_path[2048];
            snprintf(sock_path, sizeof(sock_path), "%s/%s", runtime_dir, ent->d_name);
            struct stat st;
            if (stat(sock_path, &st) == 0 && S_ISSOCK(st.st_mode)) {
                snprintf(found, sizeof(found), "%s", ent->d_name);
                break;
            }
        }
    }
    closedir(d);
    if (found[0]) {
        return strdup(found);
    }
    return NULL;
}

int main(int argc, char **argv) {
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    const char *host_disp = getenv("SC7_HOST_WAYLAND_DISPLAY");
    const char *nested_disp = getenv("WAYLAND_DISPLAY");
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

    char *detected_host = NULL;
    char *detected_nested = NULL;

    if (!host_disp || !*host_disp) {
        detected_host = detect_wayland_display(nested_disp);
        host_disp = detected_host;
    }
    if (!nested_disp || !*nested_disp) {
        detected_nested = detect_wayland_display(host_disp);
        nested_disp = detected_nested;
    }

    if (!host_disp || !nested_disp) {
        fprintf(stderr, "sc7-clipboard-bridge: Unable to discover Wayland displays. Please specify --host and --nested.\n");
        free(detected_host);
        free(detected_nested);
        return 1;
    }

    struct bridge_endpoint host, nested;
    memset(&host, 0, sizeof(host));
    memset(&nested, 0, sizeof(nested));

    // Connect both displays first
    if (connect_endpoint(&host, "HOST", host_disp, true, debug) < 0) return 1;
    if (connect_endpoint(&nested, "NESTED", nested_disp, false, debug) < 0) {
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

    // Synchronize initial selection only if the receiving side has no active selection
    if (host.current_offer && host.current_offer->mimes.count > 0 && !host.is_our_offer &&
        (!nested.current_offer || nested.current_offer->mimes.count == 0)) {
        propagate_selection(&host, false);
    } else if (nested.current_offer && nested.current_offer->mimes.count > 0 && !nested.is_our_offer &&
               (!host.current_offer || host.current_offer->mimes.count == 0)) {
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
        if (!prepare_endpoint_read(host.display)) break;
        if (!prepare_endpoint_read(nested.display)) {
            wl_display_cancel_read(host.display);
            break;
        }

        if (wl_display_flush(host.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(host.display);
            wl_display_cancel_read(nested.display);
            break;
        }
        if (wl_display_flush(nested.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(host.display);
            wl_display_cancel_read(nested.display);
            break;
        }

        int ret = poll(fds, 2, 250);
        if (ret < 0) {
            int poll_errno = errno;
            wl_display_cancel_read(host.display);
            wl_display_cancel_read(nested.display);
            if (poll_errno == EINTR) continue;
            break;
        }

        if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
            if (wl_display_read_events(host.display) < 0) {
                if (debug) fprintf(stderr, "Host read error or disconnect: %d\n", wl_display_get_error(host.display));
                wl_display_cancel_read(nested.display);
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

        if (wl_display_dispatch_pending(host.display) < 0 ||
            wl_display_dispatch_pending(nested.display) < 0) break;
    }

    if (debug) {
        printf("SC7 Rack Clipboard Bridge shutting down...\n");
    }

    wl_display_disconnect(host.display);
    wl_display_disconnect(nested.display);
    return 0;
}
