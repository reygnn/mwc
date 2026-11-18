/*
 * mwc - a Motif-style Wayland compositor. Shared types, tinywl's structure: one
 * server, listeners embedded in their objects, wlr_scene doing the rendering.
 */
#ifndef MWC_H
#define MWC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <cairo.h>
#include <pango/pango.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>

struct wlr_input_device;
struct wlr_layer_surface_v1;
struct wlr_surface;
struct wlr_xdg_surface;
struct wlr_xdg_toplevel;
struct wlr_xwayland_surface;

#define MWC_SHELL_LAYERS 4 /* background, bottom, top, overlay */

#define MWC_GAP 8 /* around tiled and snapped windows: the desktop stays in reach */

#define MWC_DESKTOPS 4 /* shared by all outputs, see desktop.c */

/* ---- theme ------------------------------------------------------------ */

struct mwc_color {
	float r, g, b;
};

/* Motif's 3D colour set. */
struct mwc_shade {
	struct mwc_color bg, top, bottom, fg;
};

struct mwc_theme {
	struct mwc_shade active, inactive, menu;
	struct mwc_color select; /* armed menu item */
	struct mwc_color root;
	bool root_none;          /* a tool draws the desktop */
	/* Title text for clients with their own background; unset: by contrast. */
	struct mwc_color active_fg, inactive_fg;
	bool has_active_fg, has_inactive_fg;
	char *font;              /* Pango name */
	int border;
	int title_height;
	int menu_item_height;
	int bevel;
};

extern struct mwc_theme theme;

void theme_init(void); /* after config_load() */
struct mwc_shade shade_from(struct mwc_color bg, struct mwc_color fg);
struct mwc_color color_mix(struct mwc_color a, struct mwc_color b, float f);
struct mwc_color color_contrast(struct mwc_color bg);
void set_source(cairo_t *cr, struct mwc_color c);
void draw_bevel(cairo_t *cr, double x, double y, double w, double h,
	int thickness, const struct mwc_shade *s, bool in);
void draw_groove_h(cairo_t *cr, double x, double y, double w,
	const struct mwc_shade *s);
void draw_groove_v(cairo_t *cr, double x, double y, double h,
	const struct mwc_shade *s);
void draw_text(cairo_t *cr, const char *text, const char *font,
	struct mwc_color color, double x, double y, double w, double h,
	bool center);
void text_extent(const char *font, const char *text, int *w, int *h);
/* Cached per name; not to be freed. */
const PangoFontDescription *font_description(const char *font);

/* ---- cairo-backed wlr_buffer --------------------------------------------- */

struct cairo_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
	cairo_t *cr;
};

struct cairo_buffer *cairo_buffer_create(int width, int height);
/* The scene keeps it while shown. */
void cairo_buffer_drop(struct cairo_buffer *buf);

/* ---- throttle, in main.c ------------------------------------------------- */

/* For work a client or a fast mouse could ask for faster than it is worth doing:
 * the first call runs at once, then at most one per ms, the last one asked for. */
struct throttle {
	int ms;
	void (*run)(void *data);
	void *data;
	struct wl_event_source *timer;
	bool resting, due;
};
void throttle_call(struct throttle *t, struct wl_event_loop *loop);
/* Drops a call that waits, and the timer. */
void throttle_cancel(struct throttle *t);

/* ---- views -------------------------------------------------------------- */

enum view_type {
	VIEW_XDG,
	VIEW_XWAYLAND,
};

/* Motif's clientDecoration and clientFunctions, bits as in _MOTIF_WM_HINTS. */
enum {
	DECOR_BORDER = 1 << 1,
	DECOR_RESIZEH = 1 << 2,
	DECOR_TITLE = 1 << 3,
	DECOR_MENU = 1 << 4,
	DECOR_MINIMIZE = 1 << 5,
	DECOR_MAXIMIZE = 1 << 6,
	DECOR_ALL = 0x7e,
};

enum {
	FUNC_RESIZE = 1 << 1,
	FUNC_MOVE = 1 << 2,
	FUNC_MINIMIZE = 1 << 3,
	FUNC_MAXIMIZE = 1 << 4,
	FUNC_CLOSE = 1 << 5,
	FUNC_ALL = 0x3e,
};

enum window_type {
	WT_NORMAL,
	WT_DIALOG,
	WT_UTILITY,
	WT_SPLASH,
	WT_DOCK,
	WT_DESKTOP,
	WT_NOTIFICATION,
};

/* SNAP_MAXIMIZE is only a target: the window ends up SNAP_NONE and maximized. */
enum snap {
	SNAP_NONE,
	SNAP_LEFT,
	SNAP_RIGHT,
	SNAP_TOP_LEFT,
	SNAP_TOP_RIGHT,
	SNAP_BOTTOM_LEFT,
	SNAP_BOTTOM_RIGHT,
	SNAP_MAXIMIZE,
};

