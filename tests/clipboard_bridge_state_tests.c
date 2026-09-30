// Isolated offer-lifetime tests. The Wayland offer calls are replaced with
// small fakes; all selection handling is the production bridge implementation.
#include <assert.h>
#include <stdbool.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../bridge/wlr-data-control-unstable-v1-client-protocol.h"

static void *fake_offer_get_user_data(struct zwlr_data_control_offer_v1 *offer);
static void fake_offer_set_user_data(struct zwlr_data_control_offer_v1 *offer, void *data);
static int fake_offer_add_listener(struct zwlr_data_control_offer_v1 *offer,
                                   const struct zwlr_data_control_offer_v1_listener *listener,
                                   void *data);
static void fake_offer_destroy(struct zwlr_data_control_offer_v1 *offer);
static void fake_source_destroy(struct zwlr_data_control_source_v1 *source);
static void fake_source_offer(struct zwlr_data_control_source_v1 *source,
                              const char *mime_type);
static struct zwlr_data_control_source_v1 *fake_create_data_source(
    struct zwlr_data_control_manager_v1 *manager);
static int fake_source_add_listener(struct zwlr_data_control_source_v1 *source,
                                   const struct zwlr_data_control_source_v1_listener *listener,
                                   void *data);
static void fake_set_selection(struct zwlr_data_control_device_v1 *device,
                               struct zwlr_data_control_source_v1 *source);
static void fake_set_primary_selection(struct zwlr_data_control_device_v1 *device,
                                       struct zwlr_data_control_source_v1 *source);
static int fake_display_flush(struct wl_display *display);
static int fake_display_prepare_read(struct wl_display *display);
static int fake_display_dispatch_pending(struct wl_display *display);
static struct wl_callback *fake_display_sync(struct wl_display *display);
static int fake_callback_add_listener(struct wl_callback *callback,
                                      const struct wl_callback_listener *listener,
                                      void *data);
static void fake_callback_destroy(struct wl_callback *callback);
static int fake_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                               void *(*start_routine)(void *), void *arg);

#define zwlr_data_control_offer_v1_get_user_data fake_offer_get_user_data
#define zwlr_data_control_offer_v1_set_user_data fake_offer_set_user_data
#define zwlr_data_control_offer_v1_add_listener fake_offer_add_listener
#define zwlr_data_control_offer_v1_destroy fake_offer_destroy
#define zwlr_data_control_source_v1_destroy fake_source_destroy
#define zwlr_data_control_source_v1_offer fake_source_offer
#define zwlr_data_control_manager_v1_create_data_source fake_create_data_source
#define zwlr_data_control_source_v1_add_listener fake_source_add_listener
#define zwlr_data_control_device_v1_set_selection fake_set_selection
#define zwlr_data_control_device_v1_set_primary_selection fake_set_primary_selection
#define wl_display_flush fake_display_flush
#define wl_display_prepare_read fake_display_prepare_read
#define wl_display_dispatch_pending fake_display_dispatch_pending
#define wl_display_sync fake_display_sync
#define wl_callback_add_listener fake_callback_add_listener
#define wl_callback_destroy fake_callback_destroy
#define pthread_create fake_pthread_create
#define main bridge_main_for_test
#include "../bridge/sc7_clipboard_bridge.c"
#undef main
#undef zwlr_data_control_offer_v1_destroy
#undef wl_display_flush
#undef wl_display_prepare_read
#undef wl_display_dispatch_pending
#undef wl_display_sync
#undef wl_callback_add_listener
#undef wl_callback_destroy
#undef pthread_create
#undef zwlr_data_control_source_v1_offer
#undef zwlr_data_control_source_v1_destroy
#undef zwlr_data_control_manager_v1_create_data_source
#undef zwlr_data_control_source_v1_add_listener
#undef zwlr_data_control_device_v1_set_selection
#undef zwlr_data_control_device_v1_set_primary_selection
#undef zwlr_data_control_offer_v1_add_listener
#undef zwlr_data_control_offer_v1_set_user_data
#undef zwlr_data_control_offer_v1_get_user_data

