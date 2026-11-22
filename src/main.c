/*
 * mwc - a Motif-style Wayland compositor
 *
 * Server setup and output handling. Run it from a TTY for a real session,
 * or inside an existing Wayland or X11 session where wlroots opens a window.
 */
#define _POSIX_C_SOURCE 200809L
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>
#include "mwc.h"
#include "version.h"

/* ---- outputs ------------------------------------------------------------ */

static void output_frame(struct wl_listener *listener, void *data)
{
	struct output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *so = wlr_scene_get_scene_output(
		output->server->scene, output->wlr_output);
	wlr_scene_output_commit(so, NULL);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(so, &now);
}

static void output_request_state(struct wl_listener *listener, void *data)
{
	/* Lets the nested backends resize their window. */
	struct output *output = wl_container_of(listener, output,
		request_state);
	const struct wlr_output_event_request_state *event = data;
	struct wlr_output *o = output->wlr_output;
	int w = o->width, h = o->height;
	/* Whoever resizes the window decides now, not what a client set before. The
	 * request always carries a mode, also when the window was only moved or
	 * (de)activated: only a new size counts. */
	if (wlr_output_commit_state(o, event->state) && (o->width != w || o->height != h)) {
		free(output->managed_rc);
		output->managed_rc = NULL;
	}
}

static bool output_commit_config(struct wlr_output *o);

/* What mwmrc says about this output, to see whether that changed. NULL without memory. */
static char *output_rc_text(struct wlr_output *o)
{
	static const char *keys[] = {
		"outputMode", "outputScale", "outputTransform", "outputPosition",
	};
	char *text = NULL;
	size_t size;
	FILE *f = open_memstream(&text, &size);
	if (!f)
		return NULL;
	for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
		const char *value = config_get_for(o->name, keys[i]);
		/* Unset and empty differ: the newline after each value keeps them apart. */
		fprintf(f, "%c%s\n", value ? '=' : '-', value ? value : "");
	}
	if (fclose(f) != 0) {
		free(text);
		return NULL;
	}
	return text;
}

static void output_forget_managed(struct output *output)
{
	free(output->managed_rc);
	output->managed_rc = NULL;
}

/* Whether what a client set still holds: mwmrc says what it said then about the output. */
static bool output_keeps_managed(struct output *output)
{
	if (!output->managed_rc)
		return false;
	char *now = output_rc_text(output->wlr_output);
	bool same = now && !strcmp(now, output->managed_rc);
	free(now);
	if (!same) {
		wlr_log(WLR_INFO, "mwmrc changed for %s: it replaces what a client set",
			output->wlr_output->name);
		output_forget_managed(output);
	}
	return same;
}

/* Switches the output on with what a client set. */
static bool output_commit_managed(struct output *output)
{
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_head_v1_state_apply(&output->managed, &state);
	wlr_output_state_set_enabled(&state, true);
	bool ok = wlr_output_commit_state(output->wlr_output, &state);
	wlr_output_state_finish(&state);
	return ok;
}

/* An output that was off is on again. */
static void output_back_on(struct server *server, struct wlr_output *o)
{
	wlr_output_schedule_frame(o);
	/* The output lost its cursor. Setting the same image again would do nothing. */
	if (server->cursor_name) {
		wlr_cursor_unset_image(server->cursor);
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
			server->cursor_name);
	}
	input_pointer_changed_soon(server);
}

/* Tells wlr-output-management clients how the outputs are now. Once the event loop
 * is idle: changes come in groups, and a dying output must be gone from the list. */
static void output_manager_send(void *data)
{
	struct server *server = data;
	server->output_manager_idle = NULL;
	struct wlr_output_configuration_v1 *config = wlr_output_configuration_v1_create();
	if (!config)
		return;
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output_configuration_head_v1 *head =
			wlr_output_configuration_head_v1_create(config, output->wlr_output);
		if (!head)
			continue;
		/* Off only for the time being: on for clients, as what it comes back with.
		 * Else wlroots reads it from the output as disabled, and a client that
		 * matches profiles against that may switch it back on. */
		if (output->dark && output_keeps_managed(output)) {
			head->state = output->managed;
			head->state.output = output->wlr_output;
		}
		if (output->dark)
			head->state.enabled = true;
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
		if (output->client_off)
			box = output->off_box;
		head->state.x = box.x;
		head->state.y = box.y;
	}
	wlr_output_manager_v1_set_configuration(server->output_manager, config);
}

/* Right away, not once idle: a configuration a client makes from the serial it
 * had must be cancelled, not applied over what changed. */
static void output_manager_send_now(struct server *server)
{
	if (!server->output_manager)
		return;
	if (server->output_manager_idle)
		wl_event_source_remove(server->output_manager_idle);
	output_manager_send(server);
}

