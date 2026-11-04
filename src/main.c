/*
 * mwc - a Motif-style Wayland compositor
 *
 * Server setup and output handling. Run it from a TTY for a real session,
 * or inside an existing Wayland/X11 session where wlroots opens a window.
 */
#define _POSIX_C_SOURCE 200809L
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_power_management_v1.h>
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

/* A client asks to switch a monitor off or on. The output stays in the layout, so
 * windows keep their places, and any client can ask: that is what the protocol is. */
static void output_power_set_mode(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		output_power_set_mode);
	struct wlr_output_power_v1_set_mode_event *event = data;
	bool on = event->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON;
	if (event->output->enabled == on)
		return;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, on);
	bool ok = wlr_output_commit_state(event->output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		wlr_log(WLR_ERROR, "can't switch output %s %s", event->output->name,
			on ? "on" : "off");
		return;
	}
	if (!on)
		return;
	wlr_output_schedule_frame(event->output);
	/* The output lost its cursor. Setting the same image again would do nothing. */
	if (server->cursor_name) {
		wlr_cursor_unset_image(server->cursor);
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
			server->cursor_name);
	}
	input_pointer_changed_soon(server);
}

/* One per inhibitor; while any exists, nobody counts as idle. */
struct idle_inhibitor {
	struct wl_listener destroy;
	struct server *server;
};

static void idle_inhibitor_destroy(struct wl_listener *listener, void *data)
{
	struct idle_inhibitor *inhibitor = wl_container_of(listener, inhibitor, destroy);
	struct server *server = inhibitor->server;
	/* It is still in the list while it says that it goes. */
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier,
		wl_list_length(&server->idle_inhibit->inhibitors) > 1);
	wl_list_remove(&inhibitor->destroy.link);
	free(inhibitor);
}

static void new_idle_inhibitor(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, new_idle_inhibitor);
	struct wlr_idle_inhibitor_v1 *wlr_inhibitor = data;
	struct idle_inhibitor *inhibitor = calloc(1, sizeof(*inhibitor));
	inhibitor->server = server;
	inhibitor->destroy.notify = idle_inhibitor_destroy;
	wl_signal_add(&wlr_inhibitor->events.destroy, &inhibitor->destroy);
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, true);
}

static void output_destroy(struct wl_listener *listener, void *data)
{
	struct output *output = wl_container_of(listener, output, destroy);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	layer_output_gone(output);
	wlr_scene_node_destroy(&output->background->node);
	if (output->wallpaper)
		wlr_scene_node_destroy(&output->wallpaper->node);
	free(output);
}

/* Mwm*NAME*outputMode: WIDTHxHEIGHT or WIDTHxHEIGHT@HZ. The highest refresh rate if
 * none is given. A backend without a mode list, nested or headless, takes any size. */
static bool configure_mode(struct wlr_output *o, struct wlr_output_state *state)
{
	const char *value = config_get_for(o->name, "outputMode");
	if (!value)
		return false;
	int w, h, consumed = 0;
	double hz = 0;
	if (sscanf(value, "%dx%d%n", &w, &h, &consumed) != 2 || w <= 0 || h <= 0 ||
			(value[consumed] && (sscanf(value + consumed, "@%lf", &hz) != 1 ||
			hz <= 0))) {
		wlr_log(WLR_ERROR, "ignoring outputMode \"%s\" for %s: expected "
			"WIDTHxHEIGHT or WIDTHxHEIGHT@HZ", value, o->name);
		return false;
	}
	if (wl_list_empty(&o->modes)) {
		wlr_output_state_set_custom_mode(state, w, h, (int)(hz * 1000));
		return true;
	}
	struct wlr_output_mode *best = NULL, *m;
	wl_list_for_each(m, &o->modes, link) {
		if (m->width != w || m->height != h)
			continue;
		/* Within half a Hz: 59.94 is what most people mean by 60. */
		if (hz > 0 && abs(m->refresh - (int)(hz * 1000)) > 500)
			continue;
		if (!best || m->refresh > best->refresh)
			best = m;
	}
	if (!best) {
		wlr_log(WLR_ERROR, "ignoring outputMode \"%s\": %s has no such mode",
			value, o->name);
		return false;
	}
	wlr_output_state_set_mode(state, best);
	return true;
}

static void configure_scale(struct wlr_output *o, struct wlr_output_state *state)
{
	const char *value = config_get_for(o->name, "outputScale");
	char *end;
	double scale = value ? strtod(value, &end) : 1;
	/* Written so that NaN fails it too. Below this the layout overflows. */
	if (value && (end == value || *end || !(scale >= 0.1 && scale <= 10))) {
		wlr_log(WLR_ERROR, "ignoring outputScale \"%s\" for %s", value, o->name);
		scale = 1;
	}
	wlr_output_state_set_scale(state, scale);
}

