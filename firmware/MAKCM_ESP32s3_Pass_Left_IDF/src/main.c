// main.c — Left MCU entry. UART1 IPC from Right delivers the real
// controller's descriptors; Left caches them and starts TinyUSB so the
// target PC enumerates against the real VID/PID. Owns the USB-lifecycle
// thread (single thread for tinyusb_driver_install / set_descriptors /
// hot-disconnect / hot-reconnect) and a diag LED heartbeat.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "pass_ipc.h"

void ipc_init(void);
bool ipc_send(uint8_t type, uint8_t ep_addr, uint16_t seq,
              const uint8_t *payload, uint16_t len);

bool pass_usb_submit_in(uint8_t ep_addr, const uint8_t *data, uint16_t len);
bool pass_usb_control_in_complete(uint16_t seq, const uint8_t *data, uint16_t len);
void pass_usb_control_status(uint16_t seq, uint8_t status);
void pass_usb_disconnect(void);
void pass_usb_reconnect(void);

// From esp_tinyusb's private header. Updates internal descriptor pointers
// + string count without re-running tinyusb_driver_install (which would
// re-init the PHY and trip the INVALID_STATE bug — see pass_usb_disconnect).
extern esp_err_t tinyusb_set_descriptors(const tinyusb_config_t *config);

void km_ingest_raw(const uint8_t *payload, uint16_t len);
void km_apply(uint8_t ep_addr, uint8_t *buf, uint16_t len);
void km_init(void);
void km_reset_injection(void);

extern int km_uart_write(const void *data, size_t len);

static const char *TAG = "PassLeft";
#define DIAG_LED_PIN   GPIO_NUM_48

// Descriptor cache — populated by IPC from Right BEFORE USB enumeration.
// esp_tinyusb's tinyusb_config_t holds pointers to these at install time.
static tusb_desc_device_t desc_device;
static bool     desc_device_valid = false;
static uint8_t  desc_config[1024];
static uint16_t desc_config_len   = 0;
static bool     desc_config_valid = false;
static bool     device_ready      = false;
// usb_started: true once tinyusb_driver_install has succeeded once. The
// stack stays installed for the life of the firmware — esp_tinyusb 1.4.x
// can't cleanly install→uninstall→install (returns INVALID_STATE on the
// second install). Hot-swap goes through tud_disconnect / tud_connect.
static bool     usb_started       = false;
// host_visible: true when D+ pull-up is asserted and Windows can see us.
static bool     host_visible      = false;

struct StringCache {
    uint8_t  idx;
    uint8_t  byte_len;            // UTF-16LE bytes (not chars)
    uint8_t  utf16[128];
};
static struct StringCache strings[8];
static uint8_t            strings_count = 0;

static TaskHandle_t main_task_handle = NULL;

// Set on FRAME_DEVICE_GONE by ipc_handle_frame, consumed by main_task —
// keeps every USB-lifecycle operation on the same thread as start_usb.
static volatile bool teardown_pending = false;

// esp_tinyusb wants char* strings, so convert each cached UTF-16LE body
// to best-effort ASCII. idx 0 is the LangID code pair (0x0409 LE).
static char *str_bufs[10];
static char  str_storage[10][130];

static void build_string_array(uint8_t *count_out) {
    str_storage[0][0] = 0x09;
    str_storage[0][1] = 0x04;
    str_storage[0][2] = 0;
    str_bufs[0] = str_storage[0];
    uint8_t n = 1;

    for (uint8_t want_idx = 1; want_idx <= 7; ++want_idx) {
        char *dst = str_storage[n];
        int   w = 0;
        bool  found = false;
        for (uint8_t i = 0; i < strings_count; ++i) {
            if (strings[i].idx != want_idx) continue;
            const uint8_t *u = strings[i].utf16;
            for (uint16_t k = 0; k + 1 < strings[i].byte_len && w < 128; k += 2) {
                uint8_t lo = u[k], hi = u[k+1];
                dst[w++] = (hi == 0 && lo >= 0x20 && lo < 0x7F) ? (char)lo : '?';
            }
            dst[w] = 0;
            found = true;
            break;
        }
        if (!found) { dst[0] = 0; }
        str_bufs[n++] = dst;
    }
    *count_out = n;
}

static void build_tusb_cfg(tinyusb_config_t *out, uint8_t *str_n_out) {
    build_string_array(str_n_out);
    out->device_descriptor        = &desc_device;
    out->string_descriptor        = (const char **)str_bufs;
    out->string_descriptor_count  = *str_n_out;
    out->external_phy             = false;
    out->configuration_descriptor = desc_config;
#if TUD_OPT_HIGH_SPEED
    out->hs_configuration_descriptor = desc_config;
    out->qualifier_descriptor        = NULL;
#endif
}

