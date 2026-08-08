# Contributing

Thanks for taking a look. This is a small, single-file C daemon — contributions
are welcome and the bar to entry is low.

## The most useful thing you can contribute

**Test reports from hardware and distributions I don't have.** I develop this on
Bazzite with a Proteus dongle (`28de:1304`). If you have a Nereid dongle
(`1305`), a wired or BLE Triton (`1302`/`1303`), or you're on Arch / Debian /
NixOS / stock Fedora, a report either way is genuinely valuable — open a
[Device report](https://github.com/benashby/steam-puck-bridge/issues/new?template=device_report.yml)
issue.

Run this and paste the output:

```sh
steam-puck-bridge --version
uname -r
grep -il 28de /sys/class/hidraw/*/device/uevent | xargs -r grep -H HID_
steam-puck-bridge --dump   # a few seconds of it, with buttons pressed
```

## Reporting bugs

Open an issue with the [bug report template](https://github.com/benashby/steam-puck-bridge/issues/new/choose).
Please include a `--dump` capture — for anything input-related it is usually the
difference between a guess and a fix. See [docs/protocol.md](docs/protocol.md)
for what the bytes mean.

## Building and running

Requirements: `gcc` (or `clang`) and kernel UAPI headers. No libraries beyond
libc — please keep it that way. The zero-dependency property is a feature: it
lets the daemon run on immutable/atomic distros with no layering.

```sh
make                      # build
make check                # build with -Werror under gcc and clang
./steam-puck-bridge --dump
```

To test a change against a live controller, stop the service first so the debug
run owns the device:

```sh
systemctl --user stop steam-puck-bridge
./steam-puck-bridge
```

`make enable` installs to `~/.local` and starts the user unit.

## Code style

Match the surrounding code. Concretely:

- C99, 4-space indent, no tabs, ~78-column soft wrap.
- Linux kernel-ish brace style: opening brace on its own line for functions,
  same line for everything else.
- `snake_case` for functions and variables, `SCREAMING_CASE` for macros and
  protocol constants.
- Fixed-width types (`uint8_t`, `int16_t`) for anything that touches the wire.
- Comments explain *why*, not *what*. The protocol is undocumented by the
  vendor, so a comment naming the SDL3 source of a magic number is worth more
  than a comment restating the code.
- No new dependencies, no build system other than the `Makefile`.

There's an [`.editorconfig`](.editorconfig) that covers the mechanical parts.

## Protocol changes

Wire-format constants in this project are derived from SDL 3's
`SDL_hidapi_steam_triton.c` and `controller_{constants,structs}.h`
(zlib licensed, © Valve Corporation / Sam Lantinga). If you add or change a
report ID, offset, or setting number:

1. Document it in [docs/protocol.md](docs/protocol.md) in the same table style.
2. Cite where it came from — the SDL file and symbol name, or your own
   `--dump` capture showing the field changing.
3. Keep the attribution comments in `src/steam-puck-bridge.c` intact.

Do not copy SDL source code verbatim into this project. Constants, offsets and
field layouts are facts about the hardware; implementation is ours.

## Pull requests

- Branch from `main`, one logical change per PR.
- Run `make check` before pushing; CI runs the same thing under gcc and clang.
- Explain what you tested on: dongle PID, controller, distro, kernel.
- Add a `## [Unreleased]` entry to [CHANGELOG.md](CHANGELOG.md) for anything
  user-visible.
- By contributing you agree your work is licensed under the
  [MIT License](LICENSE).

## Scope

Things that fit this project:

- More Valve Triton-family device IDs.
- Correctness and robustness of the report parser.
- Making it work on more distributions without extra privileges.

Things that probably don't:

- Trackpad and IMU passthrough as *X360* axes. There is no sane mapping; the
  X360 profile has nowhere to put them, and consumers wouldn't know what to do
  with them. A separate opt-in virtual device is a reasonable proposal, but
  open an issue to discuss the shape before writing it.
- A GUI, a config file format, or a daemon-management layer. The systemd unit
  and two flags are deliberate.
- Rewrites in another language.

## Governance

One maintainer ([@benashby](https://github.com/benashby)), reviewing in spare
time. If a PR sits for a couple of weeks, a ping is welcome rather than rude.

All participation is covered by the [Code of Conduct](CODE_OF_CONDUCT.md).
Security issues go through [SECURITY.md](SECURITY.md), not the issue tracker.
