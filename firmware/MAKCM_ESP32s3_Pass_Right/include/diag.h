#pragma once
// Pass_Right diag layer — pure instrumentation. Right's `Serial` is
// effectively silent during host mode (USB_MODE=1 + CDC_ON_BOOT=1 routes
// to USB-Serial-JTAG which shares pins with USB-OTG), so an on-board
// LED state machine is the only direct visibility into Right's
// pipeline progress. Each state corresponds to a host-stack milestone.

#include <stdint.h>

#define PASS_RIGHT_DIAG_RGB_PIN     48
#define PASS_RIGHT_DIAG_PLAIN_PIN   -1

#ifdef __cplusplus
extern "C" {
#endif

void diag_setup(void);

void diag_on_host_install(bool ok);
void diag_on_client_registered(void);
void diag_on_new_dev(uint8_t address);
void diag_on_descriptors_sent(void);
void diag_on_device_ready(void);

void diag_on_ipc_tx_bytes(uint16_t n);

#ifdef __cplusplus
}
#endif
