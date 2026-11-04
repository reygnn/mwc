/*
 * mwc - a Motif-style Wayland compositor
 *
 * Shared types. The compositor follows the structure of wlroots' tinywl:
 * one global server struct, listeners embedded in the objects they belong
 * to, and the wlr_scene graph doing all the actual rendering.
 */
#ifndef MWC_H
#define MWC_H

#include <stdbool.h>
#include <stdint.h>
#include <cairo.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>

struct wlr_input_device;
struct wlr_layer_surface_v1;
struct wlr_surface;
struct wlr_xdg_surface;
struct wlr_xdg_toplevel;
struct wlr_xwayland_surface;

/* background, bottom, top, overlay: the layers of wlr-layer-shell. */
#define MWC_SHELL_LAYERS 4

/* ---- theme ------------------------------------------------------------ */

struct mwc_color {
	float r, g, b;
};

/* A Motif "3D colour set": background plus the two derived shadow colours. */
struct mwc_shade {
	struct mwc_color bg, top, bottom, fg;
};

struct mwc_theme {
	struct mwc_shade active, inactive, menu;
	struct mwc_color select; /* menu item armed background */
	struct mwc_color root;   /* desktop background */
	bool root_none;          /* draw no desktop colour, a tool does that */
	/* Global title text colours from mwmrc, for clients with their own
	 * background. Unset, the text is black or white by contrast. */
	struct mwc_color active_fg, inactive_fg;
	bool has_active_fg, has_inactive_fg;
	char *font;              /* pango font description */
	int border;              /* resize border width */
	int title_height;        /* title bar height */
	int menu_item_height;
	int bevel;               /* shadow thickness */
};

extern struct mwc_theme theme;

/* Reads the configuration, so config_load() goes first. */
void theme_init(void);
struct mwc_shade shade_from(struct mwc_color bg, struct mwc_color fg);
struct mwc_color color_mix(struct mwc_color a, struct mwc_color b, float f);
struct mwc_color color_contrast(struct mwc_color bg);
void set_source(cairo_t *cr, struct mwc_color c);
/* Raised (in == false) or sunken (in == true) Motif bevel. */
void draw_bevel(cairo_t *cr, double x, double y, double w, double h,
	int thickness, const struct mwc_shade *s, bool in);
/* Etched-in groove line, as used for separators and resize corners. */
void draw_groove_h(cairo_t *cr, double x, double y, double w,
	const struct mwc_shade *s);
void draw_groove_v(cairo_t *cr, double x, double y, double h,
	const struct mwc_shade *s);
void draw_text(cairo_t *cr, const char *text, const char *font,
	struct mwc_color color, double x, double y, double w, double h,
	bool center);

/* ---- cairo-backed wlr_buffer --------------------------------------------- */

struct cairo_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
	cairo_t *cr;
};

struct cairo_buffer *cairo_buffer_create(int width, int height);
/* Drops our reference; the scene keeps the buffer alive while shown. */
void cairo_buffer_drop(struct cairo_buffer *buf);

/* ---- views -------------------------------------------------------------- */

enum view_type {
	VIEW_XDG,
	VIEW_XWAYLAND,
};

/* What a window gets, as in Motif's clientDecoration/clientFunctions. The bits
 * are those of _MOTIF_WM_HINTS. */
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

/* Snap positions. SNAP_MAXIMIZE is only a target: a window maximized that way
 * has SNAP_NONE and view.maximized. */
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

/* Frame regions, used for hit testing, cursor shapes and button state. */
enum frame_part {
	PART_NONE = 0,
	PART_CLIENT,
	PART_TITLE,
	PART_TAB,   /* the title of a group; see frame_tab_at() */
	PART_MENU_BUTTON,
	PART_MINIMIZE_BUTTON,
	PART_MAXIMIZE_BUTTON,
	PART_RESIZE, /* see resize edges */
};

struct tab_group {
	struct wl_list members; /* view.tab_link, in tab order */
	struct view *shown;
	bool busy;              /* switching: members are in between */
	int first_visible;      /* with too many for the title bar, see frame.c */
};