enum snap_dir {
	SNAP_DIR_LEFT,
	SNAP_DIR_RIGHT,
	SNAP_DIR_UP,
	SNAP_DIR_DOWN,
};

enum frame_part {
	PART_NONE = 0,
	PART_CLIENT,
	PART_TITLE,
	PART_TAB,   /* see frame_tab_at() */
	PART_MENU_BUTTON,
	PART_MINIMIZE_BUTTON,
	PART_MAXIMIZE_BUTTON,
	PART_RESIZE,
};

struct tab_group {
	struct wl_list members; /* view.tab_link, in tab order */
	struct view *shown;
	bool busy;              /* switching */
	int first_visible;      /* more than fit the title bar, see frame.c */
};

struct view {
	struct wl_list link; /* server.views, front to back */
	struct server *server;
	enum view_type type;
	union {
		struct wlr_xdg_toplevel *xdg_toplevel;
		struct wlr_xwayland_surface *xsurface;
	};

	struct wlr_scene_tree *tree;         /* frame and client */
	struct wlr_scene_tree *content_tree; /* the client's surfaces */
	/* Top (with the title bar), bottom, left, right: not one buffer the client
	 * mostly covers. */
	struct wlr_scene_buffer *frame_nodes[4];
	/* Behind the client: shows through transparency, takes clicks outside its input
	 * region. */
	struct wlr_scene_rect *frame_fill;
	struct wlr_scene_rect *backdrop; /* black behind a fullscreen window (xdg-shell) */
	int drawn_w, drawn_h;  /* at the last frame_render() */
	char *drawn_title;     /* see view_update_title() */
	char *known_app_id;    /* see view_app_id_changed() */
	struct wlr_scene_buffer *icon_node;

	/* x, y: the frame's top left in the layout; width, height: the client's. */
	int x, y, width, height;
	bool mapped;
	bool placed;    /* by the client: no cascading */
	bool ssd;
	bool decorated; /* ssd && !fullscreen */
	enum window_type wtype;
	unsigned decor, funcs; /* see view_update_properties() */
	unsigned base_funcs;   /* before tiling takes some away */
	uint32_t wm_caps;      /* last sent, see xdg_publish_caps() */
	uint32_t motif_flags, motif_funcs, motif_decor; /* X11 */
	/* 0: no limit. */
	int min_w, min_h, max_w, max_h, base_w, base_h, inc_w, inc_h;
	float min_aspect, max_aspect;
	int gravity;
	bool hidden_with_parent; /* a dialog of a minimized window */
	struct tab_group *group; /* NULL: alone */
	struct wl_list tab_link; /* tab_group.members */
	bool tab_hidden;
	int desktop;             /* 0-based */
	bool off_desktop;
	bool attention;          /* see desktop_attention() */
	bool all_desktops;       /* Mwm*NAME*allDesktops, read at map */
	bool sticky;             /* "On All Desktops" */
	int dock_order;          /* Mwm*NAME*dock: its mwmrc line; -1: not docked */
	int tab_own_w, tab_own_h; /* before joining; 0: its own choice */
	void *deco;              /* xdg-decoration, or NULL */
	bool activated;
	bool maximized;
	bool minimized;
	bool fullscreen;
	enum snap snap;
	/* Tiling, see tile.c. */
	bool tiled;
	bool tile_main;
	bool shaded;      /* rolled up in the stack */
	bool auto_shaded; /* by the layout, for lack of room */
	bool tile_back;   /* back into the layout after fullscreen */
	bool tile_nofit;  /* this round: too big for its cell */
	bool no_tile;     /* DoNotTile, or not in TileOnly */
	long tile_seq;    /* lowest: the main window */
	struct wlr_box tile; /* the frame's cell */
	struct mwc_shade shade_active, shade_inactive;
	struct wlr_box saved;  /* before maximize, snap, fullscreen */
	double fit_px, fit_py; /* on the output its state fills */
	struct output *fit_output; /* that output, while it exists */
	/* Edges kept until an xdg client has acked its new size. */
	uint32_t anchor_edges;
	int anchor_right, anchor_bottom;
	uint32_t anchor_serial;
	/* Right and/or bottom, from a geometry entry: kept as it resizes itself, until
	 * moved, resized or grouped. */
	uint32_t gravity_edges;
	int gravity_extra_w, gravity_extra_h; /* the frame's size then */
	int requested_w, requested_h; /* xdg */
	int icon_x, icon_y;
	bool icon_placed;
	long icon_seq;   /* order of becoming an icon */
	/* Its place in server.views, higher in front: the icon box's place in Alt+Tab
	 * goes by it. Set wherever the list is reordered. */
	long raise_seq;