static void output_manager_update(struct server *server)
{
	if (!server->output_manager || server->output_manager_idle)
		return;
	server->output_manager_idle = wl_event_loop_add_idle(server->event_loop,
		output_manager_send, server);
}

static void output_commit(struct wl_listener *listener, void *data)
{
	struct output *output = wl_container_of(listener, output, commit);
	const struct wlr_output_event_commit *event = data;
	if (event->state->committed & (WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE |
			WLR_OUTPUT_STATE_SCALE | WLR_OUTPUT_STATE_TRANSFORM |
			WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED))
		output_manager_update(output->server);
}

/* Switches o off or on through wlr-output-power, for a client or for input. */
static bool output_power(struct server *server, struct wlr_output *o, bool on)
{
	struct output *output = o->data;
	bool ok = false;
	if (on) {
		if (output && output_keeps_managed(output)) {
			ok = output_commit_managed(output);
			if (!ok)
				output_forget_managed(output);
		}
		if (!ok)
			ok = output_commit_config(o);
	} else {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, false);
		ok = wlr_output_commit_state(o, &state);
		wlr_output_state_finish(&state);
	}
	if (!ok) {
		wlr_log(WLR_ERROR, "can't switch output %s %s", o->name, on ? "on" : "off");
		return false;
	}
	if (output)
		output->dark = !on;
	if (on) {
		output_back_on(server, o);
	} else {
		server->dark_since = now_msec();
		server->wake_failed = false;
	}
	/* The commit tells clients too, but a dark output isn't what it looks like. */
	output_manager_update(server);
	return true;
}

/* A client asks to switch a monitor off or on. The output stays in the layout, so
 * windows keep their places, and any client can ask: that is what the protocol is.
 * Coming back it takes what mwmrc says now: a reload leaves a monitor that is off alone.
 * What a client set through wlr-output-management comes back with it, unless mwmrc
 * changed for it in the meantime. */
static void output_power_set_mode(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		output_power_set_mode);
	struct wlr_output_power_v1_set_mode_event *event = data;
	bool on = event->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON;
	if (event->output->enabled == on)
		return;
	struct output *output = event->output->data;
	/* A wlr-output-management client switched it off: wlopm --on '*' after an
	 * idle spell must not bring back a monitor someone doesn't want. */
	if (on && output && output->client_off) {
		wlr_log(WLR_INFO, "%s stays off: a client switched it off",
			event->output->name);
		return;
	}
	output_power(server, event->output, on);
}

bool outputs_wake(struct server *server)
{
	if (server->wake_failed ||
			!outputrc_wakes(&server->outputs, now_msec(), server->dark_since))
		return false;
	wlr_log(WLR_INFO, "input wakes the monitors: all were off");
	bool woke = false;
	struct output *output;
	/* A client_off output is never dark: output management clears it. */
	wl_list_for_each(output, &server->outputs, link)
		if (output->dark)
			woke = output_power(server, output->wlr_output, true) || woke;
	server->wake_failed = !woke;
	return woke;
}

/* One per inhibitor. While one exists whose surface is mapped, nobody counts as idle;
 * minimized or on another desktop it still counts, see README. One whose surface
 * never appeared doesn't, or a client could keep the screen on unseen. */
struct idle_inhibitor {
	struct wl_listener destroy;
	struct wl_listener map, unmap;
	struct server *server;
	struct wlr_idle_inhibitor_v1 *wlr;
};

static void idle_recount(struct server *server, struct wlr_idle_inhibitor_v1 *going)
{
	bool any = false;
	struct wlr_idle_inhibitor_v1 *i;
	wl_list_for_each(i, &server->idle_inhibit->inhibitors, link)
		any |= i != going && i->surface->mapped;
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, any);
}

static void idle_inhibitor_mapped(struct wl_listener *listener, void *data)
{
	struct idle_inhibitor *inhibitor = wl_container_of(listener, inhibitor, map);
	idle_recount(inhibitor->server, NULL);
}

static void idle_inhibitor_unmapped(struct wl_listener *listener, void *data)
{
	struct idle_inhibitor *inhibitor = wl_container_of(listener, inhibitor, unmap);
	idle_recount(inhibitor->server, NULL);
}

static void idle_inhibitor_destroy(struct wl_listener *listener, void *data)
{
	struct idle_inhibitor *inhibitor = wl_container_of(listener, inhibitor, destroy);
	/* It is still in the list while it says that it goes. */
	idle_recount(inhibitor->server, inhibitor->wlr);
	wl_list_remove(&inhibitor->destroy.link);
	wl_list_remove(&inhibitor->map.link);
	wl_list_remove(&inhibitor->unmap.link);
	free(inhibitor);
}

