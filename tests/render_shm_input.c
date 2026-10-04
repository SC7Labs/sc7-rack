/* Diagnostic-only client transport policy for pinned Rack Sway. The renderer,
 * allocator and all protocol objects still initialize normally. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>

extern FILE *sc7_render_trace_file(void) __attribute__((weak));
extern uint64_t sc7_render_trace_time_us(void) __attribute__((weak));

struct display_policy {
    struct wl_list link;
    struct wl_display *display;
    struct wl_listener destroy;
    wl_display_global_filter_func_t original;
    void *original_data;
};

static struct wl_list policies = {&policies, &policies};
static void (*real_set_filter)(struct wl_display *, wl_display_global_filter_func_t, void *);

static bool requested(void) {
    const char *mode = getenv("SC7_RENDER_EXPERIMENT");
    const char *trace = getenv("SC7_RENDER_TRACE");
    return trace && *trace && mode && strcmp(mode, "shm-input") == 0;
}

static void fail(const char *reason) {
    fprintf(stderr, "Rack shm-input diagnostic failed: %s\n", reason);
    _exit(EXIT_FAILURE);
}

static void trace_policy(const char *event, struct display_policy *policy,
        const struct wl_client *client, const char *interface, bool allowed) {
    FILE *file = sc7_render_trace_file ? sc7_render_trace_file() : NULL;
    if (!file) { return; }
    pid_t pid = 0;
    if (client) { wl_client_get_credentials((void *)client, &pid, NULL, NULL); }
    fprintf(file, "%" PRIu64 " %s display=%p pid=%ld interface=%s allowed=%d\n",
        sc7_render_trace_time_us ? sc7_render_trace_time_us() : 0,
        event, (void *)policy->display, (long)pid, interface, allowed);
}

static bool filter_input(const struct wl_client *client,
        const struct wl_global *global, void *data) {
    struct display_policy *policy = data;
    const char *name = wl_global_get_interface(global)->name;
    bool inherited = !policy->original ||
        policy->original(client, global, policy->original_data);
    bool dma = strcmp(name, "zwp_linux_dmabuf_v1") == 0 ||
        strcmp(name, "wl_drm") == 0;
    bool allowed = inherited && !dma;
    if (dma || strcmp(name, "wl_shm") == 0) {
        trace_policy("input-capability", policy, client, name, allowed);
    }
    return allowed;
}

static void remove_policy(struct display_policy *policy) {
    wl_list_remove(&policy->destroy.link);
    wl_list_remove(&policy->link);
    free(policy);
}

static void display_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct display_policy *policy = wl_container_of(listener, policy, destroy);
    /* No filter may retain our data while the display tears down globals. */
    real_set_filter(policy->display, NULL, NULL);
    remove_policy(policy);
}

void wl_display_set_global_filter(struct wl_display *display,
        wl_display_global_filter_func_t filter, void *data) {
    if (!real_set_filter) {
        real_set_filter = dlsym(RTLD_NEXT, "wl_display_set_global_filter");
        if (!real_set_filter) { fail("Wayland global-filter API unavailable"); }
    }
    struct display_policy *policy = NULL, *candidate;
    wl_list_for_each(candidate, &policies, link) {
        if (candidate->display == display) { policy = candidate; break; }
    }
    if (!requested()) {
        real_set_filter(display, filter, data);
        if (policy) { remove_policy(policy); }
        return;
    }
    if (!policy) {
        policy = calloc(1, sizeof(*policy));
        if (!policy) { fail("cannot allocate per-display policy"); }
        policy->display = display;
        policy->destroy.notify = display_destroy;
        wl_display_add_destroy_listener(display, &policy->destroy);
        wl_list_insert(&policies, &policy->link);
    }
    /* Sway installs its security filter before creating globals. Chain it,
     * including its data and any subsequent replacement, rather than bypass it. */
    policy->original = filter;
    policy->original_data = data;
    real_set_filter(display, filter_input, policy);
    trace_policy("shm-input-policy", policy, NULL,
        "zwp_linux_dmabuf_v1,wl_drm", false);
    fprintf(stderr, "Rack diagnostic: shm-input active; nested DMA-BUF input "
        "protocols hidden, renderer/output unchanged\n");
}
