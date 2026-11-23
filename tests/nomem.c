/*
 * nomem.c - linked into build/mwc-nomem with -Wl,--wrap for the scene's constructors,
 * so that one of them can fail as without memory. Nothing in mwc's own code makes an
 * allocation in wlroots fail, and these return NULL then.
 *
 * <MWC_TEST_STATE>.nomem holds "FUNCTION N [K]": from the Nth call of FUNCTION from
 * now, K of them (1) return NULL. The next call of any of them reads the file and
 * removes it; present at startup, it counts from there. tests/nomem.sh uses it.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_scene.h>

static char path[4096];
static char armed[64];
static int countdown; /* calls of armed until the first that fails; 0: none */
static int failing;   /* how many fail from there */

/* Before main(): testhook.c takes MWC_TEST_STATE out of the environment, for the
 * programs mwc starts. */
__attribute__((constructor)) static void find_path(void)
{
	const char *state = getenv("MWC_TEST_STATE");
	if (state && snprintf(path, sizeof(path), "%s.nomem", state) >= (int)sizeof(path))
		path[0] = '\0';
}

static bool fails(const char *function)
{
	if (path[0]) {
		FILE *f = fopen(path, "r");
		if (f) {
			int n = fscanf(f, "%63s %d %d", armed, &countdown, &failing);
			if (n < 3)
				failing = 1;
			if (n < 2 || countdown < 1 || failing < 1)
				countdown = 0;
			fclose(f);
			remove(path);
		}
	}
	if (!countdown || strcmp(function, armed))
		return false;
	if (countdown > 1) {
		countdown--;
		return false;
	}
	if (!--failing)
		countdown = 0;
	fprintf(stderr, "nomem: %s fails\n", function);
	return true;
}

/* Each as wlroots 0.19 declares it, failing or passed on. */
#define WRAP(type, name, params, args) \
	type __real_##name params; \
	type __wrap_##name params; \
	type __wrap_##name params \
	{ \
		return fails(#name) ? NULL : __real_##name args; \
	}

WRAP(struct wlr_scene *, wlr_scene_create, (void), ())
WRAP(struct wlr_scene_output_layout *, wlr_scene_attach_output_layout,
	(struct wlr_scene *scene, struct wlr_output_layout *layout), (scene, layout))
WRAP(struct wlr_scene_tree *, wlr_scene_tree_create, (struct wlr_scene_tree *parent),
	(parent))
WRAP(struct wlr_scene_rect *, wlr_scene_rect_create,
	(struct wlr_scene_tree *parent, int width, int height, const float color[static 4]),
	(parent, width, height, color))
WRAP(struct wlr_scene_buffer *, wlr_scene_buffer_create,
	(struct wlr_scene_tree *parent, struct wlr_buffer *buffer), (parent, buffer))
WRAP(struct wlr_scene_tree *, wlr_scene_xdg_surface_create,
	(struct wlr_scene_tree *parent, struct wlr_xdg_surface *xdg_surface),
	(parent, xdg_surface))
WRAP(struct wlr_scene_layer_surface_v1 *, wlr_scene_layer_surface_v1_create,
	(struct wlr_scene_tree *parent, struct wlr_layer_surface_v1 *layer_surface),
	(parent, layer_surface))
WRAP(struct wlr_scene_output *, wlr_scene_output_create,
	(struct wlr_scene *scene, struct wlr_output *output), (scene, output))
