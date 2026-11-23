/*
 * outputrc.c - what mwmrc says about an output, outputMode, outputScale,
 * outputTransform and outputPosition, from text to values, and when input wakes the
 * outputs. Nothing here touches an output: output.c applies them and logs what it
 * ignores. Tested in tests/unit.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <strings.h>
#include <wlr/types/wlr_output.h>
#include "mwc.h"

/* The most a cairo or GL buffer can be. It also keeps sums of a position and a size
 * from overflowing. */
#define MODE_MAX 32767
/* Positions; with a size up to MODE_MAX their sums stay well within int. */
#define POSITION_MAX 1000000

/* strtol, not %d: past the range of int that would be undefined. */
bool outputrc_mode(const char *value, int *pw, int *ph, double *phz)
{
	char *end;
	long w = strtol(value, &end, 10);
	if (end == value || *end != 'x')
		return false;
	const char *hs = end + 1;
	long h = strtol(hs, &end, 10);
	if (end == hs)
		return false;
	double hz = 0;
	if (*end == '@') {
		const char *zs = end + 1;
		hz = strtod(zs, &end);
		/* Written so that NaN fails it too. */
		if (end == zs || !(hz > 0 && hz <= 1000))
			return false;
	}
	if (*end || w < 1 || w > MODE_MAX || h < 1 || h > MODE_MAX)
		return false;
	*pw = (int)w;
	*ph = (int)h;
	*phz = hz;
	return true;
}

struct wlr_output_mode *outputrc_pick_mode(struct wl_list *modes, int w, int h, double hz)
{
	struct wlr_output_mode *best = NULL, *m;
	wl_list_for_each(m, modes, link) {
		if (m->width != w || m->height != h)
			continue;
		/* Within half a Hz: 59.94 is what most people mean by 60. */
		if (hz > 0 && abs(m->refresh - (int)(hz * 1000)) > 500)
			continue;
		if (!best || m->refresh > best->refresh)
			best = m;
	}
	return best;
}

bool outputrc_scale(const char *value, double *pscale)
{
	char *end;
	double scale = strtod(value, &end);
	/* Written so that NaN fails it too. Below this the layout overflows. */
	if (end == value || *end || !(scale >= 0.1 && scale <= 10))
		return false;
	*pscale = scale;
	return true;
}

bool outputrc_transform(const char *value, enum wl_output_transform *ptransform)
{
	static const struct {
		const char *name;
		enum wl_output_transform transform;
	} table[] = {
		{ "normal", WL_OUTPUT_TRANSFORM_NORMAL },
		{ "90", WL_OUTPUT_TRANSFORM_90 },
		{ "180", WL_OUTPUT_TRANSFORM_180 },
		{ "270", WL_OUTPUT_TRANSFORM_270 },
		{ "flipped", WL_OUTPUT_TRANSFORM_FLIPPED },
		{ "flipped-90", WL_OUTPUT_TRANSFORM_FLIPPED_90 },
		{ "flipped-180", WL_OUTPUT_TRANSFORM_FLIPPED_180 },
		{ "flipped-270", WL_OUTPUT_TRANSFORM_FLIPPED_270 },
	};
	for (size_t i = 0; i < sizeof(table) / sizeof(*table); i++) {
		if (!strcasecmp(table[i].name, value)) {
			*ptransform = table[i].transform;
			return true;
		}
	}
	return false;
}

/* strtol, not %d, for the same reason as above. */
bool outputrc_position(const char *value, int *px, int *py)
{
	char *end;
	long x = strtol(value, &end, 10), y;
	if (end == value || *end != ',')
		return false;
	const char *ys = end + 1;
	y = strtol(ys, &end, 10);
	if (end == ys || *end)
		return false;
	if (x < -POSITION_MAX || x > POSITION_MAX || y < -POSITION_MAX || y > POSITION_MAX)
		return false;
	*px = (int)x;
	*py = (int)y;
	return true;
}

/* rediwm's rule: any client may switch every monitor off, and then input is the only
 * way back that needs no client. With one on, the others stay as a client set them.
 * Not in the first second: the release of the key or click that switched them off,
 * a pointer that shakes after it, or a touchpad still scrolling would undo it. */
bool outputrc_wakes(struct wl_list *outputs, uint32_t now, uint32_t dark_since)
{
	bool dark = false;
	struct output *o;
	wl_list_for_each(o, outputs, link) {
		if (o->wlr_output->enabled)
			return false;
		dark = dark || o->dark;
	}
	return dark && now - dark_since >= OUTPUTRC_WAKE_AFTER_MS;
}
