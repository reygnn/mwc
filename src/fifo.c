/*
 * fifo.c - wp_fifo_v1: a content update that waits for the next refresh
 *
 * A client marks an update with set_barrier, and the next one with wait_barrier:
 * the second is applied only once the first was on the screen for a refresh. That
 * is vsync for a client that doesn't want to wait for frame callbacks, which stop
 * when its window is hidden. SDL 3 takes Wayland only where a compositor offers
 * this; elsewhere it prefers X11, and an SDL program in mwc ran through Xwayland.
 *
 * wlroots 0.19 has no implementation (0.20 brings wlr_fifo_v1); this one is built
 * on the surface's synced state and its cached-state locks:
 *
 * - set_barrier and wait_barrier are double-buffered, so they live in a
 *   wlr_surface_synced state and travel with the commit they came with.
 * - A commit with wait_barrier while the barrier is up, or while earlier ones are
 *   still held, gets a lock on wl_surface.commit (client_commit), and wlroots
 *   caches it, and every commit after it, until the lock goes.
 * - The barrier goes up when an update with set_barrier is applied, and comes down
 *   at the first presentation of the output after that: the update was latched by
 *   then. Then the held commits are released, oldest first, until one raises the
 *   barrier again.
 * - The protocol lets a compositor clear the barrier early for the client to make
 *   progress. mwc does after two refresh periods if no presentation comes (the
 *   window is covered, or the update changed nothing on screen), after a second
 *   if the surface is on no output (minimized, on a monitor that is off), and at
 *   once if it isn't mapped.
 *
 * Once mwc moves to wlroots 0.20 this file goes in favour of wlr_fifo_manager_v1.
 */
#include <stdint.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>
#include "fifo-v1-protocol.h"
#include "mwc.h"

/* How long the barrier of a surface on no output holds: as Xwayland does for a
 * window nobody sees, about one update a second. */
#define OFF_SCREEN_MS 1000

struct fifo_state {
	bool set_barrier, wait_barrier;
};

struct fifo_held {
	struct wl_list link;	/* fifo.held */
	uint32_t seq;
};

struct fifo {
	struct wl_resource *resource;
	struct wlr_surface *surface;	/* NULL once it is gone */
	struct wlr_addon addon;
	struct wlr_surface_synced synced;
	struct fifo_state pending, current;
	struct wl_listener client_commit;
	struct wl_listener surface_destroy;

	bool barrier;
	struct wl_list held;	/* fifo_held, oldest first */

	struct wl_event_loop *loop;
	struct wl_event_source *timer;	/* clears the barrier when nothing else does */
	struct wl_event_source *idle;	/* picks the output, after the commit */

	/* The output whose next presentation clears the barrier, and its last
	 * commit when the barrier went up. */
	struct wlr_output *output;
	uint32_t output_seq;
	struct wl_listener output_present;
	struct wl_listener output_destroy;
};

static void unwatch_output(struct fifo *fifo)
{
	if (!fifo->output)
		return;
	wl_list_remove(&fifo->output_present.link);
	wl_list_remove(&fifo->output_destroy.link);
	fifo->output = NULL;
}

/* Lets the held commits go, oldest first, while the barrier is down. Releasing one
 * applies it, unless something else still holds it (a subsurface's parent, an
 * explicit sync fence): then the next one may go a refresh too early. */
static void release_held(struct fifo *fifo)
{
	while (fifo->surface && !fifo->barrier && !wl_list_empty(&fifo->held)) {
		struct fifo_held *h = wl_container_of(fifo->held.next, h, link);
		uint32_t seq = h->seq;
		wl_list_remove(&h->link);
		free(h);
		wlr_surface_unlock_cached(fifo->surface, seq);
	}
}

static void clear_barrier(struct fifo *fifo)
{
	fifo->barrier = false;
	unwatch_output(fifo);
	if (fifo->timer)
		wl_event_source_timer_update(fifo->timer, 0);
	release_held(fifo);
}

static void output_present(struct wl_listener *listener, void *data)
{
	struct fifo *fifo = wl_container_of(listener, fifo, output_present);
	struct wlr_output_event_present *event = data;
	/* A presentation of a frame committed before the update was applied doesn't
	 * show it yet (DRM presents a commit later; headless at once). */
	if ((int32_t)(event->commit_seq - fifo->output_seq) <= 0)
		return;
	clear_barrier(fifo);
}

