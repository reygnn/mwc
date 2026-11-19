/*
 * config.c: mwmrc as real files in a temporary XDG_CONFIG_HOME, read by
 * config_load(): the line syntax, lookups global and per client, booleans, colours,
 * positions, placements, the root menu, DoNotTile and TileOnly, and a client's frame
 * colours from the theme, which the test sets. What mwc says about bad lines is read
 * through wlroots' own log callback.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../src/mwc.h"
#include "unit.h"
#include "mwmrc.h"

static bool is(const char *got, const char *want)
{
	return got && !strcmp(got, want);
}

/* ---- lines ------------------------------------------------------------------ */

/* Review 2026-11-18 #15: without HOME and XDG_CONFIG_HOME no file is read, and the
 * placements are the defaults, as without the file. */
TEST(no_path)
{
	load("Mwm*iconPlacement: top right\nMwm*dockPlacement: left top\n");
	char *cfg = strdup(getenv("XDG_CONFIG_HOME"));
	const char *home = getenv("HOME");
	char *home_copy = home ? strdup(home) : NULL;
	unsetenv("XDG_CONFIG_HOME");
	unsetenv("HOME");
	config_load();
	const struct config_placement *ip = config_icon_placement();
	const struct config_placement *dp = config_dock_placement();
	CHECK(ip->horizontal && ip->from_bottom && !ip->from_right);
	CHECK(dp->horizontal && dp->from_bottom && dp->from_right);
	CHECK(config_get("iconPlacement") == NULL);
	CHECK_INT(config_root_items(), 0);
	setenv("XDG_CONFIG_HOME", cfg, 1);
	if (home_copy)
		setenv("HOME", home_copy, 1);
	free(cfg);
	free(home_copy);
}

TEST(no_file)
{
	unlink(rc);
	logged[0] = '\0';
	config_load();
	CHECK(config_get("terminal") == NULL);
	CHECK_INT(config_root_items(), 0);
	CHECK(config_icon_placement()->horizontal && config_icon_placement()->from_bottom);
	CHECK(!config_icon_placement()->from_right);
	CHECK(config_dock_placement()->from_right && config_dock_placement()->from_bottom);
	CHECK(config_client_tiles("foot", "foot"));
	CHECK_INT(log_lines(""), 0);
}

TEST(lines_and_lookups)
{
	load("! a comment\n"
		"\n"
		"Mwm*terminal:   mwt  \n"
		"   Mwm*fontList: Sans Bold 10\t\n"
		"mwm.moveOpaque: true\n"
		"Mwm*terminal: foot\n"
		"Mwm*XTerm*activeBackground: #8a0303\n"
		"Mwm*org.gnome.Terminal*geometry: +10+20\n"
		"Mwm*runDialog: sh -c 'x: y'\n");
	CHECK(is(config_get("terminal"), "foot")); /* the later line wins */
	CHECK(is(config_get("TERMINAL"), "foot")); /* resources ignore case */
	CHECK(is(config_get("fontList"), "Sans Bold 10")); /* trimmed */
	CHECK(is(config_get("moveOpaque"), "true")); /* '.' separates as '*' does */
	CHECK(is(config_get("runDialog"), "sh -c 'x: y'")); /* split at the first ':' */
	CHECK(config_get("activeBackground") == NULL); /* a client's isn't global */
	CHECK(is(config_get_for("xterm", "activeBackground"), "#8a0303"));
	CHECK(is(config_get_for("org.gnome.Terminal", "geometry"), "+10+20"));
	CHECK(config_get_for("foot", "terminal") == NULL);
	CHECK_INT(log_lines(""), 0);
}

TEST(lines_malformed)
{
	load("Mwm*terminal mwt\n"         /* no ':' */
		"Xterm*foreground: red\n"   /* not Mwm */
		"Mwm: x\n"                  /* no resource */
		"Mwm*: x\n"
		"Mwm**fontList: Sans 9\n"   /* an empty client names nothing */
		"Mwmx*fontList: Sans 9\n"
		"*fontList: Sans 9\n"
		"Mwm*XTerm**background: red\n"             /* a client never matches */
		"Mwm*XTerm*vt100*activeBackground: red\n"  /* as an X resource names it */
		"Mwm*terminal: mwt\n");
	CHECK(is(config_get("terminal"), "mwt"));
	CHECK(config_get("fontList") == NULL);
	CHECK_INT(log_lines("missing ':'"), 1);
	CHECK_INT(log_lines("expected Mwm*resource or Mwm*client*resource"), 8);
	CHECK_INT(log_lines("mwmrc:8: expected"), 1);
	CHECK_INT(log_lines("mwmrc:9: expected"), 1);
	CHECK(strstr(logged, "mwmrc:1: missing") != NULL); /* with the line */
}

