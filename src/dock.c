/*
 * The dock, Mwm*NAME*dock: windows of small tools, side by side in a frame at an edge
 * of the first output, as FvwmButtons swallowed them. The tools stay windows of their
 * own and notice nothing; mwc only decides where they are.
 *
 *   +-----------------------------+
 *   | +--------+ +--------------+ |
 *   | | tool 2 | |    tool 1    | |   "right bottom": tool 1, the first mwmrc line,
 *   | +--------+ +--------------+ |   in the corner, the next to its left
 *   +-----------------------------+
 *
 * - Where and which way: Mwm*dockPlacement, iconPlacement's syntax without "tight",
 *   default "right bottom". At the output's edge, not inside panels.
 * - Arranged anew when a tool appears, goes or changes its size, and on a reload.
 * - In a row the tools align at the dock's outer edge, in a column likewise.
 * - The strip is kept free like a panel's (dock_reserve(), from layer_arrange()).
 * - Normal top-level windows only: a tool's dialogs open outside.
 * - Docked windows are on every desktop, above normal windows, out of Alt+Tab, tiling
 *   and tab groups, and can't be moved, resized, minimized, maximized or made
 *   fullscreen (see the callers of view_docked()).
 */
#include <stdlib.h>
#include <wlr/types/wlr_output_layout.h>
#include "mwc.h"

static struct {
	/* Between normal and docked windows, in a layer of its own: lowering a docked
	 * window can't put it under the frame. */
	struct wlr_scene_buffer *frame;
	int drawn_w, drawn_h;  /* 0: to be drawn anew */
	struct output *output; /* the one the dock is on, or NULL */
	struct wlr_box box;    /* the dock with its frame; empty without tools */
} dk;

static int outer_w(struct view *v)
{
	return v->width + frame_extra_w(v);
}

static int outer_h(struct view *v)
{
	return frame_height(v);
}

/* Docked windows never tile, so tile_seq is only the order they were mapped in. */
static int by_dock_order(const void *a, const void *b)
{
	const struct view *x = *(struct view *const *)a, *y = *(struct view *const *)b;
	if (x->dock_order != y->dock_order)
		return x->dock_order < y->dock_order ? -1 : 1;
	return (x->tile_seq < y->tile_seq) - (x->tile_seq > y->tile_seq); /* older first */
}

static void render_frame(int w, int h)
{
	if (w == dk.drawn_w && h == dk.drawn_h)
		return;
	struct cairo_buffer *buf = cairo_buffer_create(w, h);
	if (!buf)
		return;
	const struct mwc_shade *s = &theme.inactive;
	set_source(buf->cr, s->bg);
	cairo_paint(buf->cr);
	draw_bevel(buf->cr, 0, 0, w, h, theme.bevel, s, false);
	wlr_scene_buffer_set_buffer(dk.frame, &buf->base);
	cairo_buffer_drop(buf);
	dk.drawn_w = w;
	dk.drawn_h = h;
}

/* Only when it changed: a new strip changes the usable area, which refits every
 * maximized window and the tiling layout. A tool changing its size with every frame
 * is throttled as a panel is. */
static void set_box(struct output *output, struct wlr_box box)
{
	struct output *old = dk.output;
	if (old == output && wlr_box_equal(&dk.box, &box))
		return;
	dk.output = output;
	dk.box = box;
	if (old && old != output)
		layer_arrange(old);
	if (output)
		layer_arrange_soon(output);
}

