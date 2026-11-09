/*
 * Sets or shows the outputs through wlr-output-management, as wlr-randr does, or a VNC
 * or RDP server that makes the output as large as the viewer's window.
 *
 *   outputc list
 *   outputc apply|test NAME [WIDTHxHEIGHT[@HZ]] [at X,Y] [scale S] [off|on]
 *
 * list prints one line per output: NAME on=1 w=1280 h=720 x=0 y=0 scale=100.
 * apply and test leave the other outputs as they are. Exits 0 if mwc took it, 1 if
 * it refused, the request came too late or nothing came within five seconds, 2 on a
 * usage error.
 */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-output-management-unstable-v1-client.h"

#define MAX_HEADS 8
#define MAX_MODES 64

struct mode {
	struct zwlr_output_mode_v1 *mode;
	int width, height, refresh;
};

struct head {
	struct zwlr_output_head_v1 *head;
	char name[64];
	bool enabled;
	struct mode *current;
	int x, y;
	double scale;
};

static struct zwlr_output_manager_v1 *manager;
static struct head heads[MAX_HEADS];
static int n_heads;
static struct mode modes[MAX_MODES];
static int n_modes;
static uint32_t serial;
static bool done;

static void mode_size(void *data, struct zwlr_output_mode_v1 *m, int32_t w, int32_t h)
{
	struct mode *mode = data;
	mode->width = w;
	mode->height = h;
}

static void mode_refresh(void *data, struct zwlr_output_mode_v1 *m, int32_t refresh)
{
	struct mode *mode = data;
	mode->refresh = refresh;
}

static void mode_preferred(void *data, struct zwlr_output_mode_v1 *m)
{
}

static void mode_finished(void *data, struct zwlr_output_mode_v1 *m)
{
}

static const struct zwlr_output_mode_v1_listener mode_listener = {
	mode_size, mode_refresh, mode_preferred, mode_finished,
};

static void head_name(void *data, struct zwlr_output_head_v1 *h, const char *name)
{
	struct head *head = data;
	snprintf(head->name, sizeof(head->name), "%s", name);
}

static void head_description(void *data, struct zwlr_output_head_v1 *h, const char *d)
{
}

static void head_physical_size(void *data, struct zwlr_output_head_v1 *h, int32_t w,
	int32_t height)
{
}

static void head_mode(void *data, struct zwlr_output_head_v1 *h,
	struct zwlr_output_mode_v1 *m)
{
	if (n_modes == MAX_MODES) {
		zwlr_output_mode_v1_destroy(m);
		return;
	}
	struct mode *mode = &modes[n_modes++];
	mode->mode = m;
	zwlr_output_mode_v1_add_listener(m, &mode_listener, mode);
}

static void head_enabled(void *data, struct zwlr_output_head_v1 *h, int32_t enabled)
{
	struct head *head = data;
	head->enabled = enabled;
	if (!enabled)
		head->current = NULL;
}

static void head_current_mode(void *data, struct zwlr_output_head_v1 *h,
	struct zwlr_output_mode_v1 *m)
{
	struct head *head = data;
	for (int i = 0; i < n_modes; i++) {
		if (modes[i].mode == m)
			head->current = &modes[i];
	}
}

static void head_position(void *data, struct zwlr_output_head_v1 *h, int32_t x, int32_t y)
{
	struct head *head = data;
	head->x = x;
	head->y = y;
}

static void head_transform(void *data, struct zwlr_output_head_v1 *h, int32_t t)
{
}

static void head_scale(void *data, struct zwlr_output_head_v1 *h, wl_fixed_t scale)
{
	struct head *head = data;
	head->scale = wl_fixed_to_double(scale);
}

static void head_finished(void *data, struct zwlr_output_head_v1 *h)
{
}

static void head_string(void *data, struct zwlr_output_head_v1 *h, const char *s)
{
}

static void head_adaptive_sync(void *data, struct zwlr_output_head_v1 *h, uint32_t state)
{
}

static const struct zwlr_output_head_v1_listener head_listener = {
	.name = head_name,
	.description = head_description,
	.physical_size = head_physical_size,
	.mode = head_mode,
	.enabled = head_enabled,
	.current_mode = head_current_mode,
	.position = head_position,
	.transform = head_transform,
	.scale = head_scale,
	.finished = head_finished,
	.make = head_string,
	.model = head_string,
	.serial_number = head_string,
	.adaptive_sync = head_adaptive_sync,
};

static void manager_head(void *data, struct zwlr_output_manager_v1 *m,
	struct zwlr_output_head_v1 *h)
{
	if (n_heads == MAX_HEADS) {
		zwlr_output_head_v1_destroy(h);
		return;
	}
	struct head *head = &heads[n_heads++];
	head->head = h;
	head->scale = 1;
	zwlr_output_head_v1_add_listener(h, &head_listener, head);
}

static void manager_done(void *data, struct zwlr_output_manager_v1 *m, uint32_t s)
{
	serial = s;
	done = true;
}