struct fake_offer {
    struct offer_info *info;
};

struct fake_callback {
    const struct wl_callback_listener *listener;
    void *data;
};

static size_t destroyed_offers;
static size_t destroyed_sources;
static bool marker_was_offered;
static bool text_was_offered;
static size_t marker_offer_count;
static size_t failed_thread_creations;
static int prepare_failures_left;
static int dispatch_result;
static size_t prepare_calls;
static size_t dispatch_calls;
static struct wl_callback *last_sync;

static void init_endpoint(struct bridge_endpoint *ep) {
    (void)ep;
}

static void *fake_offer_get_user_data(struct zwlr_data_control_offer_v1 *offer) {
    return ((struct fake_offer *)offer)->info;
}

static void fake_offer_set_user_data(struct zwlr_data_control_offer_v1 *offer, void *data) {
    ((struct fake_offer *)offer)->info = data;
}

static int fake_offer_add_listener(struct zwlr_data_control_offer_v1 *offer,
                                   const struct zwlr_data_control_offer_v1_listener *listener,
                                   void *data) {
    assert(data == fake_offer_get_user_data(offer));
    assert(listener->offer == offer_handle_offer);
    return 0;
}

static void fake_offer_destroy(struct zwlr_data_control_offer_v1 *offer) {
    destroyed_offers++;
    free(offer);
}

static void fake_source_destroy(struct zwlr_data_control_source_v1 *source) {
    destroyed_sources++;
    free(source);
}

static void fake_source_offer(struct zwlr_data_control_source_v1 *source,
                              const char *mime_type) {
    (void)source;
    if (is_owner_marker(mime_type)) {
        marker_was_offered = true;
        marker_offer_count++;
    }
    if (strcmp(mime_type, "text/plain") == 0) text_was_offered = true;
}

static struct zwlr_data_control_source_v1 *fake_create_data_source(
    struct zwlr_data_control_manager_v1 *manager) {
    assert(manager);
    struct zwlr_data_control_source_v1 *source = malloc(1);
    assert(source);
    return source;
}

static int fake_source_add_listener(struct zwlr_data_control_source_v1 *source,
                                   const struct zwlr_data_control_source_v1_listener *listener,
                                   void *data) {
    assert(source && data);
    assert(listener->send == source_handle_send);
    assert(listener->cancelled == source_handle_cancelled);
    return 0;
}

static void fake_set_selection(struct zwlr_data_control_device_v1 *device,
                               struct zwlr_data_control_source_v1 *source) {
    assert(device && source);
}

static void fake_set_primary_selection(struct zwlr_data_control_device_v1 *device,
                                       struct zwlr_data_control_source_v1 *source) {
    assert(device && source);
}

static int fake_display_flush(struct wl_display *display) {
    (void)display;
    return 0;
}

static int fake_display_prepare_read(struct wl_display *display) {
    (void)display;
    prepare_calls++;
    if (prepare_failures_left > 0) {
        prepare_failures_left--;
        return -1;
    }
    return 0;
}

static int fake_display_dispatch_pending(struct wl_display *display) {
    (void)display;
    dispatch_calls++;
    return dispatch_result;
}

static struct wl_callback *fake_display_sync(struct wl_display *display) {
    (void)display;
    struct fake_callback *callback = calloc(1, sizeof(*callback));
    assert(callback);
    last_sync = (struct wl_callback *)callback;
    return last_sync;
}

static int fake_callback_add_listener(struct wl_callback *callback,
                                      const struct wl_callback_listener *listener,
                                      void *data) {
    struct fake_callback *fake = (struct fake_callback *)callback;
    fake->listener = listener;
    fake->data = data;
    return 0;
}

static void fake_callback_destroy(struct wl_callback *callback) {
    free(callback);
}

static void complete_last_sync(void) {
    assert(last_sync);
    struct wl_callback *callback = last_sync;
    struct fake_callback *fake = (struct fake_callback *)callback;
    assert(fake->listener && fake->listener->done);
    last_sync = NULL;
    fake->listener->done(fake->data, callback, 1);
}

