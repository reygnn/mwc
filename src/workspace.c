/*
 * workspace.c - ext-workspace-v1: mwc's desktops for panels and switching tools
 *
 * One workspace group for all outputs, as the desktops are global (desktop.c), with
 * MWC_DESKTOPS workspaces in it, named and with the ids "1" to "4", in a row
 * (coordinates 0 to 3). The current one is active; one whose window asked for the
 * focus is urgent until it becomes current. The only request mwc takes is activate,
 * applied on commit: switching desktops. The number is fixed, so the group can't
 * create workspaces and workspaces can't be removed, assigned or deactivated;
 * the capabilities say so.
 *
 * wlroots 0.19 has no implementation. Each client that binds the manager gets its
 * own group and workspace objects; their user data points back to it, or is NULL
 * once the manager is gone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include "ext-workspace-v1-protocol.h"
#include "mwc.h"

struct ws_client {
	struct wl_list link; /* clients */
	struct server *server;
	struct wl_resource *manager;
	struct wl_resource *group;
	struct wl_resource *workspaces[MWC_DESKTOPS];
	int pending; /* the desktop an activate asked for, -1 for none */
};

/* One per output, for its wl_output resources. */
struct ws_output {
	struct wlr_output *output;
	struct wl_listener bind;
	struct wl_listener destroy;
};

static struct wl_list clients;
static struct server *ws_server;
static bool urgent[MWC_DESKTOPS];

static uint32_t state_of(int n)
{
	uint32_t state = 0;
	if (n == ws_server->desktop)
		state |= EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE;
	if (urgent[n])
		state |= EXT_WORKSPACE_HANDLE_V1_STATE_URGENT;
	return state;
}

static void send_states(void)
{
	struct ws_client *c;
	wl_list_for_each(c, &clients, link) {
		for (int i = 0; i < MWC_DESKTOPS; i++) {
			if (c->workspaces[i])
				ext_workspace_handle_v1_send_state(c->workspaces[i], state_of(i));
		}
		ext_workspace_manager_v1_send_done(c->manager);
	}
}

void workspace_changed(struct server *server)
{
	urgent[server->desktop] = false;
	send_states();
}

void workspace_urgent(struct server *server, int n)
{
	if (n < 0 || n >= MWC_DESKTOPS || n == server->desktop || urgent[n])
		return;
	urgent[n] = true;
	send_states();
}

/* ---- workspaces ----------------------------------------------------------- */

static struct ws_client *client_of(struct wl_resource *resource, int *n)
{
	struct ws_client *c = wl_resource_get_user_data(resource);
	if (c && n) {
		*n = -1;
		for (int i = 0; i < MWC_DESKTOPS; i++) {
			if (c->workspaces[i] == resource)
				*n = i;
		}
	}
	return c;
}

static void ws_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void ws_activate(struct wl_client *client, struct wl_resource *resource)
{
	int n;
	struct ws_client *c = client_of(resource, &n);
	if (c && n >= 0)
		c->pending = n;
}

/* The capabilities say these aren't there; the protocol says to ignore them. */
static void ws_ignore(struct wl_client *client, struct wl_resource *resource)
{
}

static void ws_assign(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *group)
{
}

static const struct ext_workspace_handle_v1_interface ws_impl = {
	.destroy = ws_destroy,
	.activate = ws_activate,
	.deactivate = ws_ignore,
	.assign = ws_assign,
	.remove = ws_ignore,
};

static void ws_resource_gone(struct wl_resource *resource)
{
	int n;
	struct ws_client *c = client_of(resource, &n);
	if (c && n >= 0)
		c->workspaces[n] = NULL;
}

/* ---- the group ------------------------------------------------------------ */

static void group_create_workspace(struct wl_client *client,
	struct wl_resource *resource, const char *name)
{
}

static void group_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct ext_workspace_group_handle_v1_interface group_impl = {
	.create_workspace = group_create_workspace,
	.destroy = group_destroy,
};

static void group_resource_gone(struct wl_resource *resource)
{
	struct ws_client *c = wl_resource_get_user_data(resource);
	if (c)
		c->group = NULL;
}

/* Every wl_output this client has for the output: output_enter or output_leave. */
static void send_output(struct ws_client *c, struct wlr_output *output, bool enter)
{
	if (!c->group)
		return;
	struct wl_client *client = wl_resource_get_client(c->group);
	struct wl_resource *r;
	wl_resource_for_each(r, &output->resources) {
		if (wl_resource_get_client(r) != client)
			continue;
		if (enter)
			ext_workspace_group_handle_v1_send_output_enter(c->group, r);
		else
			ext_workspace_group_handle_v1_send_output_leave(c->group, r);
	}
}

/* ---- the manager ---------------------------------------------------------- */

static void manager_commit(struct wl_client *client, struct wl_resource *resource)
{
	struct ws_client *c = wl_resource_get_user_data(resource);
	if (!c || c->pending < 0)
		return;
	int n = c->pending;
	c->pending = -1;
	desktop_switch(c->server, n);
}

