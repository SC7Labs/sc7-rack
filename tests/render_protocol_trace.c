/* Test-only, passive Wayland protocol telemetry. A wlr_buffer release signal
 * is not a wl_buffer.release event: the public protocol logger observes the
 * actual server event at the marshal boundary, before socket delivery. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include "render_protocol_trace.h"

extern FILE *sc7_render_trace_file(void) __attribute__((weak));
extern uint64_t sc7_render_watch_buffer(struct wlr_buffer *) __attribute__((weak));
extern uint64_t sc7_render_trace_time_us(void) __attribute__((weak));
extern uint64_t sc7_render_trace_frame(void) __attribute__((weak));

struct resource_entry {
    struct wl_list link;
    struct wl_resource *resource;
    struct wl_listener destroy;
    struct wlr_buffer *buffer; /* Identity only; never dereference later. */
    uint64_t generation;
};

struct surface_entry {
    struct wl_list link;
    struct wl_resource *resource;
    struct wl_listener destroy;
    struct wl_resource *pending_buffer; /* Identity only; lookup before use. */
    bool attach_pending;
    uint64_t commit_sequence;
};

struct display_trace {
    struct wl_protocol_logger *logger;
    struct wl_listener destroy;
};

struct wlr_surface_entry {
    struct wl_list link;
    struct wlr_surface *surface;
    struct wl_listener client_commit, commit, destroy;
};

static struct wl_list resources = {&resources, &resources};
static struct wl_list surfaces = {&surfaces, &surfaces};
static struct wl_list wlr_surfaces = {&wlr_surfaces, &wlr_surfaces};

static uint64_t timestamp(void) {
    if (sc7_render_trace_time_us) {
        return sc7_render_trace_time_us();
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static FILE *trace_file(void) {
    return sc7_render_trace_file ? sc7_render_trace_file() : NULL;
}

static uint64_t frame(void) {
    return sc7_render_trace_frame ? sc7_render_trace_frame() : 0;
}

static void log_surface_state(struct wlr_surface *surface, bool pending) {
    struct wlr_surface_state *state = pending ? &surface->pending : &surface->current;
    struct wlr_buffer *buffer = state->buffer;
    uint64_t generation = buffer && sc7_render_watch_buffer ?
        sc7_render_watch_buffer(buffer) : 0;
    FILE *file = trace_file();
    if (!file) {
        return;
    }
    pid_t pid;
    wl_client_get_credentials(wl_resource_get_client(surface->resource), &pid, NULL, NULL);
    fprintf(file, "%" PRIu64 " %s frame=%" PRIu64
        " pid=%d surface=%p resource=%p id=%u seq=%u committed=0x%08x"
        " source=%p generation=%" PRIu64 " source_locks=%zu texture=%p\n",
        timestamp(), pending ? "wlroots-client-commit" : "wlroots-applied-commit",
        frame(), pid, (void *)surface, (void *)surface->resource,
        wl_resource_get_id(surface->resource), state->seq, state->committed,
        (void *)buffer, generation, buffer ? buffer->n_locks : 0,
        surface->buffer ? (void *)surface->buffer->texture : NULL);
}

static void wlr_client_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct wlr_surface_entry *entry = wl_container_of(listener, entry, client_commit);
    log_surface_state(entry->surface, true);
}

static void wlr_applied_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct wlr_surface_entry *entry = wl_container_of(listener, entry, commit);
    log_surface_state(entry->surface, false);
}

static void wlr_surface_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct wlr_surface_entry *entry = wl_container_of(listener, entry, destroy);
    wl_list_remove(&entry->client_commit.link);
    wl_list_remove(&entry->commit.link);
    wl_list_remove(&entry->destroy.link);
    wl_list_remove(&entry->link);
    free(entry);
}

void sc7_render_protocol_watch_surface(struct wlr_surface *surface) {
    if (!surface || !getenv("SC7_RENDER_TRACE")) {
        return;
    }
    struct wlr_surface_entry *entry;
    wl_list_for_each(entry, &wlr_surfaces, link) {
        if (entry->surface == surface) {
            return;
        }
    }
    entry = calloc(1, sizeof(*entry));
    if (!entry) {
        return;
    }
    entry->surface = surface;
    entry->client_commit.notify = wlr_client_commit;
    entry->commit.notify = wlr_applied_commit;
    entry->destroy.notify = wlr_surface_destroy;
    wl_signal_add(&surface->events.client_commit, &entry->client_commit);
    wl_signal_add(&surface->events.commit, &entry->commit);
    wl_signal_add(&surface->events.destroy, &entry->destroy);
    wl_list_insert(&wlr_surfaces, &entry->link);
}

