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
 *     ask_before=fullscreen|maximized  ask with a _NET_WM_STATE message, while unmapped
 *     ask_later=fullscreen|maximized   ask with one 2 s after mapping
 *     ask_after=fullscreen|maximized   ask with one right after XMapWindow, unsynced,
 *                         as xterm -fullscreen does
 *     strut=L,R,T,B       _NET_WM_STRUT_PARTIAL along the whole edge
 *     color=rrggbb        background
 *     dialog=NAME         also open a transient dialog with this WM_CLASS
 *     workarea=1          print the root's _NET_WORKAREA whenever it changes
 *     moveresize=1        don't map; ask for _NET_WM_MOVERESIZE, destroy the window
 *     override=late       create it normal, set override-redirect, then map
 *     override=clear      create it override-redirect, clear that, then map
 *     override=mapped     map it normal, set override-redirect afterwards
 *     clipboard=1         own CLIPBOARD and answer with 200 KB of text
 *     moveresize=2        map it, then ask for _NET_WM_MOVERESIZE with no button down
 *     move_press=1        a press asks for _NET_WM_MOVERESIZE, as toolkits do
 *     report=1            print _NET_FRAME_EXTENTS as it changes, and whether a Motif
 *                         window manager can be found, as XmIsMotifWMRunning looks
 *     moveto=X,Y          once mapped, XMoveWindow there
 *     iconic=1            WM_HINTS with the initial state iconic
 *     normalize=1         once mapped, WM_CHANGE_STATE to NormalState
 *     rehide=N            withdraw and map it again, N times; says "reshown N" each time,
 *                         and what WM_STATE then is
 *     rehide_ms=MS        wait that long before each, default 700
 *     rehide_drop=1       delete _NET_WM_STATE while withdrawn, as a client may
 *     script=+v,-h,...    once mapped, _NET_WM_STATE messages one at a time about the
 *                         vertical or horizontal axis; says "sent N" each time
 *     focus_main=1        once mapped, give the focus from the dialog to the main window
 *     motif_later=FUNCS   once mapped, _MOTIF_WM_HINTS with only the functions set
 *     hints_later=WxH     once mapped, the minimum and maximum size become this
 *     latin1_title=1      a WM_NAME with bytes that are not UTF-8
 *     dialog_ms=MS        wait that long before opening the dialog
 *     pulldown=1          a press posts an override-redirect menu and grabs the pointer,
 *                         as Motif does for press-drag-release; logs what follows
 */
#define _DEFAULT_SOURCE
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Display *dpy;
static Atom wm_delete, wm_protocols;

