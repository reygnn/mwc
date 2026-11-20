/*
 * Hooks for the headless input tests, active only when MWC_TEST_STATE names a
 * file. They stay off in normal use because the virtual input protocols let
 * any client type and click on the user's behalf.
 *
 * - zwp_virtual_keyboard_v1 and zwlr_virtual_pointer_v1 feed simulated devices
 *   into the normal input path (see tests/vinput.c, and input_virtual_init()).
 * - SIGUSR1 writes the window state to the file so tests can check geometry
 *   and focus, which a Wayland client cannot query for other clients.
 * - SIGUSR2 removes an output: the newest, or the one named in the file
 *   <state file>.unplug if there is one (read and removed then).
 */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include "mwc.h"

static struct server *test_server;
static const char *state_path;

/* SIGUSR2 unplugs an output, which the headless backend can't do by itself: the
 * newest, or the one <state file>.unplug names. */
static int unplug_output(int signal, void *data)
{
	if (wl_list_empty(&test_server->outputs))
		return 0;
	struct output *target = wl_container_of(test_server->outputs.next, target, link);
	char path[4096], name[64] = "";
	snprintf(path, sizeof(path), "%s.unplug", state_path);
	FILE *f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%63s", name) != 1)
			name[0] = '\0';
		fclose(f);
		remove(path);
	}
	if (name[0]) {
		struct output *o;
		target = NULL;
		wl_list_for_each(o, &test_server->outputs, link) {
			if (!strcmp(o->wlr_output->name, name))
				target = o;
		}
		if (!target) {
			wlr_log(WLR_ERROR, "test hook: no output %s to unplug", name);
			return 0;
		}
	}
	wlr_output_destroy(target->wlr_output);
	return 0;
}

/* Who a surface belongs to, for the seat line: a window's app, something else (a layer
 * surface, a popup), or nobody. */
static const char *surface_owner(struct server *server, struct wlr_surface *surface)
{
	if (!surface)
		return "none";
	surface = wlr_surface_get_root_surface(surface);
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		if (v->mapped && view_surface(v) == surface)
			return view_app_id(v);
	}
	return "other";
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
			"bw=%d th=%d snap=%d tiled=%d tile_main=%d shaded=%d "
			"fh=%d tab=%d tab_hidden=%d visible=%d tab_first=%d desktop=%d "
			"icon_shown=%d everywhere=%d rseq=%ld tlen=%zu tdraws=%d tdrawn=%zu "
			"group=%u cell=%d,%d,%d,%d sticky=%d id=%u root=%u\n",
			view_app_id(v), v->x, v->y, v->width, v->height,
			v->activated, kb_focus && kb_focus == view_surface(v),
			v->maximized, v->minimized, v->fullscreen, view_decorated(v),
			v->icon_x, v->icon_y, v->hidden_with_parent,
			frame_border(v), frame_title(v), v->snap,
			v->tiled, v->tile_main, v->shaded, frame_height(v), tabs_index(v),
			v->tab_hidden, v->tree->node.enabled,
			v->group ? v->group->first_visible : 0, v->desktop + 1,
			v->icon_node->node.enabled, view_on_all_desktops(v), v->raise_seq,
			strlen(view_title(v)), v->title_draws, v->title_drawn_len,
			v->group ? v->group->id : 0, v->tile.x, v->tile.y, v->tile.width,
			v->tile.height, v->sticky, v->id, view_transient_root(v)->id);
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
	const struct menu *m = &server->menu;
	fprintf(f, "menu open=%d x=%d y=%d h=%d items=%d title=%s sensitive=%s armed=%d "
		"ay=%d renders=%d\n", m->open, m->x, m->y, m->height, m->n_items,
		m->open && m->title ? "yes" : "no", sensitive, m->armed,
		m->open && m->armed >= 0 ? m->y + m->item_y[m->armed] : -1, m->renders);
	struct output *o = output_at(server, 0, 0);
	if (o)
		fprintf(f, "output ux=%d uy=%d uw=%d uh=%d on=%d\n", o->usable.x,
			o->usable.y, o->usable.width, o->usable.height,
			o->wlr_output->enabled);
	iconbox_state(f);
	dock_state(f);
	struct output *each;
	wl_list_for_each(each, &server->outputs, link) {
		struct wlr_box b;
		wlr_output_layout_get_box(server->output_layout, each->wlr_output, &b);
		fprintf(f, "output_%s x=%d y=%d w=%d h=%d scale=%d\n", each->wlr_output->name,
			b.x, b.y, b.width, b.height, (int)(each->wlr_output->scale * 100));
	}
	struct output *first = output_first(server);
	struct wlr_box ta = first ? tile_layout_area(server) : (struct wlr_box){ 0 };
	struct wlr_box fu = first ? first->usable : (struct wlr_box){ 0 };
	fprintf(f, "tiling on=%d area=%d,%d,%d,%d usable=%d,%d,%d,%d gap=%d\n",
		server->tiling, ta.x, ta.y, ta.width, ta.height, fu.x, fu.y, fu.width,
		fu.height, MWC_GAP);
	fprintf(f, "desktop current=%d\n", server->desktop + 1);
	int focused = 0;
	wl_list_for_each(v, &server->views, link)
		focused += v->activated;
	fprintf(f, "focus any=%d layer=%d\n", focused, server->focused_layer != NULL);
	struct wlr_box box;
	bool outline = outline_get(&box);
	fprintf(f, "outline shown=%d x=%d y=%d w=%d h=%d\n", outline, box.x, box.y,
		box.width, box.height);
	bool zone = snap_preview_get(&box);
	fprintf(f, "preview shown=%d x=%d y=%d w=%d h=%d\n", zone, box.x, box.y,
		box.width, box.height);
	/* Spaces become _, for the parser. */
	int draws;
	const char *text = feedback_get(&box, &draws);
	char label[64];
	snprintf(label, sizeof(label), "%s", text);
	for (char *p = label; *p; p++)
		*p = *p == ' ' ? '_' : *p;
	fprintf(f, "feedback text=%s x=%d y=%d draws=%d\n", label, box.x, box.y, draws);
	fprintf(f, "wallpaper decodes=%d\n", server->wallpaper_decodes);
	fprintf(f, "theme border=%d title=%d\n", theme.border,
		theme.title_height);
	/* A client's own cursor image, while it is shown: its buffer and scale. */
	struct wlr_surface *cs = server->cursor_name ? NULL : server->client_cursor;
	fprintf(f, "cursor x=%d y=%d mode=%d cw=%d ch=%d cscale=%d name=%s\n",
		(int)server->cursor->x, (int)server->cursor->y, server->cursor_mode,
		cs ? cs->current.buffer_width : 0, cs ? cs->current.buffer_height : 0,
		cs ? cs->current.scale : 0,
		server->cursor_name ? server->cursor_name : "client");
	fprintf(f, "seat pointer=%s keyboard=%s caps=%u\n",
		surface_owner(server, server->seat->pointer_state.focused_surface),
		surface_owner(server, server->seat->keyboard_state.focused_surface),
		server->seat->capabilities);
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
	if (!state_path)
		return;
	/* It is for this process, not for what it starts. */
	unsetenv("MWC_TEST_STATE");
	wlr_log(WLR_INFO, "test hooks on, state file %s", state_path);
	test_server = server;

	input_virtual_init(server);

	wl_event_loop_add_signal(server->event_loop, SIGUSR1, dump_state, NULL);
	wl_event_loop_add_signal(server->event_loop, SIGUSR2, unplug_output, NULL);
}
