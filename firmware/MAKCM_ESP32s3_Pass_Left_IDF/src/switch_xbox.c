// switch_xbox.c
//
// Why this file exists:
// The ESP32 S3 USB host is USB full speed only. Pads like the GameSir
// Cyclone 2 then speak Switch HID (VID 057E) instead of XInput.
//
// USB3 (Right MCU) still talks Switch to the pad.
// USB1 (this file, Left MCU) pretends to be a wired Xbox 360 pad
// (045E:028E) and converts Switch reports into 20 byte XInput reports.
//
// Stick Y is inverted here because Nintendo up is Xbox down.
// Official Xbox pads have a different VID, so this translator stays off.
//
// Report layouts (SDL hidapi Switch driver and hid-nintendo):
//   0x3F  simple HID (buttons, hat, analog)
//   0x30  full Nintendo input (12 bit sticks)
// Generic 7 to 8 byte joystick reports are accepted as a fallback.

#include "switch_xbox.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern int km_uart_write(const void *data, size_t len);

static bool     g_active;
static uint16_t g_src_vid;
static uint16_t g_src_pid;
static uint16_t g_log_left;
static uint8_t  g_last_xi[SWITCH_XBOX_REPORT_LEN];
static bool     g_have_last;
// 0 = unknown, 1 = buttons/hat/axes (Nintendo simple), 2 = axes then hat/buttons
static uint8_t  g_layout;

// Xbox 360 wired pad (045E:028E). Interface 0 only — headset/security
// interfaces blow the ESP32-S3 endpoint budget and are not needed on PC.
static const uint8_t k_x360_dev[18] = {
    0x12, 0x01, 0x00, 0x02, 0xFF, 0xFF, 0xFF, 0x40,
    0x5E, 0x04, 0x8E, 0x02, 0x14, 0x01,
    0x01, 0x02, 0x03, 0x01,
};

// 49-byte config: unknown 0x21 blob MUST list the same EPs as the
// endpoint descriptors or Windows xusb22 will not stream input.
static const uint8_t k_x360_cfg[] = {
    0x09, 0x02, 0x31, 0x00, 0x01, 0x01, 0x00, 0xA0, 0xFA,
    0x09, 0x04, 0x00, 0x00, 0x02, 0xFF, 0x5D, 0x01, 0x00,
    0x11, 0x21, 0x00, 0x01, 0x01, 0x25,
    0x81, 0x14, 0x00, 0x00, 0x00, 0x00, 0x13,
    0x02, 0x08, 0x00, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x20, 0x00, 0x04,
    0x07, 0x05, 0x02, 0x03, 0x20, 0x00, 0x08,
};

#define XI_DPAD_U  0x0001
#define XI_DPAD_D  0x0002
#define XI_DPAD_L  0x0004
#define XI_DPAD_R  0x0008
#define XI_START   0x0010
#define XI_BACK    0x0020
#define XI_LS      0x0040
#define XI_RS      0x0080
#define XI_LB      0x0100
#define XI_RB      0x0200
#define XI_GUIDE   0x0400
#define XI_A       0x1000
#define XI_B       0x2000
#define XI_X       0x4000
#define XI_Y       0x8000

static void log_line(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
        buf[n++] = '\n';
        km_uart_write(buf, (size_t)n);
    }
}

bool switch_xbox_on_device_desc(uint16_t vid, uint16_t pid) {
    g_src_vid = vid;
    g_src_pid = pid;
    g_active  = (vid == SWITCH_XBOX_VID_NINTENDO);
    g_log_left = 20;
    g_have_last = false;
    g_layout = 0;
    switch_xbox_idle_report(g_last_xi);
    if (g_active) {
        log_line("[L] switch_xbox ON src=%04x:%04x → 045e:028e", vid, pid);
    }
    return g_active;
}

void switch_xbox_reset(void) {
    g_active    = false;
    g_src_vid   = 0;
    g_src_pid   = 0;
    g_log_left  = 0;
    g_have_last = false;
    g_layout    = 0;
}

bool switch_xbox_active(void) {
    return g_active;
}

void switch_xbox_fill_device(tusb_desc_device_t *dev) {
    memcpy(dev, k_x360_dev, sizeof(k_x360_dev));
}

void switch_xbox_fill_config(uint8_t *cfg, uint16_t *len) {
    memcpy(cfg, k_x360_cfg, sizeof(k_x360_cfg));
    *len = (uint16_t)sizeof(k_x360_cfg);
}

uint8_t switch_xbox_utf16_string(uint8_t idx, uint8_t *dst, size_t dst_max) {
    const char *ascii = "";
    if (idx == 1) ascii = "Microsoft";
    else if (idx == 2) ascii = "Xbox 360 Controller";
    else if (idx == 3) ascii = "MAKCU";
    else return 0;

    size_t n = strlen(ascii);
    if (n * 2 > dst_max) n = dst_max / 2;
    for (size_t i = 0; i < n; i++) {
        dst[i * 2]     = (uint8_t)ascii[i];
        dst[i * 2 + 1] = 0;
    }
    return (uint8_t)(n * 2);
}

