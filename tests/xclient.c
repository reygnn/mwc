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
 *     aspect=N/D          fixed aspect ratio; N/D,N/D the smallest and largest
 *     type=dialog|utility|splash|dock|desktop|notification|toolbar
 *     motif=FUNCS,DECOR   _MOTIF_WM_HINTS
 *     input=0             WM_HINTS without input focus
 *     dialog_input=0|1    the same for the dialog alone, default as input=
 *     dialog_free=1       the dialog without WM_TRANSIENT_FOR: a second window of its own
 *     state=S[,S...]      initial _NET_WM_STATE, S: above|below|fullscreen|maximized|sticky
 *     ask_before=fullscreen|maximized  ask with a _NET_WM_STATE message, while unmapped
 *     ask_later=fullscreen|maximized   ask with one 2 s after mapping
 *     ask_later_after=FILE  that once FILE exists instead; retitled after it as below
 *     ask_after=fullscreen|maximized   ask with one right after XMapWindow, unsynced,
 *                         as xterm -fullscreen does
 *     strut=L,R,T,B       _NET_WM_STRUT_PARTIAL along the whole edge
 *     strut_flicker=N     once mapped, change the strut N times, the top one 10 px
 *                         more every other time, as fast as it can; says "flickered"
 *     color=rrggbb        background
 *     dialog=NAME         also open a transient dialog with this WM_CLASS
 *     workarea=1          print the root's _NET_WORKAREA whenever it changes
 *     netstate=1          print "net-state vert=V horz=H" whenever _NET_WM_STATE changes,
 *                         whether it says maximized vertically and horizontally, then
 *                         "net-more sticky=S shaded=R hidden=I"
 *     moveresize=1        don't map; ask for _NET_WM_MOVERESIZE, destroy the window
 *     override=late       create it normal, set override-redirect, then map
 *     override=clear      create it override-redirect, clear that, then map
 *     override=mapped     map it normal, set override-redirect afterwards
 *     override=unset_mapped  map it override-redirect, clear that once the file
 *                         override_after=FILE exists; says "override cleared"
 *     clipboard=1         own CLIPBOARD and answer with 200 KB of text
 *     moveresize=2        map it, then ask for _NET_WM_MOVERESIZE with no button down
 *     moveresize=3        the same 2 s after mapping, time to hold a button elsewhere
 *     moveresize_after=FILE  the same once FILE exists; retitled after it as below
 *     move_press=1        a press asks for _NET_WM_MOVERESIZE, as toolkits do
 *     report=1            print _NET_FRAME_EXTENTS as it changes, and whether a Motif
 *                         window manager can be found, as XmIsMotifWMRunning looks
 *     sizes=1             print "size WxH" for each ConfigureNotify of the main window
 *     moveto=X,Y          once mapped, XMoveWindow there
 *     grow=WxH            1 s after mapping, XResizeWindow to that size; says "grown"
 *     iconic=1            WM_HINTS with the initial state iconic
 *     normalize=1         once mapped, WM_CHANGE_STATE to NormalState; says "normalized"
 *     normalize_after=FILE  the same once FILE exists
 *     activate_after=FILE once FILE exists, _NET_ACTIVE_WINDOW as below
 *                         After these requests and focus_main the main window is titled
 *                         "request sent": by then the window manager has had the request,
 *                         X delivers its events in order. A test can poll for the title.
 *     activate_ms=MS      that long after mapping, _NET_ACTIVE_WINDOW for itself, as a
 *                         program does that wants to be in front; says "activate asked"
 *     iconify_ms=MS       that long after mapping (and the dialog), WM_CHANGE_STATE to
 *                         IconicState, as a program minimizing itself; says "iconified"
 *     iconify_dialog=1    that goes to the dialog instead of the main window
 *     rehide=N            withdraw and map it again, N times; says "reshown N" each time,
 *                         and what WM_STATE then is
 *     rehide_ms=MS        wait that long before each, default 700
 *     rehide_drop=1       delete _NET_WM_STATE while withdrawn, as a client may
 *     rehide_before=FILE  withdraw it once FILE exists, not after rehide_ms
 *     rehide_after=FILE   map it again once FILE exists, not after 400 ms
 *     rehide_title=TEXT   set WM_NAME to TEXT before mapping it again
 *     rehide_class=NAME   and WM_CLASS to NAME
 *     retitle_after=FILE  once mapped again and FILE exists, back to the title before
 *                         (title_later or the class), and to the class before
 *                         (class_later); says "retitled"
 *     script=+v,-h,...    once mapped, _NET_WM_STATE messages one at a time about the
 *                         vertical or horizontal axis, or s for sticky, r for shaded
 *                         (rolled up), i for hidden (an icon); says "sent N" each time
 *     focus_main=1        once mapped, give the focus from the dialog to the main window
 *     focus_main_after=FILE  the same once FILE exists
 *     focus_dialog_after=FILE  once FILE exists, give the focus to the dialog; then the
 *                         dialog is titled "request sent", as the main window above
 *     id_file=FILE        write the main window's id to FILE, in hex
 *     transient_from=FILE once FILE exists, WM_TRANSIENT_FOR to the window whose id it
 *                         holds (as id_file writes it); retitled after it as above
 *     instance_later=NAME  the same for the instance alone, the class kept
 *     class_later=NAME    once the file class_after=FILE exists, WM_CLASS becomes NAME;
 *                         says "class set"
 *     motif_later=FUNCS   once mapped, _MOTIF_WM_HINTS with only the functions set
 *     popup=X,Y           once mapped, an override-redirect window of 120x100 at X,Y,
 *                         as a menu opened from the keyboard; says "popup shown",
 *                         and "popup gone" once it is unmapped again
 *     popup_type=TYPE     its window type, as for type=; none: it wants the keyboard
 *     popup_ms=MS         wait that long before showing it, default 800
 *     popup_for=MS        and that long before unmapping it, default 1500
 *     decor_later=DECOR   once mapped, _MOTIF_WM_HINTS with only the decorations set;
 *                         says "decor set"
 *     decor_ms=MS         wait that long before that, default 500
 *     hints_later=WxH     once mapped, the minimum and maximum size become this
 *     hints_ms=MS         wait that long before that, default 800
 *     title_later=TEXT    2 s after mapping, a new WM_NAME
 *     latin1_title=1      a WM_NAME with bytes that are not UTF-8
 *     dialog_ms=MS        wait that long before opening the dialog
 *     dialog_after=FILE   open it once FILE exists
 *     withdraw_after=FILE once FILE exists, withdraw the main window; says "withdrawn"
 *     withdraw_later=FILE the same, but after the dialog and iconify_ms
 *     nomap=1             never map the main window, an owner kept out of sight
 *     urgent_after=FILE   once FILE exists, set XUrgencyHint in WM_HINTS; says "urgent"
 *     spoof_after=FILE    once FILE exists, send the root a synthetic ConfigureNotify
 *                         that says the main window is 1x1, as any client can; says
 *                         "spoofed"
 *     dialog_type=TYPE    the dialog's explicit window type, as for type=, also normal
 *     modal=1             WM_TAKE_FOCUS in WM_PROTOCOLS. Once focus_dialog_after has
 *                         focused the dialog, the focus offered or given to the main
 *                         window goes on to the dialog while that is mapped, as Java
 *                         does with a modal dialog open; says "passed", and the main
 *                         window is titled with one o per time
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
static Atom wm_delete, wm_protocols, wm_take_focus;

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
	const char *class, *type, *state, *dialog, *dialog_type, *gravity;
	const char *ask_before, *ask_after, *ask_later, *ask_later_after;
	int w, h, x, y;
	bool has_pos;
	int min_w, min_h, max_w, max_h, inc_w, inc_h, base_w, base_h;
	bool has_min, has_max, has_inc, has_base;
	int aspect_n, aspect_d, aspect_max_n, aspect_max_d;
	long motif_funcs, motif_decor;
	bool has_motif;
	int dialog_input;
	bool dialog_free;
	bool no_input, workarea, clipboard, move_press, pulldown, modal;
	int rehide, rehide_ms, dialog_ms, motif_later, later_w, later_h, hints_ms;
	const char *dialog_after, *withdraw_after, *withdraw_later, *urgent_after, *spoof_after;
	const char *normalize_after, *activate_after, *focus_main_after, *focus_dialog_after;
	const char *id_file, *transient_from;
	const char *class_later, *class_after, *moveresize_after, *override_after;
	const char *rehide_before, *rehide_after, *rehide_title, *rehide_class;
	const char *instance_later;
	const char *retitle_after;
	bool nomap;
	bool rehide_drop, focus_main, has_motif_later, latin1_title;
	int decor_later, decor_ms;
	bool has_decor_later;
	bool has_popup;
	int popup_x, popup_y, popup_ms, popup_for;
	const char *popup_type;
	const char *script, *title_later;
	int iconify_ms;
	bool iconify_dialog;
	int moveresize;
	bool has_moveto, iconic, normalize, report, netstate, sizes;
	int activate_ms;
	int moveto_x, moveto_y;
	int grow_w, grow_h;
	const char *override;
	long strut[4];
	bool has_strut;
	int strut_flicker;
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