static void configure_transform(struct wlr_output *o, struct wlr_output_state *state)
{
	static const struct {
		const char *name;
		enum wl_output_transform transform;
	} table[] = {
		{ "normal", WL_OUTPUT_TRANSFORM_NORMAL },
		{ "90", WL_OUTPUT_TRANSFORM_90 },
		{ "180", WL_OUTPUT_TRANSFORM_180 },
		{ "270", WL_OUTPUT_TRANSFORM_270 },
		{ "flipped", WL_OUTPUT_TRANSFORM_FLIPPED },
		{ "flipped-90", WL_OUTPUT_TRANSFORM_FLIPPED_90 },
		{ "flipped-180", WL_OUTPUT_TRANSFORM_FLIPPED_180 },
		{ "flipped-270", WL_OUTPUT_TRANSFORM_FLIPPED_270 },
	};
	const char *value = config_get_for(o->name, "outputTransform");
	enum wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
	if (value) {
		bool known = false;
		for (size_t i = 0; i < sizeof(table) / sizeof(*table) && !known; i++) {
			if (!strcasecmp(table[i].name, value)) {
				transform = table[i].transform;
				known = true;
			}
		}
		if (!known)
			wlr_log(WLR_ERROR, "ignoring outputTransform \"%s\" for %s", value,
				o->name);
	}
	wlr_output_state_set_transform(state, transform);
}

/* Commits what mwmrc says about this output. Its mode may not be possible, nor the
 * preferred one, two 4K monitors on one link say, so the others get their turn. */
static bool output_commit_config(struct wlr_output *o)
{
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	configure_scale(o, &state);
	configure_transform(o, &state);
	bool configured = configure_mode(o, &state);
	struct wlr_output_mode *preferred = wlr_output_preferred_mode(o);
	if (!configured && preferred)
		wlr_output_state_set_mode(&state, preferred);
	if (!wlr_output_test_state(o, &state)) {
		if (configured)
			wlr_log(WLR_ERROR, "output %s can't do what mwmrc asks", o->name);
		if (preferred)
			wlr_output_state_set_mode(&state, preferred);
		if (!wlr_output_test_state(o, &state)) {
			struct wlr_output_mode *m;
			wl_list_for_each(m, &o->modes, link) {
				wlr_output_state_set_mode(&state, m);
				if (wlr_output_test_state(o, &state))
					break;
			}
		}
	}
	bool ok = wlr_output_commit_state(o, &state);
	wlr_output_state_finish(&state);
	return ok;
}

/* Mwm*NAME*outputPosition: X,Y. Otherwise the next free place in the row. */
static struct wlr_output_layout_output *output_place(struct server *server,
	struct wlr_output *o)
{
	const char *value = config_get_for(o->name, "outputPosition");
	int x, y, consumed = 0;
	if (value) {
		/* The limit keeps sums of positions and sizes from overflowing. */
		if (sscanf(value, "%d,%d%n", &x, &y, &consumed) == 2 && !value[consumed] &&
				abs(x) <= 1000000 && abs(y) <= 1000000)
			return wlr_output_layout_add(server->output_layout, o, x, y);
		wlr_log(WLR_ERROR, "ignoring outputPosition \"%s\" for %s", value, o->name);
	}
	return wlr_output_layout_add_auto(server->output_layout, o);
}

void outputs_config_changed(struct server *server)
{
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		output_commit_config(output->wlr_output);
		output_place(server, output->wlr_output);
	}
}

static void new_output(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	/* Its name and modes are what mwmrc needs, and nowhere else to be seen. */
	wlr_log(WLR_INFO, "new output %s", wlr_output->name);
	struct wlr_output_mode *known;
	wl_list_for_each(known, &wlr_output->modes, link)
		wlr_log(WLR_INFO, "  mode %dx%d@%.3f", known->width, known->height,
			known->refresh / 1000.0);

	if (!output_commit_config(wlr_output)) {
		wlr_log(WLR_ERROR, "no usable mode for output %s", wlr_output->name);
		return;
	}

	struct output *output = calloc(1, sizeof(*output));
	output->server = server;
	output->wlr_output = wlr_output;
	wlr_output->data = output;
	for (int i = 0; i < MWC_SHELL_LAYERS; i++)
		wl_list_init(&output->layers[i]);
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	float c[4] = { theme.root.r, theme.root.g, theme.root.b, 1 };
	output->background = wlr_scene_rect_create(server->layer_bg, 0, 0, c);
	wlr_scene_node_set_enabled(&output->background->node, !theme.root_none);
	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *lo = output_place(server, wlr_output);
	struct wlr_scene_output *so = wlr_scene_output_create(server->scene,
		wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, lo, so);
	/* There is no cursor image before the first motion otherwise. */
	if (!server->cursor_name)
		cursor_set_name(server, "default");
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
		wallpaper_place(output, false);
		layer_arrange(output);
	}
	icons_arrange(server);
	views_layout_changed(server);
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

/* D-Bus-activated services, such as gnome-terminal's server and the desktop portals,
 * don't inherit from mwc. In a real session only: nested, this would point the session
 * around it at mwc. */
static void export_activation_environment(struct server *server)
{
	if (!server->session)
		return;
	setenv("XDG_SESSION_TYPE", "wayland", true);
	if (in_path("dbus-update-activation-environment"))
		spawn("dbus-update-activation-environment --systemd WAYLAND_DISPLAY "
			"DISPLAY XDG_CURRENT_DESKTOP XDG_SESSION_TYPE");
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
		spawn_file(path);
}

