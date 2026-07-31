# Triton / Puck wire format reference

Everything here was extracted from SDL 3 (`3.5.0` development tree,
July 2026), files:

- `src/joystick/hidapi/SDL_hidapi_steam_triton.c`
- `src/joystick/hidapi/steam/controller_constants.h`
- `src/joystick/hidapi/steam/controller_structs.h`

Those headers are © Valve Corporation, zlib license. All structs are
`#pragma pack(1)`, all multi-byte fields little-endian.

## Device topology

| VID:PID | Device | HID interfaces |
|---|---|---|
| `28de:1302` | Triton, wired USB | any |
| `28de:1303` | Triton, BLE | any |
| `28de:1304` | **Proteus dongle** (the Puck receiver) | **2..5 = controller slots 1..4** |
| `28de:1305` | Nereid dongle | 2..5 |

In lizard mode each slot presents keyboard+mouse HID collections — which
is exactly what shows up in `/proc/bus/input/devices` on an untouched
system ("Valve Software Steam Controller Puck Mouse/Keyboard"), and why
the device seems gamepad-less.

## Feature reports (host → controller, HIDIOCSFEATURE)

64 bytes total. Byte 0 is HID report ID `0x01`, then:

```
offset  size  field
1       1     msg type
2       1     payload length
3...    …     payload
```

### Disable lizard mode

msg type `0x87` = `ID_SET_SETTINGS_VALUES`; payload is an array of
3-byte settings `{u8 settingNum, u16 settingValue}`:

```
01 87 03 09 00 00 [58 zero bytes]
      │  │  └──── value = LIZARD_MODE_OFF (0), u16 LE
      │  └─────── settingNum = SETTING_LIZARD_MODE (9)
      └────────── length = 3 = sizeof one setting
```

**Must be re-sent every ≤3 s** (SDL uses 3000 ms) — a firmware watchdog
restores lizard mode when the host goes quiet. This is also the recovery
mechanism: kill the bridge and the controller reverts to keyboard/mouse
on its own.

### Enable IMU (not used by the bridge, for reference)

Same shape: settingNum `48` = `SETTING_IMU_MODE`, value
`0x0018` = `SEND_RAW_ACCEL (0x08) | SEND_RAW_GYRO (0x10)`, or `0` = off.
Off is the default and saves wireless bandwidth/battery.

## Input reports (controller → host, read())

Byte 0 = report ID:

| ID | Meaning |
|---|---|
| `0x42` | controller state (USB/dongle) — `TritonMTUNoQuat_t` |
| `0x45` | controller state (BLE) — same layout |
| `0x47` | controller state, newer "Ibex" +trackpad timestamp — `TritonMTUNoQuat32TS_t` |
| `0x43` | battery status |
| `0x46` / `0x79` | wireless status: byte1 `1`=disconnect `2`=connect |

### State layout (offsets include the report-ID byte)

Common prefix — identical in `0x42`/`0x45`/`0x47`:

```
off  type  field
0    u8    report ID
1    u8    seq_num
2    u32   buttons            (bitfield below)
6    s16   sTriggerLeft       0..32767
8    s16   sTriggerRight      0..32767
10   s16   sLeftStickX        -32768..32767
12   s16   sLeftStickY        up-positive (negate for evdev)
14   s16   sRightStickX
16   s16   sRightStickY       up-positive
```

Then the variants diverge (trackpads + IMU):

`0x42`/`0x45` (`TritonMTUNoQuat_t`, 45 bytes + ID):
```
18 s16 sLeftPadX    20 s16 sLeftPadY    22 u16 unPressureLeft
24 s16 sRightPadX   26 s16 sRightPadY   28 u16 unPressureRight
30 u32 imu.timestamp (µs)
34 s16 accelX  36 s16 accelY  38 s16 accelZ
40 s16 gyroX   42 s16 gyroY   44 s16 gyroZ
```

`0x47` (`TritonMTUNoQuat32TS_t`, 45 bytes + ID): inserts
`u16 unTrackpadTimestamp` at 18, shifting pads +2; IMU timestamp shrinks
to `u16` in units of **32 µs**.

### Button bitfield

```
0x00000001 A            0x00000100 R5 paddle     0x00010000 STEAM
0x00000002 B            0x00000200 R bumper      0x00020000 L4 paddle
0x00000004 X            0x00000400 DPAD_DOWN     0x00040000 L5 paddle
0x00000008 Y            0x00000800 DPAD_RIGHT    0x00080000 L bumper
0x00000010 QAM (⋯)      0x00001000 DPAD_LEFT     0x00100000 R-stick touch
0x00000020 R3           0x00002000 DPAD_UP       0x00200000 R-pad touch
0x00000040 VIEW         0x00004000 MENU          0x00400000 R-pad click
0x00000080 R4 paddle    0x00008000 L3            0x00800000 R-trigger click
0x01000000 L-stick touch   0x04000000 L-pad click     0x10000000 R-grip touch
0x02000000 L-pad touch     0x08000000 L-trig click    0x20000000 L-grip touch
```

SDL3 maps MENU→Back and VIEW→Start (mirrored by the bridge).

### Scaling used by SDL3 (adopted by the bridge where applicable)

- Triggers → SDL full-range: `v*2 - 32768`; bridge → X360 0..255: `v >> 7`
- Stick Y negated (report is up-positive, SDL/evdev want up-negative)
- Trackpad: `x/65536 + 0.5`, `-y/65536 + 0.5`, pressure `/32768`
- Gyro: `v/32768 * 2000 °/s`; accel: `v/32768 * 2 g` — with an axis swap
  (SDL emits X, Z, −Y)

## Output reports (host → controller, write())

Report ID `0x80` = haptic rumble, 10 bytes total:

```
0  u8  0x80
1  u8  type       (0)
2  u16 intensity  (0)
4  u16 left.speed     ← low-frequency / FF strong
6  s8  left.gain dB   (0)
7  u16 right.speed    ← high-frequency / FF weak
9  s8  right.gain dB  (0)
```

Hardware safety timeout stops rumble after ~50 ms — re-send every 40 ms
while an effect is active (SDL's `TRITON_RUMBLE_RESEND_INTERVAL_MS`).

Other haptic report IDs exist (`0x81` pulse, `0x82` command, `0x83` LFO
tone, `0x84` log sweep, `0x85` script) — not used by the bridge.

## Battery report (`0x43`)

```
1 u8  charge state: 0 reset, 1 discharging, 2 charging, 3 src-validate, 4 done
2 u8  battery level %
3 u16 battery mV   5 u16 system mV   7 u16 input mV
9 u16 current      11 u16 input current   13 u16 temperature
```
