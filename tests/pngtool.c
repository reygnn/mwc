/*
 * PNG helper for the wallpaper test.
 *
 *   pngtool stripes OUT W H OUTER INNER  image with outer quarters in colour
 *                                        OUTER and the middle half in INNER
 *   pngtool pixel FILE X Y               prints the pixel as rrggbb
 *   pngtool count FILE X Y W H RRGGBB    prints how many pixels have that colour
 *
 * Colours are rrggbb. Each quarter is measured along the width.
 */
#include <cairo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_color(cairo_t *cr, const char *hex)
{
	unsigned long v = strtoul(hex, NULL, 16);
	cairo_set_source_rgb(cr, ((v >> 16) & 0xff) / 255.0,
		((v >> 8) & 0xff) / 255.0, (v & 0xff) / 255.0);
}

static int stripes(const char *out, int w, int h, const char *outer,
	const char *inner)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	cairo_t *cr = cairo_create(s);
	set_color(cr, outer);
	cairo_paint(cr);
	set_color(cr, inner);
	cairo_rectangle(cr, w / 4, 0, w / 2, h);
	cairo_fill(cr);
	cairo_destroy(cr);
	cairo_status_t status = cairo_surface_write_to_png(s, out);
	cairo_surface_destroy(s);
	return status == CAIRO_STATUS_SUCCESS ? 0 : 1;
}

static int pixel(const char *file, int x, int y)
{
	cairo_surface_t *s = cairo_image_surface_create_from_png(file);
	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS ||
			x < 0 || y < 0 ||
			x >= cairo_image_surface_get_width(s) ||
			y >= cairo_image_surface_get_height(s))
		return 1;
	cairo_surface_flush(s);
	unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	uint32_t value = *(uint32_t *)(data + y * stride + x * 4);
	printf("%06x\n", value & 0xffffff);
	cairo_surface_destroy(s);
	return 0;
}

static int count(const char *file, int x, int y, int w, int h, const char *hex)
{
	cairo_surface_t *s = cairo_image_surface_create_from_png(file);
	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS)
		return 1;
	cairo_surface_flush(s);
	unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	uint32_t want = strtoul(hex, NULL, 16);
	int n = 0;
	for (int py = y; py < y + h && py < cairo_image_surface_get_height(s); py++) {
		for (int px = x; px < x + w &&
				px < cairo_image_surface_get_width(s); px++) {
			uint32_t v = *(uint32_t *)(data + py * stride + px * 4);
			n += (v & 0xffffff) == want;
		}
	}
	printf("%d\n", n);
	cairo_surface_destroy(s);
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc == 7 && strcmp(argv[1], "stripes") == 0)
		return stripes(argv[2], atoi(argv[3]), atoi(argv[4]), argv[5],
			argv[6]);
	if (argc == 5 && strcmp(argv[1], "pixel") == 0)
		return pixel(argv[2], atoi(argv[3]), atoi(argv[4]));
	if (argc == 8 && strcmp(argv[1], "count") == 0)
		return count(argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5]),
			atoi(argv[6]), argv[7]);
	fprintf(stderr, "usage: pngtool stripes OUT W H OUTER INNER\n"
		"       pngtool pixel FILE X Y\n"
		"       pngtool count FILE X Y W H RRGGBB\n");
	return 2;
}