static int handle_sighup(int signal, void *data)
{
	config_reload(data);
	return 0;
}

static int handle_terminate(int signal, void *data)
{
	struct server *server = data;
	wl_display_terminate(server->display);
	return 0;
}

static void usage(const char *argv0)
{
	printf("Usage: %s [-s startup-command] [-d]\n"
		"\n"
		"  -s CMD   run CMD once the compositor is up\n"
		"  -d       debug logging\n"
		"\n"
		"Environment: MWC_TERMINAL, MWC_ACTIVE, MWC_INACTIVE, MWC_ROOT,\n"
		"MWC_WALLPAPER, MWC_FONT, MWC_MOVE_OPAQUE, MWC_TILING,\n"
		"MWC_TILE_MAIN_WIDTH, XCURSOR_THEME, XCURSOR_SIZE, XKB_DEFAULT_*.\n"
		"Without -s, ~/.config/mwc/autostart is run if executable.\n"
		"~/.config/mwc/mwmrc is read at startup and on SIGHUP.\n",
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
	/* wlroots writes to pipes that clients hand it, for instance when pasting from
	 * X11, and a client may close its end at any time. */
	signal(SIGPIPE, SIG_IGN);

	struct server server = { 0 };
	server.display = wl_display_create();
	server.event_loop = wl_display_get_event_loop(server.display);
	/* Before anything that starts threads, Pango and Mesa do: these calls
	 * block the signals, and a thread that doesn't would catch them and
	 * die with its default action. */
	wl_event_loop_add_signal(server.event_loop, SIGHUP, handle_sighup, &server);
	wl_event_loop_add_signal(server.event_loop, SIGTERM, handle_terminate,
		&server);
	wl_event_loop_add_signal(server.event_loop, SIGINT, handle_terminate,
		&server);
	testhook_init(&server);

	config_load();
	theme_init();
	server.terminal = pick_terminal();
	wallpaper_init(&server);
	tile_init(&server);
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
	server.idle_notifier = wlr_idle_notifier_v1_create(server.display);
	server.idle_inhibit = wlr_idle_inhibit_v1_create(server.display);
	server.new_idle_inhibitor.notify = new_idle_inhibitor;
	wl_signal_add(&server.idle_inhibit->events.new_inhibitor,
		&server.new_idle_inhibitor);
	struct wlr_output_power_manager_v1 *output_power =
		wlr_output_power_manager_v1_create(server.display);
	server.output_power_set_mode.notify = output_power_set_mode;
	wl_signal_add(&output_power->events.set_mode, &server.output_power_set_mode);

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
	/* Bottom to top. Panels in the top layer stay above windows, and only
	 * the overlay layer goes above a fullscreen window. */
	struct wlr_scene_tree *root = &server.scene->tree;
	server.layer_bg = wlr_scene_tree_create(root);
	server.layer_shell[0] = wlr_scene_tree_create(root);
	server.layer_shell[1] = wlr_scene_tree_create(root);
	server.layer_icons = wlr_scene_tree_create(root);
	server.layer_below = wlr_scene_tree_create(root);
	server.layer_views = wlr_scene_tree_create(root);
	server.layer_shell[2] = wlr_scene_tree_create(root);
	server.layer_fullscreen = wlr_scene_tree_create(root);
	server.layer_unmanaged = wlr_scene_tree_create(root);
	server.layer_shell[3] = wlr_scene_tree_create(root);
	server.layer_menu = wlr_scene_tree_create(root);
	server.layer_feedback = wlr_scene_tree_create(root);
	server.layer_drag = wlr_scene_tree_create(root);

	wl_list_init(&server.views);
	xdg_init(&server);
	input_init(&server);
	layer_init(&server);
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
	/* Java under Xwayland draws grey windows and misplaces popups on a window
	 * manager that doesn't reparent, unless told. Not forced on anyone who set it. */
	setenv("_JAVA_AWT_WM_NONREPARENTING", "1", false);
	export_activation_environment(&server);
	wlr_log(WLR_INFO, "mwc running on WAYLAND_DISPLAY=%s, terminal: %s",
		socket, server.terminal);

	run_autostart(startup_cmd);
	wl_display_run(server.display);

	/* wlroots asserts that nobody listens to an object it destroys. */
	wl_display_destroy_clients(server.display);
	/* Their popups may have asked for this, and nothing will run it any more. */
	if (server.pointer_refresh)
		wl_event_source_remove(server.pointer_refresh);
	testhook_fini();
	if (server.xwayland) {
		wl_list_remove(&server.xwayland_ready.link);
		wl_list_remove(&server.new_xwayland_surface.link);
		wlr_xwayland_destroy(server.xwayland);
		server.xwayland = NULL;
	}
	wl_list_remove(&server.new_idle_inhibitor.link);
	wl_list_remove(&server.output_power_set_mode.link);
	wl_list_remove(&server.new_layer_surface.link);
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
	wl_list_remove(&server.request_start_drag.link);
	wl_list_remove(&server.start_drag.link);
	wl_list_remove(&server.keyboard_grab_end.link);
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
