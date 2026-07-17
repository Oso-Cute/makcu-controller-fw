// km_inject.c — Left-side KM injection pipeline.
//
// km.move(dx,dy) sums dx/dy into a per-tick velocity accumulator. Every
// 8 ms (km_housekeep_cb on esp_timer) the accumulator is drained through
// xim_curve(): rx = clamp(C × |accum|^P, ±32767), then zeroed. Defaults
// (C=5046, P=0.40) fit XIM Matrix output (15 cm/360 @ 1200 DPI) for
// tracking while lifting mid/flick response. Mouse stops → no events →
// next drain produces zero → stick returns to neutral with zero overshoot.
// No decay, no carryover, no rate-limit.
//
// km.moveto / km.aim_mode / smooth-move (km.move with duration > 0) are
// NOT supported. km.move_auto / km.move_bezier collapse to the same
// accumulator path as km.move (duration / path args ignored).
//
// km_apply() runs on every outgoing IN report:
//   1. extract physical right stick from the real device (mouse convention,
//      +Y = down) — extract_physical_gip / _xinput negate the wire's
//      XInput +Y=up, DS5 byte already mouse-down
//   2. blend_stick (USER-PRIORITY, XIM-style asymmetric):
//        aligned signs → inject scaled by (1 − |real|/32768) headroom
//        opposing signs → inject passes at full gain (cheat can drag the
//                         stick back through the user's deflection)
//   3. write merged value back into GIP / XInput / DS4 / DS5 report bytes
//      (apply_gip / _xinput negate ry on write, apply_ds4/_ds5 write raw;
//       DS4 vs DS5 auto-detected per detect_ds4, override with km.ds4)
//
// Upstream km-sender software owns sensitivity, ballistic curves, and
// pacing — firmware is strictly an input combiner.

#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

extern int km_uart_write(const void *data, size_t len);
extern int km_uart_write_raw(const void *data, size_t len);  // never gated by COM3_LOG

// Idle-time cleanup of stale synth template cache, called from
// km_housekeep_cb after KM_IDLE_HOUSEKEEP_MS of no km activity. Defined
// in pass_usb_device.c. Lives in km_housekeep_cb (not the KM_RING-gated
// drain task) so it stays active in quiet builds where KM_RING=0.
extern void pass_usb_idle_housekeep(void);

// After this long with no km command, drop stale synth tpl_have on every
// IN EP and zero the click latch (defense against the click_release_ms
// signed-compare wrap at ~24.85 d uptime). Long enough to survive any
// in-game pause; short enough that a real wedge clears before the user
// notices.
#define KM_IDLE_HOUSEKEEP_MS  10000

// Build-time gates. #ifndef-guarded so the quiet build can pass
// -DKM_DIAG=0 -DKM_RING=0 -DLAT_DIAG=0 without editing source.
#ifndef KM_DIAG
#define KM_DIAG 1
#endif
#ifndef KM_RING
#define KM_RING 1
#endif

#if KM_DIAG
// 5 Hz pipeline snapshot — counters and min/max per 200 ms window.
static _Atomic uint32_t km_cnt_move     = 0;
static _Atomic uint32_t km_cnt_moveto   = 0;
static _Atomic uint32_t km_cnt_mvauto   = 0;
static _Atomic uint32_t km_cnt_mvbez    = 0;
static _Atomic uint32_t km_cnt_click    = 0;
static _Atomic int32_t  km_sum_dx       = 0;
static _Atomic int32_t  km_sum_dy       = 0;
static _Atomic int32_t  km_rx_max       = 0;
static _Atomic int32_t  km_rx_min       = 0;
static _Atomic int32_t  km_ry_max       = 0;
static _Atomic int32_t  km_ry_min       = 0;
static uint32_t         km_diag_ticks   = 0;

static inline void km_diag_track_accum(int32_t rx, int32_t ry) {
    int32_t pmax = atomic_load(&km_rx_max); if (rx > pmax) atomic_store(&km_rx_max, rx);
    int32_t pmin = atomic_load(&km_rx_min); if (rx < pmin) atomic_store(&km_rx_min, rx);
    pmax = atomic_load(&km_ry_max); if (ry > pmax) atomic_store(&km_ry_max, ry);
    pmin = atomic_load(&km_ry_min); if (ry < pmin) atomic_store(&km_ry_min, ry);
}
#endif

#if KM_RING
// Verbose ring-buffer trace. Captures every km command and every km_apply
// output with µs timestamp. Drains to UART0 after KM_RING_IDLE_MS of no
// km activity (cheat closed) — overflow drops new entries so the end of
// a burst is preserved.
#define KM_RING_SZ        (64 * 1024)
#define KM_RING_IDLE_MS   2000

static char              km_ring[KM_RING_SZ];
static volatile size_t   km_ring_head = 0;
static volatile size_t   km_ring_tail = 0;
static volatile bool     km_ring_overflow = false;
static portMUX_TYPE      km_ring_lock = portMUX_INITIALIZER_UNLOCKED;
static _Atomic uint32_t  km_last_activity_ms = 0;

static inline void km_ring_mark_active(void) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    atomic_store(&km_last_activity_ms, now);
}

static void km_ring_write(const char *s, size_t len) {
    portENTER_CRITICAL(&km_ring_lock);
    for (size_t i = 0; i < len; ++i) {
        size_t next = (km_ring_head + 1) % KM_RING_SZ;
        if (next == km_ring_tail) {
            km_ring_overflow = true;
            break;
        }
        km_ring[km_ring_head] = s[i];
        km_ring_head = next;
    }
    portEXIT_CRITICAL(&km_ring_lock);
}

static void km_ring_printf(const char *fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
        km_ring_write(buf, (size_t)n);
    }
}

