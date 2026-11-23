/*
 * mwc - a Motif-style Wayland compositor
 *
 * Server setup: the protocols offered, the scene's layers, start and end. Run it
 * from a TTY for a real session, or inside an existing Wayland or X11 session where
 * wlroots opens a window. Outputs are in output.c.
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
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include "mwc.h"
#include "version.h"

/* One per inhibitor. While one exists whose surface is mapped, nobody counts as idle;
 * minimized or on another desktop it still counts, see README. One whose surface
 * never appeared doesn't, or a client could keep the screen on unseen. */
struct idle_inhibitor {
	struct wl_listener destroy;
	struct wl_listener map, unmap;
	struct server *server;
	struct wlr_idle_inhibitor_v1 *wlr;
};

static void idle_recount(struct server *server, struct wlr_idle_inhibitor_v1 *going)
{
	bool any = false;
	struct wlr_idle_inhibitor_v1 *i;
	wl_list_for_each(i, &server->idle_inhibit->inhibitors, link)
		any |= i != going && i->surface->mapped;
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, any);
}

static void idle_inhibitor_mapped(struct wl_listener *listener, void *data)
{
	struct idle_inhibitor *inhibitor = wl_container_of(listener, inhibitor, map);
	idle_recount(inhibitor->server, NULL);
}

static void idle_inhibitor_unmapped(struct wl_listener *listener, void *data)
{
	struct idle_inhibitor *inhibitor = wl_container_of(listener, inhibitor, unmap);
	idle_recount(inhibitor->server, NULL);
}

static void idle_inhibitor_destroy(struct wl_listener *listener, void *data)
{
	struct idle_inhibitor *inhibitor = wl_container_of(listener, inhibitor, destroy);
	/* It is still in the list while it says that it goes. */
	idle_recount(inhibitor->server, inhibitor->wlr);
	wl_list_remove(&inhibitor->destroy.link);
	wl_list_remove(&inhibitor->map.link);
	wl_list_remove(&inhibitor->unmap.link);
	free(inhibitor);
}

/* token->data of a good token made while the keys were on nothing. */
static char keys_on_nothing;

/* Whether s, or the surface its popups hang from, is a layer surface. */
static bool from_layer(struct wlr_surface *s)
{
	for (int i = 0; s && i < 16; i++) {
		s = wlr_surface_get_root_surface(s);
		struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(s);
		if (!xdg || xdg->role != WLR_XDG_SURFACE_ROLE_POPUP || !xdg->popup)
			return wlr_layer_surface_v1_try_from_wlr_surface(s) != NULL;
		s = xdg->popup->parent;
	}
	return false;
}

/* A good token is marked when it is made: from the window with the keyboard, or from a
 * panel or launcher, which wlroots takes only with the keyboard or the pointer on it.
 * It keeps where the keys were then; by the time it is used they may have moved on. */
static void activation_new_token(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, activation_new_token);
	struct wlr_xdg_activation_token_v1 *token = data;
	token->data = NULL;
	struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
	if (!token->seat || !token->surface)
		return;
	struct view *view = view_from_surface(token->surface);
	bool good = view ? focused && view == view_from_surface(focused) :
		from_layer(token->surface);
	if (good)
		token->data = focused ? (void *)focused : &keys_on_nothing;
}

static void activation_request(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, activation_request);
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	struct view *view = view_from_surface(event->surface);
	if (!view)
		return;
	/* Only compared: the surface the keys were on may be gone. */
	struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
	void *keys = focused ? (void *)focused : &keys_on_nothing;
	struct activation_request r = {
		.from_input = event->token->seat != NULL,
		.source_had_focus = event->token->data != NULL,
		.focus_kept = event->token->data == keys ||
			(focused && view_from_surface(focused) == view),
		.target_mapped = view->mapped,
		.target_docked = view_docked(view),
		.target_off_desktop = view->off_desktop,
	};
	enum activation a = activation_decide(&r);
	const char *app = view_app_id(view);
	wlr_log(WLR_INFO, "%s asks for the focus: %s", *app ? app : "a window",
		a == ACTIVATION_NONE ? "not given" : a == ACTIVATION_MARK ?
		"its desktop is marked" : "given");
	switch (a) {
	case ACTIVATION_SWITCH:
		desktop_switch(server, view->desktop);
		/* fall through */
	case ACTIVATION_FOCUS:
		view_focus(view);
		break;
	case ACTIVATION_MARK:
		desktop_attention(view);
		break;
	case ACTIVATION_NONE:
		break;
	}
}

