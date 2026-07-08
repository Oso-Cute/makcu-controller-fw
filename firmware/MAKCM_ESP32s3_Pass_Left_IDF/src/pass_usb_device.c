// pass_usb_device.c — custom TinyUSB class driver for the passthrough.
// Walks the config descriptor Left received from Right, opens every
// declared endpoint, and wires traffic to/from Right via IPC frames:
//
//   OUT endpoints (target PC → real device): xfer_cb fires per packet,
//     we pack into FRAME_EP_OUT and re-arm.
//   IN endpoints (real device → target PC): pass_usb_submit_in() is
//     called from ipc.c on each FRAME_EP_IN; we submit to TinyUSB.
//   Control transfers: SETUP captured, shipped as FRAME_CTRL_SETUP,
//     Right hands it to the real device, response comes back as
//     FRAME_CTRL_IN_DATA + FRAME_CTRL_STATUS.
//
// Device-recipient vendor control requests are forwarded live through
// the same FRAME_CTRL_SETUP pipeline — covers MS OS 1.0 (Compat ID,
// Extended Properties) and any bespoke vendor protocol the real driver
// uses (e.g. PowerA register-access on 20D6:4001). A local cache can't
// match the device's wLength/wValue/wIndex-dependent responses; live
// forwarding mirrors it 1:1, stall and all.

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tusb.h"
#include "device/usbd.h"
#include "device/usbd_pvt.h"
#include "tinyusb.h"
#include "pass_ipc.h"

// Latency diagnostic — emits one line per LAT_DIAG_EMIT_N IN submissions:
//   [L] LAT n=100 sub=12/34/178us gap=3120/4012/8220us coal=0
// sub  = per-report submit cost (km_apply + edpt_claim + edpt_xfer)
// gap  = inter-arrival gap between consecutive submit_in entries
// coal = count of fresh reports replaced before going out (coalesce)
#ifndef LAT_DIAG
#define LAT_DIAG 1
#endif
#define LAT_DIAG_EMIT_N 100

#if LAT_DIAG
static struct {
    uint32_t count;
    uint32_t drops;
    int64_t  sum_submit_us;
    int64_t  min_submit_us;
    int64_t  max_submit_us;
    int64_t  last_entry_us;
    int64_t  sum_gap_us;
    int64_t  min_gap_us;
    int64_t  max_gap_us;
} lat = { .min_submit_us = INT64_MAX, .min_gap_us = INT64_MAX };

static void lat_reset(void) {
    lat.count = 0; lat.drops = 0;
    lat.sum_submit_us = 0; lat.min_submit_us = INT64_MAX; lat.max_submit_us = 0;
    lat.sum_gap_us = 0;    lat.min_gap_us = INT64_MAX;    lat.max_gap_us = 0;
}
#endif

static const char *TAG = "pass_cd";

extern uint8_t  desc_config[];
extern uint16_t desc_config_len;
extern bool ipc_send(uint8_t type, uint8_t ep_addr, uint16_t seq,
                     const uint8_t *payload, uint16_t len);

extern void km_apply(uint8_t ep_addr, uint8_t *buf, uint16_t len);
extern int  km_uart_write(const void *data, size_t len);

// Left-origin log directly to UART0 (not via IPC — that would round-trip
// to Right which has no output path). Prefix "[L] " for easy splitting.
static void L_LOG(const char *fmt, ...) {
    char buf[160];
    int pfx = snprintf(buf, sizeof(buf), "[L] ");
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pfx, sizeof(buf) - pfx - 1, fmt, ap);
    va_end(ap);
    if (n > 0) {
        int total = pfx + n;
        if (total >= (int)sizeof(buf) - 1) total = sizeof(buf) - 2;
        buf[total++] = '\n';
        km_uart_write(buf, total);
    }
}

static bool pass_driver_control_xfer(uint8_t rhport, uint8_t stage,
                                      tusb_control_request_t const *request);

