// PassUsbHost.cpp — see header for design notes.
//
// Enumerates the real controller, snapshots descriptors, claims every
// alt-0 interface, opens its IN endpoints, and forwards URBs to Left
// via IPC. Output is binary IPC frames (not JSON / HID-decoded).

#include "PassUsbHost.h"
#include "pass_ipc.h"
#include "diag.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <esp_log.h>

extern bool ipc_send(uint8_t type, uint8_t ep_addr, uint16_t seq,
                     const uint8_t *payload, uint16_t len);

// Diagnostic log tunneled over IPC (Left forwards to UART0 → CH343 →
// COM3). Right's USB-Serial-JTAG is dead while host mode is active, so
// this is the only visibility into Right's progress.
static void R_LOG_fmt(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < sizeof(buf)) {
        ipc_send(FRAME_LOG, 0, 0, (const uint8_t *)buf, (uint16_t)n);
    }
}
#define R_LOG(...)  R_LOG_fmt(__VA_ARGS__)

static const char *TAG = "PassUsbHost";

// Per-connection budget of "IN done" diagnostic lines. Reset on every
// NEW_DEV so a replugged controller is visible too, not just the first
// one since boot.
static uint16_t s_in_dbg = 0;

PassUsbHost pass_host;

void PassUsbHost::lib_task(void *arg) {
    auto *self = static_cast<PassUsbHost *>(arg);
    // Register the client only after the lib state machine has stepped
    // at least once — registering too early can miss NEW_DEV events for
    // already-plugged devices.
    while (true) {
        uint32_t flags = 0;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "usb_host_lib_handle_events err=0x%x", err);
            continue;
        }
        if (self->client_handle_ == nullptr) {
            const usb_host_client_config_t cfg = {
                .is_synchronous = false,
                .max_num_event_msg = 10,
                .async = {
                    .client_event_callback = &PassUsbHost::client_event_cb,
                    .callback_arg = self,
                },
            };
            err = usb_host_client_register(&cfg, &self->client_handle_);
            R_LOG("client_register err=0x%x", err);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "client_register err=0x%x", err);
            } else {
                ESP_LOGI(TAG, "client registered");
                diag_on_client_registered();
            }
        }
    }
    (void)self;
}

void PassUsbHost::client_task(void *arg) {
    auto *self = static_cast<PassUsbHost *>(arg);
    while (true) {
        if (self->client_handle_ == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        usb_host_client_handle_events(self->client_handle_, portMAX_DELAY);
    }
}

void PassUsbHost::begin() {
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_cfg);
    diag_on_host_install(err == ESP_OK);
    R_LOG("usb_host_install err=0x%x", err);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install err=0x%x", err);
        return;
    }
    xTaskCreatePinnedToCore(&PassUsbHost::lib_task,    "usb_lib",    8192, this, 5, &lib_task_,    1);
    xTaskCreatePinnedToCore(&PassUsbHost::client_task, "usb_client", 8192, this, 5, &client_task_, 1);
    ESP_LOGI(TAG, "host started");
}

void PassUsbHost::client_event_cb(const usb_host_client_event_msg_t *msg, void *arg) {
    auto *self = static_cast<PassUsbHost *>(arg);
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        self->on_new_device(msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        self->on_device_gone();
        break;
    default:
        break;
    }
}

void PassUsbHost::on_new_device(uint8_t address) {
    ESP_LOGI(TAG, "NEW_DEV addr=%u", address);
    R_LOG("NEW_DEV addr=%u", address);
    device_connected_ = true;
    gip_got_input_ = false;   // re-arm GIP handshake for this device
    gip_seq_       = 1;
    s_in_dbg       = 0;       // fresh IN-completion log budget per device
    diag_on_new_dev(address);

    esp_err_t err = usb_host_device_open(client_handle_, address, &device_handle_);
    R_LOG("device_open err=0x%x", err);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device_open err=0x%x", err);
        return;
    }
    bool rel_ok = fetch_and_relay_descriptors();
    R_LOG("fetch_and_relay_descriptors=%d", (int)rel_ok);
    if (!rel_ok) {
        ESP_LOGE(TAG, "descriptor relay failed — device not usable");
        // Tell Left we're not bringing the device up. Otherwise Left
        // waits forever for a FRAME_DEVICE_READY that never comes.
        ipc_send(FRAME_DEVICE_GONE, 0, 0, nullptr, 0);
        release_all();
        return;
    }
    // MS OS responses are forwarded live via the control-transfer
    // pipeline, so an early probe + cache path isn't needed.
    ipc_send(FRAME_DEVICE_READY, 0, 0, nullptr, 0);
    ready_ = true;
    diag_on_device_ready();
}

