// Exercise the patched Wayland backend's real offer and drag callbacks against
// wlroots' seat/data-source implementation. The test runner supplies the
// patched dnd.c path via SC7_DND_SOURCE_PATH.
#include SC7_DND_SOURCE_PATH

#include <dirent.h>
#include <sys/socket.h>

// Icon rendering is not exercised here; this internal wlroots symbol is not
// exported from the shared library used by the callback harness.
bool renderer_bind_buffer(struct wlr_renderer *r, struct wlr_buffer *buffer) {
	return false;
}

int64_t get_current_time_msec(void) {
	return 1;
}

struct wlr_wl_output *get_wl_output_from_surface(struct wlr_wl_backend *wl,
		struct wl_surface *surface) {
	return NULL;
}

struct fixture {
	struct wlr_wl_backend backend;
	struct wlr_wl_seat seat;
	struct wl_display *server_display;
	struct wlr_seat *local_seat;
	int peer_fd;
};

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stdout, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
		return false; \
	} \
} while (0)

static bool fixture_init(struct fixture *f) {
	memset(f, 0, sizeof(*f));
	f->peer_fd = -1;
	int fds[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
	f->backend.remote_display = wl_display_connect_to_fd(fds[0]);
	CHECK(f->backend.remote_display != NULL);
	f->peer_fd = fds[1];
	f->server_display = wl_display_create();
	CHECK(f->server_display != NULL);
	f->backend.local_display = f->server_display;
	wl_list_init(&f->backend.dnd_offers);
	wl_list_init(&f->backend.outputs);
	wl_list_init(&f->backend.seats);
	wl_list_init(&f->seat.pointers);
	f->seat.backend = &f->backend;
	f->local_seat = wlr_seat_create(f->server_display, "dnd-lifetime-test");
	CHECK(f->local_seat != NULL);
	return true;
}

static void fixture_finish(struct fixture *f) {
	if (f->local_seat) wlr_seat_destroy(f->local_seat);
	if (f->server_display) wl_display_destroy(f->server_display);
	if (f->backend.remote_display) wl_display_disconnect(f->backend.remote_display);
	if (f->peer_fd >= 0) close(f->peer_fd);
}

static int count_open_fds(void) {
	DIR *dir = opendir("/proc/self/fd");
	if (!dir) return -1;
	int count = 0;
	while (readdir(dir) != NULL) count++;
	closedir(dir);
	return count;
}

static struct host_data_offer *new_offer(struct fixture *f) {
	struct wl_data_offer *offer = (struct wl_data_offer *)wl_proxy_create(
		(struct wl_proxy *)f->backend.remote_display, &wl_data_offer_interface);
	if (!offer) return NULL;
	data_device_handle_data_offer(&f->seat, NULL, offer);
	struct host_data_offer *ho = find_host_data_offer(&f->backend, offer);
	if (ho) offer_handle_offer(ho, offer, "text/plain;charset=utf-8");
	return ho;
}

static bool test_selection_offers(void) {
	struct fixture f;
	CHECK(fixture_init(&f));
	int baseline_fds = count_open_fds();
	CHECK(baseline_fds > 0);
	for (int i = 0; i < 240; i++) {
		struct host_data_offer *ho = new_offer(&f);
		CHECK(ho != NULL);
		CHECK(!wl_list_empty(&f.backend.dnd_offers));
		data_device_handle_selection(&f.seat, NULL, ho->offer);
		CHECK(wl_list_empty(&f.backend.dnd_offers));
	}
	CHECK(count_open_fds() <= baseline_fds + 2);
	fixture_finish(&f);
	return true;
}

struct drop_counter {
	struct wl_listener listener;
	int count;
};

static void count_drop(struct wl_listener *listener, void *data) {
	struct drop_counter *counter = wl_container_of(listener, counter, listener);
	counter->count++;
	wl_list_remove(&listener->link);
}

static void destroy_drag_during_drop(struct wl_listener *listener, void *data) {
	struct wlr_drag_drop_event *event = data;
	wl_list_remove(&listener->link);
	wlr_drag_destroy(event->drag);
}

static bool test_drop_receive_finish(void) {
	struct fixture f;
	CHECK(fixture_init(&f));
	struct host_data_offer *ho = new_offer(&f);
	CHECK(ho != NULL);
	data_device_handle_enter(&f.seat, NULL, 1, NULL, 0, 0, ho->offer);
	struct wlr_drag *drag = f.backend.incoming_drag;
	CHECK(drag != NULL);
	struct proxy_data_source *proxy = (struct proxy_data_source *)drag->source;
	CHECK(f.local_seat->drag == drag);

	// The receiver is represented by an empty data-device list. This exercises
	// wlroots' focused-drop path without a GUI client, then transfers data via
	// the retained proxy source after the drag grab has ended.
	struct wlr_seat_client focus = {0};
	wl_list_init(&focus.data_devices);
	wl_signal_init(&focus.events.destroy);
	focus.seat = f.local_seat;
	struct wlr_surface fake_surface = {0};
	drag->focus_client = &focus;
	drag->focus = &fake_surface;
	wl_signal_add(&focus.events.destroy, &drag->seat_client_destroy);
	struct drop_counter counter = { .listener.notify = count_drop };
	wl_signal_add(&drag->events.drop, &counter.listener);
	data_device_handle_drop(&f.seat, NULL);
	CHECK(counter.count == 1);
	CHECK(f.local_seat->drag == NULL);
	CHECK(f.backend.incoming_drag == NULL);
	CHECK(proxy->host_offer == ho);
	CHECK(!wl_list_empty(&f.backend.dnd_offers));

	int transfer[2];
	CHECK(pipe2(transfer, O_CLOEXEC) == 0);
	proxy_source_send(&proxy->base, "text/plain;charset=utf-8", transfer[1]);
	char wire[256];
	char control[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { .iov_base = wire, .iov_len = sizeof(wire) };
	struct msghdr msg = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = control, .msg_controllen = sizeof(control),
	};
	CHECK(recvmsg(f.peer_fd, &msg, 0) > 0);
	int received_fd = -1;
	for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg;
			cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
			memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(received_fd));
		}
	}
	CHECK(received_fd >= 0);
	CHECK(write(received_fd, "drop-data", 9) == 9);
	close(received_fd);
	char result[16] = {0};
	CHECK(read(transfer[0], result, sizeof(result)) == 9);
	CHECK(memcmp(result, "drop-data", 9) == 0);
	close(transfer[0]);

	proxy_source_dnd_finish(&proxy->base);
	CHECK(proxy->host_offer == NULL);
	CHECK(wl_list_empty(&f.backend.dnd_offers));
	wlr_data_source_destroy(&proxy->base);
	CHECK(f.local_seat->drag_source == NULL);
	fixture_finish(&f);
	return true;
}