// esp_tinyusb's wrapper for device-recipient vendor control requests.
// Forwarded through the same pipeline as class/interface requests so
// Windows sees the real device's exact response.
bool tinyusb_vendor_control_request_cb(uint8_t rhport, uint8_t stage,
                                        tusb_control_request_t const *request) {
    L_LOG("L vendor_req stage=%u bmReq=%02x bReq=%02x wVal=%04x wIdx=%04x wLen=%u",
          (unsigned)stage, request->bmRequestType, request->bRequest,
          request->wValue, request->wIndex, request->wLength);
    return pass_driver_control_xfer(rhport, stage, request);
}

// Some TinyUSB versions dispatch device-recipient vendor requests here
// directly (bypassing class drivers' control_xfer_cb). Mirror the wrapper
// so either routing path works.
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
    return tinyusb_vendor_control_request_cb(rhport, stage, request);
}

void tud_mount_cb(void)   { L_LOG("L tud_mount_cb"); }
void tud_umount_cb(void)  { L_LOG("L tud_umount_cb"); }
void tud_suspend_cb(bool remote_wakeup_en) { L_LOG("L tud_suspend rwe=%u", remote_wakeup_en); }
void tud_resume_cb(void)  { L_LOG("L tud_resume_cb"); }

#define PASS_MAX_EPS  8

typedef struct {
    uint8_t  addr;        // with direction bit
    uint16_t mps;
    bool     is_in;
    bool     in_flight;
    uint8_t  rx_buf[64];           // OUT: USB → us. IN: currently submitted.
    uint8_t  in_pending_buf[64];   // IN: latest coalesced report waiting for xfer_cb
    uint16_t in_pending_len;       // 0 means nothing pending
    // Synth template: last real IN report bytes, re-submitted with
    // km_apply overlaid when the real device is idle AND injection is
    // active. GIP controllers only emit on change; without this a brief
    // injection window has near-zero probability of being observed.
    uint8_t  tpl_buf[64];
    uint16_t tpl_len;
    bool     tpl_have;
    int64_t  last_real_us;
} OpenEp;

static OpenEp  open_eps[PASS_MAX_EPS];
static uint8_t open_ep_count = 0;
static bool    pass_mounted  = false;

bool km_has_active_injection(void);

// Short critical section protects (in_flight, in_pending_*) across
// ipc_rx_task (submitter) and TinyUSB task (xfer_cb). Sections < 1µs.
static portMUX_TYPE io_lock = portMUX_INITIALIZER_UNLOCKED;

static OpenEp *find_ep(uint8_t addr) {
    for (uint8_t i = 0; i < open_ep_count; ++i)
        if (open_eps[i].addr == addr) return &open_eps[i];
    return NULL;
}

// Control-transfer SETUP tracking (single-pending).
static uint16_t              ctl_seq_counter = 0;
static uint16_t              ctl_pending_seq = 0;
static bool                  ctl_pending     = false;
static uint8_t               ctl_rhport      = 0;
static tusb_control_request_t ctl_pending_request;
static int64_t               ctl_setup_us    = 0;

// OUT control-transfer data stage staging. For class OUT requests with
// wLength > 0 we receive into this buffer, then forward setup+data
// combined as one FRAME_CTRL_SETUP at DATA stage.
#define CTL_OUT_BUF_SZ 256
static uint8_t ctl_out_buf[CTL_OUT_BUF_SZ];

static void synth_timer_start_once(void);

static void pass_driver_init(void) {
    L_LOG("L pass_driver_init");
    open_ep_count = 0;
    pass_mounted  = false;
    ctl_pending   = false;
    synth_timer_start_once();
}

static void pass_driver_reset(uint8_t rhport) {
    L_LOG("L pass_driver_reset rhport=%u", rhport);
    (void)rhport;
    for (uint8_t i = 0; i < open_ep_count; ++i) {
        open_eps[i].in_flight      = false;
        open_eps[i].in_pending_len = 0;
        open_eps[i].tpl_have       = false;
        open_eps[i].tpl_len        = 0;
        open_eps[i].last_real_us   = 0;
    }
    open_ep_count = 0;
    pass_mounted  = false;
    ctl_pending   = false;
}

