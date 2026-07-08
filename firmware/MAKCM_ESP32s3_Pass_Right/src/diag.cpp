// Pass_Right diagnostic LED — see include/diag.h.

#include <Arduino.h>
#include "diag.h"

static volatile bool     diag_host_install_ok     = false;
static volatile bool     diag_client_registered   = false;
static volatile bool     diag_new_dev_seen        = false;
static volatile uint8_t  diag_new_dev_address     = 0;
static volatile bool     diag_descriptors_sent    = false;
static volatile bool     diag_device_ready        = false;
static volatile uint32_t diag_ipc_tx_byte_count   = 0;

extern "C" void diag_on_host_install(bool ok)   { diag_host_install_ok   = ok; }
extern "C" void diag_on_client_registered(void) { diag_client_registered = true; }
extern "C" void diag_on_new_dev(uint8_t addr)   { diag_new_dev_address = addr; diag_new_dev_seen = true; }
extern "C" void diag_on_descriptors_sent(void)  { diag_descriptors_sent  = true; }
extern "C" void diag_on_device_ready(void)      { diag_device_ready      = true; }
extern "C" void diag_on_ipc_tx_bytes(uint16_t n){ diag_ipc_tx_byte_count += n; }

enum DiagState : uint8_t {
    DS_IDLE           = 0,  // setup() never reached diag_setup() (crash/loop)
    DS_HOST_INSTALL   = 1,  // usb_host_install returned ESP_OK
    DS_CLIENT         = 2,  // client registered (lib_task alive)
    DS_NEW_DEV        = 3,  // NEW_DEV client event fired
    DS_DESC_SENT      = 4,  // descriptors fetched and IPC-shipped to Left
    DS_DEVICE_READY   = 5   // FRAME_DEVICE_READY emitted
};

static DiagState current_state(void) {
    if (diag_device_ready)       return DS_DEVICE_READY;
    if (diag_descriptors_sent)   return DS_DESC_SENT;
    if (diag_new_dev_seen)       return DS_NEW_DEV;
    if (diag_client_registered)  return DS_CLIENT;
    if (diag_host_install_ok)    return DS_HOST_INSTALL;
    return DS_IDLE;
}

static void led_rgb(uint8_t r, uint8_t g, uint8_t b) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    rgbLedWrite(PASS_RIGHT_DIAG_RGB_PIN, r, g, b);
#else
    neopixelWrite(PASS_RIGHT_DIAG_RGB_PIN, r, g, b);
#endif
}

static void pattern_step(DiagState s, uint32_t t) {
    uint8_t r = 0, g = 0, b = 0;
    bool on = false;
    switch (s) {
    case DS_IDLE:          r = 128;                    on = (t %   50) <  25;                              break;
    case DS_HOST_INSTALL:  r = 180; g =  60;           on = (t %  200) < 100;                              break;
    case DS_CLIENT:        r = 180; g = 180;           on = (t % 1000) < 500;                              break;
    case DS_NEW_DEV:     { g = 200; uint32_t p = t % 1000;
                           on = (p < 100) || (p >= 200 && p < 300);                                        break; }
    case DS_DESC_SENT:   { g =  80; b = 200; uint32_t p = t % 1250;
                           on = (p < 100) || (p >= 250 && p < 350);                                        break; }
    case DS_DEVICE_READY:  r = 180; g = 180; b = 180;  on = true;                                          break;
    }
    if (on) {
        led_rgb(r, g, b);
        if (PASS_RIGHT_DIAG_PLAIN_PIN >= 0) digitalWrite(PASS_RIGHT_DIAG_PLAIN_PIN, HIGH);
    } else {
        led_rgb(0, 0, 0);
        if (PASS_RIGHT_DIAG_PLAIN_PIN >= 0) digitalWrite(PASS_RIGHT_DIAG_PLAIN_PIN, LOW);
    }
}

static void diag_led_task(void *) {
    pinMode(PASS_RIGHT_DIAG_RGB_PIN, OUTPUT);
    if (PASS_RIGHT_DIAG_PLAIN_PIN >= 0) pinMode(PASS_RIGHT_DIAG_PLAIN_PIN, OUTPUT);
    led_rgb(0, 0, 0);
    for (;;) {
        pattern_step(current_state(), millis());
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void diag_setup(void) {
    // LED task on core 0 (usb_host tasks live on core 1 — keep them apart).
    xTaskCreatePinnedToCore(diag_led_task, "diag_led", 2048, nullptr, 1, nullptr, 0);
}
