/*
 * Hooks for the headless input tests, active only when MWC_TEST_STATE names a
 * file. They stay off in normal use because the virtual input protocols let
 * any client type and click on the user's behalf.
 *
 * - zwp_virtual_keyboard_v1 and zwlr_virtual_pointer_v1 feed simulated devices
 *   into the normal input path (see tests/vinput.c).
 * - SIGUSR1 writes the window state to the file so tests can check geometry
 *   and focus, which a Wayland client cannot query for other clients.
 * - SIGUSR2 removes an output.
 */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include "mwc.h"

static struct server *test_server;
static const char *state_path;
static struct wl_listener new_virtual_keyboard;
static struct wl_listener new_virtual_pointer;

/* SIGUSR2 unplugs the newest output, which the headless backend can't do by itself. */
static int unplug_output(int signal, void *data)
{
	if (!wl_list_empty(&test_server->outputs)) {
		struct output *newest = wl_container_of(test_server->outputs.next, newest,
			link);
		wlr_output_destroy(newest->wlr_output);
	}
	return 0;
}

static void on_new_virtual_keyboard(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_keyboard_v1 *kb = data;
	input_add_device(test_server, &kb->keyboard.base);
}

static void on_new_virtual_pointer(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	input_add_device(test_server, &event->new_pointer->pointer.base);
}

/* One line per window, front to back. Written to a temporary file first, so
 * a test never reads a half-written state. */
static int dump_state(int signal, void *data)
{
	struct server *server = test_server;
	char tmp[4096];
	snprintf(tmp, sizeof(tmp), "%s.tmp", state_path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		wlr_log_errno(WLR_ERROR, "cannot write %s", tmp);
		return 0;
	}
	struct wlr_surface *kb_focus =
		server->seat->keyboard_state.focused_surface;
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		if (!v->mapped)
			continue;
		fprintf(f, "view app=%s x=%d y=%d w=%d h=%d active=%d kbfocus=%d "
			"max=%d min=%d full=%d frame=%d icon_x=%d icon_y=%d hidden=%d "
			"bw=%d th=%d above=%d below=%d snap=%d tiled=%d tile_main=%d shaded=%d "
			"fh=%d tab=%d tab_hidden=%d visible=%d tab_first=%d\n",
			view_app_id(v), v->x, v->y, v->width, v->height,
			v->activated, kb_focus && kb_focus == view_surface(v),
			v->maximized, v->minimized, v->fullscreen, v->decorated,
			v->icon_x, v->icon_y, v->hidden_with_parent,
			frame_border(v), frame_title(v),
			v->type == VIEW_XWAYLAND && v->xsurface->above,
			v->type == VIEW_XWAYLAND && v->xsurface->below, v->snap,
			v->tiled, v->tile_main, v->shaded, frame_height(v), tabs_index(v),
			v->tab_hidden, v->tree->node.enabled,
			v->group ? v->group->first_visible : 0);
	}
	/* The mnemonics of what can be chosen, without spaces for the parser. */
	char sensitive[MENU_MAX_ITEMS + 1];
	int n = 0;
	if (server->menu.open) {
		for (int i = 0; i < server->menu.n_items; i++) {
			const struct menu_item *it = &server->menu.items[i];
			if (it->label && it->sensitive && it->mnemonic)
				sensitive[n++] = it->mnemonic;
		}
	}
	sensitive[n] = '\0';
	fprintf(f, "menu open=%d x=%d y=%d title=%s sensitive=%s\n", server->menu.open,
		server->menu.x, server->menu.y,
		server->menu.open && server->menu.title ? "yes" : "no", sensitive);
	struct output *o = output_at(server, 0, 0);
	if (o)
		fprintf(f, "output ux=%d uy=%d uw=%d uh=%d\n", o->usable.x,
			o->usable.y, o->usable.width, o->usable.height);
	int unmanaged = 0;
	struct unmanaged *um;
	wl_list_for_each(um, &server->unmanaged, link)
		unmanaged += um->tree != NULL;
	fprintf(f, "unmanaged count=%d\n", unmanaged);
	struct output *each;
	wl_list_for_each(each, &server->outputs, link) {
		struct wlr_box b;
		wlr_output_layout_get_box(server->output_layout, each->wlr_output, &b);
		fprintf(f, "output_%s x=%d y=%d w=%d h=%d scale=%d\n", each->wlr_output->name,
			b.x, b.y, b.width, b.height, (int)(each->wlr_output->scale * 100));
	}
	fprintf(f, "tiling on=%d\n", server->tiling);
	int focused = 0;
	wl_list_for_each(v, &server->views, link)
		focused += v->activated;
	fprintf(f, "focus any=%d\n", focused);
	struct wlr_box box;
	bool outline = outline_get(&box);
	fprintf(f, "outline shown=%d x=%d y=%d w=%d h=%d\n", outline, box.x, box.y,
		box.width, box.height);
	bool zone = snap_preview_get(&box);
	fprintf(f, "preview shown=%d x=%d y=%d w=%d h=%d\n", zone, box.x, box.y,
		box.width, box.height);
	/* Spaces become _, for the parser. */
	const char *text = feedback_get(&box);
	char label[64];
	snprintf(label, sizeof(label), "%s", text);
	for (char *p = label; *p; p++)
		*p = *p == ' ' ? '_' : *p;
	fprintf(f, "feedback text=%s x=%d y=%d\n", label, box.x, box.y);
	fprintf(f, "theme border=%d title=%d\n", theme.border,
		theme.title_height);
	fprintf(f, "cursor x=%d y=%d mode=%d\n", (int)server->cursor->x,
		(int)server->cursor->y, server->cursor_mode);
	fclose(f);
	if (rename(tmp, state_path) != 0)
		wlr_log_errno(WLR_ERROR, "cannot rename %s", tmp);
	return 0;
}

void testhook_init(struct server *server)
{
	const char *env = getenv("MWC_TEST_STATE");
	if (!env || !*env)
		return;
	state_path = strdup(env);
	/* It is for this process, not for what it starts. */
	unsetenv("MWC_TEST_STATE");
	wlr_log(WLR_INFO, "test hooks on, state file %s", state_path);
	test_server = server;

	struct wlr_virtual_keyboard_manager_v1 *vk =
		wlr_virtual_keyboard_manager_v1_create(server->display);
	new_virtual_keyboard.notify = on_new_virtual_keyboard;
	wl_signal_add(&vk->events.new_virtual_keyboard, &new_virtual_keyboard);

	struct wlr_virtual_pointer_manager_v1 *vp =
		wlr_virtual_pointer_manager_v1_create(server->display);
	new_virtual_pointer.notify = on_new_virtual_pointer;
	wl_signal_add(&vp->events.new_virtual_pointer, &new_virtual_pointer);

	wl_event_loop_add_signal(server->event_loop, SIGUSR1, dump_state, NULL);
	wl_event_loop_add_signal(server->event_loop, SIGUSR2, unplug_output, NULL);
}

void testhook_fini(void)
{
	if (!test_server)
		return;
	wl_list_remove(&new_virtual_keyboard.link);
	wl_list_remove(&new_virtual_pointer.link);
}
