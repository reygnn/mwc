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
 *   Mwm*XTerm*activeBackground:    #8a0303    per client
 *   Mwm*foot*activeBackground:     gold
 *
 * The client part matches an X11 window's WM_CLASS instance or class name, or
 * a Wayland client's app_id, ignoring case. Colours are #rrggbb, colour names
 * or rgb:rr/gg/bb. Lines starting with ! are comments; there are no comments
 * after a value. Later lines win.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
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
	/* "Mwm*XTerm*activeBackground", '.' and '*' both separate. */
	char *parts[4];
	int n = 0;
	for (char *tok = strtok(key, "*."); tok && n < 4;
			tok = strtok(NULL, "*."))
		parts[n++] = tok;
	if (n < 2 || n > 3 || strcasecmp(parts[0], "mwm") != 0) {
		wlr_log(WLR_ERROR, "%s:%d: expected Mwm*resource or "
			"Mwm*client*resource", path, line);
		return;
	}
	rules = realloc(rules, sizeof(*rules) * (n_rules + 1));
	rules[n_rules++] = (struct rule){
		.client = n == 3 ? strdup(parts[1]) : NULL,
		.resource = strdup(parts[n - 1]),
		.value = strdup(value),
		.line = line,
	};
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
	if (!f)
		return;

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

/* rgb:r/g/b with 1 to 4 hex digits per channel, as XParseColor reads it. */
static bool parse_rgb_form(const char *value, struct mwc_color *out)
{
	if (strncasecmp(value, "rgb:", 4) != 0)
		return false;
	const char *s = value + 4;
	float channels[3];
	for (int i = 0; i < 3; i++) {
		char *end;
		unsigned long v = strtoul(s, &end, 16);
		int digits = end - s;
		if (digits < 1 || digits > 4 || (i < 2 && *end != '/') ||
				(i == 2 && *end != '\0'))
			return false;
		channels[i] = v / (float)((1ul << (4 * digits)) - 1);
		s = end + 1;
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

	const float c[4] = { theme.root.r, theme.root.g, theme.root.b, 1 };
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		wlr_scene_rect_set_color(output->background, c);
		wallpaper_place(output, true);
	}
	struct view *view;
	wl_list_for_each(view, &server->views, link)
		view_theme_changed(view);
}
