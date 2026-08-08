/* SPDX-License-Identifier: MIT
 *
 * steam-puck-bridge — userspace driver for the Steam Controller "Puck"
 * (Valve Proteus/Nereid wireless dongle, and wired/BLE Triton controllers).
 *
 * Copyright (c) 2026 Ben Ashby
 *
 * WHY THIS EXISTS
 * ---------------
 * The 2nd-gen Steam Controller ships in "lizard mode": firmware-level
 * keyboard/mouse emulation so it works with zero drivers. Something has to
 * send it a vendor HID command to switch on real gamepad reports. Normally
 * that something is the Steam client. Outside Steam, on this machine:
 *
 *   - kernel hid-steam only binds PIDs 0x1102/0x1142/0x1205 (old SC + Deck),
 *     not the Puck dongle's 0x1304 — so hid-generic claims it and nothing
 *     ever leaves lizard mode (verified against mainline drivers/hid/hid-steam.c)
 *   - SDL3 >= 3.5 has a full hidapi driver for it (SDL_hidapi_steam_triton.c)
 *     but SDL2 apps (ES-DE among them) and non-SDL apps get nothing
 *
 * This daemon does what SDL3's driver does, but at the evdev layer so that
 * EVERY consumer benefits: it opens the dongle's raw HID interfaces, disables
 * lizard mode (and keeps it disabled — the firmware watchdog re-enables it
 * after a few seconds of silence), parses the Triton input reports, and
 * feeds a virtual "Microsoft X-Box 360 pad" through /dev/uinput. The Linux
 * analogue of what ViGEmBus does for SteamlessController on Windows.
 *
 * The protocol (report IDs, struct layouts, setting numbers, the 3-second
 * lizard heartbeat, the rumble output report) is derived from SDL 3's
 * src/joystick/hidapi/SDL_hidapi_steam_triton.c and
 * src/joystick/hidapi/steam/controller_{constants,structs}.h
 * (zlib licensed, Copyright (C) Valve Corporation / Sam Lantinga).
 *
 * PRIVILEGES: none needed on Bazzite. /dev/hidraw* is 0666 via Valve's
 * steam-devices udev rules and /dev/uinput carries a uaccess ACL for the
 * active seat (60-steam-input.rules). Run it as your user.
 *
 * BEHAVIOR
 * --------
 *   - scans /sys/class/hidraw for VID 28DE, PID 1304/1305 (dongle: USB
 *     interfaces 2..5 = 4 controller slots) or PID 1302/1303 (wired/BLE)
 *   - one virtual X360 pad per slot, created on controller connect and
 *     destroyed on disconnect (so no ghost pads linger in ES-DE/games)
 *   - FF_RUMBLE force feedback is translated to Triton haptic-rumble output
 *     reports, re-sent every 40 ms (hardware safety timeout is ~50 ms)
 *   - if a real Steam client starts, the bridge backs off completely (Steam
 *     wants the hidraw device itself) and resumes when Steam exits
 *   - hotplug of the dongle handled by rescan; no libudev dependency
 *
 * USAGE
 *   steam-puck-bridge            run (logs to stdout; use the systemd unit)
 *   steam-puck-bridge --dump     protocol debug: hexdump + parse all reports,
 *                                no uinput device is created
 *   steam-puck-bridge --no-steam-check   don't pause when Steam is running
 */

#define _GNU_SOURCE

#ifndef SPB_VERSION
#define SPB_VERSION "0.1.0"
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <linux/uinput.h>

/* ------------------------------------------------------------------ */
/* Protocol constants — from SDL3 steam/controller_{constants,structs}.h */

#define VALVE_VID              0x28DE
#define PID_TRITON_WIRED       0x1302
#define PID_TRITON_BLE         0x1303
#define PID_PROTEUS_DONGLE     0x1304
#define PID_NEREID_DONGLE      0x1305