static void forget(struct ws_client *c)
{
	if (c->group)
		wl_resource_set_user_data(c->group, NULL);
	for (int i = 0; i < MWC_DESKTOPS; i++) {
		if (c->workspaces[i])
			wl_resource_set_user_data(c->workspaces[i], NULL);
	}
	wl_list_remove(&c->link);
	free(c);
}

static void manager_stop(struct wl_client *client, struct wl_resource *resource)
{
	ext_workspace_manager_v1_send_finished(resource);
	wl_resource_destroy(resource);
}

static const struct ext_workspace_manager_v1_interface manager_impl = {
	.commit = manager_commit,
	.stop = manager_stop,
};

static void manager_resource_gone(struct wl_resource *resource)
{
	struct ws_client *c = wl_resource_get_user_data(resource);
	if (c)
		forget(c);
}

static void manager_bind(struct wl_client *client, void *data, uint32_t version,
	uint32_t id)
{
	struct server *server = data;
	struct ws_client *c = calloc(1, sizeof(*c));
	struct wl_resource *manager = wl_resource_create(client,
		&ext_workspace_manager_v1_interface, version, id);
	if (!c || !manager) {
		free(c);
		wl_client_post_no_memory(client);
		return;
	}
	c->server = server;
	c->manager = manager;
	c->pending = -1;
	wl_list_insert(&clients, &c->link);
	wl_resource_set_implementation(manager, &manager_impl, c, manager_resource_gone);

	c->group = wl_resource_create(client, &ext_workspace_group_handle_v1_interface,
		version, 0);
	if (!c->group) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(c->group, &group_impl, c, group_resource_gone);
	ext_workspace_manager_v1_send_workspace_group(manager, c->group);
	ext_workspace_group_handle_v1_send_capabilities(c->group, 0);
	struct output *o;
	wl_list_for_each(o, &server->outputs, link)
		send_output(c, o->wlr_output, true);

	for (int i = 0; i < MWC_DESKTOPS; i++) {
		struct wl_resource *ws = wl_resource_create(client,
			&ext_workspace_handle_v1_interface, version, 0);
		if (!ws) {
			wl_client_post_no_memory(client);
			return;
		}
		c->workspaces[i] = ws;
		wl_resource_set_implementation(ws, &ws_impl, c, ws_resource_gone);
		ext_workspace_manager_v1_send_workspace(manager, ws);
		char text[16];
		snprintf(text, sizeof(text), "%d", i + 1);
		ext_workspace_handle_v1_send_id(ws, text);
		ext_workspace_handle_v1_send_name(ws, text);
		struct wl_array coordinates;
		wl_array_init(&coordinates);
		uint32_t *x = wl_array_add(&coordinates, sizeof(*x));
		if (x) {
			*x = i;
			ext_workspace_handle_v1_send_coordinates(ws, &coordinates);
		}
		wl_array_release(&coordinates);
		ext_workspace_handle_v1_send_state(ws, state_of(i));
		ext_workspace_handle_v1_send_capabilities(ws,
			EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
		ext_workspace_group_handle_v1_send_workspace_enter(c->group, ws);
	}
	ext_workspace_manager_v1_send_done(manager);
}

/* ---- outputs coming and going ----------------------------------------------- */

static void output_bind(struct wl_listener *listener, void *data)
{
	struct wlr_output_event_bind *event = data;
	struct wl_client *client = wl_resource_get_client(event->resource);
	struct ws_client *c;
	wl_list_for_each(c, &clients, link) {
		if (!c->group || wl_resource_get_client(c->group) != client)
			continue;
		ext_workspace_group_handle_v1_send_output_enter(c->group, event->resource);
		ext_workspace_manager_v1_send_done(c->manager);
	}
}

static void output_destroy(struct wl_listener *listener, void *data)
{
	struct ws_output *wo = wl_container_of(listener, wo, destroy);
	struct ws_client *c;
	wl_list_for_each(c, &clients, link) {
		send_output(c, wo->output, false);
		ext_workspace_manager_v1_send_done(c->manager);
	}
	wl_list_remove(&wo->bind.link);
	wl_list_remove(&wo->destroy.link);
	free(wo);
}

void workspace_output_new(struct wlr_output *output)
{
	struct ws_output *wo = calloc(1, sizeof(*wo));
	if (!wo)
		return;
	wo->output = output;
	wo->bind.notify = output_bind;
	wl_signal_add(&output->events.bind, &wo->bind);
	wo->destroy.notify = output_destroy;
	wl_signal_add(&output->events.destroy, &wo->destroy);
	/* Clients that bound the manager before this output came hear of it when they
	 * bind its wl_output (output_bind). */
}

void workspace_init(struct server *server)
{
	ws_server = server;
	wl_list_init(&clients);
	if (!wl_global_create(server->display, &ext_workspace_manager_v1_interface, 1,
			server, manager_bind))
		wlr_log(WLR_ERROR, "no ext_workspace_manager_v1: desktops can't be switched");
}
