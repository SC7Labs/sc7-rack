/* Exercise the real wlroots Wayland pointer, DnD, and output cursor callbacks.
 * The host transport is replaced only at the Wayland request boundary. This
 * makes an invalid wl_pointer.set_cursor serial observable without a desktop.
 */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <wayland-client.h>
#include <wlr/interfaces/wlr_pointer.h>
#include "backend/wayland.h"

static struct wlr_wl_output *test_output;
static struct wl_surface *test_surface;
static struct wl_surface *cursor_surface;
static int cursor_requests;
static uint32_t cursor_serial;
static int surface_commits;

static void record_cursor(struct wl_pointer *pointer, uint32_t serial,
		struct wl_surface *surface, int32_t hotspot_x, int32_t hotspot_y) {
	(void)pointer;
	(void)surface;
	(void)hotspot_x;
	(void)hotspot_y;
	if (serial == 0) {
		fprintf(stderr, "invalid wl_pointer.set_cursor serial 0\n");
		exit(1);
	}
	cursor_requests++;
	cursor_serial = serial;
}

static int record_flush(struct wl_display *display) {
	(void)display;
	return 0;
}

/* The callback code under test uses normal wlroots structures and signals.
 * Only the actual host protocol requests and surface lookup are shimmed. */
#define wl_proxy_get_tag(proxy) (&surface_tag)
#define wl_surface_get_user_data(surface) (test_output)
#define wl_compositor_create_surface(compositor) ((void)(compositor), cursor_surface)
#define wl_surface_attach(surface, buffer, x, y) ((void)0)
#define wl_surface_commit(surface) ((void)(surface_commits++))
#define wl_display_flush(display) record_flush(display)
#define wl_pointer_set_cursor(pointer, serial, surface, hot_x, hot_y) \
	record_cursor(pointer, serial, surface, hot_x, hot_y)

#ifndef OUTPUT_BACKEND_SOURCE
#define OUTPUT_BACKEND_SOURCE "backend/wayland/output.c"
#endif
#ifndef POINTER_BACKEND_SOURCE
#define POINTER_BACKEND_SOURCE "backend/wayland/pointer.c"
#endif
#ifndef DND_BACKEND_SOURCE
#define DND_BACKEND_SOURCE "backend/wayland/dnd.c"
#endif

#include OUTPUT_BACKEND_SOURCE
#include POINTER_BACKEND_SOURCE
#include DND_BACKEND_SOURCE

#undef wl_proxy_get_tag
#undef wl_surface_get_user_data
#undef wl_compositor_create_surface
#undef wl_surface_attach
#undef wl_surface_commit
#undef wl_display_flush
#undef wl_pointer_set_cursor

/* output_impl retains unused backend methods in this translation unit. Their
 * internal link dependencies are supplied here, but the exercised callbacks
 * and cursor assertion above remain the actual wlroots implementation. */
const struct wl_interface zwp_linux_buffer_params_v1_interface = {0};
const struct wl_interface wp_presentation_feedback_interface = {0};
const struct wl_interface wp_viewport_interface = {0};
const struct wl_interface xdg_surface_interface = {0};
const struct wl_interface xdg_toplevel_interface = {0};
const struct wl_interface zxdg_toplevel_decoration_v1_interface = {0};

int64_t get_current_time_msec(void) { return 1; }
enum wl_shm_format convert_drm_format_to_wl_shm(uint32_t fmt) {
	(void)fmt;
	return WL_SHM_FORMAT_ARGB8888;
}
bool output_pending_enabled(struct wlr_output *output,
		const struct wlr_output_state *state) {
	(void)output;
	(void)state;
	return false;
}
void output_defer_present(struct wlr_output *output,
		struct wlr_output_event_present event) {
	(void)output;
	(void)event;
}
bool renderer_bind_buffer(struct wlr_renderer *renderer, struct wlr_buffer *buffer) {
	(void)renderer;
	(void)buffer;
	return true;
}
struct wlr_wl_backend *get_wl_backend_from_backend(struct wlr_backend *backend) {
	(void)backend;
	return NULL;
}

struct counts {
	struct wl_listener button;
	struct wl_listener motion;
	int presses;
	int releases;
	int right_presses;
	int right_releases;
	int motions;
};

static void handle_button(struct wl_listener *listener, void *data) {
	struct counts *counts = wl_container_of(listener, counts, button);
	struct wlr_pointer_button_event *event = data;
	if (event->button != BTN_LEFT && event->button != BTN_RIGHT) {
		fprintf(stderr, "unexpected button 0x%x\n", event->button);
		exit(1);
	}
	if (event->state == WLR_BUTTON_PRESSED) {
		if (event->button == BTN_LEFT) {
			counts->presses++;
		} else {
			counts->right_presses++;
		}
	} else if (event->state == WLR_BUTTON_RELEASED) {
		if (event->button == BTN_LEFT) {
			counts->releases++;
		} else {
			counts->right_releases++;
		}
	} else {
		fprintf(stderr, "unexpected button state %u\n", event->state);
		exit(1);
	}
}

static void handle_motion(struct wl_listener *listener, void *data) {
	struct counts *counts = wl_container_of(listener, counts, motion);
	struct wlr_pointer_motion_absolute_event *event = data;
	if (event->x < 0 || event->x > 1 || event->y < 0 || event->y > 1) {
		fprintf(stderr, "invalid DnD motion %.3f, %.3f\n", event->x, event->y);
		exit(1);
	}
	counts->motions++;
}

static void check(bool condition, const char *message, int cycle) {
	if (!condition) {
		fprintf(stderr, "cycle %d: %s\n", cycle, message);
		exit(1);
	}
}

