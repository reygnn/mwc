/*
 * ~/.config/mwc/mwmrc: Motif-style resource lines for colours, font and
 * wallpaper. Same syntax as mwm-x11's file, so one can be copied to the other.
 *
 *   Mwm*activeBackground:          #7387ad    focused frames
 *   Mwm*background:                #aeb2c3    other frames, menus, icons
 *   Mwm*activeForeground / foreground         title text (default: auto)
 *   Mwm*fontList:                  Sans Bold 10
 *   Mwm*rootBackground:            #5b7792
 *   Mwm*wallpaper:                 ~/pictures/desk.png
 *   Mwm*rootMenuItem:              Files: mfm   a program in the root menu, repeatable
 *   Mwm*moveOpaque:                true       drag windows, not an outline
 *   Mwm*keyboardLayout:            de         also keyboardVariant, keyboardOptions,
 *                                             keyboardModel
 *   Mwm*DP-1*outputMode:           3440x1440@144
 *   Mwm*DP-1*outputScale:          1.5        also outputTransform, outputPosition
 *   Mwm*tiling:                    true       start in tiling mode
 *   Mwm*tileMainWidth:             60         the main window's share, in %
 *   Mwm*DoNotTile:                 Navigator  Firefox floats in tiling mode
 *   Mwm*TileOnly:                  XTerm, mfm only these tile
 *   Mwm*XTerm*activeBackground:    #8a0303    per client
 *   Mwm*foot*activeBackground:     gold
 *   Mwm*pulseControl*geometry:     -0-0       where its windows appear, per client
 *
 * The client part matches an X11 window's WM_CLASS instance or class name, or
 * a Wayland client's app_id, ignoring case. Colours are #rrggbb, colour names
 * or rgb:rr/gg/bb. Lines starting with ! are comments; there are no comments
 * after a value. Later lines win.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pango/pango.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include "mwc.h"

/* An unfocused client with its own colour shows this much of the global
 * inactive colour mixed in, so it reads as a paler version. */
#define PALE 0.55f

struct rule {
	char *client; /* NULL for global lines */
	char *resource;
	char *value;
	int line;
};

static struct rule *rules;
static int n_rules;
static char path[4096];

/* DoNotTile or TileOnly, whichever comes last, with its programs. */
static enum { FILTER_NONE, FILTER_DENY, FILTER_ONLY } tile_filter;
static char **tile_names;
static int n_tile_names;

static struct {
	char *label, *command;
} root_items[CONFIG_MAX_ROOT_ITEMS];
static int n_root_items;

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

static void add_rule(char *key, const char *value, int line)
{
	/* "Mwm*XTerm*activeBackground": '.' and '*' both separate. The first part is
	 * the prefix and the last the resource; all between is the client, which may
	 * have dots of its own, as "org.gnome.Terminal" does. */
	size_t prefix = strcspn(key, "*.");
	char *resource = key + strlen(key);
	while (resource > key && resource[-1] != '*' && resource[-1] != '.')
		resource--;
	bool ok = key[prefix] && prefix == 3 && !strncasecmp(key, "mwm", 3) && *resource;
	/* Empty parts, as in "Mwm**resource", name nothing. */
	char *client = NULL;
	if (ok && resource - key > (ptrdiff_t)prefix + 1) {
		size_t len = resource - key - prefix - 2;
		ok = len > 0;
		if (ok)
			client = strndup(key + prefix + 1, len);
	}
	if (!ok) {
		wlr_log(WLR_ERROR, "%s:%d: expected Mwm*resource or "
			"Mwm*client*resource", path, line);
		return;
	}
	rules = realloc(rules, sizeof(*rules) * (n_rules + 1));
	rules[n_rules++] = (struct rule){
		.client = client,
		.resource = strdup(resource),
		.value = strdup(value),
		.line = line,
	};
}

/* The last global DoNotTile or TileOnly line decides, and an empty one switches the
 * filter off again. Programs are separated by commas; WM_CLASS names may contain
 * spaces. */
