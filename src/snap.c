/*
 * Windows-style snapping. Dragging a window with the pointer against an output's
 * edge snaps it to that half, against a corner to that quarter, against the top
 * edge to the whole usable area. A Motif-bevelled outline shows the target while
 * dragging. Super+arrow keys step between the same positions, as in Windows 11.
 * Halves and quarters keep MWC_GAP to the edges and to each other, as the tiling
 * layout does, so the desktop stays in reach; maximized is the whole area. Where
 * each of these goes, snaprules.c works out; this file asks it and acts.
 */
#define _POSIX_C_SOURCE 200809L
#include <wlr/util/box.h>
#include "mwc.h"

#define PREVIEW_W 8 /* outline thickness */

/* Four strips: the inside is empty, and one picture of the whole box would be the
 * output's size, 32 MB at 4K, made anew with each zone. */
static struct wlr_scene_tree *preview;
static struct wlr_scene_buffer *preview_strip[4];
static struct wlr_box preview_box;
static bool preview_visible;

struct wlr_box snap_area(struct server *server, enum snap s, double px, double py)
{
	struct output *o = output_at(server, px, py);
	return o ? snap_area_on(o, dock_horizontal(), s) : output_usable_area(server, px, py);
}

enum snap snap_zone_at(struct view *view, double px, double py)
{
	return snap_zone(output_full_area(view->server, px, py), view->funcs,
		view->fullscreen, px, py);
}

/* The outline of snap position s on the output at (px, py), hidden for SNAP_NONE:
 * raised outside, sunken towards the inside, in the dragged window's colour. The
 * inside of the buffer is transparent. */
void snap_preview(struct view *view, enum snap s, double px, double py)
{
	if (s == SNAP_NONE) {
		snap_preview_hide();
		return;
	}
	snap_preview_box(view, snap_rect(s, snap_area(view->server, s, px, py)));
}

bool scene_buffer_no_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	return false;
}

void snap_preview_box(struct view *view, struct wlr_box r)
{
	struct server *server = view->server;
	const int t = PREVIEW_W;
	const struct mwc_shade *sh = &view->shade_active;

	if (!preview) {
		preview = wlr_scene_tree_create(server->layer_feedback);
		if (!preview)
			return;
		for (int i = 0; i < 4; i++) {
			preview_strip[i] = wlr_scene_buffer_create(preview, NULL);
			if (preview_strip[i])
				preview_strip[i]->point_accepts_input = scene_buffer_no_input;
		}
	}
	const int st = t < r.height / 2 ? t : r.height / 2;
	const int sl = t < r.width / 2 ? t : r.width / 2;
	const struct wlr_box strips[4] = {
		{ 0, 0, r.width, st }, { 0, r.height - st, r.width, st },
		{ 0, st, sl, r.height - 2 * st }, { r.width - sl, st, sl, r.height - 2 * st },
	};
	for (int i = 0; i < 4; i++) {
		const struct wlr_box *b = &strips[i];
		if (!preview_strip[i])
			continue;
		struct cairo_buffer *buf = b->width > 0 && b->height > 0 ?
			cairo_buffer_create(b->width, b->height) : NULL;
		if (buf) {
			/* The whole ring, of which the strip shows its part. */
			cairo_t *cr = buf->cr;
			set_source(cr, sh->bg);
			cairo_paint(cr);
			cairo_translate(cr, -b->x, -b->y);
			draw_bevel(cr, 0, 0, r.width, r.height, theme.bevel, sh, false);
			draw_bevel(cr, t - 2, t - 2, r.width - 2 * (t - 2),
				r.height - 2 * (t - 2), 2, sh, true);
		}
		wlr_scene_buffer_set_buffer(preview_strip[i], buf ? &buf->base : NULL);
		if (buf)
			cairo_buffer_drop(buf);
		wlr_scene_node_set_position(&preview_strip[i]->node, b->x, b->y);
	}
	/* Where the window goes stays visible over the target. */
	wlr_scene_node_lower_to_bottom(&preview->node);
	wlr_scene_node_set_position(&preview->node, r.x, r.y);
	wlr_scene_node_set_enabled(&preview->node, true);
	preview_box = r;
	preview_visible = true;
}

void snap_preview_hide(void)
{
	if (preview)
		wlr_scene_node_set_enabled(&preview->node, false);
	preview_visible = false;
}

bool snap_preview_get(struct wlr_box *box)
{
	*box = preview_box;
	return preview_visible;
}

void snap_step(struct view *view, enum snap_dir dir)
{
	if (!view || view->minimized || view->fullscreen ||
			!(view->funcs & FUNC_RESIZE))
		return;
	struct snap_move m = snap_next(view->snap, view->maximized, view->funcs, dir);
	if (m.minimize)
		view_minimize(view);
	else if (m.snap)
		view_snap(view, m.to, view->x + (view->width + frame_extra_w(view)) / 2.0,
			view->y + (view->height + frame_extra_h(view)) / 2.0);
}

/* Super+Shift+arrow: push a floating window flush against that edge of its output's
 * area, the dock's strip included beside the dock; two in a row reach a corner. */
void view_move_to_edge(struct view *view, enum snap_dir dir)
{
	if (!view || view->minimized || view->fullscreen || view->maximized ||
			view->snap != SNAP_NONE || !(view->funcs & FUNC_MOVE))
		return;
	struct wlr_box f = { view->x, view->y, view->width + frame_extra_w(view),
		view->height + frame_extra_h(view) };
	double cx = f.x + f.width / 2.0, cy = f.y + f.height / 2.0;
	struct wlr_box b = snap_edge_place(output_at(view->server, cx, cy),
		output_usable_area(view->server, cx, cy), dir, f);
	view->gravity_edges = 0;
	view_set_position(view, b.x, b.y);
}
