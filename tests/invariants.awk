# What must hold for mwc's state at any moment, whatever led to it: a state file
# (MWC_TEST_STATE) in, one line per violation out, exit 1 if there is one. Not where
# windows are, only that what mwc says of them fits together. Windows are told apart
# by id, their dialogs' root by its id. The layout is checked on the first output only.
#
#   awk -f tests/invariants.awk STATE

function field(line, key,    n, i, parts) {
	n = split(line, parts, " ")
	for (i = 2; i <= n; i++)
		if (index(parts[i], key "=") == 1)
			return substr(parts[i], length(key) + 2)
	return ""
}

function bad(msg) {
	print "violation: " msg
	broken = 1
}

BEGIN {
	nkeys = split("active kbfocus min full hidden tiled tile_main shaded tab tab_hidden " \
		"visible desktop everywhere group sticky id root icon_shown icon_x icon_y", keys, " ")
}

# Numbers, not strings: "100" < "30" as strings.
$1 == "view" {
	n++
	app[n] = field($0, "app")
	for (k = 1; k <= nkeys; k++)
		v[n, keys[k]] = field($0, keys[k]) + 0
	split(field($0, "cell"), c, ",")
	cx[n] = c[1] + 0; cy[n] = c[2] + 0; cw[n] = c[3] + 0; ch[n] = c[4] + 0
	byid[v[n, "id"]] = n
	next
}
$1 == "desktop" { current = field($0, "current") + 0 }
$1 == "focus" { layer = field($0, "layer") + 0 }
$1 == "iconbox" { iconfocus = field($0, "focused") + 0; iconbox = field($0, "on") + 0 }
$1 == "tiling" {
	tiling = field($0, "on") + 0; gap = field($0, "gap") + 0
	split(field($0, "area"), a, ",")
	ax = a[1] + 0; ay = a[2] + 0; aw = a[3] + 0; ah = a[4] + 0
}
$1 == "seat" { seatkb = field($0, "keyboard") }
$1 == "output" && !ux_set {
	ux = field($0, "ux") + 0; uy = field($0, "uy") + 0
	uw = field($0, "uw") + 0; uh = field($0, "uh") + 0; ux_set = 1
}

