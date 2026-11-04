/*
 * An X11 client for the tests that can say anything about itself: one window,
 * plus optionally a dialog belonging to it. Prints what it receives.
 *
 *   xclient key=value ...
 *     class=NAME          WM_CLASS, required; shows up as app= in the state
 *     size=WxH            default 300x200
 *     pos=X,Y             asks for that place with USPosition
 *     gravity=nw|n|ne|w|c|e|sw|s|se|static
 *     min=WxH max=WxH inc=WxH base=WxH
 *     aspect=N/D          fixed aspect ratio
 *     type=dialog|utility|splash|dock|desktop|notification|toolbar
 *     motif=FUNCS,DECOR   _MOTIF_WM_HINTS
 *     input=0             WM_HINTS without input focus
 *     state=above|below|fullscreen|maximized   initial _NET_WM_STATE
 *     strut=L,R,T,B       _NET_WM_STRUT_PARTIAL along the whole edge
 *     color=rrggbb        background
 *     dialog=NAME         also open a transient dialog with this WM_CLASS
 *     workarea=1          print the root's _NET_WORKAREA whenever it changes
 */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Display *dpy;
static Atom wm_delete, wm_protocols;

struct options {
	const char *class, *type, *state, *dialog, *gravity;
	int w, h, x, y;
	bool has_pos;
	int min_w, min_h, max_w, max_h, inc_w, inc_h, base_w, base_h;
	bool has_min, has_max, has_inc, has_base;
	int aspect_n, aspect_d;
	long motif_funcs, motif_decor;
	bool has_motif;
	bool no_input, workarea;
	long strut[4];
	bool has_strut;
	unsigned long color;
};

static int parse_gravity(const char *g)
{
	static const struct {
		const char *name;
		int gravity;
	} table[] = {
		{ "nw", NorthWestGravity }, { "n", NorthGravity },
		{ "ne", NorthEastGravity }, { "w", WestGravity },
		{ "c", CenterGravity }, { "e", EastGravity },
		{ "sw", SouthWestGravity }, { "s", SouthGravity },
		{ "se", SouthEastGravity }, { "static", StaticGravity },
	};
	for (size_t i = 0; i < sizeof(table) / sizeof(*table); i++) {
		if (strcmp(table[i].name, g) == 0)
			return table[i].gravity;
	}
	return NorthWestGravity;
}

static void set_atom_list(Window win, const char *prop, const char *value)
{
	Atom a = XInternAtom(dpy, value, False);
	XChangeProperty(dpy, win, XInternAtom(dpy, prop, False), XA_ATOM, 32,
		PropModeReplace, (unsigned char *)&a, 1);
}

static Window create_window(const struct options *o, const char *class,
	Window transient_for)
{
	XSetWindowAttributes a = {
		.background_pixel = o->color,
		.event_mask = FocusChangeMask | StructureNotifyMask,
	};
	Window win = XCreateWindow(dpy, DefaultRootWindow(dpy), o->x, o->y, o->w, o->h,
		0, CopyFromParent, InputOutput, CopyFromParent,
		CWBackPixel | CWEventMask, &a);

	XClassHint ch = { (char *)class, (char *)class };
	XSetClassHint(dpy, win, &ch);
	XStoreName(dpy, win, class);
	XSetWMProtocols(dpy, win, &wm_delete, 1);

	XSizeHints sh = { 0 };
	if (o->has_pos) {
		sh.flags |= USPosition;
		sh.x = o->x;
		sh.y = o->y;
	}
	if (o->gravity) {
		sh.flags |= PWinGravity;
		sh.win_gravity = parse_gravity(o->gravity);
	}
	if (o->has_min) {
		sh.flags |= PMinSize;
		sh.min_width = o->min_w;
		sh.min_height = o->min_h;
	}
	if (o->has_max) {
		sh.flags |= PMaxSize;
		sh.max_width = o->max_w;
		sh.max_height = o->max_h;
	}
	if (o->has_inc) {
		sh.flags |= PResizeInc;
		sh.width_inc = o->inc_w;
		sh.height_inc = o->inc_h;
	}
	if (o->has_base) {
		sh.flags |= PBaseSize;
		sh.base_width = o->base_w;
		sh.base_height = o->base_h;
	}
	if (o->aspect_n) {
		sh.flags |= PAspect;
		sh.min_aspect.x = sh.max_aspect.x = o->aspect_n;
		sh.min_aspect.y = sh.max_aspect.y = o->aspect_d;
	}
	XSetWMNormalHints(dpy, win, &sh);

	if (o->no_input) {
		XWMHints wh = { .flags = InputHint, .input = False };
		XSetWMHints(dpy, win, &wh);
	}
	if (o->has_motif) {
		long hints[5] = { 3, o->motif_funcs, o->motif_decor, 0, 0 };
		Atom m = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
		XChangeProperty(dpy, win, m, m, 32, PropModeReplace,
			(unsigned char *)hints, 5);
	}
	if (o->type) {
		char name[64];
		snprintf(name, sizeof(name), "_NET_WM_WINDOW_TYPE_%s", o->type);
		for (char *p = name + 20; *p; p++)
			*p = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
		set_atom_list(win, "_NET_WM_WINDOW_TYPE", name);
	}
	if (o->state) {
		Atom a[2];
		int n = 1;
		if (strcmp(o->state, "maximized") == 0) {
			a[0] = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
			a[1] = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
			n = 2;
		} else {
			char name[64];
			snprintf(name, sizeof(name), "_NET_WM_STATE_%s", o->state);
			for (char *p = name + 14; *p; p++)
				*p = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
			a[0] = XInternAtom(dpy, name, False);
		}
		XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_STATE", False),
			XA_ATOM, 32, PropModeReplace, (unsigned char *)a, n);
	}
	if (o->has_strut) {
		int sw = DisplayWidth(dpy, DefaultScreen(dpy));
		int sh_ = DisplayHeight(dpy, DefaultScreen(dpy));
		long s[12] = { o->strut[0], o->strut[1], o->strut[2], o->strut[3],
			0, sh_ - 1, 0, sh_ - 1, 0, sw - 1, 0, sw - 1 };
		XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL",
			False), XA_CARDINAL, 32, PropModeReplace,
			(unsigned char *)s, 12);
	}
	if (transient_for)
		XSetTransientForHint(dpy, win, transient_for);
	XMapWindow(dpy, win);
	return win;
}

