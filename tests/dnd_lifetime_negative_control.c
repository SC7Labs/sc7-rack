// Show that the previous patched wlroots really retained ignored selection
// offers and left a focused incoming drag active after the host drop.
#include SC7_DND_SOURCE_PATH

#include <sys/socket.h>

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

int main(void) {
	int sockets[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) return 1;
	struct wlr_wl_backend backend = {0};
	backend.remote_display = wl_display_connect_to_fd(sockets[0]);
	backend.local_display = wl_display_create();
	if (!backend.remote_display || !backend.local_display) return 2;
	wl_list_init(&backend.dnd_offers);
	wl_list_init(&backend.outputs);
	wl_list_init(&backend.seats);
	struct wlr_wl_seat host_seat = { .backend = &backend };
	wl_list_init(&host_seat.pointers);

	for (int i = 0; i < 120; i++) {
		struct wl_data_offer *offer = (struct wl_data_offer *)wl_proxy_create(
			(struct wl_proxy *)backend.remote_display, &wl_data_offer_interface);
		if (!offer) return 3;
		data_device_handle_data_offer(&host_seat, NULL, offer);
		data_device_handle_selection(&host_seat, NULL, offer);
	}
	int live = 0;
	struct host_data_offer *ho;
	wl_list_for_each(ho, &backend.dnd_offers, link) live++;
	if (live != 120) return 4;

	struct wlr_seat *local_seat = wlr_seat_create(backend.local_display, "old-dnd-test");
	if (!local_seat) return 5;
	struct wl_data_offer *offer = (struct wl_data_offer *)wl_proxy_create(
		(struct wl_proxy *)backend.remote_display, &wl_data_offer_interface);
	if (!offer) return 6;
	data_device_handle_data_offer(&host_seat, NULL, offer);
	wl_list_for_each(ho, &backend.dnd_offers, link) {
		if (ho->offer == offer) {
			offer_handle_offer(ho, offer, "text/plain");
			break;
		}
	}
	data_device_handle_enter(&host_seat, NULL, 1, NULL, 0, 0, offer);
	struct wlr_drag *drag = backend.incoming_drag;
	if (!drag) return 7;
	struct wlr_seat_client focus = {0};
	wl_list_init(&focus.data_devices);
	wl_signal_init(&focus.events.destroy);
	focus.seat = local_seat;
	struct wlr_surface fake_surface = {0};
	drag->focus_client = &focus;
	drag->focus = &fake_surface;
	wl_signal_add(&focus.events.destroy, &drag->seat_client_destroy);
	data_device_handle_drop(&host_seat, NULL);
	if (local_seat->drag != drag) return 8;
	if (backend.incoming_drag != NULL) return 9;
	puts("NEGATIVE CONTROL: 120 ignored offers retained; focused host drop left seat drag active");

	wlr_drag_destroy(drag);
	wlr_data_source_destroy(local_seat->drag_source);
	finish_seat_dnd(&host_seat);
	wlr_seat_destroy(local_seat);
	wl_display_destroy(backend.local_display);
	wl_display_disconnect(backend.remote_display);
	close(sockets[1]);
	return 0;
}
