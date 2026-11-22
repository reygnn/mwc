/*
 * toast.c - a short notice in the middle of the pointer's monitor, as screenShot shows
 * one: for now the desktop switched to, however it was switched (a pager such as
 * deskControl, xdg-activation). A new one replaces the one showing and starts its time
 * anew. Clicks go through it, and none shows over a screen locker. draw.c draws it.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include "mwc.h"

#define TOAST_MS 1000

static struct wlr_scene_buffer *node;
static struct wl_event_source *timer;
static char shown[64];
static struct wlr_box box;
static int shows;

static int toast_expired(void *data)
{
	(void)data;
	wlr_scene_node_set_enabled(&node->node, false);
	shown[0] = '\0';
	return 0;
}

void toast_hide(void)
{
	if (!node || !shown[0])
		return;
	wl_event_source_timer_update(timer, 0);
	toast_expired(NULL);
}

/* MWC_TOAST_MS: how long, 1 to 60000, for the tests, which need margins of whole
 * seconds. */
static int toast_ms(void)
{
	const char *s = getenv("MWC_TOAST_MS");
	if (!s || !*s)
		return TOAST_MS;
	char *end;
	long ms = strtol(s, &end, 10);
	return *end || ms < 1 || ms > 60000 ? TOAST_MS : (int)ms;
}

static bool toast_init(struct server *server)
{
	if (node && timer)
		return true;
	node = wlr_scene_buffer_create(server->layer_feedback, NULL);
	timer = wl_event_loop_add_timer(server->event_loop, toast_expired, NULL);
	if (node && timer) {
		node->point_accepts_input = scene_buffer_no_input;
		return true;
	}
	if (node)
		wlr_scene_node_destroy(&node->node);
	if (timer)
		wl_event_source_remove(timer);
	node = NULL;
	timer = NULL;
	return false;
}

void toast_show(struct server *server, const char *text)
{
	if (server->exclusive_layer || !toast_init(server))
		return;
	struct cairo_buffer *buf = draw_toast(text);
	if (!buf)
		return;
	int w = buf->base.width, h = buf->base.height;
	wlr_scene_buffer_set_buffer(node, &buf->base);
	cairo_buffer_drop(buf);
	struct wlr_box m = output_full_area(server, server->cursor->x, server->cursor->y);
	box = (struct wlr_box){ m.x + (m.width - w) / 2, m.y + (m.height - h) / 2, w, h };
	wlr_scene_node_set_position(&node->node, box.x, box.y);
	wlr_scene_node_raise_to_top(&node->node);
	wlr_scene_node_set_enabled(&node->node, true);
	snprintf(shown, sizeof(shown), "%s", text);
	wl_event_source_timer_update(timer, toast_ms());
	shows++;
}

void toast_fini(void)
{
	if (timer)
		wl_event_source_remove(timer);
	timer = NULL;
}

const char *toast_get(struct wlr_box *b, int *n)
{
	*b = box;
	*n = shows;
	return shown;
}