static void print_workarea(void)
{
	Atom type;
	int format;
	unsigned long n, after;
	unsigned char *data = NULL;
	if (XGetWindowProperty(dpy, DefaultRootWindow(dpy),
			XInternAtom(dpy, "_NET_WORKAREA", False), 0, 4, False, XA_CARDINAL,
			&type, &format, &n, &after, &data) == Success && data && n >= 4) {
		long *w = (long *)data;
		printf("workarea %ld %ld %ld %ld\n", w[0], w[1], w[2], w[3]);
	}
	if (data)
		XFree(data);
}

int main(int argc, char *argv[])
{
	struct options o = { .w = 300, .h = 200, .color = 0x808080 };
	for (int i = 1; i < argc; i++) {
		char *v = strchr(argv[i], '=');
		if (!v) {
			fprintf(stderr, "xclient: expected key=value: %s\n", argv[i]);
			return 2;
		}
		*v++ = '\0';
		const char *k = argv[i];
		if (!strcmp(k, "class"))
			o.class = v;
		else if (!strcmp(k, "size"))
			sscanf(v, "%dx%d", &o.w, &o.h);
		else if (!strcmp(k, "pos"))
			o.has_pos = sscanf(v, "%d,%d", &o.x, &o.y) == 2;
		else if (!strcmp(k, "gravity"))
			o.gravity = v;
		else if (!strcmp(k, "min"))
			o.has_min = sscanf(v, "%dx%d", &o.min_w, &o.min_h) == 2;
		else if (!strcmp(k, "max"))
			o.has_max = sscanf(v, "%dx%d", &o.max_w, &o.max_h) == 2;
		else if (!strcmp(k, "inc"))
			o.has_inc = sscanf(v, "%dx%d", &o.inc_w, &o.inc_h) == 2;
		else if (!strcmp(k, "base"))
			o.has_base = sscanf(v, "%dx%d", &o.base_w, &o.base_h) == 2;
		else if (!strcmp(k, "aspect"))
			sscanf(v, "%d/%d", &o.aspect_n, &o.aspect_d);
		else if (!strcmp(k, "type"))
			o.type = v;
		else if (!strcmp(k, "state"))
			o.state = v;
		else if (!strcmp(k, "motif"))
			o.has_motif = sscanf(v, "%ld,%ld", &o.motif_funcs,
				&o.motif_decor) == 2;
		else if (!strcmp(k, "input"))
			o.no_input = atoi(v) == 0;
		else if (!strcmp(k, "strut"))
			o.has_strut = sscanf(v, "%ld,%ld,%ld,%ld", &o.strut[0],
				&o.strut[1], &o.strut[2], &o.strut[3]) == 4;
		else if (!strcmp(k, "color"))
			o.color = strtoul(v, NULL, 16);
		else if (!strcmp(k, "dialog"))
			o.dialog = v;
		else if (!strcmp(k, "workarea"))
			o.workarea = atoi(v);
	}
	if (!o.class) {
		fprintf(stderr, "xclient: class= is required\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "xclient: cannot open the display\n");
		return 1;
	}
	wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
	wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	if (o.workarea) {
		XSelectInput(dpy, DefaultRootWindow(dpy), PropertyChangeMask);
		print_workarea();
	}

	Window main_win = create_window(&o, o.class, None);
	Window dialog_win = None;
	if (o.dialog) {
		struct options d = o;
		d.has_pos = d.has_min = d.has_max = d.has_motif = false;
		d.type = NULL;
		d.w = 120;
		d.h = 80;
		d.x = d.y = 0;
		dialog_win = create_window(&d, o.dialog, main_win);
	}
	XFlush(dpy);
	printf("up\n");

	int open = dialog_win ? 2 : 1;
	while (open > 0) {
		XEvent e;
		XNextEvent(dpy, &e);
		switch (e.type) {
		case FocusIn:
			printf("focus-in %s\n", e.xfocus.window == main_win ? "main" : "dialog");
			break;
		case PropertyNotify:
			if (o.workarea && e.xproperty.atom ==
					XInternAtom(dpy, "_NET_WORKAREA", False))
				print_workarea();
			break;
		case ClientMessage:
			if (e.xclient.message_type == wm_protocols &&
					(Atom)e.xclient.data.l[0] == wm_delete) {
				printf("close %s\n", e.xclient.window == main_win ?
					"main" : "dialog");
				XDestroyWindow(dpy, e.xclient.window);
				open--;
				XFlush(dpy);
			}
			break;
		default:
			break;
		}
	}
	return 0;
}