/* A _NET_WM_STATE message to add a state. */
static void ask_state(Window win, const char *state)
{
	bool max = !strcmp(state, "maximized");
	XEvent e = { 0 };
	e.xclient.type = ClientMessage;
	e.xclient.window = win;
	e.xclient.message_type = XInternAtom(dpy, "_NET_WM_STATE", False);
	e.xclient.format = 32;
	e.xclient.data.l[0] = 1;
	e.xclient.data.l[1] = XInternAtom(dpy, max ? "_NET_WM_STATE_MAXIMIZED_VERT" :
		"_NET_WM_STATE_FULLSCREEN", False);
	e.xclient.data.l[2] = max ? XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ",
		False) : 0;
	XSendEvent(dpy, DefaultRootWindow(dpy), False,
		SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

struct options {
	const char *class, *type, *state, *dialog, *gravity, *ask_before, *ask_after, *ask_later;
	int w, h, x, y;
	bool has_pos;
	int min_w, min_h, max_w, max_h, inc_w, inc_h, base_w, base_h;
	bool has_min, has_max, has_inc, has_base;
	int aspect_n, aspect_d;
	long motif_funcs, motif_decor;
	bool has_motif;
	bool no_input, workarea, clipboard, move_press, pulldown;
	int rehide, rehide_ms, dialog_ms, motif_later, later_w, later_h;
	bool rehide_drop, focus_main, has_motif_later, latin1_title;
	const char *script;
	int moveresize;
	bool has_moveto, iconic, normalize, report;
	int moveto_x, moveto_y;
	const char *override;
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
	if (o->move_press || o->pulldown)
		a.event_mask |= ButtonPressMask | ButtonReleaseMask;
	if (o->report)
		a.event_mask |= PropertyChangeMask;
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

	if (o->no_input || o->iconic) {
		XWMHints wh = { 0 };
		if (o->no_input) {
			wh.flags |= InputHint;
			wh.input = False;
		}
		if (o->iconic) {
			wh.flags |= StateHint;
			wh.initial_state = IconicState;
		}
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
	return win;
}

static void set_override_redirect(Window win, bool on)
{
	XSetWindowAttributes a = { .override_redirect = on };
	XChangeWindowAttributes(dpy, win, CWOverrideRedirect, &a);
	XSync(dpy, False);
}

/* What a client in a toolkit does for a window-manager-driven move. */
static void ask_moveresize(Window win)
{
	XEvent e = { 0 };
	e.xclient.type = ClientMessage;
	e.xclient.window = win;
	e.xclient.message_type = XInternAtom(dpy, "_NET_WM_MOVERESIZE", False);
	e.xclient.format = 32;
	e.xclient.data.l[0] = 10;
	e.xclient.data.l[1] = 10;
	e.xclient.data.l[2] = 8; /* _NET_WM_MOVERESIZE_MOVE */
	e.xclient.data.l[3] = 1;
	e.xclient.data.l[4] = 1;
	XSendEvent(dpy, DefaultRootWindow(dpy), False,
		SubstructureRedirectMask | SubstructureNotifyMask, &e);
	XSync(dpy, False);
}

static void answer_selection(XSelectionRequestEvent *r)
{
	static char *text;
	const size_t size = 200000;
	if (!text) {
		text = malloc(size);
		memset(text, 'x', size);
	}
	Atom targets = XInternAtom(dpy, "TARGETS", False);
	Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	XSelectionEvent s = {
		.type = SelectionNotify, .display = dpy, .requestor = r->requestor,
		.selection = r->selection, .target = r->target, .property = r->property,
		.time = r->time,
	};
	if (r->target == targets) {
		Atom list[] = { targets, utf8 };
		XChangeProperty(dpy, r->requestor, r->property, XA_ATOM, 32,
			PropModeReplace, (unsigned char *)list, 2);
	} else if (r->target == utf8) {
		XChangeProperty(dpy, r->requestor, r->property, utf8, 8, PropModeReplace,
			(unsigned char *)text, size);
	} else {
		s.property = None;
	}
	XSendEvent(dpy, r->requestor, False, 0, (XEvent *)&s);
	XFlush(dpy);
}

static void print_frame_extents(Window win)
{
	Atom type;
	int format;
	unsigned long n, after;
	unsigned char *data = NULL;
	if (XGetWindowProperty(dpy, win, XInternAtom(dpy, "_NET_FRAME_EXTENTS", False), 0, 4,
			False, XA_CARDINAL, &type, &format, &n, &after, &data) == Success &&
			data && n == 4) {
		long *e = (long *)data;
		printf("frame-extents %ld %ld %ld %ld\n", e[0], e[1], e[2], e[3]);
	}
	if (data)
		XFree(data);
}

/* What a toolkit asks before it believes _NET_FRAME_EXTENTS. */
static void print_supported(void)
{
	Atom type, wanted = XInternAtom(dpy, "_NET_FRAME_EXTENTS", False);
	int format;
	unsigned long n, after;
	unsigned char *data = NULL;
	bool found = false;
	if (XGetWindowProperty(dpy, DefaultRootWindow(dpy),
			XInternAtom(dpy, "_NET_SUPPORTED", False), 0, 1024, False, XA_ATOM, &type,
			&format, &n, &after, &data) == Success && data) {
		for (unsigned long i = 0; i < n; i++)
			found |= ((Atom *)data)[i] == wanted;
	}
	if (data)
		XFree(data);
	printf("supported frame extents %d\n", found);
}

static void print_motif_wm(void)
{
	Atom info = XInternAtom(dpy, "_MOTIF_WM_INFO", False), type;
	int format;
	unsigned long n, after;
	unsigned char *data = NULL;
	bool running = false;
	if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), info, 0, 2, False, info, &type,
			&format, &n, &after, &data) == Success && data && n == 2) {
		Window wm = ((long *)data)[1], root, parent, *kids = NULL;
		unsigned int count = 0;
		if (XQueryTree(dpy, DefaultRootWindow(dpy), &root, &parent, &kids, &count)) {
			for (unsigned int i = 0; i < count; i++)
				running |= kids[i] == wm;
			if (kids)
				XFree(kids);
		}
	}
	if (data)
		XFree(data);
	printf("motif-wm %s\n", running ? "running" : "absent");
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
		else if (!strcmp(k, "rehide"))
			o.rehide = atoi(v);
		else if (!strcmp(k, "rehide_ms"))
			o.rehide_ms = atoi(v);
		else if (!strcmp(k, "rehide_drop"))
			o.rehide_drop = atoi(v);
		else if (!strcmp(k, "script"))
			o.script = v;
		else if (!strcmp(k, "focus_main"))
			o.focus_main = atoi(v);
		else if (!strcmp(k, "motif_later")) {
			o.motif_later = atoi(v);
			o.has_motif_later = true;
		} else if (!strcmp(k, "hints_later"))
			sscanf(v, "%dx%d", &o.later_w, &o.later_h);
		else if (!strcmp(k, "latin1_title"))
			o.latin1_title = atoi(v);
		else if (!strcmp(k, "dialog_ms"))
			o.dialog_ms = atoi(v);
		else if (!strcmp(k, "ask_before"))
			o.ask_before = v;
		else if (!strcmp(k, "ask_later"))
			o.ask_later = v;
		else if (!strcmp(k, "ask_after"))
			o.ask_after = v;
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
		else if (!strcmp(k, "moveresize"))
			o.moveresize = atoi(v);
		else if (!strcmp(k, "clipboard"))
			o.clipboard = atoi(v);
		else if (!strcmp(k, "report"))
			o.report = atoi(v);
		else if (!strcmp(k, "moveto"))
			o.has_moveto = sscanf(v, "%d,%d", &o.moveto_x, &o.moveto_y) == 2;
		else if (!strcmp(k, "iconic"))
			o.iconic = atoi(v);
		else if (!strcmp(k, "normalize"))
			o.normalize = atoi(v);
		else if (!strcmp(k, "move_press"))
			o.move_press = atoi(v);
		else if (!strcmp(k, "pulldown"))
			o.pulldown = atoi(v);
		else if (!strcmp(k, "override"))
			o.override = v;
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
	if (o.moveresize == 1) {
		XSync(dpy, False);
		usleep(300000);
		ask_moveresize(main_win);
		usleep(300000);
		XDestroyWindow(dpy, main_win);
		XSync(dpy, False);
		printf("done\n");
		for (;;)
			pause();
	}
	if (o.override && !strcmp(o.override, "late"))
		set_override_redirect(main_win, true);
	if (o.override && !strcmp(o.override, "clear")) {
		set_override_redirect(main_win, true);
		usleep(100000);
		set_override_redirect(main_win, false);
	}
	if (o.ask_before) {
		/* It is known to the window manager by now, and still not mapped. */
		XSync(dpy, False);
		usleep(300000);
		ask_state(main_win, o.ask_before);
		XSync(dpy, False);
		usleep(300000);
	}
	XMapWindow(dpy, main_win);
	if (o.ask_after) {
		ask_state(main_win, o.ask_after);
		XFlush(dpy);
	}
	if (o.has_moveto) {
		XSync(dpy, False);
		usleep(500000);
		XMoveWindow(dpy, main_win, o.moveto_x, o.moveto_y);
		XSync(dpy, False);
		printf("moved\n");
	}
	if (o.normalize) {
		XSync(dpy, False);
		usleep(800000);
		XEvent e = { 0 };
		e.xclient.type = ClientMessage;
		e.xclient.window = main_win;
		e.xclient.message_type = XInternAtom(dpy, "WM_CHANGE_STATE", False);
		e.xclient.format = 32;
		e.xclient.data.l[0] = NormalState;
		XSendEvent(dpy, DefaultRootWindow(dpy), False,
			SubstructureRedirectMask | SubstructureNotifyMask, &e);
		XSync(dpy, False);
		printf("normalized\n");
	}
	if (o.moveresize == 2) {
		XSync(dpy, False);
		usleep(500000);
		ask_moveresize(main_win);
		printf("asked\n");
	}
	if (o.override && !strcmp(o.override, "mapped")) {
		XSync(dpy, False);
		usleep(500000);
		set_override_redirect(main_win, true);
		/* wlroots hears of it with the next configure. */
		XMoveWindow(dpy, main_win, o.x + 1, o.y);
		XSync(dpy, False);
	}
	Window dialog_win = None;
	if (o.dialog) {
		if (o.dialog_ms) {
			XSync(dpy, False);
			usleep(o.dialog_ms * 1000);
		}
		struct options d = o;
		d.has_pos = d.has_min = d.has_max = d.has_motif = false;
		d.type = NULL;
		d.w = 120;
		d.h = 80;
		d.x = d.y = 0;
		dialog_win = create_window(&d, o.dialog, main_win);
		XMapWindow(dpy, dialog_win);
	}
	if (o.clipboard) {
		XSync(dpy, False);
		usleep(500000);
		XSetSelectionOwner(dpy, XInternAtom(dpy, "CLIPBOARD", False), main_win,
			CurrentTime);
		XSync(dpy, False);
		printf("owner\n");
	}
	if (o.ask_later) {
		XSync(dpy, False);
		usleep(2000000);
		ask_state(main_win, o.ask_later);
		XSync(dpy, False);
		printf("asked\n");
	}
	if (o.latin1_title)
		XStoreName(dpy, main_win, "M\xfcller \xc4rger");
	if (o.script) {
		int sent = 0;
		for (const char *s = o.script; s[0] && s[1]; s += s[2] ? 3 : 2) {
			XSync(dpy, False);
			usleep(600000);
			XEvent e = { 0 };
			e.xclient.type = ClientMessage;
			e.xclient.window = main_win;
			e.xclient.message_type = XInternAtom(dpy, "_NET_WM_STATE", False);
			e.xclient.format = 32;
			e.xclient.data.l[0] = s[0] == '+';
			e.xclient.data.l[1] = XInternAtom(dpy, s[1] == 'v' ?
				"_NET_WM_STATE_MAXIMIZED_VERT" : "_NET_WM_STATE_MAXIMIZED_HORZ", False);
			XSendEvent(dpy, DefaultRootWindow(dpy), False,
				SubstructureRedirectMask | SubstructureNotifyMask, &e);
			XSync(dpy, False);
			printf("sent %d\n", ++sent);
		}
	}
	if (o.focus_main) {
		XSync(dpy, False);
		usleep(800000);
		XSetInputFocus(dpy, main_win, RevertToParent, CurrentTime);
		XSync(dpy, False);
		printf("focused main\n");
	}
	if (o.has_motif_later) {
		XSync(dpy, False);
		usleep(500000);
		long hints[5] = { 1 /* functions only */, o.motif_later, 0, 0, 0 };
		Atom m = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
		XChangeProperty(dpy, main_win, m, m, 32, PropModeReplace,
			(unsigned char *)hints, 5);
		XSync(dpy, False);
		printf("motif set\n");
	}
	if (o.later_w) {
		XSync(dpy, False);
		usleep(800000);
		XSizeHints sh = { .flags = PMinSize | PMaxSize, .min_width = o.later_w,
			.min_height = o.later_h, .max_width = o.later_w, .max_height = o.later_h };
		XSetWMNormalHints(dpy, main_win, &sh);
		/* Something to commit, or the compositor never looks. */
		XSetWindowBackground(dpy, main_win, 0x204060);
		XClearWindow(dpy, main_win);
		XSync(dpy, False);
		printf("hints set\n");
	}
	for (int i = 0; i < o.rehide; i++) {
		XSync(dpy, False);
		usleep((o.rehide_ms ? o.rehide_ms : 700) * 1000);
		XWithdrawWindow(dpy, main_win, DefaultScreen(dpy));
		if (o.rehide_drop)
			XDeleteProperty(dpy, main_win, XInternAtom(dpy, "_NET_WM_STATE", False));
		XSync(dpy, False);
		usleep(400000);
		XMapWindow(dpy, main_win);
		XSync(dpy, False);
		usleep(300000);
		Atom wm_state = XInternAtom(dpy, "WM_STATE", False), type;
		int format;
		unsigned long n, after;
		unsigned char *data = NULL;
		long state = -1;
		if (XGetWindowProperty(dpy, main_win, wm_state, 0, 2, False, wm_state, &type,
				&format, &n, &after, &data) == Success && data && n) {
			state = *(long *)data;
		}
		if (data)
			XFree(data);
		printf("reshown %d\nwm_state %ld\n", i + 1, state);
	}
	XFlush(dpy);
	printf("up\n");

	int open = dialog_win ? 2 : 1;
	Window menu_win = None;
	while (open > 0) {
		XEvent e;
		XNextEvent(dpy, &e);
		switch (e.type) {
		case ButtonPress:
			printf("press %s\n", e.xbutton.window == main_win ? "main" : "menu");
			if (o.move_press) {
				ask_moveresize(main_win);
			} else if (o.pulldown && !menu_win) {
				XSetWindowAttributes a = {
					.override_redirect = True,
					.background_pixel = 0xff0000,
					.event_mask = ButtonPressMask | ButtonReleaseMask |
						PointerMotionMask,
				};
				menu_win = XCreateWindow(dpy, DefaultRootWindow(dpy),
					e.xbutton.x_root, e.xbutton.y_root + 20, 120, 100, 0,
					CopyFromParent, InputOutput, CopyFromParent,
					CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
				XMapRaised(dpy, menu_win);
				XGrabPointer(dpy, menu_win, True, ButtonPressMask |
					ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
					GrabModeAsync, None, None, CurrentTime);
				XFlush(dpy);
				printf("posted\n");
			}
			break;
		case ButtonRelease:
			printf("release %s %d,%d\n", e.xbutton.window == main_win ? "main" :
				"menu", e.xbutton.x_root, e.xbutton.y_root);
			if (menu_win) {
				XUngrabPointer(dpy, CurrentTime);
				XUnmapWindow(dpy, menu_win);
				XFlush(dpy);
			}
			break;
		case MotionNotify:
			printf("motion %s %d,%d\n", e.xmotion.window == main_win ? "main" :
				"menu", e.xmotion.x_root, e.xmotion.y_root);
			break;
		case MapNotify:
			/* Mapped: the window manager is there for sure. Xwayland starts on the
			 * first connection, so it may not have been before. */
			if (o.report) {
				print_motif_wm();
				print_supported();
			}
			break;
		case FocusIn:
			printf("focus-in %s\n", e.xfocus.window == main_win ? "main" : "dialog");
			break;
		case SelectionRequest:
			answer_selection(&e.xselectionrequest);
			break;
		case PropertyNotify:
			if (o.report && e.xproperty.atom ==
					XInternAtom(dpy, "_NET_FRAME_EXTENTS", False))
				print_frame_extents(main_win);
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