static void km_ring_drain_task(void *arg) {
    (void)arg;
    km_uart_write("[L] KM_RING drain task up (64KB ring, idle=2s)\n", 49);
    bool drained_this_cycle = false;
    uint32_t hb_counter = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        // 5 s heartbeat — reveals liveness + current activity marker.
        if (++hb_counter >= 25) {
            hb_counter = 0;
            char hb[96];
            uint32_t last_hb = atomic_load(&km_last_activity_ms);
            int n = snprintf(hb, sizeof(hb),
                "[L] KM hb t=%u last=%u idle=%u ring_bytes=%u\n",
                (unsigned)now, (unsigned)last_hb,
                (unsigned)(last_hb ? (now - last_hb) : 0),
                (unsigned)((km_ring_head + KM_RING_SZ - km_ring_tail) % KM_RING_SZ));
            if (n > 0) km_uart_write(hb, n);
        }
        uint32_t last = atomic_load(&km_last_activity_ms);
        if (last == 0) continue;
        if (now - last < KM_RING_IDLE_MS) {
            drained_this_cycle = false;
            continue;
        }
        if (drained_this_cycle) continue;

        char hdr[96];
        int hn = snprintf(hdr, sizeof(hdr),
            "\n=== KM TRACE DRAIN overflow=%u ===\n",
            (unsigned)km_ring_overflow);
        if (hn > 0) km_uart_write(hdr, hn);
        km_ring_overflow = false;

        while (1) {
            char chunk[128];
            size_t n = 0;
            portENTER_CRITICAL(&km_ring_lock);
            while (n < sizeof(chunk) && km_ring_tail != km_ring_head) {
                chunk[n++] = km_ring[km_ring_tail];
                km_ring_tail = (km_ring_tail + 1) % KM_RING_SZ;
            }
            portEXIT_CRITICAL(&km_ring_lock);
            if (n == 0) break;
            km_uart_write(chunk, n);
            vTaskDelay(pdMS_TO_TICKS(5));   // yield so TinyUSB doesn't starve
        }
        km_uart_write("=== END TRACE ===\n", 18);
        drained_this_cycle = true;
    }
}

static void km_ring_init(void) {
    xTaskCreatePinnedToCore(km_ring_drain_task, "km_drain", 4096, NULL, 2, NULL, 0);
}
#endif

// XIM-fit gain curve. rx_stick = clamp(KM_GAIN_C · |accum|^KM_GAIN_P, ±32767).
// Defaults tuned for XIM Matrix (15 cm/360 @ 1200 DPI).
//   accum=8   →  ~12k (tracking)
//   accum=80  →  ~29k (mid)
//   accum=240 →  rail (flick)
#ifndef KM_GAIN_C
#define KM_GAIN_C  5046.0f
#endif
#ifndef KM_GAIN_P
#define KM_GAIN_P  0.40f
#endif

#define KM_HOUSEKEEP_TICK_MS    8

// Safety net against esp_timer starvation: if km_housekeep_cb hasn't
// drained for this long, km_apply zeros the injection state itself so the
// stick can't stay pinned until power-cycle.
#define STALE_RELEASE_MS        500

// One-shot button-press latch (km.click). Covers ~7 frames @60 fps /
// ~15 @120 fps so UI poll windows reliably catch the pulse.
#define CLICK_HOLD_MS           120

// Analog stick idle noise floor — drift below this is clamped to zero so
// it doesn't add to the mouse injection.
#define PHYSICAL_IDLE_DEADZONE  4000

// Three tasks collide on these (km_uart_task, esp_timer housekeeping,
// TinyUSB km_apply). portMUX is lower-overhead than a mutex; sections
// are very short.
static portMUX_TYPE km_state_lock = portMUX_INITIALIZER_UNLOCKED;

// Mouse convention (+ry = down). apply_gip / apply_xinput negate Y when
// writing to XInput wire format; apply_ds5 writes raw.
static int32_t rx_injected = 0;
static int32_t ry_injected = 0;
static int32_t rx_physical = 0;
static int32_t ry_physical = 0;
static volatile int32_t g_vel_accum_x = 0;
static volatile int32_t g_vel_accum_y = 0;

static _Atomic uint16_t btn_held         = 0;   // persistent (set/release)
static _Atomic uint16_t btn_click        = 0;   // pulse (one-shot)
static _Atomic uint32_t click_release_ms = 0;

// Last km command timestamp (ms) — STALE_RELEASE_MS safety net and the
// canonical idle signal for KM_IDLE_HOUSEKEEP_MS.
static _Atomic uint32_t km_last_cmd_ms = 0;

// --- Accessibility: tremor-damp "steady" filter (RIGHT / aim stick) --------
// Off by default; every parameter tunable LIVE over the KM UART so you dial
// it to your own tremor without reflashing:
//   km.steady(1|0)   enable / disable the filter
//   km.steady_a(N)   smoothing 0..99 (EMA weight of history; higher = smoother
//                    but laggier). ~70 is a sane start.
//   km.steady_d(N)   deadzone 0..32000. Shake below N reads as center; output
//                    is rescaled above N so you keep full stick reach.
// A low-pass (exponential moving average) averages the stick across frames so
// fast shake is damped while intentional motion survives; the deadzone kills
// residual jitter around center.
static _Atomic uint32_t steady_on    = 0;
static _Atomic int32_t  steady_alpha = 70;
static _Atomic int32_t  steady_dead  = 6000;
static int32_t steady_fx = 0;   // EMA memory, right-stick X (km_state_lock)
static int32_t steady_fy = 0;   // EMA memory, right-stick Y

// --- Accessibility: constant right-stick trim (drift cancel / gentle pull) --
//   km.trim(x,y)   add a constant offset to the right stick. To cancel a
//                  resting drift, set the trim OPPOSITE the drift (the monitor
//                  reports resting mean → use its negative). Range ±32767.
// Applied before the steady filter, so a re-centered stick is then smoothed.
static _Atomic int32_t trim_x = 0;
static _Atomic int32_t trim_y = 0;

// --- Live telemetry: physical sticks + buttons out the KM UART -------------
// km.telem(1|0) toggles it. Emitted from the 8 ms housekeep timer (NOT the
// USB callback — km_uart_write_raw blocks, so it must never run there) so it
// can't stall the USB pipe. ASCII line format:
//   KMS lx=<i> ly=<i> rx=<i> ry=<i> b=<hex>\n
// Values are RAW physical (pre-deadzone) so a monitor can measure true shake.
#ifndef KM_TELEM
#define KM_TELEM 0
#endif
static _Atomic uint32_t telem_on = KM_TELEM;

// DS4-vs-DS5 layout selection (see detect_ds4 below, km.ds4 command).
#define DS4_SCORE_LATCH  50
static _Atomic int32_t  ds4_force = -1;   // -1 auto, 0 force DS5, 1 force DS4
static _Atomic uint32_t ds4_score = 0;
static _Atomic int32_t  tel_lx = 0, tel_ly = 0, tel_rx = 0, tel_ry = 0;
static _Atomic uint32_t tel_btn = 0;
// Diagnostic: does km_apply even run, and what report does it see?
static _Atomic uint32_t tel_calls = 0;   // km_apply call count
static _Atomic uint32_t tel_ep = 0;      // last ep_addr
static _Atomic uint32_t tel_b0 = 0;      // last report first byte
static _Atomic uint32_t tel_len = 0;     // last report length

