#!/bin/sh
# Moves a headless Ubuntu 26.04 server from mwm (X11, xrdp + xorgxrdp) to mwc
# (Wayland) with wayrdp as the RDP server. The steps of docs/WAYLAND-RDP.md.
#
#   sh wayland-rdp-upgrade.sh            sections 2-7: mwc and wayrdp on port
#                                        3390, the old xrdp stays on 3389
#   sh wayland-rdp-upgrade.sh --switch   section 8: xrdp off, wayrdp on 3389
#   sh wayland-rdp-upgrade.sh --purge    section 9: remove xrdp, X11 packages
#                                        and files
#
# Run as the user who logs in through RDP, not as root; it uses sudo where
# needed. Can be run again: it only adds what is missing.
#
#   MWC_SIZE=1920x1080   size of the headless output before a client asks
#   MWC_KBD=de           keyboard layout for wayrdp, the notebook's
#   MWC_SRC=~/mwc        where mwc's source goes
#   WAYRDP_SRC=~/wayrdp  where wayrdp's source goes
#   WAYRDP_AUTH=nla      nla (an RDP password of its own) or pam (the Linux one)
set -eu

SIZE=${MWC_SIZE:-1920x1080}
KBD=${MWC_KBD:-de}
SRC=${MWC_SRC:-$HOME/mwc}
WSRC=${WAYRDP_SRC:-$HOME/wayrdp}
AUTH=${WAYRDP_AUTH:-nla}
UNITS=$HOME/.config/systemd/user

say() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" != 0 ] || die "run this as your own user, not as root"
case $AUTH in nla|pam) ;; *) die "WAYRDP_AUTH is nla or pam, not $AUTH" ;; esac

switch() {
	say "xrdp off, wayrdp on 127.0.0.1:3389"
	[ -f "$UNITS/wayrdp.service" ] || die "no wayrdp.service yet: run without options first"
	systemctl --user is-active --quiet wayrdp ||
		die "wayrdp is not running; fix that first (journalctl --user -u wayrdp)"
	sudo systemctl disable --now xrdp xrdp-sesman 2>/dev/null || :
	sed -i 's| 127.0.0.1 3390$| 127.0.0.1|' "$UNITS/wayrdp.service"
	systemctl --user daemon-reload
	systemctl --user restart wayrdp
	sleep 2
	ss -ltn | grep -q '127.0.0.1:3389' ||
		die "wayrdp is not on 127.0.0.1:3389: journalctl --user -u wayrdp"
	cat <<EOF

wayrdp now listens on 127.0.0.1:3389. Bitvise's RDP tab (Computer = localhost)
works again; the C2S rule for 13390 can go.
Once everything works: sh $0 --purge
EOF
	exit 0
}

purge() {
	if systemctl is-active --quiet xrdp 2>/dev/null; then
		die "xrdp still runs: sh $0 --switch first"
	fi
	say "Purging xrdp and X11 packages (apt shows the list and asks)"
	echo "xwayland, xterm, xfonts-base and fonts-dejavu-core must NOT be on it."
	pkgs=
	for p in xrdp xorgxrdp libx11-dev libxext-dev libxft-dev libxrandr-dev \
			x11-utils x11-xserver-utils x11-xkb-utils dbus-x11 \
			tigervnc-standalone-server tigervnc-tools; do
		dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q 'install ok installed' &&
			pkgs="$pkgs $p"
	done
	if [ -n "$pkgs" ]; then
		# shellcheck disable=SC2086 # one word per package
		sudo apt-get purge $pkgs
	else
		echo "none of them installed"
	fi
	sudo apt-get autoremove --purge
	if dpkg -l 'xserver-xorg*' 2>/dev/null | grep -q '^ii'; then
		echo "note: still installed, something else needs it:"
		dpkg -l 'xserver-xorg*' | grep '^ii'
	fi

	say "Moving old X11 files to ~/x11-backup"
	systemctl --user disable --now mwm-vnc 2>/dev/null || :
	mkdir -p "$HOME/x11-backup"
	for f in "$HOME"/.xsession "$HOME"/.xsession-errors* "$HOME"/.xorgxrdp.*.log \
			"$HOME/.vnc" "$HOME/bin/mwm-vnc" \
			"$HOME/.config/systemd/user/mwm-vnc.service" "$HOME/.config/mwm"; do
		[ -e "$f" ] && mv "$f" "$HOME/x11-backup/" && echo "moved $f"
	done
	systemctl --user daemon-reload
	sudo rm -f /usr/local/bin/mwm /usr/share/xsessions/mwm-x11.desktop
	say "Done. ~/x11-backup can go once nothing is missed."
	exit 0
}

