# From mwm (X11) to mwc (Wayland) on a headless server, still over RDP

This is the follow-up to mwm-x11's `docs/HEADLESS-SETUP.md`. That guide put
Xorg, xorgxrdp and mwm on an Ubuntu server without a GPU, reached through an
SSH tunnel with Windows' Remote Desktop (`mstsc`), optionally through Bitvise.
Here the same server moves to mwc. The way in stays the same: the same tunnel,
the same `mstsc`. Behind the tunnel there is no X server and no xrdp any more,
but [wayrdp](https://github.com/reygnn/wayrdp), an RDP server for mwc.

```
corporate notebook (Windows)              server (Ubuntu 26.04)
┌──────────────────────────┐  SSH tunnel  ┌──────────────────────────────────┐
│ Bitvise SSH Client       │ ═══════════> │ wayrdp  127.0.0.1:3389           │
│  └─ Remote Desktop       │ (encrypted)  │  └─ mwc, headless output         │
│     (mstsc)              │              │      ├─ Wayland apps             │
└──────────────────────────┘              │      └─ Xwayland: xterm, …       │
                                          └──────────────────────────────────┘
```

mwc runs on wlroots' headless backend, and wayrdp serves its screen over RDP:
it captures the output, sends it to `mstsc` and passes keyboard, mouse and
clipboard back. Your X programs keep running, through Xwayland.

wayrdp is new and not yet tested with `mstsc` itself (only with FreeRDP's
clients). So the old setup keeps running until wayrdp does the job: wayrdp
starts on port 3390, next to the old xrdp on 3389, and takes 3389 over only
in section 8.

[`wayland-rdp-upgrade.sh`](wayland-rdp-upgrade.sh) runs the steps below
(sections 2 to 7, 8 with `--switch`, 9 with `--purge`). This page explains
what it does.

## What changes

| | before (mwm, X11) | after (mwc, Wayland) |
|---|---|---|
| RDP server | xrdp, which starts Xorg (xorgxrdp) | wayrdp, a user service next to mwc |
| Session starts | when you log in through RDP | at boot, as a systemd user service. RDP only looks at it |
| RDP login | the Linux password (PAM, in xrdp's login window) | an RDP password of its own (NLA), or the Linux password (`-a pam`) |
| Screen size | the size of the `mstsc` window at login | the size of the `mstsc` window, and it follows when you resize it |
| Keyboard layout | the notebook's, through xorgxrdp | `wayrdp -k`, set once on the server |
| Clipboard | text, images, files | text, full Unicode, both ways |
| X programs | on Xorg | on Xwayland, inside mwc |
| Config | `~/.config/mwm/mwmrc`, `~/.config/mwm/autostart` | `~/.config/mwc/mwmrc`, `~/.config/mwc/autostart`, same syntax |

## 1. Requirements

mwc needs wlroots 0.19 and wayrdp FreeRDP 3, which Ubuntu ships from 26.04 on.
A 24.04 server must be upgraded first. `do-release-upgrade` is made to run
over SSH. The risks are a connection that drops halfway, a full disk, and a
server that doesn't come back with its network after the reboot. Each has a
precaution.

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
  in doubt. That matters most for `/etc/ssh/sshd_config`, and for
  `/etc/xrdp/xrdp.ini` (`port=tcp://.:3389`) as long as xrdp is the way in.
- Keep a second SSH session open until the server is back after the reboot.

LTS-to-LTS upgrades (24.04 → 26.04) are offered from the point release
26.04.1 on. If it says "No new release found", check that
`/etc/update-manager/release-upgrades` has `Prompt=lts`. Don't force it with
`-d` on a server.

After the reboot, `lsb_release -rs` must show `26.04`.

The old setup keeps working until section 8. Leave it alone until mwc and
wayrdp work.

## 2. Packages

```sh
sudo apt update
sudo apt install --no-install-recommends \
    build-essential pkg-config git meson ninja-build \
    libwlroots-0.19-dev wayland-protocols libwayland-dev libxkbcommon-dev \
    libpixman-1-dev libcairo2-dev libpango1.0-dev \
    libxcb1-dev libxcb-icccm4-dev \
    freerdp3-dev libwinpr3-dev libdrm-dev libpam0g-dev \
    xwayland dbus-user-session \
    xterm xfonts-base fonts-dejavu-core
```

| Packages | Purpose |
|---|---|
| `build-essential … libxcb-icccm4-dev` | build mwc |
| `meson ninja-build freerdp3-dev … libpam0g-dev` | build wayrdp (FreeRDP 3, and PAM for `-a pam`) |
| `xwayland` | X programs inside mwc |
| `dbus-user-session` | the session bus for user services, replaces `dbus-launch` from `dbus-x11` |
| `xterm xfonts-base fonts-dejavu-core` | already there: terminal and fonts |

## 3. Build and install mwc and wayrdp

```sh
git clone https://github.com/reygnn/mwc.git
make -C mwc
sudo make -C mwc install                  # /usr/local/bin/mwc

git clone https://github.com/reygnn/wayrdp.git
cd wayrdp
meson setup build --buildtype=release --sysconfdir=/etc
ninja -C build
sudo ninja -C build install               # /usr/local/bin/wayrdp, /etc/pam.d/wayrdp
```

`--sysconfdir=/etc` puts wayrdp's PAM service where PAM looks. wayrdp fetches
the aml library during `meson setup`; that needs access to github.com.

Later updates: `git pull`, build and install again, then
`systemctl --user restart mwc` (mwc) or `systemctl --user restart wayrdp`
(wayrdp). Restarting mwc ends the session and every window in it: a Wayland
compositor can't restart and keep its clients the way mwm can. Restarting
wayrdp only drops the RDP connection; `mstsc` reconnects to the same
desktop.

## 4. Configuration

mwmrc has the same syntax as mwm's, so you can take the old one along:

```sh
mkdir -p ~/.config/mwc
[ -f ~/.config/mwm/mwmrc ] && cp ~/.config/mwm/mwmrc ~/.config/mwc/mwmrc
cat >> ~/.config/mwc/mwmrc <<'EOF'
! The headless output is HEADLESS-1: its size until a client asks for another.
Mwm*HEADLESS-1*outputMode:   1920x1080
! The size follows the mstsc window (wayrdp sets it).
Mwm*outputManagement:        true
Mwm*terminal:                xterm
EOF
```

- `outputMode` is the size before the first connection. Then wayrdp gives
  the output the size of the `mstsc` window, and follows when you resize it.
- `outputManagement: true` is what lets wayrdp do that. **Then nothing else
  may use output management** – no `wlr-randr`, `kanshi` or `wdisplays`:
  wlroots 0.19 and 0.20 abort when a second program uses it, and take the
  whole session with them. Without the line, `mstsc` simply gets the size of
  `outputMode`.
- `Mwm*fontList` takes Pango names (`Sans Bold 10`). An old Xft name such as
  `Sans:bold:size=10` is logged and ignored, see the README.

Programs to start with the session go in `~/.config/mwc/autostart`. An old
`~/.config/mwm/autostart` usually works as is:

```sh
[ -x ~/.config/mwm/autostart ] && cp ~/.config/mwm/autostart ~/.config/mwc/autostart
```

## 5. mwc and wayrdp as user services

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

`~/.config/systemd/user/wayrdp.service` is wayrdp's
[`contrib/wayrdp.service`](https://github.com/reygnn/wayrdp/blob/main/contrib/wayrdp.service):

```sh
mkdir -p ~/.config/systemd/user
cp wayrdp/contrib/wayrdp.service ~/.config/systemd/user/
```

Its `ExecStart` is `wayrdp -k de 127.0.0.1 3390`: port 3390 while the old
xrdp still has 3389, and `-k` the notebook's keyboard layout (`ch`, `us`,
`de-nodeadkeys`, …). wayrdp turns the keys `mstsc` sends into characters with
it. Change the layout there if yours isn't German.

The password comes first (section 6), then:

```sh
systemctl --user daemon-reload
systemctl --user enable --now mwc wayrdp
sudo loginctl enable-linger "$USER"   # run them without a login, also after a reboot
```

- `WLR_BACKENDS=headless` makes a virtual monitor with no GPU or screen behind
  it, and `WLR_RENDERER=pixman` draws in software.
- `MWC_VIRTUAL_INPUT=1` lets wayrdp type and click. mwc offers those
  protocols only when asked, because any program in the session could use
  them. Leave it out of a desktop session started from GDM.
- wayrdp's service is bound to mwc's: it stops with mwc and starts again
  with it.

Check that both are up and that wayrdp listens on localhost only:

```sh
systemctl --user status mwc wayrdp
ss -ltn | grep 3390                    # 127.0.0.1:3390, not 0.0.0.0 or *
journalctl --user -u wayrdp            # "Listening on", and the certificate's thumbprint
```

## 6. The RDP login

wayrdp has two ways to log in. Either way only your own user gets in, and
three failed logins in a row lock new connections out for 30 s.

**NLA with an RDP password of its own** (the default, what `mstsc` expects):

```sh
wayrdp --set-password
```

It asks twice and writes `~/.config/wayrdp/sam` (mode 0600). The file holds
the unsalted NT hash of that password, which is why it should be a password
of its own, not your Linux one.

**The Linux password** (`-a pam`), as with xrdp: add `-a pam` to `ExecStart`
in `wayrdp.service` and remove the line `NoNewPrivileges=yes` there (PAM
checks the password with a setgid helper). wayrdp has no login window, so
`mstsc` must have the credentials saved ("Allow me to save credentials"),
and it warns that the connection uses no NLA.

On the first start wayrdp makes a self-signed certificate
(`~/.config/wayrdp/tls.crt`). Its SHA-1 thumbprint is in the log; `mstsc`
shows the same one when it warns about the certificate.

## 7. Connect

In Bitvise, add a C2S rule `127.0.0.1:13390 → localhost:3390`, then connect
with `mstsc /v:127.0.0.1:13390`. Log in with your Linux user name and the RDP
password (or the Linux password with `-a pam`).

- **The certificate:** `mstsc` warns on the first connection. Compare the
  thumbprint with `journalctl --user -u wayrdp` and choose "Don't ask me
  again".
- **Clipboard:** tick it under *Local Resources*. Text goes both ways, with
  umlauts and everything else; images and files don't.
- **Screen size:** the desktop takes the size of the `mstsc` window, also
  when you make it larger or smaller later.
- **Disconnect and come back:** closing Remote Desktop leaves mwc running,
  so you get the same windows back next time. A second connection takes
  over from the first.
- **Ending the session:** "Quit..." in the root menu ends mwc with exit code
  0. The services then stay stopped until `systemctl --user start mwc` (or
  the next boot), and RDP finds no desktop until then.
- **Keyboard shortcuts:** in `mstsc` under *Local Resources → Keyboard*,
  "Apply Windows key combinations" *On the remote computer*. Windows keeps
  Win+L and Ctrl+Alt+Del for itself.

## 8. Switch over

Once wayrdp does the job day to day, xrdp goes and wayrdp takes port 3389:

```sh
sudo systemctl disable --now xrdp xrdp-sesman
sed -i 's| 127.0.0.1 3390$| 127.0.0.1|' ~/.config/systemd/user/wayrdp.service
systemctl --user daemon-reload
systemctl --user restart wayrdp
ss -ltn | grep 3389                    # 127.0.0.1:3389
```

Bitvise's RDP tab with *Computer* = `localhost` works again, and so does an
old C2S rule `127.0.0.1:13389 → localhost:3389`. The rule for 13390 can go.

## 9. Purge what X11 needed

Only once mwc and wayrdp work on 3389. These are the packages from the old
guide that nothing needs any more:

| Package | Was for |
|---|---|
| `xrdp` | the old RDP server |
| `xorgxrdp` | Xorg for xrdp. Takes the Xorg server (`xserver-xorg-core` …) with it |
| `libx11-dev libxext-dev libxft-dev libxrandr-dev` | building mwm |
| `x11-utils x11-xserver-utils x11-xkb-utils` | `xdpyinfo`, `xrandr`, `xsetroot`, `setxkbmap`: no use under Wayland |
| `dbus-x11` | `dbus-launch`. `dbus-user-session` does that now |
| `tigervnc-standalone-server tigervnc-tools` | variant B of the old guide, if you had it |
| `xauth` | only variant B needed it. Keep it if you use `ssh -X` |

```sh
sudo apt purge xrdp xorgxrdp libx11-dev libxext-dev libxft-dev libxrandr-dev \
    x11-utils x11-xserver-utils x11-xkb-utils dbus-x11 \
    tigervnc-standalone-server tigervnc-tools
sudo apt autoremove --purge
```

apt lists what it will remove before it asks. **`xwayland`, `xterm`,
`xfonts-base` and `fonts-dejavu-core` must not be on that list.** If one is,
answer `n` and leave out the package that pulls it along. Packages that
aren't installed are reported and skipped.

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

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `mstsc`: "can't connect" | wayrdp doesn't run: `systemctl --user status mwc wayrdp`, `journalctl --user -u wayrdp`. Is linger on (`loginctl show-user "$USER" -p Linger`)? Does the Bitvise rule point at the port wayrdp listens on? |
| `wayrdp.service` fails at once | `journalctl --user -u wayrdp`. "No password set": `wayrdp --set-password`. "can be read by others": `chmod 600 ~/.config/wayrdp/sam ~/.config/wayrdp/tls.key`. "virtual keyboard or pointer": `MWC_VIRTUAL_INPUT=1` is missing in `mwc.service`. |
| `mwc.service` fails at once | `journalctl --user -u mwc`. `failed to create wlr_backend`: the `Environment=WLR_BACKENDS=headless` line is missing. |
| Login fails although the password is right | NLA: the user name must be your Linux one, the password the one from `--set-password`. `-a pam`: are the credentials saved in `mstsc`? After three failures wayrdp takes no connection for 30 s. |
| Wrong characters | `-k` in `wayrdp.service` doesn't match the notebook's layout. |
| The size doesn't follow the window | `Mwm*outputManagement: true` is missing in mwmrc, or mwc was not restarted since (it reads it at its start). The log says "offers no output management". |
| The whole session ends when a tool changes the screen | `wlr-randr`, `kanshi` or the like used output management next to wayrdp: wlroots aborts then. Don't run them with `outputManagement: true`. |
| No mouse pointer | Add `-r` to `ExecStart`: wayrdp then draws the pointer into the picture. |
| X programs don't start | `xwayland` is missing, or `journalctl --user -u mwc` shows why Xwayland failed. |
