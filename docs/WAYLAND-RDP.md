# From mwm (X11) to mwc (Wayland) on a headless server, still over RDP

This is the follow-up to mwm-x11's `docs/HEADLESS-SETUP.md`. That guide put
Xorg, xorgxrdp and mwm on an Ubuntu server without a GPU, reached through an
SSH tunnel with Windows' Remote Desktop (`mstsc`), optionally through Bitvise.
Here the same server moves to mwc. The way in stays the same: the same tunnel,
the same `mstsc`, the same xrdp on `127.0.0.1:3389`. Behind xrdp there is no X
server any more.

```
corporate notebook (Windows)              server (Ubuntu 26.04)
┌──────────────────────────┐  SSH tunnel  ┌──────────────────────────────────┐
│ Bitvise SSH Client       │ ═══════════> │ xrdp  127.0.0.1:3389  (RDP→VNC)  │
│  └─ Remote Desktop       │ (encrypted)  │  └─ wayvnc  127.0.0.1:5900       │
│     (mstsc)              │              │      └─ mwc, headless output     │
└──────────────────────────┘              │          ├─ Wayland apps         │
                                          │          └─ Xwayland: xterm, …   │
                                          └──────────────────────────────────┘
```

wlroots has no RDP server of its own any more. So mwc runs on wlroots'
headless backend, `wayvnc` serves its screen as VNC, and xrdp translates
between RDP and VNC. xrdp ships that translation as the `vnc-any` session
type. Your X programs keep running, through Xwayland.

[`wayland-rdp-upgrade.sh`](wayland-rdp-upgrade.sh) runs the steps below
(sections 2 to 7, and 8 with `--purge`). This page explains what it does.

## What changes