// Generic button bits — protocol adapters map these to native positions.
#define BTN_A      0x0001  // face bottom (A / Cross / South)
#define BTN_B      0x0002  // face right  (B / Circle / East)
#define BTN_X      0x0004  // face left   (X / Square / West)
#define BTN_Y      0x0008  // face top    (Y / Triangle / North)
#define BTN_LB     0x0010
#define BTN_RB     0x0020
#define BTN_FIRE   0x0040  // RT analog-1 (mouse left)
#define BTN_ADS    0x0080  // LT analog-1 (mouse right)

static inline int16_t clamp_s16(int32_t v) {
    // Symmetric range: -32768 cannot be safely negated inside apply_gip /
    // apply_xinput's Y-flip (-mry overflows int16 → wraps to -32768 →
    // flips direction at saturation).
    if (v < -32767) return -32767;
    if (v >  32767) return  32767;
    return (int16_t)v;
}
static inline uint8_t s16_to_u8(int16_t v) {
    int32_t u = (int32_t)v + 32768;
    u >>= 8;
    if (u < 0) return 0;
    if (u > 255) return 255;
    return (uint8_t)u;
}

// USER-PRIORITY asymmetric XIM-style blend:
//   aligned (same sign or either zero): inject scales by (1 − |real|/32768)
//   opposing (different signs):          inject passes at FULL gain
// (a ^ b) >= 0 iff signs match (or either is zero).
static inline int16_t blend_stick(int16_t real, int16_t inject) {
    int32_t gain;
    if ((inject == 0) || (real == 0) || ((inject ^ real) >= 0)) {
        int32_t abs_real = real < 0 ? -(int32_t)real : (int32_t)real;
        gain = 32768 - abs_real;
        if (gain < 0) gain = 0;
    } else {
        gain = 32768;
    }
    int32_t sum = (int32_t)real + (((int32_t)inject * gain) >> 15);
    // Symmetric clamp — -32768 must NOT be returned; see clamp_s16.
    if (sum >  32767) return  32767;
    if (sum < -32767) return -32767;
    return (int16_t)sum;
}

static inline void compute_merged_stick(int32_t real_x, int32_t real_y,
                                        int16_t *mrx, int16_t *mry,
                                        int32_t *out_inj_x, int32_t *out_inj_y) {
    int32_t inj_x, inj_y;
    portENTER_CRITICAL(&km_state_lock);
    rx_physical = real_x;
    ry_physical = real_y;
    inj_x = rx_injected;
    inj_y = ry_injected;
    portEXIT_CRITICAL(&km_state_lock);
    // clamp_s16 (not a raw cast) on real_*: extract_physical_xinput/gip
    // apply -y to flip XInput +Y=up to internal +Y=down. When the wire's
    // y is -32768 (full physical pull-down), -y is +32768; a bare
    // narrowing cast wraps to -32768, blend_stick then sees full-up, and
    // apply_xinput's outbound -mry negation flips it to full-up on the
    // wire — pull-down on the stick produces an UP camera flick. clamp_s16
    // saturates +32768 → +32767 before the cast.
    *mrx = blend_stick(clamp_s16(real_x), clamp_s16(inj_x));
    *mry = blend_stick(clamp_s16(real_y), clamp_s16(inj_y));
    if (out_inj_x) *out_inj_x = inj_x;
    if (out_inj_y) *out_inj_y = inj_y;
}

void km_reset_injection(void) {
    portENTER_CRITICAL(&km_state_lock);
    rx_injected   = 0;
    ry_injected   = 0;
    rx_physical   = 0;
    ry_physical   = 0;
    g_vel_accum_x = 0;
    g_vel_accum_y = 0;
    steady_fx     = 0;
    steady_fy     = 0;
    portEXIT_CRITICAL(&km_state_lock);
    atomic_store(&trim_x, 0);
    atomic_store(&trim_y, 0);
    atomic_store(&btn_held, 0);
    atomic_store(&btn_click, 0);
    atomic_store(&click_release_ms, 0);
    atomic_store(&km_last_cmd_ms, 0);
}

// Active = non-zero injected stick OR any held/click button. Used by
// pass_usb_device.c's synth timer, which re-submits a cached real report
// template (modified by km_apply) when the real device is idle — GIP
// controllers only emit on change, so without synthesis a brief injection
// window lands in a poll gap and Windows never sees the override.
bool km_has_active_injection(void) {
    int32_t ix, iy;
    portENTER_CRITICAL(&km_state_lock);
    ix = rx_injected; iy = ry_injected;
    portEXIT_CRITICAL(&km_state_lock);
    if (ix || iy) return true;
    if (atomic_load(&btn_held)  != 0) return true;
    if (atomic_load(&btn_click) != 0) return true;
    return false;
}

static void applyMouseDelta(int dx, int dy) {
#if KM_DIAG
    atomic_fetch_add(&km_sum_dx, (int32_t)dx);
    atomic_fetch_add(&km_sum_dy, (int32_t)dy);
#endif
    portENTER_CRITICAL(&km_state_lock);
    g_vel_accum_x += dx;
    g_vel_accum_y += dy;
    portEXIT_CRITICAL(&km_state_lock);
}

// XIM-fit gain curve. Symmetric, saturating at int16 rail. ESP32-S3 has
// a hardware FPU so powf is cheap.
static inline int32_t xim_curve(int32_t accum) {
    if (accum == 0) return 0;
    int32_t mag = (accum < 0) ? -accum : accum;
    float r = KM_GAIN_C * powf((float)mag, KM_GAIN_P);
    int32_t ri = (r > 32767.0f) ? 32767 : (int32_t)r;
    return (accum < 0) ? -ri : ri;
}