#define HID_FEATURE_REPORT_BYTES 64
#define FEATURE_REPORT_ID        0x01
#define ID_SET_SETTINGS_VALUES   0x87
#define SETTING_LIZARD_MODE      9      /* positional in settings enum */
#define LIZARD_MODE_OFF          0
#define LIZARD_HEARTBEAT_MS      3000   /* SDL re-sends every 3000 ms; the
                                           firmware watchdog re-enables lizard
                                           mode if the host goes quiet */

/* input report IDs (first byte of every hidraw read) */
#define ID_TRITON_CONTROLLER_STATE            0x42
#define ID_TRITON_BATTERY_STATUS              0x43
#define ID_TRITON_CONTROLLER_STATE_BLE        0x45
#define ID_TRITON_WIRELESS_STATUS_X           0x46
#define ID_TRITON_CONTROLLER_STATE_TIMESTAMP  0x47
#define ID_TRITON_WIRELESS_STATUS             0x79

#define TRITON_WIRELESS_DISCONNECT 1
#define TRITON_WIRELESS_CONNECT    2

/* TritonMTUNoQuat_t field offsets, relative to the report-ID byte.
 * Buttons/triggers/sticks sit at identical offsets in the 0x42/0x45 layout
 * and the 0x47 (+trackpad-timestamp) layout — only trackpad/IMU fields move,
 * and this bridge doesn't consume those. All fields little-endian, packed. */
#define OFF_BUTTONS   2   /* u32  */
#define OFF_TRIG_L    6   /* s16  0..32767 */
#define OFF_TRIG_R    8   /* s16  0..32767 */
#define OFF_LSTICK_X 10   /* s16  */
#define OFF_LSTICK_Y 12   /* s16  up-positive */
#define OFF_RSTICK_X 14   /* s16  */
#define OFF_RSTICK_Y 16   /* s16  up-positive */
#define STATE_MIN_LEN 18  /* enough for everything we parse */

/* TritonButtons bit definitions */
#define BTN_TRITON_A            0x00000001u
#define BTN_TRITON_B            0x00000002u
#define BTN_TRITON_X            0x00000004u
#define BTN_TRITON_Y            0x00000008u
#define BTN_TRITON_QAM          0x00000010u
#define BTN_TRITON_R3           0x00000020u
#define BTN_TRITON_VIEW         0x00000040u
#define BTN_TRITON_R4           0x00000080u
#define BTN_TRITON_R5           0x00000100u
#define BTN_TRITON_R            0x00000200u
#define BTN_TRITON_DPAD_DOWN    0x00000400u
#define BTN_TRITON_DPAD_RIGHT   0x00000800u
#define BTN_TRITON_DPAD_LEFT    0x00001000u
#define BTN_TRITON_DPAD_UP      0x00002000u
#define BTN_TRITON_MENU         0x00004000u
#define BTN_TRITON_L3           0x00008000u
#define BTN_TRITON_STEAM        0x00010000u
#define BTN_TRITON_L4           0x00020000u
#define BTN_TRITON_L5           0x00040000u
#define BTN_TRITON_L            0x00080000u
/* touch/click bits beyond these exist (touchpads, grips) but an X360
 * profile has nowhere meaningful to put them */

/* rumble output report — MsgHapticRumble behind report ID 0x80, 10 bytes */
#define ID_OUT_REPORT_HAPTIC_RUMBLE 0x80
#define RUMBLE_REPORT_BYTES         10
#define RUMBLE_RESEND_MS            40

/* ------------------------------------------------------------------ */

#define MAX_SLOTS   4      /* Proteus dongle: USB interfaces 2..5 */
#define MAX_EFFECTS 16

typedef struct {
    uint16_t strong;       /* FF_RUMBLE strong magnitude (left/low motor)  */
    uint16_t weak;         /* FF_RUMBLE weak magnitude  (right/high motor) */
    uint16_t length_ms;    /* replay length, 0 = play until stopped */
    bool     used;
} ff_effect_slot;

