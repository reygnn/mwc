# mwc

Project rules. The rules for all C projects, mwc as their reference, are in the global
CLAUDE.md.


## Build and test

Build and test inside the `mwc-dev` distrobox (`distrobox.ini`); the host lacks the
test tools. `distrobox enter mwc-dev -- bash -lc 'cd ~/data/C-Projects/mwc && ...'`.

| Target | |
|---|---|
| `make` | mwc |
| `make unit` | `tests/unit`: a program per module, always under ASan and UBSan; part of `make test` |
| `make test` | the scripts in `tests/`; one alone: `make NAME-test`, log in `build/NAME.log` |
| `make check` | the same under ASan and UBSan with `-Werror`, then the kept fuzz inputs |
| `make analyze` | `gcc -fanalyzer` over the sources, findings as errors |
| `make lint` | C lines over 100 columns, tabs as 8; placement written outside `placement.c`, `struct grab` outside `grab.c` |
| `make valgrind` | memcheck, slow, without the timing-dependent scripts |
| `make fuzz` | libFuzzer on the mwmrc parser, `FUZZ_TIME` seconds |
| `make schemes` | `docs/irix-schemes.html` from `docs/mwmrc.irix-schemes`; `schemes-test` in `make test` fails while it is out of date |
| `make walk` | random operations with invariants after each, `WALK_STEPS`, `WALK_SEED`; `make check` takes 150 |

CI (`.github/workflows/ci.yml`) runs `make lint`, a `-Werror` build, `make analyze` and
`make check` in `tests/Containerfile`'s image (Arch, wlroots 0.20), and `make check` in
`tests/Containerfile.ubuntu`'s (Ubuntu 26.04, wlroots 0.19).

Unit-tested logic lives in modules that call nothing of the display code:
`outputrc.c`, `sizehints.c`, `snaprules.c`, `tilelayout.c`, `activation.c`, `config.c`,
`draw.c`, `color.c`, `keys.c`. `color.c` and `color.h` hold the colours mtk shares: no
dependencies beyond the C library, changed here and copied to mtk unchanged.
`NAME_test.c` is linked with `src/NAME.c` (and `UNIT_WITH_NAME`) and gets real data:
views, outputs and wlroots' client state on the stack, mwmrc files
(`tests/unit/mwmrc.h`), cairo buffers. The display code asks these modules and acts.
Behaviour between clients and mwc stays with the scripts. LeakSanitizer is on, but for
fontconfig (`tests/unit/lsan.supp`).

Polling helpers: `check`, `wait_for`, `pixel_is`, `pixel_isnt`, `reload_mwc` in
`tests/lib.sh`. When a test client can't do what a test needs, extend it
(`tests/xdgc.c`, `layerc.c`, `badclient.c`, `vinput.c`, `idlec.c`). A scene node wlroots
can't make, as without memory: `build/mwc-nomem`, with `tests/nomem.c` wrapped in
(`-Wl,--wrap`), in `tests/nomem.sh`.

What must hold for the window state at any moment is in `tests/invariants.awk`, what
users and clients do in `tests/ops.sh`; `matrix.sh` and `walk.sh` combine them. A new
window state or way to change one goes there too.


## Rules

- mwc builds against wlroots 0.20 and 0.19 (Ubuntu 26.04): nothing only one of them
  has. A case needing a fix 0.19.2 lacks checks `wlroots_fixed` (`tests/lib.sh`).
- No workarounds for wlroots bugs fixed upstream; mwc waits for the release. The
  wlroots source is in `~/data/src/wlroots` (tag 0.20.2) for checking what it does.
- In `mwc.h` one comment line for a contract, none for what the name says.
- The running session reads `~/.config/mwc/mwmrc`; SIGHUP to `mwc` reloads it, a new
  binary needs a restart.