static void km_housekeep_cb(void *arg) {
    (void)arg;
    int32_t rx, ry;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    portENTER_CRITICAL(&km_state_lock);
    int32_t ax = g_vel_accum_x;
    int32_t ay = g_vel_accum_y;
    g_vel_accum_x = 0;
    g_vel_accum_y = 0;
    rx_injected = xim_curve(ax);
    ry_injected = xim_curve(ay);
    rx = rx_injected;
    ry = ry_injected;
    portEXIT_CRITICAL(&km_state_lock);
#if KM_DIAG
    km_diag_track_accum(rx, ry);
    if (++km_diag_ticks >= 25) {   // 8 ms × 25 = 200 ms snapshot
        km_diag_ticks = 0;
        uint32_t c_mv  = atomic_exchange(&km_cnt_move,    0);
        uint32_t c_mto = atomic_exchange(&km_cnt_moveto,  0);
        uint32_t c_ma  = atomic_exchange(&km_cnt_mvauto,  0);
        uint32_t c_mb  = atomic_exchange(&km_cnt_mvbez,   0);
        uint32_t c_ck  = atomic_exchange(&km_cnt_click,   0);
        int32_t  sdx   = atomic_exchange(&km_sum_dx,      0);
        int32_t  sdy   = atomic_exchange(&km_sum_dy,      0);
        int32_t  rxmx  = atomic_exchange(&km_rx_max,      0);
        int32_t  rxmn  = atomic_exchange(&km_rx_min,      0);
        int32_t  rymx  = atomic_exchange(&km_ry_max,      0);
        int32_t  rymn  = atomic_exchange(&km_ry_min,      0);
        int32_t  px, py;
        portENTER_CRITICAL(&km_state_lock);
        px = rx_physical;
        py = ry_physical;
        portEXIT_CRITICAL(&km_state_lock);
        if (c_mv || c_mto || c_ma || c_mb || c_ck || rx || ry || rxmx || rxmn || rymx || rymn) {
            char m[208];
            int n = snprintf(m, sizeof(m),
                "[L] KM mv=%u mto=%u ma=%u mb=%u ck=%u dx=%+ld dy=%+ld "
                "ix=%ld iy=%ld px=%ld py=%ld "
                "rxmm=%ld/%ld rymm=%ld/%ld\n",
                (unsigned)c_mv, (unsigned)c_mto, (unsigned)c_ma, (unsigned)c_mb,
                (unsigned)c_ck,
                (long)sdx, (long)sdy,
                (long)rx, (long)ry, (long)px, (long)py,
                (long)rxmn, (long)rxmx, (long)rymn, (long)rymx);
            if (n > 0) km_uart_write(m, n);
        }
    }
#endif
    uint32_t rel = atomic_load(&click_release_ms);
    if (rel && (int32_t)(now_ms - rel) >= 0) {
        atomic_store(&btn_click, 0);
        atomic_store(&click_release_ms, 0);
    }

    // Idle housekeep — runs from km_housekeep_cb (always present) rather
    // than km_ring_drain_task so the freeze fix stays active even in
    // quiet builds where KM_RING=0 removes the drain task. After
    // ≥ KM_IDLE_HOUSEKEEP_MS of no km command, drop stale synth tpl_have
    // on every IN EP and zero the click latch. One-shot per idle period.
    {
        static bool housekept_idle = false;
        uint32_t last_cmd = atomic_load(&km_last_cmd_ms);
        if (last_cmd != 0) {
            uint32_t idle_ms = now_ms - last_cmd;
            if (idle_ms < KM_IDLE_HOUSEKEEP_MS) {
                housekept_idle = false;
            } else if (!housekept_idle) {
                pass_usb_idle_housekeep();
                atomic_store(&btn_click, 0);
                atomic_store(&click_release_ms, 0);
                km_uart_write("[L] idle housekeep done\n", 24);
                housekept_idle = true;
            }
        }
    }

    // --- Steady telemetry emit. Safe here: km_housekeep_cb runs in the
    // esp_timer task, not the USB callback, so a blocking km_uart_write_raw
    // can't stall the USB pipe. ~62 Hz (every 2nd 8 ms tick) — enough to
    // sample a 4–12 Hz tremor.
    if (atomic_load(&telem_on)) {
        static uint32_t tel_div = 0;
        if (++tel_div >= 2) {
            tel_div = 0;
            char m[128];
            int n = snprintf(m, sizeof(m),
                "KMS lx=%ld ly=%ld rx=%ld ry=%ld b=%04x n=%lu ep=%02x b0=%02x len=%lu\n",
                (long)atomic_load(&tel_lx), (long)atomic_load(&tel_ly),
                (long)atomic_load(&tel_rx), (long)atomic_load(&tel_ry),
                (unsigned)atomic_load(&tel_btn),
                (unsigned long)atomic_load(&tel_calls),
                (unsigned)atomic_load(&tel_ep),
                (unsigned)atomic_load(&tel_b0),
                (unsigned long)atomic_load(&tel_len));
            if (n > 0) km_uart_write_raw(m, n);
        }
    }
}

static void btn_hold(uint16_t mask, bool on) {
    uint16_t cur = atomic_load(&btn_held);
    atomic_store(&btn_held, on ? (cur | mask) : (uint16_t)(cur & ~mask));
}
static void btn_pulse(uint16_t mask) {
    atomic_store(&btn_click, mask);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    atomic_store(&click_release_ms, now + CLICK_HOLD_MS);
}
static inline uint16_t current_buttons(void) {
    return (uint16_t)(atomic_load(&btn_held) | atomic_load(&btn_click));
}

// ---------------------------------------------------------------------------
//  Text parser. Supported commands (one ASCII line each, null-terminated):
//    km.version()                — handshake, kmbox identification response
//    km.move(dx,dy)              — mouse delta (accumulator)
//    km.move_auto(dx,dy,t)       — same as move (duration ignored)
//    km.move_bezier(...)         — same as move (path ignored, dx/dy summed)
//    km.click(btn[,cnt])         — btn 0=L(RT/fire), 1=R(LT/ADS), 2=M(X-btn)
//    km.left(0|1) / km.right(0|1) / km.middle(0|1)
//    km.btnA/B/X/Y(0|1)
//    km.lb(0|1) / km.rb(0|1)
//  Unsupported (silently ignored): km.moveto, km.aim_mode, smooth-move.
// ---------------------------------------------------------------------------
static bool str_starts(const char *s, size_t len, const char *pfx) {
    size_t n = strlen(pfx);
    return len >= n && memcmp(s, pfx, n) == 0;
}