static bool test_cancelled_drags(void) {
	struct fixture f;
	CHECK(fixture_init(&f));
	int baseline_fds = count_open_fds();
	CHECK(baseline_fds > 0);
	for (int i = 0; i < 120; i++) {
		struct host_data_offer *ho = new_offer(&f);
		CHECK(ho != NULL);
		data_device_handle_enter(&f.seat, NULL, 1, NULL, 0, 0, ho->offer);
		CHECK(f.backend.incoming_drag != NULL);
		data_device_handle_leave(&f.seat, NULL);
		CHECK(f.local_seat->drag == NULL);
		CHECK(f.local_seat->drag_source == NULL);
		CHECK(f.backend.incoming_drag == NULL);
		CHECK(f.backend.incoming_offer == NULL);
		CHECK(wl_list_empty(&f.backend.dnd_offers));
	}
	CHECK(count_open_fds() <= baseline_fds + 2);
	fixture_finish(&f);
	return true;
}

static bool test_unfocused_drop(void) {
	struct fixture f;
	CHECK(fixture_init(&f));
	int baseline_fds = count_open_fds();
	CHECK(baseline_fds > 0);
	for (int i = 0; i < 120; i++) {
		struct host_data_offer *ho = new_offer(&f);
		CHECK(ho != NULL);
		data_device_handle_enter(&f.seat, NULL, 1, NULL, 0, 0, ho->offer);
		CHECK(f.backend.incoming_drag != NULL);
		data_device_handle_drop(&f.seat, NULL);
		CHECK(f.local_seat->drag == NULL);
		CHECK(f.local_seat->drag_source == NULL);
		CHECK(f.backend.incoming_drag == NULL);
		CHECK(f.backend.incoming_offer == NULL);
		CHECK(wl_list_empty(&f.backend.dnd_offers));
	}
	CHECK(count_open_fds() <= baseline_fds + 2);
	fixture_finish(&f);
	return true;
}