/* Review 2026-11-18 #11: a typo in a resource is said, once per load, with its line. */
TEST(unknown_resources)
{
	load("Mwm*terminl: mwt\n"
		"Mwm*foot*activeBackgrond: red\n"
		"Mwm*TERMINAL: foot\n"
		"Mwm*DP-1*outputMode: 1920x1080\n"
		"Mwm*foot*geometry: +0+0\n"
		"Mwm*foot*fontList: Sans 9\n"
		"Mwm*keyboardVariant: nodeadkeys\n"
		"Mwm*clipboardRectangleScreenshot: shot -c -r\n");
	CHECK_INT(log_lines("unknown resource"), 3);
	CHECK_INT(log_lines("mwmrc:1: unknown resource terminl"), 1);
	CHECK_INT(log_lines("mwmrc:2: unknown resource activeBackgrond"), 1);
	/* fontList is global only: for one program it means nothing. */
	CHECK_INT(log_lines("mwmrc:6: unknown resource fontList"), 1);
	CHECK(is(config_get("terminal"), "foot")); /* still read as before */
}

/* docs/mwmrc, its examples too, is read without a word: every line well-formed, every
 * resource known, the values the load checks good: per-program colours and places,
 * placements, root menu items (run from the top of the tree). */
TEST(documented_mwmrc)
{
	FILE *f = fopen("docs/mwmrc", "r");
	CHECK(f != NULL);
	static char text[65536];
	size_t n = 0;
	char line[1024];
	bool fits = true;
	while (f && fgets(line, sizeof(line), f)) {
		size_t len = strlen(line);
		if (len && line[len - 1] != '\n' && !feof(f))
			fits = false; /* longer than line */
		const char *l = line;
		if (*l == '!') {
			l += strspn(l + 1, " ") + 1;
			if (strncmp(l, "Mwm", 3))
				continue;
		}
		len = strlen(l);
		if (n + len >= sizeof(text)) {
			fits = false;
			break;
		}
		memcpy(text + n, l, len);
		n += len;
	}
	text[n] = '\0';
	if (f)
		fclose(f);
	CHECK(fits);
	load(text);
	CHECK(config_get("rootBackground") != NULL); /* it was read */
	CHECK_INT(log_lines(""), 0);
}

/* ---- values ------------------------------------------------------------------ */

TEST(booleans)
{
	load("");
	static const char *const yes[] = { "true", "TRUE", "on", "yes", "1" };
	static const char *const no[] = { "false", "Off", "no", "0" };
	for (size_t i = 0; i < sizeof(yes) / sizeof(*yes); i++)
		CHECK_INT(config_bool("x", yes[i]), 1);
	for (size_t i = 0; i < sizeof(no) / sizeof(*no); i++)
		CHECK_INT(config_bool("x", no[i]), 0);
	CHECK_INT(config_bool("x", NULL), -1);
	CHECK_INT(config_bool("x", ""), -1);
	CHECK_INT(config_bool("x", "2"), -1);
	CHECK_INT(config_bool("x", "yes please"), -1);
}

/* Said once per value and load, not on every map or drag that asks. */
TEST(booleans_reported_once)
{
	load("");
	for (int i = 0; i < 5; i++)
		config_bool("moveOpaque", "maybe");
	config_bool("tiling", "maybe");
	CHECK_INT(log_lines("ignoring moveOpaque value maybe"), 1);
	CHECK_INT(log_lines("ignoring tiling value maybe"), 1);
	load("");
	config_bool("moveOpaque", "maybe");
	CHECK_INT(log_lines("ignoring moveOpaque value maybe"), 1); /* again after a load */
	/* More bad values than it keeps: the oldest go, nothing overflows. */
	char v[32];
	for (int i = 0; i < 100; i++) {
		snprintf(v, sizeof(v), "v%d", i);
		config_bool("x", v);
	}
	CHECK_INT(log_lines("ignoring x value"), 100);
}