	enum frame_part pressed; /* held down */
	bool pressed_inside;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_minimize;
	struct wl_listener request_fullscreen;
	struct wl_listener request_show_window_menu;
	struct wl_listener set_title;
	struct wl_listener set_app_id; /* or WM_CLASS */
	/* X11 */
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener request_configure;
	struct wl_listener request_activate;
	struct wl_listener set_decorations;
	struct wl_listener focus_in;
	bool been_mapped;
	int unmap_x, unmap_y; /* where it was withdrawn */
	struct wl_listener set_parent;
	struct wl_listener set_window_type;
	struct wl_listener set_strut_partial;
	struct wl_listener request_above;
	struct wl_listener request_below;
	struct wl_listener set_override_redirect;
	struct wl_listener request_sticky;
	struct wl_listener request_shaded;
	struct wl_listener request_demands_attention;
	struct wl_listener set_hints;
};

/* Not counting another desktop: what restoring undoes. */
static inline bool view_hidden_by_state(const struct view *view)
{
	return view->minimized || view->hidden_with_parent || view->tab_hidden;
}

static inline bool view_docked(const struct view *view)
{
	return view->dock_order >= 0;
}

static inline bool view_hidden(const struct view *view)
{
	return view_hidden_by_state(view) || view->off_desktop;
}

/* Override-redirect X11 windows: menus, tooltips, drag icons. */
struct unmanaged {
	struct wl_list link;
	struct server *server;
	struct wlr_xwayland_surface *xsurface;
	struct wlr_scene_tree *tree;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener request_configure;
	struct wl_listener set_geometry;
	struct wl_listener set_override_redirect;
};

/* ---- menus -------------------------------------------------------------- */

enum menu_action {
	ACT_NONE,
	ACT_RESTORE,
	ACT_MOVE,
	ACT_SIZE,
	ACT_MINIMIZE,
	ACT_MAXIMIZE,
	ACT_TOGGLE_MAXIMIZE,
	ACT_LOWER,
	ACT_CLOSE,
	ACT_NEW_WINDOW,
	ACT_TAB_JOIN,
	ACT_TAB_NEXT,
	ACT_TAB_PREVIOUS,
	ACT_TAB_DETACH,
	ACT_SHUFFLE_UP,
	ACT_SHUFFLE_DOWN,
	ACT_TOGGLE_TILING,
	ACT_RAISE_TILED,
	ACT_LOWER_TILED,
	ACT_TOGGLE_STICKY,
	ACT_TO_DESKTOP, /* + n: to desktop n */
	ACT_TO_DESKTOP_LAST = ACT_TO_DESKTOP + MWC_DESKTOPS - 1,
	ACT_RUN,
	ACT_RELOAD,
	ACT_QUIT,
};

struct menu_item {
	const char *label;     /* NULL: separator */
	char mnemonic;         /* 0: none */
	const char *accel;
	enum menu_action action;
	bool sensitive;
	const char *command;   /* ACT_RUN; config.c owns it */
};

#define MENU_MAX_ITEMS 32

struct menu {
	bool open;
	const char *title;
	struct menu_item items[MENU_MAX_ITEMS];
	int n_items;
	int armed;              /* -1: none */
	struct view *target;    /* window menu */
	struct wlr_scene_buffer *node;
	int x, y, width, height;
	int item_y[MENU_MAX_ITEMS], item_h[MENU_MAX_ITEMS];
	uint32_t opened_msec;
	bool from_button;      /* the frame's menu button */
	double open_x, open_y;
};

/* ---- server ------------------------------------------------------------- */

enum cursor_mode {
	CURSOR_PASSTHROUGH,
	CURSOR_MOVE,
	CURSOR_RESIZE,
	CURSOR_BUTTON, /* a frame or icon box button held down */
	CURSOR_MENU,
};

struct server {
	struct wl_display *display;
	struct wl_event_loop *event_loop;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;

	/* Bottom to top. */
	struct wlr_scene_tree *layer_bg;
	struct wlr_scene_tree *layer_shell[MWC_SHELL_LAYERS]; /* see main() */
	struct wlr_scene_tree *layer_icons;
	struct wlr_scene_tree *layer_below;
	struct wlr_scene_tree *layer_views;
	struct wlr_scene_tree *layer_fullscreen; /* when focused */
	struct wlr_scene_tree *layer_unmanaged;
	struct wlr_scene_tree *layer_menu;
	struct wlr_scene_tree *layer_feedback; /* outline, move/size box */
	struct wlr_scene_tree *layer_drag;

	struct wl_listener new_layer_surface;
	struct wlr_layer_surface_v1 *focused_layer;   /* has the keyboard */
	struct wlr_layer_surface_v1 *exclusive_layer; /* insists on it */

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_listener new_xdg_decoration;
	struct wl_listener new_server_decoration;
	struct wlr_server_decoration_manager *kde_decorations;
	struct wl_list views;
	struct wl_list unmanaged;