static void new_idle_inhibitor(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, new_idle_inhibitor);
	struct wlr_idle_inhibitor_v1 *wlr_inhibitor = data;
	struct idle_inhibitor *inhibitor = calloc(1, sizeof(*inhibitor));
	if (!inhibitor) {
		wl_resource_post_no_memory(wlr_inhibitor->resource);
		return;
	}
	inhibitor->server = server;
	inhibitor->wlr = wlr_inhibitor;
	inhibitor->destroy.notify = idle_inhibitor_destroy;
	wl_signal_add(&wlr_inhibitor->events.destroy, &inhibitor->destroy);
	inhibitor->map.notify = idle_inhibitor_mapped;
	wl_signal_add(&wlr_inhibitor->surface->events.map, &inhibitor->map);
	inhibitor->unmap.notify = idle_inhibitor_unmapped;
	wl_signal_add(&wlr_inhibitor->surface->events.unmap, &inhibitor->unmap);
	idle_recount(server, NULL);
}

/* xdg-activation and the idle protocols: who may take the focus, who is idle. */
static void session_init(struct server *server)
{
	struct wlr_xdg_activation_v1 *activation =
		wlr_xdg_activation_v1_create(server->display);
	server->activation_new_token.notify = activation_new_token;
	wl_signal_add(&activation->events.new_token, &server->activation_new_token);
	server->activation_request.notify = activation_request;
	wl_signal_add(&activation->events.request_activate, &server->activation_request);
	server->idle_notifier = wlr_idle_notifier_v1_create(server->display);
	server->idle_inhibit = wlr_idle_inhibit_v1_create(server->display);
	server->new_idle_inhibitor.notify = new_idle_inhibitor;
	wl_signal_add(&server->idle_inhibit->events.new_inhibitor,
		&server->new_idle_inhibitor);
}

static void session_fini(struct server *server)
{
	wl_list_remove(&server->new_idle_inhibitor.link);
	wl_list_remove(&server->activation_new_token.link);
	wl_list_remove(&server->activation_request.link);
}

static int throttle_rested(void *data)
{
	struct throttle *t = data;
	t->resting = false;
	if (!t->due)
		return 0;
	t->due = false;
	t->resting = true;
	wl_event_source_timer_update(t->timer, t->ms);
	t->run(t->data);
	return 0;
}

void throttle_call(struct throttle *t, struct wl_event_loop *loop)
{
	if (!t->timer)
		t->timer = wl_event_loop_add_timer(loop, throttle_rested, t);
	if (t->resting) {
		t->due = true;
		return;
	}
	/* Without a timer each call runs: slower, but nothing is lost. */
	if (t->timer) {
		t->resting = true;
		wl_event_source_timer_update(t->timer, t->ms);
	}
	t->run(t->data);
}

void throttle_cancel(struct throttle *t)
{
	if (t->timer)
		wl_event_source_remove(t->timer);
	t->timer = NULL;
	t->resting = t->due = false;
}

uint32_t now_msec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- startup ------------------------------------------------------------ */

bool in_path(const char *prog)
{
	const char *path = getenv("PATH");
	if (!path)
		return false;
	char *dirs = strdup(path), *save = NULL;
	if (!dirs)
		return false;
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

/* The first one installed, if nobody says which. Terminals without client-side
 * decorations first, so they get a frame. */
static const char *pick_terminal(void)
{
	static const char *candidates[] = {
		"foot", "alacritty", "konsole", "kitty", "ptyxis", "gnome-terminal",
	};
	for (size_t i = 0; i < sizeof(candidates) / sizeof(*candidates); i++) {
		if (in_path(candidates[i]))
			return candidates[i];
	}
	return "foot";
}

/* Asked each time: a reload may have brought another one. */
const char *terminal_command(struct server *server)
{
	const char *value = getenv("MWC_TERMINAL");
	if (!value || !*value)
		value = config_get("terminal");
	return value && *value ? value : server->terminal;
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
			"XDG_CURRENT_DESKTOP XDG_SESSION_TYPE");
	/* Left there by an X session before: services would open on a display that
	 * is gone. */
	if (in_path("systemctl"))
		spawn("systemctl --user unset-environment DISPLAY");
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
	printf("Usage: %s [-s startup-command] [-d] [-v]\n"
		"\n"
		"  -s CMD   run CMD once the compositor is up\n"
		"  -d       debug logging\n"
		"  -v       print the version\n"
		"\n"
		"Environment: MWC_TERMINAL, MWC_ACTIVE, MWC_INACTIVE, MWC_ROOT,\n"
		"MWC_WALLPAPER, MWC_FONT, MWC_MOVE_OPAQUE, MWC_TILING,\n"
		"MWC_TILE_MAIN_WIDTH, MWC_VIRTUAL_INPUT, XCURSOR_THEME, XCURSOR_SIZE,\n"
		"XKB_DEFAULT_*.\n"
		"Without -s, ~/.config/mwc/autostart is run if executable.\n"
		"~/.config/mwc/mwmrc is read at startup and on SIGHUP.\n",
		argv0);
}