static bool color_is(const char *value, float r, float g, float b)
{
	struct mwc_color c = { -1, -1, -1 };
	if (!config_color(value, &c))
		return false;
	return c.r > r - 0.002f && c.r < r + 0.002f && c.g > g - 0.002f &&
		c.g < g + 0.002f && c.b > b - 0.002f && c.b < b + 0.002f;
}

TEST(colours)
{
	CHECK(color_is("#ff8000", 1, 128 / 255.0f, 0));
	CHECK(color_is("#000", 0, 0, 0));
	CHECK(color_is("rgb:f/0/8", 1, 0, 8 / 15.0f));
	CHECK(color_is("rgb:ffff/0/8000", 1, 0, 0x8000 / 65535.0f));
	CHECK(color_is("RGB:FF/ff/00", 1, 1, 0));
	CHECK(color_is("gold", 1, 215 / 255.0f, 0));
	CHECK(color_is("Navy", 0, 0, 128 / 255.0f));
	static const char *const bad[] = {
		NULL, "", "#12345", "#gggggg", "nonsense", "rgb:", "rgb:1/2",
		"rgb:1/2/3/4", "rgb:12345/0/0", "rgb: 1/2/3", "rgb:+1/2/3", "rgb:0x1/2/3",
		"rgb:1/2/3 ", "rgb:/2/3",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		struct mwc_color c = { -1, -1, -1 };
		bool took = config_color(bad[i], &c);
		if (took)
			fprintf(stderr, "  took \"%s\"\n", bad[i] ? bad[i] : "(null)");
		CHECK(!took && c.r == -1);
	}
}

TEST(home)
{
	char out[512], want[512];
	setenv("HOME", "/home/someone", 1);
	config_expand_home("~/pictures/desk.png", out, sizeof(out));
	CHECK(is(out, "/home/someone/pictures/desk.png"));
	config_expand_home("/abs/path", out, sizeof(out));
	CHECK(is(out, "/abs/path"));
	config_expand_home("~user/x", out, sizeof(out));
	CHECK(is(out, "~user/x")); /* only ~/ */
	config_expand_home("~", out, sizeof(out));
	CHECK(is(out, "~"));
	/* Cut to the buffer, not past it. */
	char small[8];
	config_expand_home("~/abcdefgh", small, sizeof(small));
	snprintf(want, sizeof(want), "%.7s", "/home/someone/abcdefgh");
	CHECK(is(small, want));
	unsetenv("HOME");
	config_expand_home("~/x", out, sizeof(out));
	CHECK(is(out, "~/x"));
}

/* ---- per client ------------------------------------------------------------------ */

static bool position_is(const char *name, int x, int y, bool right, bool bottom)
{
	struct config_position p;
	return config_client_position(name, name, &p) && p.x == x && p.y == y &&
		p.from_right == right && p.from_bottom == bottom && !p.at_pointer;
}

TEST(positions)
{
	load("Mwm*a*geometry: +10+20\n"
		"Mwm*b*geometry: -0-0\n"
		"Mwm*c*geometry: +99999-5\n"
		"Mwm*d*geometry: pointer\n"
		"Mwm*e*geometry: +100000+0\n" /* six digits */
		"Mwm*f*geometry: 10+20\n"
		"Mwm*g*geometry: +10+20+30\n"
		"Mwm*h*geometry: +-1+2\n"
		"Mwm*i*geometry: 300x200+0+0\n"
		"Mwm*j*geometry: +1 +2\n");
	CHECK(position_is("a", 10, 20, false, false));
	CHECK(position_is("b", 0, 0, true, true));
	CHECK(position_is("c", 99999, 5, false, true));
	struct config_position p;
	CHECK(config_client_position("D", "D", &p) && p.at_pointer); /* ignoring case */
	for (const char *c = "efghij"; *c; c++) {
		char name[2] = { *c, 0 };
		CHECK(!config_client_position(name, name, &p));
	}
	CHECK(!config_client_position("nobody", "nobody", &p));
	CHECK_INT(log_lines("expected a position like"), 6); /* once, when loaded */
}

