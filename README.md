# steam-puck-bridge

Userspace driver that makes the 2nd-generation **Steam Controller** (the
"Puck" wireless dongle, `28de:1304`) work as a normal gamepad on Linux
**without Steam running**.

It disables the controller's firmware keyboard/mouse fallback ("lizard
mode"), reads the raw vendor HID reports, and exposes a virtual
**Microsoft X-Box 360 pad** through `/dev/uinput` — so every game,
frontend, and library on the system sees a fully-mapped controller with
zero per-app configuration. Rumble works (FF_RUMBLE → Triton haptics).

Developed on Bazzite (Fedora Atomic), where it powers controller input
for an ES-DE HTPC "game mode" setup — but nothing in it is
distro-specific beyond two udev rules (see
[Permissions](#permissions--security-notes)).

## Why this needs to exist (as of July 2026)

The controller ships in **lizard mode**: the firmware emulates a USB
keyboard + mouse so it "works" with no drivers. Real gamepad reports only
flow after the host sends a vendor HID command — and keeps re-sending it,
because a firmware watchdog re-enables lizard mode after a few seconds of
silence. Normally the Steam client does this. Without Steam, the state of
the ecosystem is:

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
make            # build
make install    # → ~/.local/bin/steam-puck-bridge + systemd user unit
make enable     # install + systemctl --user enable --now
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

- **No slots found:** dongle unplugged, or udev rules missing (check
  `ls -l /dev/hidraw*` — should be world-writable on Bazzite).
- **`open /dev/uinput failed`:** the uaccess ACL is missing — are you
  running as the active graphical session user? (`getfacl /dev/uinput`)
- **Controller does nothing until touched:** normal — the puck sleeps;
  press the Steam button to wake it, the dongle then sends CONNECT.
- **Everything dead while Steam is open:** by design (see above).

## Permissions / security notes

Runs fully unprivileged. It relies on two facts of the Bazzite image:
`/dev/hidraw*` is 0666 (Valve's `steam-devices` rules ship in the image)
and `/dev/uinput` gets a uaccess ACL for the seated user
(`60-steam-input.rules`). On a stock Fedora you'd need equivalent udev
rules.

Masquerading as `045e:028e` is cosmetic-but-load-bearing: SDL and games
key their built-in mappings off the evdev vendor/product/version, and the
X360 triple is the one identity everything knows. The kernel `xpad`
driver does not bind uinput devices, so there's no conflict.

## Repo layout

```
src/steam-puck-bridge.c        the whole daemon (~700 lines, plain C)
systemd/steam-puck-bridge.service   user unit (WantedBy=default.target)
Makefile                       build/install/enable/uninstall
docs/protocol.md               Triton wire-format reference
```

## License

[MIT](LICENSE). The wire-format constants and struct layouts are derived
from SDL 3's zlib-licensed Steam Triton driver, © Valve Corporation /
Sam Lantinga — see the attribution notes in `src/steam-puck-bridge.c`
and `docs/protocol.md`.