static int fake_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                               void *(*start_routine)(void *), void *arg) {
    (void)thread;
    (void)attr;
    (void)start_routine;
    (void)arg;
    failed_thread_creations++;
    return EAGAIN;
}

static size_t count_open_fds(void) {
    DIR *dir = opendir("/proc/self/fd");
    assert(dir);
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] != '.') count++;
    }
    closedir(dir);
    return count;
}

static struct zwlr_data_control_offer_v1 *new_offer(struct bridge_endpoint *ep,
                                                    bool copied_marker) {
    struct fake_offer *fake = calloc(1, sizeof(*fake));
    assert(fake);
    struct zwlr_data_control_offer_v1 *offer =
        (struct zwlr_data_control_offer_v1 *)fake;
    device_handle_data_offer(ep, NULL, offer);
    struct offer_info *info = fake_offer_get_user_data(offer);
    assert(info && info->offer == offer);
    offer_handle_offer(info, offer, "text/plain");
    if (copied_marker) {
        offer_handle_offer(info, offer, OWNER_MIME_PREFIX "copied-old-marker");
    }
    return offer;
}

static struct zwlr_data_control_offer_v1 *new_owned_offer(
        struct bridge_endpoint *ep, bool is_primary) {
    struct zwlr_data_control_source_v1 *source = is_primary ?
        ep->our_primary_source : ep->our_source;
    assert(source);
    struct pending_owner_marker *marker =
        track_pending_source(ep, source, is_primary);
    assert(marker);
    struct zwlr_data_control_offer_v1 *offer = new_offer(ep, false);
    struct offer_info *info = fake_offer_get_user_data(offer);
    offer_handle_offer(info, offer, marker->mime);
    return offer;
}

static void test_regular_offer_lifetime(void) {
    struct bridge_endpoint ep = {0};
    init_endpoint(&ep);
    for (int cycle = 0; cycle < 1000; cycle++) {
        struct zwlr_data_control_offer_v1 *external = new_offer(&ep, false);
        device_handle_selection(&ep, NULL, external);
        assert(ep.current_offer == fake_offer_get_user_data(external));

        // Destroying a prior mirrored source may first produce a transient
        // NULL selection, followed by an offer bearing our private marker.
        ep.our_source = malloc(1);
        assert(ep.our_source);
        device_handle_selection(&ep, NULL, NULL);
        device_handle_selection(&ep, NULL, new_owned_offer(&ep, false));
        assert(ep.current_offer == NULL);
        assert(ep.is_our_offer);
        assert(destroyed_offers == (size_t)(cycle + 1) * 2);
        fake_source_destroy(ep.our_source);
        ep.our_source = NULL;
    }

    device_handle_selection(&ep, NULL, new_offer(&ep, false));
    device_handle_selection(&ep, NULL, NULL);
    assert(ep.current_offer == NULL);
    assert(!ep.is_our_offer);
    assert(destroyed_offers == 2001);
}

static void test_primary_offer_lifetime(void) {
    struct bridge_endpoint ep = {0};
    init_endpoint(&ep);
    size_t before = destroyed_offers;
    for (int cycle = 0; cycle < 1000; cycle++) {
        struct zwlr_data_control_offer_v1 *external = new_offer(&ep, false);
        device_handle_primary_selection(&ep, NULL, external);
        assert(ep.current_primary_offer == fake_offer_get_user_data(external));

        ep.our_primary_source = malloc(1);
        assert(ep.our_primary_source);
        device_handle_primary_selection(&ep, NULL, NULL);
        device_handle_primary_selection(&ep, NULL, new_owned_offer(&ep, true));
        assert(ep.current_primary_offer == NULL);
        assert(ep.is_our_primary_offer);
        assert(destroyed_offers == before + (size_t)(cycle + 1) * 2);
        fake_source_destroy(ep.our_primary_source);
        ep.our_primary_source = NULL;
    }

    device_handle_primary_selection(&ep, NULL, new_offer(&ep, false));
    device_handle_primary_selection(&ep, NULL, NULL);
    assert(ep.current_primary_offer == NULL);
    assert(!ep.is_our_primary_offer);
    assert(destroyed_offers == before + 2001);
}