	struct wlr_xwayland *xwayland;
	struct wl_listener xwayland_ready;
	struct wl_listener new_xwayland_surface;
	uint32_t atom_motif_hints, atom_motif_info, atom_frame_extents;
	uint32_t atom_net_wm_state, atom_state_above, atom_state_below;
	uint32_t atom_state_fullscreen, atom_state_max_vert, atom_state_max_horz;
	uint32_t atom_state_sticky;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	const char *cursor_name;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wlr_surface *client_cursor; /* the last one a client set, for the tests */
	struct wl_listener client_cursor_destroy;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;
	struct wl_listener keyboard_grab_end;
	bool refocus_after_grab; /* held back by a keyboard grab */
	struct wl_event_source *pointer_refresh; /* see input_pointer_changed_soon() */
	/* Down in a client, not yet up: its implicit grab. */
	uint32_t forwarded_buttons[16];
	int n_forwarded;
	double grab_surface_x, grab_surface_y; /* that surface's origin */
	struct wl_list keyboards;

	enum cursor_mode cursor_mode;
	struct view *grabbed_view;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;
	bool grab_until_click; /* from the menu or keyboard */
	uint32_t grab_button;
	int grab_tab;          /* shown unless this becomes a drag */
	struct view *grab_take; /* the one window a drag takes along */
	struct view *grab_drop; /* the window it would join */
	int tab_wheel;         /* fractions of a wheel step */
	int icon_wheel;
	enum frame_part grab_part;
	bool grab_outline;
	bool grab_detached;  /* past the drag threshold */
	bool grab_changed;
	bool grab_detaching; /* the grab is freeing the window */
	double grab_px, grab_py;
	struct wlr_box grab_orig;   /* for Escape */
	bool grab_orig_maximized;
	enum snap grab_orig_snap;
	enum snap grab_zone;
	struct wlr_box grab_target;
	bool grab_resize_waiting;   /* opaque: not sent, the client is busy */

	uint32_t last_click_msec;
	struct view *last_click_view;
	enum frame_part last_click_part;

	struct menu menu;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;
	struct wl_listener layout_change;

	int cascade;
	long raise_hi, raise_lo; /* for raise_seq */
	bool tiling;
	int desktop;     /* current */
	long tile_front; /* the newest window's tile_seq */
	const char *terminal;
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
	struct wl_listener new_idle_inhibitor;
	struct wl_listener output_power_set_mode;
	struct wlr_output_manager_v1 *output_manager; /* NULL unless outputManagement */
	struct wl_listener output_manager_apply;
	struct wl_listener output_manager_test;
	struct wl_event_source *output_manager_idle; /* an update is due */
	/* output_manager_configure(): one layout change at the end, not per output. */
	bool layout_batch, layout_dirty;
	char *wallpaper; /* a readable PNG, or NULL */
};

struct output {
	struct wl_list link;
	struct server *server;
	struct wlr_output *wlr_output;
	struct wl_list layers[MWC_SHELL_LAYERS]; /* layer_surface.link */
	struct wlr_box usable; /* what panels leave free */
	struct wlr_box open;   /* usable, but with the dock's strip */
	struct wlr_box dock;   /* the dock's box here, empty without one */
	struct throttle arrange; /* see layer_arrange_soon() */
	struct wlr_scene_rect *background;
	struct wlr_scene_buffer *wallpaper; /* NULL: none */
	int wallpaper_w, wallpaper_h;       /* rendered for */
	/* Set by a wlr-output-management client; kept while mwmrc still says
	 * managed_rc about this output. NULL: mwmrc rules. */
	char *managed_rc;
	struct wlr_output_head_v1_state managed;
	/* dark: off through wlr-output-power, clients of output management still see it
	 * on. client_off: such a client wants it off; only such a client turns it on. */
	bool dark, client_off;
	struct wlr_box off_box; /* where a client_off output comes back */
	/* Scratch for output_manager_configure(). */
	struct wlr_box box_before;
	bool named;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener commit;
	struct wl_listener destroy;
};

/* ---- view.c ------------------------------------------------------------- */

void xdg_init(struct server *server);
/* In base->data for as long as the tree lives: a surface can outlast its role. */
void xdg_surface_set_tree(struct wlr_xdg_surface *base, struct wlr_scene_tree *tree);
void view_focus(struct view *view);
void view_set_position(struct view *view, int x, int y);
void view_set_geometry(struct view *view, int x, int y, int w, int h);
/* Where the frame goes when the client resizes itself, keeping gravity_edges. */
void view_gravity_place(struct view *view, int ow, int oh, int w, int h,
	int *x, int *y);