// Left asks for this on boot: if it rebooted (reflash) after we already
// enumerated the device, the one-shot descriptor relay + DEVICE_READY
// never reached it and its USB device stack stays down forever — the
// console-facing port looks electrically dead. Re-ship the descriptor
// set without re-claiming interfaces (they're already ours).
void PassUsbHost::resync() {
    if (!device_connected_ || !ready_ || !device_handle_) {
        R_LOG("resync ignored (no ready device)");
        return;
    }
    R_LOG("resync: re-relaying descriptors");
    if (fetch_and_relay_descriptors(false)) {
        ipc_send(FRAME_DEVICE_READY, 0, 0, nullptr, 0);
    }
}

void PassUsbHost::on_device_gone() {
    ESP_LOGI(TAG, "DEV_GONE");
    ready_ = false;
    device_connected_ = false;
    release_all();
    ipc_send(FRAME_DEVICE_GONE, 0, 0, nullptr, 0);
}

bool PassUsbHost::fetch_and_relay_descriptors(bool claim_interfaces) {
    const usb_device_desc_t *dev_desc = nullptr;
    esp_err_t e = usb_host_get_device_descriptor(device_handle_, &dev_desc);
    R_LOG("get_device_desc err=0x%x", e);
    if (e != ESP_OK) return false;
    R_LOG("dev VID=%04x PID=%04x cls=%02x/%02x/%02x bcdDev=%04x",
          (unsigned)dev_desc->idVendor, (unsigned)dev_desc->idProduct,
          (unsigned)dev_desc->bDeviceClass, (unsigned)dev_desc->bDeviceSubClass,
          (unsigned)dev_desc->bDeviceProtocol, (unsigned)dev_desc->bcdDevice);
    ipc_send(FRAME_DESC_DEVICE, 0, 0, (const uint8_t *)dev_desc, 18);

    const usb_config_desc_t *cfg_desc = nullptr;
    e = usb_host_get_active_config_descriptor(device_handle_, &cfg_desc);
    R_LOG("get_config_desc err=0x%x", e);
    if (e != ESP_OK) return false;
    R_LOG("cfg wTotal=%u nIf=%u",
          (unsigned)cfg_desc->wTotalLength, (unsigned)cfg_desc->bNumInterfaces);
    ipc_send(FRAME_DESC_CONFIG, 0, 0, (const uint8_t *)cfg_desc, cfg_desc->wTotalLength);

    // Ship every referenced string descriptor: device manufacturer /
    // product / serial, config string, and each interface string.
    uint8_t  idx_set[16] = {0};
    uint8_t  idx_n = 0;
    auto push_idx = [&](uint8_t i) {
        if (!i) return;
        for (uint8_t k = 0; k < idx_n; ++k) if (idx_set[k] == i) return;
        if (idx_n < sizeof(idx_set)) idx_set[idx_n++] = i;
    };
    push_idx(dev_desc->iManufacturer);
    push_idx(dev_desc->iProduct);
    push_idx(dev_desc->iSerialNumber);
    push_idx(cfg_desc->iConfiguration);

    const uint8_t *p   = (const uint8_t *)cfg_desc;
    const uint8_t *end = p + cfg_desc->wTotalLength;
    while (p + 2 <= end) {
        uint8_t blen = p[0], btyp = p[1];
        if (!blen) break;
        if (btyp == USB_B_DESCRIPTOR_TYPE_INTERFACE && blen >= 9) {
            const usb_intf_desc_t *ifd = (const usb_intf_desc_t *)p;
            push_idx(ifd->iInterface);
        }
        p += blen;
    }

    usb_device_info_t dinfo;
    if (usb_host_device_info(device_handle_, &dinfo) == ESP_OK) {
        auto ship_one = [&](uint8_t idx, const usb_str_desc_t *sd) {
            if (!sd || !idx) return;
            uint8_t body_len = (sd->bLength > 2) ? (sd->bLength - 2) : 0;
            uint8_t buf[129];
            buf[0] = idx;
            if (body_len > sizeof(buf) - 1) body_len = sizeof(buf) - 1;
            // usb_str_desc_t is a union; val[] overlays the whole
            // descriptor starting at bLength. Skip the 2-byte header so
            // we ship only the UTF-16LE body — what Left expects.
            memcpy(&buf[1], sd->val + 2, body_len);
            ipc_send(FRAME_DESC_STRING, 0, 0, buf, 1 + body_len);
        };
        ship_one(dev_desc->iManufacturer, dinfo.str_desc_manufacturer);
        ship_one(dev_desc->iProduct,      dinfo.str_desc_product);
        ship_one(dev_desc->iSerialNumber, dinfo.str_desc_serial_num);
    }
    diag_on_descriptors_sent();

    // Claim every interface's alt-0 and open its IN endpoints. (Alt 0 is
    // what SET_CONFIG selects; SET_INTERFACE on alt > 0 is currently
    // forwarded as a control transfer but new endpoints aren't reopened.)
    // Skipped on a resync — interfaces/endpoints are already ours.
    if (!claim_interfaces) return true;
    p = (const uint8_t *)cfg_desc;
    while (p + 2 <= end) {
        uint8_t blen = p[0], btyp = p[1];
        if (!blen) break;
        if (btyp == USB_B_DESCRIPTOR_TYPE_INTERFACE && blen >= 9) {
            const usb_intf_desc_t *ifd = (const usb_intf_desc_t *)p;
            if (ifd->bAlternateSetting == 0 && claimed_if_count_ < PASS_MAX_INTERFACES) {
                esp_err_t ce = usb_host_interface_claim(client_handle_, device_handle_,
                                                       ifd->bInterfaceNumber, 0);
                R_LOG("claim if=%u err=0x%x cls=%02x/%02x/%02x",
                      (unsigned)ifd->bInterfaceNumber, ce,
                      (unsigned)ifd->bInterfaceClass,
                      (unsigned)ifd->bInterfaceSubClass,
                      (unsigned)ifd->bInterfaceProtocol);
                if (ce == ESP_OK) {
                    claimed_ifs_[claimed_if_count_++] = ifd->bInterfaceNumber;
                    open_all_endpoints_for_interface(cfg_desc, ifd->bInterfaceNumber, 0);
                } else {
                    ESP_LOGW(TAG, "interface_claim if=%u err=0x%x",
                             ifd->bInterfaceNumber, ce);
                }
            }
        }
        p += blen;
    }
    return true;
}