static void new_idle_inhibitor(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, new_idle_inhibitor);
	struct wlr_idle_inhibitor_v1 *wlr_inhibitor = data;
	struct idle_inhibitor *inhibitor = calloc(1, sizeof(*inhibitor));
	if (!inhibitor) {
		wl_resource_post_no_memory(wlr_inhibitor->resource);
		return;
	}
	inhibitor->server = server;
	inhibitor->wlr = wlr_inhibitor;
	inhibitor->destroy.notify = idle_inhibitor_destroy;
	wl_signal_add(&wlr_inhibitor->events.destroy, &inhibitor->destroy);
	inhibitor->map.notify = idle_inhibitor_mapped;
	wl_signal_add(&wlr_inhibitor->surface->events.map, &inhibitor->map);
	inhibitor->unmap.notify = idle_inhibitor_unmapped;
	wl_signal_add(&wlr_inhibitor->surface->events.unmap, &inhibitor->unmap);
	idle_recount(server, NULL);
}

static int throttle_rested(void *data)
{
	struct throttle *t = data;
	t->resting = false;
	if (!t->due)
		return 0;
	t->due = false;
	t->resting = true;
	wl_event_source_timer_update(t->timer, t->ms);
	t->run(t->data);
	return 0;
}

void throttle_call(struct throttle *t, struct wl_event_loop *loop)
{
	if (!t->timer)
		t->timer = wl_event_loop_add_timer(loop, throttle_rested, t);
	if (t->resting) {
		t->due = true;
		return;
	}
	/* Without a timer each call runs: slower, but nothing is lost. */
	if (t->timer) {
		t->resting = true;
		wl_event_source_timer_update(t->timer, t->ms);
	}
	t->run(t->data);
}

void throttle_cancel(struct throttle *t)
{
	if (t->timer)
		wl_event_source_remove(t->timer);
	t->timer = NULL;
	t->resting = t->due = false;
}

uint32_t now_msec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void output_destroy(struct wl_listener *listener, void *data)
{
	struct output *output = wl_container_of(listener, output, destroy);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->commit.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	output_forget_managed(output);
	output_manager_update(output->server);
	layer_output_gone(output);
	views_output_gone(output);
	wlr_scene_node_destroy(&output->background->node);
	if (output->wallpaper)
		wlr_scene_node_destroy(&output->wallpaper->node);
	/* wlroots leaves it in the layout until after this signal, and listeners after
	 * this one (fifo's) may commit surfaces, which ask the layout where outputs are. */
	output->wlr_output->data = NULL;
	wlr_output_layout_remove(output->server->output_layout, output->wlr_output);
	free(output);
}

/* Mwm*NAME*outputMode: WIDTHxHEIGHT or WIDTHxHEIGHT@HZ. The highest refresh rate if
 * none is given. A backend without a mode list, nested or headless, takes any size. */
static bool configure_mode(struct wlr_output *o, struct wlr_output_state *state)
{
	const char *value = config_get_for(o->name, "outputMode");
	if (!value)
		return false;
	int w, h;
	double hz;
	if (!outputrc_mode(value, &w, &h, &hz)) {
		wlr_log(WLR_ERROR, "ignoring outputMode \"%s\" for %s: expected "
			"WIDTHxHEIGHT or WIDTHxHEIGHT@HZ, up to 32767x32767@1000", value,
			o->name);
		return false;
	}
	if (wl_list_empty(&o->modes)) {
		wlr_output_state_set_custom_mode(state, w, h, (int)(hz * 1000));
		return true;
	}
	struct wlr_output_mode *best = outputrc_pick_mode(&o->modes, w, h, hz);
	if (!best) {
		wlr_log(WLR_ERROR, "ignoring outputMode \"%s\": %s has no such mode",
			value, o->name);
		return false;
	}
	wlr_output_state_set_mode(state, best);
	return true;
}

static void configure_scale(struct wlr_output *o, struct wlr_output_state *state)
{
	const char *value = config_get_for(o->name, "outputScale");
	double scale = 1;
	if (value && !outputrc_scale(value, &scale))
		wlr_log(WLR_ERROR, "ignoring outputScale \"%s\" for %s", value, o->name);
	wlr_output_state_set_scale(state, scale);
}

static void configure_transform(struct wlr_output *o, struct wlr_output_state *state)
{
	const char *value = config_get_for(o->name, "outputTransform");
	enum wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
	if (value && !outputrc_transform(value, &transform))
		wlr_log(WLR_ERROR, "ignoring outputTransform \"%s\" for %s", value, o->name);
	wlr_output_state_set_transform(state, transform);
}