static void load_tile_filter(void)
{
	for (int i = 0; i < n_tile_names; i++)
		free(tile_names[i]);
	free(tile_names);
	tile_names = NULL;
	n_tile_names = 0;
	tile_filter = FILTER_NONE;

	const struct rule *r = NULL;
	for (int i = 0; i < n_rules; i++) {
		if (!rules[i].client && (!strcasecmp(rules[i].resource, "DoNotTile") ||
				!strcasecmp(rules[i].resource, "TileOnly")))
			r = &rules[i];
	}
	if (!r)
		return;
	char *list = strdup(r->value), *save = NULL;
	for (char *tok = strtok_r(list, ",", &save); tok;
			tok = strtok_r(NULL, ",", &save)) {
		char *name = trim(tok);
		if (!*name)
			continue;
		tile_names = realloc(tile_names, sizeof(*tile_names) * (n_tile_names + 1));
		tile_names[n_tile_names++] = strdup(name);
	}
	free(list);
	if (n_tile_names)
		tile_filter = strcasecmp(r->resource, "TileOnly") ? FILTER_DENY :
			FILTER_ONLY;
}

/* Unlike other resources, every line counts. "Label: command", split at the first colon,
 * as a command may well have colons of its own. The menu is closed before these go away. */
static void load_root_items(void)
{
	for (int i = 0; i < n_root_items; i++) {
		free(root_items[i].label);
		free(root_items[i].command);
	}
	n_root_items = 0;
	for (int i = 0; i < n_rules; i++) {
		if (rules[i].client || strcasecmp(rules[i].resource, "rootMenuItem"))
			continue;
		char *value = strdup(rules[i].value);
		char *colon = strchr(value, ':');
		char *label = value, *command = colon ? colon + 1 : NULL;
		if (colon)
			*colon = '\0';
		label = trim(label);
		command = command ? trim(command) : NULL;
		if (!*label || !command || !*command) {
			wlr_log(WLR_ERROR, "%s:%d: expected Mwm*rootMenuItem: label: command",
				path, rules[i].line);
		} else if (n_root_items == CONFIG_MAX_ROOT_ITEMS) {
			wlr_log(WLR_ERROR, "%s:%d: only the first %d of Mwm*rootMenuItem are used",
				path, rules[i].line, CONFIG_MAX_ROOT_ITEMS);
		} else {
			root_items[n_root_items].label = strdup(label);
			root_items[n_root_items++].command = strdup(command);
		}
		free(value);
	}
}

int config_root_items(void)
{
	return n_root_items;
}

void config_root_item(int i, const char **label, const char **command)
{
	*label = root_items[i].label;
	*command = root_items[i].command;
}

/* "+X+Y", with - for a distance from the right or bottom edge, as in X11's -geometry.
 * No size: a program knows its own, and a Wayland client may overrule one anyway. */
static bool parse_position(const char *s, struct config_position *out)
{
	int *value[2] = { &out->x, &out->y };
	bool *from_end[2] = { &out->from_right, &out->from_bottom };
	for (int i = 0; i < 2; i++) {
		if (*s != '+' && *s != '-')
			return false;
		*from_end[i] = *s++ == '-';
		/* Only digits, and few enough to add to a screen position safely. */
		int digits = strspn(s, "0123456789");
		if (digits < 1 || digits > 5)
			return false;
		*value[i] = strtol(s, NULL, 10);
		s += digits;
	}
	return !*s;
}

/* Reported once per load, not each time a window asks. */
static void check_positions(void)
{
	struct config_position pos;
	for (int i = 0; i < n_rules; i++) {
		if (rules[i].client && !strcasecmp(rules[i].resource, "geometry") &&
				!parse_position(rules[i].value, &pos))
			wlr_log(WLR_ERROR, "%s:%d: expected a position like -0-0 or +10+10, "
				"not \"%s\"", path, rules[i].line, rules[i].value);
	}
}

