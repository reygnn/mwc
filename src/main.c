/*
 * mwc - a Motif-style Wayland compositor
 *
 * Server setup and output handling. Run it from a TTY for a real session,
 * or inside an existing Wayland/X11 session where wlroots opens a window.
 */
#define _POSIX_C_SOURCE 200809L
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include "mwc.h"

/* ---- outputs ------------------------------------------------------------ */

static void output_frame(struct wl_listener *listener, void *data)
{
	struct output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *so = wlr_scene_get_scene_output(
		output->server->scene, output->wlr_output);
	wlr_scene_output_commit(so, NULL);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(so, &now);
}

static void output_request_state(struct wl_listener *listener, void *data)
{
	/* Lets the nested backends resize their window. */
	struct output *output = wl_container_of(listener, output,
		request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data)
{
	struct output *output = wl_container_of(listener, output, destroy);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	wlr_scene_node_destroy(&output->background->node);
	if (output->wallpaper)
		wlr_scene_node_destroy(&output->wallpaper->node);
	free(output);
}

static void new_output(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode)
		wlr_output_state_set_mode(&state, mode);
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct output *output = calloc(1, sizeof(*output));
	output->server = server;
	output->wlr_output = wlr_output;
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	float c[4] = { theme.root.r, theme.root.g, theme.root.b, 1 };
	output->background = wlr_scene_rect_create(server->layer_bg, 0, 0, c);
	if (server->wallpaper)
		output->wallpaper = wlr_scene_buffer_create(server->layer_bg, NULL);
	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *lo =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	struct wlr_scene_output *so = wlr_scene_output_create(server->scene,
		wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, lo, so);
}

static void layout_change(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		layout_change);
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout,
			output->wlr_output, &box);
		wlr_scene_node_set_position(&output->background->node,
			box.x, box.y);
		wlr_scene_rect_set_size(output->background, box.width,
			box.height);
		wallpaper_place(output, &box);
	}
	icons_arrange(server);
}

/* ---- startup ------------------------------------------------------------ */

static bool in_path(const char *prog)
{
	const char *path = getenv("PATH");
	if (!path)
		return false;
	char *dirs = strdup(path), *save = NULL;
	bool found = false;
	for (char *d = strtok_r(dirs, ":", &save); d && !found;
			d = strtok_r(NULL, ":", &save)) {
		char buf[4096];
		snprintf(buf, sizeof(buf), "%s/%s", d, prog);
		found = access(buf, X_OK) == 0;
	}
	free(dirs);
	return found;
}

/* Terminals without client-side decorations first, so they get a frame. */
static const char *pick_terminal(void)
{
	const char *env = getenv("MWC_TERMINAL");
	if (env && *env)
		return env;
	static const char *candidates[] = {
		"xterm", "foot", "alacritty", "konsole", "kitty",
		"x-terminal-emulator", "ptyxis", "gnome-terminal",
	};
	for (size_t i = 0; i < sizeof(candidates) / sizeof(*candidates); i++) {
		if (in_path(candidates[i]))
			return candidates[i];
	}
	return "xterm";
}

static void run_autostart(const char *startup_cmd)
{
	if (startup_cmd) {
		spawn(startup_cmd);
		return;
	}
	const char *cfg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char path[4096];
	if (cfg && *cfg)
		snprintf(path, sizeof(path), "%s/mwc/autostart", cfg);
	else if (home)
		snprintf(path, sizeof(path), "%s/.config/mwc/autostart", home);
	else
		return;
	if (access(path, X_OK) == 0)
		spawn(path);
}

static void usage(const char *argv0)
{
	printf("Usage: %s [-s startup-command] [-d]\n"
		"\n"
		"  -s CMD   run CMD once the compositor is up\n"
		"  -d       debug logging\n"
		"\n"
		"Environment: MWC_TERMINAL, MWC_ACTIVE, MWC_INACTIVE, MWC_ROOT,\n"
		"MWC_WALLPAPER, MWC_FONT, XCURSOR_THEME, XCURSOR_SIZE,\n"
		"XKB_DEFAULT_LAYOUT.\n"
		"Without -s, ~/.config/mwc/autostart is run if executable.\n",
		argv0);
}

