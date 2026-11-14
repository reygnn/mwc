/*
 * libFuzzer harness for the mwmrc parser: each input is a whole mwmrc. It goes
 * through config_load() as a file, then through everything that reads the rules:
 * lookups, colours, booleans, positions, iconPlacement, the root menu, DoNotTile and
 * TileOnly. config.c is included, so its static parts are reached too; what it calls
 * of the compositor is stubbed below and never reached from here.
 *
 *   make fuzz          fuzzes for FUZZ_TIME seconds, corpus in fuzz/corpus/mwmrc
 *   make fuzz-replay   runs every kept input once, as make check does
 */
#include "../src/config.c"

#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- stubs: the compositor around config.c -------------------------------- */

struct mwc_theme theme;

struct mwc_shade shade_from(struct mwc_color bg, struct mwc_color fg)
{
	return (struct mwc_shade){ bg, bg, bg, fg };
}

struct mwc_color color_mix(struct mwc_color a, struct mwc_color b, float f)
{
	return a;
}

struct mwc_color color_contrast(struct mwc_color bg)
{
	return bg;
}

const char *view_app_id(struct view *view) { return NULL; }
struct view *view_parent(struct view *view) { return NULL; }
void view_theme_changed(struct view *view) {}
void theme_init(void) {}
void wallpaper_init(struct server *server) {}
void wallpaper_place(struct output *output, bool force) {}
void menu_close(struct server *server) {}
void input_config_changed(struct server *server) {}
void outputs_config_changed(struct server *server) {}
void iconbox_config_changed(struct server *server) {}
void desktop_config_changed(struct server *server) {}
void icons_arrange(struct server *server) {}
void tile_config_changed(struct server *server) {}
void dock_config_changed(struct server *server) {}
void wlr_scene_node_set_enabled(struct wlr_scene_node *node, bool enabled) {}
void wlr_scene_rect_set_color(struct wlr_scene_rect *rect, const float color[4]) {}

/* The log is the parser's answer to bad input: formatted, so its arguments are
 * read, but not printed. */
void _wlr_log(enum wlr_log_importance verbosity, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
}

/* ---- the harness ------------------------------------------------------------ */

static char rc[4096];

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	char dir[] = "/tmp/mwc-fuzz.XXXXXX";
	if (!mkdtemp(dir))
		abort();
	setenv("XDG_CONFIG_HOME", dir, 1);
	setenv("HOME", dir, 1);
	char sub[4200];
	snprintf(sub, sizeof(sub), "%s/mwc", dir);
	mkdir(sub, 0700);
	snprintf(rc, sizeof(rc), "%s/mwmrc", sub);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	FILE *f = fopen(rc, "w");
	if (!f)
		abort();
	fwrite(data, 1, size, f);
	fclose(f);

	config_load();
	/* Everything mwc asks of the rules, with each rule's own words as well. */
	for (int i = 0; i < n_rules; i++) {
		const struct rule *r = &rules[i];
		struct mwc_color c;
		struct config_position pos;
		struct config_placement ip;
		char path[4096];
		config_color(r->value, &c);
		config_bool(r->resource, r->value);
		parse_position(r->value, &pos);
		parse_icon_placement(r->value, &ip);
		config_expand_home(r->value, path, sizeof(path));
		config_get(r->resource);
		if (r->client)
			config_get_for(r->client, r->resource);
	}
	for (int i = 0; i < config_root_items(); i++) {
		const char *label, *command;
		config_root_item(i, &label, &command);
	}
	config_icon_placement();
	return 0;
}