struct view {
	struct wl_list link; /* server.views, front to back */
	struct server *server;
	enum view_type type;
	union {
		struct wlr_xdg_toplevel *xdg_toplevel;
		struct wlr_xwayland_surface *xsurface;
	};

	struct wlr_scene_tree *tree;         /* whole window incl. frame */
	struct wlr_scene_tree *content_tree; /* the client's surfaces */
	struct wlr_scene_buffer *frame_node;
	struct wlr_scene_buffer *icon_node;  /* shown while minimized */

	/* Frame geometry: x/y is the outer top-left corner in layout coords,
	 * width/height the client size. */
	int x, y, width, height;
	bool mapped;
	bool placed;    /* position chosen by the client, skip cascading */
	bool ssd;       /* client accepted server-side decorations */
	bool decorated; /* frame currently drawn: ssd && !fullscreen */
	enum window_type wtype;
	unsigned decor, funcs; /* see view_update_properties() */
	unsigned base_funcs;   /* the same, before tiling takes some away */
	uint32_t motif_flags, motif_funcs, motif_decor; /* X11 only */
	/* Size limits of the client: 0 means none. */
	int min_w, min_h, max_w, max_h, base_w, base_h, inc_w, inc_h;
	float min_aspect, max_aspect;
	int gravity;
	bool hidden_with_parent; /* a dialog gone with its minimized window */
	struct tab_group *group; /* NULL when alone; see tabs.c */
	struct wl_list tab_link; /* tab_group.members */
	bool tab_hidden;         /* in a group, and not the one shown */
	void *deco;     /* xdg-decoration object, if any */
	bool activated;
	bool maximized;
	bool minimized;
	bool fullscreen;
	enum snap snap;
	/* Tiling, see tile.c. */
	bool tiled;
	bool tile_main;  /* the main window on the right */
	bool shaded;     /* in the stack, rolled up to its title bar */
	bool auto_shaded; /* by the layout for lack of room, not by the user */
	bool tile_back;  /* fullscreen from the layout: back into it after */
	bool tile_nofit; /* this round: too big for its cell, floats */
	bool no_tile;    /* DoNotTile or not in TileOnly: always floats */
	long tile_seq;   /* layout order, lowest first (the main window) */
	struct wlr_box tile; /* its cell: the frame's box */
	struct mwc_shade shade_active, shade_inactive; /* frame and icon colours */
	struct wlr_box saved;  /* geometry before maximize/snap/fullscreen */
	double fit_px, fit_py; /* a point on the output its state fills */
	/* xdg clients answer a size request later, possibly after the button
	 * was released: these edges stay put until the client has caught up. */
	uint32_t anchor_edges;
	int anchor_right, anchor_bottom;
	uint32_t anchor_serial; /* last configure sent with a new size */
	int requested_w, requested_h; /* xdg: the client's size once it has caught up */
	int icon_x, icon_y;
	bool icon_placed;

	enum frame_part pressed; /* button currently held down, for drawing */
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
	/* xwayland only */
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener request_configure;
	struct wl_listener request_activate;
	struct wl_listener set_decorations;
	struct wl_listener focus_in;
	bool been_mapped;
	int unmap_x, unmap_y; /* the X window's place when it was withdrawn */
	struct wl_listener set_parent;
	struct wl_listener set_window_type;
	struct wl_listener set_strut_partial;
	struct wl_listener request_above;
	struct wl_listener request_below;
	struct wl_listener set_override_redirect;
};

/* Not on the screen right now, whatever the reason. */
static inline bool view_hidden(const struct view *view)
{
	return view->minimized || view->hidden_with_parent || view->tab_hidden;
}

/* Unmanaged XWayland windows: override-redirect menus, tooltips, DnD icons. */
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
	ACT_RUN,      /* the item's command */
	ACT_RESTART,
	ACT_QUIT,
};

struct menu_item {
	const char *label;     /* NULL means separator */
	char mnemonic;         /* underlined letter, 0 for none */
	const char *accel;     /* right-aligned accelerator text */
	enum menu_action action;
	bool sensitive;
	const char *command;   /* for ACT_RUN, owned by config.c */
};

