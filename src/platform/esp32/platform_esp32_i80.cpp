// Parallel WS2812 output over the ESP-IDF esp_lcd i80 bus — the peripheral half of MultiPinLedDriver
// (src/light/drivers/MultiPinLedDriver.h), which does all the domain work: applies Correction and
// 3-slot-encodes every light into the DMA frame buffer (ParallelSlots.h). This file owns only the
// peripheral — the esp_lcd i80 bus, the IO device, the DMA-capable frame buffer, transmit + wait,
// and the loopback test's TX side. No domain logic here.
//
// Design: the whole frame is pre-encoded into ONE buffer and sent as ONE gapless GDMA stream
// (tx_color with lcd_cmd = -1 → pure data phase). Once started, no CPU work remains until the done
// callback — there is no refill deadline for WiFi to miss, the deliberate difference from the
// ISR-refilled rings in the hpwit/FastLED I2S lineage this design studied.
//
// **One seam, two silicon backends.** IDF's esp_lcd component exposes ONE public i80 API
// (esp_lcd_new_i80_bus / esp_lcd_panel_io_tx_color / esp_lcd_i80_alloc_draw_buffer) and routes it,
// via its own CMake, to whichever peripheral the chip has: **LCD_CAM** on the S3/P4
// (esp_lcd_panel_io_i80.c) or the **I2S peripheral in i80/LCD mode** on the classic ESP32
// (esp_lcd_panel_io_i2s.c). Both do WHOLE-FRAME chained DMA with WR/DC + 8/16 bus width, so this
// file's body is 100% generic i80 and serves both — the single MultiPinLedDriver runs on all three.
//
// Gated on SOC_LCD_I80_SUPPORTED (true on classic + S3 + P4) with inert stubs otherwise. This is the
// BROAD macro on purpose (not the narrower SOC_LCDCAM_I80_LCD_SUPPORTED), precisely so the classic
// I2S backend compiles here too. (An earlier note warned against the broad macro — that was before
// an i2s-backed driver existed, so compiling this onto classic init'd a bus with no consumer; now
// MultiPinLedDriver is that consumer on every i80 chip.)

#include "platform/platform.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

// SOC_LCD_I80_SUPPORTED: the generic esp_lcd i80 API, backed by LCD_CAM on the S3/P4 and by
// the I2S peripheral on the classic ESP32 — both selected by IDF's own CMake. The body below
// is backend-agnostic. (esp_lcd i80 headers exist wherever SOC_LCD_I80_SUPPORTED is set.)
#if SOC_LCD_I80_SUPPORTED

#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i80.h"
#include "esp_log.h"
#include "esp_timer.h"   // esp_timer_get_time — ISR-safe wire-time stamp
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cstring>
#include <functional>  // the transmit callback passed to the shared frame loopback
#include <new>      // std::nothrow
#if !SOC_LCDCAM_I80_LCD_SUPPORTED
#include "esp_private/i2s_platform.h"   // the I2S0 occupancy probe (the same API esp_lcd's I2S backend uses)
#endif