int main(int argc, char **argv) {
	int cycles = argc > 1 ? atoi(argv[1]) : 100;
	if (cycles < 100 || cycles > 10000) {
		fprintf(stderr, "cycles must be 100..10000\n");
		return 2;
	}

	struct wlr_wl_backend backend = {0};
	struct wlr_wl_seat seat = {0};
	struct wlr_wl_output output = {0};
	struct wlr_wl_pointer pointer = {0};
	struct wlr_wl_outgoing_drag outgoing = {0};
	struct counts counts = {0};
	int surface_token = 0;
	int cursor_token = 0;
	test_surface = (struct wl_surface *)&surface_token;
	cursor_surface = (struct wl_surface *)&cursor_token;
	test_output = &output;

	wl_list_init(&backend.seats);
	wl_list_init(&backend.dnd_offers);
	wl_list_init(&seat.pointers);
	wl_list_init(&seat.pressed_buttons);
	seat.name = "cursor-boundary-test";
	seat.backend = &backend;
	seat.wl_pointer = (struct wl_pointer *)&seat;
	wl_list_insert(&backend.seats, &seat.link);
	output.backend = &backend;
	output.wlr_output.impl = &output_impl;
	output.wlr_output.width = 800;
	output.wlr_output.height = 600;
	pointer.seat = &seat;
	pointer.output = &output;
	wl_list_insert(&seat.pointers, &pointer.link);
	outgoing.backend = &backend;
	outgoing.seat = &seat;
	outgoing.button = BTN_LEFT;
	backend.outgoing_drag = &outgoing;
	wl_signal_init(&pointer.wlr_pointer.events.button);
	wl_signal_init(&pointer.wlr_pointer.events.motion_absolute);
	counts.button.notify = handle_button;
	counts.motion.notify = handle_motion;
	wl_signal_add(&pointer.wlr_pointer.events.button, &counts.button);
	wl_signal_add(&pointer.wlr_pointer.events.motion_absolute, &counts.motion);

	for (int cycle = 0; cycle < cycles; cycle++) {
		uint32_t serial = (uint32_t)cycle * 10 + 1;
		pointer_handle_enter(&seat, seat.wl_pointer, serial, test_surface,
			wl_fixed_from_int(100), wl_fixed_from_int(100));
		check(output.enter_serial == serial &&
			output.cursor.pointer == &pointer, "pointer enter lost serial", cycle);
		check(cursor_requests == cycle * 2 + 1 && cursor_serial == serial,
			"enter did not apply cursor using valid serial", cycle);

		pointer_handle_button(&seat, seat.wl_pointer, serial + 1, serial,
			BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
		check(counts.presses == cycle + 1 && seat.button_is_down,
			"drag press was not forwarded", cycle);
		pointer_handle_leave(&seat, seat.wl_pointer, serial + 2, test_surface);
		check(output.enter_serial == 0 && output.cursor.pointer == NULL &&
			seat.active_pointer == NULL, "pointer leave retained cursor focus", cycle);

		/* A host DnD enter is distinct from wl_pointer.enter and carries no
		 * pointer-enter serial. This callback triggered the physical crash. */
		data_device_handle_enter(&seat, NULL, serial + 3, test_surface,
			wl_fixed_from_int(120), wl_fixed_from_int(130), NULL);
		check(seat.active_pointer == &pointer,
			"DnD enter did not restore pointer for drag motion", cycle);
		check(output_set_cursor(&output.wlr_output, NULL, cycle % 3, cycle % 5),
			"cursor update failed during drag", cycle);
		check(output.enter_serial == 0 && output.cursor.pointer == NULL,
			"DnD enter fabricated cursor focus without pointer serial", cycle);
		check(counts.motions == cycle * 2 + 1,
			"DnD enter motion was not forwarded", cycle);
		check(cursor_requests == cycle * 2 + 1,
			"cursor was sent without a valid pointer-enter serial", cycle);
		data_device_handle_motion(&seat, NULL, serial + 3,
			wl_fixed_from_int(140), wl_fixed_from_int(150));
		check(counts.motions == cycle * 2 + 2,
			"DnD motion stopped at boundary", cycle);
		data_device_handle_leave(&seat, NULL);
		wlr_wl_pointer_release_button(&seat, BTN_LEFT, serial + 3);
		check(counts.releases == cycle + 1 && !seat.button_is_down,
			"drag release remained stuck", cycle);

		pointer_handle_enter(&seat, seat.wl_pointer, serial + 4, test_surface,
			wl_fixed_from_int(160), wl_fixed_from_int(170));
		check(cursor_requests == cycle * 2 + 2 &&
			cursor_serial == serial + 4,
			"re-enter did not apply pending cursor with fresh serial", cycle);
		/* A menu button must work immediately after the completed drag. */
		pointer_handle_button(&seat, seat.wl_pointer, serial + 5, serial + 5,
			BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
		pointer_handle_button(&seat, seat.wl_pointer, serial + 6, serial + 6,
			BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED);
		check(counts.right_presses == cycle + 1 &&
			counts.right_releases == cycle + 1 && !seat.button_is_down,
			"right-click button stuck after drag", cycle);
		pointer_handle_leave(&seat, seat.wl_pointer, serial + 7, test_surface);
	}

	check(surface_commits == cycles, "cursor updates were not retained", cycles);
	printf("PASS: %d DnD boundary cycles; %d valid cursor requests; "
		"%d balanced drag buttons; %d balanced menu buttons; %d drag motions\n",
		cycles, cursor_requests, counts.releases,
		counts.right_releases, counts.motions);
	return 0;
}