static void set_strut(Window win, const struct options *o, long more_top)
{
	int sw = DisplayWidth(dpy, DefaultScreen(dpy));
	int sh_ = DisplayHeight(dpy, DefaultScreen(dpy));
	long s[12] = { o->strut[0], o->strut[1], o->strut[2] + more_top, o->strut[3],
		0, sh_ - 1, 0, sh_ - 1, 0, sw - 1, 0, sw - 1 };
	XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False),
		XA_CARDINAL, 32, PropModeReplace, (unsigned char *)s, 12);
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
	if (o->report || o->netstate)
		a.event_mask |= PropertyChangeMask;
	Window win = XCreateWindow(dpy, DefaultRootWindow(dpy), o->x, o->y, o->w, o->h,
		0, CopyFromParent, InputOutput, CopyFromParent,
		CWBackPixel | CWEventMask, &a);

	XClassHint ch = { (char *)class, (char *)class };
	XSetClassHint(dpy, win, &ch);
	XStoreName(dpy, win, class);
	Atom protocols[] = { wm_delete, wm_take_focus };
	XSetWMProtocols(dpy, win, protocols, o->modal ? 2 : 1);

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
		sh.min_aspect.x = o->aspect_n;
		sh.min_aspect.y = o->aspect_d;
		sh.max_aspect.x = o->aspect_max_n;
		sh.max_aspect.y = o->aspect_max_d;
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
		Atom a[8];
		int n = 0;
		char list[128];
		snprintf(list, sizeof(list), "%s", o->state);
		for (char *s = strtok(list, ","); s && n < 7; s = strtok(NULL, ",")) {
			if (strcmp(s, "maximized") == 0) {
				a[n++] = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
				a[n++] = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
				continue;
			}
			char name[64];
			snprintf(name, sizeof(name), "_NET_WM_STATE_%s", s);
			for (char *p = name + 14; *p; p++)
				*p = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
			a[n++] = XInternAtom(dpy, name, False);
		}
		XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_STATE", False),
			XA_ATOM, 32, PropModeReplace, (unsigned char *)a, n);
	}
	if (o->has_strut)
		set_strut(win, o, 0);
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