END {
	active = 0; kb = 0
	for (i = 1; i <= n; i++) {
		if (v[i, "active"]) { active++; act = i }
		if (v[i, "kbfocus"]) { kb++; k = i }
	}
	# The focus: one window at most, the keys where it is, or with a layer surface, or
	# with nobody while the icon box has them.
	if (active > 1)
		bad(active " windows are active")
	if (kb > 1)
		bad(kb " windows have the keyboard")
	if (kb == 1 && !v[k, "active"])
		bad(app[k] " has the keyboard but isn't active")
	if (active == 1 && kb == 0 && !layer && !iconfocus)
		bad(app[act] " is active but the keyboard is with " seatkb)
	if (layer && kb > 0)
		bad("a layer surface has the keys and " app[k] " the keyboard")
	if (layer && seatkb != "other")
		bad("a layer surface has the keys, the seat's keyboard is with " seatkb)
	if (!layer && seatkb == "other")
		bad("the seat's keyboard is with a surface of no window, but no layer has the keys")
	if (iconfocus && active > 0)
		bad("the icon box has the focus and " app[act] " is active")

	for (i = 1; i <= n; i++) {
		r = byid[v[i, "root"]]
		here[i] = v[i, "desktop"] == current || v[i, "everywhere"]
		# In sight exactly when nothing hides it: not an icon, not a hidden tab, not
		# hidden with its window, here or everywhere.
		want = !v[i, "min"] && !v[i, "tab_hidden"] && !v[i, "hidden"] && here[i]
		if (v[i, "visible"] != want)
			bad(app[i] " visible=" v[i, "visible"] ", but min=" v[i, "min"] \
				" tab_hidden=" v[i, "tab_hidden"] " hidden=" v[i, "hidden"] \
				" desktop=" v[i, "desktop"] "/" current \
				" everywhere=" v[i, "everywhere"])
		if (v[i, "active"] && !v[i, "visible"])
			bad(app[i] " is active but out of sight")
		# Its icon: there for an icon of this desktop; with the icon box, some may be
		# scrolled out.
		wicon = v[i, "min"] && here[i]
		if (iconbox ? v[i, "icon_shown"] && !wicon : v[i, "icon_shown"] != wicon)
			bad(app[i] " icon_shown=" v[i, "icon_shown"] ", but min=" v[i, "min"] \
				" desktop=" v[i, "desktop"] "/" current)
		# Two icons in sight never in one slot: one would hide the other.
		if (!iconbox && v[i, "icon_shown"]) {
			slot = v[i, "icon_x"] "," v[i, "icon_y"]
			if (slot in icon_at)
				bad(app[i] "'s and " app[icon_at[slot]] "'s icons are both at " slot)
			icon_at[slot] = i
		}
		if (v[i, "full"] && v[i, "tiled"])
			bad(app[i] " is fullscreen and tiled")
		if (v[i, "shaded"] && (!v[i, "tiled"] || cw[i] <= 0 || ch[i] <= 0))
			bad(app[i] " is shaded without a cell")
		if (v[i, "shaded"] && v[i, "tile_main"])
			bad(app[i] " is shaded and the main window")
		if (v[i, "tiled"] && here[i] && (!v[i, "visible"] || v[i, "min"] || v[i, "full"]))
			bad(app[i] " is tiled here but out of sight, an icon or fullscreen")
		# A dialog: hidden only with a window, and on its window's desktop.
		if (v[i, "hidden"] && r == i)
			bad(app[i] " is hidden with its window but has none")
		if (r && r != i && !v[i, "everywhere"] && v[i, "desktop"] != v[r, "desktop"])
			bad(app[i] " is on desktop " v[i, "desktop"] ", its window " app[r] \
				" on " v[r, "desktop"])
		# A tab group: two windows at least, tabs 0 to n-1, one shown, all on one
		# desktop or all everywhere, all sticky or none.
		g = v[i, "group"]
		if (!g && (v[i, "tab"] >= 0 || v[i, "tab_hidden"]))
			bad(app[i] " is in no group but tab=" v[i, "tab"] " tab_hidden=" \
				v[i, "tab_hidden"])
		if (g) {
			size[g]++
			shown[g] += !v[i, "tab_hidden"]
			tabs[g, v[i, "tab"]]++
			if (!(g in first))
				first[g] = i
			f = first[g]
			if (v[i, "everywhere"] != v[f, "everywhere"] || v[i, "sticky"] != v[f, "sticky"] ||
					(!v[i, "everywhere"] && v[i, "desktop"] != v[f, "desktop"]))
				bad("group " g ": " app[i] " is on desktop " v[i, "desktop"] \
					" everywhere=" v[i, "everywhere"] " sticky=" v[i, "sticky"] \
					", " app[f] " on " v[f, "desktop"] " everywhere=" \
					v[f, "everywhere"] " sticky=" v[f, "sticky"])
		}
	}
	for (g in size) {
		if (size[g] < 2)
			bad("group " g " has " size[g] " window")
		if (shown[g] != 1)
			bad("group " g " shows " shown[g] " tabs")
		for (t = 0; t < size[g]; t++)
			if (tabs[g, t] != 1)
				bad("group " g " of " size[g] " has " tabs[g, t] + 0 " tab " t)
	}

	# The layout: the cells of what it lays out here don't overlap, are in the usable
	# area and fill the layout's area but for the gap: the main window on the right,
	# the stack beside it from top to bottom.
	if (!tiling)
		exit broken
	m = 0; mains = 0; open = 0
	for (i = 1; i <= n; i++) {
		if (!v[i, "tiled"] || !here[i])
			continue
		tl[++m] = i
		if (v[i, "tile_main"]) { mains++; main = i }
	}
	for (p = 1; p <= m; p++) {
		i = tl[p]
		if (cw[i] <= 0 || ch[i] <= 0)
			bad(app[i] " is tiled in an empty cell")
		if (cx[i] < ux || cy[i] < uy || cx[i] + cw[i] > ux + uw || cy[i] + ch[i] > uy + uh)
			bad(app[i] "'s cell " cx[i] "," cy[i] " " cw[i] "x" ch[i] \
				" is outside the usable area")
		for (q = p + 1; q <= m; q++) {
			j = tl[q]
			if (cx[i] < cx[j] + cw[j] && cx[j] < cx[i] + cw[i] &&
					cy[i] < cy[j] + ch[j] && cy[j] < cy[i] + ch[i])
				bad(app[i] "'s and " app[j] "'s cells overlap")
		}
		if (p == 1 || cx[i] < x0) x0 = cx[i]
		if (p == 1 || cy[i] < y0) y0 = cy[i]
		if (p == 1 || cx[i] + cw[i] > x1) x1 = cx[i] + cw[i]
		if (p == 1 || cy[i] + ch[i] > y1) y1 = cy[i] + ch[i]
	}
	if (m == 0)
		exit broken
	if (mains != 1)
		bad(mains " main windows in a layout of " m)
	if (x0 != ax + gap || y0 != ay + gap || x1 != ax + aw - gap || y1 != ay + ah - gap)
		bad("the cells cover " x0 "," y0 " to " x1 "," y1 ", the layout's area is " \
			ax "," ay " " aw "x" ah " with a gap of " gap)
	if (mains == 1 && (cx[main] + cw[main] != x1 || cy[main] != y0 ||
			cy[main] + ch[main] != y1))
		bad(app[main] ", the main window, isn't on the right at full height")
	# The stack, top to bottom: one column, a gap between, the last open one to the
	# bottom.
	s = 0
	for (p = 1; p <= m; p++)
		if (!v[tl[p], "tile_main"])
			st[++s] = tl[p]
	for (p = 2; p <= s; p++)
		for (q = p; q > 1 && cy[st[q]] < cy[st[q - 1]]; q--) {
			tmp = st[q]; st[q] = st[q - 1]; st[q - 1] = tmp
		}
	for (p = 1; p <= s; p++) {
		i = st[p]
		if (cx[i] != x0 || (mains == 1 && cx[i] + cw[i] + gap != cx[main]))
			bad(app[i] " is out of the stack's column")
		if (p == 1 ? cy[i] != y0 : cy[i] != cy[st[p - 1]] + ch[st[p - 1]] + gap)
			bad(app[i] " doesn't follow the one above it in the stack")
		if (!v[i, "shaded"])
			open = i
	}
	if (s > 0 && open && cy[st[s]] + ch[st[s]] != y1)
		bad("the stack ends at " cy[st[s]] + ch[st[s]] ", not at " y1)
	exit broken
}