int main(int argc, char *argv[])
{
	const char *startup_cmd = NULL;
	enum wlr_log_importance log_level = WLR_INFO;
	int c;
	while ((c = getopt(argc, argv, "s:dh")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		case 'd':
			log_level = WLR_DEBUG;
			break;
		default:
			usage(argv[0]);
			return c == 'h' ? 0 : 1;
		}
	}
	wlr_log_init(log_level, NULL);
	theme_init();

	struct server server = { 0 };
	server.terminal = pick_terminal();
	wallpaper_init(&server);
	server.display = wl_display_create();
	server.event_loop = wl_display_get_event_loop(server.display);
	server.backend = wlr_backend_autocreate(server.event_loop,
		&server.session);
	if (!server.backend) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (!server.renderer) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.display);
	server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);
	if (!server.allocator) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	server.compositor = wlr_compositor_create(server.display, 6,
		server.renderer);
	wlr_subcompositor_create(server.display);
	wlr_data_device_manager_create(server.display);
	wlr_primary_selection_v1_device_manager_create(server.display);
	wlr_viewporter_create(server.display);
	wlr_single_pixel_buffer_manager_v1_create(server.display);
	wlr_presentation_create(server.display, server.backend, 2);
	wlr_screencopy_manager_v1_create(server.display);

	server.output_layout = wlr_output_layout_create(server.display);
	wlr_xdg_output_manager_v1_create(server.display, server.output_layout);
	wl_list_init(&server.outputs);
	server.new_output.notify = new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);
	server.layout_change.notify = layout_change;
	wl_signal_add(&server.output_layout->events.change,
		&server.layout_change);

	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene,
		server.output_layout);
	server.layer_bg = wlr_scene_tree_create(&server.scene->tree);
	server.layer_icons = wlr_scene_tree_create(&server.scene->tree);
	server.layer_views = wlr_scene_tree_create(&server.scene->tree);
	server.layer_unmanaged = wlr_scene_tree_create(&server.scene->tree);
	server.layer_menu = wlr_scene_tree_create(&server.scene->tree);

	wl_list_init(&server.views);
	xdg_init(&server);
	input_init(&server);
	testhook_init(&server);
	xwayland_init(&server);

	const char *socket = wl_display_add_socket_auto(server.display);
	if (!socket) {
		wlr_log(WLR_ERROR, "failed to create a Wayland socket");
		return 1;
	}
	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "failed to start the backend");
		return 1;
	}
	setenv("WAYLAND_DISPLAY", socket, true);
	setenv("XDG_CURRENT_DESKTOP", "mwc", true);
	/* Ask toolkits for server-side decorations where they support it. */
	setenv("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1", true);
	wlr_log(WLR_INFO, "mwc running on WAYLAND_DISPLAY=%s, terminal: %s",
		socket, server.terminal);

	run_autostart(startup_cmd);
	wl_display_run(server.display);

	/* wlroots asserts that nobody listens to an object it destroys. */
	wl_display_destroy_clients(server.display);
	testhook_fini();
	if (server.xwayland) {
		wl_list_remove(&server.xwayland_ready.link);
		wl_list_remove(&server.new_xwayland_surface.link);
		wlr_xwayland_destroy(server.xwayland);
	}
	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_popup.link);
	wl_list_remove(&server.new_xdg_decoration.link);
	wl_list_remove(&server.new_server_decoration.link);
	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);
	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.request_cursor.link);
	wl_list_remove(&server.request_set_selection.link);
	wl_list_remove(&server.request_set_primary_selection.link);
	wl_list_remove(&server.new_output.link);
	wl_list_remove(&server.layout_change.link);
	/* The backend goes first: output_destroy() still needs the scene. */
	wlr_backend_destroy(server.backend);
	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wl_display_destroy(server.display);
	wallpaper_fini(&server);
	return 0;
}