static void manager_finished(void *data, struct zwlr_output_manager_v1 *m)
{
}

static const struct zwlr_output_manager_v1_listener manager_listener = {
	manager_head, manager_done, manager_finished,
};

static void global(void *data, struct wl_registry *registry, uint32_t name,
	const char *interface, uint32_t version)
{
	if (!strcmp(interface, zwlr_output_manager_v1_interface.name))
		manager = wl_registry_bind(registry, name,
			&zwlr_output_manager_v1_interface, version < 4 ? version : 4);
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

static int result = -1;

static void config_succeeded(void *data, struct zwlr_output_configuration_v1 *c)
{
	result = 0;
}

static void config_failed(void *data, struct zwlr_output_configuration_v1 *c)
{
	fprintf(stderr, "outputc: refused\n");
	result = 1;
}

static void config_cancelled(void *data, struct zwlr_output_configuration_v1 *c)
{
	fprintf(stderr, "outputc: cancelled, the outputs changed meanwhile\n");
	result = 1;
}

static const struct zwlr_output_configuration_v1_listener config_listener = {
	config_succeeded, config_failed, config_cancelled,
};

static void timed_out(int sig)
{
	static const char msg[] = "outputc: no answer within five seconds\n";
	if (write(2, msg, sizeof(msg) - 1) < 0)
		_exit(1);
	_exit(1);
}

static int usage(void)
{
	fprintf(stderr, "usage: outputc list\n"
		"       outputc apply|test NAME [WxH[@HZ]] [at X,Y] [scale S] [off|on]\n");
	return 2;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return usage();
	bool list = !strcmp(argv[1], "list");
	bool apply = !strcmp(argv[1], "apply");
	if (!list && !apply && strcmp(argv[1], "test"))
		return usage();
	if (!list && argc < 3)
		return usage();
	signal(SIGALRM, timed_out);
	alarm(5);

	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "outputc: can't connect\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!manager) {
		fprintf(stderr, "outputc: no output manager\n");
		return 1;
	}
	zwlr_output_manager_v1_add_listener(manager, &manager_listener, NULL);
	while (!done && wl_display_dispatch(display) != -1)
		;

	if (list) {
		for (int i = 0; i < n_heads; i++) {
			struct head *h = &heads[i];
			printf("%s on=%d w=%d h=%d x=%d y=%d scale=%d\n", h->name, h->enabled,
				h->current ? h->current->width : 0,
				h->current ? h->current->height : 0, h->x, h->y,
				(int)(h->scale * 100 + 0.5));
		}
		wl_display_disconnect(display);
		return 0;
	}

	struct head *target = NULL;
	for (int i = 0; i < n_heads; i++) {
		if (!strcmp(heads[i].name, argv[2]))
			target = &heads[i];
	}
	if (!target) {
		fprintf(stderr, "outputc: no output %s\n", argv[2]);
		return 1;
	}
	struct zwlr_output_configuration_v1 *config =
		zwlr_output_manager_v1_create_configuration(manager, serial);
	zwlr_output_configuration_v1_add_listener(config, &config_listener, NULL);
	bool off = false;
	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "off"))
			off = true;
		else if (!strcmp(argv[i], "on"))
			off = false;
	}
	for (int i = 0; i < n_heads; i++) {
		struct head *h = &heads[i];
		if (h == target && off) {
			zwlr_output_configuration_v1_disable_head(config, h->head);
			continue;
		}
		if (h != target && !h->enabled) {
			zwlr_output_configuration_v1_disable_head(config, h->head);
			continue;
		}
		struct zwlr_output_configuration_head_v1 *ch =
			zwlr_output_configuration_v1_enable_head(config, h->head);
		if (h != target)
			continue;
		for (int a = 3; a < argc; a++) {
			int w, height, x, y, consumed = 0;
			double hz = 0, scale;
			if (!strcmp(argv[a], "at") && a + 1 < argc &&
					sscanf(argv[a + 1], "%d,%d", &x, &y) == 2) {
				zwlr_output_configuration_head_v1_set_position(ch, x, y);
				a++;
			} else if (!strcmp(argv[a], "scale") && a + 1 < argc &&
					sscanf(argv[a + 1], "%lf", &scale) == 1) {
				zwlr_output_configuration_head_v1_set_scale(ch,
					wl_fixed_from_double(scale));
				a++;
			} else if (sscanf(argv[a], "%dx%d%n", &w, &height, &consumed) == 2) {
				if (argv[a][consumed])
					sscanf(argv[a] + consumed, "@%lf", &hz);
				zwlr_output_configuration_head_v1_set_custom_mode(ch, w, height,
					(int)(hz * 1000));
			} else if (strcmp(argv[a], "on")) {
				return usage();
			}
		}
	}
	if (apply)
		zwlr_output_configuration_v1_apply(config);
	else
		zwlr_output_configuration_v1_test(config);
	while (result < 0 && wl_display_dispatch(display) != -1)
		;
	wl_display_disconnect(display);
	return result < 0 ? 1 : result;
}