void view_close(struct view *view);
void view_lower(struct view *view);
void view_set_maximized(struct view *view, bool maximized);
void view_set_fullscreen(struct view *view, bool fullscreen);
void view_minimize(struct view *view);
void view_restore(struct view *view);
void view_begin_interactive(struct view *view, enum cursor_mode mode,
	uint32_t edges, bool until_click);
/* Free size again under the pointer, the grab point in proportion. */
void view_drag_free(struct view *view);
/* On the output at px, py. SNAP_NONE: the geometry from before. */
void view_snap(struct view *view, enum snap s, double px, double py);
void view_update_title(struct view *view);
void view_frame_changed(struct view *view);
void view_theme_changed(struct view *view);
const char *view_title(struct view *view);
const char *view_app_id(struct view *view);
/* Since the last call. */
bool view_app_id_changed(struct view *view);
struct wlr_surface *view_surface(struct view *view);
void view_cycle(struct server *server, bool backwards);
/* After the xdg or X11 part has set the client. */
void view_init_common(struct view *view);
void view_handle_map(struct view *view);
void view_handle_unmap(struct view *view);
void view_handle_destroy(struct view *view);
void view_send_size(struct view *view, int w, int h);
void view_set_activated(struct view *view, bool activated);
struct view *view_at(struct server *server, double lx, double ly,
	struct wlr_surface **surface, double *sx, double *sy,
	enum frame_part *part, uint32_t *edges, bool *on_icon);
/* apply: back to its floating geometry or state. */
void view_set_tiled(struct view *view, bool on, bool apply);
void view_apply_tile(struct view *view);
void view_focus_next(struct view *except);
void view_focus_top(struct server *server);
struct view *view_focused(struct server *server);
/* Without its dialogs. */
void view_raise_one(struct view *view);
void views_update_layers(struct server *server);
/* The output there, or else the first; NULL when there are none. */
struct output *output_at(struct server *server, double lx, double ly);
/* False once a client switched it off. */
bool output_in_layout(struct output *output);
/* The oldest in the layout; NULL if clients switched all off. */
struct output *output_first(struct server *server);
struct wlr_box output_usable_area(struct server *server, double lx, double ly);
/* Whether b reaches into the dock on o. */
bool output_dock_in_the_way(struct output *o, struct wlr_box b);
struct wlr_box output_full_area(struct server *server, double lx, double ly);
void views_usable_area_changed(struct server *server);
/* Only the dock's box changed on output: snapped windows refit, and free ones it
 * would cover move out of its strip. */
void views_refit_snapped(struct server *server);
void views_clear_dock(struct output *output);
void views_layout_changed(struct server *server);
void views_output_gone(struct output *output);

/* ---- sizehints.c -------------------------------------------------------- */

/* Largest window size; beyond it frame sizes and placement overflow. */
#define VIEW_MAX_SIZE (1 << 20)
#define X11_MAX_SIZE 32767

/* The frame around a client: where the client sits in it, and what it adds. */
struct frame_size {
	int left, top, extra_w, extra_h;
};

void read_xwayland_size_hints(struct view *view);
void read_xdg_size_hints(struct view *view);
bool view_fixed(struct view *view);
bool view_has_aspect(const struct view *view);
void view_aspect_follow(struct view *view, int *pw, int *ph, bool width_given);
void view_apply_size_hints(struct view *view, int *w, int *h, bool incr);
void view_gravity_to_frame(struct view *view, struct frame_size f, int x, int y, int w,
	int h, int *fx, int *fy);

/* ---- hints.c ------------------------------------------------------------ */

/* Type and size limits from the client; decor and funcs from them. */
void view_update_properties(struct view *view);
/* wm_capabilities from funcs, sent on a change. */
void xdg_publish_caps(struct view *view);
void view_update_decorations(struct view *view);
struct view *view_parent(struct view *view);
/* Nothing else follows from it. */
void view_show(struct view *view, bool show);
void view_show_content(struct view *view);
void view_publish_state(struct view *view);
void view_refit(struct view *view);
void view_refit_at(struct view *view, double px, double py);
void view_place(struct view *view);
void view_parent_changed(struct view *view);
bool view_is_transient_of(struct view *view, struct view *ancestor);
struct view *view_transient_root(struct view *view);
bool view_focusable(struct view *view);

/* ---- main.c ------------------------------------------------------------- */

void outputs_config_changed(struct server *server);

/* ---- xwayland.c --------------------------------------------------------- */

void xwayland_init(struct server *server);
/* Sticky and shaded into _NET_WM_STATE. */
void xwayland_publish_state(struct view *view);
void xwayland_publish_frame_extents(struct view *view);
/* Blocks on the X server. wlroots tells only of decoration changes. */
void xwayland_read_motif_hints(struct view *view);