typedef struct {
    char hidraw_path[272];
    int  iface;            /* USB interface number */
    int  hid_fd;           /* /dev/hidrawN, -1 when closed */
    int  ui_fd;            /* /dev/uinput virtual pad, -1 when absent */
    bool connected;        /* controller present on this slot */

    uint64_t last_lizard_ms;
    uint64_t last_rumble_ms;
    uint64_t rumble_until_ms;   /* 0 = no deadline (infinite) */
    bool     rumble_active;
    uint16_t rumble_strong, rumble_weak;
    uint16_t ff_gain;           /* 0..0xFFFF, default max */
    ff_effect_slot effects[MAX_EFFECTS];

    /* last emitted state, to skip redundant events */
    uint32_t last_buttons;
    int32_t  last_abs[6];       /* X Y RX RY Z RZ */
    int32_t  last_hat[2];
} slot_t;

static slot_t slots[MAX_SLOTS];
static int nslots = 0;
static bool opt_dump = false;
static bool opt_steam_check = true;
static volatile sig_atomic_t running = 1;

static void on_signal(int sig) { (void)sig; running = 0; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

static void logts(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

static int16_t rd16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/* Device discovery: walk /sys/class/hidraw, match VID/PID, and pull the
 * USB interface number out of HID_PHYS ("usb-....-2/input3" → 3). */

static bool parse_uevent(const char *hidraw_name, uint16_t *vid, uint16_t *pid, int *iface)
{
    char path[300], line[256];
    snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", hidraw_name);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;

    bool have_id = false;
    *iface = -1;
    while (fgets(line, sizeof(line), f)) {
        unsigned bus, v, p;
        if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &v, &p) == 3) {
            *vid = (uint16_t)v;
            *pid = (uint16_t)p;
            have_id = true;
        } else if (strncmp(line, "HID_PHYS=", 9) == 0) {
            const char *in = strstr(line, "/input");
            if (in)
                *iface = atoi(in + 6);
        }
    }
    fclose(f);
    return have_id;
}

static bool is_bridge_target(uint16_t vid, uint16_t pid, int iface)
{
    if (vid != VALVE_VID)
        return false;
    if (pid == PID_PROTEUS_DONGLE || pid == PID_NEREID_DONGLE)
        return iface >= 2 && iface <= 5;   /* controller slots — SDL claims exactly these */
    if (pid == PID_TRITON_WIRED || pid == PID_TRITON_BLE)
        return true;
    return false;
}

static int scan_devices(void)
{
    DIR *d = opendir("/sys/class/hidraw");
    nslots = 0;
    if (!d)
        return 0;

    struct dirent *e;
    while ((e = readdir(d)) != NULL && nslots < MAX_SLOTS) {
        if (strncmp(e->d_name, "hidraw", 6) != 0)
            continue;
        uint16_t vid, pid;
        int iface;
        if (!parse_uevent(e->d_name, &vid, &pid, &iface))
            continue;
        if (!is_bridge_target(vid, pid, iface))
            continue;

        slot_t *s = &slots[nslots];
        memset(s, 0, sizeof(*s));
        snprintf(s->hidraw_path, sizeof(s->hidraw_path), "/dev/%s", e->d_name);
        s->iface = iface;
        s->hid_fd = -1;
        s->ui_fd = -1;
        s->ff_gain = 0xFFFF;

        s->hid_fd = open(s->hidraw_path, O_RDWR | O_NONBLOCK);
        if (s->hid_fd < 0) {
            logts("open %s failed: %m", s->hidraw_path);
            continue;
        }
        logts("slot %d: %s (pid %04x, usb interface %d)",
              nslots, s->hidraw_path, pid, iface);
        nslots++;
    }
    closedir(d);
    return nslots;
}

/* ------------------------------------------------------------------ */
/* Lizard mode disable — 64-byte feature report, ID 0x01:
 * SET_SETTINGS_VALUES { SETTING_LIZARD_MODE = LIZARD_MODE_OFF } */

static void send_lizard_off(slot_t *s)
{
    uint8_t buf[HID_FEATURE_REPORT_BYTES] = { 0 };
    buf[0] = FEATURE_REPORT_ID;
    buf[1] = ID_SET_SETTINGS_VALUES;
    buf[2] = 3;                        /* length = sizeof(ControllerSetting) */
    buf[3] = SETTING_LIZARD_MODE;
    buf[4] = LIZARD_MODE_OFF & 0xFF;   /* u16 LE value */
    buf[5] = 0;

    /* Feature report to an empty slot fails; that's expected and harmless. */
    ioctl(s->hid_fd, HIDIOCSFEATURE(sizeof(buf)), buf);
    s->last_lizard_ms = now_ms();
}