static void output_destroy(struct wl_listener *listener, void *data)
{
	struct fifo *fifo = wl_container_of(listener, fifo, output_destroy);
	clear_barrier(fifo);
}

static int timer_fired(void *data)
{
	clear_barrier(data);
	return 0;
}

/* After the commit that raised the barrier: by now a newly mapped window is in the
 * scene, and the surface knows its outputs. */
static void watch_output(void *data)
{
	struct fifo *fifo = data;
	fifo->idle = NULL;
	if (!fifo->barrier || !fifo->surface)
		return;

	struct wlr_output *output = NULL;
	struct wlr_surface_output *so;
	wl_list_for_each(so, &fifo->surface->current_outputs, link) {
		if (so->output->enabled) {
			output = so->output;
			break;
		}
	}
	if (!output) {
		if (!fifo->surface->mapped)
			clear_barrier(fifo);
		else
			wl_event_source_timer_update(fifo->timer, OFF_SCREEN_MS);
		return;
	}

	unwatch_output(fifo);
	fifo->output = output;
	fifo->output_seq = output->commit_seq;
	fifo->output_present.notify = output_present;
	wl_signal_add(&output->events.present, &fifo->output_present);
	fifo->output_destroy.notify = output_destroy;
	wl_signal_add(&output->events.destroy, &fifo->output_destroy);
	int mhz = output->refresh > 0 ? output->refresh : 60000;
	wl_event_source_timer_update(fifo->timer, 2 * 1000000 / mhz + 1);
}

/* ---- the synced state ---------------------------------------------------- */

/* Both requests count for one commit only, so moving a state empties its source. */
static void state_move(void *dst, void *src)
{
	struct fifo_state *d = dst, *s = src;
	*d = *s;
	*s = (struct fifo_state){0};
}

static void state_commit(struct wlr_surface_synced *synced)
{
	struct fifo *fifo = wl_container_of(synced, fifo, synced);
	if (!fifo->current.set_barrier)
		return;
	fifo->barrier = true;
	unwatch_output(fifo);
	wl_event_source_timer_update(fifo->timer, 0);
	if (!fifo->idle)
		fifo->idle = wl_event_loop_add_idle(fifo->loop, watch_output, fifo);
	if (!fifo->idle)
		clear_barrier(fifo);	/* out of memory: rather no barrier than a stuck one */
}

static const struct wlr_surface_synced_impl synced_impl = {
	.state_size = sizeof(struct fifo_state),
	.move_state = state_move,
	.commit = state_commit,
};

static void client_commit(struct wl_listener *listener, void *data)
{
	struct fifo *fifo = wl_container_of(listener, fifo, client_commit);
	if (!fifo->pending.wait_barrier)
		return;
	/* The protocol: ignored for a subsurface in synchronized mode, whose commits
	 * wait for its parent's anyway. */
	struct wlr_subsurface *sub = wlr_subsurface_try_from_wlr_surface(fifo->surface);
	if (sub && sub->synchronized)
		return;
	if (!fifo->barrier && wl_list_empty(&fifo->held))
		return;
	struct fifo_held *h = calloc(1, sizeof(*h));
	if (!h) {
		wl_resource_post_no_memory(fifo->resource);
		return;
	}
	h->seq = wlr_surface_lock_pending(fifo->surface);
	wl_list_insert(fifo->held.prev, &h->link);
}

/* ---- lifetime ------------------------------------------------------------ */

/* The surface and the wp_fifo_v1 object can go in either order. When the object
 * goes, the held commits are let go at once (unlock); when the surface goes,
 * wlroots drops them itself. */
static void detach_surface(struct fifo *fifo, bool unlock)
{
	struct wlr_surface *surface = fifo->surface;
	if (!surface)
		return;
	fifo->barrier = false;
	while (!wl_list_empty(&fifo->held)) {
		struct fifo_held *h = wl_container_of(fifo->held.next, h, link);
		uint32_t seq = h->seq;
		wl_list_remove(&h->link);
		free(h);
		if (unlock)
			wlr_surface_unlock_cached(surface, seq);
	}
	/* Applying them may have raised the barrier and asked for an idle again. */
	fifo->barrier = false;
	unwatch_output(fifo);
	if (fifo->idle)
		wl_event_source_remove(fifo->idle);
	fifo->idle = NULL;
	wl_event_source_remove(fifo->timer);
	fifo->timer = NULL;
	wl_list_remove(&fifo->client_commit.link);
	wl_list_remove(&fifo->surface_destroy.link);
	wlr_surface_synced_finish(&fifo->synced);
	wlr_addon_finish(&fifo->addon);
	fifo->surface = NULL;
}