static void test_null_selection_does_not_reuse_stale_offer(void) {
    struct bridge_endpoint host = {0};
    struct bridge_endpoint nested = {0};
    init_endpoint(&host);
    init_endpoint(&nested);
    host.peer = &nested;
    nested.peer = &host;
    nested.our_source = malloc(1);
    assert(nested.our_source);
    size_t sources_before = destroyed_sources;

    device_handle_selection(&host, NULL, new_offer(&host, false));
    device_handle_selection(&host, NULL, NULL);
    assert(nested.our_source == NULL);
    assert(destroyed_sources == sources_before + 1);

    // Even if a late send callback arrives before source cancellation has
    // been dispatched, a cleared host offer cannot be reused.
    nested.our_source = malloc(1);
    assert(nested.our_source);
    int p[2];
    assert(pipe(p) == 0);
    source_handle_send(&nested, nested.our_source, "text/plain", p[1]);
    char byte;
    assert(read(p[0], &byte, 1) == 0);
    close(p[0]);
    free(nested.our_source);
    nested.our_source = NULL;

    nested.our_primary_source = malloc(1);
    assert(nested.our_primary_source);
    device_handle_primary_selection(&host, NULL, new_offer(&host, false));
    device_handle_primary_selection(&host, NULL, NULL);
    assert(nested.our_primary_source == NULL);
    assert(destroyed_sources == sources_before + 2);
}

static void test_untracked_offer_is_destroyed(void) {
    struct bridge_endpoint ep = {0};
    struct fake_offer *regular = calloc(1, sizeof(*regular));
    struct fake_offer *primary = calloc(1, sizeof(*primary));
    assert(regular && primary);
    device_handle_selection(&ep, NULL,
        (struct zwlr_data_control_offer_v1 *)regular);
    device_handle_primary_selection(&ep, NULL,
        (struct zwlr_data_control_offer_v1 *)primary);
    assert(ep.current_offer == NULL);
    assert(ep.current_primary_offer == NULL);
}

static void test_cancel_without_echo_does_not_swallow_foreign_offer(void) {
    struct bridge_endpoint ep = {0};
    init_endpoint(&ep);

    device_handle_selection(&ep, NULL, new_offer(&ep, false));
    device_handle_selection(&ep, NULL, NULL);
    ep.our_source = malloc(1);
    assert(ep.our_source);
    struct pending_owner_marker *marker =
        track_pending_source(&ep, ep.our_source, false);
    assert(marker);
    char stale_regular[OWNER_MIME_CAPACITY];
    strcpy(stale_regular, marker->mime);
    source_handle_cancelled(&ep, ep.our_source);
    assert(ep.our_source == NULL);
    complete_last_sync();
    assert(ep.pending_markers == NULL);

    struct zwlr_data_control_offer_v1 *foreign = new_offer(&ep, false);
    device_handle_selection(&ep, NULL, foreign);
    assert(ep.current_offer == fake_offer_get_user_data(foreign));
    // A clipboard history app can copy an old marker after our source was
    // cancelled. It must still be treated as a foreign selection.
    foreign = new_offer(&ep, false);
    offer_handle_offer(fake_offer_get_user_data(foreign), foreign, stale_regular);
    device_handle_selection(&ep, NULL, foreign);
    assert(ep.current_offer == fake_offer_get_user_data(foreign));
    device_handle_selection(&ep, NULL, NULL);

    device_handle_primary_selection(&ep, NULL, new_offer(&ep, false));
    device_handle_primary_selection(&ep, NULL, NULL);
    ep.our_primary_source = malloc(1);
    assert(ep.our_primary_source);
    marker = track_pending_source(&ep, ep.our_primary_source, true);
    assert(marker);
    char stale_primary[OWNER_MIME_CAPACITY];
    strcpy(stale_primary, marker->mime);
    source_handle_cancelled(&ep, ep.our_primary_source);
    assert(ep.our_primary_source == NULL);
    complete_last_sync();
    assert(ep.pending_markers == NULL);
    foreign = new_offer(&ep, false);
    device_handle_primary_selection(&ep, NULL, foreign);
    assert(ep.current_primary_offer == fake_offer_get_user_data(foreign));
    foreign = new_offer(&ep, false);
    offer_handle_offer(fake_offer_get_user_data(foreign), foreign, stale_primary);
    device_handle_primary_selection(&ep, NULL, foreign);
    assert(ep.current_primary_offer == fake_offer_get_user_data(foreign));
    device_handle_primary_selection(&ep, NULL, NULL);
}