/* Commits what mwmrc says about this output. Its mode may not be possible, nor the
 * preferred one, two 4K monitors on one link say, so the others get their turn. */
static bool output_commit_config(struct wlr_output *o)
{
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	configure_scale(o, &state);
	configure_transform(o, &state);
	/* mwmrc has no say about it: what a client turned on goes with the client. */
	if (o->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED)
		wlr_output_state_set_adaptive_sync_enabled(&state, false);
	bool configured = configure_mode(o, &state);
	struct wlr_output_mode *preferred = wlr_output_preferred_mode(o);
	if (!configured && preferred)
		wlr_output_state_set_mode(&state, preferred);
	if (!wlr_output_test_state(o, &state)) {
		if (configured)
			wlr_log(WLR_ERROR, "output %s can't do what mwmrc asks", o->name);
		if (preferred)
			wlr_output_state_set_mode(&state, preferred);
		if (!wlr_output_test_state(o, &state)) {
			struct wlr_output_mode *m;
			wl_list_for_each(m, &o->modes, link) {
				wlr_output_state_set_mode(&state, m);
				if (wlr_output_test_state(o, &state))
					break;
			}
		}
	}
	bool ok = wlr_output_commit_state(o, &state);
	wlr_output_state_finish(&state);
	return ok;
}

/* Mwm*NAME*outputPosition: X,Y. Otherwise the next free place in the row. */
static struct wlr_output_layout_output *output_place(struct server *server,
	struct wlr_output *o)
{
	const char *value = config_get_for(o->name, "outputPosition");
	int x, y;
	if (value && outputrc_position(value, &x, &y))
		return wlr_output_layout_add(server->output_layout, o, x, y);
	if (value)
		wlr_log(WLR_ERROR, "ignoring outputPosition \"%s\" for %s", value, o->name);
	return wlr_output_layout_add_auto(server->output_layout, o);
}

void outputs_config_changed(struct server *server)
{
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		/* What a client set stays, until mwmrc says something new about the output.
		 * One a client switched off stays out of the layout. */
		if (output->client_off || output_keeps_managed(output))
			continue;
		/* Whoever switched it off wants it off, see output_power_set_mode(). */
		if (output->wlr_output->enabled)
			output_commit_config(output->wlr_output);
		output_place(server, output->wlr_output);
	}
}

static void new_output(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);
	workspace_output_new(wlr_output);

	/* Its name and modes are what mwmrc needs, and nowhere else to be seen. */
	wlr_log(WLR_INFO, "new output %s", wlr_output->name);
	struct wlr_output_mode *known;
	wl_list_for_each(known, &wlr_output->modes, link)
		wlr_log(WLR_INFO, "  mode %dx%d@%.3f", known->width, known->height,
			known->refresh / 1000.0);

	if (!output_commit_config(wlr_output)) {
		wlr_log(WLR_ERROR, "no usable mode for output %s", wlr_output->name);
		return;
	}

	struct output *output = calloc(1, sizeof(*output));
	if (!output) {
		wlr_log(WLR_ERROR, "out of memory: output %s not used", wlr_output->name);
		return;
	}
	output->server = server;
	output->wlr_output = wlr_output;
	wlr_output->data = output;
	for (int i = 0; i < MWC_SHELL_LAYERS; i++)
		wl_list_init(&output->layers[i]);
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->commit.notify = output_commit;
	wl_signal_add(&wlr_output->events.commit, &output->commit);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	float c[4] = { theme.root.r, theme.root.g, theme.root.b, 1 };
	output->background = wlr_scene_rect_create(server->layer_bg, 0, 0, c);
	wlr_scene_node_set_enabled(&output->background->node, !theme.root_none);
	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *lo = output_place(server, wlr_output);
	struct wlr_scene_output *so = wlr_scene_output_create(server->scene,
		wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, lo, so);
	/* There is no cursor image before the first motion otherwise. */
	if (!server->cursor_name)
		cursor_set_name(server, "default");
}

static void layout_change(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		layout_change);
	if (server->layout_batch) {
		server->layout_dirty = true;
		return;
	}
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout,
			output->wlr_output, &box);
		wlr_scene_node_set_position(&output->background->node,
			box.x, box.y);
		wlr_scene_rect_set_size(output->background, box.width,
			box.height);
		wallpaper_place(output, false);
		layer_arrange(output);
	}
	icons_arrange(server);
	views_layout_changed(server);
	output_manager_update(server);
}

