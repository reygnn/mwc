/*
 * Writes docs/irix-schemes.html from docs/mwmrc.irix-schemes: each scheme as a focused
 * and an unfocused frame, with title text and shadows from draw.c's own color_contrast()
 * and shade_from(), as mwc draws them. make schemes runs it, make schemes-test checks
 * that the page in the repository is current.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/mwc.h"

#define MAX_SCHEMES 64

struct scheme {
	char name[32];
	char note[64];             /* "(colours as Base)" and the like, without brackets */
	unsigned active, inactive; /* activeBackground, background */
	unsigned over_active, over_inactive; /* the optional title text, NONE if none */
};

#define NONE 0x1000000u

static struct mwc_color rgb(unsigned v)
{
	return (struct mwc_color){ ((v >> 16) & 0xff) / 255.0f, ((v >> 8) & 0xff) / 255.0f,
		(v & 0xff) / 255.0f };
}

/* As the screen shows it: the 16-bit value cut to its high byte, as cairo does. */
static unsigned byte(float c)
{
	return (unsigned)lroundf(c * 65535) >> 8;
}

static unsigned hex(struct mwc_color c)
{
	return byte(c.r) << 16 | byte(c.g) << 8 | byte(c.b);
}

/* "#rrggbb" after the first ':' of l, or NONE. */
static unsigned value(const char *l)
{
	const char *p = strchr(l, ':');
	p = p ? strchr(p, '#') : NULL;
	return p ? (unsigned)strtoul(p + 1, NULL, 16) : NONE;
}

static int read_schemes(const char *path, struct scheme *s)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		perror(path);
		exit(1);
	}
	int n = 0;
	bool optional = false;
	struct scheme *cur = NULL;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		/* Commented out or not, a scheme's lines count. */
		const char *l = line + (line[0] == '!' ? strspn(line + 1, " ") + 1 : 0);
		if (!strncmp(line, "! ===", 5)) {
			optional = true;
		} else if (!strncmp(line, "! --- ", 6)) {
			char name[32] = "", note[64] = "";
			sscanf(line + 6, "%31s (%63[^)]", name, note);
			cur = NULL;
			for (int i = 0; i < n; i++)
				if (!strcmp(s[i].name, name))
					cur = &s[i];
			if (!optional && !cur && n < MAX_SCHEMES) {
				cur = &s[n++];
				*cur = (struct scheme){ .active = NONE, .inactive = NONE,
					.over_active = NONE, .over_inactive = NONE };
				strcpy(cur->name, name);
				strcpy(cur->note, note);
			}
		} else if (cur && !strncmp(l, "Mwm*", 4)) {
			bool active = !strncmp(l + 4, "active", 6);
			if (!optional && strstr(l, "ackground:"))
				*(active ? &cur->active : &cur->inactive) = value(l);
			else if (optional && strstr(l, "oreground:"))
				*(active ? &cur->over_active : &cur->over_inactive) = value(l);
		}
	}
	fclose(f);
	return n;
}

static void frame(unsigned bg, const char *title)
{
	struct mwc_color c = rgb(bg);
	struct mwc_shade s = shade_from(c, color_contrast(c));
	char bevel[64];
	snprintf(bevel, sizeof(bevel), "border-color:#%06x #%06x #%06x #%06x",
		hex(s.top), hex(s.bottom), hex(s.bottom), hex(s.top));
	printf("<div class=\"win\" style=\"background:#%06x;%s\"><div class=\"bar\">", bg, bevel);
	static const char *const glyphs[] = { "g", "g min", "g max" };
	for (int i = 0; i < 3; i++) {
		if (i == 1)
			printf("<span class=\"title\" style=\"color:#%06x;%s\">%s</span>",
				hex(s.fg), bevel, title);
		printf("<span class=\"btn\" style=\"%s\"><span class=\"%s\" style=\"%s\">"
			"</span></span>", bevel, glyphs[i], bevel);
	}
	printf("</div><div class=\"client\"></div></div>\n");
}

static void row(const char *resource, unsigned bg, unsigned over)
{
	unsigned fg = hex(color_contrast(rgb(bg)));
	printf("<div class=\"row\"><span class=\"sw\" style=\"background:#%06x\"></span>"
		"<b>%s</b> #%06x &middot; text #%06x", bg, resource, bg, fg);
	if (over != NONE)
		printf(" <em>(optional override: #%06x)</em>", over);
	printf("</div>\n");
}