#define MENU_MAX_ITEMS 32

struct menu {
	bool open;
	const char *title;
	struct menu_item items[MENU_MAX_ITEMS];
	int n_items;
	int armed;              /* highlighted item, -1 for none */
	struct view *target;    /* window menu: the window it acts on */
	struct wlr_scene_buffer *node;
	int x, y, width, height;
	int item_y[MENU_MAX_ITEMS], item_h[MENU_MAX_ITEMS];
	uint32_t opened_msec;
	bool from_button;      /* opened by a click on the frame's menu button */
	double open_x, open_y; /* pointer position when it opened */
};

/* ---- server ------------------------------------------------------------- */

enum cursor_mode {
	CURSOR_PASSTHROUGH,
	CURSOR_MOVE,
	CURSOR_RESIZE,
	CURSOR_BUTTON, /* a frame button is held down */
	CURSOR_MENU,   /* a menu has the pointer grab */
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

	/* Stacking layers, bottom to top. */
	struct wlr_scene_tree *layer_bg;
	struct wlr_scene_tree *layer_shell[MWC_SHELL_LAYERS]; /* see main() */
	struct wlr_scene_tree *layer_icons;
	struct wlr_scene_tree *layer_below; /* _NET_WM_STATE_BELOW */
	struct wlr_scene_tree *layer_views;
	struct wlr_scene_tree *layer_fullscreen; /* the focused fullscreen window */
	struct wlr_scene_tree *layer_unmanaged;
	struct wlr_scene_tree *layer_menu;
	struct wlr_scene_tree *layer_feedback; /* outline and move/size box */
	struct wlr_scene_tree *layer_drag; /* drag icons, follows the pointer */

	struct wl_listener new_layer_surface;
	struct wlr_layer_surface_v1 *focused_layer;   /* has the keyboard */
	struct wlr_layer_surface_v1 *exclusive_layer; /* keeps the keyboard */

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
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;
	struct wl_listener keyboard_grab_end;
	bool refocus_after_grab; /* a focus change the keyboard grab held back */
	struct wl_event_source *pointer_refresh; /* pending, see input_pointer_changed_soon() */
	/* Buttons a client has seen go down and not yet up. While there are any, the
	 * pointer stays with that client: its implicit grab. */
	uint32_t forwarded_buttons[16];
	int n_forwarded;
	double grab_surface_x, grab_surface_y; /* the surface's origin in the layout */
	struct wl_list keyboards;

	enum cursor_mode cursor_mode;
	struct view *grabbed_view;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;
	bool grab_until_click; /* move/size started from menu or keyboard */
	uint32_t grab_button;  /* the press a drag or frame button goes by */
	int grab_tab;          /* the tab pressed: shown if this doesn't become a drag */
	struct view *grab_take; /* a drag taking only this window along, see input.c */
	struct view *grab_drop; /* the window it would join, for the highlight */
	int tab_wheel;         /* the wheel over tabs, in fractions of a step */
	enum frame_part grab_part;
	bool grab_outline;  /* wireframe: the window changes when the grab ends */
	bool grab_detached; /* a move got going, past the drag threshold */
	bool grab_changed;  /* the target differs from where the grab began */
	bool grab_detaching; /* the grab itself is freeing the window right now */
	double grab_px, grab_py;    /* pointer where the grab began */
	struct wlr_box grab_orig;   /* x/y of the frame, client size: for Escape */
	bool grab_orig_maximized;
	enum snap grab_orig_snap;
	enum snap grab_zone;        /* snap target under the pointer */
	struct wlr_box grab_target; /* same, where the window is to go */

	/* Double-click detection */
	uint32_t last_click_msec;
	struct view *last_click_view;
	enum frame_part last_click_part;

	struct menu menu;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;
	struct wl_listener layout_change;

	int cascade;
	bool tiling;     /* tiling mode on */
	long tile_front; /* tile_seq of the newest window, which leads */
	const char *terminal;
	char *wallpaper; /* path of a readable PNG, NULL: plain desktop colour */
};