void config_load(void)
{
	for (int i = 0; i < n_rules; i++) {
		free(rules[i].client);
		free(rules[i].resource);
		free(rules[i].value);
	}
	free(rules);
	rules = NULL;
	n_rules = 0;

	const char *cfg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (cfg && *cfg)
		snprintf(path, sizeof(path), "%s/mwc/mwmrc", cfg);
	else if (home)
		snprintf(path, sizeof(path), "%s/.config/mwc/mwmrc", home);
	else
		return;
	FILE *f = fopen(path, "r");
	if (!f) {
		load_tile_filter();
		load_root_items();
		return;
	}

	char *buf = NULL;
	size_t cap = 0;
	int line = 0;
	while (getline(&buf, &cap, f) != -1) {
		line++;
		char *s = trim(buf);
		if (!*s || *s == '!')
			continue;
		char *colon = strchr(s, ':');
		if (!colon) {
			wlr_log(WLR_ERROR, "%s:%d: missing ':'", path, line);
			continue;
		}
		*colon = '\0';
		add_rule(trim(s), trim(colon + 1), line);
	}
	free(buf);
	fclose(f);
	wlr_log(WLR_DEBUG, "%s: %d rules", path, n_rules);
	load_tile_filter();
	load_root_items();
	check_positions();
}

/* Global value of a resource, NULL when unset. */
const char *config_get(const char *resource)
{
	const char *v = NULL;
	for (int i = 0; i < n_rules; i++) {
		if (!rules[i].client &&
				strcasecmp(rules[i].resource, resource) == 0)
			v = rules[i].value;
	}
	return v;
}

const char *config_get_for(const char *client, const char *resource)
{
	const char *v = NULL;
	for (int i = 0; i < n_rules; i++) {
		if (rules[i].client && strcasecmp(rules[i].client, client) == 0 &&
				strcasecmp(rules[i].resource, resource) == 0)
			v = rules[i].value;
	}
	return v;
}

/* rgb:r/g/b with 1 to 4 hex digits per channel, as XParseColor reads it. */
static bool parse_rgb_form(const char *value, struct mwc_color *out)
{
	if (strncasecmp(value, "rgb:", 4) != 0)
		return false;
	const char *s = value + 4;
	float channels[3];
	for (int i = 0; i < 3; i++) {
		/* Only hex digits: strtoul alone also takes spaces, signs and 0x. */
		int digits = strspn(s, "0123456789abcdefABCDEF");
		if (digits < 1 || digits > 4 || (i < 2 && s[digits] != '/') ||
				(i == 2 && s[digits] != '\0'))
			return false;
		channels[i] = strtoul(s, NULL, 16) / (float)((1ul << (4 * digits)) - 1);
		s += digits + 1;
	}
	*out = (struct mwc_color){ channels[0], channels[1], channels[2] };
	return true;
}

bool config_color(const char *value, struct mwc_color *out)
{
	if (!value || !*value)
		return false;
	if (parse_rgb_form(value, out))
		return true;
	PangoColor c;
	if (!pango_color_parse(&c, value))
		return false;
	*out = (struct mwc_color){ c.red / 65535.0f, c.green / 65535.0f,
		c.blue / 65535.0f };
	return true;
}

int config_bool(const char *name, const char *s)
{
	static const char *warned; /* log each bad value once, not per drag */
	if (!s || !*s)
		return -1;
	if (!strcasecmp(s, "true") || !strcasecmp(s, "on") ||
			!strcasecmp(s, "yes") || !strcmp(s, "1"))
		return 1;
	if (!strcasecmp(s, "false") || !strcasecmp(s, "off") ||
			!strcasecmp(s, "no") || !strcmp(s, "0"))
		return 0;
	if (s != warned) {
		wlr_log(WLR_ERROR, "ignoring %s value %s, not a boolean", name, s);
		warned = s;
	}
	return -1;
}