static void parse_km_text(const char *line, uint16_t len) {
    char buf[96];
    uint16_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, line, n); buf[n] = 0;

    // km.version() — mimic the original kmbox B/B+ firmware's identification
    // line. _raw bypasses the COM3_LOG gate so handshake works in quiet builds.
    if (str_starts(buf, n, "km.version(")) {
        static const char kResp[] =
            "kmbox:   1.0.0 " __DATE__ " " __TIME__ "\r\n>>> ";
        km_uart_write_raw(kResp, sizeof(kResp) - 1);
        return;
    }

    // Accessibility: steady (tremor-damp) filter + telemetry toggles.
    if (str_starts(buf, n, "km.steady_a(")) {
        int v = 70; sscanf(buf + 12, "%d", &v);
        if (v < 0) v = 0;
        if (v > 99) v = 99;
        atomic_store(&steady_alpha, v); return;
    }
    if (str_starts(buf, n, "km.steady_d(")) {
        int v = 0; sscanf(buf + 12, "%d", &v);
        if (v < 0) v = 0;
        if (v > 32000) v = 32000;
        atomic_store(&steady_dead, v); return;
    }
    if (str_starts(buf, n, "km.steady(")) {
        int v = 0; sscanf(buf + 10, "%d", &v);
        atomic_store(&steady_on, v ? 1 : 0); return;
    }
    if (str_starts(buf, n, "km.telem(")) {
        int v = 0; sscanf(buf + 9, "%d", &v);
        atomic_store(&telem_on, v ? 1 : 0); return;
    }
    if (str_starts(buf, n, "km.ds4(")) {
        int v = -1; sscanf(buf + 7, "%d", &v);
        if (v < -1) v = -1;
        if (v > 1) v = 1;
        atomic_store(&ds4_force, v);
        if (v == -1) atomic_store(&ds4_score, 0);   // restart auto-detect
        return;
    }
    if (str_starts(buf, n, "km.trim(")) {
        const char *a = strchr(buf, '('); if (!a) return; a++;
        char *e; long x = strtol(a, &e, 10); if (e == a) return;
        while (*e == ' ' || *e == ',' || *e == '\t') e++;
        long y = strtol(e, &e, 10);
        if (x < -32767) x = -32767;
        if (x > 32767) x = 32767;
        if (y < -32767) y = -32767;
        if (y > 32767) y = 32767;
        atomic_store(&trim_x, (int32_t)x);
        atomic_store(&trim_y, (int32_t)y);
        return;
    }

    if (str_starts(buf, n, "km.move(") ||
        str_starts(buf, n, "km.move_auto(") ||
        str_starts(buf, n, "km.move_bezier(")) {
#if KM_DIAG
        if (buf[7] == '(')       atomic_fetch_add(&km_cnt_move, 1);
        else if (buf[8] == 'a')  atomic_fetch_add(&km_cnt_mvauto, 1);
        else                      atomic_fetch_add(&km_cnt_mvbez, 1);
#endif
        const char *args = strchr(buf, '(');
        if (!args) return;
        args++;
        char *endp;
        long x = strtol(args, &endp, 10);
        if (endp == args) return;
        while (*endp == ' ' || *endp == ',' || *endp == '\t') endp++;
        long y = strtol(endp, &endp, 10);
#if KM_RING
        km_ring_mark_active();
        uint32_t tms = (uint32_t)(esp_timer_get_time() / 1000);
        char kind = (buf[7] == '(') ? 'M' : (buf[8] == 'a') ? 'A' : 'B';
        km_ring_printf("T%u %c %ld,%ld\n", (unsigned)tms, kind, x, y);
#endif
        applyMouseDelta((int)x, (int)y);
        return;
    }

    if (str_starts(buf, n, "km.click(")) {
        int btn = 0;
        sscanf(buf + 9, "%d", &btn);
#if KM_DIAG
        atomic_fetch_add(&km_cnt_click, 1);
#endif
        switch (btn) {
            case 0: btn_pulse(BTN_FIRE); break;
            case 1: btn_pulse(BTN_ADS);  break;
            case 2: btn_pulse(BTN_X);    break;
        }
        return;
    }
    if (str_starts(buf, n, "km.left(1"))   { btn_hold(BTN_FIRE, true);  return; }
    if (str_starts(buf, n, "km.left(0"))   { btn_hold(BTN_FIRE, false); return; }
    if (str_starts(buf, n, "km.right(1"))  { btn_hold(BTN_ADS, true);   return; }
    if (str_starts(buf, n, "km.right(0"))  { btn_hold(BTN_ADS, false);  return; }
    if (str_starts(buf, n, "km.middle(1")) { btn_hold(BTN_X, true);     return; }
    if (str_starts(buf, n, "km.middle(0")) { btn_hold(BTN_X, false);    return; }

    if (str_starts(buf, n, "km.btnA(1")) { btn_hold(BTN_A, true);  return; }
    if (str_starts(buf, n, "km.btnA(0")) { btn_hold(BTN_A, false); return; }
    if (str_starts(buf, n, "km.btnB(1")) { btn_hold(BTN_B, true);  return; }
    if (str_starts(buf, n, "km.btnB(0")) { btn_hold(BTN_B, false); return; }
    if (str_starts(buf, n, "km.btnX(1")) { btn_hold(BTN_X, true);  return; }
    if (str_starts(buf, n, "km.btnX(0")) { btn_hold(BTN_X, false); return; }
    if (str_starts(buf, n, "km.btnY(1")) { btn_hold(BTN_Y, true);  return; }
    if (str_starts(buf, n, "km.btnY(0")) { btn_hold(BTN_Y, false); return; }
    if (str_starts(buf, n, "km.lb(1"))   { btn_hold(BTN_LB, true); return; }
    if (str_starts(buf, n, "km.lb(0"))   { btn_hold(BTN_LB, false);return; }
    if (str_starts(buf, n, "km.rb(1"))   { btn_hold(BTN_RB, true); return; }
    if (str_starts(buf, n, "km.rb(0"))   { btn_hold(BTN_RB, false);return; }
}

void km_ingest_raw(const uint8_t *payload, uint16_t len) {
    if (len == 0) return;
    atomic_store(&km_last_cmd_ms, (uint32_t)(esp_timer_get_time() / 1000));
#if KM_RING
    km_ring_mark_active();
    uint32_t tms = (uint32_t)(esp_timer_get_time() / 1000);
    char hdr[16];
    int hn = snprintf(hdr, sizeof(hdr), "T%u R ", (unsigned)tms);
    if (hn > 0) km_ring_write(hdr, (size_t)hn);
    uint16_t showlen = len > 80 ? 80 : len;
    km_ring_write((const char *)payload, showlen);
    km_ring_write("\n", 1);
#endif
    if (payload[0] >= 0x20 && payload[0] < 0x7F) {
        parse_km_text((const char *)payload, len);
    }
}