| | before (mwm, X11) | after (mwc, Wayland) |
|---|---|---|
| Session starts | when you log in through RDP (xrdp starts Xorg) | at boot, as a systemd user service. RDP only looks at it |
| RDP login | starts or reconnects a session | a password check (PAM), then the running desktop |
| Screen size | the size of the `mstsc` window at login | fixed in mwmrc (`outputMode`), it doesn't follow the window |
| Keyboard layout | the notebook's, through xorgxrdp | `wayvnc -k`, set once on the server |
| Clipboard | text, images, files | plain text, Latin-1 only (a limit of xrdp's VNC module) |
| X programs | on Xorg | on Xwayland, inside mwc |
| Config | `~/.config/mwm/mwmrc`, `~/.config/mwm/autostart` | `~/.config/mwc/mwmrc`, `~/.config/mwc/autostart`, same syntax |

## 1. Requirements

mwc needs wlroots 0.19, which Ubuntu ships from 26.04 on. A 24.04 server
must be upgraded first. `do-release-upgrade` is made to run over SSH. The
risks are a connection that drops halfway, a full disk, and a server that
doesn't come back with its network after the reboot. Each has a precaution.

### Before

1. **Snapshot or backup.** For a VM, a snapshot at the hoster or in the
   hypervisor. This is the real insurance.
2. **A way in besides SSH:** the hoster's web console, KVM or serial console.
   Without one the risk is noticeably higher.
3. **24.04 fully up to date, and rebooted:**

   ```sh
   sudo apt update && sudo apt full-upgrade && sudo reboot
   ```

4. **Room:** `df -h / /boot` should show a few GB free. A full `/boot` is
   usually old kernels: `sudo apt autoremove --purge`.
5. **`screen`:** `sudo apt install screen`. The upgrade runs inside it when it
   is there, so it goes on if the connection drops. After reconnecting,
   `sudo screen -x` brings it back. Check the first lines of its output to
   see whether this release really does so.
6. **PPAs and other sources** are switched off by the upgrade. Check what
   came from them afterwards.
7. **A server someone else manages** (Landscape, Ansible, Puppet): ask
   first. Such tools may undo changes or bring their own upgrade procedure.

### The upgrade

```sh
sudo do-release-upgrade
```

- Run it from a plain SSH session, not from the RDP desktop. That one runs on
  Xorg and xrdp, which are being replaced.
- It notices SSH, warns, and starts a second sshd on port 1022 as a way back.
  That only helps if a firewall (ufw, the hoster's, the company's) lets 1022
  through. Through a Bitvise tunnel that only reaches port 22 it usually
  doesn't.
- When it asks about changed configuration files, keep your own version if
  in doubt. That matters most for `/etc/ssh/sshd_config`, and always for
  `/etc/xrdp/xrdp.ini` (`port=tcp://.:3389`).
- Keep a second SSH session open until the server is back after the reboot.

LTS-to-LTS upgrades (24.04 → 26.04) are offered from the point release
26.04.1 on. If it says "No new release found", check that
`/etc/update-manager/release-upgrades` has `Prompt=lts`. Don't force it with
`-d` on a server.

After the reboot, `lsb_release -rs` must show `26.04`.

The old setup keeps working until section 8. Leave it alone until mwc works.

## 2. Packages

```sh
sudo apt update
sudo apt install --no-install-recommends \
    build-essential pkg-config git \
    libwlroots-0.19-dev wayland-protocols libwayland-dev libxkbcommon-dev \
    libpixman-1-dev libcairo2-dev libpango1.0-dev \
    libxcb1-dev libxcb-icccm4-dev \
    xwayland wayvnc xrdp dbus-user-session \
    xterm xfonts-base fonts-dejavu-core
```

| Packages | Purpose |
|---|---|
| `build-essential … libxcb-icccm4-dev` | build mwc |
| `xwayland` | X programs inside mwc |
| `wayvnc` | VNC server for wlroots compositors |
| `xrdp` | already there: RDP on the outside, VNC on the inside |
| `dbus-user-session` | the session bus for user services, replaces `dbus-launch` from `dbus-x11` |
| `xterm xfonts-base fonts-dejavu-core` | already there: terminal and fonts |

Use `--no-install-recommends` for xrdp too. Otherwise apt keeps (or brings
back) `xorgxrdp` and with it the whole Xorg server.

## 3. Build and install mwc

```sh
git clone https://github.com/reygnn/mwc.git
cd mwc
make
sudo make install          # /usr/local/bin/mwc
```

Later updates: `git pull && make && sudo make install`, then
`systemctl --user restart mwc`. That ends the session and every window in it.
A Wayland compositor can't restart and keep its clients the way mwm can.

## 4. Configuration

mwmrc has the same syntax as mwm's, so you can take the old one along:

```sh
mkdir -p ~/.config/mwc
[ -f ~/.config/mwm/mwmrc ] && cp ~/.config/mwm/mwmrc ~/.config/mwc/mwmrc
cat >> ~/.config/mwc/mwmrc <<'EOF'
! The headless output is HEADLESS-1. Its size is what Remote Desktop shows.
Mwm*HEADLESS-1*outputMode:   1920x1080
Mwm*terminal:                xterm
EOF
```

Choose the size of your notebook's screen, or a little less if `mstsc` doesn't
run full screen. `Mwm*fontList` takes Pango names (`Sans Bold 10`). An old
Xft name such as `Sans:bold:size=10` is logged and ignored, see the README.

Programs to start with the session go in `~/.config/mwc/autostart`. An old
`~/.config/mwm/autostart` usually works as is:

```sh
[ -x ~/.config/mwm/autostart ] && cp ~/.config/mwm/autostart ~/.config/mwc/autostart
```

## 5. mwc and wayvnc as user services

`~/.config/systemd/user/mwc.service`:

```ini
[Unit]
Description=mwc on a headless output

[Service]
Environment=WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
Environment=MWC_VIRTUAL_INPUT=1
ExecStart=/usr/local/bin/mwc
Restart=on-failure

[Install]
WantedBy=default.target
```

`~/.config/systemd/user/wayvnc.service`:

```ini
[Unit]
Description=wayvnc for mwc
BindsTo=mwc.service
After=mwc.service

[Service]
Environment=WAYLAND_DISPLAY=wayland-0
ExecStart=/usr/bin/wayvnc -k de 127.0.0.1 5900
# mwc's socket may not be there yet when this starts.
Restart=on-failure
RestartSec=1

[Install]
WantedBy=mwc.service
```

```sh
systemctl --user daemon-reload
systemctl --user enable --now mwc wayvnc
sudo loginctl enable-linger "$USER"   # run them without a login, also after a reboot
```

- `WLR_BACKENDS=headless` makes a virtual monitor with no GPU or screen behind
  it, and `WLR_RENDERER=pixman` draws in software.
- `MWC_VIRTUAL_INPUT=1` lets wayvnc type and click. mwc offers those
  protocols only when asked, because any program in the session could use
  them. Leave it out of a desktop session started from GDM.
- `-k de` is the layout wayvnc turns keys into, so it has to match the
  notebook's (`ch`, `us`, `de-nodeadkeys`, …).

Check that both are up and that wayvnc listens on localhost only:

```sh
systemctl --user status mwc wayvnc
ss -ltn | grep 5900          # 127.0.0.1:5900, not 0.0.0.0 or *
```

## 6. xrdp: a session type for mwc

xrdp keeps its port from the old setup (`port=tcp://.:3389`, localhost only).
It gets one more section at the end of `/etc/xrdp/xrdp.ini`, and `autorun` in
`[Globals]` picks it:

```sh
sudo tee -a /etc/xrdp/xrdp.ini >/dev/null <<'EOF'

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
sudo sed -i 's|^autorun=.*|autorun=mwc|' /etc/xrdp/xrdp.ini
grep -q '^autorun=mwc' /etc/xrdp/xrdp.ini || \
    sudo sed -i 's|^\[Globals\]$|[Globals]\nautorun=mwc|' /etc/xrdp/xrdp.ini
sudo systemctl restart xrdp xrdp-sesman
```

`pamusername=same` makes xrdp check the user name and password from its login
window (or from `mstsc`) with PAM before it opens the VNC connection. That is
the Linux password, as before.

### Who may get in

wayvnc itself asks for no password. The only VNC security type xrdp knows
besides "none" is the old VNC password, and wayvnc doesn't offer that one. So
two more locks are worth having:

- **Only your user through xrdp.** Without this, any Linux account with a
  password passes the PAM check. Put yourself in xrdp's group and make the
  group required, in `/etc/xrdp/sesman.ini`, section `[Security]`:

  ```sh
  sudo groupadd -f tsusers
  sudo adduser "$USER" tsusers
  sudo sed -i 's|^AlwaysGroupCheck=false|AlwaysGroupCheck=true|' /etc/xrdp/sesman.ini
  sudo systemctl restart xrdp-sesman
  ```

- **Nobody past xrdp, straight to port 5900.** On a server with other users
  (or other services you don't trust), they could connect to
  `127.0.0.1:5900` themselves. An nftables rule lets only root (xrdp) and you
  through:

  ```sh
  sudo nft add table inet mwc
  sudo nft add chain inet mwc out '{ type filter hook output priority 0; }'
  sudo nft add rule inet mwc out oif lo tcp dport 5900 \
      meta skuid != "{ 0, $(id -u) }" reject
  ```

  That holds until the next reboot. To keep it, put it in `/etc/nftables.conf`
  and `sudo systemctl enable nftables`.

On a server that is yours alone, both are optional.

## 7. Connect

Nothing changes on the notebook. In Bitvise, use the RDP tab with
*Computer* = `localhost`, or your C2S rule `127.0.0.1:13389 → localhost:3389`
and `mstsc /v:127.0.0.1:13389`. Log in with the Linux user name and password.
With `autorun=mwc`, xrdp goes straight to mwc's desktop when `mstsc` sends the
credentials. Otherwise pick *Session* = **mwc** in xrdp's login window.

- **Disconnect and come back:** closing Remote Desktop leaves mwc running, so
  you get the same windows back next time.
- **Ending the session:** "Quit..." in the root menu ends mwc with exit code
  0. The service then stays stopped until `systemctl --user start mwc` (or
  the next boot), and RDP finds no desktop until then.
- **Screen size:** `mstsc` scales or scrolls if its window differs from
  `outputMode`. Change the size in mwmrc and choose "Reload" in the root menu.
  No restart is needed.
- **Keyboard shortcuts:** as before, in `mstsc` under *Local Resources →
  Keyboard*, "Apply Windows key combinations" *On the remote computer*.
  Windows keeps Win+L and Ctrl+Alt+Del for itself.

## 8. Purge what X11 needed

Only once mwc works over RDP. These are the packages from the old guide that
nothing needs any more:

| Package | Was for |
|---|---|
| `xorgxrdp` | Xorg for xrdp. Takes the Xorg server (`xserver-xorg-core` …) with it |
| `libx11-dev libxext-dev libxft-dev libxrandr-dev` | building mwm |
| `x11-utils x11-xserver-utils x11-xkb-utils` | `xdpyinfo`, `xrandr`, `xsetroot`, `setxkbmap`: no use under Wayland |
| `dbus-x11` | `dbus-launch`. `dbus-user-session` does that now |
| `tigervnc-standalone-server tigervnc-tools` | variant B of the old guide, if you had it |
| `xauth` | only variant B needed it. Keep it if you use `ssh -X` |

```sh
sudo apt purge xorgxrdp libx11-dev libxext-dev libxft-dev libxrandr-dev \
    x11-utils x11-xserver-utils x11-xkb-utils dbus-x11 \
    tigervnc-standalone-server tigervnc-tools
sudo apt autoremove --purge
```

apt lists what it will remove before it asks. **`xrdp`, `xwayland`,
`wayvnc`, `xterm`, `xfonts-base` and `fonts-dejavu-core` must not be on that
list.** If one is, answer `n` and leave out the package that pulls it along.
Packages that aren't installed are reported and skipped.

Afterwards no Xorg should be left:

```sh
dpkg -l 'xserver-xorg*' | grep '^ii'    # prints nothing
```

Then the old files go. Move them out of the way instead of deleting them at
first:

```sh
systemctl --user disable --now mwm-vnc 2>/dev/null
mkdir -p ~/x11-backup
mv ~/.xsession ~/.xsession-errors* ~/.xorgxrdp.*.log ~/.vnc ~/bin/mwm-vnc \
    ~/.config/systemd/user/mwm-vnc.service ~/.config/mwm ~/x11-backup/ 2>/dev/null
systemctl --user daemon-reload
sudo rm -f /usr/local/bin/mwm /usr/share/xsessions/mwm-x11.desktop
```

The `[Xorg]` and `[Xvnc]` sections in `xrdp.ini` don't work any more. With
`autorun=mwc` they do no harm.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| xrdp: "error - problem connecting" | wayvnc doesn't run: `systemctl --user status mwc wayvnc`, `journalctl --user -u wayvnc`. Is linger on (`loginctl show-user "$USER" -p Linger`)? |
| `mwc.service` fails at once | `journalctl --user -u mwc`. `failed to create wlr_backend`: the `Environment=WLR_BACKENDS=headless` line is missing. |
| The picture comes, but keys and mouse do nothing, or wayvnc won't start | `MWC_VIRTUAL_INPUT=1` is missing in `mwc.service` (then `daemon-reload` and restart both). |
| Login fails although the password is right | `AlwaysGroupCheck=true` without your user in `tsusers`: `id` must list it, then log in to SSH again. See `/var/log/xrdp-sesman.log`. |
| Wrong characters | `-k` in `wayvnc.service` doesn't match the notebook's layout. |
| No mouse pointer, or two | Add `-r` to wayvnc's command line: it then draws the pointer into the picture. |
| Black or wrong size | `outputMode` names `HEADLESS-1`? `journalctl --user -u mwc` reports a value it can't use. |
| X programs don't start | `xwayland` is missing, or `journalctl --user -u mwc` shows why Xwayland failed. |