/* ------------------------------------------------------------------ */
/* Rumble: Triton haptic-rumble output report. SDL maps SDL low-frequency
 * rumble → left.speed and high-frequency → right.speed; FF_RUMBLE's
 * strong/weak magnitudes correspond respectively. */

static void send_rumble(slot_t *s, uint16_t left, uint16_t right)
{
    uint8_t buf[RUMBLE_REPORT_BYTES] = { 0 };
    buf[0] = ID_OUT_REPORT_HAPTIC_RUMBLE;
    /* [1] type = 0, [2..3] intensity = 0 */
    buf[4] = left & 0xFF;
    buf[5] = left >> 8;
    /* [6] left gain = 0 dB */
    buf[7] = right & 0xFF;
    buf[8] = right >> 8;
    /* [9] right gain = 0 dB */

    if (write(s->hid_fd, buf, sizeof(buf)) < 0 && errno != EAGAIN)
        logts("rumble write on %s: %m", s->hidraw_path);
    s->last_rumble_ms = now_ms();
}

/* ------------------------------------------------------------------ */
/* Virtual pad. We masquerade as a Microsoft X-Box 360 pad (045e:028e) with
 * the exact evdev profile the kernel xpad driver exposes. Every controller
 * consumer on Linux — SDL2's udev joystick backend included — has a native,
 * complete mapping for that GUID, so buttons/axes are correct everywhere
 * with zero per-app configuration. Paddles/QAM ride along as
 * BTN_TRIGGER_HAPPY1..5, which mappers can bind and everything else ignores. */

static const int uinput_keys[] = {
    BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR,
    BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR,
    BTN_TRIGGER_HAPPY1, BTN_TRIGGER_HAPPY2, BTN_TRIGGER_HAPPY3,
    BTN_TRIGGER_HAPPY4, BTN_TRIGGER_HAPPY5,
};

static int create_uinput(void)
{
    int fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        logts("open /dev/uinput failed: %m (is the uaccess ACL present?)");
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_FF);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);

    for (size_t i = 0; i < sizeof(uinput_keys) / sizeof(uinput_keys[0]); i++)
        ioctl(fd, UI_SET_KEYBIT, uinput_keys[i]);

    const int axes[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ,
                         ABS_HAT0X, ABS_HAT0Y };
    for (size_t i = 0; i < sizeof(axes) / sizeof(axes[0]); i++)
        ioctl(fd, UI_SET_ABSBIT, axes[i]);

    ioctl(fd, UI_SET_FFBIT, FF_RUMBLE);
    ioctl(fd, UI_SET_FFBIT, FF_GAIN);

    struct uinput_user_dev ud;
    memset(&ud, 0, sizeof(ud));
    snprintf(ud.name, UINPUT_MAX_NAME_SIZE, "Microsoft X-Box 360 pad");
    ud.id.bustype = BUS_USB;
    ud.id.vendor  = 0x045e;
    ud.id.product = 0x028e;
    ud.id.version = 0x0110;
    ud.ff_effects_max = MAX_EFFECTS;

    const int stick[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY };
    for (size_t i = 0; i < 4; i++) {
        ud.absmin[stick[i]]  = -32768;
        ud.absmax[stick[i]]  = 32767;
        ud.absfuzz[stick[i]] = 16;
        ud.absflat[stick[i]] = 128;
    }
    ud.absmin[ABS_Z]  = 0;   ud.absmax[ABS_Z]  = 255;
    ud.absmin[ABS_RZ] = 0;   ud.absmax[ABS_RZ] = 255;
    ud.absmin[ABS_HAT0X] = -1; ud.absmax[ABS_HAT0X] = 1;
    ud.absmin[ABS_HAT0Y] = -1; ud.absmax[ABS_HAT0Y] = 1;

    if (write(fd, &ud, sizeof(ud)) != sizeof(ud) ||
        ioctl(fd, UI_DEV_CREATE) < 0) {
        logts("uinput device creation failed: %m");
        close(fd);
        return -1;
    }
    return fd;
}