static void test_foreign_before_own_echo(void) {
    struct bridge_endpoint ep = {0};
    init_endpoint(&ep);
    ep.our_source = malloc(1);
    ep.our_primary_source = malloc(1);
    assert(ep.our_source && ep.our_primary_source);

    struct zwlr_data_control_offer_v1 *foreign = new_offer(&ep, false);
    device_handle_selection(&ep, NULL, foreign);
    assert(ep.current_offer == fake_offer_get_user_data(foreign));
    device_handle_selection(&ep, NULL, new_owned_offer(&ep, false));
    assert(ep.current_offer == NULL);
    assert(ep.is_our_offer);

    foreign = new_offer(&ep, false);
    device_handle_primary_selection(&ep, NULL, foreign);
    assert(ep.current_primary_offer == fake_offer_get_user_data(foreign));
    device_handle_primary_selection(&ep, NULL, new_owned_offer(&ep, true));
    assert(ep.current_primary_offer == NULL);
    assert(ep.is_our_primary_offer);
    free(ep.our_source);
    free(ep.our_primary_source);
}

static void test_revoke_before_queued_echo(void) {
    struct bridge_endpoint host = {0};
    struct bridge_endpoint nested = {0};
    host.peer = &nested;
    nested.peer = &host;
    for (int mode = 0; mode < 2; mode++) {
        struct zwlr_data_control_source_v1 *source = malloc(1);
        assert(source);
        if (mode == 0) nested.our_source = source;
        else nested.our_primary_source = source;
        struct pending_owner_marker *marker =
            track_pending_source(&nested, source, mode == 1);
        assert(marker);
        char mime[OWNER_MIME_CAPACITY];
        strcpy(mime, marker->mime);

        if (mode == 0) device_handle_selection(&host, NULL, NULL);
        else device_handle_primary_selection(&host, NULL, NULL);
        assert(mode == 0 ? nested.our_source == NULL :
                           nested.our_primary_source == NULL);
        assert(last_sync);

        // The echoed offer was already queued on the socket when the local
        // source was revoked. It must still be recognized as our own.
        struct zwlr_data_control_offer_v1 *echo = new_offer(&nested, false);
        offer_handle_offer(fake_offer_get_user_data(echo), echo, mime);
        if (mode == 0) device_handle_selection(&nested, NULL, echo);
        else device_handle_primary_selection(&nested, NULL, echo);
        assert(mode == 0 ? nested.current_offer == NULL :
                           nested.current_primary_offer == NULL);
        complete_last_sync();
        assert(nested.pending_markers == NULL);

        // Reusing the old MIME after the sync fence is a foreign selection.
        struct zwlr_data_control_offer_v1 *copy = new_offer(&nested, false);
        offer_handle_offer(fake_offer_get_user_data(copy), copy, mime);
        if (mode == 0) {
            device_handle_selection(&nested, NULL, copy);
            assert(nested.current_offer == fake_offer_get_user_data(copy));
            device_handle_selection(&nested, NULL, NULL);
        } else {
            device_handle_primary_selection(&nested, NULL, copy);
            assert(nested.current_primary_offer == fake_offer_get_user_data(copy));
            device_handle_primary_selection(&nested, NULL, NULL);
        }
    }
}