static int16_t u8_to_x(uint8_t v) {
    int32_t s = ((int32_t)v - 128) << 8;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

// HID / Switch simple Y is often 0 = up. XInput LY positive = up.
static int16_t u8_to_y(uint8_t v) {
    int32_t s = ((int32_t)128 - (int32_t)v) << 8;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

static int16_t s12_to_s16(uint16_t raw12) {
    int32_t s = ((int32_t)raw12 - 2048) * 16;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

static void write_le16(uint8_t *p, int16_t v) {
    p[0] = (uint8_t)((uint16_t)v & 0xFF);
    p[1] = (uint8_t)((uint16_t)v >> 8);
}

static uint16_t hat_to_dpad(uint8_t hat) {
    switch (hat) {
    case 0: return XI_DPAD_U;
    case 1: return XI_DPAD_U | XI_DPAD_R;
    case 2: return XI_DPAD_R;
    case 3: return XI_DPAD_D | XI_DPAD_R;
    case 4: return XI_DPAD_D;
    case 5: return XI_DPAD_D | XI_DPAD_L;
    case 6: return XI_DPAD_L;
    case 7: return XI_DPAD_U | XI_DPAD_L;
    default: return 0;
    }
}

static void emit_xinput(uint8_t *out, uint16_t btn, uint8_t lt, uint8_t rt,
                        int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    memset(out, 0, SWITCH_XBOX_REPORT_LEN);
    out[0] = 0x00;
    out[1] = 0x14;
    out[2] = (uint8_t)(btn & 0xFF);
    out[3] = (uint8_t)(btn >> 8);
    out[4] = lt;
    out[5] = rt;
    write_le16(out + 6,  lx);
    write_le16(out + 8,  (int16_t)-ly);
    write_le16(out + 10, rx);
    write_le16(out + 12, (int16_t)-ry);
}

// 0x3F simple: bit0 Switch B → Xbox A, bit1 Switch A → Xbox B, etc.
static uint16_t map_simple_buttons(uint8_t b0, uint8_t b1) {
    uint16_t btn = 0;
    if (b0 & 0x01) btn |= XI_A;
    if (b0 & 0x02) btn |= XI_B;
    if (b0 & 0x04) btn |= XI_X;
    if (b0 & 0x08) btn |= XI_Y;
    if (b0 & 0x10) btn |= XI_LB;
    if (b0 & 0x20) btn |= XI_RB;
    if (b1 & 0x01) btn |= XI_BACK;
    if (b1 & 0x02) btn |= XI_START;
    if (b1 & 0x04) btn |= XI_LS;
    if (b1 & 0x08) btn |= XI_RS;
    if (b1 & 0x10) btn |= XI_GUIDE;
    return btn;
}

static bool parse_simple(const uint8_t *p, uint16_t len, uint8_t *out) {
    if (len < 7) return false;
    uint8_t b0 = p[0], b1 = p[1], hat = p[2];
    uint16_t btn = map_simple_buttons(b0, b1) | hat_to_dpad(hat);
    uint8_t lt = (b0 & 0x40) ? 0xFF : 0;
    uint8_t rt = ((b0 & 0x80) || (b1 & 0x80)) ? 0xFF : 0;
    int16_t lx, ly, rx, ry;
    if (len >= 11) {
        lx = (int16_t)((uint16_t)p[3] | ((uint16_t)p[4] << 8));
        ly = (int16_t)((uint16_t)p[5] | ((uint16_t)p[6] << 8));
        rx = (int16_t)((uint16_t)p[7] | ((uint16_t)p[8] << 8));
        ry = (int16_t)((uint16_t)p[9] | ((uint16_t)p[10] << 8));
    } else {
        lx = u8_to_x(p[3]);
        ly = u8_to_y(p[4]);
        rx = u8_to_x(p[5]);
        ry = u8_to_y(p[6]);
    }
    emit_xinput(out, btn, lt, rt, lx, ly, rx, ry);
    return true;
}

static bool parse_full_30(const uint8_t *buf, uint16_t len, uint8_t *out) {
    if (len < 12) return false;
    uint8_t br = buf[3], bs = buf[4], bl = buf[5];
    uint16_t btn = 0;
    if (br & 0x04) btn |= XI_A;       // Switch B
    if (br & 0x08) btn |= XI_B;       // Switch A
    if (br & 0x01) btn |= XI_X;       // Switch Y
    if (br & 0x02) btn |= XI_Y;       // Switch X
    if (br & 0x40) btn |= XI_RB;
    if (bs & 0x01) btn |= XI_BACK;
    if (bs & 0x02) btn |= XI_START;
    if (bs & 0x04) btn |= XI_RS;
    if (bs & 0x08) btn |= XI_LS;
    if (bs & 0x10) btn |= XI_GUIDE;
    if (bl & 0x01) btn |= XI_DPAD_D;
    if (bl & 0x02) btn |= XI_DPAD_U;
    if (bl & 0x04) btn |= XI_DPAD_R;
    if (bl & 0x08) btn |= XI_DPAD_L;
    if (bl & 0x40) btn |= XI_LB;
    uint8_t rt = (br & 0x80) ? 0xFF : 0;
    uint8_t lt = (bl & 0x80) ? 0xFF : 0;
    uint16_t lx12 = (uint16_t)buf[6] | ((uint16_t)(buf[7] & 0x0F) << 8);
    uint16_t ly12 = (uint16_t)((buf[7] >> 4) | ((uint16_t)buf[8] << 4));
    uint16_t rx12 = (uint16_t)buf[9] | ((uint16_t)(buf[10] & 0x0F) << 8);
    uint16_t ry12 = (uint16_t)((buf[10] >> 4) | ((uint16_t)buf[11] << 4));
    emit_xinput(out, btn, lt, rt,
                s12_to_s16(lx12), (int16_t)~s12_to_s16(ly12),
                s12_to_s16(rx12), (int16_t)~s12_to_s16(ry12));
    return true;
}

void switch_xbox_idle_report(uint8_t out[SWITCH_XBOX_REPORT_LEN]) {
    emit_xinput(out, 0, 0, 0, 0, 0, 0, 0);
}

static bool near_center_u8(uint8_t v) {
    return v >= 0x40 && v <= 0xC0;
}

static bool parse_axes_first(const uint8_t *p, uint16_t len, uint8_t *out) {
    if (len < 6) return false;
    int16_t lx = u8_to_x(p[0]);
    int16_t ly = u8_to_y(p[1]);
    int16_t rx = u8_to_x(p[2]);
    int16_t ry = u8_to_y(p[3]);
    uint8_t hat = p[4] & 0x0F;
    uint8_t b0  = (len > 5) ? p[5] : 0;
    uint8_t b1  = (len > 6) ? p[6] : 0;
    uint16_t btn = map_simple_buttons(b0, b1) | hat_to_dpad(hat);
    uint8_t lt = (b0 & 0x40) ? 0xFF : 0;
    uint8_t rt = ((b0 & 0x80) || (b1 & 0x80)) ? 0xFF : 0;
    emit_xinput(out, btn, lt, rt, lx, ly, rx, ry);
    return true;
}

static uint8_t detect_layout(const uint8_t *in, uint16_t len) {
    if (len >= 7 && in[2] <= 8 && near_center_u8(in[3]) && near_center_u8(in[4])) {
        return 1;
    }
    if (len >= 6 && near_center_u8(in[0]) && near_center_u8(in[1]) &&
        near_center_u8(in[2]) && near_center_u8(in[3])) {
        return 2;
    }
    return 1;
}

bool switch_xbox_translate(const uint8_t *in, uint16_t in_len,
                           uint8_t out[SWITCH_XBOX_REPORT_LEN]) {
    if (!in || in_len == 0) {
        switch_xbox_idle_report(out);
        return true;
    }

    bool ok = false;
    if (in[0] == 0x21) {
        if (g_have_last) memcpy(out, g_last_xi, SWITCH_XBOX_REPORT_LEN);
        else switch_xbox_idle_report(out);
        return true;
    }
    if (in[0] == 0x3F && in_len >= 8) {
        g_layout = 1;
        ok = parse_simple(in + 1, (uint16_t)(in_len - 1), out);
    } else if ((in[0] == 0x30 || in[0] == 0x31) && in_len >= 12) {
        ok = parse_full_30(in, in_len, out);
    } else if (in_len >= 6 && in[0] != 0x81 && in[0] != 0x80) {
        if (!g_layout) g_layout = detect_layout(in, in_len);
        if (g_layout == 2) ok = parse_axes_first(in, in_len, out);
        else ok = parse_simple(in, in_len, out);
    }

    if (!ok) {
        if (g_have_last) memcpy(out, g_last_xi, SWITCH_XBOX_REPORT_LEN);
        else switch_xbox_idle_report(out);
    } else {
        memcpy(g_last_xi, out, SWITCH_XBOX_REPORT_LEN);
        g_have_last = true;
    }

    if (g_log_left) {
        g_log_left--;
        char hex[48];
        int hn = 0;
        uint16_t show = in_len > 12 ? 12 : in_len;
        for (uint16_t i = 0; i < show && hn < (int)sizeof(hex) - 3; ++i) {
            hn += snprintf(hex + hn, sizeof(hex) - hn, "%02x", in[i]);
        }
        log_line("[L] switch_xbox len=%u lay=%u ok=%d raw=%s btn=%02x%02x",
                 (unsigned)in_len, (unsigned)g_layout, (int)ok, hex,
                 out[3], out[2]);
    }
    return true;
}
