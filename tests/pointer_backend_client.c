/* Exercise the actual wlroots Wayland backend pointer callbacks without a GUI.
 * Including the backend translation unit keeps this test tied to its real
 * enter/leave/button implementation, including its static callbacks.
 */
#define _POSIX_C_SOURCE 200809L

#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef POINTER_BACKEND_SOURCE
#define POINTER_BACKEND_SOURCE "backend/wayland/pointer.c"
#endif
#include POINTER_BACKEND_SOURCE

static struct wlr_wl_output *test_output;
static struct wl_surface *test_surface;

struct wlr_wl_output *get_wl_output_from_surface(struct wlr_wl_backend *backend,
		struct wl_surface *surface) {
	(void)backend;
	return surface == test_surface ? test_output : NULL;
}

void update_wl_output_cursor(struct wlr_wl_output *output) {
	(void)output;
}

struct counts {
	struct wl_listener button;
	struct wl_listener motion;
	int presses;
	int releases;
	int motions;
};

static void handle_button(struct wl_listener *listener, void *data) {
	struct counts *counts = wl_container_of(listener, counts, button);
	struct wlr_pointer_button_event *event = data;
	if (event->button != BTN_RIGHT) {
		fprintf(stderr, "unexpected button 0x%x\n", event->button);
		exit(1);
	}
	if (event->state == WLR_BUTTON_PRESSED) {
		counts->presses++;
	} else if (event->state == WLR_BUTTON_RELEASED) {
		counts->releases++;
	} else {
		fprintf(stderr, "unexpected button state %u\n", event->state);
		exit(1);
	}
}

static void handle_motion(struct wl_listener *listener, void *data) {
	struct counts *counts = wl_container_of(listener, counts, motion);
	struct wlr_pointer_motion_absolute_event *event = data;
	if (event->x < 0 || event->x > 1 || event->y < 0 || event->y > 1) {
		fprintf(stderr, "unexpected motion position %.3f, %.3f\n",
			event->x, event->y);
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
	int cycles = 100;
	if (argc > 1) {
		cycles = atoi(argv[1]);
	}
	if (cycles < 1 || cycles > 10000) {
		fprintf(stderr, "cycles must be 1..10000\n");
		return 2;
	}

	struct wlr_wl_backend backend = {0};
	struct wlr_wl_seat seat = {0};
	struct wlr_wl_output output = {0};
	struct wlr_wl_pointer pointer = {0};
	struct counts counts = {0};
	int surface_token = 0;
	test_surface = (struct wl_surface *)&surface_token;
	test_output = &output;

	wl_list_init(&backend.seats);
	wl_list_init(&seat.pointers);
	wl_list_init(&seat.pressed_buttons);
	seat.name = "pointer-test";
	seat.backend = &backend;
	seat.wl_pointer = (struct wl_pointer *)&seat;
	wl_list_insert(&backend.seats, &seat.link);
	output.backend = &backend;
	output.wlr_output.width = 800;
	output.wlr_output.height = 600;
	pointer.seat = &seat;
	pointer.output = &output;
	wl_list_insert(&seat.pointers, &pointer.link);
	wl_signal_init(&pointer.wlr_pointer.events.button);
	wl_signal_init(&pointer.wlr_pointer.events.motion_absolute);
	counts.button.notify = handle_button;
	counts.motion.notify = handle_motion;
	wl_signal_add(&pointer.wlr_pointer.events.button, &counts.button);
	wl_signal_add(&pointer.wlr_pointer.events.motion_absolute, &counts.motion);

	for (int cycle = 0; cycle < cycles; cycle++) {
		uint32_t serial = (uint32_t)cycle * 4 + 1;
		pointer_handle_enter(&seat, seat.wl_pointer, serial, test_surface,
			wl_fixed_from_int(100), wl_fixed_from_int(100));
		check(seat.active_pointer == &pointer, "enter lost pointer", cycle);
		check(output.cursor.pointer == &pointer, "enter lost cursor", cycle);
		pointer_handle_motion(&seat, seat.wl_pointer, serial,
			wl_fixed_from_int(120), wl_fixed_from_int(120));
		check(counts.motions == cycle + 1, "motion was not forwarded", cycle);

		pointer_handle_button(&seat, seat.wl_pointer, serial + 1, serial,
			BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
		check(counts.presses == cycle + 1, "press was not forwarded", cycle);
		pointer_handle_leave(&seat, seat.wl_pointer, serial + 2, test_surface);
		check(seat.active_pointer == NULL, "leave retained active pointer", cycle);
		check(output.cursor.pointer == NULL, "leave retained cursor pointer", cycle);
		pointer_handle_button(&seat, seat.wl_pointer, serial + 3, serial,
			BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED);
		check(counts.releases == cycle + 1,
			"release after leave was not forwarded exactly once", cycle);
		check(!seat.button_is_down, "button stayed down", cycle);
	}

#ifndef TEST_PREVIOUS_BACKEND
	/* DnD finish and cancel both call this shared release helper when the host
	 * drag consumes its pointer release. The later host release must be inert.
	 */
	for (int cycle = 0; cycle < cycles; cycle++) {
		uint32_t serial = (uint32_t)(cycles + cycle) * 4 + 1;
		pointer_handle_enter(&seat, seat.wl_pointer, serial, test_surface,
			wl_fixed_from_int(100), wl_fixed_from_int(100));
		pointer_handle_button(&seat, seat.wl_pointer, serial + 1, serial,
			BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
		check(counts.presses == cycles + cycle + 1,
			"drag press was not forwarded", cycle);
		pointer_handle_leave(&seat, seat.wl_pointer, serial + 2, test_surface);
		wlr_wl_pointer_release_button(&seat, BTN_RIGHT, serial);
		check(counts.releases == cycles + cycle + 1,
			"drag finish/cancel did not balance press", cycle);
		pointer_handle_button(&seat, seat.wl_pointer, serial + 3, serial,
			BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED);
		check(counts.releases == cycles + cycle + 1,
			"late host release caused duplicate release", cycle);
		check(!seat.button_is_down, "drag button stayed down", cycle);
	}
#endif

	printf("PASS: %d press/leave/release cycles; %d drag finish/cancel cycles; "
		"%d motions\n", cycles,
#ifdef TEST_PREVIOUS_BACKEND
		0,
#else
		cycles,
#endif
		counts.motions);
	return 0;
}