static void destroy_uinput(slot_t *s)
{
    if (s->ui_fd >= 0) {
        ioctl(s->ui_fd, UI_DEV_DESTROY);
        close(s->ui_fd);
        s->ui_fd = -1;
    }
    s->rumble_active = false;
    memset(s->effects, 0, sizeof(s->effects));
}

static void emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) < 0) { /* non-fatal */ }
}

static void set_connected(slot_t *s, bool connected)
{
    if (s->connected == connected)
        return;
    s->connected = connected;

    if (connected) {
        logts("controller connected on %s (interface %d)",
              s->hidraw_path, s->iface);
        send_lizard_off(s);
        if (!opt_dump && s->ui_fd < 0) {
            s->ui_fd = create_uinput();
            s->last_buttons = 0;
            memset(s->last_abs, 0, sizeof(s->last_abs));
            memset(s->last_hat, 0, sizeof(s->last_hat));
            if (s->ui_fd >= 0)
                logts("virtual X-Box 360 pad created for %s", s->hidraw_path);
        }
    } else {
        logts("controller disconnected on %s", s->hidraw_path);
        destroy_uinput(s);
    }
}

/* ------------------------------------------------------------------ */
/* State report → evdev translation */

struct btn_map { uint32_t bit; uint16_t key; };
static const struct btn_map button_map[] = {
    { BTN_TRITON_A,     BTN_A       },
    { BTN_TRITON_B,     BTN_B       },
    { BTN_TRITON_X,     BTN_X       },
    { BTN_TRITON_Y,     BTN_Y       },
    { BTN_TRITON_L,     BTN_TL      },
    { BTN_TRITON_R,     BTN_TR      },
    { BTN_TRITON_MENU,  BTN_SELECT  },  /* SDL maps MENU→Back  */
    { BTN_TRITON_VIEW,  BTN_START   },  /* SDL maps VIEW→Start */
    { BTN_TRITON_STEAM, BTN_MODE    },
    { BTN_TRITON_L3,    BTN_THUMBL  },
    { BTN_TRITON_R3,    BTN_THUMBR  },
    { BTN_TRITON_R4,    BTN_TRIGGER_HAPPY1 },
    { BTN_TRITON_L4,    BTN_TRIGGER_HAPPY2 },
    { BTN_TRITON_R5,    BTN_TRIGGER_HAPPY3 },
    { BTN_TRITON_L5,    BTN_TRIGGER_HAPPY4 },
    { BTN_TRITON_QAM,   BTN_TRIGGER_HAPPY5 },
};

static int32_t neg16(int16_t v)   /* negate with INT16_MIN clamp */
{
    return (v == INT16_MIN) ? INT16_MAX : -v;
}

