#!/bin/sh
# Moves a headless Ubuntu 26.04 server from mwm (X11, xrdp + xorgxrdp) to mwc
# (Wayland, xrdp -> wayvnc -> mwc). The steps of docs/WAYLAND-RDP.md, sections 2-7.
#
#   sh wayland-rdp-upgrade.sh            install and set up mwc, the old setup stays
#   sh wayland-rdp-upgrade.sh --purge    afterwards: remove X11 packages and files
#
# Run as the user who logs in through RDP, not as root; it uses sudo where needed.
# Can be run again: it only adds what is missing.
#
#   MWC_SIZE=1920x1080   size of the headless output (what Remote Desktop shows)
#   MWC_KBD=de           keyboard layout for wayvnc, the notebook's
#   MWC_SRC=~/mwc        where the source goes
#   MWC_LOCK=1           only your user through xrdp (sesman's tsusers group)
set -eu

SIZE=${MWC_SIZE:-1920x1080}
KBD=${MWC_KBD:-de}
SRC=${MWC_SRC:-$HOME/mwc}
LOCK=${MWC_LOCK:-0}

say() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" != 0 ] || die "run this as your own user, not as root"

purge() {
	say "Purging X11 packages (apt shows the list and asks)"
	echo "xrdp, xwayland, wayvnc, xterm, xfonts-base and fonts-dejavu-core must NOT be on it."
	pkgs=
	for p in xorgxrdp libx11-dev libxext-dev libxft-dev libxrandr-dev \
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
--purge) purge ;;
"") ;;
*) die "unknown option $1 (only --purge)" ;;
esac

# ---- 1. requirements ----------------------------------------------------------
. /etc/os-release
[ "$ID" = ubuntu ] || echo "warning: written for Ubuntu, this is $ID $VERSION_ID"
sudo apt-get update
apt-cache show libwlroots-0.19-dev >/dev/null 2>&1 ||
	die "Ubuntu $VERSION_ID has no wlroots 0.19 (26.04 has): sudo do-release-upgrade first"

# ---- 2. packages --------------------------------------------------------------
say "Installing packages"
sudo apt-get install -y --no-install-recommends \
	build-essential pkg-config git \
	libwlroots-0.19-dev wayland-protocols libwayland-dev libxkbcommon-dev \
	libpixman-1-dev libcairo2-dev libpango1.0-dev \
	libxcb1-dev libxcb-icccm4-dev \
	xwayland wayvnc xrdp dbus-user-session \
	xterm xfonts-base fonts-dejavu-core

# ---- 3. build mwc -------------------------------------------------------------
say "Building mwc in $SRC"
if [ -d "$SRC/.git" ]; then
	git -C "$SRC" pull --ff-only
else
	git clone https://github.com/reygnn/mwc.git "$SRC"
fi
make -C "$SRC"
sudo make -C "$SRC" install

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
	printf '! The headless output: what Remote Desktop shows.\nMwm*HEADLESS-1*outputMode:   %s\n' \
		"$SIZE" >>"$CFG/mwmrc"
grep -qi '^Mwm\*terminal:' "$CFG/mwmrc" ||
	echo 'Mwm*terminal:                xterm' >>"$CFG/mwmrc"

# ---- 5. user services ---------------------------------------------------------
say "User services mwc and wayvnc"
UNITS=$HOME/.config/systemd/user
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
cat >"$UNITS/wayvnc.service" <<EOF
[Unit]
Description=wayvnc for mwc
BindsTo=mwc.service
After=mwc.service

[Service]
Environment=WAYLAND_DISPLAY=wayland-0
ExecStart=/usr/bin/wayvnc -k $KBD 127.0.0.1 5900
# mwc's socket may not be there yet when this starts.
Restart=on-failure
RestartSec=1

[Install]
WantedBy=mwc.service
EOF
sudo loginctl enable-linger "$USER"
systemctl --user daemon-reload
systemctl --user enable mwc wayvnc
systemctl --user restart mwc wayvnc

# ---- 6. xrdp --------------------------------------------------------------------
say "xrdp: session type mwc, localhost only"
INI=/etc/xrdp/xrdp.ini
sudo sed -i 's|^port=3389$|port=tcp://.:3389|' "$INI"
if ! grep -q '^\[mwc\]' "$INI"; then
	sudo tee -a "$INI" >/dev/null <<'EOF'

[mwc]
name=mwc
lib=libvnc.so
ip=127.0.0.1
port=5900
username=ask
password=ask
pamusername=same
pampassword=same
EOF
fi
if grep -q '^autorun=' "$INI"; then
	sudo sed -i 's|^autorun=.*|autorun=mwc|' "$INI"
else
	sudo sed -i 's|^\[Globals\]$|[Globals]\nautorun=mwc|' "$INI"
fi
if [ "$LOCK" = 1 ]; then
	sudo groupadd -f tsusers
	sudo adduser "$USER" tsusers
	sudo sed -i 's|^AlwaysGroupCheck=false|AlwaysGroupCheck=true|' /etc/xrdp/sesman.ini
	echo "only members of tsusers may log in (log in to SSH again for the group)"
fi
sudo adduser xrdp ssl-cert >/dev/null 2>&1 || :
sudo systemctl restart xrdp-sesman xrdp

# ---- 7. check -------------------------------------------------------------------
say "Check"
sleep 2
ok=1
systemctl --user is-active --quiet mwc || { echo "mwc is not running: journalctl --user -u mwc"; ok=; }
systemctl --user is-active --quiet wayvnc || { echo "wayvnc is not running: journalctl --user -u wayvnc"; ok=; }
ss -ltn | grep -q '127.0.0.1:5900' || { echo "nothing on 127.0.0.1:5900"; ok=; }
ss -ltn | grep -q '127.0.0.1:3389' || { echo "xrdp is not on 127.0.0.1:3389"; ok=; }
if ss -ltn | grep -E ':(3389|5900) ' | grep -qv '127.0.0.1'; then
	echo "WARNING: 3389 or 5900 listens beyond localhost:"; ss -ltn | grep -E ':(3389|5900) '
	ok=
fi
[ -n "$ok" ] || die "see above"
cat <<EOF

mwc runs on a ${SIZE} headless output, wayvnc on 127.0.0.1:5900, xrdp on 127.0.0.1:3389.
Connect as before (Bitvise -> Remote Desktop) and log in with your Linux password.
Once that works: sh $0 --purge
EOF
