/*
 * Wireframe move and resize, as Motif mwm does with moveOpaque: False and twm by
 * default: while a window is dragged only a thin outline follows the pointer, and
 * the window itself moves and resizes once, when the drag ends. Also the little
 * box mwm shows in the middle of the screen, with "+x +y" while a window is moved
 * and its size while it is resized.
 *
 * The outline is made of thin strips (four sides and the title bar line), each a
 * buffer only as big as the strip itself. A move just moves them; a resize
 * repaints only the strips whose size changed.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pango/pangocairo.h>
#include <wlr/util/log.h>
#include "mwc.h"

#define OUTLINE_W 3 /* line thickness */

enum { STRIP_TOP, STRIP_BOTTOM, STRIP_LEFT, STRIP_RIGHT, STRIP_SEP, N_STRIPS };

static struct wlr_scene_buffer *strip[N_STRIPS];
/* What each strip shows now; width 0 while it is hidden. */
static struct wlr_box shown[N_STRIPS];
static struct wlr_box outline_box;
static bool outline_visible;

static struct wlr_scene_buffer *feedback_node;
static char feedback_shown[64];
static struct wlr_box feedback_box;

/* The environment wins over mwmrc, unless it holds no boolean. */
bool outline_wanted(void)
{
	int opaque = config_bool("MWC_MOVE_OPAQUE", getenv("MWC_MOVE_OPAQUE"));
	if (opaque < 0)
		opaque = config_bool("moveOpaque", config_get("moveOpaque"));
	return opaque != 1;
}

/* Paint a strip as its part of the whole outline: a small Motif bevel in the
 * window's colour, so the lines read on light and dark backgrounds alike. The
 * whole ring is drawn shifted into the strip's buffer, which clips it. */
static void render_strip(int i, struct wlr_box s, int w, int h, int sep,
	const struct mwc_shade *sh)
{
	const int t = OUTLINE_W;
	struct cairo_buffer *buf = cairo_buffer_create(s.width, s.height);
	if (!buf)
		return;
	cairo_t *cr = buf->cr;
	set_source(cr, sh->bg);
	cairo_paint(cr);
	draw_bevel(cr, -s.x, -s.y, w, h, 1, sh, false);
	draw_bevel(cr, t - 1 - s.x, t - 1 - s.y, w - 2 * (t - 1), h - 2 * (t - 1), 1,
		sh, true);
	if (sep)
		draw_bevel(cr, t - s.x, sep - s.y, w - 2 * t, t, 1, sh, false);
	wlr_scene_buffer_set_buffer(strip[i], &buf->base);
	cairo_buffer_drop(buf);
}

void outline_show(struct view *view, struct wlr_box b)
{
	struct server *server = view->server;
	const int t = OUTLINE_W;
	if (b.width < 2 * t + 1)
		b.width = 2 * t + 1;
	if (b.height < 2 * t + 1)
		b.height = 2 * t + 1;
	/* The line between title bar and client, as twm and mwm draw it. */
	int sep = 0;
	if (frame_title(view)) {
		sep = frame_top(view) - (t + 1) / 2;
		if (sep < t || sep + t > b.height - t)
			sep = 0;
	}
	const struct wlr_box rel[N_STRIPS] = {
		[STRIP_TOP] = { 0, 0, b.width, t },
		[STRIP_BOTTOM] = { 0, b.height - t, b.width, t },
		[STRIP_LEFT] = { 0, t, t, b.height - 2 * t },
		[STRIP_RIGHT] = { b.width - t, t, t, b.height - 2 * t },
		[STRIP_SEP] = { t, sep, sep ? b.width - 2 * t : 0, t },
	};
	for (int i = 0; i < N_STRIPS; i++) {
		struct wlr_box s = rel[i];
		if (!strip[i]) {
			strip[i] = wlr_scene_buffer_create(server->layer_feedback, NULL);
			strip[i]->point_accepts_input = scene_buffer_no_input;
		}
		if (!s.width) {
			wlr_scene_node_set_enabled(&strip[i]->node, false);
			shown[i].width = 0;
			continue;
		}
		/* A strip's picture depends only on its size. */
		if (s.width != shown[i].width || s.height != shown[i].height)
			render_strip(i, s, b.width, b.height, sep, &view->shade_active);
		shown[i] = (struct wlr_box){ b.x + s.x, b.y + s.y, s.width, s.height };
		wlr_scene_node_set_position(&strip[i]->node, shown[i].x, shown[i].y);
		wlr_scene_node_set_enabled(&strip[i]->node, true);
	}
	outline_box = b;
	outline_visible = true;
}

void outline_hide(void)
{
	for (int i = 0; i < N_STRIPS; i++) {
		if (strip[i])
			wlr_scene_node_set_enabled(&strip[i]->node, false);
	}
	/* Forgetting the sizes repaints every strip on the next show, in the
	 * colour of whichever window is dragged then. */
	memset(shown, 0, sizeof(shown));
	outline_visible = false;
}

bool outline_get(struct wlr_box *box)
{
	*box = outline_box;
	return outline_visible;
}

/* ---- feedback ------------------------------------------------------------ */

static void text_size(const char *text, int *w, int *h)
{
	cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = cairo_create(tmp);
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(theme.font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);
	pango_layout_get_pixel_size(layout, w, h);
	g_object_unref(layout);
	cairo_destroy(cr);
	cairo_surface_destroy(tmp);
}

void feedback_show(struct server *server, const char *text, double lx, double ly)
{
	struct wlr_box m = output_full_area(server, lx, ly);
	int tw, th;
	text_size(text, &tw, &th);
	int w = tw + 24 > 80 ? tw + 24 : 80, h = th + 14;

	if (!feedback_node) {
		feedback_node = wlr_scene_buffer_create(server->layer_feedback, NULL);
		feedback_node->point_accepts_input = scene_buffer_no_input;
	}
	feedback_box = (struct wlr_box){ m.x + (m.width - w) / 2,
		m.y + (m.height - h) / 2, w, h };
	wlr_scene_node_set_position(&feedback_node->node, feedback_box.x,
		feedback_box.y);
	wlr_scene_node_set_enabled(&feedback_node->node, true);
	if (strcmp(text, feedback_shown) == 0)
		return;
	snprintf(feedback_shown, sizeof(feedback_shown), "%s", text);

	const struct mwc_shade *s = &theme.inactive;
	struct cairo_buffer *buf = cairo_buffer_create(w, h);
	if (!buf)
		return;
	cairo_t *cr = buf->cr;
	set_source(cr, s->bg);
	cairo_paint(cr);
	draw_bevel(cr, 0, 0, w, h, theme.bevel, s, false);
	draw_bevel(cr, 4, 4, w - 8, h - 8, 1, s, true);
	draw_text(cr, text, theme.font, s->fg, 6, 4, w - 12, h - 8, true);
	wlr_scene_buffer_set_buffer(feedback_node, &buf->base);
	cairo_buffer_drop(buf);
}

void feedback_hide(void)
{
	if (feedback_node)
		wlr_scene_node_set_enabled(&feedback_node->node, false);
	feedback_shown[0] = '\0';
}

const char *feedback_get(struct wlr_box *box)
{
	*box = feedback_box;
	return feedback_shown;
}