/* ---- frame.c ------------------------------------------------------------ */

int frame_border(struct view *view);
int frame_thin_border(void); /* without resize handles */
int frame_title(struct view *view);
int frame_left(struct view *view);
int frame_top(struct view *view);
int frame_extra_w(struct view *view);
int frame_extra_h(struct view *view);
struct frame_size frame_size(struct view *view);
/* For a window opened at the pointer. */
void frame_title_center(struct view *view, int *x, int *y);
int frame_height(struct view *view);
void frame_render(struct view *view);
/* The top strip only, if the size is the same. */
void frame_render_title(struct view *view);
enum frame_part frame_hit(struct view *view, int fx, int fy, uint32_t *edges);
/* -1: none. */
int frame_tab_at(struct view *view, int fx);
void icon_render(struct view *view);
void icons_arrange(struct server *server);
/* The current desktop's, by icon_seq; NULL with *n = 0 for none. Caller frees. */
struct view **icons_in_order(struct server *server, int *n);
/* toggled: the maximize glyph sunken. */
void draw_title_button(cairo_t *cr, enum frame_part part, int x, int y, int size,
	const struct mwc_shade *s, bool down, bool toggled);
#define ICON_W 72
#define ICON_H 72
#define ICON_MARGIN 8 /* from the usable area's edges */
int icon_gap(void); /* 0 with "tight" */

/* ---- tabs.c ------------------------------------------------------------- */

bool tabs_can_join(struct view *view);
bool tabs_fit_together(struct view *view, struct view *target);
/* What Super+G joins, or NULL. */
struct view *tabs_previous(struct view *view);
struct view *tabs_nth(struct view *view, int n); /* or NULL */
/* view takes target's place, shown, in its group. */
bool tabs_join(struct view *view, struct view *target);
void tabs_detach(struct view *view);
void tabs_show(struct view *view);
/* Also out of an icon or fullscreen. */
void tabs_bring(struct view *view);
void tabs_step(struct view *view, bool forward);
void tabs_show_nth(struct view *view, int n);
/* Unmapping: the next takes over without its help. */
void tabs_view_gone(struct view *view);
void tabs_view_changed(struct view *view);
/* The others take the shown one's size, so a switch needn't wait. */
void tabs_sync_sizes(struct view *view);
void tabs_after_restore(struct view *view);
int tabs_index(struct view *view); /* -1: alone */
/* The size it leaves its group with. */
void tabs_own_size(struct view *view, int *w, int *h);

/* ---- menu.c ------------------------------------------------------------- */

void menu_open_window(struct server *server, struct view *view, int x, int y);
void menu_open_root(struct server *server, int x, int y);
void menu_close(struct server *server);
void menu_motion(struct server *server, double lx, double ly);
/* True when the menu took it. */
bool menu_button(struct server *server, double lx, double ly, bool pressed,
	uint32_t time_msec);
bool menu_key(struct server *server, uint32_t keysym);
void run_action(struct server *server, enum menu_action action,
	struct view *view);

/* ---- input.c ------------------------------------------------------------ */

void input_init(struct server *server);
void input_config_changed(struct server *server);
/* Opaque resize: the size held back while the client was busy goes out. */
void grab_resize_caught_up(struct view *view);
void grab_started(struct server *server);
void grab_forget(struct server *server);
/* Its state changes under a move or resize: the grab ends, the window stays. */
void grab_give_way(struct view *view);
void input_add_device(struct server *server, struct wlr_input_device *dev);
/* Clients type and click as the user: wayrdp, the tests. */
void input_virtual_init(struct server *server);
void input_virtual_fini(void);
void cursor_set_name(struct server *server, const char *name);
void process_cursor_motion(struct server *server, uint32_t time);
/* What is under it changed without motion. */
void input_pointer_changed(struct server *server);
/* The same once wlroots is through: a surface going may still be in the scene. */
void input_pointer_changed_soon(struct server *server);
/* A button down that a client got: it may start a move. */
bool input_button_held(struct server *server);
/* Over it or its subsurfaces. */
bool input_pointer_on(struct server *server, struct wlr_surface *surface);
/* Without the keys mwc kept. */
void input_keyboard_enter(struct server *server, struct wlr_surface *surface);
/* A client's menu holds the keyboard wherever the focus goes. Not during a drag. */
void input_end_client_grab(struct server *server);
void spawn(const char *cmd);
/* No shell, no quoting. */
void spawn_file(const char *path);

/* ---- tile.c ------------------------------------------------------------- */