// open() is called by TinyUSB during SET_CONFIGURATION for each interface.
// We consume the interface + endpoint descriptors for alt 0 and open
// each endpoint with usbd_edpt_open. Return byte-count consumed.
static uint16_t pass_driver_open(uint8_t rhport,
                                  tusb_desc_interface_t const *itf_desc,
                                  uint16_t max_len) {
    if (!itf_desc || max_len < 2) return 0;
    const uint8_t *base = (const uint8_t *)itf_desc;
    if (base[1] != TUSB_DESC_INTERFACE) return base[0];

    L_LOG("L pass_driver_open if=%u alt=%u nEP=%u cls=%02x/%02x/%02x",
          itf_desc->bInterfaceNumber, itf_desc->bAlternateSetting,
          itf_desc->bNumEndpoints, itf_desc->bInterfaceClass,
          itf_desc->bInterfaceSubClass, itf_desc->bInterfaceProtocol);

    const uint8_t  target_iface = itf_desc->bInterfaceNumber;
    uint16_t       drv_len      = 0;
    const uint8_t *p            = base;
    bool           in_alt_zero  = false;

    while (drv_len + 2 <= max_len) {
        uint8_t blen = p[0], btyp = p[1];
        if (!blen) break;

        if (btyp == TUSB_DESC_INTERFACE) {
            const tusb_desc_interface_t *i = (const tusb_desc_interface_t *)p;
            if (i->bInterfaceNumber != target_iface) break;
            in_alt_zero = (i->bAlternateSetting == 0);
        } else if (btyp == TUSB_DESC_ENDPOINT && in_alt_zero) {
            const tusb_desc_endpoint_t *ep = (const tusb_desc_endpoint_t *)p;
            if (open_ep_count < PASS_MAX_EPS) {
                if (usbd_edpt_open(rhport, ep)) {
                    OpenEp *slot  = &open_eps[open_ep_count++];
                    slot->addr    = ep->bEndpointAddress;
                    slot->mps     = ep->wMaxPacketSize;
                    slot->is_in   = (ep->bEndpointAddress & TUSB_DIR_IN_MASK) != 0;
                    slot->in_flight = false;
                    slot->in_pending_len = 0;
                    slot->tpl_have     = false;
                    slot->tpl_len      = 0;
                    slot->last_real_us = 0;
                    if (!slot->is_in) {
                        uint16_t n = slot->mps > sizeof(slot->rx_buf) ?
                                     sizeof(slot->rx_buf) : slot->mps;
                        usbd_edpt_xfer(rhport, slot->addr, slot->rx_buf, n);
                    }
                } else {
                    ESP_LOGW(TAG, "usbd_edpt_open failed ep=0x%02x", ep->bEndpointAddress);
                }
            }
        }
        drv_len += blen;
        p       += blen;
    }

    pass_mounted = true;
    return drv_len;
}