/* X11 windows go by instance or class; the last line for either wins. */
TEST(instance_or_class)
{
	load("Mwm*XTerm*geometry: +1+1\n"
		"Mwm*login*geometry: +2+2\n"
		"Mwm*XTerm*allDesktops: true\n");
	struct config_position p;
	CHECK(config_client_position("login", "XTerm", &p) && p.x == 2);
	CHECK(config_client_position("other", "XTerm", &p) && p.x == 1);
	CHECK(config_client_position("login", NULL, &p) && p.x == 2);
	CHECK(config_client_position(NULL, "xterm", &p) && p.x == 1);
	CHECK(config_client_all_desktops("x", "XTerm"));
	CHECK(!config_client_all_desktops("login", "UXTerm"));
}

TEST(dock_order)
{
	load("Mwm*clock*dock: true\n"
		"Mwm*pulse*dock: yes\n"
		"Mwm*mixer*dock: off\n"
		"Mwm*term*geometry: +0+0\n"
		"Mwm*clock*dock: true\n");
	int clock = config_client_dock("clock", "clock");
	int pulse = config_client_dock("pulse", "pulse");
	CHECK(pulse >= 0 && clock > pulse); /* the line that counts decides the order */
	CHECK_INT(config_client_dock("mixer", "mixer"), -1);
	CHECK_INT(config_client_dock("term", "term"), -1);
}

static bool tiles(const char *name)
{
	return config_client_tiles(name, name);
}

TEST(tile_filter)
{
	load("Mwm*DoNotTile: Navigator, Gimp Image Window ,,mpv\n");
	CHECK(!tiles("navigator"));
	CHECK(!tiles("Gimp Image Window")); /* spaces belong to the name */
	CHECK(!tiles("MPV"));
	CHECK(tiles("foot"));
	CHECK(config_client_tiles("firefox", "foot"));
	CHECK(!config_client_tiles("Navigator", "firefox")); /* either name */
	load("Mwm*DoNotTile: foot\nMwm*TileOnly: XTerm, mfm\n"); /* the last one counts */
	CHECK(tiles("xterm"));
	CHECK(tiles("mfm"));
	CHECK(!tiles("foot"));
	load("Mwm*TileOnly: XTerm\nMwm*TileOnly:\n"); /* empty: off again */
	CHECK(tiles("foot"));
	load("Mwm*foot*DoNotTile: x\n"); /* a client's line isn't the filter */
	CHECK(tiles("x"));
}

/* ---- placements and the root menu --------------------------------------------- */

static bool placed(const char *value, bool horizontal, bool right, bool bottom,
	bool center, bool tight)
{
	char text[256];
	snprintf(text, sizeof(text), "Mwm*iconPlacement: %s\n", value);
	load(text);
	const struct config_placement *p = config_icon_placement();
	return p->horizontal == horizontal && p->from_right == right &&
		p->from_bottom == bottom && p->center == center && p->tight == tight;
}

TEST(placements)
{
	CHECK(placed("left bottom", true, false, true, false, false));
	CHECK(placed("top right", false, true, false, false, false));
	CHECK(placed("bottom left tight", false, false, true, false, true));
	CHECK(placed("Bottom LEFT Tight", false, false, true, false, true));
	CHECK(placed("right top", true, true, false, false, false));
	CHECK(placed("center right", false, true, false, true, false));
	CHECK(placed("top center", true, false, false, true, false));
	CHECK_INT(log_lines(""), 0);
	static const char *const bad[] = {
		"", "left", "left left", "top bottom", "left top bottom", "center",
		"center center", "left center top", "tight left bottom", "left bottom loose",
		"middle right",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		bool fell_back = placed(bad[i], true, false, true, false, false);
		if (!fell_back || log_lines("expected a placement like") != 1)
			fprintf(stderr, "  took \"%s\"\n", bad[i]);
		CHECK(fell_back);
		CHECK_INT(log_lines("expected a placement like"), 1);
	}
	load("Mwm*dockPlacement: left top\n");
	CHECK(!config_dock_placement()->from_right && !config_dock_placement()->from_bottom);
	CHECK(config_icon_placement()->from_bottom); /* its own default still */
}