void tile_init(struct server *server);
void tile_config_changed(struct server *server);
void tile_toggle(struct server *server);
void tile_arrange(struct server *server);
bool tile_can_shade(struct view *view);
bool tile_has_stack(struct server *server);
/* Main window: into the stack. Stack window: roll up or down. */
void tile_iconify(struct view *view);
/* A stack window becomes the main one. */
void tile_promote(struct view *view);
void tile_unshade(struct view *view);
/* 1: tiled on top, 0: floating on top, -1: not both kinds. */
int tile_stacking(struct server *server);
/* All tiled windows above or below all floating ones. */
void tile_stack(struct server *server, bool raise);

/* ---- snap.c ------------------------------------------------------------- */

struct wlr_box snap_rect(enum snap s, struct wlr_box area);
/* What snap_rect() divides for s at (px, py): with the dock's strip beside the dock. */
struct wlr_box snap_area(struct server *server, enum snap s, double px, double py);
const char *snap_name(enum snap s);
enum snap snap_zone_at(struct view *view, double px, double py);
void snap_preview(struct view *view, enum snap s, double px, double py);
void snap_preview_box(struct view *view, struct wlr_box box);
/* point_accepts_input for buffers the pointer goes through. */
bool scene_buffer_no_input(struct wlr_scene_buffer *buffer, double *sx, double *sy);
void snap_preview_hide(void);
bool snap_preview_get(struct wlr_box *box); /* false while hidden */
void snap_step(struct view *view, enum snap_dir dir);
void view_move_to_edge(struct view *view, enum snap_dir dir);

/* ---- outline.c ---------------------------------------------------------- */

bool outline_wanted(void);
void outline_show(struct view *view, struct wlr_box frame_box);
void outline_hide(void);
bool outline_get(struct wlr_box *box); /* false while hidden */
void feedback_show(struct server *server, const char *text, double lx, double ly);
void feedback_hide(void);
/* "" while hidden; draws: how often it was drawn. */
const char *feedback_get(struct wlr_box *box, int *draws);

/* ---- layer.c ------------------------------------------------------------ */

void layer_init(struct server *server);
void layer_arrange(struct output *output);
/* At once, then at most once per refresh. */
void layer_arrange_soon(struct output *output);
void layers_arrange_all_soon(struct server *server);
void layers_arrange_all(struct server *server);
void layer_publish_workarea(struct server *server);
void layer_output_gone(struct output *output);
void layer_handle_click(struct server *server, struct wlr_surface *surface);
bool layer_surface_is_desktop(struct wlr_surface *surface);
bool layer_keyboard_back(struct server *server);
/* Where a top-layer surface has the keyboard: no fullscreen window covers it there. */
struct output *layer_keyboard_output(struct server *server);
/* Raises that surface above the windows sharing the top layer with it. */
void layer_raise_keyboard(struct server *server);
/* Of the layer surface this popup parent belongs to. */
bool layer_root_coords(struct wlr_surface *surface, int *lx, int *ly);

/* ---- wallpaper.c -------------------------------------------------------- */

/* MWC_WALLPAPER or Mwm*wallpaper. */
void wallpaper_init(struct server *server);
void wallpaper_fini(struct server *server);
/* force: also at the same size. */
void wallpaper_place(struct output *output, bool force);

/* ---- outputrc.c --------------------------------------------------------- */

/* Each false for text mwmrc must not have; the out values are left alone then. */
bool outputrc_mode(const char *value, int *w, int *h, double *hz);
/* hz 0: the highest refresh rate. */
struct wlr_output_mode *outputrc_pick_mode(struct wl_list *modes, int w, int h, double hz);
bool outputrc_scale(const char *value, double *scale);
bool outputrc_transform(const char *value, enum wl_output_transform *transform);
bool outputrc_position(const char *value, int *x, int *y);

/* ---- config.c ----------------------------------------------------------- */

void config_load(void);
const char *config_get(const char *resource);
const char *terminal_command(struct server *server);
/* Mwm*client*resource, or NULL. */
const char *config_get_for(const char *client, const char *resource);
/* Mwm*rootMenuItem, in file order. */
#define CONFIG_MAX_ROOT_ITEMS 20
int config_root_items(void);
void config_root_item(int i, const char **label, const char **command);
bool config_color(const char *value, struct mwc_color *out);
/* 1, 0, or -1 for unset or not a boolean; name for the log. */
int config_bool(const char *name, const char *value);
void config_view_shades(struct view *view);
/* DoNotTile, TileOnly. */
void config_view_tiling(struct view *view);
bool config_view_all_desktops(struct view *view);
/* Mwm*NAME*geometry. at_pointer: the title bar's middle at the pointer. */
struct config_position {
	int x, y;
	bool from_right, from_bottom;
	bool at_pointer;
};
bool config_view_position(struct view *view, struct config_position *out);
/* A leading ~/ as $HOME. */
void config_expand_home(const char *value, char *out, size_t size);
/* Mwm*iconPlacement: "primary secondary [tight]", default "left bottom". */
struct config_placement {
	bool horizontal;
	bool from_right, from_bottom;
	bool center; /* one block in the middle of the edge */
	bool tight;
};
const struct config_placement *config_icon_placement(void);
/* Mwm*dockPlacement, iconPlacement's syntax; default "right bottom". */
const struct config_placement *config_dock_placement(void);
/* Its dock line's place in mwmrc, the dock's order; -1: not docked. */
int config_view_dock(struct view *view);
/* Reload and SIGHUP: mwmrc applied to what runs; no re-exec. */
void config_reload(struct server *server);