namespace mm::platform {

// Defined in platform_esp32_rmt.cpp — the plain-GPIO continuity pre-check the
// RMT loopback uses; the wire question is identical here.
namespace detail { bool loopbackJumperOk(uint8_t txGpio, uint8_t rxGpio); }

namespace {

static const char* I80_TAG = "mm_i80";

// The lcd_cmd passed to esp_lcd_panel_io_tx_color. LCD_CAM (S3/P4) takes -1 = "no command phase"
// (pure data). The classic I2S backend has an UNCONDITIONAL command phase (see the lcd_cmd_bits note
// in createState), so it needs a real 8-bit command: 0 is a benign no-op byte that completes the
// backend's command poll before the WS2812 data frame.
#if SOC_LCDCAM_I80_LCD_SUPPORTED
constexpr int kI80Cmd = -1;
#else
constexpr int kI80Cmd = 0;
#endif

// 3 slots per WS2812 bit (the ParallelSlots.h contract): 2.67 MHz pclk = 375 ns
// slots, "0" = 1 slot HIGH, "1" = 2 slots HIGH. 375 ns and not the lineage's
// usual 416 ns: newer WS2812B revisions spec T0H max ≈ 380 ns, and on a
// direct 3.3 V data line (no level shifter) a longer "0" pulse gets misread
// as "1" — the strip washes out white. 375 ns sits inside every revision's
// window; the 160 MHz LCD clock divides to it exactly (/60).
constexpr uint32_t kPclkHz = 2'666'666;

// Pixel clock with a 74HCT595 expander fitted. **The constraint that matters is the WS2812 SLOT
// DURATION, not the elegance of the divider — getting that backwards is what broke the first bench
// run.** A '595 shifts each slot out over `clockMultiplier` bus words, so:
//
//     slot = clockMultiplier / pclk        and the slot IS the "0" pulse (T0H).
//
// WS2812B spec: **T0H 200-380 ns** (newer revisions cap ~380 — see lessons.md #5, the max-white
// flicker), T1H 580-1000 ns. The direct path picks 2.67 MHz for a 375 ns slot, right at that edge.
//
// The first attempt here was 20 MHz "because it divides exactly": 8 × 50 ns = **400 ns**, which is
// OVER the 380 ns T0H max. Bench result (2026-07-14, board B): the strands rendered scattered
// MAX-BRIGHTNESS pixels and washed-out white — zeros being read as ones, exactly lesson #5. An exact
// divider that produces an out-of-spec waveform is worthless; the divider is a means, not the goal.
//
// 26.67 MHz (prescale 3 off the 80 MHz bus resolution — still an exact divide, so esp_lcd's
// silent round-down cannot bite):
//     slot = 8 / 26.67 MHz  = 300 ns   T0H 300 (spec 200-380 ✓)  T1H 600 (spec 580-1000 ✓)
//
// (This band is also why a ×16 cascade is NOT offered: it needs a 42-55 MHz pclk to stay in spec, and
// NO exact divide of 80 MHz lands there — the divides are 80/40/20/16/10. Two pins beat two cascaded
// registers on every axis anyway; see ParallelSlots.h kPinExpanderOutputs.)
//
// **Do NOT "fix" flicker by lowering this clock** — that was the original note's advice and it is
// exactly backwards: a lower pclk makes the slot LONGER, pushing T0H further past 380 ns. If the
// waveform ever needs adjusting, adjust it *up* (prescale 2 -> 40 MHz -> 200 ns slot, the bottom of
// the T0H window) and check the buffer can carry the rate. The fitted SN74HCT245N (t_pd ~10-18 ns)
// has real but adequate margin at 37.5 ns per shift cycle.
constexpr uint32_t kShiftPclkHz = 26'666'666;   // prescale 3 of 80 MHz -> 300 ns WS2812 slots


// Two DMA frame buffers for the async deferred-wait double-buffer: the driver encodes frame N+1
// into buf[1-active] while the GDMA clocks frame N out of buf[active]. buf[1] is null when the
// second allocation didn't fit (single-buffer mode — the driver runs the old wait-every-frame path
// on buf[0]). Each buffer has its OWN done-semaphore so a wait targets the right transfer.
//
// With trans_queue_depth 2 both buffers can be in flight at once (N draining while N+1 is queued),
// and the empty esp_lcd done-event carries no per-transfer token — but the i80 GDMA completes
// transfers in ENQUEUE order, so a 2-slot FIFO of enqueued buffer indices, popped in the callback,
// routes each done-signal to the buffer that actually finished. (This is the textbook completion-
// FIFO for an in-order DMA queue; the depth is 2 because at most two transfers — one per buffer —
// are ever outstanding.)
struct I80State {
    esp_lcd_i80_bus_handle_t bus = nullptr;
    esp_lcd_panel_io_handle_t io = nullptr;
    SemaphoreHandle_t done[2] = {nullptr, nullptr};
    uint8_t* buf[2] = {nullptr, nullptr};
    size_t cap = 0;   // shared per-buffer capacity (both buffers equal)
    // In-order completion FIFO of enqueued buffer indices (0/1). enqueue at head under a critical
    // section around tx_color; the ISR pops at tail. Only ever 0..2 entries (one per buffer).
    volatile uint8_t fifo[2] = {0, 0};
    volatile uint8_t fifoHead = 0;   // next write slot (mod 2)
    volatile uint8_t fifoTail = 0;   // next read slot (mod 2)
    // Wire-time KPI: the start timestamp of the oldest in-flight transfer (paired with the FIFO,
    // so it tracks the transfer the next done-callback completes), and the last measured duration.
    volatile int64_t txStartUs[2] = {0, 0};
    volatile uint32_t lastTransmitUs = 0;
};

// Done-callback: the GDMA stream finished — pop the oldest enqueued buffer index (transfers
// complete in enqueue order), record the wire duration (now − that transfer's start), and release
// THAT buffer's waiter. esp_timer_get_time() is ISR-safe (it reads a hardware counter).
//
// IRAM_ATTR: the DMA-done ISR calls this from interrupt context, so keeping it out of flash is the
// correct, defensive choice (matches the sibling Parlio done-cb) — everything it touches is IRAM-safe
// (esp_timer_get_time reads a hardware counter, xSemaphoreGiveFromISR, plain member stores). The
// classic I2S backend's extra quirk (an unconditional command phase that busy-waits) is handled by
// the kI80Cmd / lcd_cmd_bits split above, not here.
bool IRAM_ATTR i80DoneCb(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* user) {
    auto* st = static_cast<I80State*>(user);
    const uint8_t slot = st->fifoTail;
    const uint8_t b = st->fifo[slot] & 1u;
    const int64_t now = esp_timer_get_time();
    st->lastTransmitUs = static_cast<uint32_t>(now - st->txStartUs[slot]);
    st->fifoTail = (st->fifoTail + 1u) & 1u;
    // In-order queue: a buffer already queued behind this one starts the instant this transfer ends —
    // stamp its true start here, since the transmit call deliberately skipped stamping it (the wire was
    // busy). Without this the second buffer's frameTime would include this one's remaining wire time.
    if (st->fifoTail != st->fifoHead) st->txStartUs[st->fifoTail] = now;
    BaseType_t high = pdFALSE;
    xSemaphoreGiveFromISR(st->done[b], &high);
    return high == pdTRUE;
}

void destroyState(I80State* st) {
    if (!st) return;
    if (st->io) esp_lcd_panel_io_del(st->io);
    if (st->bus) esp_lcd_del_i80_bus(st->bus);
    for (auto* b : st->buf) if (b) heap_caps_free(b);
    for (auto* s : st->done) if (s) vSemaphoreDelete(s);
    delete st;
}

// One bus + IO device + DMA buffer(s). `wantSecond` allocates the async double-buffer's second
// frame buffer (best-effort — null if it won't fit); false allocates buffer 0 only. Shared by the
// runtime init and the loopback's private bus (which passes false — one transfer).
I80State* createState(const uint16_t* dataPins, uint8_t laneCount,
                      uint16_t wrGpio, uint16_t dcGpio, size_t bufferBytes, bool wantSecond,
                      uint8_t clockMultiplier = 1) {
    auto* st = new (std::nothrow) I80State();
    if (!st) return nullptr;

    esp_lcd_i80_bus_config_t busCfg = {};
    busCfg.dc_gpio_num = static_cast<gpio_num_t>(dcGpio);
    busCfg.wr_gpio_num = static_cast<gpio_num_t>(wrGpio);
    busCfg.clk_src = LCD_CLK_SRC_DEFAULT;
    // Bus width is power-of-two only (8 or 16), derived from the lane count: ≤8 → 8,
    // 9..16 → 16. The domain driver already guarantees exactly 8 or 16 real data pins.
    const size_t busWidth = laneCount <= 8 ? 8 : 16;
    busCfg.bus_width = busWidth;
    // The i80 layer REJECTS an NC data pin (unlike Parlio), so every data line up to
    // bus_width must be a real GPIO. A board that drives fewer than bus_width lanes
    // parks the unused ones on the WR "ghost pin" (hpwit's trick) — WR toggles on it
    // harmlessly, and the domain driver clears those lanes' activeMask so they idle.
    for (size_t i = 0; i < ESP_LCD_I80_BUS_WIDTH_MAX; i++) {
        busCfg.data_gpio_nums[i] = (i < busWidth) ? static_cast<gpio_num_t>(wrGpio)
                                                  : GPIO_NUM_NC;
    }
    for (uint8_t i = 0; i < laneCount && i < busWidth; i++) {
        busCfg.data_gpio_nums[i] = static_cast<gpio_num_t>(dataPins[i]);
    }
    busCfg.max_transfer_bytes = bufferBytes;
    busCfg.dma_burst_size = 64;
    if (esp_lcd_new_i80_bus(&busCfg, &st->bus) != ESP_OK) {
        destroyState(st);
        return nullptr;
    }

    // One done-semaphore per buffer (buf[1]'s is created only when its buffer allocates).
    st->done[0] = xSemaphoreCreateBinary();
    if (!st->done[0]) {
        destroyState(st);
        return nullptr;
    }

    esp_lcd_panel_io_i80_config_t ioCfg = {};
    ioCfg.cs_gpio_num = GPIO_NUM_NC;    // no chip select — we own the bus
    // A '595 expander shifts each WS2812 slot out over 8 bus words, so the bus must clock 8× faster
    // to keep the slot inside the WS2812 bit window. kShiftPclkHz is that rate — and it is one of the
    // EXACT divides of the 80 MHz bus resolution, because esp_lcd silently rounds an inexact pclk DOWN
    // into a wrong waveform rather than reporting it (see kShiftPclkHz).
    ioCfg.pclk_hz = (clockMultiplier > 1) ? kShiftPclkHz : kPclkHz;
    // Queue depth 2 so the deferred-wait tick can hand the IO device the next frame's transfer while
    // the current one is still draining (the double-buffer). The driver still waits before REUSING a
    // buffer, so at most two transfers (one per buffer) are ever outstanding.
    //
    // **Shift mode drops to depth 1, and it is NOT a fix — it is a narrowing.** With the expander's
    // ×8 frame, EVERY GDMA mount fails (`lli full need=38 avail=3`) whatever the depth, and the
    // failure is silent: tx_color returns ESP_OK because the enqueue succeeded — the mount fails later
    // in the ISR — so the driver waits out a 1 s timeout per frame and the strands hold stale data.
    // Depth 1 was tried because `avail` counts only what the PREVIOUS transfer released
    // (gdma_link_mount_buffers walks from index 0 and stops at the first DMA-owned descriptor,
    // gdma_link.c:163-174, `check_owner`), so serialising the mounts *should* have made the pool
    // available. It did not. Neither did doubling the pool (76 descriptors verified on-device against
    // a need of 38). Direct mode is unaffected: 0 errors on every bench board.
    //
    // Six hypotheses are ruled out by measurement; the open one is IDF's own note that without
    // descriptor write-back "descriptor is always owned by DMA after being used". Full account +
    // what NOT to re-try: docs/history/shift-register-driver-analysis.md § 7.5.
    ioCfg.trans_queue_depth = (clockMultiplier > 1) ? 1 : 2;
    ioCfg.on_color_trans_done = i80DoneCb;
    ioCfg.user_ctx = st;
    // LCD_CAM backend (S3/P4): no command phase — tx_color(cmd = -1) skips it, so cmd_bits = 0.
    // Classic I2S backend: its tx_color has an UNCONDITIONAL command phase that busy-waits for the
    // command DMA's TX_EOF. With cmd_bits = 0 that command buffer is zero-length, the DMA never
    // asserts EOF, and the poll loop hangs → interrupt-watchdog reset (WROVER bench, 2026-07-13). So
    // give the i2s backend a real 8-bit command phase: one benign byte clocks out (completing the
    // poll) before the WS2812 data frame. It rides in the inter-frame idle-LOW gap, within the strand
    // latch window, so the strands ignore it. (See the classic-only cmd value at each tx_color call.)
#if SOC_LCDCAM_I80_LCD_SUPPORTED
    ioCfg.lcd_cmd_bits = 0;
#else
    ioCfg.lcd_cmd_bits = 8;
#endif
    ioCfg.lcd_param_bits = 0;
    ioCfg.flags.pclk_idle_low = 1;      // WR rests LOW like the data lines
    if (esp_lcd_new_panel_io_i80(st->bus, &ioCfg, &st->io) != ESP_OK) {
        destroyState(st);
        return nullptr;
    }

    // DMA-capable draw buffer. On the LCD_CAM backend (S3/P4) the i80 GDMA can burst straight from
    // PSRAM (access_ext_mem), so allocate PSRAM-first to keep the large 16-bit frame off scarce
    // internal DRAM. But the classic ESP32's I2S-i80 backend CANNOT DMA from PSRAM —
    // esp_lcd_i80_alloc_draw_buffer rejects MALLOC_CAP_SPIRAM there with "external memory is not
    // supported" (bench-confirmed on the WROVER, 2026-07-13). So the PSRAM attempt is compiled in
    // ONLY on the LCD_CAM chips; the classic backend goes straight to internal DMA RAM. IDF also does
    // its own DMA/ext-mem cache alignment for whichever region it lands in. Zeroed so the trailing
    // latch pad holds the lines LOW.
    // **Shift mode prefers INTERNAL RAM; direct mode prefers PSRAM.**
    //
    // Direct mode: PSRAM first, as above — the frame is large, the 2.67 MHz pixel clock is easy to
    // sustain from PSRAM, and keeping it out of scarce internal DRAM is the right trade.
    //
    // Shift mode: internal first. **Measured, not theorised** (board B, S3, 2026-07-14): with the
    // expander's 8× frame in PSRAM the strands flicker wildly and the GDMA logs thousands of mount
    // failures; with the frame in internal RAM the same strands render smooth and flicker-free. The
    // mechanism is NOT yet understood — the obvious explanations (PSRAM bandwidth, descriptor pool
    // size, FIFO underrun) have each been tested and refuted, and the P4 runs the identical frame from
    // PSRAM perfectly. So this is an empirical preference, honestly labelled as one.
    //
    // It is a STOPGAP, not the destination: internal DMA RAM is ~110 KB, which caps a shift display at
    // roughly 2,000 lights — less than direct mode already reaches. PSRAM is what makes large displays
    // possible and the driver must get back to it. PSRAM therefore remains the fallback (a frame too
    // big for internal RAM still runs, just poorly, rather than refusing to drive at all).
    //
    // Full account + what has already been ruled out: docs/history/shift-register-driver-analysis.md § 7.5.
#if SOC_LCDCAM_I80_LCD_SUPPORTED
    // Only the LCD_CAM backend can reach PSRAM at all, so the preference only exists here. (The
    // classic ESP32's i80 is the I2S peripheral, whose DMA cannot address PSRAM — it takes the
    // internal-only path below unconditionally, and never asks the question.)
    const bool pinExpanderMode = clockMultiplier > 1;
    if (!pinExpanderMode)
        st->buf[0] = static_cast<uint8_t*>(esp_lcd_i80_alloc_draw_buffer(
            st->io, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM));
    if (!st->buf[0])
#endif
        st->buf[0] = static_cast<uint8_t*>(esp_lcd_i80_alloc_draw_buffer(
            st->io, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
#if SOC_LCDCAM_I80_LCD_SUPPORTED
    // Shift mode wanted internal RAM and could not have it (a frame too big): take PSRAM rather than
    // refuse to drive. Expect the flicker until the frame fits or the real fix lands.
    if (!st->buf[0] && pinExpanderMode) {
        ESP_LOGW(I80_TAG, "shift frame (%u B) does not fit internal DMA RAM — using PSRAM; "
                          "expect stalled transfers. Reduce lights per strand.", (unsigned)bufferBytes);
        st->buf[0] = static_cast<uint8_t*>(esp_lcd_i80_alloc_draw_buffer(
            st->io, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM));
    }
#endif
    if (!st->buf[0]) {
        destroyState(st);
        return nullptr;
    }
    std::memset(st->buf[0], 0, bufferBytes);
    st->cap = bufferBytes;

    // Second buffer for the async double-buffer — ONLY when asked (wantSecond). Off by default, so
    // the common path allocates exactly one frame buffer and pays no async memory. When wanted, same
    // PSRAM-first-else-internal allocate-and-degrade: if it fits, arm double-buffer mode (buf[1] + its
    // semaphore); if it doesn't (memory-tight board), leave buf[1] null and run single-buffer. The
    // double-buffer is never *required* (allocate-and-degrade, ADR 0002).
    if (wantSecond) {
        st->done[1] = xSemaphoreCreateBinary();
        if (st->done[1]) {
            // buf[1] follows buf[0]'s allocation POLICY exactly (same region preference per mode), so the
            // async back buffer never lands where the front one refused to: PSRAM-first only in DIRECT mode
            // (LCD_CAM reaches PSRAM fine at the 2.67 MHz clock); in SHIFT mode the LCD_CAM GDMA can't
            // sustain a PSRAM read at the 26.67 MHz expander clock, so internal-first — a PSRAM buf[1] there
            // would stall exactly like a PSRAM buf[0] does. PSRAM doesn't touch the scarce internal DMA heap,
            // so no reserve check on that branch.
#if SOC_LCDCAM_I80_LCD_SUPPORTED
            if (!pinExpanderMode)
                st->buf[1] = static_cast<uint8_t*>(esp_lcd_i80_alloc_draw_buffer(
                    st->io, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM));
#endif
            // Internal fallback ONLY if it leaves HEAP_RESERVE intact — the second buffer is a nice-to-
            // have (allocate-and-degrade), so it must never eat the WiFi/HTTP reserve. Without this guard
            // a default-ON async board whose frame lands internal (e.g. a big moving-head frame, or the
            // P4 where PSRAM DMA degrades) would drop internal RAM below the reserve → WiFi/HTTP alloc
            // failures. Degrade to single-buffer instead. This is also the FIRST attempt in shift mode.
            if (!st->buf[1]
                && heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
                       >= bufferBytes + HEAP_RESERVE) {
                st->buf[1] = static_cast<uint8_t*>(esp_lcd_i80_alloc_draw_buffer(
                    st->io, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            }
            if (st->buf[1]) {
                std::memset(st->buf[1], 0, bufferBytes);
            } else {
                // No room for the second buffer — drop its unused semaphore, stay single-buffer.
                vSemaphoreDelete(st->done[1]);
                st->done[1] = nullptr;
            }
        }
    }
    return st;
}

} // namespace

namespace {
const char* s_lastError = nullptr;   // set by i80Ws2812Init on a refusal it can name; cold path
// Whether that refusal was CONTENTION (another module holds the instance) rather than a config
// fault. Only contention can clear on its own, so only it earns a retry: keying the retry on
// `s_lastError` alone made a bad pin set rebuild the bus once a second forever.
bool s_refusedForContention = false;
}

const char* i80Ws2812LastError() { return s_lastError; }

bool i80Ws2812SharedBusFree() {
#if !SOC_LCDCAM_I80_LCD_SUPPORTED
    // Only meaningful after THIS backend was refused for contention: otherwise a driver that failed
    // for its own reasons (bad pins, no memory) would rebuild once a second forever. Probing is the
    // acquire/release pair esp_lcd itself uses, which is why it is safe to call repeatedly.
    if (!s_refusedForContention) return false;
    if (i2s_platform_acquire_occupation(I2S_CTLR_HP, 1, "mm_i80_probe") != ESP_OK) return false;
    i2s_platform_release_occupation(I2S_CTLR_HP, 1);
    return true;
#else
    return false;   // LCD_CAM: the i80 bus shares nothing
#endif
}

bool i80Ws2812Init(I80Ws2812Handle& h, const uint16_t* dataPinsIn, uint8_t laneCount,
                   uint16_t wrGpio, uint16_t dcGpio, size_t bufferBytes,
                   bool wantSecondBuffer, uint8_t clockMultiplier) {
    s_lastError = nullptr;
    s_refusedForContention = false;
    if (!dataPinsIn || laneCount == 0 || bufferBytes == 0 || clockMultiplier == 0) return false;
    if (laneCount > ESP_LCD_I80_BUS_WIDTH_MAX) return false;
    uint16_t dataPins[ESP_LCD_I80_BUS_WIDTH_MAX];
    std::memcpy(dataPins, dataPinsIn, laneCount * sizeof(uint16_t));
#if !SOC_LCDCAM_I80_LCD_SUPPORTED
    // The classic ESP32's i80 IS an I2S peripheral, and the chip has two instances. **The LED bus
    // always takes instance 1, and everything else gets 0.** The split is fixed in silicon rather
    // than chosen: instance 0 is the only one with the PDM-to-PCM and PCM-to-PDM converters
    // (I2S_LL_PDM2PCM_SUPPORTED_PORT_MASK is 1U << 0), so a PDM microphone can ONLY live there,
    // while nothing in the chip requires instance 1 for anything. The LED bus is therefore the one
    // consumer that can always yield, and hard-coding it to 1 leaves 0 free for every audio source
    // (PDM, standard I2S, line-in ADC, codec), with no ordering or boot race to reason about.
    //
    // esp_lcd picks the first FREE instance rather than taking one by number, so 1 is claimed by
    // holding 0 across bus creation and releasing it straight after.
    if (i2s_platform_acquire_occupation(I2S_CTLR_HP, 1, "mm_i80_probe") != ESP_OK) {
        s_lastError = "I2S1 is in use: on the classic ESP32 the parallel LED bus is an I2S "
                      "peripheral, and it drives from instance 1";
        s_refusedForContention = true;
        return false;
    }
    i2s_platform_release_occupation(I2S_CTLR_HP, 1);
    const bool parked = i2s_platform_acquire_occupation(I2S_CTLR_HP, 0, "mm_i80_park") == ESP_OK;
    struct ParkGuard {   // release instance 0 on EVERY path out of this function
        bool on;
        ~ParkGuard() { if (on) i2s_platform_release_occupation(I2S_CTLR_HP, 0); }
    } parkGuard{parked};
    // "No pin" for WR (kBusPinUnset), and for the spare bus lanes the driver parks on it: the
    // peripheral insists on a GPIO number, a WS2812 strand reads none of these lines, and an
    // input-only pad has no output driver. So WR is routed to SENSOR_VP (36), bonded on every
    // classic package, where the matrix drives nothing and no usable GPIO is spent.
    //
    // DC gets NO such treatment, and the asymmetry is load-bearing. WR reaches the pad through the
    // GPIO matrix (esp_rom_gpio_connect_out_signal), which is inert on a pad that cannot drive. DC
    // is software-toggled: esp_lcd calls gpio_set_level on it for every transfer, and on an
    // input-only pad that call fails, logs "GPIO output gpio_num error", and the log call itself
    // aborts from that context. So an unset DC is refused by the driver before it reaches here.
    constexpr uint16_t kWrSink = 36;
    if (wrGpio == kBusPinUnset) wrGpio = kWrSink;
    for (uint8_t i = 0; i < laneCount; i++) if (dataPins[i] == kBusPinUnset) dataPins[i] = wrGpio;
    if (dcGpio == kBusPinUnset) {
        s_lastError = "dcPin (DC) needs a real GPIO: the i80 bus toggles it in software every frame";
        return false;
    }
#else
    // LCD_CAM (S3 / P4 / S31): both control lines need a real pad. The driver refuses an unset one
    // before calling here; this is the backstop that keeps an invalid number away from the ROM.
    if (wrGpio == kBusPinUnset || dcGpio == kBusPinUnset) {
        s_lastError = "clockPin (WR) and dcPin (DC) need a real GPIO on this chip";
        return false;
    }
#endif
    // The shift-register expander needs the LCD_CAM backend: its ×8 frame (~145 KB) only fits in
    // PSRAM, and the classic ESP32's I2S-i80 backend cannot DMA from PSRAM at all (see buf[0]) —
    // it would fall back to internal RAM and fail the allocation, or worse, half-fit. Refuse it
    // here so the driver reports a clean init failure instead of a mystery, and so the frame-size
    // pre-check below isn't the thing that (accidentally) enforces a hardware rule.
#if !SOC_LCDCAM_I80_LCD_SUPPORTED
    if (clockMultiplier > 1) return false;
#endif
    // Pre-check that the draw buffer can land SOMEWHERE before building the bus. createState
    // allocates PSRAM-first (the 16-lane frame is meant to live there), then falls back to internal.
    // So init is fine when EITHER the internal DMA heap has room past HEAP_RESERVE, OR PSRAM can hold
    // it. Gating only on internal would reject a board whose frame fits solely in PSRAM — exactly the
    // PSRAM-first case (an S3/16-lane grid). The HEAP_RESERVE floor only guards INTERNAL RAM (the
    // WiFi/HTTP reserve); a PSRAM buffer doesn't touch it. Degrade (return false → driver idles with a
    // status) when neither region fits.
    const bool fitsInternal =
        heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
            >= bufferBytes + HEAP_RESERVE;
    // PSRAM capacity is queried with MALLOC_CAP_SPIRAM ALONE, not `| MALLOC_CAP_DMA`. The combined
    // query asks the heap for a region tagged with BOTH caps and no registered heap is tagged both,
    // so it returns 0 — even on an S3 whose LCD_CAM GDMA reaches PSRAM perfectly well. (The *alloc*
    // does pass both caps, which is correct and is what IDF itself does in
    // esp_lcd_i80_alloc_draw_buffer; it's only the free-SIZE query that must not be over-constrained.)
    // Querying the combined caps made this pre-check reject every PSRAM frame, silently forcing the
    // internal heap and capping the driver at the ~80 KB largest internal block.
    // Guarded by the SAME capability check createState uses: only the LCD_CAM backends (S3/P4) can
    // DMA a frame out of PSRAM. On the classic ESP32 (i80 = I2S) PSRAM is unreachable by the DMA, so
    // counting it here would let an over-large frame pass this pre-check and then die inside bus
    // creation with a misleading "check pins / memory" — the pre-check must fail first, and say so.
#if SOC_LCDCAM_I80_LCD_SUPPORTED
    const bool fitsPsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= bufferBytes;
#else
    const bool fitsPsram = false;
#endif
    if (!fitsInternal && !fitsPsram) return false;
    I80State* st = createState(dataPins, laneCount, wrGpio, dcGpio, bufferBytes, wantSecondBuffer,
                               clockMultiplier);
    if (!st) return false;
    h.impl = st;
    return true;
}

uint8_t* i80Ws2812Buffer(const I80Ws2812Handle& h, uint8_t buffer) {
    auto* st = static_cast<I80State*>(h.impl);
    return (st && buffer < 2) ? st->buf[buffer] : nullptr;
}

size_t i80Ws2812BufferCapacity(const I80Ws2812Handle& h) {
    auto* st = static_cast<I80State*>(h.impl);
    return st ? st->cap : 0;
}

bool i80Ws2812Transmit(I80Ws2812Handle& h, uint8_t buffer, size_t bytes) {
    auto* st = static_cast<I80State*>(h.impl);
    if (!st || !st->io || buffer >= 2 || !st->buf[buffer] || bytes == 0 || bytes > st->cap) return false;
    // Push this buffer onto the completion FIFO BEFORE enqueuing the transfer, so a fast done-callback
    // (which pops the FIFO) can never fire before its slot is populated. The push is the only thing the
    // ISR races, and it is a couple of plain stores into slot `fifoHead` — the ISR only ever reads slot
    // `fifoTail`, and head != tail while a transfer is in flight (the driver waits before reusing a
    // buffer), so the push and the pop touch DIFFERENT slots. That slot-disjointness is what makes the
    // push safe without a lock. **Do NOT wrap tx_color in a critical section:** esp_lcd_panel_io_tx_color
    // blocks on an internal FreeRTOS queue (xQueueSend/Receive), and calling a blocking RTOS API from
    // inside taskENTER_CRITICAL panics (spinlock held + interrupts off). Push, then enqueue outside any CS.
    const uint8_t slot = st->fifoHead;
    const bool wireIdle = (st->fifoHead == st->fifoTail);   // nothing in flight → this one starts NOW
    st->fifo[slot] = buffer;
    // Stamp the wire-time start only when the wire is IDLE (enqueue == hardware-start). If a transfer is
    // already clocking out, this one does not begin until that one ends, so stamping here would fold the
    // predecessor's remaining wire time into this buffer's measured duration. The done-callback stamps it
    // instead, at the moment the hardware actually starts it.
    if (wireIdle) st->txStartUs[slot] = esp_timer_get_time();
    st->fifoHead = (st->fifoHead + 1u) & 1u;
    // lcd_cmd = -1: no command phase — the transfer is one continuous GDMA data stream, gapless
    // at the pclk rate.
    const esp_err_t err = esp_lcd_panel_io_tx_color(st->io, kI80Cmd, st->buf[buffer], bytes);
    if (err != ESP_OK) {
        // Enqueue failed — unwind the FIFO push so the ISR count stays balanced. Safe: a failed
        // enqueue produced no transfer, so no done-callback will pop this slot.
        st->fifoHead = (st->fifoHead + 1u) & 1u;
    }
    return err == ESP_OK;
}

bool i80Ws2812Wait(I80Ws2812Handle& h, uint8_t buffer, uint32_t timeoutMs) {
    auto* st = static_cast<I80State*>(h.impl);
    if (!st || buffer >= 2 || !st->done[buffer]) return true;   // nothing to wait on = not in flight
    // Report whether the transfer actually completed. On a timeout the DMA may still be reading this
    // buffer, so the caller must keep it marked in-flight rather than re-encoding into it — handing a
    // live DMA a half-rewritten buffer is exactly the frame corruption the timeout is meant to avoid.
    return xSemaphoreTake(st->done[buffer], pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

uint32_t i80Ws2812LastTransmitUs(const I80Ws2812Handle& h) {
    auto* st = static_cast<I80State*>(h.impl);
    return st ? st->lastTransmitUs : 0;
}

void i80Ws2812Deinit(I80Ws2812Handle& h) {
    auto* st = static_cast<I80State*>(h.impl);
    if (!st) return;
    destroyState(st);
    h.impl = nullptr;
}

// ---------------------------------------------------------------------------
// Loopback self-test: a private FULL-width bus (the i80 layer rejects NC data
// pins, so the driver's complete pin set is rebuilt) transmits the CALLER'S
// real frame — full size, real DMA descriptor chain, real latch pad — back to
// back like the render loop, while an RMT RX channel (rmtWs2812RxCapture with
// the DMA backend — transmitter-agnostic, reused from the RMT rig) captures
// the WHOLE frame off the jumpered rxGpio and verifies every bit. A short
// synthetic burst would miss exactly the failures a real frame hits
// (descriptor boundaries, sustained-rate stalls), so the test sends the
// genuine article.
// ---------------------------------------------------------------------------

// The capture + bit-verify half is shared with the Parlio loopback in
// detail::captureAndVerifyFrame (platform_esp32_rmt.cpp); only the i80 transmit
// differs. Declared here so this TU can call it (same pattern as loopbackJumperOk).
namespace detail {
void captureAndVerifyFrame(uint16_t rxGpio, size_t frameBytes, size_t dataBytes,
                           uint8_t rowBits, uint32_t pclkHz, bool pinExpanderMode, const char* tag,
                           const std::function<void()>& transmitOnce,
                           RmtLoopbackResult& r, bool rideMode = false,
                           uint32_t* rxSymbols = nullptr);
// Pre-allocate the capture buffer captureAndVerifyFrame needs (one contiguous DMA-capable internal
// block, sized from dataBytes) so a caller can grab it BEFORE its own allocations fragment the heap
// (largest-first allocation order). Pass the result as `rxSymbols`; ownership transfers to
// captureAndVerifyFrame regardless of outcome. nullptr on alloc failure is fine to pass through —
// the helper then retries the alloc itself and reports the failure.
uint32_t* allocLoopbackCapture(size_t dataBytes);
}

RmtLoopbackResult i80Ws2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                    uint16_t wrGpio, uint16_t dcGpio, uint16_t rxGpio,
                                    const uint8_t* frame, size_t frameBytes,
                                    size_t dataBytes, uint8_t rowBits,
                                    uint8_t clockMultiplier) {
    RmtLoopbackResult r;
    r.sent[0] = 0xA5; r.sent[1] = 0x00; r.sent[2] = 0xFF;  // pattern in every row
    if (!dataPins || laneCount == 0 || !frame || frameBytes == 0
        || dataBytes < 3 || dataBytes > frameBytes || rowBits < 8
        || clockMultiplier == 0) return r;
    const uint16_t txGpio = dataPins[0];   // lane 0 carries the pattern
    const bool pinExpanderMode = clockMultiplier > 1;

    if (pinExpanderMode) {
        // SKIP the continuity pre-check. It drives txGpio and expects rxGpio to follow directly,
        // which is true of a bare jumper but FALSE through a 74HCT595: raising the serial input does
        // not raise an output (that takes 8 shift clocks + a latch). Running it here would report
        // "jumper not detected" on perfectly good wiring. The rx pin is fed from a '595 OUTPUT, so
        // the only proof the wire is right is the bit-verify itself — which is the stronger check
        // anyway (it validates the whole chain: encode → bus → shift → latch → output).
        r.jumperDetected = true;
    } else {
        r.jumperDetected = detail::loopbackJumperOk(static_cast<uint8_t>(txGpio),
                                                    static_cast<uint8_t>(rxGpio));
        if (!r.jumperDetected) return r;
    }

    // The continuity check above reset txGpio's GPIO matrix route; bus
    // creation re-claims it.
    I80State* st = createState(dataPins, laneCount, wrGpio, dcGpio, frameBytes,
                               /*wantSecond=*/false,    // one transfer — single buffer
                               clockMultiplier);        // shift mode → the kShiftPclkHz bus clock
    if (!st) {
        ESP_LOGE(I80_TAG, "loopback: private bus creation failed");
        return r;
    }
    std::memcpy(st->buf[0], frame, frameBytes);   // loopback uses buffer 0 only (single transfer)

    // The i80-specific transmit: ship one frame from buffer 0 and wait for its done-callback.
    // Everything else (capture, cadence, bit-verify) is the shared helper. The FIFO/semaphore
    // bookkeeping matches the runtime path: push buffer 0, enqueue, the ISR pops and gives done[0].
    auto transmitOnce = [st, frameBytes]() {
        // Loopback self-test path (not the render hot path): surface a failed
        // enqueue or a done-callback timeout instead of letting it show up only as
        // a later capture mismatch (same handling as the Parlio sibling).
        st->fifo[st->fifoHead] = 0;
        st->fifoHead = (st->fifoHead + 1u) & 1u;
        const esp_err_t err = esp_lcd_panel_io_tx_color(st->io, kI80Cmd, st->buf[0], frameBytes);
        if (err != ESP_OK) {
            st->fifoHead = (st->fifoHead + 1u) & 1u;   // unwind the push
            ESP_LOGE(I80_TAG, "loopback: tx enqueue failed (%s)", esp_err_to_name(err));
            return;
        }
        if (xSemaphoreTake(st->done[0], pdMS_TO_TICKS(1000)) != pdTRUE)
            ESP_LOGE(I80_TAG, "loopback: tx done-callback timed out");
    };
    // `pclkHz` tells the verifier the WS2812 SLOT RATE the strand sees — it derives both the pulse-
    // width threshold ("0" = one slot, "1" = two) and the expected transmit duration from it. So it
    // must describe the STRAND's waveform, not the bus.
    //
    // In shift mode the strand's slot is NOT the bus period: `clockMultiplier` bus words fill one
    // slot, so slot rate = pclk / clockMultiplier. At 26.67 MHz ÷ 8 that is 3.33 MHz → a 300 ns slot.
    //
    // Passing kPclkHz (2.67 MHz → 375 ns) in shift mode — which this did, with a comment confidently
    // asserting "the slot is still 375 ns" — makes the capture expect 15-tick pulses while the strand
    // emits 12-tick ones, and sizes the capture window for a frame 8× shorter than the real one. The
    // result is a decode that matches nothing: `bad bit 0/0`, zero bits captured, on a strand whose
    // LEDs were visibly lighting. (Bench, board B, 2026-07-14 — the giveaway was exactly that: the
    // LEDs worked, so a waveform existed; only the capture was blind to it.)
    const uint32_t slotHz = (clockMultiplier > 1) ? (kShiftPclkHz / clockMultiplier) : kPclkHz;
    detail::captureAndVerifyFrame(rxGpio, frameBytes, dataBytes, rowBits, slotHz, clockMultiplier > 1,
                                  I80_TAG, transmitOnce, r);
    destroyState(st);
    return r;
}

} // namespace mm::platform

#else  // !SOC_LCD_I80_SUPPORTED — inert stubs so a chip with no i80 (LCD_CAM or I2S) links

namespace mm::platform {

bool i80Ws2812Init(I80Ws2812Handle&, const uint16_t*, uint8_t, uint16_t, uint16_t,
                   size_t, bool, uint8_t) {
    return false;
}
const char* i80Ws2812LastError() { return nullptr; }
bool i80Ws2812SharedBusFree() { return false; }
uint8_t* i80Ws2812Buffer(const I80Ws2812Handle&, uint8_t) { return nullptr; }
size_t i80Ws2812BufferCapacity(const I80Ws2812Handle&) { return 0; }
bool i80Ws2812Transmit(I80Ws2812Handle&, uint8_t, size_t) { return false; }
bool i80Ws2812Wait(I80Ws2812Handle&, uint8_t, uint32_t) { return true; }
uint32_t i80Ws2812LastTransmitUs(const I80Ws2812Handle&) { return 0; }
void i80Ws2812Deinit(I80Ws2812Handle&) {}
RmtLoopbackResult i80Ws2812Loopback(const uint16_t*, uint8_t, uint16_t, uint16_t,
                                    uint16_t, const uint8_t*, size_t, size_t, uint8_t,
                                    uint8_t) {
    return {};
}

} // namespace mm::platform

#endif  // SOC_LCD_I80_SUPPORTED