TEST(root_menu)
{
	load("Mwm*rootMenuItem: Files: mfm\n"
		"Mwm*rootMenuItem:  Web :  firefox --new-window http://x  \n"
		"Mwm*rootMenuItem: no command\n"
		"Mwm*rootMenuItem: : nolabel\n"
		"Mwm*rootMenuItem: empty:\n"
		"Mwm*foot*rootMenuItem: Not: global\n");
	CHECK_INT(config_root_items(), 2);
	const char *label, *command;
	config_root_item(0, &label, &command);
	CHECK(is(label, "Files") && is(command, "mfm"));
	config_root_item(1, &label, &command);
	CHECK(is(label, "Web") && is(command, "firefox --new-window http://x"));
	CHECK_INT(log_lines("expected Mwm*rootMenuItem: label: command"), 3);
	char text[2048] = "";
	for (int i = 0; i < CONFIG_MAX_ROOT_ITEMS + 5; i++)
		snprintf(text + strlen(text), sizeof(text) - strlen(text),
			"Mwm*rootMenuItem: P%d: p%d\n", i, i);
	load(text);
	CHECK_INT(config_root_items(), CONFIG_MAX_ROOT_ITEMS);
	config_root_item(CONFIG_MAX_ROOT_ITEMS - 1, &label, &command);
	CHECK(is(label, "P19"));
	CHECK_INT(log_lines("only the first 20"), 5);
}

/* ---- frame colours ------------------------------------------------------------ */

static struct mwc_shade shade(float bg, float fg)
{
	return (struct mwc_shade){ .bg = { bg, bg, bg }, .fg = { fg, fg, fg } };
}

static bool same(struct mwc_color a, struct mwc_color b)
{
	return a.r == b.r && a.g == b.g && a.b == b.b;
}

TEST(shades_from_the_theme)
{
	theme = (struct mwc_theme){ .active = shade(0.5f, 0), .inactive = shade(0.7f, 0) };
	load("");
	struct mwc_shade a, in;
	config_client_shades("foot", "foot", &a, &in);
	CHECK(same(a.bg, theme.active.bg) && same(in.bg, theme.inactive.bg));
	load("Mwm*terminal: mwt\n");
	config_client_shades("foot", "foot", &a, &in);
	CHECK(same(a.bg, theme.active.bg) && same(in.bg, theme.inactive.bg));
}

TEST(shades_of_a_client)
{
	theme = (struct mwc_theme){ .active = shade(0.5f, 0), .inactive = shade(0.7f, 0) };
	struct mwc_color red = { 1, 0, 0 }, white = { 1, 1, 1 }, black = { 0, 0, 0 };
	struct mwc_shade a, in;
	/* Its own active colour: text by contrast, a paler one when unfocused. */
	load("Mwm*XTerm*activeBackground: #ff0000\n");
	config_client_shades("xterm", "XTerm", &a, &in);
	CHECK(same(a.bg, red));
	CHECK(same(a.fg, color_contrast(red)));
	CHECK(same(in.bg, color_mix(red, theme.inactive.bg, 0.55f)));
	CHECK(same(a.top, shade_from(red, a.fg).top));
	/* The global foreground goes before contrast. */
	theme.has_active_fg = true;
	theme.active_fg = white;
	config_client_shades("xterm", "XTerm", &a, &in);
	CHECK(same(a.fg, white));
	/* Only an inactive colour: the active frame stays the theme's. */
	theme.has_active_fg = false;
	load("Mwm*XTerm*background: black\n");
	config_client_shades("xterm", "XTerm", &a, &in);
	CHECK(same(a.bg, theme.active.bg));
	CHECK(same(in.bg, black));
	CHECK(same(in.fg, color_contrast(black)));
	/* A bad colour is said, with its line, and left out: once when mwmrc is read,
	 * not each time a window maps (Review 2026-11-18 #12). */
	load("! x\nMwm*XTerm*activeBackground: reddish\n");
	CHECK_INT(log_lines("mwmrc:2: unknown colour \"reddish\""), 1);
	for (int i = 0; i < 3; i++)
		config_client_shades("xterm", "XTerm", &a, &in);
	CHECK(same(a.bg, theme.active.bg));
	CHECK_INT(log_lines("unknown colour"), 1);
}

int main(void)
{
	mwmrc_init();

	RUN(no_file);
	RUN(no_path);
	RUN(unknown_resources);
	RUN(documented_mwmrc);
	RUN(lines_and_lookups);
	RUN(lines_malformed);
	RUN(booleans);
	RUN(booleans_reported_once);
	RUN(colours);
	RUN(home);
	RUN(positions);
	RUN(instance_or_class);
	RUN(dock_order);
	RUN(tile_filter);
	RUN(placements);
	RUN(root_menu);
	RUN(shades_from_the_theme);
	RUN(shades_of_a_client);

	mwmrc_done();
	return unit_done();
}