// Submit an IN transfer for every IN endpoint belonging to (iface, alt).
// OUT endpoints are allocated on demand in submit_out().
bool PassUsbHost::open_all_endpoints_for_interface(const usb_config_desc_t *cfg,
                                                    uint8_t iface_num,
                                                    uint8_t alt) {
    const uint8_t *p   = (const uint8_t *)cfg;
    const uint8_t *end = p + cfg->wTotalLength;
    bool inside = false;

    while (p + 2 <= end) {
        uint8_t blen = p[0], btyp = p[1];
        if (!blen) break;
        if (btyp == USB_B_DESCRIPTOR_TYPE_INTERFACE && blen >= 9) {
            const usb_intf_desc_t *ifd = (const usb_intf_desc_t *)p;
            inside = (ifd->bInterfaceNumber == iface_num && ifd->bAlternateSetting == alt);
        } else if (btyp == USB_B_DESCRIPTOR_TYPE_ENDPOINT && blen >= 7 && inside) {
            const usb_ep_desc_t *ed = (const usb_ep_desc_t *)p;
            bool is_in = (ed->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) != 0;

            if (!is_in) {
                gip_out_ep_ = ed->bEndpointAddress;   // learn OUT ep for GIP init
                R_LOG("OUT ep=%02x", (unsigned)ed->bEndpointAddress);
            }

            if (is_in && in_transfer_count_ < PASS_MAX_ENDPOINTS) {
                usb_transfer_t *t = nullptr;
                uint16_t mps = ed->wMaxPacketSize ? ed->wMaxPacketSize : 64;
                if (usb_host_transfer_alloc(mps, 0, &t) == ESP_OK) {
                    t->device_handle     = device_handle_;
                    t->bEndpointAddress  = ed->bEndpointAddress;
                    t->num_bytes         = mps;
                    t->callback          = &PassUsbHost::in_xfer_complete;
                    t->context           = this;
                    in_transfers_[in_transfer_count_++] = t;
                    esp_err_t e = usb_host_transfer_submit(t);
                    ESP_LOGI(TAG, "IN submit ep=0x%02x err=0x%x", ed->bEndpointAddress, e);
                    R_LOG("IN open ep=%02x mps=%u submit_err=0x%x",
                          (unsigned)ed->bEndpointAddress, (unsigned)mps, e);
                }
            }
        }
        p += blen;
    }
    return true;
}

