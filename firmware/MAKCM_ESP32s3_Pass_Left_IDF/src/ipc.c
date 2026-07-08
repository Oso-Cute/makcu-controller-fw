// ipc.c — Left-side IPC framer/deframer.
// UART1: binary IPC to Right MCU @ 5 Mbps.
// UART0: ASCII KM-command channel + diagnostic log out, via CH343 bridge
//        to the second PC's COM3 @ 4 Mbaud.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "pass_ipc.h"

#define IPC_UART_PORT     UART_NUM_1
#define IPC_UART_BAUD     5000000
#define IPC_UART_RX_PIN   1
#define IPC_UART_TX_PIN   2
#define IPC_UART_BUF_SZ   8192

#define KM_UART_PORT      UART_NUM_0
#define KM_UART_BAUD      4000000   // 80 MHz APB / 20 — clean divisor; CH343 supports multi-Mbaud.
#define KM_UART_TX_PIN    43        // ESP32-S3 UART0 defaults; matches CH343 wiring on MAKCM.
#define KM_UART_RX_PIN    44
#define KM_UART_BUF_SZ    8192

extern void ipc_handle_frame(uint8_t type, uint8_t ep_addr, uint16_t seq,
                             const uint8_t *payload, uint16_t len);
extern void km_ingest_raw(const uint8_t *payload, uint16_t len);

static const char *TAG = "ipc";
static SemaphoreHandle_t ipc_tx_mutex;

enum rx_state {
    S_WAIT_MAGIC0, S_WAIT_MAGIC1,
    S_READ_TYPE, S_READ_EP,
    S_READ_SEQ_LO, S_READ_SEQ_HI,
    S_READ_LEN_LO, S_READ_LEN_HI,
    S_READ_PAYLOAD,
    S_READ_CRC_LO, S_READ_CRC_HI,
};

static enum rx_state rx_state  = S_WAIT_MAGIC0;
static uint8_t  rx_type        = 0;
static uint8_t  rx_ep          = 0;
static uint16_t rx_seq         = 0;
static uint16_t rx_len         = 0;
static uint16_t rx_have        = 0;
static uint16_t rx_crc_rcvd    = 0;
static uint8_t  rx_buf[PASS_IPC_MAX_PAYLOAD];

static void ipc_feed(uint8_t b) {
    switch (rx_state) {
    case S_WAIT_MAGIC0:
        if (b == PASS_IPC_MAGIC0) rx_state = S_WAIT_MAGIC1;
        break;
    case S_WAIT_MAGIC1:
        rx_state = (b == PASS_IPC_MAGIC1) ? S_READ_TYPE
                 : (b == PASS_IPC_MAGIC0) ? S_WAIT_MAGIC1
                                          : S_WAIT_MAGIC0;
        break;
    case S_READ_TYPE:    rx_type   = b; rx_state = S_READ_EP;     break;
    case S_READ_EP:      rx_ep     = b; rx_state = S_READ_SEQ_LO; break;
    case S_READ_SEQ_LO:  rx_seq    = b; rx_state = S_READ_SEQ_HI; break;
    case S_READ_SEQ_HI:  rx_seq   |= ((uint16_t)b) << 8; rx_state = S_READ_LEN_LO; break;
    case S_READ_LEN_LO:  rx_len    = b; rx_state = S_READ_LEN_HI; break;
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
        } else {
            ESP_LOGW(TAG, "CRC fail type=0x%02x len=%u", rx_type, rx_len);
        }
        rx_state = S_WAIT_MAGIC0;
        break;
    }
    }
}

static void ipc_rx_task(void *arg) {
    (void)arg;
    uint8_t chunk[256];
    for (;;) {
        // 1-tick literal timeout (at 1 kHz tick = 1 ms). pdMS_TO_TICKS(1)
        // would round down to 0 ticks at the legacy 100 Hz default — a
        // non-blocking spin that starved TinyUSB during enum bursts.
        int n = uart_read_bytes(IPC_UART_PORT, chunk, sizeof(chunk), 1);
        for (int i = 0; i < n; ++i) ipc_feed(chunk[i]);
    }
}

bool ipc_send(uint8_t type, uint8_t ep_addr, uint16_t seq,
              const uint8_t *payload, uint16_t len) {
    if (len > PASS_IPC_MAX_PAYLOAD) return false;

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

    xSemaphoreTake(ipc_tx_mutex, portMAX_DELAY);
    uart_write_bytes(IPC_UART_PORT, frame, 8 + len + 2);
    xSemaphoreGive(ipc_tx_mutex);
    return true;
}

// COM3_LOG gates diagnostic output on the KM UART.
//   1 → every EP_IN/EP_OUT/ctl/KM log line is sent down COM3. Floods the
//       wire — cheat software parsing the same channel can get confused.
//   0 → diagnostic output is suppressed. km.version() responses (which
//       use km_uart_write_raw) still go through, so handshake detection
//       still works. This is the gameplay build.
#ifndef COM3_LOG
#define COM3_LOG 1
#endif

// Always-on writer — protocol responses that must reach the second PC
// regardless of the diagnostic gate.
int km_uart_write_raw(const void *data, size_t len) {
    return uart_write_bytes(KM_UART_PORT, data, len);
}

int km_uart_write(const void *data, size_t len) {
#if COM3_LOG
    return uart_write_bytes(KM_UART_PORT, data, len);
#else
    (void)data;
    return (int)len;
#endif
}

// Newline-delimited line reader: feeds each line to km_ingest_raw.
static void km_uart_task(void *arg) {
    (void)arg;
    static char line[256];
    size_t n = 0;
    uint8_t chunk[128];
    for (;;) {
        int got = uart_read_bytes(KM_UART_PORT, chunk, sizeof(chunk), 1);
        for (int i = 0; i < got; ++i) {
            char c = (char)chunk[i];
            if (c == '\n' || c == '\r') {
                if (n > 0) {
                    km_ingest_raw((const uint8_t *)line, (uint16_t)n);
                    n = 0;
                }
            } else if (n < sizeof(line) - 1) {
                line[n++] = c;
            }
        }
    }
}

void ipc_init(void) {
    ipc_tx_mutex = xSemaphoreCreateMutex();

    const uart_config_t cfg = {
        .baud_rate           = IPC_UART_BAUD,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
        .source_clk          = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(IPC_UART_PORT, IPC_UART_BUF_SZ, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(IPC_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(IPC_UART_PORT, IPC_UART_TX_PIN, IPC_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreatePinnedToCore(ipc_rx_task, "ipc_rx", 8192, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "IPC up on UART%d @ %d baud, RX=%d TX=%d",
             IPC_UART_PORT, IPC_UART_BAUD, IPC_UART_RX_PIN, IPC_UART_TX_PIN);

    const uart_config_t km_cfg = {
        .baud_rate  = KM_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(KM_UART_PORT, KM_UART_BUF_SZ, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(KM_UART_PORT, &km_cfg));
    ESP_ERROR_CHECK(uart_set_pin(KM_UART_PORT, KM_UART_TX_PIN, KM_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreatePinnedToCore(km_uart_task, "km_uart", 4096, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "KM UART up on UART%d @ %d baud, RX=%d TX=%d",
             KM_UART_PORT, KM_UART_BAUD, KM_UART_RX_PIN, KM_UART_TX_PIN);
}
