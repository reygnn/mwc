# mwc

Project rules. Git, commit identity and version numbers: see the global CLAUDE.md.


## Build and test

Build and test inside the `mwc-dev` distrobox (`distrobox.ini`); the host lacks the
test tools. `distrobox enter mwc-dev -- bash -lc 'cd ~/data/C-Projects/mwc && ...'`.

| Target | |
|---|---|
| `make` | mwc |
| `make test` | the 21 headless scripts in `tests/`; one alone: `make NAME-test`, log in `build/NAME.log` |
| `make check` | the same under ASan and UBSan with `-Werror`, then the kept fuzz inputs |
| `make analyze` | `gcc -fanalyzer` over the sources, findings as errors |
| `make valgrind` | memcheck, slow, without the timing-dependent scripts |
| `make fuzz` | libFuzzer on the mwmrc parser, `FUZZ_TIME` seconds |

CI (`.github/workflows/ci.yml`) runs a `-Werror` build, `make analyze` and `make check` in
`tests/Containerfile`'s image.

Every fix or feature gets a test that fails without it; check that by taking the
change out when it is cheap. Poll, don't sleep: `check`, `wait_for`, `pixel_is`,
`reload_mwc` in `tests/lib.sh`. When a test client can't do what a test needs, extend
it (`tests/xdgc.c`, `xclient.c`, `layerc.c`, `badclient.c`, `vinput.c`, `idlec.c`).


## Rules

- No workarounds for wlroots bugs fixed upstream; mwc waits for the release. The
  wlroots 0.19.2 source is in `~/data/src/wlroots` for checking what it does.
- Limits left as they are go to `CAVEATS.md`, planned work to `TODO`, reviews to
  `docs/history/Review-DATE.md` once every point is settled.
- README and CAVEATS: short and factual, no dashes as punctuation, little bold.
- Comments in English, short, the why rather than the what; in `mwc.h` one line for
  a contract, none for what the name says.
- Commit subject German, body English.
- `sudo make install` is the user's. The running session reads
  `~/.config/mwc/mwmrc`; SIGHUP to `mwc` reloads it, a new binary needs a restart.