// ---------------------------------------------------------------------------
//  Protocol adapters
// ---------------------------------------------------------------------------
static inline void write_le16(uint8_t *p, int16_t v) {
    p[0] = (uint8_t)((uint16_t)v & 0xFF);
    p[1] = (uint8_t)((uint16_t)v >> 8);
}

// GIP (Xbox One / Scuf / PowerA / Elite / GameSir) — bit layout from xone gamepad.c
#define GIP_BTN_A      0x0010
#define GIP_BTN_B      0x0020
#define GIP_BTN_X      0x0040
#define GIP_BTN_Y      0x0080
#define GIP_BTN_LB     0x1000   // bit 12; 0x0100/0x0200 are DPad-Up/Down
#define GIP_BTN_RB     0x2000   // bit 13  (verified on hardware, tools/inject_mapper.py)

static uint16_t map_btn_gip(uint16_t generic) {
    uint16_t r = 0;
    if (generic & BTN_A)  r |= GIP_BTN_A;
    if (generic & BTN_B)  r |= GIP_BTN_B;
    if (generic & BTN_X)  r |= GIP_BTN_X;
    if (generic & BTN_Y)  r |= GIP_BTN_Y;
    if (generic & BTN_LB) r |= GIP_BTN_LB;
    if (generic & BTN_RB) r |= GIP_BTN_RB;
    return r;
}

static void apply_gip(uint8_t *gp, uint16_t len, int16_t mrx, int16_t mry, uint16_t gen_btn) {
    if (len < 16) return;
    uint16_t btn = (uint16_t)gp[0] | ((uint16_t)gp[1] << 8);
    btn |= map_btn_gip(gen_btn);
    gp[0] = (uint8_t)(btn & 0xFF); gp[1] = (uint8_t)(btn >> 8);
    // Always write the merged sticks. Gating on (m* != 0) let raw drift in
    // the incoming buf bytes pass through whenever blend produced exactly 0
    // (common between drains) — manifested as a directional fight against
    // the controller's resting drift. Pure-idle is handled by the early
    // return in km_apply.
    write_le16(gp + 10, mrx);
    write_le16(gp + 12, (int16_t)-mry);   // GIP wire is +Y=up
    if (gen_btn & BTN_FIRE) { gp[4] = 0xFF; gp[5] = 0x03; } // RT = 1023
    if (gen_btn & BTN_ADS)  { gp[2] = 0xFF; gp[3] = 0x03; } // LT = 1023
}

// XInput (Xbox 360) 20-byte report.
#define XI_BTN_A     0x1000
#define XI_BTN_B     0x2000
#define XI_BTN_X     0x4000
#define XI_BTN_Y     0x8000
#define XI_BTN_LB    0x0100
#define XI_BTN_RB    0x0200

static uint16_t map_btn_xinput(uint16_t generic) {
    uint16_t r = 0;
    if (generic & BTN_A)  r |= XI_BTN_A;
    if (generic & BTN_B)  r |= XI_BTN_B;
    if (generic & BTN_X)  r |= XI_BTN_X;
    if (generic & BTN_Y)  r |= XI_BTN_Y;
    if (generic & BTN_LB) r |= XI_BTN_LB;
    if (generic & BTN_RB) r |= XI_BTN_RB;
    return r;
}

static void apply_xinput(uint8_t *buf, uint16_t len, int16_t mrx, int16_t mry, uint16_t gen_btn) {
    if (len < 14 || buf[0] != 0x00 || buf[1] != 0x14) return;
    uint16_t btn = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    btn |= map_btn_xinput(gen_btn);
    buf[2] = (uint8_t)(btn & 0xFF); buf[3] = (uint8_t)(btn >> 8);
    write_le16(buf + 10, mrx);
    write_le16(buf + 12, (int16_t)-mry);   // XInput wire is +Y=up
    if (gen_btn & BTN_FIRE) buf[5] = 0xFF;
    if (gen_btn & BTN_ADS)  buf[4] = 0xFF;
}

// DS4 / DS5 — Linux hid-playstation byte layout.
#define DS5_BTN_SQUARE   0x10  // byte 8 upper nibble
#define DS5_BTN_CROSS    0x20
#define DS5_BTN_CIRCLE   0x40
#define DS5_BTN_TRIANGLE 0x80
#define DS5_BTN_L1       0x01  // byte 9
#define DS5_BTN_R1       0x02

// DS4 (DualShock 4) USB report 0x01 — same stick bytes as DS5 (3/4 = RX/RY)
// but buttons live elsewhere:
//   byte 5: face buttons upper nibble, dpad HAT lower nibble (released = 8)
//   byte 6: R3 80 L3 40 Options 20 Share 10 R2 08 L2 04 R1 02 L1 01
//   byte 8/9: L2/R2 analog
#define DS4_BTN_SQUARE   0x10  // byte 5 upper nibble
#define DS4_BTN_CROSS    0x20
#define DS4_BTN_CIRCLE   0x40
#define DS4_BTN_TRIANGLE 0x80
#define DS4_BTN_L1       0x01  // byte 6
#define DS4_BTN_R1       0x02
#define DS4_BTN_L2       0x04
#define DS4_BTN_R2       0x08

// DS4-vs-DS5 detection. Both pads report b0=0x01 len=64 over USB, so the
// layouts are told apart by byte 5: on DS4 its low nibble is the dpad hat
// (idle = 8, pressed = 0..7 — never 9..15); on DS5 byte 5 is the L2 analog
// axis whose low nibble sweeps 0..15. Score rises on hat-idle frames and
// hard-resets on any impossible-for-DS4 nibble. Latches per boot; a pad
// swap without reboot may need km.ds4(0|1) manual override:
//   km.ds4(1) force DS4 layout, km.ds4(0) force DS5, km.ds4(-1) auto.
static bool detect_ds4(const uint8_t *buf) {
    int32_t f = atomic_load(&ds4_force);
    if (f >= 0) return f == 1;
    uint32_t sc = atomic_load(&ds4_score);
    uint8_t nib = buf[5] & 0x0F;
    if (nib == 0x08) {
        if (sc < 200) atomic_store(&ds4_score, sc + 1);
    } else if (nib > 0x08) {
        atomic_store(&ds4_score, 0);
        sc = 0;
    }
    return sc >= DS4_SCORE_LATCH;
}