static bool test_source_destroy_before_drop(void) {
	struct fixture f;
	CHECK(fixture_init(&f));
	int baseline_fds = count_open_fds();
	CHECK(baseline_fds > 0);
	for (int i = 0; i < 120; i++) {
		struct host_data_offer *ho = new_offer(&f);
		CHECK(ho != NULL);
		data_device_handle_enter(&f.seat, NULL, 1, NULL, 0, 0, ho->offer);
		struct wlr_drag *drag = f.backend.incoming_drag;
		CHECK(drag != NULL);
		wlr_data_source_destroy(drag->source);
		CHECK(f.backend.incoming_drag == NULL);
		CHECK(f.backend.incoming_offer == NULL);
		CHECK(f.local_seat->drag == NULL);
		CHECK(wl_list_empty(&f.backend.dnd_offers));
		data_device_handle_leave(&f.seat, NULL);
	}
	CHECK(count_open_fds() <= baseline_fds + 2);
	fixture_finish(&f);
	return true;
}

static bool test_direct_drag_cancel(void) {
	struct fixture f;
	CHECK(fixture_init(&f));
	for (int i = 0; i < 120; i++) {
		struct host_data_offer *ho = new_offer(&f);
		CHECK(ho != NULL);
		data_device_handle_enter(&f.seat, NULL, 1, NULL, 0, 0, ho->offer);
		struct wlr_drag *drag = f.backend.incoming_drag;
		CHECK(drag != NULL);
		wlr_drag_destroy(drag);
		CHECK(f.backend.incoming_drag == NULL);
		CHECK(f.backend.incoming_offer == NULL);
		CHECK(f.local_seat->drag == NULL);
		CHECK(wl_list_empty(&f.backend.dnd_offers));
		// wlroots retains the idle source until replacement; it is harmless
		// after its host offer has been released.
		wlr_data_source_destroy(f.local_seat->drag_source);
		CHECK(f.local_seat->drag_source == NULL);
	}
	fixture_finish(&f);
	return true;
}

static bool test_drop_listener_destroys_drag(void) {
	struct fixture f;
	CHECK(fixture_init(&f));
	struct host_data_offer *ho = new_offer(&f);
	CHECK(ho != NULL);
	data_device_handle_enter(&f.seat, NULL, 1, NULL, 0, 0, ho->offer);
	struct wlr_drag *drag = f.backend.incoming_drag;
	CHECK(drag != NULL);
	struct proxy_data_source *proxy = (struct proxy_data_source *)drag->source;
	struct wlr_seat_client focus = {0};
	wl_list_init(&focus.data_devices);
	wl_signal_init(&focus.events.destroy);
	focus.seat = f.local_seat;
	struct wlr_surface fake_surface = {0};
	drag->focus_client = &focus;
	drag->focus = &fake_surface;
	wl_signal_add(&focus.events.destroy, &drag->seat_client_destroy);
	struct wl_listener drop_listener = { .notify = destroy_drag_during_drop };
	wl_signal_add(&drag->events.drop, &drop_listener);
	data_device_handle_drop(&f.seat, NULL);
	CHECK(f.local_seat->drag == NULL);
	CHECK(f.backend.incoming_drag == NULL);
	CHECK(proxy->host_offer == ho);
	proxy_source_dnd_finish(&proxy->base);
	CHECK(wl_list_empty(&f.backend.dnd_offers));
	wlr_data_source_destroy(&proxy->base);
	fixture_finish(&f);
	return true;
}