static const char head[] =
"<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"<title>IRIX colour schemes</title>\n<style>\n"
":root { --page:#d9d9d9; --card:#ececec; --ink:#1a1a1a; --muted:#555; --line:#bdbdbd; }\n"
"@media (prefers-color-scheme: dark) { :root:not([data-theme=\"light\"]) {\n"
"  --page:#1b1d20; --card:#26292d; --ink:#e8e8e8; --muted:#a8a8a8; --line:#3a3e43; } }\n"
":root[data-theme=\"dark\"] {\n"
"  --page:#1b1d20; --card:#26292d; --ink:#e8e8e8; --muted:#a8a8a8; --line:#3a3e43; }\n"
"* { box-sizing:border-box; }\n"
"body { margin:0; background:var(--page); color:var(--ink);\n"
"  font:14px/1.45 system-ui, sans-serif; padding:24px 16px; }\n"
"header { max-width:1200px; margin:0 auto 20px; }\n"
"h1 { margin:0 0 6px; font-size:22px; }\n"
"header p { margin:0; color:var(--muted); max-width:75ch; }\n"
"main { max-width:1200px; margin:0 auto; display:grid;\n"
"  grid-template-columns:repeat(auto-fill, minmax(min(340px, 100%), 1fr)); gap:16px; }\n"
".card { background:var(--card); border:1px solid var(--line); border-radius:6px;\n"
"  padding:14px; min-width:0; }\n"
"h2 { margin:0 0 10px; font-size:16px; }\n"
"h2 small { font-weight:400; color:var(--muted); font-size:13px; }\n"
".pair { display:grid; grid-template-columns:1fr 1fr; gap:10px; margin-bottom:10px; }\n"
".win { border:2px solid; padding:3px; min-width:0; }\n"
".bar { display:flex; height:22px; }\n"
".btn { flex:0 0 22px; border:1px solid; display:flex; align-items:center;\n"
"  justify-content:center; }\n"
".g { width:12px; height:4px; border:1px solid; }\n"
".g.min { width:5px; height:5px; } .g.max { width:12px; height:12px; }\n"
".title { flex:1; min-width:0; border:1px solid; text-align:center; padding:0 4px;\n"
"  font:bold 12px/20px \"DejaVu Sans\", sans-serif; white-space:nowrap; overflow:hidden;\n"
"  text-overflow:ellipsis; }\n"
".client { height:46px; margin-top:3px; background:#fff; border:1px solid;\n"
"  border-color:#00000066 #ffffff66 #ffffff66 #00000066; }\n"
".row { display:flex; gap:6px; align-items:center; font-size:12.5px; flex-wrap:wrap; }\n"
".row b { font-weight:600; min-width:118px; } .row em { color:var(--muted); }\n"
".sw { width:14px; height:14px; border:1px solid var(--line); flex:0 0 auto; }\n"
"pre { margin:10px 0 0; padding:8px; background:var(--page); border-radius:4px;\n"
"  font-size:12px; overflow-x:auto; }\n"
"</style></head><body>\n"
"<header><h1>IRIX 6.5 colour schemes for mwc</h1>\n"
"<p>The frame colours in <code>docs/mwmrc.irix-schemes</code>. Title text and shadows\n"
"are mwc's, from Motif's formula: black text above 70% brightness, white otherwise.\n"
"Where the palette gives other title text, its optional override is noted. The lines\n"
"in the box go into <code>~/.config/mwc/mwmrc</code>.</p></header>\n<main>\n";

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s mwmrc.irix-schemes\n", argv[0]);
		return 2;
	}
	static struct scheme s[MAX_SCHEMES];
	int n = read_schemes(argv[1], s);
	fputs(head, stdout);
	for (int i = 0; i < n; i++) {
		if (s[i].active == NONE || s[i].inactive == NONE) {
			fprintf(stderr, "%s: scheme %s lacks a colour\n", argv[1], s[i].name);
			return 1;
		}
		printf("<section class=\"card\"><h2>%s", s[i].name);
		if (*s[i].note)
			printf(" <small>%s</small>", s[i].note);
		printf("</h2>\n<div class=\"pair\">\n");
		frame(s[i].active, "focused");
		frame(s[i].inactive, "unfocused");
		printf("</div>\n");
		row("activeBackground", s[i].active, s[i].over_active);
		row("background", s[i].inactive, s[i].over_inactive);
		printf("<pre>Mwm*activeBackground:        #%06x\n"
			"Mwm*background:              #%06x", s[i].active, s[i].inactive);
		if (s[i].over_active != NONE || s[i].over_inactive != NONE)
			printf("\n! optional");
		if (s[i].over_active != NONE)
			printf("\nMwm*activeForeground:        #%06x", s[i].over_active);
		if (s[i].over_inactive != NONE)
			printf("\nMwm*foreground:              #%06x", s[i].over_inactive);
		printf("</pre></section>\n");
	}
	printf("</main></body></html>\n");
	return 0;
}