static const struct rule *client_rule(const char *name, const char *class,
	const char *resource)
{
	const struct rule *r = NULL;
	for (int i = 0; i < n_rules; i++) {
		const char *c = rules[i].client;
		if (c && strcasecmp(rules[i].resource, resource) == 0 &&
				((name && strcasecmp(c, name) == 0) ||
				(class && strcasecmp(c, class) == 0)))
			r = &rules[i];
	}
	return r;
}

static bool rule_color(const struct rule *r, struct mwc_color *out)
{
	if (!r)
		return false;
	if (config_color(r->value, out))
		return true;
	wlr_log(WLR_ERROR, "%s:%d: unknown colour \"%s\"", path, r->line,
		r->value);
	return false;
}

bool config_view_position(struct view *view, struct config_position *out)
{
	/* Dialogs belong with their window, not in the program's corner. */
	if (view->wtype != WT_NORMAL || view_parent(view))
		return false;
	const char *name = view_app_id(view), *class = name;
	if (view->type == VIEW_XWAYLAND && view->xsurface->instance)
		name = view->xsurface->instance;
	const struct rule *r = client_rule(name, class, "geometry");
	return r && parse_position(r->value, out);
}

void config_view_tiling(struct view *view)
{
	view->no_tile = false;
	if (tile_filter == FILTER_NONE)
		return;
	const char *name = view_app_id(view), *class = name;
	if (view->type == VIEW_XWAYLAND && view->xsurface->instance)
		name = view->xsurface->instance;
	bool listed = false;
	for (int i = 0; i < n_tile_names && !listed; i++)
		listed = !strcasecmp(tile_names[i], name) ||
			!strcasecmp(tile_names[i], class);
	view->no_tile = tile_filter == FILTER_DENY ? listed : !listed;
}

/* Pick the frame colours for a window from its class or app_id. */
void config_view_shades(struct view *view)
{
	view->shade_active = theme.active;
	view->shade_inactive = theme.inactive;
	if (!n_rules)
		return;

	const char *name = view_app_id(view), *class = name;
	if (view->type == VIEW_XWAYLAND && view->xsurface->instance)
		name = view->xsurface->instance;

	struct mwc_color abg, afg, ibg, ifg;
	bool has_abg = rule_color(client_rule(name, class,
		"activeBackground"), &abg);
	bool has_afg = rule_color(client_rule(name, class,
		"activeForeground"), &afg);
	bool has_ibg = rule_color(client_rule(name, class, "background"), &ibg);
	bool has_ifg = rule_color(client_rule(name, class, "foreground"), &ifg);

	if (has_abg || has_afg) {
		if (!has_abg)
			abg = theme.active.bg;
		/* Title text the program doesn't set itself comes from the
		 * global entry, if there is one. */
		if (!has_afg)
			afg = theme.has_active_fg ? theme.active_fg :
				color_contrast(abg);
		view->shade_active = shade_from(abg, afg);
	}
	if (has_abg || has_ibg || has_ifg) {
		/* Unfocused: a paler version of the client's own colour. */
		if (!has_ibg)
			ibg = has_abg ? color_mix(abg, theme.inactive.bg, PALE) :
				theme.inactive.bg;
		if (!has_ifg)
			ifg = theme.has_inactive_fg ? theme.inactive_fg :
				color_contrast(ibg);
		view->shade_inactive = shade_from(ibg, ifg);
	}
}

void config_reload(struct server *server)
{
	wlr_log(WLR_INFO, "reloading the configuration");
	menu_close(server);
	config_load();
	theme_init();
	wallpaper_init(server);
	input_config_changed(server);
	outputs_config_changed(server);

	const float c[4] = { theme.root.r, theme.root.g, theme.root.b, 1 };
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		wlr_scene_rect_set_color(output->background, c);
		wlr_scene_node_set_enabled(&output->background->node,
			!theme.root_none);
		wallpaper_place(output, true);
	}
	struct view *view;
	wl_list_for_each(view, &server->views, link)
		view_theme_changed(view);
	tile_config_changed(server);
}