static void surface_destroy(struct wl_listener *listener, void *data)
{
	struct fifo *fifo = wl_container_of(listener, fifo, surface_destroy);
	detach_surface(fifo, false);
}

static void addon_destroy(struct wlr_addon *addon)
{
	struct fifo *fifo = wl_container_of(addon, fifo, addon);
	detach_surface(fifo, false);
}

/* Marks a surface as having a wp_fifo_v1, for the already_exists error. */
static const struct wlr_addon_interface addon_impl = {
	.name = "mwc_fifo",
	.destroy = addon_destroy,
};

static void fifo_resource_destroy(struct wl_resource *resource)
{
	struct fifo *fifo = wl_resource_get_user_data(resource);
	detach_surface(fifo, true);
	free(fifo);
}

/* ---- requests ------------------------------------------------------------ */

static struct fifo *fifo_from_resource(struct wl_resource *resource)
{
	struct fifo *fifo = wl_resource_get_user_data(resource);
	if (!fifo->surface) {
		wl_resource_post_error(resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
			"the surface of this wp_fifo_v1 is gone");
		return NULL;
	}
	return fifo;
}

static void fifo_set_barrier(struct wl_client *client, struct wl_resource *resource)
{
	struct fifo *fifo = fifo_from_resource(resource);
	if (fifo)
		fifo->pending.set_barrier = true;
}

static void fifo_wait_barrier(struct wl_client *client, struct wl_resource *resource)
{
	struct fifo *fifo = fifo_from_resource(resource);
	if (fifo)
		fifo->pending.wait_barrier = true;
}

static void fifo_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wp_fifo_v1_interface fifo_impl = {
	.set_barrier = fifo_set_barrier,
	.wait_barrier = fifo_wait_barrier,
	.destroy = fifo_destroy,
};

static void manager_get_fifo(struct wl_client *client, struct wl_resource *resource,
	uint32_t id, struct wl_resource *surface_resource)
{
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (wlr_addon_find(&surface->addons, NULL, &addon_impl)) {
		wl_resource_post_error(resource, WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS,
			"this surface has a wp_fifo_v1 already");
		return;
	}

	struct fifo *fifo = calloc(1, sizeof(*fifo));
	if (!fifo) {
		wl_client_post_no_memory(client);
		return;
	}
	fifo->loop = wl_display_get_event_loop(wl_client_get_display(client));
	fifo->resource = wl_resource_create(client, &wp_fifo_v1_interface,
		wl_resource_get_version(resource), id);
	fifo->timer = wl_event_loop_add_timer(fifo->loop, timer_fired, fifo);
	if (!fifo->resource || !fifo->timer ||
			!wlr_surface_synced_init(&fifo->synced, surface, &synced_impl,
				&fifo->pending, &fifo->current)) {
		if (fifo->timer)
			wl_event_source_remove(fifo->timer);
		if (fifo->resource)
			wl_resource_destroy(fifo->resource);
		free(fifo);
		wl_client_post_no_memory(client);
		return;
	}
	fifo->surface = surface;
	wl_list_init(&fifo->held);
	wlr_addon_init(&fifo->addon, &surface->addons, NULL, &addon_impl);
	fifo->client_commit.notify = client_commit;
	wl_signal_add(&surface->events.client_commit, &fifo->client_commit);
	fifo->surface_destroy.notify = surface_destroy;
	wl_signal_add(&surface->events.destroy, &fifo->surface_destroy);
	wl_resource_set_implementation(fifo->resource, &fifo_impl, fifo,
		fifo_resource_destroy);
}

static void manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wp_fifo_manager_v1_interface manager_impl = {
	.destroy = manager_destroy,
	.get_fifo = manager_get_fifo,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version,
	uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&wp_fifo_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

void fifo_init(struct server *server)
{
	if (!wl_global_create(server->display, &wp_fifo_manager_v1_interface, 1, NULL,
			manager_bind))
		wlr_log(WLR_ERROR, "no wp_fifo_manager_v1: SDL programs will take Xwayland");
}