// Forward every non-standard / class / vendor control transfer to Right.
//   IN: at SETUP ship setup bytes only; Right fetches asynchronously and
//       pass_usb_control_in_complete calls tud_control_xfer once the data
//       arrives.
//   OUT, wLength > 0: at SETUP, tud_control_xfer to receive data into
//       ctl_out_buf; at DATA ship setup+data combined as one
//       FRAME_CTRL_SETUP of (8 + wLength) bytes.
//   OUT, wLength == 0: ship setup, wait for STATUS.
static bool pass_driver_control_xfer(uint8_t rhport, uint8_t stage,
                                      tusb_control_request_t const *request) {
    if (stage == CONTROL_STAGE_SETUP) {
        if (!request) return false;
        if (ctl_pending) {
            // Stale-pending guard. ctl_pending is normally cleared by the
            // STATUS frame from Right. If that frame is lost (CRC drop in
            // ipc.c), ctl_pending stays true forever and every subsequent
            // control BUSY-rejects → EP0 STALL → host stops polling INs.
            // Looks identical to a hard freeze. If the pending xfer is
            // >2 s old it's never coming back: force-clear and accept.
            int64_t age_us = ctl_setup_us ? (esp_timer_get_time() - ctl_setup_us) : 0;
            if (ctl_setup_us != 0 && age_us > 2000000) {
                char m[112];
                int n = snprintf(m, sizeof(m),
                    "[L] ctl STALE clear prev_seq=%u age=%lldms new bmReq=%02x bReq=%02x\n",
                    ctl_pending_seq, (long long)(age_us / 1000),
                    request->bmRequestType, request->bRequest);
                if (n > 0) km_uart_write(m, n);
                ctl_pending = false;
            } else {
                char m[96];
                int n = snprintf(m, sizeof(m),
                    "[L] ctl BUSY bmReq=%02x bReq=%02x wVal=%04x wIdx=%04x\n",
                    request->bmRequestType, request->bRequest,
                    request->wValue, request->wIndex);
                if (n > 0) km_uart_write(m, n);
                return false;
            }
        }

        const bool     is_in   = (request->bmRequestType & 0x80) != 0;
        const uint16_t wLength = request->wLength;

        ctl_pending_request = *request;
        ctl_pending_seq     = ++ctl_seq_counter;
        ctl_rhport          = rhport;
        ctl_pending         = true;
        ctl_setup_us        = esp_timer_get_time();

        char m[128];
        int n = snprintf(m, sizeof(m),
            "[L] ctl FWD seq=%u bmReq=%02x bReq=%02x wVal=%04x wIdx=%04x wLen=%u dir=%s\n",
            ctl_pending_seq, request->bmRequestType, request->bRequest,
            request->wValue, request->wIndex, wLength, is_in ? "IN" : "OUT");
        if (n > 0) km_uart_write(m, n);

        if (!is_in && wLength > 0) {
            if (wLength > CTL_OUT_BUF_SZ) {
                n = snprintf(m, sizeof(m),
                    "[L] ctl OUT wLen=%u > buf=%u; STALL\n",
                    (unsigned)wLength, (unsigned)CTL_OUT_BUF_SZ);
                if (n > 0) km_uart_write(m, n);
                ctl_pending = false;
                return false;
            }
            return tud_control_xfer(rhport, request, ctl_out_buf, wLength);
        }

        uint8_t setup[8];
        memcpy(setup, request, 8);
        ipc_send(FRAME_CTRL_SETUP, 0, ctl_pending_seq, setup, 8);
        return true;
    }

    if (stage == CONTROL_STAGE_DATA) {
        if (ctl_pending) {
            const bool is_in = (ctl_pending_request.bmRequestType & 0x80) != 0;
            const uint16_t dlen = ctl_pending_request.wLength;
            if (!is_in && dlen > 0 && dlen <= CTL_OUT_BUF_SZ) {
                uint8_t payload[8 + CTL_OUT_BUF_SZ];
                memcpy(payload, &ctl_pending_request, 8);
                memcpy(payload + 8, ctl_out_buf, dlen);
                ipc_send(FRAME_CTRL_SETUP, 0, ctl_pending_seq,
                         payload, (uint16_t)(8 + dlen));

                char m[96];
                int n = snprintf(m, sizeof(m),
                    "[L] ctl FWD_OUT_DATA seq=%u dlen=%u\n",
                    ctl_pending_seq, (unsigned)dlen);
                if (n > 0) km_uart_write(m, n);
            }
        }
        return true;
    }

    if (stage == CONTROL_STAGE_ACK)  {
        char m[32];
        int n = snprintf(m, sizeof(m), "[L] ctl ACK seq=%u\n", ctl_pending_seq);
        if (n > 0) km_uart_write(m, n);
        ctl_pending = false;
        return true;
    }
    return false;
}

