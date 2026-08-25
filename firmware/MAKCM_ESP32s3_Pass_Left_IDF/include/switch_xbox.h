// Switch Pro HID (VID 057E) to Xbox 360 XInput (045E:028E).
// Used when the real pad on USB3 is Nintendo. USB1 then looks like a
// wired Xbox 360 controller. See switch_xbox.c for the why.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "tusb.h"

#define SWITCH_XBOX_VID_NINTENDO  0x057E
#define SWITCH_XBOX_EP_IN         0x81
#define SWITCH_XBOX_REPORT_LEN    20

bool switch_xbox_on_device_desc(uint16_t vid, uint16_t pid);
void switch_xbox_reset(void);
bool switch_xbox_active(void);

void switch_xbox_fill_device(tusb_desc_device_t *dev);
void switch_xbox_fill_config(uint8_t *cfg, uint16_t *len);

// UTF-16LE string bodies for TinyUSB string indices 1..3.
// dst must hold at least 128 bytes. Returns byte length (not chars).
uint8_t switch_xbox_utf16_string(uint8_t idx, uint8_t *dst, size_t dst_max);

void switch_xbox_idle_report(uint8_t out[SWITCH_XBOX_REPORT_LEN]);

// Convert a Switch / generic HID IN report into a 20-byte XInput packet.
bool switch_xbox_translate(const uint8_t *in, uint16_t in_len,
                           uint8_t out[SWITCH_XBOX_REPORT_LEN]);
