/*
 * Desktop wallpaper from MWC_WALLPAPER (a PNG file). Each output shows the
 * image scaled to cover it: the aspect ratio stays, what sticks out is cut off
 * evenly. Without a usable image the desktop keeps its plain colour.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include "mwc.h"

void wallpaper_init(struct server *server)
{
	const char *value = getenv("MWC_WALLPAPER");
	if (!value || !*value)
		return;

	/* A shell expands ~ only in some places: a display manager's session
	 * environment or a quoted value still has it. */
	char path[4096];
	const char *home = getenv("HOME");
	if (strncmp(value, "~/", 2) == 0 && home)
		snprintf(path, sizeof(path), "%s/%s", home, value + 2);
	else
		snprintf(path, sizeof(path), "%s", value);

	cairo_surface_t *image = cairo_image_surface_create_from_png(path);
	cairo_status_t status = cairo_surface_status(image);
	if (status != CAIRO_STATUS_SUCCESS) {
		wlr_log(WLR_ERROR, "cannot load wallpaper %s: %s", path,
			cairo_status_to_string(status));
		cairo_surface_destroy(image);
		return;
	}
	server->wallpaper = image;
}

void wallpaper_fini(struct server *server)
{
	if (server->wallpaper)
		cairo_surface_destroy(server->wallpaper);
	server->wallpaper = NULL;
}

void wallpaper_place(struct output *output, const struct wlr_box *box)
{
	cairo_surface_t *image = output->server->wallpaper;
	if (!image || !output->wallpaper)
		return;
	wlr_scene_node_set_position(&output->wallpaper->node, box->x, box->y);
	wlr_scene_buffer_set_dest_size(output->wallpaper, box->width, box->height);

	/* Render in device pixels, so a scaled output stays sharp. */
	int w = lround(box->width * output->wlr_output->scale);
	int h = lround(box->height * output->wlr_output->scale);
	if (w <= 0 || h <= 0 ||
			(w == output->wallpaper_w && h == output->wallpaper_h))
		return;

	struct cairo_buffer *buf = cairo_buffer_create(w, h);
	if (!buf)
		return;
	int iw = cairo_image_surface_get_width(image);
	int ih = cairo_image_surface_get_height(image);
	double scale = fmax((double)w / iw, (double)h / ih);
	cairo_t *cr = buf->cr;
	cairo_translate(cr, (w - iw * scale) / 2, (h - ih * scale) / 2);
	cairo_scale(cr, scale, scale);
	cairo_set_source_surface(cr, image, 0, 0);
	/* GOOD handles strong downscaling, PAD keeps the outermost pixels
	 * from blending into transparency. */
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
	cairo_paint(cr);

	wlr_scene_buffer_set_buffer(output->wallpaper, &buf->base);
	cairo_buffer_drop(buf);
	output->wallpaper_w = w;
	output->wallpaper_h = h;
}