/* What the protocol lets through but mwc doesn't take: sizes a software renderer can't
 * allocate, and positions and scales the layout overflows with, as for mwmrc. Any
 * client may ask, so more is refused than mwmrc refuses:
 * - more pixels than 8K, 135 MiB a buffer. Each output holds a few such buffers, and
 *   they are memory like any other: past what mwc may use, the OOM killer ends it.
 *   The cap bounds what one request costs, not what all outputs together do.
 * - a refresh below 1 Hz (0 means the backend's default): a headless output would draw
 *   a frame every quarter hour, and whoever watches it over VNC or RDP sees nothing.
 * - a scale that leaves less than one logical pixel. */
static bool head_state_usable(const struct wlr_output_head_v1_state *s)
{
	if (!s->enabled)
		return true;
	if (!s->mode && (s->custom_mode.width > 16384 || s->custom_mode.height > 16384 ||
			(int64_t)s->custom_mode.width * s->custom_mode.height > 8192 * 4320 ||
			s->custom_mode.refresh > 1000000 ||
			(s->custom_mode.refresh > 0 && s->custom_mode.refresh < 1000)))
		return false;
	if (!(s->scale >= 0.1 && s->scale <= 10 && s->x >= -1000000 &&
			s->x <= 1000000 && s->y >= -1000000 && s->y <= 1000000))
		return false;
	int w = s->mode ? s->mode->width : s->custom_mode.width;
	int h = s->mode ? s->mode->height : s->custom_mode.height;
	return (w <= 0 || w / s->scale >= 1) && (h <= 0 || h / s->scale >= 1);
}

/* Tests or applies what a client asks for, all outputs at once. An output it switches
 * off leaves the layout and comes back where it was; one that is dark (output-power)
 * stays in it and stays dark: what the client set for it is kept, and comes with the
 * power. */
static bool output_manager_configure(struct server *server,
	struct wlr_output_configuration_v1 *config, bool apply)
{
	struct wlr_output_configuration_head_v1 *head;
	size_t n_heads = 0;
	wl_list_for_each(head, &config->heads, link) {
		if (!head->state.output->data || !head_state_usable(&head->state)) {
			wlr_log(WLR_ERROR, "refusing an output configuration for %s",
				head->state.output->name);
			return false;
		}
		n_heads++;
	}
	size_t n;
	struct wlr_backend_output_state *states =
		wlr_output_configuration_v1_build_state(config, &n);
	if (!states)
		return false;
	/* Dark outputs don't go to the backend: they aren't to light up. */
	size_t kept = 0;
	for (size_t i = 0; i < n; i++) {
		struct output *output = states[i].output->data;
		if (output && output->dark)
			wlr_output_state_finish(&states[i].base);
		else
			states[kept++] = states[i];
	}
	/* In the order of config->heads, as walked below. */
	bool *was_on = calloc(n_heads ? n_heads : 1, sizeof(*was_on));
	size_t i = 0;
	wl_list_for_each(head, &config->heads, link) {
		if (was_on)
			was_on[i] = head->state.output->enabled;
		i++;
	}
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		wlr_output_layout_get_box(server->output_layout, output->wlr_output,
			&output->box_before);
		output->named = false;
	}
	bool ok = was_on && wlr_backend_test(server->backend, states, kept);
	/* A commit that changes a mode moves the outputs wlroots placed itself:
	 * the layout's change waits until everything is where it is going. */
	server->layout_batch = apply;
	server->layout_dirty = false;
	if (ok && apply)
		ok = wlr_backend_commit(server->backend, states, kept);
	for (size_t k = 0; k < kept; k++)
		wlr_output_state_finish(&states[k].base);
	free(states);
	if (!ok || !apply) {
		free(was_on);
		goto done;
	}
	i = 0;
	wl_list_for_each(head, &config->heads, link) {
		output = head->state.output->data;
		output->named = true;
		/* wlroots sends a head switched off with everything else zeroed: it keeps
		 * its place, and what a client set before comes back with it. */
		if (!head->state.enabled) {
			/* Out of the layout, unlike a dark one: nobody is to work there. */
			if (!output->client_off)
				output->off_box = output->box_before;
			output->client_off = true;
			output->dark = false;
			workspace_output_leaving(head->state.output);
			wlr_output_layout_remove(server->output_layout, head->state.output);
			/* Its panels have nowhere to be: closed, as when it is unplugged. */
			layer_output_gone(output);
			i++;
			continue;
		}
		bool was_out = !output_in_layout(output);
		output->client_off = false;
		/* Where the client says, also where it already is: placed, it stays
		 * there when other outputs change. */
		struct wlr_output_layout_output *lo = wlr_output_layout_add(
			server->output_layout, head->state.output, head->state.x,
			head->state.y);
		/* Leaving the layout cut the scene's tie to it: without it the picture
		 * would stay where it was and not follow the layout any more. */
		struct wlr_scene_output *so = wlr_scene_get_scene_output(server->scene,
			head->state.output);
		if (was_out && lo && so)
			wlr_scene_output_layout_add_output(server->scene_layout, lo, so);
		output_forget_managed(output);
		output->managed_rc = output_rc_text(head->state.output);
		output->managed = head->state;
		if (!was_on[i] && !output->dark)
			output_back_on(server, head->state.output);
		i++;
	}
	/* wlroots lets through configurations that leave outputs out, which the
	 * protocol forbids. Those it lined up anew stay where they were. */
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
		if (!output->named && !wlr_box_empty(&output->box_before) &&
				(box.x != output->box_before.x || box.y != output->box_before.y))
			wlr_output_layout_add(server->output_layout, output->wlr_output,
				output->box_before.x, output->box_before.y);
	}
	free(was_on);