void dock_arrange(struct server *server)
{
	int count = wl_list_length(&server->views), n = 0;
	struct view **list = count ? calloc(count, sizeof(*list)) : NULL;
	struct view *v;
	if (list) {
		wl_list_for_each(v, &server->views, link) {
			if (v->mapped && view_docked(v) && !view_hidden_by_state(v))
				list[n++] = v;
		}
	}
	struct output *output = output_first(server);
	if (!n || !output) {
		free(list);
		wlr_scene_node_set_enabled(&dk.frame->node, false);
		set_box(NULL, (struct wlr_box){ 0 });
		return;
	}
	qsort(list, n, sizeof(*list), by_dock_order);

	const struct config_placement *p = config_dock_placement();
	const int B = frame_thin_border();
	int along = 0, across = 0;
	for (int i = 0; i < n; i++) {
		int a = p->horizontal ? outer_w(list[i]) : outer_h(list[i]);
		int c = p->horizontal ? outer_h(list[i]) : outer_w(list[i]);
		along += a;
		if (c > across)
			across = c;
	}
	int w = 2 * B + (p->horizontal ? along : across);
	int h = 2 * B + (p->horizontal ? across : along);
	struct wlr_box full;
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &full);
	struct wlr_box box = {
		p->from_right ? full.x + full.width - w : full.x,
		p->from_bottom ? full.y + full.height - h : full.y,
		w, h,
	};
	if (p->center) {
		if (p->horizontal)
			box.x = full.x + (full.width - w) / 2;
		else
			box.y = full.y + (full.height - h) / 2;
	}
	/* From the corner the placement names; centred, from the left or top. */
	bool from_end = p->horizontal ? p->from_right && !p->center :
		p->from_bottom && !p->center;
	int pos = from_end ? (p->horizontal ? box.x + w - B : box.y + h - B) :
		(p->horizontal ? box.x + B : box.y + B);
	for (int i = 0; i < n; i++) {
		v = list[i];
		int ow = outer_w(v), oh = outer_h(v), x, y;
		if (p->horizontal) {
			x = from_end ? pos - ow : pos;
			pos = from_end ? x : pos + ow;
			/* Flush with the dock's outer edge. */
			y = p->from_bottom ? box.y + h - B - oh : box.y + B;
		} else {
			y = from_end ? pos - oh : pos;
			pos = from_end ? y : pos + oh;
			x = p->from_right ? box.x + w - B - ow : box.x + B;
		}
		if (x != v->x || y != v->y)
			view_set_position(v, x, y);
	}
	free(list);

	render_frame(w, h);
	wlr_scene_node_set_position(&dk.frame->node, box.x, box.y);
	wlr_scene_node_set_enabled(&dk.frame->node, true);
	set_box(output, box);
}

void dock_reserve(struct output *output, struct wlr_box *usable)
{
	if (output != dk.output || dk.box.width <= 0)
		return;
	const struct config_placement *p = config_dock_placement();
	struct wlr_box b = dk.box;
	int left = usable->x, top = usable->y;
	int right = usable->x + usable->width, bottom = usable->y + usable->height;
	if (p->horizontal && p->from_bottom) {
		if (b.y < bottom)
			bottom = b.y;
	} else if (p->horizontal) {
		if (b.y + b.height > top)
			top = b.y + b.height;
	} else if (p->from_right) {
		if (b.x < right)
			right = b.x;
	} else if (b.x + b.width > left) {
		left = b.x + b.width;
	}
	*usable = (struct wlr_box){ left, top, right - left, bottom - top };
}

void dock_admit(struct view *view)
{
	if (view->group)
		tabs_detach(view);
	if (view->minimized)
		view_restore(view);
	if (view->tiled)
		view_set_tiled(view, false, false);
	if (view->fullscreen)
		view_set_fullscreen(view, false);
	if (view->maximized || view->snap != SNAP_NONE)
		view_snap(view, SNAP_NONE, 0, 0);
}

void dock_view_resized(struct view *view)
{
	if (view->mapped && view_docked(view))
		dock_arrange(view->server);
}

void dock_view_changed(struct view *view)
{
	if (!view->mapped)
		return;
	int order = config_view_dock(view);
	if (order == view->dock_order)
		return;
	bool joins = order >= 0 && !view_docked(view);
	view->dock_order = order;
	if (joins)
		dock_admit(view);
	view_update_decorations(view); /* docked windows lose functions */
	config_view_tiling(view);
	views_update_layers(view->server);
	desktop_recheck(view);
	tile_arrange(view->server);
	dock_arrange(view->server);
}

void dock_config_changed(struct server *server)
{
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		if (!v->mapped)
			continue;
		if (view_docked(v))
			dock_admit(v);
		view_update_decorations(v);
	}
	views_update_layers(server);
	dk.drawn_w = 0; /* the theme may have changed */
	dock_arrange(server);
	/* Another placement may keep the box and reserve another edge. */
	if (dk.output)
		layer_arrange(dk.output);
}

void dock_output_gone(struct output *output)
{
	if (dk.output != output)
		return;
	dk.output = NULL;
	dk.box = (struct wlr_box){ 0 };
}

bool dock_contains(double lx, double ly)
{
	return dk.box.width > 0 && wlr_box_contains_point(&dk.box, lx, ly);
}

void dock_state(FILE *f)
{
	fprintf(f, "dock x=%d y=%d w=%d h=%d\n", dk.box.x, dk.box.y, dk.box.width,
		dk.box.height);
}

void dock_init(struct server *server)
{
	dk.frame = wlr_scene_buffer_create(&server->scene->tree, NULL);
	wlr_scene_node_set_enabled(&dk.frame->node, false);
}
