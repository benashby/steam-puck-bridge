# steam-puck-bridge — Steam Controller 2 ("Puck") driver for Linux without Steam

[![CI](https://github.com/benashby/steam-puck-bridge/actions/workflows/ci.yml/badge.svg)](https://github.com/benashby/steam-puck-bridge/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platform: Linux](https://img.shields.io/badge/platform-Linux-informational)
![Dependencies: none](https://img.shields.io/badge/dependencies-libc%20only-brightgreen)

Userspace driver that makes the 2nd-generation **Steam Controller** (the
"Puck" wireless dongle, `28de:1304`) work as a normal gamepad on Linux
**without Steam running**.

> **Is this your problem?** Your Steam Controller Puck shows up as
> "Valve Software Steam Controller Puck Mouse" and "…Keyboard" in
> `/proc/bus/input/devices`, the sticks move the mouse cursor instead of
> the game, `jstest`/`evtest` see no joystick, and everything is fine the
> moment you launch Steam. That's **lizard mode**, and this fixes it.

It disables the controller's firmware keyboard/mouse fallback ("lizard
mode"), reads the raw vendor HID reports, and exposes a virtual
**Microsoft X-Box 360 pad** through `/dev/uinput` — so every game,
frontend, and library on the system sees a fully-mapped controller with
zero per-app configuration. Rumble works (FF_RUMBLE → Triton haptics).

Developed on Bazzite (Fedora Atomic), where it powers controller input
for an ES-DE HTPC "game mode" setup — but nothing in it is
distro-specific beyond two udev rules (see
[Permissions](#permissions--security-notes)).

## Quick start

```sh
git clone https://github.com/benashby/steam-puck-bridge
cd steam-puck-bridge
make enable          # build, install to ~/.local, start the systemd user unit
```

On a distro that doesn't already ship Valve's `steam-devices` udev rules
(anything other than Bazzite / SteamOS / ChimeraOS / Nobara, roughly), also:

```sh
sudo make install-udev
# then unplug and replug the dongle
```

Turn the controller on. `/proc/bus/input/devices` should gain a
`Microsoft X-Box 360 pad`. See [Build & install](#build--install) for the
details and [Debugging](#debugging) when it doesn't.

## Why this needs to exist

The controller ships in **lizard mode**: the firmware emulates a USB
keyboard + mouse so it "works" with no drivers. Real gamepad reports only
flow after the host sends a vendor HID command — and keeps re-sending it,
because a firmware watchdog re-enables lizard mode after a few seconds of
silence. Normally the Steam client does this. Without Steam, the state of
the ecosystem is (**last verified 2026-08-08** — if this has changed,
please [open an issue](https://github.com/benashby/steam-puck-bridge/issues),
I would be glad to retire this project):

| Layer | Supports the Puck (`28de:1304`)? |
|---|---|
| Kernel `hid-steam` (incl. mainline) | ❌ binds only `1102`/`1142`/`1205` (SC1 wired/dongle, Deck) — `hid-generic` claims the Puck and it stays in lizard mode |
| SDL2 (2.32.x) | ❌ hidapi Steam driver covers SC1 only |
| SDL3 ≥ 3.5 | ✅ full driver (`SDL_hidapi_steam_triton.c`) — but only helps SDL3 apps, and e.g. ES-DE links SDL2 |
| [SteamlessController](https://github.com/ddeverill/SteamlessController) | ❌ Windows-only (ViGEmBus kernel driver, Win32, WinRT) — but its README documents the lizard-mode mechanism nicely |

A uinput bridge is the one fix that covers everything at once: evdev is
the layer every consumer reads (SDL2, SDL3, Proton/Wine, browsers…), and
a virtual device with the X360 identity (`045e:028e`, kernel `xpad`
evdev profile) is universally auto-mapped. This is the same approach
ViGEmBus-based tools take on Windows and `xboxdrv` took historically on
Linux.

## How it works

```
   Steam Controller Puck (USB dongle 28de:1304)
   ├─ USB interfaces 2..5 = 4 controller slots (hidraw, via hid-generic)
   │
   │   every 3 s:  SET_SETTINGS_VALUES { LIZARD_MODE = OFF }   (feature report)
   │   ~250 Hz  :  Triton state reports (buttons/sticks/triggers/pads/IMU)
   │   on rumble:  haptic-rumble output report, re-sent every 40 ms
   ▼
 steam-puck-bridge (this daemon, plain C, no deps, unprivileged)
   ▼
 /dev/uinput  →  virtual "Microsoft X-Box 360 pad" (one per connected controller)
   ▼
 ES-DE, games, Proton, anything evdev/SDL
```

Specifics:

- **Discovery:** walks `/sys/class/hidraw`, matches `HID_ID` for Valve
  VID `28DE` with PID `1304`/`1305` (dongles — only USB interfaces 2–5,
  which are the controller slots) or `1302`/`1303` (wired/BLE Triton).
  No libudev; hotplug is handled by rescanning when reads fail or no
  device is present.
- **Connect/disconnect:** the dongle sends explicit wireless-status
  reports (`0x46`/`0x79`: byte = 1 disconnect, 2 connect). The virtual
  pad is created/destroyed to match, so no ghost controllers linger.
- **State reports** (`0x42` USB, `0x45` BLE, `0x47` newer +timestamps):
  buttons (u32 bitfield), triggers (0..32767 → ABS_Z/ABS_RZ 0..255),
  both sticks (s16, up-positive → negated for evdev), d-pad bits → hat.
  Stick/trigger/button offsets are identical across all three variants;
  only trackpad/IMU fields differ, and those have no X360 equivalent so
  the bridge doesn't consume them (SDL3 apps can't see them through an
  X360 profile anyway).
- **Paddles & QAM:** L4/L5/R4/R5 and the QAM (⋯) button are emitted as
  `BTN_TRIGGER_HAPPY1..5` — invisible to standard X360 mappings, but
  bindable in anything that does raw evdev (Steam Input–style remappers,
  `keyd`, etc.).
- **Menu/View:** mapped to Back/Start following SDL3's own choice for
  this hardware.
- **Rumble:** FF_RUMBLE effects are accepted via the uinput FF upload
  protocol; strong→left/low motor, weak→right/high motor, scaled by
  FF_GAIN, re-sent every 40 ms while active (the hardware auto-stops
  after ~50 ms as a safety timeout).
- **Steam coexistence:** if a process named `steam` appears, the bridge
  closes everything and waits; Steam gets the raw device (it speaks this
  protocol natively) and lizard-mode control. When Steam exits the bridge
  reclaims the controller. Disable with `--no-steam-check`.

The protocol details were extracted from SDL 3's
`src/joystick/hidapi/SDL_hidapi_steam_triton.c` and
`src/joystick/hidapi/steam/controller_{constants,structs}.h`
(zlib license, © Valve Corporation / Sam Lantinga) — see
[docs/protocol.md](docs/protocol.md) for the full wire format reference.

## Build & install

Requirements: gcc and kernel UAPI headers — nothing else, no libraries
beyond libc. (Both are present on the Bazzite base image, so no
rpm-ostree layering is needed there.)

```sh
make               # build
make install       # → ~/.local/bin/steam-puck-bridge + systemd user unit
make enable        # install + systemctl --user enable --now
sudo make install-udev   # device access rules, if your distro lacks them
make uninstall     # remove
make help          # all targets and variables
```

`PREFIX`, `BINDIR`, `UNITDIR`, `UDEVDIR`, `DESTDIR` and `CC`/`CFLAGS` are all
overridable, so packaging is a normal staged install:

```sh
make install DESTDIR="$pkgdir" PREFIX=/usr \
             UNITDIR=/usr/lib/systemd/user SKIP_RELOAD=1
```

Check it's alive:

```sh
systemctl --user status steam-puck-bridge
journalctl --user -u steam-puck-bridge -f
```

With the controller on, `/proc/bus/input/devices` gains a
`Microsoft X-Box 360 pad` entry and ES-DE/games pick it up immediately.

## Debugging

```sh
# stop the service so the debug run owns the device
systemctl --user stop steam-puck-bridge

# raw protocol dump: hexdumps every report + a parsed summary line
~/.local/bin/steam-puck-bridge --dump
```

In dump mode no virtual pad is created. Press buttons / move sticks and
compare against the field table in [docs/protocol.md](docs/protocol.md).

Common issues:

- **No slots found:** dongle unplugged, or udev rules missing. Check that
  the kernel sees it and that you can open it:
  ```sh
  grep -il 28de /sys/class/hidraw/*/device/uevent | xargs -r grep -H HID_
  ls -l /dev/hidraw*
  ```
  If the devices exist but you can't open them, run `sudo make install-udev`.
- **`open /dev/uinput failed`:** the uaccess ACL is missing — are you
  running as the active graphical session user? (`getfacl /dev/uinput`).
  `sudo make install-udev` ships a rule for this too.
- **Controller does nothing until touched:** normal — the puck sleeps;
  press the Steam button to wake it, the dongle then sends CONNECT.
- **Everything dead while Steam is open:** by design (see above).

## Permissions / security notes

Runs fully unprivileged — no root, no setuid, no capabilities. It relies on
two facts of the Bazzite image: `/dev/hidraw*` is 0666 (Valve's
`steam-devices` rules ship in the image) and `/dev/uinput` gets a uaccess
ACL for the seated user (`60-steam-input.rules`).

On distros without those,
[`udev/60-steam-puck-bridge.rules`](udev/60-steam-puck-bridge.rules)
(`sudo make install-udev`) grants the same access via `uaccess` — the
locally logged-in user only, rather than world-writable.

The systemd unit applies the usual sandboxing (`ProtectSystem=strict`,
`NoNewPrivileges`, `SystemCallFilter=@system-service`, …). Note that a
uinput device is an input-injection primitive: a compromised bridge could
synthesize keystrokes into whatever has focus. See
[SECURITY.md](SECURITY.md) for the threat model and how to report issues.

Masquerading as `045e:028e` is cosmetic-but-load-bearing: SDL and games
key their built-in mappings off the evdev vendor/product/version, and the
X360 triple is the one identity everything knows. The kernel `xpad`
driver does not bind uinput devices, so there's no conflict.

## FAQ

**Does this work with the Steam Deck's built-in controls, or the original
2015 Steam Controller?**
No. Those are `28de:1205` and `28de:1102`/`1142`, and the kernel's
`hid-steam` driver already handles them. This is only for the Triton family
(`1302`–`1305`).

**Do I need to uninstall it once Steam is running?**
No. The bridge detects a running Steam client and backs off automatically,
handing the raw device back. It reclaims the controller when Steam exits.

**Does the trackpad or gyro work?**
The reports are parsed but not forwarded. There is no X360 axis to put them
on, and consumers reading an X360 profile wouldn't know what to do with
them. See [Scope in CONTRIBUTING.md](CONTRIBUTING.md#scope).

**Will this conflict with the kernel `xpad` driver?**
No — `xpad` binds USB devices, not uinput ones.

**Can I use it on a headless machine / over SSH?**
Only if you can get access to `/dev/uinput` and `/dev/hidraw*`, which the
`uaccess` rules grant to the *seated* user. Without a local session you'll
need your own rules.

**Why not just patch `hid-steam`?**
That would be the right long-term fix and someone should do it. This is
userspace, needs no kernel build, no signed module, and works today on
immutable distros where layering a module is painful.

## Related projects

| Project | Platform | Notes |
|---|---|---|
| [`hid-steam`](https://www.kernel.org/doc/html/latest/) (in-kernel) | Linux | Handles SC1 and the Steam Deck. Doesn't bind the Puck. |
| [SDL 3](https://github.com/libsdl-org/SDL) `SDL_hidapi_steam_triton.c` | any | Full driver, but only SDL3 apps benefit. Source of this project's protocol reference. |
| [SteamlessController](https://github.com/ddeverill/SteamlessController) | Windows | Same idea via ViGEmBus. Its README explains lizard mode well. |
| [`xboxdrv`](https://github.com/xboxdrv/xboxdrv) | Linux | Historical precedent for the userspace-driver-to-uinput pattern. |
| [`sc-controller`](https://github.com/C0rn3j/sc-controller) | Linux | GUI mapper for the original Steam Controller. |

## Contributing

Yes please — especially **test reports from hardware and distros I don't
have** (Nereid dongles, wired/BLE Tritons, Arch/Debian/NixOS). See
[CONTRIBUTING.md](CONTRIBUTING.md), and note the
[device report issue template](https://github.com/benashby/steam-puck-bridge/issues/new?template=device_report.yml).

Participation is covered by the [Code of Conduct](CODE_OF_CONDUCT.md).
Security issues go through [SECURITY.md](SECURITY.md), not the issue
tracker. Release notes live in [CHANGELOG.md](CHANGELOG.md).

## Repo layout

```
src/steam-puck-bridge.c              the whole daemon (~850 lines, plain C)
systemd/steam-puck-bridge.service    user unit (WantedBy=default.target)
udev/60-steam-puck-bridge.rules      device access for non-Valve-rules distros
Makefile                             build / install / enable / uninstall / check
docs/protocol.md                     Triton wire-format reference
.github/workflows/ci.yml             gcc + clang builds, cppcheck, unit lint
```

## License

[MIT](LICENSE). The wire-format constants and struct layouts are derived
from SDL 3's zlib-licensed Steam Triton driver, © Valve Corporation /
Sam Lantinga — see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and the
attribution comments in `src/steam-puck-bridge.c` and `docs/protocol.md`.

"Steam", "Steam Controller" and "Steam Deck" are trademarks of Valve
Corporation. This is an independent, unofficial project, not endorsed by or
affiliated with Valve.