static bool pass_driver_xfer_cb(uint8_t rhport, uint8_t ep_addr,
                                 xfer_result_t result, uint32_t xferred) {
    OpenEp *slot = find_ep(ep_addr);
    if (!slot) return false;

    if (slot->is_in) {
        // Previous IN done. If a coalesced-latest report is waiting,
        // promote it to rx_buf and submit — keeps the freshest snapshot
        // in flight; drops are replaced by overwrites.
        uint16_t promote_len = 0;
        portENTER_CRITICAL(&io_lock);
        if (slot->in_pending_len > 0) {
            promote_len = slot->in_pending_len;
            memcpy(slot->rx_buf, slot->in_pending_buf, promote_len);
            slot->in_pending_len = 0;
            // in_flight stays true — a new xfer is about to go out.
        } else {
            slot->in_flight = false;
        }
        portEXIT_CRITICAL(&io_lock);
        if (promote_len) {
            if (!usbd_edpt_xfer(rhport, ep_addr, slot->rx_buf, promote_len)) {
                portENTER_CRITICAL(&io_lock);
                slot->in_flight = false;
                portEXIT_CRITICAL(&io_lock);
            }
        }
    } else {
        if (result == XFER_RESULT_SUCCESS && xferred) {
            ipc_send(FRAME_EP_OUT, ep_addr, 0, slot->rx_buf, (uint16_t)xferred);
            int64_t t_us = esp_timer_get_time();
            char m[160];
            int ln = snprintf(m, sizeof(m), "[L] EP_OUT t=%lld ep=%02x len=%u hex=",
                              (long long)t_us, ep_addr, (unsigned)xferred);
            uint16_t show = xferred > 20 ? 20 : xferred;
            for (uint16_t i = 0; i < show && ln < (int)sizeof(m) - 3; ++i) {
                ln += snprintf(m + ln, sizeof(m) - ln, "%02x", slot->rx_buf[i]);
            }
            if (ln < (int)sizeof(m) - 1) m[ln++] = '\n';
            km_uart_write(m, ln);
        }
        uint16_t n = slot->mps > sizeof(slot->rx_buf) ?
                     sizeof(slot->rx_buf) : slot->mps;
        usbd_edpt_xfer(rhport, ep_addr, slot->rx_buf, n);
    }
    return true;
}

// TinyUSB weak-overrides this to pull our driver into the class-driver
// table at tusb_init() time.
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    static const usbd_class_driver_t pass_driver = {
#if CFG_TUSB_DEBUG >= CFG_TUD_LOG_LEVEL
        .name             = "Pass",
#endif
        .init             = pass_driver_init,
        .reset            = pass_driver_reset,
        .open             = pass_driver_open,
        .control_xfer_cb  = pass_driver_control_xfer,
        .xfer_cb          = pass_driver_xfer_cb,
        .sof              = NULL,
    };
    *driver_count = 1;
    return &pass_driver;
}

