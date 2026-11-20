/*
 * A Wayland client for the tests: a solid toplevel with server-side decorations,
 * with a minimum and maximum size if asked, and optionally a dialog belonging
 * to it. It takes whatever size it is configured to.
 *
 *   xdgc key=value ...
 *     app=NAME     app_id, required
 *     size=WxH     default 300x200
 *     min=WxH max=WxH
 *     pre=max|full ask for that state before the first commit
 *     csd=1        draw its own decorations: ask for no frame
 *     late_frame=xdg ask for a server-side frame after the first configure, before
 *                  the first buffer: as late as xdg-decoration allows
 *     late_frame=kde ask for one once mapped, with KDE's protocol, as GTK3 does
 *     early_frame=1  ask with KDE's protocol before the surface has a role
 *     pre_late=max|full  ask for a state after the first configure, before the first buffer
 *     unparent=1     once running, the dialog stops belonging to the main window
 *     states=1     log the states and the size each configure carries
 *     late_app=NAME set this app_id a moment after mapping
 *     late_ms=MS   that moment, default 500; says "late app" then
 *     late_after=FILE  or once FILE exists
 *     grow=WxH     after mapping, take that size of its own accord; says "grown"
 *     grow_ms=MS   wait that long before, default 1000
 *     dialog=NAME  also open a child toplevel with this app_id
 *     dialog_after=FILE  open it only once FILE exists, after all else below; says
 *                  "dialog opened"
 *     dialog_minimize_after=FILE  then, once FILE exists, the dialog asks to be
 *                  minimized; says "dialog minimize asked"
 *     minimize_after=FILE  once FILE exists, ask to be minimized; says "minimize asked"
 *     rehide_after=FILE  once FILE exists, unmap (a null buffer); says "hidden"; map
 *                  again once reshow_after=FILE exists, or 500 ms later; says "shown"
 *     unframe_after=FILE  once FILE exists, drop its xdg-decoration: no frame asked
 *                  for any more; says "unframed"
 *     control=FIFO  once all else is done, read commands from FIFO, one a line, and
 *                  after each a roundtrip, say "did N" (N counts them): title T,
 *                  app A, size WxH (of its own accord), dialog NAME, undialog,
 *                  fullscreen, unfullscreen, maximize, unmaximize, minimize, quit
 *     caps=1       print the wm_capabilities each window is offered:
 *                  "caps NAME max=M min=N"
 *     fs_output=N  a moment after mapping, ask for fullscreen on the Nth wl_output,
 *                  counting from 0; says "fullscreen asked"
 *     own_size=1   keep its own size whatever a configure says, as a game with a
 *                  fixed resolution does
 *     clear=1      draw nothing (fully transparent) and take no clicks: an empty
 *                  input region
 *     viewport=1   a 1x1 buffer, stretched to the window's size by wp_viewporter:
 *                  a window of any size for next to no memory
 *     cursor=WxH@S a cursor of its own while the pointer is over it: a WxH buffer
 *                  at buffer scale S
 *     menu_at=X,Y  a button or key press asks for the window menu at X,Y, any int32;
 *                  says "menu asked"
 *     geometry_at=X,Y  the window geometry at X,Y of the surface, any int32
 *     titles=N     once mapped, N different titles in one burst, the last "last";
 *                  says "titles sent"
 *     shadow=N     a margin of N around the window geometry, as GTK draws its shadow
 *                  in: the surface is N larger on each side, the margin 402020
 *     popup=X,Y,W,H  once the file popup_after=FILE exists, a WxH popup anchored at
 *                  X,Y of the window geometry, growing right and down, slid back
 *                  onto the output if it must be; says "popup at X Y", where it went
 *     fixed_after=FILE  once FILE exists, its size becomes its smallest and largest;
 *                  says "fixed"
 *     sub=X,Y,W,H  once mapped, a WxH subsurface at X,Y, 2040a0, as GTK3 shows its
 *                  tooltips; says "sub added"
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "server-decoration-client.h"
#include "viewporter-client.h"
#include "xdg-decoration-unstable-v1-client.h"
#include "xdg-shell-client.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct zxdg_decoration_manager_v1 *deco_manager;
static int default_w = 300, default_h = 200, min_w, min_h, max_w, max_h;
static int open_windows;
static const char *pre_state;
static const char *late_app;
static int late_ms = 500;
static const char *late_after;
static int grow_w, grow_h, grow_ms = 1000;
static bool log_states, client_side;
static const char *late_frame, *pre_late;
static bool early_frame, unparent;
static struct org_kde_kwin_server_decoration_manager *kde_manager;
static struct wp_viewporter *viewporter;
static bool use_viewport, clear;
static int cursor_w, cursor_h, cursor_scale;
static bool menu_asked;
static int32_t menu_x, menu_y;
static struct wl_seat *the_seat;
static struct xdg_toplevel *menu_toplevel;
static struct wl_surface *cursor_surf;
static int shadow, titles;
static bool geometry_at;
static int32_t geom_x, geom_y;
static int popup_x, popup_y, popup_w, popup_h;
static const char *popup_after, *fixed_after;
static const char *dialog_after, *minimize_after, *rehide_after, *reshow_after;
static const char *unframe_after, *dialog_minimize_after, *control;
static const char *dialog;
static struct window *main_win, *dialog_win;
static int sub_x, sub_y, sub_w, sub_h;
static struct wl_subcompositor *subcompositor;

struct window {
	struct wl_surface *surface;
	struct xdg_surface *xdg;
	struct xdg_toplevel *toplevel;
	int w, h;
	int natural_w, natural_h; /* what it takes when the choice is its own */
	bool framed;
	bool hidden; /* unmapped: a configure sent before mwc saw it is stale */
	struct zxdg_toplevel_decoration_v1 *deco;
	struct wp_viewport *viewport;
	const char *app;
};

