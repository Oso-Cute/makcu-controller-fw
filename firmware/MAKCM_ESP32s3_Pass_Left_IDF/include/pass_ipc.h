// pass_ipc.h — UART framing shared by Left (USB-device) and Right (USB-host).
// Both sides use an identical copy. Keep header-only.
//
// Frame format (little-endian on wire, variable payload):
//
//   +----+----+------+--------+----------+-------------+-------+-------+
//   |magic[0..1] | type | ep_addr| seq_id |  length_le  |  payload    | crc16 |
//   +----+----+------+--------+----------+-------------+-------+-------+
//   |0xA5| 0x5A |  u8  |   u8   |   u16   |      u16    | <N bytes>   | u16   |
//
//   total_size = 2 + 1 + 1 + 2 + 2 + N + 2 = N + 10
//
// - seq_id wraps 0..65535 and lets Left match a response to a control
//   transfer it initiated.
// - length is payload only (excludes magic/header/CRC).
// - CRC is CCITT-FALSE (poly 0x1021, init 0xFFFF), computed over every
//   byte from `type` through the last payload byte.

#pragma once
#include <stdint.h>

#define PASS_IPC_MAGIC0        0xA5
#define PASS_IPC_MAGIC1        0x5A
#define PASS_IPC_HDR_OVERHEAD  10
#define PASS_IPC_MAX_PAYLOAD   1024

enum pass_ipc_type {
    // Right → Left, once after enumerating the real device:
    FRAME_DESC_DEVICE    = 0x01,  // 18-byte device descriptor
    FRAME_DESC_CONFIG    = 0x02,  // full config descriptor
    FRAME_DESC_STRING    = 0x03,  // [u8 string_idx][UTF-16LE bytes]
    FRAME_DESC_MSOS1_EE  = 0x04,  // (reserved) 0xEE string descriptor response
    FRAME_DESC_MSOS1_CID = 0x05,  // (reserved) MS OS Compat ID response
    FRAME_DEVICE_READY   = 0x06,  // "snapshot complete, you may enumerate"
    FRAME_DEVICE_GONE    = 0x07,  // real device disconnected

    // Left → Right: relay target-PC traffic to the real device:
    FRAME_CTRL_SETUP     = 0x10,  // 8B SETUP, optionally followed by OUT data
    FRAME_CTRL_OUT_DATA  = 0x11,  // (reserved) two-phase OUT — unused; setup+data combined
    FRAME_EP_OUT         = 0x12,  // OUT endpoint data

    // Right → Left:
    FRAME_CTRL_IN_DATA   = 0x20,  // IN control data (seq matches SETUP)
    FRAME_CTRL_STATUS    = 0x21,  // [u8 status] 0=OK, 1=STALL, 2=NAK, 3=TIMEOUT, 4=ERROR
    FRAME_EP_IN          = 0x22,  // IN endpoint data

    // (reserved) Out-of-band KM injection over IPC; current build delivers
    // KM commands directly to Left over UART0 (CH343 bridge) instead.
    FRAME_KM_INJECT      = 0x30,

    FRAME_LOG            = 0xF0,  // ASCII log line (Right → Left, tunneled to COM3)
    FRAME_PING           = 0xF1,  // empty payload; responder echoes same seq
};

enum pass_ipc_xfer_status {
    XFER_OK      = 0,
    XFER_STALL   = 1,
    XFER_NAK     = 2,
    XFER_TIMEOUT = 3,
    XFER_ERROR   = 4,
};

static inline uint16_t pass_ipc_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= ((uint16_t)data[i]) << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
