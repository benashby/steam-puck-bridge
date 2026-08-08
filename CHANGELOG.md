# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-08-08

First public release.

### Added

- hidraw → uinput bridge for the 2nd-generation Steam Controller "Puck"
  (Proteus dongle `28de:1304`, Nereid dongle `28de:1305`, wired `28de:1302`,
  BLE `28de:1303`).
- Lizard-mode suppression with a 3-second heartbeat, so the controller keeps
  emitting real gamepad reports instead of firmware keyboard/mouse events.
- Triton state-report parsing for report IDs `0x42` (USB), `0x45` (BLE) and
  `0x47` (newer, with timestamps): buttons, both sticks, both analog triggers,
  d-pad.
- One virtual **Microsoft X-Box 360 pad** (`045e:028e`) per connected controller
  slot, created and destroyed to follow the dongle's wireless connect/disconnect
  reports so no ghost devices linger.
- `FF_RUMBLE` force feedback translated to Triton haptic-rumble output reports,
  re-sent every 40 ms, scaled by `FF_GAIN`.
- Back paddles (L4/L5/R4/R5) and the QAM button exposed as
  `BTN_TRIGGER_HAPPY1..5` for raw-evdev remappers.
- Automatic back-off while a real Steam client is running, with reclaim on exit
  (`--no-steam-check` to disable).
- Dongle hotplug handling by rescanning `/sys/class/hidraw`; no libudev
  dependency.
- `--dump` mode: hexdump plus parsed summary of every report, no uinput device
  created.
- `--help` and `--version`.
- systemd **user** unit, `Makefile` with `install` / `enable` / `uninstall`,
  and udev rules for distributions that don't ship Valve's `steam-devices`.

[Unreleased]: https://github.com/benashby/steam-puck-bridge/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/benashby/steam-puck-bridge/releases/tag/v0.1.0