static void handle_state(slot_t *s, const uint8_t *d, int len)
{
    if (len < STATE_MIN_LEN)
        return;
    set_connected(s, true);
    if (s->ui_fd < 0)
        return;

    uint32_t buttons = rd32(d + OFF_BUTTONS);
    bool dirty = false;

    if (buttons != s->last_buttons) {
        for (size_t i = 0; i < sizeof(button_map) / sizeof(button_map[0]); i++) {
            uint32_t bit = button_map[i].bit;
            if ((buttons ^ s->last_buttons) & bit) {
                emit(s->ui_fd, EV_KEY, button_map[i].key, (buttons & bit) ? 1 : 0);
                dirty = true;
            }
        }
    }

    /* d-pad → hat; sticks: report Y is up-positive, evdev wants up-negative
     * (matches both SDL3's negation and the kernel xpad convention) */
    int32_t hat[2];
    hat[0] = (buttons & BTN_TRITON_DPAD_RIGHT) ? 1 :
             (buttons & BTN_TRITON_DPAD_LEFT)  ? -1 : 0;
    hat[1] = (buttons & BTN_TRITON_DPAD_DOWN)  ? 1 :
             (buttons & BTN_TRITON_DPAD_UP)    ? -1 : 0;

    int32_t abs[6];
    abs[0] = rd16(d + OFF_LSTICK_X);
    abs[1] = neg16(rd16(d + OFF_LSTICK_Y));
    abs[2] = rd16(d + OFF_RSTICK_X);
    abs[3] = neg16(rd16(d + OFF_RSTICK_Y));
    /* triggers 0..32767 → 0..255 */
    int32_t tl = rd16(d + OFF_TRIG_L), tr = rd16(d + OFF_TRIG_R);
    abs[4] = (tl < 0 ? 0 : tl) >> 7;
    abs[5] = (tr < 0 ? 0 : tr) >> 7;

    static const uint16_t abs_codes[6] = { ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ };
    for (int i = 0; i < 6; i++) {
        if (abs[i] != s->last_abs[i]) {
            emit(s->ui_fd, EV_ABS, abs_codes[i], abs[i]);
            s->last_abs[i] = abs[i];
            dirty = true;
        }
    }
    for (int i = 0; i < 2; i++) {
        if (hat[i] != s->last_hat[i]) {
            emit(s->ui_fd, EV_ABS, i == 0 ? ABS_HAT0X : ABS_HAT0Y, hat[i]);
            s->last_hat[i] = hat[i];
            dirty = true;
        }
    }

    s->last_buttons = buttons;
    if (dirty)
        emit(s->ui_fd, EV_SYN, SYN_REPORT, 0);
}

/* ------------------------------------------------------------------ */
/* uinput readback: force-feedback uploads and playback */