static struct resource_entry *find_resource(struct wl_resource *resource) {
    struct resource_entry *entry;
    wl_list_for_each(entry, &resources, link) {
        if (entry->resource == resource) {
            return entry;
        }
    }
    return NULL;
}

bool sc7_render_protocol_buffer_identity(struct wlr_buffer *buffer,
        struct sc7_render_protocol_identity *identity) {
    if (!identity) {
        return false;
    }
    *identity = (struct sc7_render_protocol_identity){0};
    /* Live client wrappers can be nested by saved-view imports. Bound the walk
     * defensively; a corrupt or cyclic wrapper chain must not hang telemetry. */
    for (unsigned depth = 0; buffer && depth < 16; ++depth) {
        struct resource_entry *entry;
        wl_list_for_each(entry, &resources, link) {
            if (entry->buffer == buffer) {
                pid_t pid;
                wl_client_get_credentials(wl_resource_get_client(entry->resource),
                    &pid, NULL, NULL);
                *identity = (struct sc7_render_protocol_identity){
                    .resource = entry->resource,
                    .id = wl_resource_get_id(entry->resource),
                    .pid = pid,
                    .generation = entry->generation,
                    .source = entry->buffer,
                };
                return true;
            }
        }
        struct wlr_client_buffer *client = wlr_client_buffer_get(buffer);
        if (!client) {
            return false;
        }
        buffer = client->source;
    }
    return false;
}

static void resource_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct resource_entry *entry = wl_container_of(listener, entry, destroy);
    /* Pending attachment identities cannot outlive a destroyed resource: its
     * address may later be reused by an unrelated client object. */
    struct surface_entry *surface;
    wl_list_for_each(surface, &surfaces, link) {
        if (surface->pending_buffer == entry->resource) {
            surface->pending_buffer = NULL;
        }
    }
    wl_list_remove(&entry->destroy.link);
    wl_list_remove(&entry->link);
    free(entry);
}

void sc7_render_protocol_resource_buffer(struct wl_resource *resource,
        struct wlr_buffer *buffer) {
    if (!resource || !buffer || !getenv("SC7_RENDER_TRACE")) {
        return;
    }
    uint64_t generation = sc7_render_watch_buffer ?
        sc7_render_watch_buffer(buffer) : 0;
    struct resource_entry *entry = find_resource(resource);
    if (!entry) {
        entry = calloc(1, sizeof(*entry));
        if (!entry) {
            return;
        }
        entry->resource = resource;
        entry->destroy.notify = resource_destroy;
        wl_resource_add_destroy_listener(resource, &entry->destroy);
        wl_list_insert(&resources, &entry->link);
    }
    entry->buffer = buffer;
    entry->generation = generation;
    FILE *file = trace_file();
    if (file) {
        pid_t pid;
        wl_client_get_credentials(wl_resource_get_client(resource), &pid, NULL, NULL);
        fprintf(file, "%" PRIu64 " wl-buffer-source-map frame=%" PRIu64
            " pid=%d resource=%p id=%u source=%p generation=%" PRIu64 "\n",
            timestamp(), frame(), pid, (void *)resource,
            wl_resource_get_id(resource), (void *)buffer, generation);
    }
}

static void surface_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct surface_entry *entry = wl_container_of(listener, entry, destroy);
    wl_list_remove(&entry->destroy.link);
    wl_list_remove(&entry->link);
    free(entry);
}

static struct surface_entry *surface_entry(struct wl_resource *resource) {
    struct surface_entry *entry;
    wl_list_for_each(entry, &surfaces, link) {
        if (entry->resource == resource) {
            return entry;
        }
    }
    entry = calloc(1, sizeof(*entry));
    if (!entry) {
        return NULL;
    }
    entry->resource = resource;
    entry->destroy.notify = surface_destroy;
    wl_resource_add_destroy_listener(resource, &entry->destroy);
    wl_list_insert(&surfaces, &entry->link);
    return entry;
}

/* Verify against the generated public interface table; no numeric protocol
 * opcodes are guessed or overloaded between request and event directions. */