static void test_marker_detected_after_mime_limit(void) {
    struct bridge_endpoint ep = {0};
    for (int mode = 0; mode < 2; mode++) {
        struct zwlr_data_control_source_v1 *source = malloc(1);
        assert(source);
        if (mode == 0) ep.our_source = source;
        else ep.our_primary_source = source;
        struct pending_owner_marker *marker =
            track_pending_source(&ep, source, mode == 1);
        assert(marker);

        struct zwlr_data_control_offer_v1 *offer = new_offer(&ep, false);
        struct offer_info *info = fake_offer_get_user_data(offer);
        for (int i = 1; i < MAX_MIMES; i++) {
            char mime[32];
            snprintf(mime, sizeof(mime), "application/x-test-%d", i);
            offer_handle_offer(info, offer, mime);
        }
        assert(info->mimes.count == MAX_MIMES);
        offer_handle_offer(info, offer, marker->mime);
        assert(info->mimes.count == MAX_MIMES);
        if (mode == 0) {
            device_handle_selection(&ep, NULL, offer);
            assert(ep.current_offer == NULL);
            assert(ep.is_our_offer);
        } else {
            device_handle_primary_selection(&ep, NULL, offer);
            assert(ep.current_primary_offer == NULL);
            assert(ep.is_our_primary_offer);
        }
        assert(ep.pending_markers == NULL);
        fake_source_destroy(source);
        if (mode == 0) ep.our_source = NULL;
        else ep.our_primary_source = NULL;
    }
}

static void test_marker_is_filtered_from_forwarded_mimes(void) {
    struct offer_info offer = {0};
    mime_list_add(&offer.mimes, "text/plain");
    mime_list_add(&offer.mimes, OWNER_MIME_PREFIX "copied-owner");
    marker_was_offered = false;
    text_was_offered = false;
    offer_all_synthesized_mimes((struct zwlr_data_control_source_v1 *)1, &offer);
    assert(!marker_was_offered);
    assert(text_was_offered);
    mime_list_clear(&offer.mimes);
}

static void test_owner_marker_generation(void) {
    char marker[sizeof(OWNER_MIME_PREFIX) + OWNER_MIME_RANDOM_BYTES * 2];
    assert(generate_owner_mime(marker, sizeof(marker)) == 0);
    assert(is_owner_marker(marker));
    assert(strlen(marker) == strlen(OWNER_MIME_PREFIX) + OWNER_MIME_RANDOM_BYTES * 2);
}

static void test_propagation_advertises_only_destination_marker(void) {
    struct bridge_endpoint src = {0};
    struct bridge_endpoint dst = {0};
    init_endpoint(&src);
    init_endpoint(&dst);
    src.peer = &dst;
    dst.peer = &src;
    dst.ready = true;
    dst.manager = (struct zwlr_data_control_manager_v1 *)1;
    dst.device = (struct zwlr_data_control_device_v1 *)1;
    dst.display = (struct wl_display *)1;

    for (int mode = 0; mode < 2; mode++) {
        struct zwlr_data_control_offer_v1 *incoming = new_offer(&src, false);
        struct offer_info *info = fake_offer_get_user_data(incoming);
        mime_list_add(&info->mimes, OWNER_MIME_PREFIX "copied-old-marker");
        if (mode == 0) src.current_offer = info;
        else src.current_primary_offer = info;

        marker_offer_count = 0;
        text_was_offered = false;
        propagate_selection(&src, mode == 1);
        assert(marker_offer_count == 1);
        assert(text_was_offered);
        struct zwlr_data_control_source_v1 *outgoing = mode == 0 ?
            dst.our_source : dst.our_primary_source;
        assert(outgoing);
        struct pending_owner_marker *marker = find_pending_source(&dst, outgoing);
        assert(marker);
        struct zwlr_data_control_offer_v1 *echo = new_offer(&dst, false);
        offer_handle_offer(fake_offer_get_user_data(echo), echo, marker->mime);
        if (mode == 0) device_handle_selection(&dst, NULL, echo);
        else device_handle_primary_selection(&dst, NULL, echo);
        assert(find_pending_source(&dst, outgoing) == NULL);
        fake_source_destroy(outgoing);
        if (mode == 0) dst.our_source = NULL;
        else dst.our_primary_source = NULL;
        offer_info_destroy(info);
        if (mode == 0) src.current_offer = NULL;
        else src.current_primary_offer = NULL;
    }
}