static void apply_ds4(uint8_t *buf, uint16_t len, int16_t mrx, int16_t mry, uint16_t gen_btn) {
    if (len < 10 || buf[0] != 0x01) return;
    buf[3] = s16_to_u8(mrx);
    buf[4] = s16_to_u8(mry);
    uint8_t face = 0;
    if (gen_btn & BTN_A) face |= DS4_BTN_CROSS;
    if (gen_btn & BTN_B) face |= DS4_BTN_CIRCLE;
    if (gen_btn & BTN_X) face |= DS4_BTN_SQUARE;
    if (gen_btn & BTN_Y) face |= DS4_BTN_TRIANGLE;
    buf[5] |= face;
    uint8_t b6 = 0;
    if (gen_btn & BTN_LB)   b6 |= DS4_BTN_L1;
    if (gen_btn & BTN_RB)   b6 |= DS4_BTN_R1;
    if (gen_btn & BTN_FIRE) { b6 |= DS4_BTN_R2; buf[9] = 0xFF; }
    if (gen_btn & BTN_ADS)  { b6 |= DS4_BTN_L2; buf[8] = 0xFF; }
    buf[6] |= b6;
}

static void apply_ds5(uint8_t *buf, uint16_t len, int16_t mrx, int16_t mry, uint16_t gen_btn) {
    if (len < 11 || buf[0] != 0x01) return;
    // DS5 sticks are u8; 0x80 = center. DS5 byte Y+ = DOWN (matches mouse).
    buf[3] = s16_to_u8(mrx);
    buf[4] = s16_to_u8(mry);
    uint8_t face = 0;
    if (gen_btn & BTN_A) face |= DS5_BTN_CROSS;
    if (gen_btn & BTN_B) face |= DS5_BTN_CIRCLE;
    if (gen_btn & BTN_X) face |= DS5_BTN_SQUARE;
    if (gen_btn & BTN_Y) face |= DS5_BTN_TRIANGLE;
    buf[8] |= face;
    uint8_t shoulder = 0;
    if (gen_btn & BTN_LB) shoulder |= DS5_BTN_L1;
    if (gen_btn & BTN_RB) shoulder |= DS5_BTN_R1;
    buf[9] |= shoulder;
    if (gen_btn & BTN_FIRE) buf[6] = 0xFF;   // R2
    if (gen_btn & BTN_ADS)  buf[5] = 0xFF;   // L2
}

// Physical-stick extractors — read the real controller's right stick
// from the incoming IN report BEFORE injection is overlaid. Return in
// mouse convention (+Y = down).
static inline int32_t physical_deadzone_clean(int32_t v) {
    if (v > -PHYSICAL_IDLE_DEADZONE && v < PHYSICAL_IDLE_DEADZONE) return 0;
    return v;
}

static inline void extract_physical_ds5(const uint8_t *buf, int32_t *rx, int32_t *ry) {
    *rx = physical_deadzone_clean(((int32_t)buf[3] - 128) << 8);
    *ry = physical_deadzone_clean(((int32_t)buf[4] - 128) << 8);
}

static inline void extract_physical_gip(const uint8_t *gp, int32_t *rx, int32_t *ry) {
    int16_t r = (int16_t)((uint16_t)gp[10] | ((uint16_t)gp[11] << 8));
    int16_t y = (int16_t)((uint16_t)gp[12] | ((uint16_t)gp[13] << 8));
    *rx = physical_deadzone_clean((int32_t)r);
    *ry = physical_deadzone_clean((int32_t)-y);
}

static inline void extract_physical_xinput(const uint8_t *buf, int32_t *rx, int32_t *ry) {
    int16_t r = (int16_t)((uint16_t)buf[10] | ((uint16_t)buf[11] << 8));
    int16_t y = (int16_t)((uint16_t)buf[12] | ((uint16_t)buf[13] << 8));
    *rx = physical_deadzone_clean((int32_t)r);
    *ry = physical_deadzone_clean((int32_t)-y);
}

