// MAKCM_ESP32s3_Pass_Right — USB host side of the passthrough.
// Real controller plugs into Right's USB-OTG; Right snapshots its
// descriptors and forwards every URB over UART IPC to Left.
//
// Peripheral allocation:
//   USB-OTG : HOST mode → real controller
//   Serial  : USB-Serial-JTAG (effectively silent in host mode)
//   Serial1 : 5 Mbps UART → Left.  RX=GPIO2 (from Left's TX=GPIO2),
//                                  TX=GPIO1 (to Left's RX=GPIO1).
//
// Right does NOT touch KM. Left reads KM commands directly from its
// own UART0 (CH343 bridge → second PC COM3). Right is purely USB
// host + IPC.

#include <Arduino.h>
#include <string.h>
#include "pass_ipc.h"
#include "PassUsbHost.h"
#include "diag.h"

#define IPC_UART_BAUD   5000000UL
#define IPC_UART_RX     2
#define IPC_UART_TX     1

HardwareSerial IpcSerial(1);

// IPC frame handler — Left sends OUT endpoint data + control transfers.
void ipc_handle_frame(uint8_t type, uint8_t ep_addr, uint16_t seq,
                      const uint8_t *payload, uint16_t len) {
    switch (type) {
    case FRAME_CTRL_SETUP:
        // Payload is 8B SETUP, optionally followed by the OUT data stage
        // concatenated. Left coalesces setup+data into one frame so we
        // submit the full control transfer in a single URB.
        if (len >= 8) {
            const uint8_t *data_ptr = (len > 8) ? (payload + 8) : nullptr;
            uint16_t data_len = (uint16_t)(len - 8);
            pass_host.submit_control(payload, data_ptr, data_len, seq);
        }
        break;
    case FRAME_EP_OUT:
        pass_host.submit_out(ep_addr, payload, len);
        break;
    case FRAME_PING:
        extern bool ipc_send(uint8_t, uint8_t, uint16_t, const uint8_t *, uint16_t);
        ipc_send(FRAME_PING, 0, seq, nullptr, 0);
        break;
    case FRAME_LOG:
        Serial.write(payload, len);
        break;
    default:
        break;
    }
    (void)seq;
}

void ipc_pump_serial(void);
static void ipc_rx_task(void *) {
    for (;;) {
        ipc_pump_serial();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void setup() {
    Serial.begin(115200);
    IpcSerial.begin(IPC_UART_BAUD, SERIAL_8N1, IPC_UART_RX, IPC_UART_TX);

    diag_setup();      // LED task first so we see liveness even if
                       // usb_host_install hangs below.

    pass_host.begin(); // Installs usb_host lib, registers client,
                       // spawns lib_task + client_task on core 1.

    xTaskCreatePinnedToCore(ipc_rx_task, "ipc_rx", 8192, nullptr, 5, nullptr, 0);

    extern bool ipc_send(uint8_t, uint8_t, uint16_t, const uint8_t *, uint16_t);
    ipc_send(FRAME_LOG, 0, 0, (const uint8_t *)"Pass_Right boot\n", 16);
}

void loop() {
    // USB host event processing runs in its own tasks; keep loop() idle.
    delay(10);
}