static void test_conversion_thread_failure_closes_fds(void) {
    struct bridge_endpoint host = {0};
    struct bridge_endpoint nested = {0};
    init_endpoint(&host);
    init_endpoint(&nested);
    host.peer = &nested;
    nested.peer = &host;
    nested.our_source = malloc(1);
    assert(nested.our_source);

    size_t baseline = count_open_fds();
    for (int mode = 0; mode < 2; mode++) {
        struct zwlr_data_control_offer_v1 *offer = new_offer(&host, false);
        host.current_offer = fake_offer_get_user_data(offer);
        mime_list_clear(&host.current_offer->mimes);
        mime_list_add(&host.current_offer->mimes,
                      mode == 0 ? "text/uri-list" : "text/plain");

        int p[2];
        assert(pipe(p) == 0);
        source_handle_send(&nested, nested.our_source,
                           mode == 0 ? "text/plain" : "text/uri-list", p[1]);
        assert(fcntl(p[1], F_GETFD) == -1 && errno == EBADF);
        char byte;
        assert(read(p[0], &byte, 1) == 0);
        close(p[0]);
        assert(count_open_fds() == baseline);
        offer_info_destroy(host.current_offer);
        host.current_offer = NULL;
    }
    assert(failed_thread_creations == 2);
    free(nested.our_source);
}

static void test_private_marker_send_has_no_payload(void) {
    struct bridge_endpoint host = {0};
    struct bridge_endpoint nested = {0};
    init_endpoint(&host);
    init_endpoint(&nested);
    host.peer = &nested;
    nested.peer = &host;
    nested.our_source = malloc(1);
    assert(nested.our_source);

    struct zwlr_data_control_offer_v1 *offer = new_offer(&host, false);
    host.current_offer = fake_offer_get_user_data(offer);
    int p[2];
    assert(pipe(p) == 0);
    source_handle_send(&nested, nested.our_source,
                       OWNER_MIME_PREFIX "private", p[1]);
    char byte;
    assert(read(p[0], &byte, 1) == 0);
    close(p[0]);
    offer_info_destroy(host.current_offer);
    free(nested.our_source);
}

static void test_disconnected_display_does_not_spin(void) {
    prepare_failures_left = 1;
    dispatch_result = -1;
    prepare_calls = 0;
    dispatch_calls = 0;
    assert(!prepare_endpoint_read((struct wl_display *)1));
    assert(prepare_calls == 1);
    assert(dispatch_calls == 1);

    prepare_failures_left = 1;
    dispatch_result = 0;
    prepare_calls = 0;
    dispatch_calls = 0;
    assert(prepare_endpoint_read((struct wl_display *)1));
    assert(prepare_calls == 2);
    assert(dispatch_calls == 1);
}

int main(void) {
    test_regular_offer_lifetime();
    test_primary_offer_lifetime();
    test_null_selection_does_not_reuse_stale_offer();
    test_untracked_offer_is_destroyed();
    test_cancel_without_echo_does_not_swallow_foreign_offer();
    test_foreign_before_own_echo();
    test_revoke_before_queued_echo();
    test_marker_detected_after_mime_limit();
    test_marker_is_filtered_from_forwarded_mimes();
    test_owner_marker_generation();
    test_propagation_advertises_only_destination_marker();
    test_conversion_thread_failure_closes_fds();
    test_private_marker_send_has_no_payload();
    test_disconnected_display_does_not_spin();
    assert(destroyed_offers == 4029);
    puts("clipboard bridge: 4029 offers reclaimed across 2000 ownership cycles");
    return 0;
}