// Build and send a GIP packet on the interrupt OUT endpoint.
// Wire layout: [command][options][sequence][length][payload...], length is a
// single byte here (all our payloads are < 128). Sequence must be non-zero
// and increments per packet. options = client_id | GIP_OPT_INTERNAL(0x20).
void PassUsbHost::gip_send(uint8_t cmd, uint8_t options,
                           const uint8_t *payload, uint8_t plen) {
    uint8_t pkt[16];
    if (gip_seq_ == 0) gip_seq_ = 1;
    pkt[0] = cmd;
    pkt[1] = options;
    pkt[2] = gip_seq_++;
    pkt[3] = plen;
    for (uint8_t i = 0; i < plen && i < sizeof(pkt) - 4; ++i) pkt[4 + i] = payload[i];
    uint8_t total = 4 + plen;
    if (total & 1) pkt[total++] = 0x00;      // pad to even length
    bool ok = submit_out(gip_out_ep_, pkt, total);
    R_LOG("GIP send cmd=%02x seq=%u ok=%d", (unsigned)cmd, (unsigned)pkt[2], (int)ok);
}

// Kick a freshly-announced GIP controller into reporting: request identify,
// then power on, then set LED. Sequence per the xone driver.
void PassUsbHost::gip_kickstart() {
    const uint8_t opt = 0x20;                 // client_id 0 | internal
    gip_send(0x04, opt, nullptr, 0);          // GIP_CMD_IDENTIFY
    uint8_t pwr = 0x00;                        // GIP_PWR_ON
    gip_send(0x05, opt, &pwr, 1);             // GIP_CMD_POWER
    uint8_t led[3] = { 0x00, 0x01, 0x14 };    // reserved, LED_ON, brightness
    gip_send(0x0a, opt, led, 3);              // GIP_CMD_LED
}

void PassUsbHost::in_xfer_complete(usb_transfer_t *t) {
    auto *self = static_cast<PassUsbHost *>(t->context);
    // If the device is gone (release_all ran) or status is NO_DEVICE /
    // CANCELED, do NOT re-submit — the transfer is about to be freed
    // and a re-submit would race to double-free. ESP-IDF sets every
    // in-flight URB to NO_DEVICE before firing DEV_GONE.
    if (t->status == USB_TRANSFER_STATUS_NO_DEVICE ||
        t->status == USB_TRANSFER_STATUS_CANCELED ||
        !self->device_connected_) {
        return;
    }
    // Diagnostic: log the first ~20 IN completions per device connection so
    // we can see whether the controller is streaming at all, and with what
    // status/bytes.
    if (s_in_dbg < 20) {
        s_in_dbg++;
        R_LOG("IN done ep=%02x status=%d nbytes=%u b0=%02x",
              (unsigned)t->bEndpointAddress, (int)t->status,
              (unsigned)t->actual_num_bytes,
              t->actual_num_bytes ? (unsigned)t->data_buffer[0] : 0);
    }
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes > 0) {
        uint8_t b0 = t->data_buffer[0];
        // GIP handshake: 0x20 = input report (device is live), 0x02 = announce
        // (device waiting to be initialized). Kick it until input flows.
        if (b0 == 0x20) {
            self->gip_got_input_ = true;
        } else if (b0 == 0x02 && !self->gip_got_input_) {
            self->gip_kickstart();
        }
        ipc_send(FRAME_EP_IN, t->bEndpointAddress, 0,
                 t->data_buffer, (uint16_t)t->actual_num_bytes);
    }
    usb_host_transfer_submit(t);
}

void PassUsbHost::out_xfer_complete(usb_transfer_t *t) {
    usb_host_transfer_free(t);
}

void PassUsbHost::control_xfer_complete(usb_transfer_t *t) {
    uint16_t seq = (uint16_t)(uintptr_t)t->context;
    R_LOG("ctl done seq=%u status=%d nbytes=%u", seq, (int)t->status,
          (unsigned)t->actual_num_bytes);
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        if (t->actual_num_bytes > 8) {
            ipc_send(FRAME_CTRL_IN_DATA, 0, seq,
                     t->data_buffer + 8, (uint16_t)(t->actual_num_bytes - 8));
        }
        uint8_t status = XFER_OK;
        ipc_send(FRAME_CTRL_STATUS, 0, seq, &status, 1);
    } else {
        uint8_t status = (t->status == USB_TRANSFER_STATUS_STALL) ? XFER_STALL
                       : (t->status == USB_TRANSFER_STATUS_TIMED_OUT) ? XFER_TIMEOUT
                       : XFER_ERROR;
        ipc_send(FRAME_CTRL_STATUS, 0, seq, &status, 1);
    }
    usb_host_transfer_free(t);
}