/* ---- testhook.c --------------------------------------------------------- */

void testhook_init(struct server *server);

/* ---- desktop.c ------------------------------------------------------------ */

bool view_on_all_desktops(struct view *view);
void desktop_view_mapped(struct view *view);
/* 0-based. */
void desktop_switch(struct server *server, int n);
/* With its dialogs and tab group. */
void view_set_desktop(struct view *view, int n);
/* It asked for the focus on another desktop. */
void desktop_attention(struct view *view);
/* Its type or parent changed. */
void desktop_recheck(struct view *view);
bool desktop_urgent(struct server *server, int n);
void view_toggle_sticky(struct view *view);
/* By type or mwmrc: the menu can't change it. */
bool view_sticky_by_rule(struct view *view);
void desktop_config_changed(struct server *server);

/* ---- workspace.c ---------------------------------------------------------- */

/* ext-workspace-v1. */
void workspace_init(struct server *server);
void workspace_update(struct server *server);
struct wlr_output;
void workspace_output_new(struct wlr_output *output);
/* Before its wl_output objects go inert. */
void workspace_output_leaving(struct wlr_output *output);

/* ---- iconbox.c ------------------------------------------------------------ */

bool iconbox_enabled(void); /* Mwm*useIconBox */
void iconbox_init(struct server *server);
/* For icons_arrange(). */
void iconbox_arrange(struct server *server);
/* Chosen with the arrows, and the box has the keyboard. */
bool iconbox_selected(struct view *view);
bool iconbox_has_focus(void);
void iconbox_focus(struct server *server);
void iconbox_blur(void);
/* Before the desktop changes. */
void iconbox_desktop_changed(void);
/* After theme_init(). */
void iconbox_config_changed(struct server *server);
/* Alt+Tab would go to next: whether the box comes first. */
bool iconbox_cycle_takes(struct view *next, bool backwards);
void iconbox_lowered(struct server *server);
bool iconbox_key(struct server *server, uint32_t mods, uint32_t sym);
bool iconbox_contains(double lx, double ly);
bool iconbox_title_at(double lx, double ly);
/* double_click: on its title bar. */
bool iconbox_press(struct server *server, uint32_t button, double lx, double ly,
	bool double_click);
/* CURSOR_BUTTON with no grabbed view. */
void iconbox_button_motion(double lx, double ly);
void iconbox_button_release(struct server *server);
bool iconbox_scrollable_at(double lx, double ly);
/* Whole icons. */
void iconbox_scroll(int steps);
void iconbox_icon_pressed(struct view *view);
/* The box or its logo; false: none. */
bool iconbox_rect(struct wlr_box *out);
void iconbox_state(FILE *f);

/* ---- dock.c -------------------------------------------------------------- */

/* Between the normal windows' layer and the top layer. */
void dock_init(struct server *server);
/* When a docked window comes or goes, or the outputs change. */
void dock_arrange(struct server *server);
/* From layer_arrange(): takes the dock's strip off the usable area. */
void dock_reserve(struct output *output, struct wlr_box *usable);
/* The dock's box on output, empty if it is elsewhere or empty. */
struct wlr_box dock_box(struct output *output);
/* A row along the top or bottom edge, not a column along a side. */
bool dock_horizontal(void);
/* Takes what docked windows can't have: a state, a tab group, an icon. */
void dock_admit(struct view *view);
/* Its size or frame changed. */
void dock_view_resized(struct view *view);
/* Its name, type or parent changed: into the dock or out of it. */
void dock_view_changed(struct view *view);
/* After a reload has set view.dock_order anew. */
void dock_config_changed(struct server *server);
/* Forgets the output; the next dock_arrange() picks one. */
void dock_output_gone(struct output *output);
bool dock_contains(double lx, double ly);
void dock_state(FILE *f);

/* ---- fifo.c --------------------------------------------------------------- */

/* wp_fifo_v1, which SDL 3 needs for Wayland. */
void fifo_init(struct server *server);

/* ---- shmalloc.c ----------------------------------------------------------- */

/* Buffers in memfds, not in /dev/shm, which may be too small for them. */
struct wlr_allocator *shm_allocator_create(void);

#endif