static bool submit_in_core(uint8_t ep_addr, const uint8_t *data, uint16_t len, bool is_synth) {
    OpenEp *slot = find_ep(ep_addr);
    if (!slot || !slot->is_in || !pass_mounted || !tud_ready()) return false;
    // First 200 IN packets per EP — real only, so the synth stream
    // (high rate) can't flood the log.
    static uint16_t in_dbg_count[PASS_MAX_EPS] = {0};
    uint8_t slot_ix = (uint8_t)(slot - open_eps);
    if (!is_synth && slot_ix < PASS_MAX_EPS && in_dbg_count[slot_ix] < 200) {
        in_dbg_count[slot_ix]++;
        int64_t t_us = esp_timer_get_time();
        char m[160];
        int ln = snprintf(m, sizeof(m), "[L] EP_IN t=%lld ep=%02x len=%u hex=",
                          (long long)t_us, ep_addr, (unsigned)len);
        uint16_t show = len > 20 ? 20 : len;
        for (uint16_t i = 0; i < show && ln < (int)sizeof(m) - 3; ++i) {
            ln += snprintf(m + ln, sizeof(m) - ln, "%02x", data[i]);
        }
        if (ln < (int)sizeof(m) - 1) m[ln++] = '\n';
        km_uart_write(m, ln);
    }
    // Only cache reports km_apply would actually modify — heartbeats /
    // announces have no stick bytes and would just replay "nothing
    // happened" every 4 ms while injection is active.
    //   0x20  GIP input  (Scuf / PowerA / Elite / GameSir / Xbox One)
    //   0x00  XInput-style 20B  (buf[1]==0x14)
    //   0x01  DS5 HID   (len >= 64)
    if (!is_synth && slot_ix < PASS_MAX_EPS && len >= 2) {
        bool is_input =
            (data[0] == 0x20) ||
            (data[0] == 0x00 && len >= 14 && data[1] == 0x14) ||
            (data[0] == 0x01 && len >= 64);
        if (is_input) {
            uint16_t tlen = len > sizeof(slot->tpl_buf) ? (uint16_t)sizeof(slot->tpl_buf) : len;
            memcpy(slot->tpl_buf, data, tlen);
            slot->tpl_len      = tlen;
            slot->tpl_have     = true;
            slot->last_real_us = esp_timer_get_time();
        }
    }
#if LAT_DIAG
    int64_t t0 = esp_timer_get_time();
#endif
    // km_apply on a stack scratch so we never touch shared buffers under
    // contention without the lock. 64 B = FS max-packet-size cap.
    uint8_t scratch[64];
    if (len > sizeof(scratch)) len = sizeof(scratch);
    memcpy(scratch, data, len);
    km_apply(ep_addr, scratch, len);

    bool start_xfer = false;
    bool coalesced  = false;
    portENTER_CRITICAL(&io_lock);
    if (!slot->in_flight) {
        memcpy(slot->rx_buf, scratch, len);
        slot->in_flight = true;
        start_xfer = true;
    } else {
        if (slot->in_pending_len > 0) coalesced = true;
        memcpy(slot->in_pending_buf, scratch, len);
        slot->in_pending_len = len;
    }
    portEXIT_CRITICAL(&io_lock);

    bool ok = true;
    if (start_xfer) {
        if (!usbd_edpt_claim(0, ep_addr)) {
            portENTER_CRITICAL(&io_lock);
            slot->in_flight = false;
            portEXIT_CRITICAL(&io_lock);
            ok = false;
        } else {
            ok = usbd_edpt_xfer(0, ep_addr, slot->rx_buf, len);
            if (!ok) {
                usbd_edpt_release(0, ep_addr);
                portENTER_CRITICAL(&io_lock);
                slot->in_flight = false;
                portEXIT_CRITICAL(&io_lock);
            }
        }
    }
#if LAT_DIAG
    int64_t t1 = esp_timer_get_time();
    int64_t dt_sub = t1 - t0;
    if (coalesced) lat.drops++;
    if (dt_sub < lat.min_submit_us) lat.min_submit_us = dt_sub;
    if (dt_sub > lat.max_submit_us) lat.max_submit_us = dt_sub;
    lat.sum_submit_us += dt_sub;
    if (lat.last_entry_us != 0) {
        int64_t gap = t0 - lat.last_entry_us;
        if (gap < lat.min_gap_us) lat.min_gap_us = gap;
        if (gap > lat.max_gap_us) lat.max_gap_us = gap;
        lat.sum_gap_us += gap;
    }
    lat.last_entry_us = t0;
    lat.count++;
    if (lat.count >= LAT_DIAG_EMIT_N) {
        int64_t avg_sub = lat.sum_submit_us / lat.count;
        int64_t avg_gap = lat.count > 1 ? lat.sum_gap_us / (lat.count - 1) : 0;
        int64_t min_sub = lat.min_submit_us == INT64_MAX ? 0 : lat.min_submit_us;
        int64_t min_gap = lat.min_gap_us    == INT64_MAX ? 0 : lat.min_gap_us;
        L_LOG("LAT n=%u sub=%lld/%lld/%lldus gap=%lld/%lld/%lldus coal=%u",
              (unsigned)lat.count,
              (long long)min_sub, (long long)avg_sub, (long long)lat.max_submit_us,
              (long long)min_gap, (long long)avg_gap, (long long)lat.max_gap_us,
              (unsigned)lat.drops);
        lat_reset();
        lat.last_entry_us = 0;
    }
#endif
    return ok;
}