done:
	server->layout_batch = false;
	if (server->layout_dirty) {
		server->layout_dirty = false;
		layout_change(&server->layout_change, NULL);
	}
	return ok;
}

static void output_manager_apply(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, output_manager_apply);
	struct wlr_output_configuration_v1 *config = data;
	if (output_manager_configure(server, config, true))
		wlr_output_configuration_v1_send_succeeded(config);
	else
		wlr_output_configuration_v1_send_failed(config);
	wlr_output_configuration_v1_destroy(config);
	/* Also after a failure: the client goes on from what it was told last. */
	output_manager_send_now(server);
}

static void output_manager_test(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, output_manager_test);
	struct wlr_output_configuration_v1 *config = data;
	if (output_manager_configure(server, config, false))
		wlr_output_configuration_v1_send_succeeded(config);
	else
		wlr_output_configuration_v1_send_failed(config);
	wlr_output_configuration_v1_destroy(config);
}

/* ---- startup ------------------------------------------------------------ */

static bool in_path(const char *prog)
{
	const char *path = getenv("PATH");
	if (!path)
		return false;
	char *dirs = strdup(path), *save = NULL;
	if (!dirs)
		return false;
	bool found = false;
	for (char *d = strtok_r(dirs, ":", &save); d && !found;
			d = strtok_r(NULL, ":", &save)) {
		char buf[4096];
		snprintf(buf, sizeof(buf), "%s/%s", d, prog);
		found = access(buf, X_OK) == 0;
	}
	free(dirs);
	return found;
}

/* The first one installed, if nobody says which. Terminals without client-side
 * decorations first, so they get a frame. */
static const char *pick_terminal(void)
{
	static const char *candidates[] = {
		"foot", "alacritty", "konsole", "kitty", "ptyxis", "gnome-terminal",
	};
	for (size_t i = 0; i < sizeof(candidates) / sizeof(*candidates); i++) {
		if (in_path(candidates[i]))
			return candidates[i];
	}
	return "foot";
}

/* Asked each time: a reload may have brought another one. */
const char *terminal_command(struct server *server)
{
	const char *value = getenv("MWC_TERMINAL");
	if (!value || !*value)
		value = config_get("terminal");
	return value && *value ? value : server->terminal;
}

/* D-Bus-activated services, such as gnome-terminal's server and the desktop portals,
 * don't inherit from mwc. In a real session only: nested, this would point the session
 * around it at mwc. */
static void export_activation_environment(struct server *server)
{
	if (!server->session)
		return;
	setenv("XDG_SESSION_TYPE", "wayland", true);
	if (in_path("dbus-update-activation-environment"))
		spawn("dbus-update-activation-environment --systemd WAYLAND_DISPLAY "
			"XDG_CURRENT_DESKTOP XDG_SESSION_TYPE");
	/* Left there by an X session before: services would open on a display that
	 * is gone. */
	if (in_path("systemctl"))
		spawn("systemctl --user unset-environment DISPLAY");
}

static void run_autostart(const char *startup_cmd)
{
	if (startup_cmd) {
		spawn(startup_cmd);
		return;
	}
	const char *cfg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char path[4096];
	if (cfg && *cfg)
		snprintf(path, sizeof(path), "%s/mwc/autostart", cfg);
	else if (home)
		snprintf(path, sizeof(path), "%s/.config/mwc/autostart", home);
	else
		return;
	if (access(path, X_OK) == 0)
		spawn_file(path);
}

static int handle_sighup(int signal, void *data)
{
	config_reload(data);
	return 0;
}

static int handle_terminate(int signal, void *data)
{
	struct server *server = data;
	wl_display_terminate(server->display);
	return 0;
}