static void start_usb(void) {
    if (!desc_device_valid || !desc_config_valid) return;

    uint8_t n = 0;
    tinyusb_config_t tusb_cfg = {0};
    build_tusb_cfg(&tusb_cfg, &n);

    char m[96];
    int ln;

    if (!usb_started) {
        ln = snprintf(m, sizeof(m),
            "[L] start_usb VID=%04x PID=%04x cfgLen=%u strCnt=%u\n",
            desc_device.idVendor, desc_device.idProduct,
            desc_config_len, strings_count);
        if (ln > 0) km_uart_write(m, ln);

        esp_err_t err = tinyusb_driver_install(&tusb_cfg);
        ln = snprintf(m, sizeof(m), "[L] tinyusb_driver_install err=0x%x\n", err);
        if (ln > 0) km_uart_write(m, ln);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tinyusb_driver_install failed: 0x%x", err);
            return;
        }
        usb_started  = true;
        host_visible = true;
        ESP_LOGI(TAG, "USB started with VID 0x%04X PID 0x%04X (cfg %uB, %u strings)",
                 desc_device.idVendor, desc_device.idProduct,
                 desc_config_len, strings_count);
        return;
    }

    // Stack already running, descriptors may have changed (different
    // controller plugged in). Refresh esp_tinyusb's pointers, then
    // assert D+ to retrigger host enumeration.
    ln = snprintf(m, sizeof(m),
        "[L] reconnect VID=%04x PID=%04x cfgLen=%u strCnt=%u\n",
        desc_device.idVendor, desc_device.idProduct,
        desc_config_len, strings_count);
    if (ln > 0) km_uart_write(m, ln);

    esp_err_t err = tinyusb_set_descriptors(&tusb_cfg);
    ln = snprintf(m, sizeof(m), "[L] tinyusb_set_descriptors err=0x%x\n", err);
    if (ln > 0) km_uart_write(m, ln);
    if (err != ESP_OK) return;

    pass_usb_reconnect();
    host_visible = true;
}

void ipc_handle_frame(uint8_t type, uint8_t ep_addr, uint16_t seq,
                      const uint8_t *payload, uint16_t len) {
    (void)ep_addr; (void)seq;
    bool poke_main = false;
    switch (type) {
    case FRAME_DESC_DEVICE:
        if (len == 18) {
            memcpy(&desc_device, payload, 18);
            desc_device_valid = true;
            poke_main = true;
        }
        break;
    case FRAME_DESC_CONFIG:
        if (len >= 4 && len <= sizeof(desc_config)) {
            uint16_t wTotal = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
            if (wTotal == len) {
                memcpy(desc_config, payload, len);
                desc_config_len   = len;
                desc_config_valid = true;
                poke_main = true;
            }
        }
        break;
    case FRAME_DESC_STRING:
        if (len >= 1 && strings_count < 8) {
            uint8_t idx     = payload[0];
            uint16_t raw    = len - 1;
            uint8_t stored  = (raw > sizeof(strings[0].utf16))
                                 ? (uint8_t)sizeof(strings[0].utf16)
                                 : (uint8_t)raw;
            strings[strings_count].idx      = idx;
            strings[strings_count].byte_len = stored;
            memcpy(strings[strings_count].utf16, payload + 1, stored);
            strings_count++;
        }
        break;
    case FRAME_DEVICE_READY:
        device_ready = true;
        poke_main = true;
        break;
    case FRAME_DEVICE_GONE:
        // Hand teardown off to main_task so install/uninstall/disconnect
        // always run from the same thread that called start_usb.
        teardown_pending = true;
        poke_main = true;
        break;
    case FRAME_EP_IN:
        pass_usb_submit_in(ep_addr, payload, len);
        break;
    case FRAME_CTRL_IN_DATA:
        pass_usb_control_in_complete(seq, payload, len);
        break;
    case FRAME_CTRL_STATUS:
        pass_usb_control_status(seq, len ? payload[0] : (uint8_t)XFER_ERROR);
        break;
    case FRAME_KM_INJECT:
        km_ingest_raw(payload, len);
        break;
    case FRAME_LOG:
        // Tunnel Right's logs out UART0 TX so they land on COM3. Prefix
        // "[R] " so the stream is easy to split from [L] lines.
        km_uart_write("[R] ", 4);
        km_uart_write(payload, len);
        if (len == 0 || payload[len - 1] != '\n') km_uart_write("\n", 1);
        break;
    case FRAME_PING:
        ipc_send(FRAME_PING, 0, seq, NULL, 0);
        break;
    default:
        break;
    }
    if (poke_main && main_task_handle) xTaskNotifyGive(main_task_handle);
}

// LED heartbeat — distinct period for each pipeline state.
static void led_task(void *arg) {
    (void)arg;
    gpio_set_direction(DIAG_LED_PIN, GPIO_MODE_OUTPUT);
    bool level = false;
    while (1) {
        uint32_t period;
        if (host_visible)         period = 100;    // 10 Hz — visible to host
        else if (device_ready)    period = 250;    // 4 Hz  — descriptors landed
        else if (desc_config_valid) period = 500;  // 2 Hz
        else if (desc_device_valid) period = 750;
        else                      period = 1500;   // nothing from IPC yet
        level = !level;
        gpio_set_level(DIAG_LED_PIN, level);
        vTaskDelay(pdMS_TO_TICKS(period));
    }
}

static void main_task(void *arg) {
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (teardown_pending) {
            teardown_pending = false;
            if (host_visible) {
                km_uart_write("[L] DEVICE_GONE — disconnect\n", 30);
                pass_usb_disconnect();
                km_reset_injection();
                host_visible = false;
            } else {
                ESP_LOGW(TAG, "DEVICE_GONE while not visible to host");
            }
            // Re-arm staging so next FRAME_DESC_* / FRAME_DEVICE_READY
            // cycle has to repopulate before reconnect fires. usb_started
            // stays true — only the D+ pull-up was dropped.
            desc_device_valid = false;
            desc_config_valid = false;
            desc_config_len   = 0;
            strings_count     = 0;
            device_ready      = false;
            continue;
        }
        if (!host_visible && device_ready &&
            desc_device_valid && desc_config_valid) {
            start_usb();
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Pass_Left up — waiting for descriptors from Right on UART1");
    ipc_init();
    km_init();
    xTaskCreatePinnedToCore(led_task,  "led",   2048, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(main_task, "main",  4096, NULL, 3, &main_task_handle, 1);
}