bool pass_usb_submit_in(uint8_t ep_addr, const uint8_t *data, uint16_t len) {
    return submit_in_core(ep_addr, data, len, /*is_synth=*/false);
}

// Synthesis timer. If any km injection is active AND it's been > SYNTH_GAP_MS
// since the last real report on a given IN EP, re-submit the cached template
// (km_apply overlays the injected stick + buttons). GIP controllers only
// emit on change — without this a brief injection window falls between
// real reports and Windows never sees the override.
//
// Falling-edge tracking emits one final synth frame on the
// active→inactive transition so a clean release lands on the host (e.g.
// RT=full from a km.click pulse, then a real RT=0 carried by the template).
#define SYNTH_TICK_MS   4
#define SYNTH_GAP_MS    3
static esp_timer_handle_t synth_timer_handle = NULL;

static void synth_cb(void *arg) {
    (void)arg;
    static bool prev_active = false;
    if (!pass_mounted || !tud_ready()) {
        prev_active = false;
        return;
    }
    bool now_active   = km_has_active_injection();
    bool falling_edge = prev_active && !now_active;
    prev_active       = now_active;
    if (!now_active && !falling_edge) return;
    int64_t now = esp_timer_get_time();
    for (uint8_t i = 0; i < open_ep_count; ++i) {
        OpenEp *slot = &open_eps[i];
        if (!slot->is_in || !slot->tpl_have) continue;
        if ((now - slot->last_real_us) < ((int64_t)SYNTH_GAP_MS * 1000)) continue;
        uint8_t scratch[64];
        uint16_t len = slot->tpl_len > sizeof(scratch) ? (uint16_t)sizeof(scratch) : slot->tpl_len;
        memcpy(scratch, slot->tpl_buf, len);
        submit_in_core(slot->addr, scratch, len, /*is_synth=*/true);
    }
}

static void synth_timer_start_once(void) {
    if (synth_timer_handle) return;
    const esp_timer_create_args_t args = {
        .callback        = &synth_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "km_synth",
    };
    if (esp_timer_create(&args, &synth_timer_handle) == ESP_OK) {
        esp_timer_start_periodic(synth_timer_handle, SYNTH_TICK_MS * 1000);
        L_LOG("synth timer started tick=%dms gap=%dms", SYNTH_TICK_MS, SYNTH_GAP_MS);
    }
}

void pass_usb_stop_synth(void) {
    if (synth_timer_handle) {
        esp_timer_stop(synth_timer_handle);
        // Handle kept — resume_synth restarts on replug.
    }
    portENTER_CRITICAL(&io_lock);
    for (uint8_t i = 0; i < open_ep_count; ++i) {
        open_eps[i].tpl_have       = false;
        open_eps[i].tpl_len        = 0;
        open_eps[i].in_flight      = false;
        open_eps[i].in_pending_len = 0;
    }
    portEXIT_CRITICAL(&io_lock);
}