bool PassUsbHost::submit_out(uint8_t ep_addr, const uint8_t *data, uint16_t len) {
    if (!device_connected_ || !device_handle_) return false;
    usb_transfer_t *t = nullptr;
    if (usb_host_transfer_alloc(len, 0, &t) != ESP_OK) return false;
    t->device_handle    = device_handle_;
    t->bEndpointAddress = ep_addr;
    t->num_bytes        = len;
    memcpy(t->data_buffer, data, len);
    t->callback = &PassUsbHost::out_xfer_complete;
    t->context  = this;
    if (usb_host_transfer_submit(t) != ESP_OK) {
        usb_host_transfer_free(t);
        return false;
    }
    return true;
}

bool PassUsbHost::submit_control(const uint8_t setup[8], const uint8_t *data_out,
                                  uint16_t len, uint16_t seq) {
    R_LOG("ctl SUBMIT seq=%u setup=%02x %02x %02x %02x %02x %02x %02x %02x dlen=%u",
          seq, setup[0], setup[1], setup[2], setup[3],
          setup[4], setup[5], setup[6], setup[7], (unsigned)len);
    // ANY failure path must emit FRAME_CTRL_STATUS so Left's ctl_pending
    // clears; otherwise every subsequent control stalls.
    auto fail = [&](const char *why, uint8_t status_code) -> bool {
        R_LOG("ctl FAIL seq=%u why=%s", seq, why);
        uint8_t s = status_code;
        ipc_send(FRAME_CTRL_STATUS, 0, seq, &s, 1);
        return false;
    };

    if (!device_connected_ || !device_handle_) {
        return fail("NO-DEV", XFER_ERROR);
    }

    const uint16_t wLength = ((uint16_t)setup[6]) | ((uint16_t)setup[7] << 8);
    const bool is_in = (setup[0] & 0x80) != 0;
    const uint16_t data_stage = is_in ? wLength : len;

    // For OUT controls, reject if caller's len doesn't match setup
    // wLength. Prevents ESP-IDF returning ESP_ERR_INVALID_ARG.
    if (!is_in && len != wLength) {
        return fail("OUT-LEN-MISMATCH", XFER_ERROR);
    }

    usb_transfer_t *t = nullptr;
    if (usb_host_transfer_alloc(8 + data_stage, 0, &t) != ESP_OK) {
        return fail("ALLOC-FAIL", XFER_ERROR);
    }

    memcpy(t->data_buffer, setup, 8);
    if (!is_in && len) {
        memcpy(t->data_buffer + 8, data_out, len);
    }
    t->device_handle    = device_handle_;
    t->bEndpointAddress = 0x00;
    t->num_bytes        = 8 + data_stage;
    t->callback         = &PassUsbHost::control_xfer_complete;
    t->context          = (void *)(uintptr_t)seq;

    esp_err_t e = usb_host_transfer_submit_control(client_handle_, t);
    if (e != ESP_OK) {
        R_LOG("ctl SUBMIT-FAIL seq=%u err=0x%x", seq, e);
        usb_host_transfer_free(t);
        uint8_t s = XFER_ERROR;
        ipc_send(FRAME_CTRL_STATUS, 0, seq, &s, 1);
        return false;
    }
    return true;
}

void PassUsbHost::release_all() {
    // Mark disconnected FIRST so any in-flight IN completions that fire
    // while we're tearing down bail out of the re-submit path cleanly.
    device_connected_ = false;

    // ESP-IDF completes every in-flight URB with status=NO_DEVICE before
    // dispatching DEV_GONE, so they're safe to free here. Halt+flush is
    // belt-and-suspenders against double-free races.
    for (uint8_t i = 0; i < in_transfer_count_; ++i) {
        if (in_transfers_[i]) {
            if (device_handle_) {
                usb_host_endpoint_halt(device_handle_, in_transfers_[i]->bEndpointAddress);
                usb_host_endpoint_flush(device_handle_, in_transfers_[i]->bEndpointAddress);
            }
            usb_host_transfer_free(in_transfers_[i]);
            in_transfers_[i] = nullptr;
        }
    }
    in_transfer_count_ = 0;

    for (uint8_t i = 0; i < claimed_if_count_; ++i) {
        usb_host_interface_release(client_handle_, device_handle_, claimed_ifs_[i]);
        claimed_ifs_[i] = 0;
    }
    claimed_if_count_ = 0;
    if (device_handle_) {
        usb_host_device_close(client_handle_, device_handle_);
        device_handle_ = nullptr;
    }
}