case ${1:-} in
--switch) switch ;;
--purge) purge ;;
"") ;;
*) die "unknown option $1 (only --switch or --purge)" ;;
esac

# ---- 1. requirements ----------------------------------------------------------
. /etc/os-release
[ "$ID" = ubuntu ] || echo "warning: written for Ubuntu, this is $ID $VERSION_ID"
sudo apt-get update
apt-cache show libwlroots-0.19-dev freerdp3-dev >/dev/null 2>&1 ||
	die "Ubuntu $VERSION_ID has no wlroots 0.19 or FreeRDP 3 (26.04 has): sudo do-release-upgrade first"

# ---- 2. packages --------------------------------------------------------------
say "Installing packages"
sudo apt-get install -y --no-install-recommends \
	build-essential pkg-config git meson ninja-build \
	libwlroots-0.19-dev wayland-protocols libwayland-dev libxkbcommon-dev \
	libpixman-1-dev libcairo2-dev libpango1.0-dev \
	libxcb1-dev libxcb-icccm4-dev \
	freerdp3-dev libwinpr3-dev libdrm-dev libpam0g-dev \
	xwayland dbus-user-session \
	xterm xfonts-base fonts-dejavu-core

# ---- 3. build mwc and wayrdp ----------------------------------------------------
fetch() {	# fetch <dir> <repository>
	if [ -d "$1/.git" ]; then
		git -C "$1" pull --ff-only
	else
		git clone "$2" "$1"
	fi
}
say "Building mwc in $SRC"
fetch "$SRC" https://github.com/reygnn/mwc.git
make -C "$SRC"
sudo make -C "$SRC" install

say "Building wayrdp in $WSRC"
fetch "$WSRC" https://github.com/reygnn/wayrdp.git
[ -f "$WSRC/build/build.ninja" ] ||
	meson setup "$WSRC/build" "$WSRC" --buildtype=release --sysconfdir=/etc
ninja -C "$WSRC/build"
sudo ninja -C "$WSRC/build" install

# ---- 4. configuration ---------------------------------------------------------
say "Configuration in ~/.config/mwc"
CFG=$HOME/.config/mwc
mkdir -p "$CFG"
if [ ! -e "$CFG/mwmrc" ] && [ -f "$HOME/.config/mwm/mwmrc" ]; then
	cp "$HOME/.config/mwm/mwmrc" "$CFG/mwmrc"
	echo "copied ~/.config/mwm/mwmrc"
fi
if [ ! -e "$CFG/autostart" ] && [ -x "$HOME/.config/mwm/autostart" ]; then
	cp "$HOME/.config/mwm/autostart" "$CFG/autostart"
	echo "copied ~/.config/mwm/autostart"
fi
touch "$CFG/mwmrc"
grep -qi '^Mwm\*HEADLESS-1\*outputMode:' "$CFG/mwmrc" ||
	printf '! The headless output: its size until a client asks for another.\nMwm*HEADLESS-1*outputMode:   %s\n' \
		"$SIZE" >>"$CFG/mwmrc"