static bool test_forward_worker(void) {
	int baseline_fds = count_open_fds();
	CHECK(baseline_fds > 0);
	int source[2], dest[2];
	CHECK(pipe2(source, O_CLOEXEC) == 0);
	CHECK(pipe2(dest, O_CLOEXEC) == 0);
	char data[4096], actual[4096];
	for (size_t i = 0; i < sizeof(data); i++) data[i] = (char)(i % 251);
	CHECK(write(source[1], data, sizeof(data)) == sizeof(data));
	close(source[1]);
	struct send_forward_ctx *ctx = calloc(1, sizeof(*ctx));
	CHECK(ctx != NULL);
	ctx->src_fd = source[0];
	ctx->dst_fd = dest[1];
	ctx->mime = strdup("application/octet-stream");
	send_forward_worker(ctx);
	size_t received = 0;
	while (received < sizeof(actual)) {
		ssize_t n = read(dest[0], actual + received, sizeof(actual) - received);
		CHECK(n > 0);
		received += n;
	}
	CHECK(memcmp(data, actual, sizeof(data)) == 0);
	close(dest[0]);

	CHECK(pipe2(source, O_CLOEXEC) == 0);
	CHECK(pipe2(dest, O_CLOEXEC) == 0);
	CHECK(write(source[1], "closed-target", 13) == 13);
	close(source[1]);
	close(dest[0]);
	ctx = calloc(1, sizeof(*ctx));
	CHECK(ctx != NULL);
	ctx->src_fd = source[0];
	ctx->dst_fd = dest[1];
	ctx->mime = strdup("application/octet-stream");
	send_forward_worker(ctx);
	CHECK(count_open_fds() <= baseline_fds + 2);
	return true;
}

static bool test_outgoing_offer_endings(void) {
	struct fixture f;
	CHECK(fixture_init(&f));
	int baseline_fds = count_open_fds();
	CHECK(baseline_fds > 0);
	for (int i = 0; i < 120; i++) {
		struct host_data_offer *ho = new_offer(&f);
		CHECK(ho != NULL);
		struct wlr_wl_outgoing_drag *od = calloc(1, sizeof(*od));
		CHECK(od != NULL);
		od->backend = &f.backend;
		od->self_offer = ho;
		od->icon_fd = -1;
		f.backend.outgoing_drag = od;
		od->host_source = (struct wl_data_source *)wl_proxy_create(
			(struct wl_proxy *)f.backend.remote_display, &wl_data_source_interface);
		CHECK(od->host_source != NULL);
		if (i % 2 == 0) {
			outgoing_source_handle_cancelled(od, od->host_source);
		} else {
			outgoing_source_handle_dnd_finished(od, od->host_source);
		}
		CHECK(f.backend.outgoing_drag == NULL);
		CHECK(wl_list_empty(&f.backend.dnd_offers));
	}
	CHECK(count_open_fds() <= baseline_fds + 2);
	fixture_finish(&f);
	return true;
}

static bool test_outgoing_leave(void) {
	struct fixture f;
	CHECK(fixture_init(&f));
	int baseline_fds = count_open_fds();
	CHECK(baseline_fds > 0);
	for (int i = 0; i < 120; i++) {
		struct host_data_offer *ho = new_offer(&f);
		CHECK(ho != NULL);
		struct wlr_wl_outgoing_drag *od = calloc(1, sizeof(*od));
		CHECK(od != NULL);
		od->backend = &f.backend;
		od->self_offer = ho;
		od->icon_fd = -1;
		od->host_source = (struct wl_data_source *)wl_proxy_create(
			(struct wl_proxy *)f.backend.remote_display, &wl_data_source_interface);
		CHECK(od->host_source != NULL);
		struct wlr_drag *drag = wlr_drag_create_synthetic(f.local_seat, NULL);
		CHECK(drag != NULL);
		od->wlr_drag = drag;
		od->drag_destroy.notify = handle_outgoing_drag_destroy;
		wl_signal_add(&drag->events.destroy, &od->drag_destroy);
		f.backend.outgoing_drag = od;
		data_device_handle_leave(&f.seat, NULL);
		CHECK(od->self_offer == NULL);
		CHECK(wl_list_empty(&f.backend.dnd_offers));
		wlr_drag_destroy(drag);
		CHECK(f.backend.outgoing_drag == NULL);
	}
	CHECK(count_open_fds() <= baseline_fds + 2);
	fixture_finish(&f);
	return true;
}

int main(void) {
	if (!test_selection_offers() || !test_drop_receive_finish() ||
			!test_cancelled_drags() || !test_unfocused_drop() ||
			!test_source_destroy_before_drop() ||
			!test_direct_drag_cancel() || !test_drop_listener_destroys_drag() ||
			!test_outgoing_offer_endings() || !test_outgoing_leave() ||
			!test_forward_worker()) return 1;
	puts("PASS: 240 selection swaps, 2 focused drops with retained transfers, 480 incoming cancellations/unfocused drops, 240 outgoing endings/leaves, closed-pipe forwarding");
	return 0;
}