static struct wl_display *the_display;

/* Wait that long while still answering the compositor. */
static void wait_dispatching(struct wl_display *display, int ms)
{
	struct timespec now, end;
	clock_gettime(CLOCK_MONOTONIC, &end);
	end.tv_sec += ms / 1000;
	end.tv_nsec += (ms % 1000) * 1000000L;
	if (end.tv_nsec >= 1000000000L) {
		end.tv_sec++;
		end.tv_nsec -= 1000000000L;
	}
	for (;;) {
		while (wl_display_prepare_read(display) != 0)
			wl_display_dispatch_pending(display);
		wl_display_flush(display);
		clock_gettime(CLOCK_MONOTONIC, &now);
		long left = (end.tv_sec - now.tv_sec) * 1000 +
			(end.tv_nsec - now.tv_nsec) / 1000000;
		if (left <= 0) {
			wl_display_cancel_read(display);
			return;
		}
		struct pollfd p = { .fd = wl_display_get_fd(display), .events = POLLIN };
		if (poll(&p, 1, left) > 0) {
			wl_display_read_events(display);
			wl_display_dispatch_pending(display);
		} else {
			wl_display_cancel_read(display);
		}
	}
}
#define wl_proxy_get_display_of(p) the_display

static void draw(struct window *win)
{
	int bw = win->w + 2 * shadow, bh = win->h + 2 * shadow;
	if (shadow && win->toplevel)
		xdg_surface_set_window_geometry(win->xdg, shadow, shadow, win->w, win->h);
	if (geometry_at && win->toplevel)
		xdg_surface_set_window_geometry(win->xdg, geom_x, geom_y, win->w, win->h);
	if (win->viewport) {
		wp_viewport_set_destination(win->viewport, win->w, win->h);
		bw = bh = 1;
	}
	size_t size = (size_t)bw * bh * 4;
	int fd = memfd_create("xdgc", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) != 0) {
		perror("xdgc: buffer");
		exit(1);
	}
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (size_t i = 0; i < size / 4; i++) {
		int x = i % bw, y = i / bw;
		bool margin = x < shadow || y < shadow || x >= bw - shadow || y >= bh - shadow;
		px[i] = clear ? 0 : margin ? 0xff402020 : 0xff808080;
	}
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, bw, bh,
		bw * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_surface_attach(win->surface, buffer, 0, 0);
	wl_surface_damage_buffer(win->surface, 0, 0, bw, bh);
	wl_surface_commit(win->surface);
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
	struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
	if (cursor_scale <= 0)
		return;
	if (!cursor_surf) {
		size_t size = (size_t)cursor_w * cursor_h * 4;
		int fd = memfd_create("xdgc-cursor", MFD_CLOEXEC);
		if (fd < 0 || ftruncate(fd, size) != 0) {
			perror("xdgc: cursor buffer");
			exit(1);
		}
		uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		for (size_t i = 0; i < size / 4; i++)
			px[i] = 0xff000000;
		munmap(px, size);
		struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
		struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, cursor_w,
			cursor_h, cursor_w * 4, WL_SHM_FORMAT_ARGB8888);
		wl_shm_pool_destroy(pool);
		close(fd);
		cursor_surf = wl_compositor_create_surface(compositor);
		wl_surface_set_buffer_scale(cursor_surf, cursor_scale);
		wl_surface_attach(cursor_surf, buffer, 0, 0);
		wl_surface_damage_buffer(cursor_surf, 0, 0, cursor_w, cursor_h);
		wl_surface_commit(cursor_surf);
	}
	wl_pointer_set_cursor(pointer, serial, cursor_surf, 0, 0);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
	struct wl_surface *surface)
{
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
	wl_fixed_t x, wl_fixed_t y)
{
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
	uint32_t time, uint32_t button, uint32_t state)
{
	if (!menu_asked || state != WL_POINTER_BUTTON_STATE_PRESSED || !menu_toplevel)
		return;
	xdg_toplevel_show_window_menu(menu_toplevel, the_seat, serial, menu_x, menu_y);
	printf("menu asked\n");
	fflush(stdout);
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
	uint32_t axis, wl_fixed_t value)
{
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
};

