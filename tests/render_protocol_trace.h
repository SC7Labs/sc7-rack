#ifndef SC7_RENDER_PROTOCOL_TRACE_H
#define SC7_RENDER_PROTOCOL_TRACE_H

#include <stdbool.h>
#include <stdint.h>

struct wl_resource;
struct wlr_buffer;
struct wlr_surface;

/* Correlate the actual protocol resource with the buffer returned by
 * wlr_buffer_try_from_resource. Does not take an additional buffer lock. */
void sc7_render_protocol_resource_buffer(struct wl_resource *resource,
    struct wlr_buffer *buffer);

/* Attach passive listeners to actual wlroots client/apply commit signals. */
void sc7_render_protocol_watch_surface(struct wlr_surface *surface);

struct sc7_render_protocol_identity {
    struct wl_resource *resource;
    uint32_t id;
    int32_t pid;
    uint64_t generation;
    struct wlr_buffer *source;
};

/* Passive lookup, including saved-view wrappers. False clears identity.
 * No returned pointer is retained independently of the existing owner. */
bool sc7_render_protocol_buffer_identity(struct wlr_buffer *buffer,
    struct sc7_render_protocol_identity *identity);

#endif
