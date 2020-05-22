#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <linux/input-event-codes.h>

#include "shm.h"

static int width;
static int height;
void *mf_data;
volatile int mf_update = 0;
volatile int on_top = 1;
struct wl_display *display;
int stride;
int size;

static struct wl_shm *shm = NULL;
static struct wl_compositor *compositor = NULL;

static void *shm_data = NULL;
static struct wl_surface *surface = NULL;
static struct wl_shell *shell = NULL;

static void noop() {
	// This space intentionally left blank
}

static void wl_shell_surface_handle_ping(void *data,
		struct wl_shell_surface *shell_surface, uint32_t serial) {
	wl_shell_surface_pong(shell_surface, serial);
}

static const struct wl_shell_surface_listener shell_surface_listener = {
	.ping = wl_shell_surface_handle_ping,
	.configure = noop,
	.popup_done = noop,
};

static void keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
	on_top = 1;
}
static void keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, struct wl_surface *surface) {
	on_top = 0;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = noop,
	.enter = keyboard_handle_enter,
	.leave = keyboard_handle_leave,
	.key = noop,
	.modifiers = noop,
};

static void seat_handle_capabilities(void *data, struct wl_seat *seat,
		uint32_t capabilities) {
	if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
		struct wl_keyboard *keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	}
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct wl_seat *seat =
			wl_registry_bind(registry, name, &wl_seat_interface, 1);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 1);
	} else if (strcmp(interface, wl_shell_interface.name) == 0) {
		shell = wl_registry_bind(registry, name, &wl_shell_interface, 1);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	// Who cares
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static struct wl_buffer *create_buffer() {
	int fd = create_shm_file(size);
	if (fd < 0) {
		fprintf(stderr, "creating a buffer file for %d B failed: %m\n", size);
		return NULL;
	}

	shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (shm_data == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %m\n");
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
		stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);

	memcpy(shm_data, mf_data, size);
	return buffer;
}

void update(int signum)
{
  if (on_top == 0) write(STDOUT_FILENO, "0", 1);
  else mf_update = 1;
}

void redraw(void *data, struct wl_callback *callback, uint32_t time);
const struct wl_callback_listener frame_listener = { redraw };
void redraw(void *data, struct wl_callback *callback, uint32_t time)
{
    wl_callback_destroy(callback);
    if (mf_update) {
      mf_update = 0;
      memcpy(shm_data, mf_data, size);
      wl_surface_damage(surface, 0, 0, width, height);
      write(STDOUT_FILENO, "1", 1);
    }
    wl_callback_add_listener(wl_surface_frame(surface), &frame_listener, NULL);
    wl_surface_commit(surface);
}

int main(int argc, char *argv[]) {
	sscanf(getenv("SCREEN_SIZE"), "%dx%d", &width, &height);
	stride = width * 4;
	size = stride * height;

	struct sigaction sa;

	sa.sa_handler = update;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &sa, NULL);

	mf_data = mmap(NULL, size, PROT_READ, MAP_SHARED, STDIN_FILENO, 0);
	if (mf_data == MAP_FAILED) exit(1);

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to create display\n");
		return EXIT_FAILURE;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (shm == NULL || compositor == NULL) {
		fprintf(stderr, "no wl_shm or wl_compositor\n");
		return EXIT_FAILURE;
	}

	struct wl_buffer *buffer = create_buffer();
	if (buffer == NULL) {
		return EXIT_FAILURE;
	}

	surface = wl_compositor_create_surface(compositor);
	struct wl_shell_surface *shell_surface = wl_shell_get_shell_surface(shell, surface);
	wl_shell_surface_set_fullscreen(shell_surface, WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT, 0, NULL);

	wl_shell_surface_add_listener(shell_surface, &shell_surface_listener, NULL);

	wl_display_roundtrip(display);

	wl_surface_attach(surface, buffer, 0, 0);
	wl_callback_add_listener(wl_surface_frame(surface), &frame_listener, NULL);
	wl_surface_commit(surface);

	write(STDOUT_FILENO, "", 1);

	while (wl_display_dispatch(display) != -1) {
		// This space intentionally left blank
	}

	wl_shell_surface_destroy(shell_surface);
	wl_surface_destroy(surface);
	wl_buffer_destroy(buffer);

	return EXIT_SUCCESS;
}