grep -qi '^Mwm\*outputManagement:' "$CFG/mwmrc" ||
	printf '! The size follows the mstsc window (wayrdp sets it).\nMwm*outputManagement:        true\n' \
		>>"$CFG/mwmrc"
grep -qi '^Mwm\*terminal:' "$CFG/mwmrc" ||
	echo 'Mwm*terminal:                xterm' >>"$CFG/mwmrc"

# ---- 5. user services ---------------------------------------------------------
say "User services mwc and wayrdp"
mkdir -p "$UNITS"
cat >"$UNITS/mwc.service" <<'EOF'
[Unit]
Description=mwc on a headless output

[Service]
Environment=WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
Environment=MWC_VIRTUAL_INPUT=1
ExecStart=/usr/local/bin/mwc
Restart=on-failure

[Install]
WantedBy=default.target
EOF
if [ ! -e "$UNITS/wayrdp.service" ]; then
	cp "$WSRC/contrib/wayrdp.service" "$UNITS/wayrdp.service"
	# The notebook's layout; port 3390 while xrdp still has 3389.
	sed -i "s|^ExecStart=.*|ExecStart=/usr/local/bin/wayrdp -k $KBD 127.0.0.1 3390|" \
		"$UNITS/wayrdp.service"
	if [ "$AUTH" = pam ]; then
		sed -i "s|^ExecStart=/usr/local/bin/wayrdp |&-a pam |" "$UNITS/wayrdp.service"
		# PAM checks the password with the setgid helper unix_chkpwd.
		sed -i '/^NoNewPrivileges=yes$/d' "$UNITS/wayrdp.service"
	fi
else
	echo "keeping the existing $UNITS/wayrdp.service"
fi

# ---- 6. the RDP login -----------------------------------------------------------
if [ "$AUTH" = nla ] && [ ! -e "$HOME/.config/wayrdp/sam" ]; then
	say "The RDP password (one of its own, not your Linux password)"
	if [ -t 0 ]; then
		/usr/local/bin/wayrdp --set-password
	else
		die "no terminal to ask on: run 'wayrdp --set-password', then this again"
	fi
fi
if [ "$AUTH" = pam ]; then
	echo "Login with your Linux password: save the credentials in mstsc,"
	echo "wayrdp has no login window."
fi

sudo loginctl enable-linger "$USER"
systemctl --user daemon-reload
systemctl --user enable mwc wayrdp
systemctl --user restart mwc
systemctl --user restart wayrdp

# ---- 7. check -------------------------------------------------------------------
say "Check"
sleep 3
ok=1
systemctl --user is-active --quiet mwc || { echo "mwc is not running: journalctl --user -u mwc"; ok=; }
systemctl --user is-active --quiet wayrdp || { echo "wayrdp is not running: journalctl --user -u wayrdp"; ok=; }
ss -ltn | grep -qE '127.0.0.1:(3390|3389) ' || { echo "wayrdp listens on neither 127.0.0.1:3390 nor :3389"; ok=; }
if ss -ltn | grep -E ':3390 ' | grep -qv '127.0.0.1'; then
	echo "WARNING: 3390 listens beyond localhost:"; ss -ltn | grep -E ':3390 '
	ok=
fi
[ -n "$ok" ] || die "see above"
thumb=$(journalctl --user -u wayrdp -n 50 --no-pager 2>/dev/null |
	sed -n 's/.*SHA-1 thumbprint: //p' | tail -1)
cat <<EOF

mwc runs on a ${SIZE} headless output, wayrdp on 127.0.0.1:3390.
In Bitvise: a C2S rule 127.0.0.1:13390 -> localhost:3390,
then mstsc /v:127.0.0.1:13390 and your Linux user name.
EOF
[ -z "$thumb" ] || echo "Certificate thumbprint (SHA-1, mstsc shows it): $thumb"
echo "The old xrdp still answers on 3389. Once wayrdp does the job: sh $0 --switch"
