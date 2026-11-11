/*
 * Lists and switches mwc's desktops through ext-workspace-v1, as a pager or a
 * switching tool does.
 *
 *   workspacec list           one line per workspace, in the order they came:
 *                             workspace id=1 name=1 x=0 state=active caps=activate
 *                             and one for the group: group outputs=N caps=0
 *   workspacec activate ID    activate that one and wait until it says active
 *   workspacec urgent ID      wait until that one says urgent
 *
 * state is a list of active, urgent and hidden joined by +, or none. Exits 0 on
 * success, 1 on failure or after five seconds, 2 on a usage error.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "ext-workspace-v1-client.h"

#define MAX_WS 16

struct ws {
	struct ext_workspace_handle_v1 *handle;
	char id[32], name[32];
	int x;
	uint32_t state, caps;
	bool in_group;
};

static struct ext_workspace_manager_v1 *manager;
static struct ws workspaces[MAX_WS];
static int n_ws, group_outputs, group_caps = -1;
static bool done;

static void ws_id(void *data, struct ext_workspace_handle_v1 *h, const char *id)
{
	struct ws *w = data;
	snprintf(w->id, sizeof(w->id), "%s", id);
}

static void ws_name(void *data, struct ext_workspace_handle_v1 *h, const char *name)
{
	struct ws *w = data;
	snprintf(w->name, sizeof(w->name), "%s", name);
}

static void ws_coordinates(void *data, struct ext_workspace_handle_v1 *h,
	struct wl_array *coordinates)
{
	struct ws *w = data;
	if (coordinates->size >= sizeof(uint32_t))
		w->x = *(uint32_t *)coordinates->data;
}

static void ws_state(void *data, struct ext_workspace_handle_v1 *h, uint32_t state)
{
	struct ws *w = data;
	w->state = state;
}

static void ws_capabilities(void *data, struct ext_workspace_handle_v1 *h, uint32_t caps)
{
	struct ws *w = data;
	w->caps = caps;
}

static void ws_removed(void *data, struct ext_workspace_handle_v1 *h)
{
}

static const struct ext_workspace_handle_v1_listener ws_listener = {
	.id = ws_id,
	.name = ws_name,
	.coordinates = ws_coordinates,
	.state = ws_state,
	.capabilities = ws_capabilities,
	.removed = ws_removed,
};

static void group_capabilities(void *data, struct ext_workspace_group_handle_v1 *g,
	uint32_t caps)
{
	group_caps = caps;
}

static void group_output_enter(void *data, struct ext_workspace_group_handle_v1 *g,
	struct wl_output *output)
{
	group_outputs++;
}

static void group_output_leave(void *data, struct ext_workspace_group_handle_v1 *g,
	struct wl_output *output)
{
	group_outputs--;
}

static void group_workspace_enter(void *data, struct ext_workspace_group_handle_v1 *g,
	struct ext_workspace_handle_v1 *handle)
{
	for (int i = 0; i < n_ws; i++) {
		if (workspaces[i].handle == handle)
			workspaces[i].in_group = true;
	}
}

static void group_workspace_leave(void *data, struct ext_workspace_group_handle_v1 *g,
	struct ext_workspace_handle_v1 *handle)
{
}

static void group_removed(void *data, struct ext_workspace_group_handle_v1 *g)
{
}

static const struct ext_workspace_group_handle_v1_listener group_listener = {
	.capabilities = group_capabilities,
	.output_enter = group_output_enter,
	.output_leave = group_output_leave,
	.workspace_enter = group_workspace_enter,
	.workspace_leave = group_workspace_leave,
	.removed = group_removed,
};

static void manager_group(void *data, struct ext_workspace_manager_v1 *m,
	struct ext_workspace_group_handle_v1 *group)
{
	ext_workspace_group_handle_v1_add_listener(group, &group_listener, NULL);
}

static void manager_workspace(void *data, struct ext_workspace_manager_v1 *m,
	struct ext_workspace_handle_v1 *handle)
{
	if (n_ws == MAX_WS)
		return;
	struct ws *w = &workspaces[n_ws++];
	w->handle = handle;
	ext_workspace_handle_v1_add_listener(handle, &ws_listener, w);
}

static void manager_done(void *data, struct ext_workspace_manager_v1 *m)
{
	done = true;
}

static void manager_finished(void *data, struct ext_workspace_manager_v1 *m)
{
}

static const struct ext_workspace_manager_v1_listener manager_listener = {
	.workspace_group = manager_group,
	.workspace = manager_workspace,
	.done = manager_done,
	.finished = manager_finished,
};

static void global(void *data, struct wl_registry *registry, uint32_t name,
	const char *interface, uint32_t version)
{
	if (!strcmp(interface, ext_workspace_manager_v1_interface.name))
		manager = wl_registry_bind(registry, name, &ext_workspace_manager_v1_interface, 1);
	else if (!strcmp(interface, wl_output_interface.name))
		wl_registry_bind(registry, name, &wl_output_interface, 1);
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

static const char *state_text(uint32_t s)
{
	static char buf[64];
	buf[0] = '\0';
	if (s & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE)
		strcat(buf, "+active");
	if (s & EXT_WORKSPACE_HANDLE_V1_STATE_URGENT)
		strcat(buf, "+urgent");
	if (s & EXT_WORKSPACE_HANDLE_V1_STATE_HIDDEN)
		strcat(buf, "+hidden");
	return buf[0] ? buf + 1 : "none";
}

static const char *caps_text(uint32_t c)
{
	static char buf[64];
	buf[0] = '\0';
	if (c & EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE)
		strcat(buf, "+activate");
	if (c & EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_DEACTIVATE)
		strcat(buf, "+deactivate");
	if (c & EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_REMOVE)
		strcat(buf, "+remove");
	if (c & EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ASSIGN)
		strcat(buf, "+assign");
	return buf[0] ? buf + 1 : "none";
}

static struct ws *find(const char *id)
{
	for (int i = 0; i < n_ws; i++) {
		if (!strcmp(workspaces[i].id, id))
			return &workspaces[i];
	}
	return NULL;
}

/* Until the next done event. */
static int next_done(struct wl_display *display)
{
	done = false;
	while (!done)
		if (wl_display_dispatch(display) < 0)
			return -1;
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc < 2 || (strcmp(argv[1], "list") && argc < 3)) {
		fprintf(stderr, "usage: workspacec list | activate ID | urgent ID\n");
		return 2;
	}
	alarm(5);
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "workspacec: no Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!manager) {
		fprintf(stderr, "workspacec: no ext_workspace_manager_v1\n");
		return 1;
	}
	ext_workspace_manager_v1_add_listener(manager, &manager_listener, NULL);
	if (next_done(display) < 0)
		return 1;

	if (!strcmp(argv[1], "list")) {
		/* The outputs' wl_output objects may only come with the roundtrip. */
		wl_display_roundtrip(display);
		for (int i = 0; i < n_ws; i++) {
			struct ws *w = &workspaces[i];
			printf("workspace id=%s name=%s x=%d state=%s caps=%s group=%d\n", w->id,
				w->name, w->x, state_text(w->state), caps_text(w->caps), w->in_group);
		}
		printf("group outputs=%d caps=%d\n", group_outputs, group_caps);
		return 0;
	}
	struct ws *w = find(argv[2]);
	if (!w) {
		fprintf(stderr, "workspacec: no workspace %s\n", argv[2]);
		return 1;
	}
	uint32_t want = !strcmp(argv[1], "urgent") ? EXT_WORKSPACE_HANDLE_V1_STATE_URGENT :
		EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE;
	if (!strcmp(argv[1], "activate")) {
		ext_workspace_handle_v1_activate(w->handle);
		ext_workspace_manager_v1_commit(manager);
	}
	while (!(w->state & want))
		if (next_done(display) < 0)
			return 1;
	printf("%s %s\n", !strcmp(argv[1], "urgent") ? "urgent" : "active", w->id);
	return 0;
}