// Idle-time housekeep. Called from km_inject.c after ≥10 s of no km
// activity. Drops the synth template cache on every IN EP so a stale
// tpl_have can't keep replaying old reports if a DEVICE_GONE was missed.
// Real emissions repopulate tpl_have on the next IN report.
//
// Intentionally does NOT touch ctl_pending — that's serialized by TinyUSB
// on its own task; its 2 s stale-clear lives in the SETUP branch.
// Intentionally does NOT touch in_flight / in_pending_len — clearing
// mid-flight would desync the EP.
void pass_usb_idle_housekeep(void) {
    int cleared = 0;
    portENTER_CRITICAL(&io_lock);
    for (uint8_t i = 0; i < open_ep_count; ++i) {
        if (open_eps[i].is_in && open_eps[i].tpl_have) {
            open_eps[i].tpl_have = false;
            open_eps[i].tpl_len  = 0;
            cleared++;
        }
    }
    portEXIT_CRITICAL(&io_lock);
    if (cleared) {
        char m[64];
        int n = snprintf(m, sizeof(m),
            "[L] idle housekeep cleared=%d tpl\n", cleared);
        if (n > 0) km_uart_write(m, n);
    }
}

void pass_usb_resume_synth(void) {
    if (synth_timer_handle) {
        esp_timer_start_periodic(synth_timer_handle, SYNTH_TICK_MS * 1000);
    }
}

// Hot-disconnect: drop D+ so Windows sees the device removed (joy.cpl
// unmounts), without re-running tinyusb_driver_install on reconnect.
// esp_tinyusb 1.4.x's uninstall leaves the USB DWC peripheral in a state
// where the next install() returns ESP_ERR_INVALID_STATE (0x103);
// disconnect/connect avoids the PHY churn entirely.
void pass_usb_disconnect(void) {
    pass_usb_stop_synth();
    ctl_pending = false;
    bool ok = tud_disconnect();
    L_LOG("L pass_usb_disconnect tud_ok=%d", (int)ok);
}

// Hot-reconnect. Caller must run tinyusb_set_descriptors with the
// updated config first so esp_tinyusb's pointers/string-count refresh.
void pass_usb_reconnect(void) {
    pass_usb_resume_synth();
    bool ok = tud_connect();
    L_LOG("L pass_usb_reconnect tud_ok=%d", (int)ok);
}

bool pass_usb_control_in_complete(uint16_t seq, const uint8_t *data, uint16_t len) {
    char m[160];
    int n = snprintf(m, sizeof(m), "[L] ctl IN_DATA seq=%u len=%u match=%u hex=",
                     seq, len, (unsigned)(ctl_pending && seq == ctl_pending_seq));
    if (n > 0 && n < (int)sizeof(m)) {
        uint16_t show = len > 16 ? 16 : len;
        for (uint16_t i = 0; i < show && n < (int)sizeof(m) - 3; ++i) {
            n += snprintf(m + n, sizeof(m) - n, "%02x", data[i]);
        }
        if (n < (int)sizeof(m) - 1) m[n++] = '\n';
        km_uart_write(m, n);
    }
    if (!ctl_pending || seq != ctl_pending_seq) return false;
    return tud_control_xfer(ctl_rhport, &ctl_pending_request, (void *)data, len);
}

void pass_usb_control_status(uint16_t seq, uint8_t status) {
    int64_t elapsed_us = (ctl_setup_us && seq == ctl_pending_seq)
                         ? (esp_timer_get_time() - ctl_setup_us) : -1;
    char m[96];
    const char *stxt = (status == XFER_OK) ? "OK"
                     : (status == XFER_STALL) ? "STALL"
                     : (status == XFER_TIMEOUT) ? "TIMEOUT" : "ERROR";
    int n = snprintf(m, sizeof(m), "[L] ctl STATUS seq=%u st=%s(%u) dt=%lldus\n",
                     seq, stxt, status, (long long)elapsed_us);
    if (n > 0) km_uart_write(m, n);
    if (!ctl_pending || seq != ctl_pending_seq) return;
    ctl_pending = false;
    if (status != XFER_OK) {
        usbd_edpt_stall(ctl_rhport, 0x00);
    }
}