static inline int16_t rd_s16(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

// Per-axis scaled deadzone: |v| <= dz → 0, else rescaled to full range so
// reach isn't lost above the threshold.
static inline int32_t steady_dz(int32_t v, int32_t dz) {
    if (dz <= 0) return v;
    int32_t a = v < 0 ? -v : v;
    if (a <= dz) return 0;
    int32_t out = (int32_t)((int64_t)(a - dz) * 32767 / (32767 - dz));
    return v < 0 ? -out : out;
}

// EMA low-pass + deadzone on the right stick. Called from km_apply before the
// blend, so steady damping composes with (and precedes) any km injection.
static void steady_apply(int32_t *rx, int32_t *ry) {
    int32_t a = atomic_load(&steady_alpha);
    int32_t d = atomic_load(&steady_dead);
    portENTER_CRITICAL(&km_state_lock);
    steady_fx = (a * steady_fx + (100 - a) * (*rx)) / 100;
    steady_fy = (a * steady_fy + (100 - a) * (*ry)) / 100;
    int32_t fx = steady_fx, fy = steady_fy;
    portEXIT_CRITICAL(&km_state_lock);
    *rx = steady_dz(fx, d);
    *ry = steady_dz(fy, d);
}

// Main entry — called from pass_usb_device.c::pass_usb_submit_in for
// every outbound IN report (real or synth).
void km_apply(uint8_t ep_addr, uint8_t *buf, uint16_t len) {
    // Diagnostic probe — unconditional, before any format check, so we can see
    // whether km_apply runs at all and what report bytes arrive.
    atomic_fetch_add(&tel_calls, 1);
    atomic_store(&tel_ep, ep_addr);
    atomic_store(&tel_b0, len ? buf[0] : 0);
    atomic_store(&tel_len, len);
    // Stale-release safety net. If km_housekeep_cb hasn't fired for an
    // unusually long time, STALE_RELEASE_MS guarantees the stick zeros.
    {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t last_ms = atomic_load(&km_last_cmd_ms);
        if (last_ms && (now_ms - last_ms) > (uint32_t)STALE_RELEASE_MS) {
            portENTER_CRITICAL(&km_state_lock);
            rx_injected   = 0;
            ry_injected   = 0;
            g_vel_accum_x = 0;
            g_vel_accum_y = 0;
            portEXIT_CRITICAL(&km_state_lock);
        }
    }
#if KM_RING
    static uint32_t km_apply_calls = 0;
    if ((++km_apply_calls % 25) == 0) {
        uint16_t dump = len > 20 ? 20 : len;
        char hex[80];
        int hn = 0;
        for (uint16_t i = 0; i < dump && hn < (int)sizeof(hex) - 3; ++i) {
            hn += snprintf(hex + hn, sizeof(hex) - hn, "%02x", buf[i]);
        }
        km_ring_printf("T%u A ep=%02x len=%u bytes=%s\n",
                       (unsigned)(esp_timer_get_time() / 1000),
                       ep_addr, (unsigned)len, hex);
    }
#endif
    // GIP (Xbox One / Scuf / PowerA / Elite / GameSir) — IN EP 0x82, cmd 0x20 after 4B GIP header
    if (ep_addr == 0x82 && len >= 20 && buf[0] == 0x20) {
        const uint8_t *gp = buf + 4;
        atomic_store(&tel_lx, (int32_t)rd_s16(gp + 6));
        atomic_store(&tel_ly, (int32_t)-rd_s16(gp + 8));
        atomic_store(&tel_rx, (int32_t)rd_s16(gp + 10));
        atomic_store(&tel_ry, (int32_t)-rd_s16(gp + 12));
        atomic_store(&tel_btn, (uint32_t)((uint16_t)gp[0] | ((uint16_t)gp[1] << 8)));
        int32_t px, py, ix, iy;
        extract_physical_gip(gp, &px, &py);
        int32_t tx = atomic_load(&trim_x), ty = atomic_load(&trim_y);
        px += tx; py += ty;
        bool steady = atomic_load(&steady_on) != 0;
        bool force  = steady || tx || ty;
        if (steady) steady_apply(&px, &py);
        int16_t mrx, mry;
        compute_merged_stick(px, py, &mrx, &mry, &ix, &iy);
        uint16_t gen_btn = current_buttons();
#if KM_RING
        if (ix || iy || px || py || gen_btn || mrx || mry) {
            uint32_t tms = (uint32_t)(esp_timer_get_time() / 1000);
            km_ring_printf("T%u O %ld,%ld|%ld,%ld|%d,%d\n",
                           (unsigned)tms, (long)ix, (long)iy, (long)px, (long)py,
                           (int)mrx, (int)mry);
        }
#endif
        if (!force && mrx == 0 && mry == 0 && gen_btn == 0) return;
        apply_gip(buf + 4, len - 4, mrx, mry, gen_btn);
        return;
    }
    // XInput (Xbox 360) — 20-byte report starting 00 14
    if ((ep_addr == 0x81 || ep_addr == 0x82) &&
        len >= 14 && buf[0] == 0x00 && buf[1] == 0x14) {
        atomic_store(&tel_lx, (int32_t)rd_s16(buf + 6));
        atomic_store(&tel_ly, (int32_t)-rd_s16(buf + 8));
        atomic_store(&tel_rx, (int32_t)rd_s16(buf + 10));
        atomic_store(&tel_ry, (int32_t)-rd_s16(buf + 12));
        atomic_store(&tel_btn, (uint32_t)((uint16_t)buf[2] | ((uint16_t)buf[3] << 8)));
        int32_t px, py, ix, iy;
        extract_physical_xinput(buf, &px, &py);
        int32_t tx = atomic_load(&trim_x), ty = atomic_load(&trim_y);
        px += tx; py += ty;
        bool steady = atomic_load(&steady_on) != 0;
        bool force  = steady || tx || ty;
        if (steady) steady_apply(&px, &py);
        int16_t mrx, mry;
        compute_merged_stick(px, py, &mrx, &mry, &ix, &iy);
        uint16_t gen_btn = current_buttons();
#if KM_RING
        if (ix || iy || px || py || gen_btn || mrx || mry) {
            uint32_t tms = (uint32_t)(esp_timer_get_time() / 1000);
            km_ring_printf("T%u O %ld,%ld|%ld,%ld|%d,%d\n",
                           (unsigned)tms, (long)ix, (long)iy, (long)px, (long)py,
                           (int)mrx, (int)mry);
        }
#endif
        if (!force && mrx == 0 && mry == 0 && gen_btn == 0) return;
        apply_xinput(buf, len, mrx, mry, gen_btn);
        return;
    }
    // DS4 / DS5 — 64-byte HID report starting 0x01
    if (len >= 64 && buf[0] == 0x01) {
        bool is_ds4 = detect_ds4(buf);
        atomic_store(&tel_lx, ((int32_t)buf[1] - 128) << 8);
        atomic_store(&tel_ly, ((int32_t)buf[2] - 128) << 8);
        atomic_store(&tel_rx, ((int32_t)buf[3] - 128) << 8);
        atomic_store(&tel_ry, ((int32_t)buf[4] - 128) << 8);
        atomic_store(&tel_btn, is_ds4
            ? (uint32_t)((uint16_t)buf[5] | ((uint16_t)buf[6] << 8))
            : (uint32_t)((uint16_t)buf[8] | ((uint16_t)buf[9] << 8)));
        int32_t px, py, ix, iy;
        extract_physical_ds5(buf, &px, &py);
        int32_t tx = atomic_load(&trim_x), ty = atomic_load(&trim_y);
        px += tx; py += ty;
        bool steady = atomic_load(&steady_on) != 0;
        bool force  = steady || tx || ty;
        if (steady) steady_apply(&px, &py);
        int16_t mrx, mry;
        compute_merged_stick(px, py, &mrx, &mry, &ix, &iy);
        uint16_t gen_btn = current_buttons();
#if KM_RING
        if (ix || iy || px || py || gen_btn || mrx || mry) {
            uint32_t tms = (uint32_t)(esp_timer_get_time() / 1000);
            km_ring_printf("T%u O %ld,%ld|%ld,%ld|%d,%d\n",
                           (unsigned)tms, (long)ix, (long)iy, (long)px, (long)py,
                           (int)mrx, (int)mry);
        }
#endif
        if (!force && mrx == 0 && mry == 0 && gen_btn == 0) return;
        if (is_ds4) apply_ds4(buf, len, mrx, mry, gen_btn);
        else        apply_ds5(buf, len, mrx, mry, gen_btn);
        return;
    }
}

void km_init(void) {
    const esp_timer_create_args_t args = {
        .callback        = &km_housekeep_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "km_hk",
    };
    esp_timer_handle_t h;
    if (esp_timer_create(&args, &h) == ESP_OK) {
        esp_timer_start_periodic(h, KM_HOUSEKEEP_TICK_MS * 1000);
    }
#if KM_RING
    km_ring_init();
#endif
}