/* After a request: a title the window manager reads after it, see the usage. */
static void mark_request(Window win)
{
	XStoreName(dpy, win, "request sent");
	XSync(dpy, False);
}

/* After it, the main window's title says how many times. */
static void pass_to_dialog(Window main_win, Window dialog_win, int *passes)
{
	char title[64];
	XSetInputFocus(dpy, dialog_win, RevertToParent, CurrentTime);
	*passes += *passes < (int)sizeof(title) - 1;
	memset(title, 'o', *passes);
	title[*passes] = '\0';
	XStoreName(dpy, main_win, title);
	XSync(dpy, False);
	printf("passed\n");
	fflush(stdout);
}

/* The test says when, by creating it. */
static void wait_for_file(const char *path)
{
	while (access(path, F_OK) != 0)
		usleep(50000);
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

static void print_net_state(Window win)
{
	Atom type, vert = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False),
		horz = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False),
		sticky = XInternAtom(dpy, "_NET_WM_STATE_STICKY", False),
		shaded = XInternAtom(dpy, "_NET_WM_STATE_SHADED", False),
		hidden = XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", False);
	int format, v = 0, h = 0, st = 0, sh = 0, hi = 0;
	unsigned long n = 0, after;
	unsigned char *data = NULL;
	if (XGetWindowProperty(dpy, win, XInternAtom(dpy, "_NET_WM_STATE", False), 0, 32,
			False, XA_ATOM, &type, &format, &n, &after, &data) == Success && data) {
		for (unsigned long i = 0; i < n; i++) {
			v |= ((Atom *)data)[i] == vert;
			h |= ((Atom *)data)[i] == horz;
			st |= ((Atom *)data)[i] == sticky;
			sh |= ((Atom *)data)[i] == shaded;
			hi |= ((Atom *)data)[i] == hidden;
		}
	}
	if (data)
		XFree(data);
	printf("net-state vert=%d horz=%d\n", v, h);
	printf("net-more sticky=%d shaded=%d hidden=%d\n", st, sh, hi);
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
		else if (!strcmp(k, "aspect")) {
			if (sscanf(v, "%d/%d,%d/%d", &o.aspect_n, &o.aspect_d,
					&o.aspect_max_n, &o.aspect_max_d) != 4) {
				o.aspect_max_n = o.aspect_n;
				o.aspect_max_d = o.aspect_d;
			}
		}
		else if (!strcmp(k, "type"))
			o.type = v;
		else if (!strcmp(k, "rehide"))
			o.rehide = atoi(v);
		else if (!strcmp(k, "rehide_ms"))
			o.rehide_ms = atoi(v);
		else if (!strcmp(k, "rehide_drop"))
			o.rehide_drop = atoi(v);
		else if (!strcmp(k, "instance_later"))
			o.instance_later = v;
		else if (!strcmp(k, "rehide_before"))
			o.rehide_before = v;
		else if (!strcmp(k, "rehide_class"))
			o.rehide_class = v;
		else if (!strcmp(k, "rehide_after"))
			o.rehide_after = v;
		else if (!strcmp(k, "rehide_title"))
			o.rehide_title = v;
		else if (!strcmp(k, "retitle_after"))
			o.retitle_after = v;
		else if (!strcmp(k, "title_later"))
			o.title_later = v;
		else if (!strcmp(k, "script"))
			o.script = v;
		else if (!strcmp(k, "focus_main"))
			o.focus_main = atoi(v);
		else if (!strcmp(k, "motif_later")) {
			o.motif_later = atoi(v);
			o.has_motif_later = true;
		} else if (!strcmp(k, "decor_later")) {
			o.decor_later = atoi(v);
			o.has_decor_later = true;
		} else if (!strcmp(k, "popup"))
			o.has_popup = sscanf(v, "%d,%d", &o.popup_x, &o.popup_y) == 2;
		else if (!strcmp(k, "popup_type"))
			o.popup_type = v;
		else if (!strcmp(k, "popup_ms"))
			o.popup_ms = atoi(v);
		else if (!strcmp(k, "popup_for"))
			o.popup_for = atoi(v);
		else if (!strcmp(k, "decor_ms"))
			o.decor_ms = atoi(v);
		else if (!strcmp(k, "hints_later"))
			sscanf(v, "%dx%d", &o.later_w, &o.later_h);
		else if (!strcmp(k, "latin1_title"))
			o.latin1_title = atoi(v);
		else if (!strcmp(k, "hints_ms"))
			o.hints_ms = atoi(v);
		else if (!strcmp(k, "dialog_type"))
			o.dialog_type = v;
		else if (!strcmp(k, "dialog_ms"))
			o.dialog_ms = atoi(v);
		else if (!strcmp(k, "dialog_after"))
			o.dialog_after = v;
		else if (!strcmp(k, "withdraw_after"))
			o.withdraw_after = v;
		else if (!strcmp(k, "withdraw_later"))
			o.withdraw_later = v;
		else if (!strcmp(k, "nomap"))
			o.nomap = atoi(v);
		else if (!strcmp(k, "urgent_after"))
			o.urgent_after = v;
		else if (!strcmp(k, "spoof_after"))
			o.spoof_after = v;
		else if (!strcmp(k, "normalize_after"))
			o.normalize_after = v;
		else if (!strcmp(k, "activate_after"))
			o.activate_after = v;
		else if (!strcmp(k, "focus_main_after"))
			o.focus_main_after = v;
		else if (!strcmp(k, "class_later"))
			o.class_later = v;
		else if (!strcmp(k, "class_after"))
			o.class_after = v;
		else if (!strcmp(k, "moveresize_after"))
			o.moveresize_after = v;

		else if (!strcmp(k, "ask_before"))
			o.ask_before = v;
		else if (!strcmp(k, "ask_later"))
			o.ask_later = v;
		else if (!strcmp(k, "ask_later_after"))
			o.ask_later_after = v;
		else if (!strcmp(k, "focus_dialog_after"))
			o.focus_dialog_after = v;
		else if (!strcmp(k, "id_file"))
			o.id_file = v;
		else if (!strcmp(k, "transient_from"))
			o.transient_from = v;
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
		else if (!strcmp(k, "strut_flicker"))
			o.strut_flicker = atoi(v);
		else if (!strcmp(k, "workarea"))
			o.workarea = atoi(v);
		else if (!strcmp(k, "moveresize"))
			o.moveresize = atoi(v);
		else if (!strcmp(k, "clipboard"))
			o.clipboard = atoi(v);
		else if (!strcmp(k, "report"))
			o.report = atoi(v);
		else if (!strcmp(k, "sizes"))
			o.sizes = atoi(v);
		else if (!strcmp(k, "netstate"))
			o.netstate = atoi(v);
		else if (!strcmp(k, "moveto"))
			o.has_moveto = sscanf(v, "%d,%d", &o.moveto_x, &o.moveto_y) == 2;
		else if (!strcmp(k, "grow"))
			sscanf(v, "%dx%d", &o.grow_w, &o.grow_h);
		else if (!strcmp(k, "iconic"))
			o.iconic = atoi(v);
		else if (!strcmp(k, "normalize"))
			o.normalize = atoi(v);
		else if (!strcmp(k, "activate_ms"))
			o.activate_ms = atoi(v);
		else if (!strcmp(k, "iconify_ms"))
			o.iconify_ms = atoi(v);
		else if (!strcmp(k, "iconify_dialog"))
			o.iconify_dialog = atoi(v);
		else if (!strcmp(k, "move_press"))
			o.move_press = atoi(v);
		else if (!strcmp(k, "dialog_free"))
			o.dialog_free = atoi(v);
		else if (!strcmp(k, "dialog_input"))
			o.dialog_input = atoi(v) ? 1 : 2;
		else if (!strcmp(k, "modal"))
			o.modal = atoi(v);
		else if (!strcmp(k, "pulldown"))
			o.pulldown = atoi(v);
		else if (!strcmp(k, "override"))
			o.override = v;
		else if (!strcmp(k, "override_after"))
			o.override_after = v;
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
	wm_take_focus = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
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
	if (o.override && (!strcmp(o.override, "late") || !strcmp(o.override, "unset_mapped")))
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
	if (!o.nomap)
		XMapWindow(dpy, main_win);
	if (o.ask_after) {
		ask_state(main_win, o.ask_after);
		XFlush(dpy);
	}
	if (o.strut_flicker > 0 && o.has_strut) {
		XSync(dpy, False);
		usleep(300000);
		for (int i = 1; i <= o.strut_flicker; i++) {
			set_strut(main_win, &o, i % 2 ? 10 : 0);
			XSync(dpy, False);
		}
		printf("flickered\n");
		fflush(stdout);
	}
	if (o.has_moveto) {
		usleep(500000);
		XMoveWindow(dpy, main_win, o.moveto_x, o.moveto_y);
		XSync(dpy, False);
		printf("moved\n");
	}
	if (o.grow_w > 0) {
		XSync(dpy, False);
		usleep(1000000);
		XResizeWindow(dpy, main_win, o.grow_w, o.grow_h);
		XSync(dpy, False);
		printf("grown\n");
		fflush(stdout);
	}
	if (o.normalize || o.normalize_after) {
		XSync(dpy, False);
		if (o.normalize_after)
			wait_for_file(o.normalize_after);
		else
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
		mark_request(main_win);
		printf("normalized\n");
		fflush(stdout);
	}
	if (o.activate_ms > 0 || o.activate_after) {
		XSync(dpy, False);
		if (o.activate_after)
			wait_for_file(o.activate_after);
		else
			usleep(o.activate_ms * 1000);
		XEvent e = { 0 };
		e.xclient.type = ClientMessage;
		e.xclient.window = main_win;
		e.xclient.message_type = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
		e.xclient.format = 32;
		e.xclient.data.l[0] = 1; /* from an application */
		e.xclient.data.l[1] = CurrentTime;
		XSendEvent(dpy, DefaultRootWindow(dpy), False,
			SubstructureRedirectMask | SubstructureNotifyMask, &e);
		XSync(dpy, False);
		mark_request(main_win);
		printf("activate asked\n");
		fflush(stdout);
	}
	if (o.moveresize == 2 || o.moveresize == 3 || o.moveresize_after) {
		XSync(dpy, False);
		if (o.moveresize_after)
			wait_for_file(o.moveresize_after);
		else
			usleep(o.moveresize == 3 ? 2000000 : 500000);
		ask_moveresize(main_win);
		if (o.moveresize_after)
			mark_request(main_win);
		printf("asked\n");
	}
	if (o.override && !strcmp(o.override, "unset_mapped") && o.override_after) {
		XSync(dpy, False);
		wait_for_file(o.override_after);
		set_override_redirect(main_win, false);
		/* wlroots hears of it with the next configure. */
		XMoveWindow(dpy, main_win, o.x + 1, o.y);
		XSync(dpy, False);
		printf("override cleared\n");
		fflush(stdout);
	}
	if (o.override && !strcmp(o.override, "mapped")) {
		XSync(dpy, False);
		usleep(500000);
		set_override_redirect(main_win, true);
		/* wlroots hears of it with the next configure. */
		XMoveWindow(dpy, main_win, o.x + 1, o.y);
		XSync(dpy, False);
	}
	if (o.withdraw_after) {
		XSync(dpy, False);
		wait_for_file(o.withdraw_after);
		XWithdrawWindow(dpy, main_win, DefaultScreen(dpy));
		XSync(dpy, False);
		printf("withdrawn\n");
		fflush(stdout);
	}
	if (o.urgent_after) {
		XSync(dpy, False);
		wait_for_file(o.urgent_after);
		XWMHints *h = XGetWMHints(dpy, main_win);
		if (!h)
			h = XAllocWMHints();
		h->flags |= XUrgencyHint;
		XSetWMHints(dpy, main_win, h);
		XFree(h);
		XSync(dpy, False);
		printf("urgent\n");
		fflush(stdout);
	}
	if (o.spoof_after) {
		XSync(dpy, False);
		wait_for_file(o.spoof_after);
		XEvent e = { 0 };
		e.xconfigure.type = ConfigureNotify;
		e.xconfigure.event = DefaultRootWindow(dpy);
		e.xconfigure.window = main_win;
		e.xconfigure.width = e.xconfigure.height = 1;
		XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureNotifyMask, &e);
		XSync(dpy, False);
		printf("spoofed\n");
		fflush(stdout);
	}
	Window dialog_win = None;
	if (o.dialog) {
		if (o.dialog_ms) {
			XSync(dpy, False);
			usleep(o.dialog_ms * 1000);
		}
		if (o.dialog_after) {
			XSync(dpy, False);
			wait_for_file(o.dialog_after);
		}
		struct options d = o;
		d.has_pos = d.has_min = d.has_max = d.has_motif = false;
		d.type = o.dialog_type;
		if (o.dialog_input)
			d.no_input = o.dialog_input == 2;
		d.w = 120;
		d.h = 80;
		d.x = d.y = 0;
		dialog_win = create_window(&d, o.dialog, o.dialog_free ? None : main_win);
		XMapWindow(dpy, dialog_win);
	}
	if ((o.class_later || o.instance_later) && o.class_after) {
		XSync(dpy, False);
		wait_for_file(o.class_after);
		const char *c = o.class_later ? o.class_later : o.class;
		XClassHint ch = { (char *)(o.instance_later ? o.instance_later : c), (char *)c };
		XSetClassHint(dpy, main_win, &ch);
		XSync(dpy, False);
		printf("class set\n");
		fflush(stdout);
	}
	if (o.iconify_ms) {
		XSync(dpy, False);
		usleep(o.iconify_ms * 1000);
		XEvent e = { 0 };
		e.xclient.type = ClientMessage;
		e.xclient.window = o.iconify_dialog && dialog_win ? dialog_win : main_win;
		e.xclient.message_type = XInternAtom(dpy, "WM_CHANGE_STATE", False);
		e.xclient.format = 32;
		e.xclient.data.l[0] = IconicState;
		XSendEvent(dpy, DefaultRootWindow(dpy), False,
			SubstructureRedirectMask | SubstructureNotifyMask, &e);
		XSync(dpy, False);
		printf("iconified\n");
	}
	if (o.withdraw_later) {
		XSync(dpy, False);
		wait_for_file(o.withdraw_later);
		XWithdrawWindow(dpy, main_win, DefaultScreen(dpy));
		XSync(dpy, False);
		printf("withdrawn\n");
		fflush(stdout);
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
		if (o.ask_later_after)
			wait_for_file(o.ask_later_after);
		else
			usleep(2000000);
		ask_state(main_win, o.ask_later);
		XSync(dpy, False);
		if (o.ask_later_after)
			mark_request(main_win);
		printf("asked\n");
	}
	if (o.title_later) {
		XSync(dpy, False);
		usleep(2000000);
		XStoreName(dpy, main_win, o.title_later);
		XSync(dpy, False);
		printf("renamed\n");
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
			e.xclient.data.l[1] = XInternAtom(dpy,
				s[1] == 'v' ? "_NET_WM_STATE_MAXIMIZED_VERT" :
				s[1] == 's' ? "_NET_WM_STATE_STICKY" :
				s[1] == 'r' ? "_NET_WM_STATE_SHADED" :
				s[1] == 'i' ? "_NET_WM_STATE_HIDDEN" :
				"_NET_WM_STATE_MAXIMIZED_HORZ", False);
			XSendEvent(dpy, DefaultRootWindow(dpy), False,
				SubstructureRedirectMask | SubstructureNotifyMask, &e);
			XSync(dpy, False);
			printf("sent %d\n", ++sent);
		}
	}
	if (o.focus_main || o.focus_main_after) {
		XSync(dpy, False);
		if (o.focus_main_after)
			wait_for_file(o.focus_main_after);
		else
			usleep(800000);
		XSetInputFocus(dpy, main_win, RevertToParent, CurrentTime);
		XSync(dpy, False);
		mark_request(main_win);
		printf("focused main\n");
		fflush(stdout);
	}
	bool prefer_dialog = false;
	unsigned long modal_from = 0;
	if (o.id_file) {
		char tmp[512];
		snprintf(tmp, sizeof(tmp), "%s.tmp", o.id_file);
		FILE *f = fopen(tmp, "w");
		if (f) {
			fprintf(f, "%lx\n", (unsigned long)main_win);
			fclose(f);
			rename(tmp, o.id_file); /* whole, when the test sees it */
		}
	}
	if (o.transient_from) {
		XSync(dpy, False);
		wait_for_file(o.transient_from);
		unsigned long id = 0;
		FILE *f = fopen(o.transient_from, "r");
		if (f) {
			if (fscanf(f, "%lx", &id) != 1)
				id = 0;
			fclose(f);
		}
		XSetTransientForHint(dpy, main_win, (Window)id);
		mark_request(main_win);
		printf("transient set\n");
		fflush(stdout);
	}
	if (o.focus_dialog_after && dialog_win) {
		XSync(dpy, False);
		wait_for_file(o.focus_dialog_after);
		/* What the server sent before it read this request is no answer. */
		modal_from = NextRequest(dpy);
		XSetInputFocus(dpy, dialog_win, RevertToParent, CurrentTime);
		XSync(dpy, False);
		prefer_dialog = true;
		mark_request(dialog_win);
		printf("focused dialog\n");
		fflush(stdout);
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
	if (o.has_decor_later) {
		XSync(dpy, False);
		usleep((o.decor_ms ? o.decor_ms : 500) * 1000);
		long hints[5] = { 2 /* decorations only */, 0, o.decor_later, 0, 0 };
		Atom m = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
		XChangeProperty(dpy, main_win, m, m, 32, PropModeReplace,
			(unsigned char *)hints, 5);
		XSync(dpy, False);
		printf("decor set\n");
	}
	if (o.has_popup) {
		XSync(dpy, False);
		usleep((o.popup_ms ? o.popup_ms : 800) * 1000);
		XSetWindowAttributes a = { .override_redirect = True,
			.background_pixel = 0x00ff00 };
		Window popup = XCreateWindow(dpy, DefaultRootWindow(dpy), o.popup_x,
			o.popup_y, 120, 100, 0, CopyFromParent, InputOutput, CopyFromParent,
			CWOverrideRedirect | CWBackPixel, &a);
		if (o.popup_type) {
			char name[64];
			snprintf(name, sizeof(name), "_NET_WM_WINDOW_TYPE_%s", o.popup_type);
			for (char *p = name + 20; *p; p++)
				*p = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
			set_atom_list(popup, "_NET_WM_WINDOW_TYPE", name);
		}
		XMapRaised(dpy, popup);
		XSync(dpy, False);
		printf("popup shown\n");
		usleep((o.popup_for ? o.popup_for : 1500) * 1000);
		XUnmapWindow(dpy, popup);
		XSync(dpy, False);
		printf("popup gone\n");
	}
	if (o.later_w) {
		XSync(dpy, False);
		usleep((o.hints_ms ? o.hints_ms : 800) * 1000);
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
		if (o.rehide_before)
			wait_for_file(o.rehide_before);
		else
			usleep((o.rehide_ms ? o.rehide_ms : 700) * 1000);
		XWithdrawWindow(dpy, main_win, DefaultScreen(dpy));
		if (o.rehide_drop)
			XDeleteProperty(dpy, main_win, XInternAtom(dpy, "_NET_WM_STATE", False));
		XSync(dpy, False);
		if (o.rehide_after)
			wait_for_file(o.rehide_after);
		else
			usleep(400000);
		if (o.rehide_title)
			XStoreName(dpy, main_win, o.rehide_title);
		if (o.rehide_class) {
			XClassHint ch = { (char *)o.rehide_class, (char *)o.rehide_class };
			XSetClassHint(dpy, main_win, &ch);
		}
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
		fflush(stdout);
		if ((o.rehide_title || o.rehide_class) && o.retitle_after) {
			wait_for_file(o.retitle_after);
			if (o.rehide_title)
				XStoreName(dpy, main_win, o.title_later ? o.title_later : o.class);
			if (o.rehide_class) {
				const char *c = o.class_later ? o.class_later : o.class;
				XClassHint ch = { (char *)c, (char *)c };
				XSetClassHint(dpy, main_win, &ch);
			}
			XSync(dpy, False);
			printf("retitled\n");
			fflush(stdout);
		}
	}
	XFlush(dpy);
	printf("up\n");

	int open = dialog_win ? 2 : 1;
	Window menu_win = None;
	bool dialog_mapped = false;
	int passes = 0;
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
		case UnmapNotify:
			if (dialog_win && e.xunmap.window == dialog_win)
				dialog_mapped = false;
			break;
		case MapNotify:
			/* Mapped: the window manager is there for sure. Xwayland starts on the
			 * first connection, so it may not have been before. */
			if (o.report) {
				print_motif_wm();
				print_supported();
			}
			if (dialog_win && e.xmap.window == dialog_win)
				dialog_mapped = true;
			break;
		case ConfigureNotify:
			if (o.sizes && e.xconfigure.window == main_win)
				printf("size %dx%d\n", e.xconfigure.width, e.xconfigure.height);
			fflush(stdout);
			break;
		case FocusIn:
			printf("focus-in %s\n", e.xfocus.window == main_win ? "main" : "dialog");
			if (o.modal && prefer_dialog && dialog_mapped &&
					e.xfocus.window == main_win &&
					e.xfocus.mode == NotifyNormal &&
					e.xfocus.detail != NotifyPointer &&
					e.xfocus.serial >= modal_from)
				pass_to_dialog(main_win, dialog_win, &passes);
			break;
		case SelectionRequest:
			answer_selection(&e.xselectionrequest);
			break;
		case PropertyNotify:
			if (o.report && e.xproperty.atom ==
					XInternAtom(dpy, "_NET_FRAME_EXTENTS", False))
				print_frame_extents(main_win);
			if (o.netstate && e.xproperty.window == main_win && e.xproperty.atom ==
					XInternAtom(dpy, "_NET_WM_STATE", False))
				print_net_state(main_win);
			if (o.workarea && e.xproperty.atom ==
					XInternAtom(dpy, "_NET_WORKAREA", False))
				print_workarea();
			break;
		case ClientMessage:
			if (e.xclient.message_type == wm_protocols &&
					(Atom)e.xclient.data.l[0] == wm_take_focus) {
				if (prefer_dialog && dialog_mapped)
					pass_to_dialog(main_win, dialog_win, &passes);
				else
					XSetInputFocus(dpy, e.xclient.window, RevertToParent,
						CurrentTime);
			}
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