struct output {
	struct wl_list link;
	struct server *server;
	struct wlr_output *wlr_output;
	struct wl_list layers[MWC_SHELL_LAYERS]; /* layer_surface.link */
	struct wlr_box usable; /* what panels leave free */
	struct wlr_scene_rect *background;
	struct wlr_scene_buffer *wallpaper; /* above background, may be NULL */
	int wallpaper_w, wallpaper_h;       /* size it was rendered for */
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

/* ---- view.c ------------------------------------------------------------- */

void xdg_init(struct server *server);
/* Remembers the scene tree of an xdg surface in its data, for popups to find, for
 * exactly as long as the tree lives: a surface can outlast its role. */
void xdg_surface_set_tree(struct wlr_xdg_surface *base, struct wlr_scene_tree *tree);
void view_focus(struct view *view);
void view_set_position(struct view *view, int x, int y);
void view_set_geometry(struct view *view, int x, int y, int w, int h);
void view_close(struct view *view);
void view_lower(struct view *view);
void view_set_maximized(struct view *view, bool maximized);
void view_set_fullscreen(struct view *view, bool fullscreen);
void view_minimize(struct view *view);
void view_restore(struct view *view);
void view_begin_interactive(struct view *view, enum cursor_mode mode,
	uint32_t edges, bool until_click);
/* Back to its free size under the pointer, keeping the grab point proportional. */
void view_drag_free(struct view *view);
/* Snap to a position on the output at (px, py). SNAP_NONE goes back to the
 * geometry from before. */
void view_snap(struct view *view, enum snap s, double px, double py);
void view_update_title(struct view *view);
/* The frame's size changed: move the client and redraw. */
void view_frame_changed(struct view *view);
/* Colours or frame size changed: pick up the new theme and redraw. */
void view_theme_changed(struct view *view);
const char *view_title(struct view *view);
const char *view_app_id(struct view *view);
struct wlr_surface *view_surface(struct view *view);
void view_cycle(struct server *server, bool backwards);
/* Common setup after the type-specific part has filled in the client. */
void view_init_common(struct view *view);
void view_handle_map(struct view *view);
void view_handle_unmap(struct view *view);
void view_handle_destroy(struct view *view);
void view_send_size(struct view *view, int w, int h);
void view_set_activated(struct view *view, bool activated);
struct view *view_at(struct server *server, double lx, double ly,
	struct wlr_surface **surface, double *sx, double *sy,
	enum frame_part *part, uint32_t *edges, bool *on_icon);
/* The output there, or else the first one; NULL when there are none. */
/* Into the layout or out of it, back to its floating geometry or state if apply. */
void view_set_tiled(struct view *view, bool on, bool apply);
/* Into its cell, as far as the client's limits allow. */
void view_apply_tile(struct view *view);
void view_focus_next(struct view *except);
/* To the front of its scene layer, alone. */
void view_raise_one(struct view *view);
/* Puts every window into its scene layer, by type, state and focus. */
void views_update_layers(struct server *server);
struct output *output_at(struct server *server, double lx, double ly);
struct wlr_box output_usable_area(struct server *server, double lx, double ly);
struct wlr_box output_full_area(struct server *server, double lx, double ly);
/* Maximized windows and icons follow when panels come and go. */
void views_usable_area_changed(struct server *server);
/* Outputs came, went or changed: nothing may stay out of reach. */
void views_layout_changed(struct server *server);

/* ---- hints.c ------------------------------------------------------------ */

/* Reads type and size limits from the client, derives decor and funcs. */
void view_update_properties(struct view *view);
void view_update_decorations(struct view *view);
bool view_fixed(struct view *view);
void view_apply_size_hints(struct view *view, int *w, int *h, bool incr);
/* The frame's position when the client asks for x, y at the size w, h. */
void view_gravity_to_frame(struct view *view, int x, int y, int w, int h, int *fx,
	int *fy);
struct view *view_parent(struct view *view);
/* Hidden or shown, without any other consequence. */
void view_show(struct view *view, bool show);
void view_show_content(struct view *view);
/* Tells the client what it is: maximized, tiled, snapped. */
void view_publish_state(struct view *view);
void view_refit(struct view *view);
void view_place(struct view *view);
void view_parent_changed(struct view *view);
bool view_is_transient_of(struct view *view, struct view *ancestor);
struct view *view_transient_root(struct view *view);
bool view_focusable(struct view *view);

/* ---- main.c ------------------------------------------------------------- */

/* The output settings in mwmrc changed: apply them to what is there. */
void outputs_config_changed(struct server *server);

/* ---- xwayland.c --------------------------------------------------------- */

void xwayland_init(struct server *server);
/* _NET_FRAME_EXTENTS, for an X11 window whose frame changed. */
void xwayland_publish_frame_extents(struct view *view);
/* _MOTIF_WM_HINTS once more. Blocks on the X server. wlroots only tells of changes to
 * the decoration part, so this is for when the functions are about to matter. */
void xwayland_read_motif_hints(struct view *view);

/* ---- frame.c ------------------------------------------------------------ */

int frame_border(struct view *view);
int frame_title(struct view *view);
int frame_left(struct view *view);
int frame_top(struct view *view);
int frame_extra_w(struct view *view);
int frame_extra_h(struct view *view);
/* Outer frame height; a shaded window keeps only its title bar. */
int frame_height(struct view *view);
void frame_render(struct view *view);
enum frame_part frame_hit(struct view *view, int fx, int fy, uint32_t *edges);
/* Which tab is at x in the frame: its index, or -1 for none. */
int frame_tab_at(struct view *view, int fx);
void icon_render(struct view *view);
void icons_arrange(struct server *server);
#define ICON_W 72
#define ICON_H 72

/* ---- tabs.c ------------------------------------------------------------- */

bool tabs_can_join(struct view *view);
/* Whether one can join the other: not itself, not its own group, and they agree. */
bool tabs_fit_together(struct view *view, struct view *target);
/* The window Super+G would join, or NULL. */
struct view *tabs_previous(struct view *view);
struct view *tabs_nth(struct view *view, int n); /* NULL when there is none */
/* The view takes the target's place, and is shown there, in its group. */
bool tabs_join(struct view *view, struct view *target);
/* Out of its group, as a window of its own. */
void tabs_detach(struct view *view);
void tabs_show(struct view *view);
/* Shows it whatever the group is doing: an icon or fullscreen comes back first. */
void tabs_bring(struct view *view);
void tabs_step(struct view *view, bool forward);
void tabs_show_nth(struct view *view, int n);
/* Being unmapped: the next one takes over without its help. */
void tabs_view_gone(struct view *view);
/* Its properties changed: it may not qualify any more. */
void tabs_view_changed(struct view *view);
/* The shown view has a new size: the others follow, so a switch needn't wait. */
void tabs_sync_sizes(struct view *view);
void tabs_after_restore(struct view *view);
int tabs_index(struct view *view); /* -1 when alone */

/* ---- menu.c ------------------------------------------------------------- */

void menu_open_window(struct server *server, struct view *view, int x, int y);
void menu_open_root(struct server *server, int x, int y);
void menu_close(struct server *server);
void menu_motion(struct server *server, double lx, double ly);
/* Returns true when the event was consumed by the menu. */
bool menu_button(struct server *server, double lx, double ly, bool pressed,
	uint32_t time_msec);
bool menu_key(struct server *server, uint32_t keysym);
void run_action(struct server *server, enum menu_action action,
	struct view *view);

/* ---- input.c ------------------------------------------------------------ */

void input_init(struct server *server);
/* The keyboard settings in mwmrc changed: new keymaps for real keyboards. */
void input_config_changed(struct server *server);
/* After view_begin_interactive() set the grab up: show what it needs. */
void grab_started(struct server *server);
/* The grabbed window went away. */
void grab_forget(struct server *server);
/* The window's state changes under a grab, which has to give way: it ends, and the
 * window stays as it is now. Does nothing unless this window is being moved or
 * resized. */
void grab_give_way(struct view *view);
void input_add_device(struct server *server, struct wlr_input_device *dev);
void cursor_set_name(struct server *server, const char *name);
void process_cursor_motion(struct server *server, uint32_t time);
/* What is under the pointer may have changed without it moving. */
void input_pointer_changed(struct server *server);
/* The same once wlroots is through with what it is doing, for a surface on its way out:
 * until then the scene may still find it under the pointer. */
void input_pointer_changed_soon(struct server *server);
/* Whether a client may start a move now: only with a button down that it got. */
bool input_button_held(struct server *server);
/* Gives the keyboard to a surface, without the keys mwc kept to itself. */
void input_keyboard_enter(struct server *server, struct wlr_surface *surface);
void spawn(const char *cmd);
/* The same for a program by its path, which needs no quoting. */
void spawn_file(const char *path);

/* ---- tile.c ------------------------------------------------------------- */

void tile_init(struct server *server);
void tile_config_changed(struct server *server);
void tile_toggle(struct server *server);
void tile_arrange(struct server *server);
bool tile_can_shade(struct view *view);
bool tile_has_stack(struct server *server);
/* Minimize in the layout: the main window goes into the stack, its place taken by
 * the first stack window; a stack window shades or unshades. */
void tile_iconify(struct view *view);
/* Maximize in the layout: a stack window trades places with the main window. */
void tile_promote(struct view *view);
void tile_unshade(struct view *view);
/* 1 when the topmost visible window of the normal layer belongs to the layout, 0
 * when it floats; -1 when there are not both kinds. */
int tile_stacking(struct server *server);
/* All tiled windows (with their dialogs) above or below all floating ones. */
void tile_stack(struct server *server, bool raise);

/* ---- snap.c ------------------------------------------------------------- */

struct wlr_box snap_rect(enum snap s, struct wlr_box area);
const char *snap_name(enum snap s);
enum snap snap_zone_at(struct view *view, double px, double py);
void snap_preview(struct view *view, enum snap s, double px, double py);
/* The same look around any box. */
void snap_preview_box(struct view *view, struct wlr_box box);
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
const char *feedback_get(struct wlr_box *box); /* "" while hidden */

/* ---- layer.c ------------------------------------------------------------ */

void layer_init(struct server *server);
void layer_arrange(struct output *output);
void layers_arrange_all(struct server *server);
void layer_publish_workarea(struct server *server);
void layer_output_gone(struct output *output);
void layer_handle_click(struct server *server, struct wlr_surface *surface);
bool layer_surface_is_desktop(struct wlr_surface *surface);
/* Back to the layer surface that holds the keyboard, if one does. */
bool layer_keyboard_back(struct server *server);
/* The position of the layer surface at the root of this popup parent. */
bool layer_root_coords(struct wlr_surface *surface, int *lx, int *ly);

/* ---- wallpaper.c -------------------------------------------------------- */

/* (Re)loads the image named by MWC_WALLPAPER or Mwm*wallpaper. */
void wallpaper_init(struct server *server);
void wallpaper_fini(struct server *server);
/* Fits it to the output's current box; force redraws an unchanged size. */
void wallpaper_place(struct output *output, bool force);

/* ---- config.c ----------------------------------------------------------- */

void config_load(void);
const char *config_get(const char *resource);
/* The value of Mwm*client*resource, NULL when unset. */
const char *config_get_for(const char *client, const char *resource);
/* The programs of the root menu, Mwm*rootMenuItem, in the order of the file. */
#define CONFIG_MAX_ROOT_ITEMS 20
int config_root_items(void);
void config_root_item(int i, const char **label, const char **command);
bool config_color(const char *value, struct mwc_color *out);
/* 1 or 0, or -1 when value is unset or no boolean; name is for the log. */
int config_bool(const char *name, const char *value);
void config_view_shades(struct view *view);
/* Whether the tiling layout may take it, from DoNotTile or TileOnly. */
void config_view_tiling(struct view *view);
/* "Restart..." and SIGHUP: a Wayland compositor can't re-exec and keep its
 * clients, so this reads mwmrc again and applies it to what is running. */
void config_reload(struct server *server);

/* ---- testhook.c --------------------------------------------------------- */

void testhook_init(struct server *server);
void testhook_fini(void);

#endif
