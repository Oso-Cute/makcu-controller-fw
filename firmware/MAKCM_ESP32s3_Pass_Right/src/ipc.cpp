// ipc.cpp — Right-side IPC framer/deframer. Wire format in pass_ipc.h.
//   TX: ipc_send() assembles a frame into a stack buffer, CRCs
//       type..last-payload, writes the whole frame atomically.
//   RX: ipc_feed() drives a per-byte state machine. CRC-valid frames
//       dispatch via ipc_handle_frame(); CRC mismatches and magic
//       resyncs are silently dropped.

#include <Arduino.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pass_ipc.h"
#include "diag.h"

extern HardwareSerial IpcSerial;
extern void ipc_handle_frame(uint8_t type, uint8_t ep_addr, uint16_t seq,
                             const uint8_t *payload, uint16_t len);

// ipc_send is called from USB completion callbacks, IPC RX task, and
// library callbacks — three sources on two cores. HardwareSerial locks
// only per-write(), so three sequential write() calls could interleave.
// We assemble the frame into a single buffer and emit under a mutex.
static SemaphoreHandle_t ipc_tx_mutex = nullptr;

bool ipc_send(uint8_t type, uint8_t ep_addr, uint16_t seq,
              const uint8_t *payload, uint16_t len) {
    if (len > PASS_IPC_MAX_PAYLOAD) return false;

    if (!ipc_tx_mutex) ipc_tx_mutex = xSemaphoreCreateMutex();

    uint8_t frame[PASS_IPC_HDR_OVERHEAD + PASS_IPC_MAX_PAYLOAD];
    frame[0] = PASS_IPC_MAGIC0;
    frame[1] = PASS_IPC_MAGIC1;
    frame[2] = type;
    frame[3] = ep_addr;
    frame[4] = (uint8_t)(seq & 0xFF);
    frame[5] = (uint8_t)(seq >> 8);
    frame[6] = (uint8_t)(len & 0xFF);
    frame[7] = (uint8_t)(len >> 8);
    if (len) memcpy(&frame[8], payload, len);
    const uint16_t crc = pass_ipc_crc16(&frame[2], 6 + len);
    frame[8 + len]     = (uint8_t)(crc & 0xFF);
    frame[8 + len + 1] = (uint8_t)(crc >> 8);

    if (ipc_tx_mutex) xSemaphoreTake(ipc_tx_mutex, portMAX_DELAY);
    const size_t wrote = (size_t)(8 + len + 2);
    IpcSerial.write(frame, wrote);
    if (ipc_tx_mutex) xSemaphoreGive(ipc_tx_mutex);
    diag_on_ipc_tx_bytes((uint16_t)wrote);
    return true;
}

enum : uint8_t {
    S_WAIT_MAGIC0, S_WAIT_MAGIC1,
    S_READ_TYPE, S_READ_EP,
    S_READ_SEQ_LO, S_READ_SEQ_HI,
    S_READ_LEN_LO, S_READ_LEN_HI,
    S_READ_PAYLOAD,
    S_READ_CRC_LO, S_READ_CRC_HI,
};

static uint8_t  rx_state     = S_WAIT_MAGIC0;
static uint8_t  rx_type      = 0;
static uint8_t  rx_ep        = 0;
static uint16_t rx_seq       = 0;
static uint16_t rx_len       = 0;
static uint16_t rx_have      = 0;
static uint16_t rx_crc_rcvd  = 0;
static uint8_t  rx_buf[PASS_IPC_MAX_PAYLOAD];

void ipc_feed(uint8_t b) {
    switch (rx_state) {
    case S_WAIT_MAGIC0:
        if (b == PASS_IPC_MAGIC0) rx_state = S_WAIT_MAGIC1;
        break;
    case S_WAIT_MAGIC1:
        rx_state = (b == PASS_IPC_MAGIC1) ? S_READ_TYPE
                 : (b == PASS_IPC_MAGIC0) ? S_WAIT_MAGIC1
                                          : S_WAIT_MAGIC0;
        break;
    case S_READ_TYPE:    rx_type   = b; rx_state = S_READ_EP;      break;
    case S_READ_EP:      rx_ep     = b; rx_state = S_READ_SEQ_LO;  break;
    case S_READ_SEQ_LO:  rx_seq    = b; rx_state = S_READ_SEQ_HI;  break;
    case S_READ_SEQ_HI:  rx_seq   |= ((uint16_t)b) << 8; rx_state = S_READ_LEN_LO; break;
    case S_READ_LEN_LO:  rx_len    = b; rx_state = S_READ_LEN_HI;  break;
    case S_READ_LEN_HI:
        rx_len |= ((uint16_t)b) << 8;
        if (rx_len > PASS_IPC_MAX_PAYLOAD) { rx_state = S_WAIT_MAGIC0; break; }
        rx_have  = 0;
        rx_state = rx_len ? S_READ_PAYLOAD : S_READ_CRC_LO;
        break;
    case S_READ_PAYLOAD:
        rx_buf[rx_have++] = b;
        if (rx_have == rx_len) rx_state = S_READ_CRC_LO;
        break;
    case S_READ_CRC_LO:  rx_crc_rcvd = b; rx_state = S_READ_CRC_HI; break;
    case S_READ_CRC_HI: {
        rx_crc_rcvd |= ((uint16_t)b) << 8;
        uint8_t region[6];
        region[0] = rx_type;
        region[1] = rx_ep;
        region[2] = (uint8_t)(rx_seq & 0xFF);
        region[3] = (uint8_t)(rx_seq >> 8);
        region[4] = (uint8_t)(rx_len & 0xFF);
        region[5] = (uint8_t)(rx_len >> 8);
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < 6; ++i) {
            crc ^= ((uint16_t)region[i]) << 8;
            for (int k = 0; k < 8; ++k) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
        for (uint16_t i = 0; i < rx_len; ++i) {
            crc ^= ((uint16_t)rx_buf[i]) << 8;
            for (int k = 0; k < 8; ++k) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
        if (crc == rx_crc_rcvd) {
            ipc_handle_frame(rx_type, rx_ep, rx_seq, rx_buf, rx_len);
        }
        rx_state = S_WAIT_MAGIC0;
        break;
    }
    }
}

// Inactivity watchdog: if we're mid-frame and haven't seen a byte for
// >10 ms, reset to S_WAIT_MAGIC0 — avoids being wedged by a dropped
// byte (5 Mbps UART can overflow under heavy load).
static uint32_t rx_last_byte_ms = 0;
void ipc_pump_serial(void) {
    bool got = false;
    while (IpcSerial.available()) {
        ipc_feed((uint8_t)IpcSerial.read());
        got = true;
    }
    uint32_t now = millis();
    if (got) {
        rx_last_byte_ms = now;
    } else if (rx_state != S_WAIT_MAGIC0 &&
               (now - rx_last_byte_ms) > 10) {
        rx_state = S_WAIT_MAGIC0;
        rx_last_byte_ms = now;
    }
}