static void usage(const char *argv0)
{
	printf("Usage: %s [-s startup-command] [-d] [-v]\n"
		"\n"
		"  -s CMD   run CMD once the compositor is up\n"
		"  -d       debug logging\n"
		"  -v       print the version\n"
		"\n"
		"Environment: MWC_TERMINAL, MWC_ACTIVE, MWC_INACTIVE, MWC_ROOT,\n"
		"MWC_WALLPAPER, MWC_FONT, MWC_MOVE_OPAQUE, MWC_TILING,\n"
		"MWC_TILE_MAIN_WIDTH, MWC_VIRTUAL_INPUT, XCURSOR_THEME, XCURSOR_SIZE,\n"
		"XKB_DEFAULT_*.\n"
		"Without -s, ~/.config/mwc/autostart is run if executable.\n"
		"~/.config/mwc/mwmrc is read at startup and on SIGHUP.\n",
		argv0);
}

int main(int argc, char *argv[])
{
	const char *startup_cmd = NULL;
	enum wlr_log_importance log_level = WLR_INFO;
	int c;
	while ((c = getopt(argc, argv, "s:dvh")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		case 'd':
			log_level = WLR_DEBUG;
			break;
		case 'v':
			puts("mwc " MWC_VERSION);
			return 0;
		default:
			usage(argv[0]);
			return c == 'h' ? 0 : 1;
		}
	}
	wlr_log_init(log_level, NULL);
	/* wlroots writes to pipes that clients hand it, for instance when pasting, and a
	 * client may close its end at any time. */
	signal(SIGPIPE, SIG_IGN);

	struct server server = { 0 };
	server.display = wl_display_create();
	server.event_loop = wl_display_get_event_loop(server.display);
	/* Before anything that starts threads, Pango and Mesa do: these calls
	 * block the signals, and a thread that doesn't would catch them and
	 * die with its default action. */
	wl_event_loop_add_signal(server.event_loop, SIGHUP, handle_sighup, &server);
	wl_event_loop_add_signal(server.event_loop, SIGTERM, handle_terminate,
		&server);
	wl_event_loop_add_signal(server.event_loop, SIGINT, handle_terminate,
		&server);
	testhook_init(&server);
	/* For a VNC server such as wayvnc. Only for this process: a nested mwc started
	 * from here asks for itself. */
	if (config_bool("MWC_VIRTUAL_INPUT", getenv("MWC_VIRTUAL_INPUT")) == 1)
		input_virtual_init(&server);
	unsetenv("MWC_VIRTUAL_INPUT");

	config_load();
	theme_init();
	server.terminal = pick_terminal();
	wallpaper_init(&server);
	tile_init(&server);
	server.backend = wlr_backend_autocreate(server.event_loop,
		&server.session);
	if (!server.backend) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (!server.renderer) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.display);
	server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);
	if (!server.allocator) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}
	/* Where wlroots took its shm allocator, which puts buffers in /dev/shm: see
	 * shmalloc.c. */
	if (server.allocator->buffer_caps == (WLR_BUFFER_CAP_SHM | WLR_BUFFER_CAP_DATA_PTR)) {
		struct wlr_allocator *shm = shm_allocator_create();
		if (shm) {
			wlr_allocator_destroy(server.allocator);
			server.allocator = shm;
		}
	}

	server.compositor = wlr_compositor_create(server.display, 6,
		server.renderer);
	wlr_subcompositor_create(server.display);
	wlr_data_device_manager_create(server.display);
	wlr_primary_selection_v1_device_manager_create(server.display);
	/* The clipboard for programs without a window, such as screenShot -c or wl-copy:
	 * they cannot get the keyboard focus that wl_data_device asks for. Both versions,
	 * the older one is what most clipboard tools still speak. */
	wlr_ext_data_control_manager_v1_create(server.display, 1);
	wlr_data_control_manager_v1_create(server.display);
	wlr_viewporter_create(server.display);
	wlr_single_pixel_buffer_manager_v1_create(server.display);
	wlr_presentation_create(server.display, server.backend, 2);
	fifo_init(&server);
	workspace_init(&server);
	wlr_screencopy_manager_v1_create(server.display);
	/* screencopy's successor, with damage and the pointer as an image of its own: a
	 * VNC or RDP server can send it apart, and the viewer draws it without delay. */
	wlr_ext_image_copy_capture_manager_v1_create(server.display, 1);
	wlr_ext_output_image_capture_source_manager_v1_create(server.display, 1);
	server.idle_notifier = wlr_idle_notifier_v1_create(server.display);
	server.idle_inhibit = wlr_idle_inhibit_v1_create(server.display);
	server.new_idle_inhibitor.notify = new_idle_inhibitor;
	wl_signal_add(&server.idle_inhibit->events.new_inhibitor,
		&server.new_idle_inhibitor);
	struct wlr_output_power_manager_v1 *output_power =
		wlr_output_power_manager_v1_create(server.display);
	server.output_power_set_mode.notify = output_power_set_mode;
	wl_signal_add(&output_power->events.set_mode, &server.output_power_set_mode);

	/* Only when asked for, read once at startup. wlroots up to 0.20 aborts in its
	 * own code when one client holds a custom mode another doesn't (fixed on its
	 * master in 40640950): with headless or nested outputs, which only have custom
	 * modes, any client can bring mwc down. */
	if (config_bool("outputManagement", config_get("outputManagement")) == 1) {
		server.output_manager = wlr_output_manager_v1_create(server.display);
		server.output_manager_apply.notify = output_manager_apply;
		wl_signal_add(&server.output_manager->events.apply,
			&server.output_manager_apply);
		server.output_manager_test.notify = output_manager_test;
		wl_signal_add(&server.output_manager->events.test,
			&server.output_manager_test);
	}

	server.output_layout = wlr_output_layout_create(server.display);
	wlr_xdg_output_manager_v1_create(server.display, server.output_layout);
	wl_list_init(&server.outputs);
	server.new_output.notify = new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);
	server.layout_change.notify = layout_change;
	wl_signal_add(&server.output_layout->events.change,
		&server.layout_change);

	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene,
		server.output_layout);
	/* Bottom to top. Panels in the top layer stay above windows, and only
	 * the overlay layer goes above a fullscreen window. */
	struct wlr_scene_tree *root = &server.scene->tree;
	server.layer_bg = wlr_scene_tree_create(root);
	server.layer_shell[0] = wlr_scene_tree_create(root);
	server.layer_shell[1] = wlr_scene_tree_create(root);
	server.layer_icons = wlr_scene_tree_create(root);
	iconbox_init(&server);
	server.layer_views = wlr_scene_tree_create(root);
	dock_init(&server);
	server.layer_shell[2] = wlr_scene_tree_create(root);
	server.layer_fullscreen = wlr_scene_tree_create(root);
	server.layer_shell[3] = wlr_scene_tree_create(root);
	server.layer_menu = wlr_scene_tree_create(root);
	server.layer_feedback = wlr_scene_tree_create(root);
	server.layer_drag = wlr_scene_tree_create(root);

	wl_list_init(&server.views);
	xdg_init(&server);
	input_init(&server);
	layer_init(&server);

	const char *socket = wl_display_add_socket_auto(server.display);
	if (!socket) {
		wlr_log(WLR_ERROR, "failed to create a Wayland socket");
		return 1;
	}
	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "failed to start the backend");
		return 1;
	}
	setenv("WAYLAND_DISPLAY", socket, true);
	setenv("XDG_CURRENT_DESKTOP", "mwc", true);
	/* Nested in an X session, its programs would open there instead. */
	unsetenv("DISPLAY");
	export_activation_environment(&server);
	wlr_log(WLR_INFO, "mwc " MWC_VERSION " running on WAYLAND_DISPLAY=%s, terminal: %s",
		socket, terminal_command(&server));

	run_autostart(startup_cmd);
	wl_display_run(server.display);

	/* wlroots asserts that nobody listens to an object it destroys. */
	wl_display_destroy_clients(server.display);
	/* Their popups may have asked for this, and nothing will run it any more. */
	if (server.pointer_refresh)
		wl_event_source_remove(server.pointer_refresh);
	input_virtual_fini();
	wl_list_remove(&server.new_idle_inhibitor.link);
	wl_list_remove(&server.output_power_set_mode.link);
	if (server.output_manager) {
		wl_list_remove(&server.output_manager_apply.link);
		wl_list_remove(&server.output_manager_test.link);
	}
	/* The outputs go with the backend below, and nobody is left to tell. */
	if (server.output_manager_idle)
		wl_event_source_remove(server.output_manager_idle);
	server.output_manager_idle = NULL;
	server.output_manager = NULL;
	wl_list_remove(&server.new_layer_surface.link);
	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_popup.link);
	wl_list_remove(&server.new_xdg_decoration.link);
	wl_list_remove(&server.new_server_decoration.link);
	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);
	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.request_cursor.link);
	wl_list_remove(&server.client_cursor_destroy.link);
	wl_list_remove(&server.request_set_selection.link);
	wl_list_remove(&server.request_set_primary_selection.link);
	wl_list_remove(&server.request_start_drag.link);
	wl_list_remove(&server.start_drag.link);
	wl_list_remove(&server.keyboard_grab_end.link);
	wl_list_remove(&server.new_output.link);
	wl_list_remove(&server.layout_change.link);
	/* The backend goes first: output_destroy() still needs the scene. */
	wlr_backend_destroy(server.backend);
	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wl_display_destroy(server.display);
	wallpaper_fini(&server);
	return 0;
}
