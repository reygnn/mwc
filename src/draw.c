/*
 * Theme colours, Motif bevel primitives and a cairo image surface wrapped
 * as a wlr_buffer so the scene graph can display what we draw.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include "mwc.h"

struct mwc_theme theme;

static struct mwc_color hex(uint32_t rgb)
{
	return (struct mwc_color){
		((rgb >> 16) & 0xff) / 255.0f,
		((rgb >> 8) & 0xff) / 255.0f,
		(rgb & 0xff) / 255.0f,
	};
}

static struct mwc_color parse_env_color(const char *var, uint32_t fallback)
{
	const char *s = getenv(var);
	if (s) {
		if (*s == '#')
			s++;
		char *end;
		unsigned long v = strtoul(s, &end, 16);
		if (*end == '\0' && end - s == 6)
			return hex((uint32_t)v);
	}
	return hex(fallback);
}

/*
 * Derive the top/bottom shadow colours from a background the way Motif's
 * XmeGetDefaultThreeDColors roughly does: lighten towards white for the
 * top shadow, darken towards black for the bottom shadow.
 */
static struct mwc_shade shade_from(struct mwc_color bg, struct mwc_color fg)
{
	struct mwc_shade s = { .bg = bg, .fg = fg };
	s.top.r = bg.r + (1.0f - bg.r) * 0.55f;
	s.top.g = bg.g + (1.0f - bg.g) * 0.55f;
	s.top.b = bg.b + (1.0f - bg.b) * 0.55f;
	s.bottom.r = bg.r * 0.5f;
	s.bottom.g = bg.g * 0.5f;
	s.bottom.b = bg.b * 0.5f;
	return s;
}

void theme_init(void)
{
	/* Defaults follow the Motif 2.x look: #aeb2c3 grey-blue background,
	 * a stronger blue for the window that has the focus. Each one can be
	 * overridden through the environment, e.g. MWC_ACTIVE=#b24d7a. */
	struct mwc_color inactive = parse_env_color("MWC_INACTIVE", 0xaeb2c3);
	struct mwc_color active = parse_env_color("MWC_ACTIVE", 0x7387ad);
	struct mwc_color root = parse_env_color("MWC_ROOT", 0x5b7792);

	theme.inactive = shade_from(inactive, hex(0x000000));
	theme.active = shade_from(active, hex(0xffffff));
	theme.menu = shade_from(inactive, hex(0x000000));
	theme.icon = shade_from(inactive, hex(0x000000));
	theme.select = active;
	theme.root = root;
	theme.font = getenv("MWC_FONT") ? getenv("MWC_FONT") : "Sans Bold 10";
	theme.border = 6;
	theme.title_height = 22;
	theme.bevel = 2;
}

void set_source(cairo_t *cr, struct mwc_color c)
{
	cairo_set_source_rgb(cr, c.r, c.g, c.b);
}

void draw_bevel(cairo_t *cr, double x, double y, double w, double h,
	int t, const struct mwc_shade *s, bool in)
{
	struct mwc_color tl = in ? s->bottom : s->top;
	struct mwc_color br = in ? s->top : s->bottom;

	/* Each shadow is a polygon so the diagonal joins at the top-right
	 * and bottom-left corners look exactly like Motif's. */
	set_source(cr, tl);
	cairo_move_to(cr, x, y);
	cairo_line_to(cr, x + w, y);
	cairo_line_to(cr, x + w - t, y + t);
	cairo_line_to(cr, x + t, y + t);
	cairo_line_to(cr, x + t, y + h - t);
	cairo_line_to(cr, x, y + h);
	cairo_close_path(cr);
	cairo_fill(cr);

	set_source(cr, br);
	cairo_move_to(cr, x + w, y + h);
	cairo_line_to(cr, x, y + h);
	cairo_line_to(cr, x + t, y + h - t);
	cairo_line_to(cr, x + w - t, y + h - t);
	cairo_line_to(cr, x + w - t, y + t);
	cairo_line_to(cr, x + w, y);
	cairo_close_path(cr);
	cairo_fill(cr);
}

void draw_groove_h(cairo_t *cr, double x, double y, double w,
	const struct mwc_shade *s)
{
	set_source(cr, s->bottom);
	cairo_rectangle(cr, x, y, w, 1);
	cairo_fill(cr);
	set_source(cr, s->top);
	cairo_rectangle(cr, x, y + 1, w, 1);
	cairo_fill(cr);
}

void draw_groove_v(cairo_t *cr, double x, double y, double h,
	const struct mwc_shade *s)
{
	set_source(cr, s->bottom);
	cairo_rectangle(cr, x, y, 1, h);
	cairo_fill(cr);
	set_source(cr, s->top);
	cairo_rectangle(cr, x + 1, y, 1, h);
	cairo_fill(cr);
}

/* ---- cairo_buffer ------------------------------------------------------- */

static void cairo_buffer_destroy(struct wlr_buffer *wlr_buffer)
{
	struct cairo_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	wlr_buffer_finish(wlr_buffer);
	cairo_destroy(buf->cr);
	cairo_surface_destroy(buf->surface);
	free(buf);
}

static bool cairo_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
	uint32_t flags, void **data, uint32_t *format, size_t *stride)
{
	struct cairo_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
		return false;
	cairo_surface_flush(buf->surface);
	*data = cairo_image_surface_get_data(buf->surface);
	*format = DRM_FORMAT_ARGB8888;
	*stride = cairo_image_surface_get_stride(buf->surface);
	return true;
}

static void cairo_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer)
{
	(void)wlr_buffer;
}

static const struct wlr_buffer_impl cairo_buffer_impl = {
	.destroy = cairo_buffer_destroy,
	.begin_data_ptr_access = cairo_buffer_begin_data_ptr_access,
	.end_data_ptr_access = cairo_buffer_end_data_ptr_access,
};

struct cairo_buffer *cairo_buffer_create(int width, int height)
{
	if (width < 1)
		width = 1;
	if (height < 1)
		height = 1;
	struct cairo_buffer *buf = calloc(1, sizeof(*buf));
	if (!buf)
		return NULL;
	wlr_buffer_init(&buf->base, &cairo_buffer_impl, width, height);
	buf->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		width, height);
	buf->cr = cairo_create(buf->surface);
	/* Crisp pixel edges for bevels; text sets its own antialiasing. */
	cairo_set_antialias(buf->cr, CAIRO_ANTIALIAS_NONE);
	return buf;
}

void cairo_buffer_drop(struct cairo_buffer *buf)
{
	wlr_buffer_drop(&buf->base);
}
