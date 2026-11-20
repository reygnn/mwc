/*
 * configview.c - mwmrc applied to windows: by their app_id through config.c's client
 * rules, and the reload that applies it all again.
 */
#define _POSIX_C_SOURCE 200809L
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include "mwc.h"

/* Dialogs belong with their window, not in the program's corner or the dock. */
static bool toplevel(struct view *view)
{
	return view->wtype == WT_NORMAL && !view_parent(view);
}

int config_view_dock(struct view *view)
{
	if (!toplevel(view))
		return -1;
	return config_client_dock(view_app_id(view));
}

bool config_view_all_desktops(struct view *view)
{
	return config_client_all_desktops(view_app_id(view));
}

bool config_view_position(struct view *view, struct config_position *out)
{
	if (!toplevel(view))
		return false;
	return config_client_position(view_app_id(view), out);
}

void config_view_tiling(struct view *view)
{
	view->no_tile = view_docked(view) || !config_client_tiles(view_app_id(view));
}

/* Pick the frame colours for a window from its app_id. */
void config_view_shades(struct view *view)
{
	config_client_shades(view_app_id(view), &view->shade_active, &view->shade_inactive);
}

void config_reload(struct server *server)
{
	wlr_log(WLR_INFO, "reloading the configuration");
	menu_close(server);
	config_load();
	/* Before anything rearranges the dock: a window leaving it stays where it is. */
	struct view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->mapped)
			view->dock_order = config_view_dock(view);
	}
	theme_init();
	iconbox_config_changed(server);
	bool wallpaper = wallpaper_init(server);
	input_config_changed(server);
	outputs_config_changed(server);

	const float c[4] = { theme.root.r, theme.root.g, theme.root.b, 1 };
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		wlr_scene_rect_set_color(output->background, c);
		wlr_scene_node_set_enabled(&output->background->node,
			!theme.root_none);
		wallpaper_place(output, wallpaper);
	}
	wl_list_for_each(view, &server->views, link) {
		view_theme_changed(view);
		if (view->mapped)
			view->all_desktops = config_view_all_desktops(view);
	}
	desktop_config_changed(server);
	/* iconPlacement may have changed: every icon finds its slot anew. */
	wl_list_for_each(view, &server->views, link)
		view->icon_placed = false;
	icons_arrange(server);
	tile_config_changed(server);
	dock_config_changed(server);
}
