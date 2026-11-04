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
 *     moveresize=1        don't map; ask for _NET_WM_MOVERESIZE, destroy the window
 *     override=late       create it normal, set override-redirect, then map
 *     override=clear      create it override-redirect, clear that, then map
 *     override=mapped     map it normal, set override-redirect afterwards
 *     clipboard=1         own CLIPBOARD and answer with 200 KB of text
 *     moveresize=2        map it, then ask for _NET_WM_MOVERESIZE with no button down
 *     move_press=1        a press asks for _NET_WM_MOVERESIZE, as toolkits do
 *     moveto=X,Y          once mapped, XMoveWindow there
 *     iconic=1            WM_HINTS with the initial state iconic
 *     normalize=1         once mapped, WM_CHANGE_STATE to NormalState
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

struct options {
	const char *class, *type, *state, *dialog, *gravity;
	int w, h, x, y;
	bool has_pos;
	int min_w, min_h, max_w, max_h, inc_w, inc_h, base_w, base_h;
	bool has_min, has_max, has_inc, has_base;
	int aspect_n, aspect_d;
	long motif_funcs, motif_decor;
	bool has_motif;
	bool no_input, workarea, clipboard, move_press, pulldown;
	int moveresize;
	bool has_moveto, iconic, normalize;
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
		else if (!strcmp(k, "moveresize"))
			o.moveresize = atoi(v);
		else if (!strcmp(k, "clipboard"))
			o.clipboard = atoi(v);
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
	XMapWindow(dpy, main_win);
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
		case FocusIn:
			printf("focus-in %s\n", e.xfocus.window == main_win ? "main" : "dialog");
			break;
		case SelectionRequest:
			answer_selection(&e.xselectionrequest);
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
