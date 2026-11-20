# What must hold for mwc's state at any moment, whatever led to it: a state file
# (MWC_TEST_STATE) in, one line per violation out, exit 1 if there is one. Not where
# windows are, only that what mwc says of them fits together.
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

$1 == "view" {
	n++
	app[n] = field($0, "app")
	for (k = 1; k <= nkeys; k++)
		v[n, keys[k]] = field($0, keys[k])
	split(field($0, "cell"), c, ",")
	cx[n] = c[1]; cy[n] = c[2]; cw[n] = c[3]; ch[n] = c[4]
	next
}
$1 == "desktop" { current = field($0, "current") }
$1 == "focus" { layer = field($0, "layer") }
$1 == "iconbox" { iconfocus = field($0, "focused") }
$1 == "tiling" { tiling = field($0, "on"); gap = field($0, "gap")
	split(field($0, "area"), ta, ",") }
$1 == "seat" { seatkb = field($0, "keyboard") }
$1 == "output" && !ux_set { ux = field($0, "ux"); uy = field($0, "uy");
	uw = field($0, "uw"); uh = field($0, "uh"); ux_set = 1 }

BEGIN {
	nkeys = split("active kbfocus min full hidden tiled shaded tab tab_hidden " \
		"visible desktop everywhere group sticky root", keys, " ")
}

END {
	active = 0; kb = 0
	for (i = 1; i <= n; i++) {
		if (v[i, "active"] == 1) { active++; a = i }
		if (v[i, "kbfocus"] == 1) { kb++; k = i }
	}
	# The focus: one window at most, and the keys where it is, or with a panel, a
	# popup or the icon box.
	if (active > 1)
		bad(active " windows are active")
	if (kb > 1)
		bad(kb " windows have the keyboard")
	if (kb == 1 && v[k, "active"] != 1)
		bad(app[k] " has the keyboard but isn't active")
	if (active == 1 && kb == 0 && layer != 1 && iconfocus != 1 && seatkb != "other")
		bad(app[a] " is active but the keyboard is with " seatkb)
	if (iconfocus == 1 && kb > 0)
		bad("the icon box has the focus and " app[k] " the keyboard")

	for (i = 1; i <= n; i++) {
		# In sight exactly when nothing hides it: not an icon, not a hidden tab, not
		# hidden with its window, here or everywhere.
		here = v[i, "desktop"] == current || v[i, "everywhere"] == 1
		want = v[i, "min"] == 0 && v[i, "tab_hidden"] == 0 && v[i, "hidden"] == 0 && here
		if (v[i, "visible"] != want)
			bad(app[i] " visible=" v[i, "visible"] ", but min=" v[i, "min"] \
				" tab_hidden=" v[i, "tab_hidden"] " hidden=" v[i, "hidden"] \
				" desktop=" v[i, "desktop"] "/" current \
				" everywhere=" v[i, "everywhere"])
		if (v[i, "active"] == 1 && v[i, "visible"] != 1)
			bad(app[i] " is active but out of sight")
		if (v[i, "full"] == 1 && v[i, "tiled"] == 1)
			bad(app[i] " is fullscreen and tiled")
		if (v[i, "shaded"] == 1 && (v[i, "tiled"] != 1 || cw[i] <= 0 || ch[i] <= 0))
			bad(app[i] " is shaded without a cell")
		# Sticky goes for its dialogs as well.
		for (j = 1; j <= n; j++)
			if (app[j] == v[i, "root"] && v[j, "sticky"] == 1 &&
					v[i, "everywhere"] != 1)
				bad(app[i] " belongs to sticky " app[j] " but isn't everywhere")
		# A tab group: one tab shown, all on one desktop or all everywhere.
		g = v[i, "group"]
		if (g > 0) {
			shown[g] += v[i, "tab_hidden"] == 0
			if (!(g in gdesk)) {
				gdesk[g] = v[i, "desktop"]; gevery[g] = v[i, "everywhere"]
			} else if (gdesk[g] != v[i, "desktop"] || gevery[g] != v[i, "everywhere"])
				bad("group " g ": " app[i] " is on desktop " v[i, "desktop"] \
					"/" v[i, "everywhere"] ", the group on " gdesk[g] "/" gevery[g])
		}
	}
	for (g in shown)
		if (shown[g] != 1)
			bad("group " g " shows " shown[g] " tabs")

	# The layout: the cells of what it lays out here don't overlap and are in the
	# usable area.
	if (tiling == 1) {
		m = 0
		for (i = 1; i <= n; i++)
			if (v[i, "tiled"] == 1 && v[i, "visible"] == 1)
				t[++m] = i
		for (p = 1; p <= m; p++) {
			i = t[p]
			if (cw[i] <= 0 || ch[i] <= 0)
				bad(app[i] " is tiled in an empty cell")
			if (cx[i] < ux || cy[i] < uy || cx[i] + cw[i] > ux + uw ||
					cy[i] + ch[i] > uy + uh)
				bad(app[i] "'s cell " cx[i] "," cy[i] " " cw[i] "x" ch[i] \
					" is outside the usable area")
			for (q = p + 1; q <= m; q++) {
				j = t[q]
				if (cx[i] < cx[j] + cw[j] && cx[j] < cx[i] + cw[i] &&
						cy[i] < cy[j] + ch[j] && cy[j] < cy[i] + ch[i])
					bad(app[i] "'s and " app[j] "'s cells overlap")
			}
			if (p == 1 || cx[i] < x0) x0 = cx[i]
			if (p == 1 || cy[i] < y0) y0 = cy[i]
			if (p == 1 || cx[i] + cw[i] > x1) x1 = cx[i] + cw[i]
			if (p == 1 || cy[i] + ch[i] > y1) y1 = cy[i] + ch[i]
		}
		# And fill its area, but for the gap around them.
		if (m > 0 && (x0 != ta[1] + gap || y0 != ta[2] + gap ||
				x1 != ta[1] + ta[3] - gap || y1 != ta[2] + ta[4] - gap))
			bad("the cells cover " x0 "," y0 " to " x1 "," y1 ", the layout's area is " \
				ta[1] "," ta[2] " " ta[3] "x" ta[4] " with a gap of " gap)
	}
	exit broken
}
