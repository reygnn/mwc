/*
 * Switches all outputs off or on, as tools like wlopm or displayOff do.
 *
 *   powerc off|on
 */
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
#include "wlr-output-power-management-unstable-v1-client.h"

#define MAX_OUTPUTS 8

static struct zwlr_output_power_manager_v1 *manager;
static struct wl_output *outputs[MAX_OUTPUTS];
static int n_outputs;

static void global(void *data, struct wl_registry *registry, uint32_t name,
	const char *interface, uint32_t version)
{
	if (!strcmp(interface, zwlr_output_power_manager_v1_interface.name))
		manager = wl_registry_bind(registry, name,
			&zwlr_output_power_manager_v1_interface, 1);
	else if (!strcmp(interface, wl_output_interface.name) && n_outputs < MAX_OUTPUTS)
		outputs[n_outputs++] = wl_registry_bind(registry, name,
			&wl_output_interface, 1);
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

int main(int argc, char **argv)
{
	if (argc != 2 || (strcmp(argv[1], "off") && strcmp(argv[1], "on"))) {
		fprintf(stderr, "usage: powerc off|on\n");
		return 2;
	}
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "powerc: can't connect\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!manager) {
		fprintf(stderr, "powerc: no output power manager\n");
		return 1;
	}
	uint32_t mode = strcmp(argv[1], "on") ? ZWLR_OUTPUT_POWER_V1_MODE_OFF :
		ZWLR_OUTPUT_POWER_V1_MODE_ON;
	for (int i = 0; i < n_outputs; i++) {
		struct zwlr_output_power_v1 *power =
			zwlr_output_power_manager_v1_get_output_power(manager, outputs[i]);
		zwlr_output_power_v1_set_mode(power, mode);
	}
	wl_display_roundtrip(display);
	wl_display_disconnect(display);
	return 0;
}