static bool is_message(const struct wl_protocol_logger_message *message,
        const struct wl_interface *interface, bool event, const char *name) {
    if (strcmp(wl_resource_get_class(message->resource), interface->name) != 0) {
        return false;
    }
    int count = event ? interface->event_count : interface->method_count;
    const struct wl_message *table = event ? interface->events : interface->methods;
    int opcode = message->message_opcode;
    return opcode >= 0 && opcode < count && message->message &&
        strcmp(table[opcode].name, name) == 0 &&
        strcmp(message->message->name, name) == 0 &&
        strcmp(table[opcode].signature, message->message->signature) == 0;
}

static void protocol_message(void *data, enum wl_protocol_logger_type direction,
        const struct wl_protocol_logger_message *message) {
    (void)data;
    bool event = direction == WL_PROTOCOL_LOGGER_EVENT;
    const char *name = NULL;
    struct resource_entry *buffer = NULL;
    struct surface_entry *surface = NULL;
    uint32_t buffer_id = 0;
    bool pending = false;
    if (event && is_message(message, &wl_buffer_interface, true, "release")) {
        name = "wl-buffer-release-sent";
        buffer = find_resource(message->resource);
        buffer_id = wl_resource_get_id(message->resource);
    } else if (!event && is_message(message, &wl_buffer_interface, false, "destroy")) {
        name = "wl-buffer-destroy-request";
        buffer = find_resource(message->resource);
        buffer_id = wl_resource_get_id(message->resource);
    } else if (!event && is_message(message, &wl_surface_interface, false, "attach")) {
        if (message->arguments_count != 3) {
            return;
        }
        name = "wl-surface-attach-request";
        surface = surface_entry(message->resource);
        struct wl_resource *attached = (void *)message->arguments[0].o;
        if (surface) {
            surface->pending_buffer = attached;
            surface->attach_pending = true;
        }
        if (attached) {
            buffer_id = wl_resource_get_id(attached);
            buffer = find_resource(attached);
        }
        pending = true;
    } else if (!event && is_message(message, &wl_surface_interface, false, "commit")) {
        name = "wl-surface-commit-request";
        surface = surface_entry(message->resource);
        if (surface) {
            ++surface->commit_sequence;
            pending = surface->attach_pending;
            buffer = find_resource(surface->pending_buffer);
            if (buffer) {
                buffer_id = wl_resource_get_id(buffer->resource);
            }
            surface->attach_pending = false;
        }
    } else {
        return;
    }
    FILE *file = trace_file();
    if (!file) {
        return;
    }
    pid_t pid;
    wl_client_get_credentials(wl_resource_get_client(message->resource), &pid, NULL, NULL);
    fprintf(file, "%" PRIu64 " %s frame=%" PRIu64
        " pid=%d resource=%p id=%u opcode=%d surface=%p commit=%" PRIu64
        " attach_pending=%d wl_buffer=%p buffer_id=%u source=%p generation=%" PRIu64 "\n",
        timestamp(), name, frame(), pid, (void *)message->resource,
        wl_resource_get_id(message->resource), message->message_opcode,
        surface ? (void *)surface->resource : NULL,
        surface ? surface->commit_sequence : 0, pending,
        buffer ? (void *)buffer->resource :
            surface ? (void *)surface->pending_buffer : NULL,
        buffer_id, buffer ? (void *)buffer->buffer : NULL,
        buffer ? buffer->generation : 0);
}

static void display_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct display_trace *trace = wl_container_of(listener, trace, destroy);
    wl_protocol_logger_destroy(trace->logger);
    wl_list_remove(&trace->destroy.link);
    free(trace);
}

struct wl_display *wl_display_create(void) {
    static struct wl_display *(*next)(void);
    if (!next) {
        next = dlsym(RTLD_NEXT, "wl_display_create");
    }
    struct wl_display *display = next();
    if (!display || !getenv("SC7_RENDER_TRACE")) {
        return display;
    }
    struct display_trace *trace = calloc(1, sizeof(*trace));
    if (!trace) {
        return display;
    }
    trace->logger = wl_display_add_protocol_logger(display, protocol_message, trace);
    if (!trace->logger) {
        free(trace);
        return display;
    }
    trace->destroy.notify = display_destroy;
    wl_display_add_destroy_listener(display, &trace->destroy);
    return display;
}
