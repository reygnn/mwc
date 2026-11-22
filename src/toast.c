/*
 * toast.c - short notices in the middle of the pointer's monitor, as screenShot shows
 * one. The desktop switched to, however it was switched (a pager such as deskControl,
 * xdg-activation): one in the middle, which the next switch replaces, its time anew. A
 * program mwc started that couldn't be: one each, with a time of its own, in the
 * bottom right corner clear of panels and the dock, the newest lowest; the same text
 * again only starts its time anew. Clicks go through them, and none shows over a
 * screen locker. draw.c draws them.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include "mwc.h"

#define TOAST_ERRORS 5 /* the oldest goes for a sixth */
#define TOAST_GAP 8     /* between errors */
#define TOAST_MARGIN 16 /* from the corner */

struct toast {
	struct wlr_scene_buffer *node;
	struct wl_event_source *timer;
	char text[64]; /* "" while hidden */
	struct wlr_box box;
};

static struct server *toast_server;
static struct toast desktop;
static struct toast *errors[TOAST_ERRORS];
static int n_errors;
static int shows;

/* MWC_TOAST_MS, 1 to 60000, instead of ms: for the tests, which need margins of whole
 * seconds. */
static int toast_ms(int ms)
{
	const char *s = getenv("MWC_TOAST_MS");
	if (!s || !*s)
		return ms;
	char *end;
	long env = strtol(s, &end, 10);
	return *end || env < 1 || env > 60000 ? ms : (int)env;
}

/* On the pointer's monitor: the desktop's in its middle, the errors stacked up from the
 * bottom right corner of what panels and the dock leave free, the newest lowest. */
static void arrange(void)
{
	struct server *server = toast_server;
	double lx = server->cursor->x, ly = server->cursor->y;
	if (desktop.text[0]) {
		struct wlr_box m = output_full_area(server, lx, ly);
		struct wlr_box *b = &desktop.box;
		b->x = m.x + (m.width - b->width) / 2;
		b->y = m.y + (m.height - b->height) / 2;
		wlr_scene_node_set_position(&desktop.node->node, b->x, b->y);
	}
	struct wlr_box u = output_usable_area(server, lx, ly);
	int bottom = u.y + u.height - TOAST_MARGIN;
	for (int i = n_errors - 1; i >= 0; i--) {
		struct wlr_box *b = &errors[i]->box;
		b->x = u.x + u.width - TOAST_MARGIN - b->width;
		b->y = bottom - b->height;
		bottom = b->y - TOAST_GAP;
		wlr_scene_node_set_position(&errors[i]->node->node, b->x, b->y);
	}
}

static bool toast_init(struct toast *t, wl_event_loop_timer_func_t expired)
{
	if (t->node && t->timer)
		return true;
	struct server *server = toast_server;
	t->node = wlr_scene_buffer_create(server->layer_feedback, NULL);
	t->timer = wl_event_loop_add_timer(server->event_loop, expired, t);
	if (t->node && t->timer) {
		t->node->point_accepts_input = scene_buffer_no_input;
		return true;
	}
	if (t->node)
		wlr_scene_node_destroy(&t->node->node);
	if (t->timer)
		wl_event_source_remove(t->timer);
	t->node = NULL;
	t->timer = NULL;
	return false;
}

/* Draws text into t and shows it for ms. */
static bool toast_put(struct toast *t, const char *text, int ms)
{
	struct cairo_buffer *buf = draw_toast(text);
	if (!buf)
		return false;
	t->box.width = buf->base.width;
	t->box.height = buf->base.height;
	wlr_scene_buffer_set_buffer(t->node, &buf->base);
	cairo_buffer_drop(buf);
	wlr_scene_node_raise_to_top(&t->node->node);
	wlr_scene_node_set_enabled(&t->node->node, true);
	snprintf(t->text, sizeof(t->text), "%s", text);
	wl_event_source_timer_update(t->timer, toast_ms(ms));
	shows++;
	return true;
}

static int desktop_expired(void *data)
{
	(void)data;
	wlr_scene_node_set_enabled(&desktop.node->node, false);
	desktop.text[0] = '\0';
	arrange();
	return 0;
}

static void error_free(int i)
{
	struct toast *t = errors[i];
	if (t->node)
		wlr_scene_node_destroy(&t->node->node);
	if (t->timer)
		wl_event_source_remove(t->timer);
	free(t);
	memmove(&errors[i], &errors[i + 1], (size_t)(n_errors - i - 1) * sizeof(*errors));
	n_errors--;
}

static int error_expired(void *data)
{
	for (int i = 0; i < n_errors; i++) {
		if (errors[i] == data) {
			error_free(i);
			break;
		}
	}
	arrange();
	return 0;
}

static bool toast_ready(struct server *server)
{
	toast_server = server;
	return !server->exclusive_layer;
}

void toast_show(struct server *server, const char *text, int ms)
{
	if (!toast_ready(server) || !toast_init(&desktop, desktop_expired))
		return;
	if (toast_put(&desktop, text, ms))
		arrange();
}

void toast_error(struct server *server, const char *text, int ms)
{
	if (!toast_ready(server) || !text || !*text)
		return;
	for (int i = 0; i < n_errors; i++) {
		if (!strcmp(errors[i]->text, text)) {
			wl_event_source_timer_update(errors[i]->timer, toast_ms(ms));
			arrange(); /* the pointer or a panel may have moved */
			return;
		}
	}
	if (n_errors == TOAST_ERRORS)
		error_free(0);
	struct toast *t = calloc(1, sizeof(*t));
	if (!t) {
		arrange();
		return;
	}
	errors[n_errors++] = t;
	if (!toast_init(t, error_expired) || !toast_put(t, text, ms))
		error_free(n_errors - 1);
	arrange();
}

void toast_hide(void)
{
	while (n_errors)
		error_free(n_errors - 1);
	if (desktop.node && desktop.text[0]) {
		wl_event_source_timer_update(desktop.timer, 0);
		desktop_expired(NULL);
	}
}

void toast_fini(void)
{
	while (n_errors)
		error_free(n_errors - 1);
	if (desktop.timer)
		wl_event_source_remove(desktop.timer);
	desktop.timer = NULL;
}

const char *toast_get(struct wlr_box *b, int *n)
{
	*b = desktop.box;
	*n = shows;
	return desktop.text;
}

const char *toast_error_get(int i, struct wlr_box *b)
{
	if (i < 0 || i >= n_errors)
		return NULL;
	*b = errors[i]->box;
	return errors[i]->text;
}