int main(int argc, char *argv[])
{
	const char *startup_cmd = NULL;
	enum wlr_log_importance log_level = WLR_INFO;
	int c;
	while ((c = getopt(argc, argv, "s:dvh")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		case 'd':
			log_level = WLR_DEBUG;
			break;
		case 'v':
			puts("mwc " MWC_VERSION);
			return 0;
		default:
			usage(argv[0]);
			return c == 'h' ? 0 : 1;
		}
	}
	wlr_log_init(log_level, NULL);
	/* wlroots writes to pipes that clients hand it, for instance when pasting, and a
	 * client may close its end at any time. */
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
	/* For a VNC server such as wayvnc. Only for this process: a nested mwc started
	 * from here asks for itself. */
	if (config_bool("MWC_VIRTUAL_INPUT", getenv("MWC_VIRTUAL_INPUT")) == 1)
		input_virtual_init(&server);
	unsetenv("MWC_VIRTUAL_INPUT");

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
	/* Where wlroots took its shm allocator, which puts buffers in /dev/shm: see
	 * shmalloc.c. */
	if (server.allocator->buffer_caps == (WLR_BUFFER_CAP_SHM | WLR_BUFFER_CAP_DATA_PTR)) {
		struct wlr_allocator *shm = shm_allocator_create();
		if (shm) {
			wlr_allocator_destroy(server.allocator);
			server.allocator = shm;
		}
	}

	server.compositor = wlr_compositor_create(server.display, 6,
		server.renderer);
	wlr_subcompositor_create(server.display);
	wlr_data_device_manager_create(server.display);
	wlr_primary_selection_v1_device_manager_create(server.display);
	/* The clipboard for programs without a window, such as screenShot -c or wl-copy:
	 * they cannot get the keyboard focus that wl_data_device asks for. Both versions,
	 * the older one is what most clipboard tools still speak. */
	wlr_ext_data_control_manager_v1_create(server.display, 1);
	wlr_data_control_manager_v1_create(server.display);
	wlr_viewporter_create(server.display);
	wlr_single_pixel_buffer_manager_v1_create(server.display);
	wlr_presentation_create(server.display, server.backend, 2);
	fifo_init(&server);
	workspace_init(&server);
	wlr_screencopy_manager_v1_create(server.display);
	/* screencopy's successor, with damage and the pointer as an image of its own: a
	 * VNC or RDP server can send it apart, and the viewer draws it without delay. */
	wlr_ext_image_copy_capture_manager_v1_create(server.display, 1);
	wlr_ext_output_image_capture_source_manager_v1_create(server.display, 1);
	session_init(&server);
	outputs_init(&server);

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
	iconbox_init(&server);
	server.layer_views = wlr_scene_tree_create(root);
	dock_init(&server);
	server.layer_shell[2] = wlr_scene_tree_create(root);
	server.layer_fullscreen = wlr_scene_tree_create(root);
	server.layer_shell[3] = wlr_scene_tree_create(root);
	server.layer_menu = wlr_scene_tree_create(root);
	server.layer_feedback = wlr_scene_tree_create(root);
	server.layer_drag = wlr_scene_tree_create(root);

	wl_list_init(&server.views);
	xdg_init(&server);
	input_init(&server);
	/* After wlr_cursor's, which input_init() attaches: the pointer must be off an output
	 * that went before mwc asks what is under it. */
	outputs_watch_layout(&server);
	layer_init(&server);

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
	/* Nested in an X session, its programs would open there instead. */
	unsetenv("DISPLAY");
	export_activation_environment(&server);
	wlr_log(WLR_INFO, "mwc " MWC_VERSION " running on WAYLAND_DISPLAY=%s, terminal: %s",
		socket, terminal_command(&server));

	run_autostart(startup_cmd);
	wl_display_run(server.display);

	/* wlroots asserts that nobody listens to an object it destroys. */
	wl_display_destroy_clients(server.display);
	/* Every module lets go of what it listens to, before the objects go. */
	input_fini(&server);
	toast_fini();
	spawn_fini();
	session_fini(&server);
	outputs_fini(&server);
	layer_fini(&server);
	xdg_fini(&server);
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