static void keyboard_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int fd,
	uint32_t size)
{
	close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
	struct wl_surface *surface, struct wl_array *keys)
{
}

static void keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
	struct wl_surface *surface)
{
}

/* A key asks for the menu as a button does: for a window whose surface is out of
 * the pointer's reach. Not a modifier, which may start one of mwc's bindings. */
static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial,
	uint32_t time, uint32_t key, uint32_t state)
{
	static const uint32_t modifiers[] = { 29, 42, 54, 56, 97, 100, 125, 126 };
	for (size_t i = 0; i < sizeof(modifiers) / sizeof(*modifiers); i++) {
		if (key == modifiers[i])
			return;
	}
	pointer_button(data, NULL, serial, time, 0, state);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
	uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
};

/* Asked for before the seat has one, a keyboard is a protocol error. */
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	static struct wl_keyboard *kb;
	if (menu_asked && !kb && (caps & WL_SEAT_CAPABILITY_KEYBOARD)) {
		kb = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(kb, &keyboard_listener, NULL);
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void xdg_configure(void *data, struct xdg_surface *xdg, uint32_t serial)
{
	struct window *win = data;
	if (win->hidden)
		return;
	xdg_surface_ack_configure(xdg, serial);
	if (pre_late) {
		if (!strcmp(pre_late, "max"))
			xdg_toplevel_set_maximized(win->toplevel);
		else
			xdg_toplevel_set_fullscreen(win->toplevel, NULL);
		pre_late = NULL;
	}
	if (late_frame && !strcmp(late_frame, "xdg") && !win->framed) {
		win->framed = true;
		win->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(deco_manager,
			win->toplevel);
		zxdg_toplevel_decoration_v1_set_mode(win->deco,
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	draw(win);
}

static const struct xdg_surface_listener xdg_listener = {
	.configure = xdg_configure,
};

static bool own_size; /* keeps its size, whatever a configure says */

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w,
	int32_t h, struct wl_array *states)
{
	struct window *win = data;
	win->w = w > 0 && !own_size ? w : win->natural_w;
	win->h = h > 0 && !own_size ? h : win->natural_h;
	if (log_states) {
		bool maximized = false;
		uint32_t *state;
		wl_array_for_each(state, states)
			maximized |= *state == XDG_TOPLEVEL_STATE_MAXIMIZED;
		printf("configure %s %dx%d\n", maximized ? "maximized" : "free", w, h);
		fflush(stdout);
	}
}

static void toplevel_close(void *data, struct xdg_toplevel *t)
{
	struct window *win = data;
	/* The decoration goes first: it may not outlive its toplevel. */
	if (win->deco)
		zxdg_toplevel_decoration_v1_destroy(win->deco);
	win->deco = NULL;
	if (win->toplevel == menu_toplevel)
		menu_toplevel = NULL;
	xdg_toplevel_destroy(win->toplevel);
	xdg_surface_destroy(win->xdg);
	wl_surface_destroy(win->surface);
	win->toplevel = NULL; /* closed: what waits for a file leaves it alone */
	if (--open_windows == 0)
		exit(0);
}

static void toplevel_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h)
{
}

static bool print_caps;
static int fs_output = -1;
static struct wl_output *outputs[4];
static int n_outputs;

static void toplevel_caps(void *data, struct xdg_toplevel *t, struct wl_array *c)
{
	if (!print_caps)
		return;
	struct window *win = data;
	bool max = false, min = false;
	uint32_t *cap;
	wl_array_for_each(cap, c) {
		max |= *cap == XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE;
		min |= *cap == XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE;
	}
	printf("caps %s max=%d min=%d\n", win->app, max, min);
	fflush(stdout);
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_bounds,
	.wm_capabilities = toplevel_caps,
};

static void ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { .ping = ping };

static void registry_global(void *data, struct wl_registry *registry,
	uint32_t name, const char *interface, uint32_t version)
{
	if (strcmp(interface, wl_compositor_interface.name) == 0)
		compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 4);
	else if (strcmp(interface, wl_shm_interface.name) == 0)
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (strcmp(interface, wl_subcompositor_interface.name) == 0)
		subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
	else if (strcmp(interface, wl_seat_interface.name) == 0 &&
			(cursor_scale > 0 || menu_asked)) {
		/* The tests always have a pointer, so its capability is not awaited. */
		the_seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
		wl_pointer_add_listener(wl_seat_get_pointer(the_seat), &pointer_listener, NULL);
		wl_seat_add_listener(the_seat, &seat_listener, NULL);
	}
	else if (strcmp(interface, wl_output_interface.name) == 0 && n_outputs < 4)
		outputs[n_outputs++] = wl_registry_bind(registry, name, &wl_output_interface, 1);
	else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		/* wm_capabilities came with version 5; the other tests keep version 1. */
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
			print_caps && version >= 5 ? 5 : 1);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (strcmp(interface,
			zxdg_decoration_manager_v1_interface.name) == 0) {
		deco_manager = wl_registry_bind(registry, name,
			&zxdg_decoration_manager_v1_interface, 1);
	} else if (strcmp(interface,
			org_kde_kwin_server_decoration_manager_interface.name) == 0) {
		kde_manager = wl_registry_bind(registry, name,
			&org_kde_kwin_server_decoration_manager_interface, 1);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
	uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static struct window *open_window(const char *app, struct window *parent,
	int w, int h, bool limits)
{
	struct window *win = calloc(1, sizeof(*win));
	win->app = app;
	win->w = win->natural_w = w;
	win->h = win->natural_h = h;
	win->surface = wl_compositor_create_surface(compositor);
	if (use_viewport)
		win->viewport = wp_viewporter_get_viewport(viewporter, win->surface);
	if (clear) {
		struct wl_region *none = wl_compositor_create_region(compositor);
		wl_surface_set_input_region(win->surface, none);
		wl_region_destroy(none);
	}
	if (early_frame && limits) {
		struct org_kde_kwin_server_decoration *deco =
			org_kde_kwin_server_decoration_manager_create(kde_manager, win->surface);
		org_kde_kwin_server_decoration_request_mode(deco,
			ORG_KDE_KWIN_SERVER_DECORATION_MODE_SERVER);
		wl_display_roundtrip(the_display);
	}
	win->xdg = xdg_wm_base_get_xdg_surface(wm_base, win->surface);
	xdg_surface_add_listener(win->xdg, &xdg_listener, win);
	win->toplevel = xdg_surface_get_toplevel(win->xdg);
	xdg_toplevel_add_listener(win->toplevel, &toplevel_listener, win);
	xdg_toplevel_set_app_id(win->toplevel, app);
	xdg_toplevel_set_title(win->toplevel, app);
	if (parent)
		xdg_toplevel_set_parent(win->toplevel, parent->toplevel);
	if (limits) {
		xdg_toplevel_set_min_size(win->toplevel, min_w, min_h);
		xdg_toplevel_set_max_size(win->toplevel, max_w, max_h);
	}
	if (!client_side && !late_frame && !early_frame) {
		win->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(deco_manager,
			win->toplevel);
		zxdg_toplevel_decoration_v1_set_mode(win->deco,
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	if (limits && pre_state && !strcmp(pre_state, "max"))
		xdg_toplevel_set_maximized(win->toplevel);
	else if (limits && pre_state && !strcmp(pre_state, "full"))
		xdg_toplevel_set_fullscreen(win->toplevel, NULL);
	if (pre_state)
		wl_display_roundtrip(wl_proxy_get_display_of(win->toplevel));
	wl_surface_commit(win->surface);
	open_windows++;
	return win;
}

static void popup_surface_configure(void *data, struct xdg_surface *xdg, uint32_t serial)
{
	xdg_surface_ack_configure(xdg, serial);
	draw(data);
}

static const struct xdg_surface_listener popup_surface_listener = {
	.configure = popup_surface_configure,
};

static void popup_configure(void *data, struct xdg_popup *p, int32_t x, int32_t y,
	int32_t w, int32_t h)
{
	printf("popup at %d %d\n", x, y);
	fflush(stdout);
}

static void popup_done(void *data, struct xdg_popup *p)
{
}

static const struct xdg_popup_listener popup_listener = {
	.configure = popup_configure,
	.popup_done = popup_done,
};

static void open_popup(struct window *parent)
{
	struct window *win = calloc(1, sizeof(*win));
	win->w = popup_w;
	win->h = popup_h;
	win->surface = wl_compositor_create_surface(compositor);
	win->xdg = xdg_wm_base_get_xdg_surface(wm_base, win->surface);
	xdg_surface_add_listener(win->xdg, &popup_surface_listener, win);
	struct xdg_positioner *pos = xdg_wm_base_create_positioner(wm_base);
	xdg_positioner_set_size(pos, popup_w, popup_h);
	xdg_positioner_set_anchor_rect(pos, popup_x, popup_y, 2, 2);
	xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
	xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	xdg_positioner_set_constraint_adjustment(pos,
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
	struct xdg_popup *popup = xdg_surface_get_popup(win->xdg, parent->xdg, pos);
	xdg_popup_add_listener(popup, &popup_listener, win);
	xdg_positioner_destroy(pos);
	wl_surface_commit(win->surface);
}

static void wait_for_file(struct wl_display *display, const char *path)
{
	while (access(path, F_OK) != 0)
		wait_dispatching(display, 50);
}

/* Not closed by mwc meanwhile: a closed window's objects are gone. */
static bool alive(const struct window *win)
{
	return win && win->toplevel;
}

static void open_dialog(struct wl_display *display)
{
	/* Its window mapped first: wlroots takes only a mapped parent. */
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);
	if (dialog_after)
		wait_for_file(display, dialog_after);
	if (!alive(main_win))
		return;
	dialog_win = open_window(dialog, main_win, 120, 80, false);
	if (unparent) {
		wl_display_roundtrip(display);
		wl_display_roundtrip(display);
		wait_dispatching(display, 1500);
		if (alive(dialog_win))
			xdg_toplevel_set_parent(dialog_win->toplevel, NULL);
	}
	wl_display_flush(display);
	if (dialog_after) {
		printf("dialog opened\n");
		fflush(stdout);
	}
}

static void add_subsurface(struct window *parent)
{
	struct wl_surface *s = wl_compositor_create_surface(compositor);
	struct wl_subsurface *sub = wl_subcompositor_get_subsurface(subcompositor, s,
		parent->surface);
	wl_subsurface_set_position(sub, sub_x, sub_y);
	wl_subsurface_set_desync(sub);
	size_t size = (size_t)sub_w * sub_h * 4;
	int fd = memfd_create("xdgc-sub", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) != 0) {
		perror("xdgc: subsurface buffer");
		exit(1);
	}
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (size_t i = 0; i < size / 4; i++)
		px[i] = 0xff2040a0;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, sub_w, sub_h,
		sub_w * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_surface_attach(s, buffer, 0, 0);
	wl_surface_damage_buffer(s, 0, 0, sub_w, sub_h);
	wl_surface_commit(s);
	wl_surface_commit(parent->surface);
}

/* One command of control=FIFO; false for quit. */
static bool command(struct wl_display *display, char *line)
{
	char *arg = strchr(line, ' ');
	if (arg)
		*arg++ = '\0';
	if (!strcmp(line, "quit"))
		return false;
	if (!strcmp(line, "dialog") && arg && dialog_win && dialog_win->toplevel)
		; /* one is enough */
	else if (!strcmp(line, "dialog") && arg && alive(main_win))
		dialog_win = open_window(strdup(arg), main_win, 120, 80, false);
	else if (!strcmp(line, "undialog") && dialog_win && dialog_win->toplevel)
		toplevel_close(dialog_win, dialog_win->toplevel);
	else if (!alive(main_win))
		;
	else if (!strcmp(line, "title") && arg)
		xdg_toplevel_set_title(main_win->toplevel, arg);
	else if (!strcmp(line, "app") && arg)
		xdg_toplevel_set_app_id(main_win->toplevel, arg);
	else if (!strcmp(line, "size") && arg &&
			sscanf(arg, "%dx%d", &main_win->natural_w, &main_win->natural_h) == 2) {
		main_win->w = main_win->natural_w;
		main_win->h = main_win->natural_h;
		draw(main_win);
	} else if (!strcmp(line, "fullscreen"))
		xdg_toplevel_set_fullscreen(main_win->toplevel, NULL);
	else if (!strcmp(line, "unfullscreen"))
		xdg_toplevel_unset_fullscreen(main_win->toplevel);
	else if (!strcmp(line, "maximize"))
		xdg_toplevel_set_maximized(main_win->toplevel);
	else if (!strcmp(line, "unmaximize"))
		xdg_toplevel_unset_maximized(main_win->toplevel);
	else if (!strcmp(line, "minimize"))
		xdg_toplevel_set_minimized(main_win->toplevel);
	return true;
}

/* Answers mwc and takes commands until quit or no window is left. */
static int control_loop(struct wl_display *display)
{
	int fd = open(control, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("xdgc: control");
		return 1;
	}
	char buf[512];
	size_t len = 0;
	int done = 0;
	while (open_windows > 0) {
		while (wl_display_prepare_read(display) != 0)
			wl_display_dispatch_pending(display);
		wl_display_flush(display);
		struct pollfd p[2] = {
			{ .fd = wl_display_get_fd(display), .events = POLLIN },
			{ .fd = fd, .events = POLLIN },
		};
		if (poll(p, fd >= 0 ? 2 : 1, -1) < 0) {
			wl_display_cancel_read(display);
			continue;
		}
		if (p[0].revents & POLLIN) {
			if (wl_display_read_events(display) < 0)
				return 1;
			wl_display_dispatch_pending(display);
		} else {
			wl_display_cancel_read(display);
		}
		if (fd < 0 || !(p[1].revents & (POLLIN | POLLHUP)))
			continue;
		ssize_t n = read(fd, buf + len, sizeof(buf) - 1 - len);
		if (n <= 0) {
			/* No writer: wait for the next one without spinning. */
			close(fd);
			fd = open(control, O_RDONLY);
			continue;
		}
		len += n;
		char *nl;
		while ((nl = memchr(buf, '\n', len))) {
			*nl = '\0';
			if (!command(display, buf))
				return 0;
			wl_display_roundtrip(display);
			printf("did %d\n", ++done);
			fflush(stdout);
			len -= nl + 1 - buf;
			memmove(buf, nl + 1, len);
		}
		if (len == sizeof(buf) - 1)
			len = 0; /* a line too long: dropped */
	}
	return 0;
}

int main(int argc, char *argv[])
{
	const char *app = NULL;
	for (int i = 1; i < argc; i++) {
		char *v = strchr(argv[i], '=');
		if (!v) {
			fprintf(stderr, "xdgc: expected key=value: %s\n", argv[i]);
			return 2;
		}
		*v++ = '\0';
		if (!strcmp(argv[i], "app"))
			app = v;
		else if (!strcmp(argv[i], "size"))
			sscanf(v, "%dx%d", &default_w, &default_h);
		else if (!strcmp(argv[i], "min"))
			sscanf(v, "%dx%d", &min_w, &min_h);
		else if (!strcmp(argv[i], "max"))
			sscanf(v, "%dx%d", &max_w, &max_h);
		else if (!strcmp(argv[i], "dialog"))
			dialog = v;
		else if (!strcmp(argv[i], "pre"))
			pre_state = v;
		else if (!strcmp(argv[i], "early_frame"))
			early_frame = atoi(v);
		else if (!strcmp(argv[i], "pre_late"))
			pre_late = v;
		else if (!strcmp(argv[i], "unparent"))
			unparent = atoi(v);
		else if (!strcmp(argv[i], "late_frame"))
			late_frame = v;
		else if (!strcmp(argv[i], "csd"))
			client_side = atoi(v);
		else if (!strcmp(argv[i], "states"))
			log_states = atoi(v);
		else if (!strcmp(argv[i], "clear"))
			clear = atoi(v);
		else if (!strcmp(argv[i], "own_size"))
			own_size = atoi(v);
		else if (!strcmp(argv[i], "caps"))
			print_caps = atoi(v);
		else if (!strcmp(argv[i], "fs_output"))
			fs_output = atoi(v);
		else if (!strcmp(argv[i], "viewport"))
			use_viewport = atoi(v);
		else if (!strcmp(argv[i], "late_app"))
			late_app = v;
		else if (!strcmp(argv[i], "late_ms"))
			late_ms = atoi(v);
		else if (!strcmp(argv[i], "late_after"))
			late_after = v;
		else if (!strcmp(argv[i], "grow"))
			sscanf(v, "%dx%d", &grow_w, &grow_h);
		else if (!strcmp(argv[i], "grow_ms"))
			grow_ms = atoi(v);
		else if (!strcmp(argv[i], "cursor"))
			sscanf(v, "%dx%d@%d", &cursor_w, &cursor_h, &cursor_scale);
		else if (!strcmp(argv[i], "menu_at"))
			menu_asked = sscanf(v, "%d,%d", &menu_x, &menu_y) == 2;
		else if (!strcmp(argv[i], "shadow"))
			shadow = atoi(v);
		else if (!strcmp(argv[i], "titles"))
			titles = atoi(v);
		else if (!strcmp(argv[i], "geometry_at"))
			geometry_at = sscanf(v, "%d,%d", &geom_x, &geom_y) == 2;
		else if (!strcmp(argv[i], "popup"))
			sscanf(v, "%d,%d,%d,%d", &popup_x, &popup_y, &popup_w, &popup_h);
		else if (!strcmp(argv[i], "popup_after"))
			popup_after = v;
		else if (!strcmp(argv[i], "fixed_after"))
			fixed_after = v;
		else if (!strcmp(argv[i], "dialog_after"))
			dialog_after = v;
		else if (!strcmp(argv[i], "minimize_after"))
			minimize_after = v;
		else if (!strcmp(argv[i], "rehide_after"))
			rehide_after = v;
		else if (!strcmp(argv[i], "reshow_after"))
			reshow_after = v;
		else if (!strcmp(argv[i], "unframe_after"))
			unframe_after = v;
		else if (!strcmp(argv[i], "dialog_minimize_after"))
			dialog_minimize_after = v;
		else if (!strcmp(argv[i], "control"))
			control = v;
		else if (!strcmp(argv[i], "sub"))
			sscanf(v, "%d,%d,%d,%d", &sub_x, &sub_y, &sub_w, &sub_h);
	}
	if (!app) {
		fprintf(stderr, "xdgc: app= is required\n");
		return 2;
	}
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "xdgc: cannot connect to the compositor\n");
		return 1;
	}
	the_display = display;
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !wm_base || !deco_manager) {
		fprintf(stderr, "xdgc: the compositor lacks a needed global\n");
		return 1;
	}
	main_win = open_window(app, NULL, default_w, default_h, true);
	menu_toplevel = main_win->toplevel;
	if (dialog && !dialog_after)
		open_dialog(display);
	if (late_frame && !strcmp(late_frame, "kde")) {
		wl_display_roundtrip(display);
		wl_display_roundtrip(display);
		usleep(1000000);
		struct org_kde_kwin_server_decoration *deco =
			org_kde_kwin_server_decoration_manager_create(kde_manager,
			main_win->surface);
		org_kde_kwin_server_decoration_request_mode(deco,
			ORG_KDE_KWIN_SERVER_DECORATION_MODE_SERVER);
		wl_display_flush(display);
	}
	if (late_app) {
		wl_display_roundtrip(display);
		wl_display_roundtrip(display);
		if (late_after)
			wait_for_file(display, late_after);
		else
			wait_dispatching(display, late_ms);
		if (alive(main_win)) {
			xdg_toplevel_set_app_id(main_win->toplevel, late_app);
			wl_surface_commit(main_win->surface);
		}
		wl_display_flush(display);
		printf("late app\n");
		fflush(stdout);
	}
	if (fs_output >= 0 && fs_output < n_outputs) {
		wl_display_roundtrip(display);
		wl_display_roundtrip(display);
		wait_dispatching(display, 500);
		if (alive(main_win))
			xdg_toplevel_set_fullscreen(main_win->toplevel, outputs[fs_output]);
		wl_display_flush(display);
		printf("fullscreen asked\n");
		fflush(stdout);
	}
	if (grow_w > 0) {
		wl_display_roundtrip(display);
		wl_display_roundtrip(display);
		wait_dispatching(display, grow_ms);
		main_win->natural_w = main_win->w = grow_w;
		main_win->natural_h = main_win->h = grow_h;
		if (alive(main_win))
			draw(main_win);
		wl_display_flush(display);
		printf("grown\n");
		fflush(stdout);
	}
	if (titles > 0) {
		wl_display_roundtrip(display);
		wl_display_roundtrip(display);
		char t[32];
		for (int i = 1; i < titles && alive(main_win); i++) {
			snprintf(t, sizeof(t), "title %d", i);
			xdg_toplevel_set_title(main_win->toplevel, t);
		}
		if (alive(main_win))
			xdg_toplevel_set_title(main_win->toplevel, "last");
		wl_display_flush(display);
		printf("titles sent\n");
		fflush(stdout);
	}
	if (popup_w > 0 && popup_after) {
		wait_for_file(display, popup_after);
		if (alive(main_win))
			open_popup(main_win);
		wl_display_flush(display);
	}
	if (sub_w > 0 && sub_h > 0 && subcompositor) {
		wl_display_roundtrip(display);
		wl_display_roundtrip(display);
		if (alive(main_win))
			add_subsurface(main_win);
		wl_display_flush(display);
		printf("sub added\n");
		fflush(stdout);
	}
	if (fixed_after) {
		wait_for_file(display, fixed_after);
		if (alive(main_win)) {
			xdg_toplevel_set_min_size(main_win->toplevel, main_win->w, main_win->h);
			xdg_toplevel_set_max_size(main_win->toplevel, main_win->w, main_win->h);
			wl_surface_commit(main_win->surface);
		}
		wl_display_flush(display);
		printf("fixed\n");
		fflush(stdout);
	}
	if (minimize_after) {
		wait_for_file(display, minimize_after);
		if (alive(main_win))
			xdg_toplevel_set_minimized(main_win->toplevel);
		wl_display_flush(display);
		printf("minimize asked\n");
		fflush(stdout);
	}
	if (rehide_after) {
		wait_for_file(display, rehide_after);
		if (alive(main_win)) {
			main_win->hidden = true;
			wl_surface_attach(main_win->surface, NULL, 0, 0);
			wl_surface_commit(main_win->surface);
			wl_display_roundtrip(display);
			printf("hidden\n");
			fflush(stdout);
		}
		if (reshow_after)
			wait_for_file(display, reshow_after);
		else
			wait_dispatching(display, 500);
		/* Unmapped, it starts over, as wlroots forgets its app_id and title: a commit
		 * without a buffer, then the configure. */
		if (alive(main_win)) {
			xdg_toplevel_set_app_id(main_win->toplevel, main_win->app);
			xdg_toplevel_set_title(main_win->toplevel, main_win->app);
			main_win->hidden = false;
			wl_surface_commit(main_win->surface);
			wl_display_roundtrip(display);
			wl_display_roundtrip(display);
			printf("shown\n");
			fflush(stdout);
		}
	}
	if (unframe_after) {
		wait_for_file(display, unframe_after);
		if (alive(main_win) && main_win->deco) {
			zxdg_toplevel_decoration_v1_destroy(main_win->deco);
			main_win->deco = NULL;
			wl_surface_commit(main_win->surface);
		}
		wl_display_flush(display);
		printf("unframed\n");
		fflush(stdout);
	}
	if (dialog && dialog_after)
		open_dialog(display);
	if (dialog_minimize_after && dialog_win) {
		wait_for_file(display, dialog_minimize_after);
		if (alive(dialog_win))
			xdg_toplevel_set_minimized(dialog_win->toplevel);
		wl_display_flush(display);
		printf("dialog minimize asked\n");
		fflush(stdout);
	}
	if (control)
		return control_loop(display);
	while (open_windows > 0 && wl_display_dispatch(display) != -1) {
	}
	return 0;
}