static void handle_uinput_event(slot_t *s)
{
    struct input_event ev;
    while (read(s->ui_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_UINPUT && ev.code == UI_FF_UPLOAD) {
            struct uinput_ff_upload up;
            memset(&up, 0, sizeof(up));
            up.request_id = ev.value;
            if (ioctl(s->ui_fd, UI_BEGIN_FF_UPLOAD, &up) == 0) {
                if (up.effect.type == FF_RUMBLE &&
                    up.effect.id >= 0 && up.effect.id < MAX_EFFECTS) {
                    ff_effect_slot *fe = &s->effects[up.effect.id];
                    fe->strong = up.effect.u.rumble.strong_magnitude;
                    fe->weak = up.effect.u.rumble.weak_magnitude;
                    fe->length_ms = up.effect.replay.length;
                    fe->used = true;
                    up.retval = 0;
                } else {
                    up.retval = -EINVAL;
                }
                ioctl(s->ui_fd, UI_END_FF_UPLOAD, &up);
            }
        } else if (ev.type == EV_UINPUT && ev.code == UI_FF_ERASE) {
            struct uinput_ff_erase er;
            memset(&er, 0, sizeof(er));
            er.request_id = ev.value;
            if (ioctl(s->ui_fd, UI_BEGIN_FF_ERASE, &er) == 0) {
                if (er.effect_id < MAX_EFFECTS)
                    s->effects[er.effect_id].used = false;
                er.retval = 0;
                ioctl(s->ui_fd, UI_END_FF_ERASE, &er);
            }
        } else if (ev.type == EV_FF) {
            if (ev.code == FF_GAIN) {
                s->ff_gain = (uint16_t)(ev.value > 0xFFFF ? 0xFFFF : ev.value);
            } else if (ev.code < MAX_EFFECTS && s->effects[ev.code].used) {
                ff_effect_slot *fe = &s->effects[ev.code];
                if (ev.value > 0) {
                    s->rumble_strong = (uint16_t)(((uint32_t)fe->strong * s->ff_gain) >> 16);
                    s->rumble_weak   = (uint16_t)(((uint32_t)fe->weak   * s->ff_gain) >> 16);
                    s->rumble_active = (s->rumble_strong || s->rumble_weak);
                    s->rumble_until_ms = fe->length_ms
                        ? now_ms() + fe->length_ms : 0;
                    send_rumble(s, s->rumble_strong, s->rumble_weak);
                } else {
                    s->rumble_active = false;
                    send_rumble(s, 0, 0);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */

static void dump_report(slot_t *s, const uint8_t *d, int len)
{
    char hex[3 * 64 + 1];
    int n = len > 64 ? 64 : len;
    for (int i = 0; i < n; i++)
        sprintf(hex + i * 3, "%02x ", d[i]);
    hex[n * 3] = '\0';
    logts("[%s if%d] len=%d  %s", s->hidraw_path, s->iface, len, hex);

    uint8_t id = d[0];
    if ((id == ID_TRITON_CONTROLLER_STATE ||
         id == ID_TRITON_CONTROLLER_STATE_BLE ||
         id == ID_TRITON_CONTROLLER_STATE_TIMESTAMP) && len >= STATE_MIN_LEN) {
        logts("    state: buttons=%08x trigL=%d trigR=%d LS=(%d,%d) RS=(%d,%d)",
              rd32(d + OFF_BUTTONS), rd16(d + OFF_TRIG_L), rd16(d + OFF_TRIG_R),
              rd16(d + OFF_LSTICK_X), rd16(d + OFF_LSTICK_Y),
              rd16(d + OFF_RSTICK_X), rd16(d + OFF_RSTICK_Y));
    } else if (id == ID_TRITON_WIRELESS_STATUS ||
               id == ID_TRITON_WIRELESS_STATUS_X) {
        logts("    wireless status: %s",
              d[1] == TRITON_WIRELESS_CONNECT ? "CONNECT" :
              d[1] == TRITON_WIRELESS_DISCONNECT ? "DISCONNECT" : "?");
    } else if (id == ID_TRITON_BATTERY_STATUS && len >= 3) {
        logts("    battery: charge_state=%u level=%u%%", d[1], d[2]);
    }
}

static bool steam_is_running(void)
{
    DIR *d = opendir("/proc");
    if (!d)
        return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        char path[300], comm[32];
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(comm, sizeof(comm), f) && strcmp(comm, "steam\n") == 0)
            found = true;
        fclose(f);
    }
    closedir(d);
    return found;
}

static void close_all(void)
{
    for (int i = 0; i < nslots; i++) {
        destroy_uinput(&slots[i]);
        if (slots[i].hid_fd >= 0) {
            close(slots[i].hid_fd);
            slots[i].hid_fd = -1;
        }
    }
    nslots = 0;
}

/* ------------------------------------------------------------------ */

static void usage(FILE *out, const char *argv0)
{
    fprintf(out,
        "usage: %s [--dump] [--no-steam-check]\n"
        "\n"
        "Bridges the 2nd-gen Steam Controller (\"Puck\") from raw hidraw to a\n"
        "virtual Microsoft X-Box 360 pad on /dev/uinput, so it works as a\n"
        "normal gamepad without Steam running.\n"
        "\n"
        "options:\n"
        "  --dump             protocol debug: hexdump and parse every report;\n"
        "                     no virtual pad is created\n"
        "  --no-steam-check   keep running even when a Steam client is up\n"
        "                     (default: back off and let Steam own the device)\n"
        "  -h, --help         this text\n"
        "  -V, --version      print version and exit\n"
        "\n"
        "Logs to stdout; normally run via the systemd user unit.\n"
        "Home page: <https://github.com/benashby/steam-puck-bridge>\n",
        argv0);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0)
            opt_dump = true;
        else if (strcmp(argv[i], "--no-steam-check") == 0)
            opt_steam_check = false;
        else if (strcmp(argv[i], "--help") == 0 ||
                 strcmp(argv[i], "-h") == 0) {
            usage(stdout, argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0 ||
                   strcmp(argv[i], "-V") == 0) {
            printf("steam-puck-bridge %s\n", SPB_VERSION);
            return 0;
        } else {
            usage(stderr, argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    logts("steam-puck-bridge starting%s", opt_dump ? " (dump mode)" : "");

    uint64_t last_steam_check = 0;
    bool paused_for_steam = false;

    while (running) {
        /* ---- device (re)discovery ---- */
        if (nslots == 0) {
            if (scan_devices() == 0) {
                /* nothing present; poll for hotplug */
                for (int t = 0; t < 30 && running; t++)
                    usleep(100 * 1000);
                continue;
            }
            for (int i = 0; i < nslots; i++)
                send_lizard_off(&slots[i]);
        }

        /* ---- steam backoff ---- */
        uint64_t now = now_ms();
        if (opt_steam_check && !opt_dump && now - last_steam_check > 2000) {
            last_steam_check = now;
            bool steam = steam_is_running();
            if (steam && !paused_for_steam) {
                logts("Steam client detected — releasing controller to Steam");
                close_all();
                paused_for_steam = true;
            } else if (!steam && paused_for_steam) {
                logts("Steam exited — reclaiming controller");
                paused_for_steam = false;
                continue;   /* rescan at loop top */
            }
        }
        if (paused_for_steam) {
            for (int t = 0; t < 20 && running; t++)
                usleep(100 * 1000);
            continue;
        }

        /* ---- periodic per-slot work ---- */
        for (int i = 0; i < nslots; i++) {
            slot_t *s = &slots[i];
            if (s->hid_fd < 0)
                continue;
            if (now - s->last_lizard_ms >= LIZARD_HEARTBEAT_MS)
                send_lizard_off(s);
            if (s->rumble_active) {
                if (s->rumble_until_ms && now >= s->rumble_until_ms) {
                    s->rumble_active = false;
                    send_rumble(s, 0, 0);
                } else if (now - s->last_rumble_ms >= RUMBLE_RESEND_MS) {
                    send_rumble(s, s->rumble_strong, s->rumble_weak);
                }
            }
        }

        /* ---- poll hidraw + uinput fds ---- */
        struct pollfd pfds[MAX_SLOTS * 2];
        int map[MAX_SLOTS * 2], npfd = 0;
        for (int i = 0; i < nslots; i++) {
            if (slots[i].hid_fd >= 0) {
                pfds[npfd].fd = slots[i].hid_fd;
                pfds[npfd].events = POLLIN;
                map[npfd++] = i * 2;
            }
            if (slots[i].ui_fd >= 0) {
                pfds[npfd].fd = slots[i].ui_fd;
                pfds[npfd].events = POLLIN;
                map[npfd++] = i * 2 + 1;
            }
        }

        int rc = poll(pfds, npfd, 50);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            logts("poll: %m");
            break;
        }

        bool device_lost = false;
        for (int p = 0; p < npfd && rc > 0; p++) {
            if (!(pfds[p].revents & (POLLIN | POLLERR | POLLHUP)))
                continue;
            slot_t *s = &slots[map[p] / 2];
            bool is_uinput = map[p] & 1;

            if (is_uinput) {
                handle_uinput_event(s);
                continue;
            }

            uint8_t buf[128];
            for (;;) {
                int n = read(s->hid_fd, buf, sizeof(buf));
                if (n < 0) {
                    if (errno == EAGAIN)
                        break;
                    logts("read %s: %m — device lost, rescanning",
                          s->hidraw_path);
                    device_lost = true;
                    break;
                }
                if (n == 0)
                    break;

                if (opt_dump) {
                    dump_report(s, buf, n);
                    continue;
                }
                switch (buf[0]) {
                case ID_TRITON_CONTROLLER_STATE:
                case ID_TRITON_CONTROLLER_STATE_BLE:
                case ID_TRITON_CONTROLLER_STATE_TIMESTAMP:
                    handle_state(s, buf, n);
                    break;
                case ID_TRITON_WIRELESS_STATUS:
                case ID_TRITON_WIRELESS_STATUS_X:
                    if (n >= 2)
                        set_connected(s, buf[1] == TRITON_WIRELESS_CONNECT);
                    break;
                case ID_TRITON_BATTERY_STATUS:
                    /* informational only */
                    break;
                default:
                    break;
                }
            }
        }

        if (device_lost) {
            close_all();
            /* brief settle before rescan */
            for (int t = 0; t < 10 && running; t++)
                usleep(100 * 1000);
        }
    }

    logts("steam-puck-bridge stopping");
    close_all();
    return 0;
}
