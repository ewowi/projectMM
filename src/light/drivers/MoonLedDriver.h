#pragma once

#include "light/drivers/ParallelLedDriver.h"   // shared driver body + LedPeripheral
#include "platform/platform.h"

#include <atomic>   // the parallel-snapshot helper join flags

namespace mm {

/// A `LedPeripheral` backend: parallel WS2812B on the **LCD_CAM** peripheral (ESP32-S3 / -P4 / -S31), driven by
/// **our own DMA code** instead of ESP-IDF's `esp_lcd`. Same peripheral, pins and wire contract as the
/// `i80` backend (`I80Peripheral`); the difference is underneath, and it buys two things `esp_lcd` cannot
/// give — a frame **streamed** rather than held whole, and a **74HCT595 pin expander** (one GPIO driving
/// 8 strands).
///
/// **Streamed, not held:** the DMA refills a small pool of internal buffers behind the read head, so RAM
/// stops scaling with strand length (`useRing` picks this path; `ringRows`/`ringBufs` size it). The `i80`
/// backend is the memory-capped **reference**, this is the streaming **challenger**, and switching the
/// `peripheral` control A/Bs them on the same board with no reflash. LCD_CAM only — the classic ESP32's
/// i80 is the I2S peripheral, a separate backend. Everything above the DMA (slicing, the fused 3-slot
/// encode, the async double-buffer, loopback, the `frameTime` KPI, the dead-frame guard) lives in
/// ParallelLedDriver, so this backend is nearly all one-liners.
///
/// The deep dives are under *More info*, below the attribute/method lists:
/// @xref{why-our-own-dma-driver-below-the-read-head|why our own DMA driver},
/// @xref{the-74hct595-pin-expander-skip-unless-one-is-fitted|the 74HCT595 pin expander}, and
/// @xref{the-ringdbg-instrument-expert-mode-read-only|the ringDbg instrument legend}.
///
/// @moreinfo
///
/// ## Why our own DMA driver (below the read head)
///
/// `esp_lcd` re-arms the peripheral on every transaction: `lcd_start_transaction()` does `lcd_ll_reset()`
/// + `lcd_ll_fifo_reset()` + a hard-coded 4 µs busy-wait before each one. An LCD panel does not care — it
/// is addressed, not clocked continuously. WS2812 is one unbroken self-clocked bit stream, so a reset
/// mid-frame corrupts everything after it: a frame cannot be split across transactions at ANY chunk size,
/// which forces the whole frame into ONE transaction from ONE contiguous DMA-reachable block. That is the
/// cap this driver exists to lift.
///
/// The hardware never demanded that cap. The LCD peripheral has **no data-length register**
/// (`lcd_ll_set_phase_cycles()` sets `lcd_dout` as a boolean *enable*; IDF's own comment reads "Number of
/// data phase cycles are controlled by DMA buffer length"). It clocks exactly what the DMA feeds it and
/// stops when the chain ends. So **one `gdma_start()` over an arbitrarily long descriptor chain plus one
/// `lcd_ll_start()` is a single gapless stream across as many buffers as we like** — built on IDF's HAL +
/// GDMA link-list APIs, one level below `esp_lcd` (not raw registers; IDF's own drivers use these APIs).
/// Rationale + what we give up:
/// [ADR-0014](https://github.com/MoonModules/projectMM/blob/main/docs/adr/0014-own-i80-dma-driver-below-esp-lcd.md).
///
/// What streaming costs: the whole-frame path has no CPU deadline once armed; the ring does. Its refill
/// runs from the DMA's end-of-buffer interrupt and must beat the wire — 576 B per light is **28.8 µs/light**
/// at the default 20 MHz shift clock (21.6 at the div-3 overclock) — or the strands see a gap. That trade
/// is why `useRing` is a switch, not a constant.
///
/// ## The 74HCT595 pin expander (skip unless one is fitted)
///
/// **1 GPIO drives 8 strands**, so 6 data pins → **48 strands**. Off by default (`pinExpander`); with it
/// off, everything above is the whole story and WR never reaches a pad.
///
/// ```text
///   bus lane 0 ──────► SER    ┌── 74HCT595 ──┐ QA ──► strand 0
///                             │              │ QB ──► strand 1
///   WR (pixel clock) ──┬────► SRCLK (shift)  │ ..        ..
///                      │      │              │ QH ──► strand 7
///   bus lane N ──┬─────┼────► RCLK (latch)   └──────────────┘
///                │     │
///                │     └────► SRCLK of every other '595  (all shift in lockstep)
///                └──────────► RCLK  of every other '595  (all latch together)
/// ```
///
/// **How one lane becomes 8 strands.** A '595 is a serial-in / parallel-out shift register: 8 SRCLK edges
/// clock 8 bits down its chain, then one RCLK edge presents all 8 at once on QA..QH. So a lane's byte
/// becomes 8 strands' worth of one WS2812 bit, and the whole '595 bank presents that bit simultaneously.
///
/// **Why WR is free, and why the latch is not.** The peripheral toggles WR once per bus word in hardware —
/// exactly a shift clock — so SRCLK costs **zero DMA bytes**. But the peripheral has only ONE clock
/// output, and it is already spent on SRCLK; the latch has to come from somewhere else, and the only
/// thing left is a **data lane**, driven per word like pixel data — one lane that carries no strand.
///
/// **What bounds the strand count is the driver, not the bus:** every data pin fans out to 8, and
/// ParallelLedDriver refuses more than `kMaxStrands` (64), so **8 data pins is the ceiling** — 64 strands.
/// hpwit's board populates **6 → 48 strands**.
///
/// **The '595 is a one-slot pipeline.** Its outputs only change on the latch, so during slot N the strand
/// sees the byte latched at the START of slot N — i.e. what was shifted in during slot N−1. The encoder
/// therefore writes every value ONE SLOT EARLY (the rotation in ParallelSlots.h). Get that wrong and the
/// first LED survives while everything after it is noise.
///
/// **Why WR is the shift clock, and why there is no DC pin.** WR toggles once per bus word in
/// hardware, exactly what a '595's SRCLK needs — so the expander costs zero DMA bytes for its clock,
/// and the LATCH must ride a *data lane* instead (the peripheral gives only one clock output). DC
/// exists so an LCD panel can separate command bytes from data; a WS2812 strand has no such concept,
/// so the peripheral holds DC at a constant level and this backend never routes it to a pad. Unlike
/// `esp_lcd`, which mandates a valid DC GPIO, spending a pin on it would buy nothing.
///
/// **Why the expander needs the ring.** The fan-out costs 8 bus words per WS2812 bit, so a light is 576 B
/// and a **48 x 256** frame is ~144 KB — more contiguous internal RAM than an S3 has, and from PSRAM the
/// DMA cannot sustain the expander's shift clock (measured at 26.67 MHz). Streaming is the only route to
/// that target, which is why these two features are one story.
///
/// ## Tuning the three ring controls
///
/// A config at or under the prime-only boundary (`ceil(lights/strand ÷ ringRows) ≤ ringBufs`, ~224
/// lights/strand at the defaults) needs none of them — the whole frame is encoded before arming, the
/// pad is not even mounted, and the defaults just work. They exist for the LAPPING frontier
/// (256+/strand), where the ISR races the wire. `ringRows`/`ringBufs` are DEVICE tuning (RAM vs
/// interrupt rate vs jitter pool); `ringPadUs` is a HARDWARE fact of the strip. The `ringDbg`
/// counters — `lt` above all — are the meter every sweep reads.
///
/// **`ringRows` — lights per DMA buffer.** A control rather than a constant because the optimum is a
/// measurement, not a derivation. RAM is the ONLY axis that wants it small: at 1 the ring is ~18 KB
/// flat at ANY strand length (128, 256, 1024 all cost the same), which is what puts 48x256 in reach.
/// Three axes want it big:
///
/// | axis | why big wins |
/// |---|---|
/// | per-call overhead | the encode seam's fixed cost amortises over the rows in a buffer; at 1 it is paid per light, inside the ISR |
/// | interrupt rate | one EOF per buffer: 256 lights at 100 fps is 25.6k int/s at 1 row vs 1.6k at 16 — and a busy ISR starves the network stack |
/// | refill deadline | the drain of one buffer (`ringRows × 21.6 µs` + the pad) is the slice deadline the ISR's AVERAGE encode must beat |
///
/// The platform additionally clamps a buffer to ONE DMA descriptor node (4095 bytes — 7 rows at the
/// 16-strand row size), so values above the clamp behave as the clamp: 7 is the effective maximum
/// there, and the sweet spot measured on the bench.
///
/// **`ringPadUs` — and why its ceiling must be measured on the wall.** Sweep it up only until
/// `ringDbg`'s `lt` counter stays 0 (hpwit's _DMA_EXTENSTION, studied then written fresh). The ceiling
/// is the STRIP's latch threshold and it is per-silicon: a WS2812 that reads a LOW gap as a reset
/// LATCHES AND RESETS ITS ADDRESS, and then every slice repaints LEDs 0..ringRows-1 — the unmistakable
/// signature (each panel shows only its first few LEDs flickering through the whole frame's colors,
/// while every ring counter stays clean). The bench wall latches at ≤60 µs (30 is safe there); other
/// strips tolerate up to ~150. That hardware dependence is why it is a user control, not a constant.
///
/// **`ringAuto` — what it derives.** Per the wall-validated viability rule ((nSlices − ringBufs) ×
/// refill ≤ frame wire time): rows = the one-descriptor-node maximum (fewest slices), bufs = as deep as
/// fits (min of nSlices+1 — which IS prime-only when it fits — the platform max, and the internal-RAM
/// budget after its reserve). Deeper is strictly better: more prime coverage, less ISR load.
/// `ringPadUs` stays manual — the strip's latch threshold is a hardware fact no derivation can know.
/// The toggle is explicit, not edit-triggered: persistence restore fires the same control-change path
/// as a user edit, so an auto-flip-on-edit would trip on every boot.
///
/// ## The `ringDbg` instrument (expert-mode read-only)
///
/// A one-line raw readout of the ring's health + timing, refreshed each second — the field it backs is
/// expert-only in the UI. A whole line reads, e.g.:
///
///     sl37/bf31 co671 cr5 ab0 dn4403 ld13 lt0 tx7459 ipb1 ci31 tn-1 de0 enc536 ea369 sg5815 se11239 tw23 ts7740 tp9425 gap1469854
///
/// Each token is `<abbr><value>` (µs = microseconds, cy = CPU cycles):
///
/// | Field | Name | What it measures |
/// |---|---|---|
/// | `sn` | snapshot residency | where the ring's snapshot buffer lives: `I` internal, `P` PSRAM (the internal-first alloc fell back), `-` absent. The ISR reads it per byte, so `P` under lapping means the measured 4-8x encode cost plus cache-contention exposure. |
/// | `lv` | live-source residency | same probe for the live source buffer (what the encode reads when `ringSnapshot` is off). |
/// | `sl` | slices | frame length in DMA slices (`nSlices`). Geometry. |
/// | `bf` | buffers | ring pool depth (`ringBufs`). `bf < sl` ⇒ the *lapping* regime (the ISR refills behind the DMA); `bf ≥ sl` ⇒ *prime-only* (whole frame encoded before arm). |
/// | `co` | cache-off defers | lifetime EOF firings that skipped their refill because the flash cache was off (a flash/WiFi write). ~10/s at idle is normal and harmless. |
/// | `cr` | cache-off run | worst run of *consecutive* cache-off defers ≈ buffers the DMA drained un-refilled in one window. Healthy while `< bf`. |
/// | `ab` | stall abandons | lifetime frames the DMA self-terminated at the frontier because a cache-off window outlasted the pool — each is one *held* (not corrupted) frame. `0` ideal; climbing = held frames the eye may catch. |
/// | `dn` | done | lifetime completed frames. Should climb steadily; frozen = the ring stalled. |
/// | `ld` | last drain | oracle drain position (slice index) observed at the last EOF. Diagnostic. |
/// | `lt` | late | lifetime slices refilled *after* their drain began (stale on the wire) — the scatter meter. A clean run holds `lt` at its arm-time value; any climb is a deadline miss. |
/// | `tx` | transmit µs | last frame's measured wire time. |
/// | `ipb` | items per buffer | DMA descriptor nodes per ring buffer (`1` by design — a buffer that spans nodes breaks the terminator). |
/// | `ci` | consumed items | descriptor nodes the mount actually used. Diagnostic. |
/// | `tn` | terminator node | the fixed NULL-terminator node index (prime-only); `-1` = the lapping chain (no fixed terminator). |
/// | `de` | descriptor errors | GDMA fetched a bad descriptor = memory corruption. `0` = clean; `> 0` is a real fault. |
/// | `enc` | encode max µs | worst single ISR refill-encode (the jitter number). Compare to `gap`. |
/// | `ea` | encode avg µs | average refill-encode (the pace number — the one that must beat the slice deadline). |
/// | `sg` | seg gather cy | avg CPU cycles/row spent gathering source pixels (encode profiling). |
/// | `se` | seg emit cy | avg CPU cycles/row spent emitting bus words — the '595 transpose (encode profiling). |
/// | `tw` | tick wait µs | the ring tick's wire-wait segment (waiting the previous frame out). |
/// | `ts` | tick snapshot µs | the per-frame source-snapshot copy. |
/// | `tp` | tick prime µs | the pool-prime segment (encode the first `bf` slices + arm). |
/// | `gap` | max EOF gap µs | worst inter-interrupt gap = the per-buffer drain deadline the encode must beat. |
///
/// Reading it: a healthy lapping run has `lt`/`de`/`ab` flat at zero, `cr < bf`, `dn` climbing, and
/// `ea < gap` (the average refill beats the deadline). `co` climbing is normal (WiFi). `enc ≥ gap` means
/// the worst refill can't keep pace (a *pace* problem); `enc ≪ gap` but `lt` climbing means a cursor/logic
/// miss (a *timing* problem, not throughput).
///
/// **Prior art.** The '595 expander and the per-light streaming ring are hpwit's ideas, from
/// [I2SClocklessVirtualLedDriver](https://github.com/hpwit/I2SClocklessVirtualLedDriver) and his expander
/// board — studied hard, credited, and then written fresh against our own architecture and layout (his
/// `putdefaultones()` prefill has our own counterpart; the transpose is our own SWAR). He runs the S3
/// shift clock at ~19.2 MHz; we default to the same reliability point (20 MHz, a 28.8 µs/light budget)
/// with the `shiftOverclock` switch (26.67 MHz, 21.6 µs/light) for short-wired rigs (see the control).
class MoonI80Peripheral : public LedPeripheral {
public:
    // Data pins + loopback pin default to UNSET, for the same reason as the sibling: they are
    // user-soldered, so a hard-coded default would be a guess that could drive a pin the user
    // committed elsewhere. The orchestrator declares pins="" / loopbackRxPin=-1, so nothing is
    // needed here.

    /// WR — the pixel clock — needed **only by a 74HCT595 expander** (in direct mode nothing reads it,
    /// so the platform leaves it unrouted and the pin stays free for a strand); why it doubles as the
    /// shift clock, and why there is no `dcPin`, is under
    /// @xref{the-74hct595-pin-expander-skip-unless-one-is-fitted|More info → the expander}.
    int8_t clockPin = 10;

    /// Stream the frame as a RING of small internal DMA buffers (on, the default), or send it WHOLE from
    /// one contiguous buffer (off). Ignored in direct mode — that never rings. A prepare trigger (rebuilds
    /// the bus).
    ///
    /// Whole-frame is the simpler path and is proven under ~240 lights/strand, but it cannot reach the
    /// expander's target size: a 48x256 frame is ~144 KB contiguous internal, which does not exist, and
    /// from PSRAM the DMA cannot sustain the 26.67 MHz expander clock (measured: the identical frame runs
    /// from internal RAM and produces no output from PSRAM). So it stays as the A/B reference the ring is
    /// measured against, not as a fallback the ring degrades into.
    bool useRing = true;

    /// Derive `ringRows`/`ringBufs` from the live config (ON, the default) and write them INTO those
    /// controls so the user sees the real numbers — the DHCP pattern; switch OFF to tune by hand, and
    /// see @xref{tuning-the-three-ring-controls|More info → tuning the ring} for the derivation.
    bool ringAuto = true;

    /// Lights per DMA buffer — the ring's grain, the main RAM-vs-interrupt-rate lever, and a prepare
    /// trigger (pin-expander mode only); how to pick it is under
    /// @xref{tuning-the-three-ring-controls|More info → tuning the ring}.
    uint8_t ringRows = platform::kRingRowsDefault;

    /// How many buffers the DMA circulates. Depth buys **jitter absorption**, not capacity: the pool is a
    /// sliding window the clock-oracle refill keeps ahead of the read head, so a worst-case encode spike
    /// may borrow up to `(ringBufs − 2) × slice-duration` of lead and repay it over the next batches —
    /// which is why only the AVERAGE refill must beat the slice deadline, not the worst case. More depth
    /// = more RAM (`ringRows × rowBytes` each, internal DMA heap); 16 rides the measured knee on the
    /// bench. Pin-expander mode only; a prepare trigger.
    uint8_t ringBufs = platform::kRingBufsDefault;

    /// Inter-buffer zero-pad, µs (LAPPING only; 0 = off) — a strand-level pause that buys refill
    /// deadline at a linear frame-time cost, whose ceiling is a HARDWARE fact of the strip measured
    /// under @xref{tuning-the-three-ring-controls|More info → tuning the ring}.
    uint8_t ringPadUs = 0;

    /// Overclock the '595 shift clock: OFF (default) = 20 MHz — the reliability point, wall-verified on
    /// two rigs and the rate hpwit ships (~19.2 MHz). ON = 26.67 MHz, for short-wired rigs chasing the
    /// higher fps ceiling (151 vs 118 at 48×256) — but a 74HCT595 driving long/loaded strands can't
    /// always sample cleanly that fast: specific strands scramble (random colors on some panels) while
    /// clean ones survive, and drive strength does NOT help — turn this OFF if any panel misbehaves.
    /// A switch, not a divider: slower than 20 MHz is never useful (16 MHz pushes T0H past the WS2812's
    /// 0-vs-1 threshold — every strand goes all-white), so the only real choice is safe vs fast.
    /// Shift-mode only; a bus-rebuild trigger.
    bool shiftOverclock = false;

    /// Backing store for the read-only `ringDbg` control — the ring's raw instrument (a one-line
    /// health + timing dump), legend under
    /// @xref{the-ringdbg-instrument-expert-mode-read-only|More info → the ringDbg instrument}.
    ///
    /// Refreshed each second (`refreshBusKpi` via `tick1s`), expert-only in the UI. Public like the other
    /// controls' backing members (the control framework reads it by pointer). Grows-once static size;
    /// `sizeof` bounds the `snprintf`.
    char ringDbgStr_[176] = "—";

    // --- LedPeripheral descriptors ---

    /// LCD_CAM lanes on this chip (0 = none, and then the orchestrator's guards make the driver inert).
    /// Unlike the sibling this does NOT add `i2sLanes`: the classic ESP32's i80 is the I2S peripheral,
    /// which this backend does not implement.
    uint8_t lanesAvailable() const MM_NONBLOCKING override { return platform::lcdLanes; }
    /// The i80 bus width is 8 or 16 — a hardware fact (`lcd_ll_set_data_wire_width` takes nothing else).
    /// The PIN count stays free: configure only the pins that drive something and the orchestrator rounds
    /// the bus up around them, parking the spare lanes on WR (which the peripheral already drives, and
    /// nothing reads). Parlio's backend sets this false — its bus width IS its pin count.
    bool powerOfTwoBus() const override { return true; }
    /// The loopback cannot build a 1-lane private bus, so it rebuilds the full-width bus and carries
    /// the pattern on lane 0 — the test frame must therefore be encoded at the operational bus width.
    bool loopbackFullWidth() const override { return true; }
    /// MoonI80 programs LCD_CAM directly (its own GDMA below esp_lcd), so it claims the LcdCam block —
    /// the same block the esp_lcd-i80 backend uses on the LCD_CAM chips (S3/P4/S31), hence the two can't run together.
    LedHwBlock hwBlock() const override { return LedHwBlock::LcdCam; }
    /// Status text when the bus will not come up, so the cause is on screen rather than in a serial log.
    /// The two real causes are named: a pin the peripheral cannot route, or no DMA-reachable memory for
    /// the frame (or the ring's pool).
    const char* initFailMsg() const override { return "LCD-MM: bus init failed, check pins / memory"; }
    /// The expander needs a backend that can stream its ×8 frame; LCD_CAM is it, and this backend is
    /// LCD_CAM-only, so the answer is simply "wherever this backend runs at all".
    bool supportsPinExpander() const override { return platform::hasLcdCam; }

    /// No async double-buffer on this backend — the own-GDMA two-buffer completion handshake races and
    /// wedges the bus (see busInit). Single-buffer only; the ring is where MoonI80's speed lives.
    bool supportsDoubleBuffer() const override { return false; }

    /// The orchestrator pads spare bus lanes with this GPIO. Unrouted lanes cost nothing here, so the value is
    /// only ever *used* in shift mode — where WR is a real pad and the padding is genuinely inert.
    uint16_t clockPinForBus() const override { return static_cast<uint16_t>(clockPin); }
    /// This backend routes its own GPIOs, so a lane past the data-pin count is left unconnected and
    /// costs nothing (see configureGpio: it routes only what it is handed). No ghost pad, and no
    /// hidden claim on a GPIO the board may have given to something else.
    bool spareLanesNeedPad() const override { return false; }

    /// WR is a '595 pin here, so the control follows the expander toggle: bound always (a saved value
    /// survives a round-trip through direct mode) but shown only when a shift register can read it.
    void addBusControls(ControlList& controls) override {
        controls.addPin("clockPin", clockPin);
        controls.setHidden(controls.count() - 1, !owner_->pinExpanderMode());
    }

    /// The output path + the ring's geometry and instrument. A separate hook from addBusControls() so the
    /// orchestrator can place these AFTER latchPin — clockPin and latchPin are one '595 wiring pair and
    /// belong together in the UI, not split by a mode selector.
    void addRingControls(ControlList& controls) override {
        // The shift-clock speed switch, below the clockPin/latchPin wiring pair (the orchestrator places
        // this hook right after latchPin): OFF = 20 MHz (safe default), ON = 26.67 MHz (overclock). The
        // fix for per-strand '595 corruption is OFF; see the member doc. Shift-mode only.
        controls.addControl("shiftOverclock", shiftOverclock);
        controls.setHidden(controls.count() - 1, !owner_->pinExpanderMode());
        controls.setAdvanced(controls.count() - 1);   // a '595 clock tuning knob — expert only
        // Path selector (pin-expander mode only): the ring, or the whole frame. A distinct axis from
        // ringSnapshot — this picks the PATH, ringSnapshot tunes how the RING reads its source; they
        // compose. Whole-frame is the A/B reference: it is how the whole-frame-PSRAM-at-the-expander-clock
        // question stays testable on the same board and content.
        controls.addControl("useRing", useRing);
        controls.setHidden(controls.count() - 1, !owner_->pinExpanderMode());
        // The source-snapshot A/B knob, directly under useRing (the path it belongs to): the ring reads
        // its source through an immutable snapshot (ON, the safe default) or the live buffer (OFF, a bench
        // lever). Meaningful only when the ring is the chosen path, so hidden on wantsRing() like the
        // geometry below. `ringSnapshot` lives on the orchestrator (ParallelLedDriver); the control binds
        // it here through the mutable reference accessor.
        controls.addControl("ringSnapshot", owner_->ringSnapshotRef());
        controls.setHidden(controls.count() - 1, !wantsRing());
        // The geometry + the instrument, shown only when the RING is the chosen path — all meaningless on
        // the whole-frame one. (Gating on wantsRing() is safe: it reads plain members, pinExpanderMode +
        // useRing, not frameBytes_, so it resolves even before the source buffer is wired at boot.)
        controls.addControl("ringAuto", ringAuto);
        controls.setHidden(controls.count() - 1, !wantsRing());
        // ringRows/ringBufs/ringPadUs are DEV TUNING — ringAuto derives them for the end user (kept
        // visible above); the manual knobs are expert-only. (Once ringAuto is verified to always pick the
        // right geometry, ringAuto itself could go advanced too — for now it stays visible as the recourse.)
        controls.addControl("ringRows", ringRows, 1, 64);
        controls.setHidden(controls.count() - 1, !wantsRing());
        controls.setAdvanced(controls.count() - 1);
        controls.addControl("ringBufs", ringBufs, platform::kRingBufsMin, platform::kRingBufsMax);
        controls.setHidden(controls.count() - 1, !wantsRing());
        controls.setAdvanced(controls.count() - 1);
        controls.addControl("ringPadUs", ringPadUs, 0, platform::kRingPadMaxUs);
        controls.setHidden(controls.count() - 1, !wantsRing());
        controls.setAdvanced(controls.count() - 1);
        // Ring internals, so the streaming can be diagnosed by polling /api/state (reliable) rather than
        // scraping serial. The raw instrument — expert only; the full field-by-field legend lives on the
        // ringDbgStr_ member below (rendered into the technical page).
        controls.addReadOnly("ringDbg", ringDbgStr_, sizeof(ringDbgStr_));
        controls.setHidden(controls.count() - 1, !wantsRing());
        controls.setAdvanced(controls.count() - 1);
    }

    /// Refresh the ringDbg diagnostic string once a second (the orchestrator's tick1s chains here via
    /// refreshBusKpi). Nothing to report on the whole-frame path — the control is hidden there, so leave
    /// it untouched rather than writing a "no ring" status that only repeats what useRing says.
    void refreshBusKpi() override {
        const platform::MoonI80RingStats s = platform::moonI80Ws2812RingStats(bus_);
        if (!s.isRing) return;
        // Read-and-clear the segment sums each 1 s window — a free-running uint32 sum wraps every
        // ~20 s at the ring's accumulation rate, which silently garbles the averages (measured: se "64").
        const uint32_t segGather = ParallelLedDriver::dbgSegGatherCy;
        const uint32_t segEmit = ParallelLedDriver::dbgSegEmitCy;
        const uint32_t segRows = ParallelLedDriver::dbgSegRows;
        ParallelLedDriver::dbgSegGatherCy = 0;
        ParallelLedDriver::dbgSegEmitCy = 0;
        ParallelLedDriver::dbgSegRows = 0;
        // The extended fields (ld/tx/ipb/ci/tn) are the LAPPING-phase instruments — the same readouts that
        // isolated the prime-only bugs (ld = drain progress, tx = real wire time vs the physical frame
        // minimum, ipb/ci/tn = node accounting + the terminator). Their scope lives in the backlog's ring
        // entry.
        // sn/lv: memory residency of the two encode sources — the snapshot buffer (sn) and the live source
        // (lv); P = PSRAM, I = internal, '-' = absent. The ISR reads one of these per byte, and a PSRAM
        // 'sn' under a lapping ring is the measured 4-8x encode cost + the cache-contention corruption
        // exposure.
        const uint8_t* snap = owner_->snapshotBuf();
        const Buffer* src = owner_->sourceBuffer();
        const char snapWhere = snap
            ? (platform::ptrIsPsram(snap) ? 'P' : 'I') : '-';
        const char liveWhere = (src && src->data())
            ? (platform::ptrIsPsram(src->data()) ? 'P' : 'I') : '-';
        std::snprintf(ringDbgStr_, sizeof(ringDbgStr_), "sn%c lv%c sl%u/bf%u co%u cr%u ab%u dn%u ld%u lt%u tx%u ipb%u ci%u tn%d de%u enc%u ea%u sg%u se%u tw%u ts%u tp%u gap%u",
                      snapWhere, liveWhere,
                      static_cast<unsigned>(s.nSlices), static_cast<unsigned>(s.ringBufs),
                      static_cast<unsigned>(s.cacheOffDefers), static_cast<unsigned>(s.cacheOffMaxRun),
                      static_cast<unsigned>(s.stallAbandons),
                      static_cast<unsigned>(s.doneGiven), static_cast<unsigned>(s.lastDrain),
                      static_cast<unsigned>(s.late),
                      static_cast<unsigned>(platform::moonI80Ws2812LastTransmitUs(bus_)),
                      static_cast<unsigned>(s.itemsPerBuf), static_cast<unsigned>(s.consumedItems),
                      static_cast<int>(s.termNodeDiag), static_cast<unsigned>(s.descErr),
                      static_cast<unsigned>(s.maxEncodeUs), static_cast<unsigned>(s.avgEncodeUs),
                      static_cast<unsigned>(segRows ? segGather / segRows : 0),   // avg gather cycles/row (last window)
                      static_cast<unsigned>(segRows ? segEmit / segRows : 0),     // avg emit cycles/row (last window)
                      static_cast<unsigned>(ParallelLedDriver::dbgTickWaitUs),   // wire-wait µs
                      static_cast<unsigned>(ParallelLedDriver::dbgTickSnapUs),   // snapshot µs
                      static_cast<unsigned>(ParallelLedDriver::dbgTickPrimeUs),  // prime µs
                      static_cast<unsigned>(s.maxIsrGapUs));
                      // co = lifetime cache-off ISR defers (flash/WiFi write; ~10/s at idle is normal). cr =
                      // worst consecutive-defer run ≈ buffers drained un-refilled in one window. ab = frames
                      // the DMA self-terminated at the frontier because a window outlasted the pool's lead
                      // (moonI80Ws2812Wait) — each is one held (not corrupted) frame. cr climbing while ab
                      // stays 0 = the pool absorbed every window; ab climbing = held frames the eye may catch.
                      // lt = slices refilled AFTER their drain began (stale on the wire) — the scatter
                      // meter; a clean soak is lt frozen at its arm-time value.
                      // enc = worst ISR refill-encode µs (producer); gap = worst EOF-to-EOF µs (deadline).
                      // enc >= gap == the refill can't keep pace (PACE); enc << gap but still fails == CURSOR/logic.
    }
    /// Which of this backend's controls need the BUS rebuilt (not just a re-encode) when they change:
    /// the WR pin, the output path, and the ring's geometry — buffers are sized and the DMA chain mounted
    /// at build time, so each of these is a rebuild.
    bool busControlTriggersBuild(const char* name) const override {
        return std::strcmp(name, "clockPin") == 0
            || std::strcmp(name, "useRing") == 0      // path switch: rebuild the bus on the new path
            || std::strcmp(name, "ringAuto") == 0     // re-derive (or stop deriving) the geometry
            || std::strcmp(name, "ringRows") == 0     // geometry: buffers are sized and the chain mounted
            || std::strcmp(name, "ringBufs") == 0     // at build time, so a change is a rebuild
            || std::strcmp(name, "ringPadUs") == 0    // the pad is a mounted DMA node — same rebuild
            || std::strcmp(name, "ringSnapshot") == 0  // the snapshot buffer is (de)allocated at build time
            || std::strcmp(name, "shiftOverclock") == 0; // the peripheral clock is set at bus (re)build
    }

    /// WR only reaches a pad in shift mode, so it can only COLLIDE in shift mode. In direct mode the
    /// signal never leaves the peripheral, so `clockPin` naming a strand's GPIO is harmless — and
    /// rejecting it would forbid a perfectly good config for the sake of a signal nobody reads.
    const char* validateBusFatal() const override {
        if (owner_->pinExpanderMode()) {
            // The '595 needs WR on a real GPIO (it is the SRCLK). Unset (-1) would route the
            // peripheral's WR signal to GPIO 65535 — reject it before busInit reaches the pad.
            if (clockPin < 0) return "the 74HCT595 expander needs a clockPin (its shift clock)";
            if (owner_->latchPin >= 0 && owner_->latchPin == clockPin)
                return "latchPin is on clockPin (WR) — the latch needs its own GPIO";
        }
        return nullptr;
    }
    /// A data lane sharing WR's GPIO is silent corruption — the matrix routes both signals to the one
    /// pad and that strand emits the shift clock instead of pixel data. Only possible in shift mode.
    const char* validateBusPins(const uint16_t* lanes, uint8_t n) const override {
        if (!owner_->pinExpanderMode()) return nullptr;
        for (uint8_t i = 0; i < n; i++)
            if (lanes[i] == static_cast<uint16_t>(clockPin)) return "a data pin is on clockPin (WR)";
        return nullptr;
    }

    /// Create the bus + its DMA buffer(s) for `frameBytes`. `busPinList()`/`busPinCount()` come from
    /// the orchestrator (in shift mode the list appends the latch — it is a bus lane), and
    /// `busClockMultiplier()` tells the platform how many bus words one WS2812 slot is shifted out
    /// over, so it can scale the pixel clock and the slot keeps its wire duration.
    bool busInit(size_t frameBytes, bool /*wantSecondBuffer*/) override {
        platform::moonI80SetShiftClockDiv(shiftOverclock ? 3 : 4);   // ON = 26.67 MHz, OFF = 20 MHz
        // Force single-buffer HERE regardless of the request: this backend's own-GDMA whole-frame
        // two-buffer completion handshake races and wedges the bus (the double-buffer freeze). The
        // orchestrator already gates the request via supportsDoubleBuffer()==false, but the backend
        // enforces its own contract too, so a future caller that forgets the gate can't reach the broken
        // path. MoonI80's speed comes from the streaming ring, not from double-buffering a whole frame.
        return platform::moonI80Ws2812Init(bus_, owner_->busPinList(), owner_->busPinCount(),
                                           static_cast<uint16_t>(clockPin), frameBytes,
                                           /*wantSecondBuffer=*/false, owner_->busClockMultiplier());
    }
    /// The rest of the bus surface: one-line forwards to the platform seam, each carrying the
    /// contract its `LedPeripheral` base already states (`busBuffer` is "DMA buffer i the encoder
    /// writes into", and so on). Restating that here would give doxygen two copies of one contract
    /// to drift apart — the base is the single home. Only `busDeinit` adds behaviour of its own:
    /// the snapshot helper is this backend's, so it stops before the bus goes away.
    void busDeinit() override { stopSnapHelper(); platform::moonI80Ws2812Deinit(bus_); }
    uint8_t* busBuffer(uint8_t i) override { return platform::moonI80Ws2812Buffer(bus_, i); }
    size_t busCapacity() const override { return platform::moonI80Ws2812BufferCapacity(bus_); }
    bool busTransmit(uint8_t i, size_t bytes) override { return platform::moonI80Ws2812Transmit(bus_, i, bytes); }
    bool busWait(uint8_t i, uint32_t ms) override { return platform::moonI80Ws2812Wait(bus_, i, ms); }
    uint32_t busLastTransmitUs() const override { return platform::moonI80Ws2812LastTransmitUs(bus_); }

    /// Drive `frame` on a private bus and capture the wire back on `loopbackRxPin` (jumpered), so the
    /// self-test bit-verifies what the peripheral ACTUALLY emitted — the one instrument that does not
    /// take the driver's word for it.
    platform::RmtLoopbackResult busLoopback(const uint8_t* frame, size_t frameBytes,
                                            size_t dataBytes, uint8_t rowBits) override {
        return platform::moonI80Ws2812Loopback(owner_->busPinList(), owner_->busPinCount(),
                                               static_cast<uint16_t>(clockPin),
                                               static_cast<uint16_t>(owner_->loopbackRxPin),
                                               frame, frameBytes, dataBytes, rowBits,
                                               owner_->busClockMultiplier(),
                                               ringRows, ringBufs, useRing);
    }

    /// Should reinit build a RING for this config instead of the whole-frame path? Only in pin-expander
    /// mode — direct mode never rings, it drives PSRAM fine at the 10x-slower clock — and then it is the
    /// user's choice via `useRing`, defaulting to on.
    ///
    /// There is no auto-router. It would have exactly one right answer at the size the expander exists for
    /// (a 48x256 frame never fits internal DMA RAM, so it would always pick the ring) while presenting
    /// itself as a decision, and its silent fallback made the ACTIVE path invisible — the driver reported
    /// "driving N lights" while quietly running whole-frame from PSRAM, which does not clock at the
    /// expander's 26.67 MHz. The switch says what runs.
    bool wantsRing() const override {
        if (!owner_->pinExpanderMode()) return false;   // direct mode never rings (drives PSRAM fine)
        return useRing;
    }

    /// Bring the bus up as a streaming RING (the phase-2 path): the platform loops a few small internal
    /// buffers and calls back per drained buffer to refill it, so a frame too big for internal RAM never
    /// materialises (see platform.h). `rowBytes`/`padBytes` come from the orchestrator's frame arithmetic;
    /// the trampoline below is the encode seam. Returns false if even the small ring won't fit — the
    /// orchestrator then falls back to busInit (whole-frame), which idles with a status if IT can't fit
    /// either.
    bool busInitRing(size_t rowBytes, uint32_t totalRows) override {
        // AUTO geometry (see ringAuto): derive the winning combination for THIS config and write it into
        // the visible controls — rows = one-node max (fewest slices), bufs = as deep as fits. The RAM
        // budget takes free internal MINUS a reserve (WiFi/HTTP need their share; same spirit as the
        // platform's own HEAP_RESERVE floor).
        if (ringAuto && rowBytes > 0) {
            const uint32_t maxRows = static_cast<uint32_t>(platform::kRingNodeMaxBytes / rowBytes);
            ringRows = static_cast<uint8_t>(maxRows > 64 ? 64 : (maxRows ? maxRows : 1));
            const uint32_t slices = (totalRows + ringRows - 1u) / ringRows;
            const size_t bufBytes = static_cast<size_t>(ringRows) * rowBytes;
            const size_t freeInt = platform::freeInternalHeap();
            constexpr size_t kReserve = 64 * 1024;   // leave WiFi/HTTP/stacks their internal share
            // Spend the full post-reserve budget: a deep pool is the whole win (each buffer of depth
            // removes one slice from the ISR's per-frame load), and the proven 48x256 config runs a
            // ~121 KB pool alongside WiFi+HTTP. The snapshot falls back to PSRAM when squeezed (a
            // measured ~10% encode cost — far cheaper than a shallow pool's stale slices).
            const size_t budget = freeInt > kReserve ? (freeInt - kReserve) : 0;
            const uint32_t byRam = bufBytes ? static_cast<uint32_t>(budget / bufBytes) : 0;
            uint32_t bufs = slices + 1u;             // prime-only when it fits — the ideal
            if (bufs > platform::kRingBufsMax) bufs = platform::kRingBufsMax;
            if (bufs > byRam) bufs = byRam;
            ringBufs = static_cast<uint8_t>(bufs >= platform::kRingBufsMin ? bufs : platform::kRingBufsMin);
        }
        platform::moonI80SetShiftClockDiv(shiftOverclock ? 3 : 4);   // ON = 26.67 MHz, OFF = 20 MHz
        const bool ok = platform::moonI80Ws2812InitRing(bus_, owner_->busPinList(), owner_->busPinCount(),
                                               static_cast<uint16_t>(clockPin), rowBytes, totalRows,
                                               ringRows, ringBufs, ringPadUs, owner_->busClockMultiplier(),
                                               &MoonI80Peripheral::ringEncodeTrampoline, this);
        // Ring up → bring the parallel-snapshot helper up too (idempotent; parks in waitNotify). Torn down
        // in busDeinit with the bus. Spawned here (cold reinit path) so kick()/join() only ever notify.
        if (ok) ensureSnapHelper();
        return ok;
    }
    /// Send one frame on the ring: prime the pool, fire the DMA, and let the EOF ISR refill behind it.
    ///
    /// With the core-0 helper up (dual-core split active), the PRIME is fork-joined: ring buffers are
    /// independent (each derives its rows from its index), so the helper primes the bottom half of the
    /// pool on core 0 while this core primes the top half, the join fences both, then the arm starts
    /// the DMA. Serial fallback: the platform's prime-all-then-arm combo.
    bool busTransmitRing() override {
        if (snapHelperReady() && ringBufs >= 2) {
            // `done` is written-gated (leads the wire drain); the prime itself takes the wire barrier —
            // each prime call waits out the previous frame's deterministic wire end before writing (see
            // waitWireDrained/primeRingRange in the i80 platform driver), so neither fork half can repaint
            // a buffer the DMA is still draining.
            const uint8_t half = static_cast<uint8_t>(ringBufs / 2);
            primeLo_ = 0; primeHi_ = half;
            helperKick();                                             // core 0: buffers [0, half)
            platform::moonI80Ws2812PrimeRange(bus_, half, ringBufs);   // this core: [half, ringBufs)
            helperJoin();                                              // fence: every buffer primed
            return platform::moonI80Ws2812ArmRing(bus_);
        }
        return platform::moonI80Ws2812TransmitRing(bus_);
    }
    /// The ring's regime for the driving-status suffix: "primed" (whole frame encoded before arming —
    /// no deadline, pixel-perfect) vs "lapping" (the ISR refills behind the DMA — the deadline regime).
    const char* busRingMode() const override {
        const platform::MoonI80RingStats s = platform::moonI80Ws2812RingStats(bus_);
        if (!s.isRing) return nullptr;
        return s.nSlices <= s.ringBufs ? "primed" : "lapping";
    }
    /// Did the bus actually come up as a ring? The orchestrator routes tick() on this, so it reports what
    /// the platform BUILT, not what was asked for — a ring that would not fit falls back to whole-frame.
    bool busIsRing() const MM_NONBLOCKING override { return platform::moonI80Ws2812IsRing(bus_); }

    /// The platform's `MoonI80EncodeFn` seam: a plain function pointer (there is no virtual hook for it),
    /// so this static trampoline recovers `this` from `user` and encodes one slice into the ring buffer the
    /// platform hands it. `dst` is the buffer; `firstRow`/`rowCount` name the slice; `closeFrame` gates
    /// the trailing latch pad (only the last slice emits it).
    ///
    /// **It prefills the shift constants FIRST, then the data — because a ring buffer is recycled, not
    /// re-zeroed.** The whole-frame path prefills once at reinit and `encodeRows` then writes only the
    /// DATA word (two-thirds of the stores hoisted out); but a ring buffer holds a DIFFERENT slice each
    /// time it drains, so its constants (pulse-start/tail per this slice's active mask, and the latch pad
    /// on the last slice) must be re-laid on every refill or the buffer keeps the previous slice's
    /// constants and renders wrong on the second frame (pinned by the recycled==fresh host test). Direct
    /// mode writes every slot word in encodeRows, so it needs no prefill.
    static void MM_RAMFUNC ringEncodeTrampoline(void* user, uint8_t* dst, uint32_t firstRow, uint32_t rowCount,
                                                bool closeFrame, bool needsPrefill) {
        auto* self = static_cast<MoonI80Peripheral*>(user);
        ParallelLedDriver* owner = self->owner_;
        const uint8_t outCh = owner->correction().outChannels;
        const auto first = static_cast<nrOfLightsType>(firstRow);
        const auto count = static_cast<nrOfLightsType>(rowCount);
        if (rowCount == 0) {
            // The FRAME-CLOSE call (see MoonI80EncodeFn): write only the latch-only word, which presents
            // the register's final slot on the strand. Direct mode has no close word — zeros are LOW.
            if (closeFrame) owner->encodeFrameClose(dst);
            return;
        }
        // Prefill only when the buffer's constants are actually gone (`needsPrefill` — the platform's
        // buffer-lifecycle fact: first use, or after a platform memset; see MoonI80EncodeFn). A recycled
        // buffer's constants survive a data-only refill byte-identically, and the per-refill prefill was
        // ~1/3 of the ISR encode cost — the difference between the refill fitting its drain deadline or not.
        // RAGGED strands still prefill every time: the active mask varies per ROW, so a buffer holding a
        // different slice needs that slice's row masks re-laid regardless of recycling.
        const bool prefill = owner->pinExpanderMode() && (needsPrefill || !owner->uniformLaneCounts());
        if (owner->slotBytes() == 1) {
            if (prefill) owner->prefillShiftRows<uint8_t>(outCh, dst, first, count);
            owner->encodeRows<uint8_t>(outCh, dst, first, count, closeFrame);
        } else {
            if (prefill) owner->prefillShiftRows<uint16_t>(outCh, dst, first, count);
            owner->encodeRows<uint16_t>(outCh, dst, first, count, closeFrame);
        }
    }

    /// WR is part of the bus identity, so a change to it rebuilds the bus — not just a data-pin edit.
    void recordBusPins() override { lastClockPin_ = clockPin; }
    /// Do this backend's extra bus pins still match the live bus? WR is bus identity here, so the
    /// orchestrator rebuilds when this goes false rather than routing a stale clock.
    bool extraBusPinsCurrent() const override { return lastClockPin_ == clockPin; }

    // --- Fork-join helper (the ring's core-0 prime-half worker) ---
    // The pool PRIME (~14 ms at 48×256) is embarrassingly parallel — each ring buffer derives its rows from
    // its own index — so under the render/encode split (ring tick on core 1) one core-0 helper task primes
    // the bottom half of the pool while core 1 primes the top, per frame. The task is spawned once at ring
    // engage (kept parked in waitNotify between kicks) and torn down with the bus. ready() gates on the
    // split running: with it off (single-core) the prime stays serial and the helper never engages.
    // NOTE: the SNAPSHOT is NOT forked (it copies serially on core 1). Forking it too stacked a second
    // core-0 job on the render's composite and SATURATED core 0 at 48×256 — the idle task starved and the
    // board hung (whole-session freeze); it also raced the prime fork's shared helper state (the bottom-16
    // flicker). Priming alone keeps the parallel win without either failure.

    /// True when the fork-join should engage: the helper task is up AND this caller is running on core 1
    /// — i.e. the render/encode split is active and core 0 is the idle one to hand the bottom half. On
    /// core 0 (single-core, or the split disengaged), there is no idle second core, so stay serial and
    /// avoid spawning contention onto the very core doing the render.
    bool snapHelperReady() const override {
        return snapHelper_.impl != nullptr && !snapHelperBroken_ && platform::currentCore() == 1;
    }


    /// Publish this frame's prime job and wake the helper. Paired with `helperJoin` — never call
    /// one without the other, or the next kick mutates bounds the helper is still reading.
    void helperKick() {
        if (!snapHelper_.impl) return;
        // Wait for the helper to be PARKED before notifying it for the next job. What actually keeps the
        // bounds (primeLo_/primeHi_) race-free is the strict ordering, not this wait: the caller only writes
        // new bounds AFTER the previous helperJoin() returned, and join returns only once the helper stored
        // snapHelperDone_ (which happens strictly after runHelperJob finished reading the old bounds). So the
        // helper is never reading bounds the caller is writing. The park-wait is belt-and-braces on top of
        // that: it ensures the helper is back in waitNotify (not still between "set done" and "re-park")
        // before this notify, so the notify can't be lost or double-counted. Single-producer/consumer
        // discipline. Bounded — a stuck helper self-heals to serial (do the prime ourselves, stop using it).
        const uint32_t t0 = platform::millis();
        while (!snapHelperParked_.load(std::memory_order_acquire)) {
            if (platform::millis() - t0 > kSnapJoinTimeoutMs) { snapHelperBroken_ = true; runHelperJob(); return; }
            platform::yield();
        }
        snapHelperParked_.store(false, std::memory_order_release);   // consume the park; helper re-sets it
        snapHelperDone_.store(false, std::memory_order_release);
        platform::notifyTask(snapHelper_);
    }

    /// Block until the helper's half is primed. The fence that makes the fork-join safe: the caller
    /// may not touch the pool or the bounds until this returns. A timeout latches the helper broken
    /// and runs the job serially instead, so a wedged worker degrades rather than stalls the frame.
    void helperJoin() {
        if (!snapHelper_.impl) return;
        // A degraded kick (helper never parked → snapHelperBroken_, ran serially) already did the work and
        // left snapHelperDone_ untouched; nothing to wait for.
        if (snapHelperBroken_) return;
        // Acquire-poll on the helper's release with a REAL-TIME deadline — the Drivers::quiesceEncode
        // idiom (a spin COUNT is meaningless: yield() may return instantly, turning a count into a hot
        // burn). A healthy half takes single-digit ms; 100 ms means the helper is broken.
        const uint32_t t0 = platform::millis();
        while (!snapHelperDone_.load(std::memory_order_acquire)) {
            if (platform::millis() - t0 > kSnapJoinTimeoutMs) {
                // SELF-HEAL, never wedge: do the helper's half OURSELVES (both jobs are idempotent —
                // same inputs, same bytes — so even a late helper write is identical, not torn) and stop
                // using the helper from now on. One degraded frame beats a stuck render loop.
                runHelperJob();
                snapHelperBroken_ = true;
                return;
            }
            platform::yield();
        }
    }

    /// The job itself: prime the buffer range the kick published. Called on the helper task, and on
    /// THIS task as the serial fallback when the helper is broken or absent.
    void runHelperJob() { platform::moonI80Ws2812PrimeRange(bus_, primeLo_, primeHi_); }

    /// Bring the helper task up (idempotent). Called at ring engage — see busInitRing's tail. Pinned to
    /// CORE 0: the ring's tick runs on core 1 under the split, so the helper takes the OTHER core.
    void ensureSnapHelper() {
        if (snapHelper_.impl) return;
        snapHelperBroken_ = false;   // a rebuild gets a fresh chance
        snapHelperStop_.store(false, std::memory_order_release);
        // The fresh task parks once (setting snapHelperParked_=true) before any kick; seed it true so the
        // very first kick doesn't wait on a park signal the helper only emits after it starts running.
        snapHelperParked_.store(true, std::memory_order_release);
        snapHelperDone_.store(true, std::memory_order_release);
        platform::spawnPinnedTask(snapHelper_, "mmSnap", &MoonI80Peripheral::snapHelperTramp, this,
                                  4096, 5, /*core=*/0);
    }
    /// Tear the helper down: signal the stop flag, wake it, and wait for it to leave its loop. Called
    /// from `busDeinit`, so the worker cannot outlive the bus it primes into.
    void stopSnapHelper() {
        if (!snapHelper_.impl) return;
        snapHelperStop_.store(true, std::memory_order_release);
        platform::stopPinnedTask(snapHelper_);
    }

    /// The worker's entry point — a plain function, because the task API takes no member pointer.
    /// Loops: park in waitNotify, run the published job, mark done, repeat until stopped.
    static void snapHelperTramp(void* user) {
        auto* self = static_cast<MoonI80Peripheral*>(user);
        while (!self->snapHelperStop_.load(std::memory_order_acquire)) {
            // Announce PARKED before blocking, so helperKick knows the previous job is fully done and the
            // helper is no longer reading helperJob_/the bounds — only then does it publish the next job.
            // This closes the fast-path window where the trampoline had set snapHelperDone_ but not yet
            // returned to waitNotify, during which the next kick would overwrite the inputs under it.
            self->snapHelperParked_.store(true, std::memory_order_release);
            if (!platform::waitNotify(self->snapHelper_, 100)) continue;   // re-check stop on timeout
            if (self->snapHelperStop_.load(std::memory_order_acquire)) break;
            self->runHelperJob();                                           // this core-0 frame's half
            self->snapHelperDone_.store(true, std::memory_order_release);
        }
    }

private:
    /// Join deadline for one prime half. A half is single-digit ms, so 100 means the helper is broken,
    /// not slow — the timeout latches `snapHelperBroken_` rather than retrying forever.
    static constexpr uint32_t kSnapJoinTimeoutMs = 100;
    /// The core-0 worker that primes the bottom half of the ring pool. Spawned on first ring init,
    /// parked in waitNotify between frames.
    platform::WorkerTask snapHelper_{};
    /// Stored by the helper when its job is finished; `helperJoin` spins on it. Starts true so the
    /// first join returns immediately.
    std::atomic<bool> snapHelperDone_{true};
    /// Asks the helper to exit its wait loop; set once by `stopSnapHelper` at teardown.
    std::atomic<bool> snapHelperStop_{false};
    /// Set by the trampoline right before it blocks in waitNotify; cleared by helperKick when it
    /// publishes a new job. The kick waits for this before mutating the bounds (primeLo_/primeHi_) — so
    /// the helper is provably done reading the PREVIOUS job's inputs. Starts true: the helper parks once
    /// at spawn before any kick.
    std::atomic<bool> snapHelperParked_{true};
    /// Self-heal latch: one timed-out join disables the helper for good and the prime runs serial from
    /// then on. A wedged worker must not be re-waited every frame.
    bool snapHelperBroken_ = false;
    /// The buffer range [lo, hi) the helper primes this frame — written only between join and kick.
    uint8_t primeLo_ = 0, primeHi_ = 0;

    /// The platform-side bus handle; opaque here, so no LCD_CAM type escapes the platform layer.
    platform::MoonI80Ws2812Handle bus_;
    /// The clockPin the bus was last built with, so a change to the control can force a rebuild.
    int8_t lastClockPin_ = -1;
};

// Register the MoonI80 (own-GDMA below esp_lcd) backend into the peripheral registry once, at
// static-init. Gated by this header's CONFIG_SOC include in main.cpp (LCD_CAM chips only). No separate
// driver class — the one ParallelLedDriver drives it, chosen via the `peripheral` control.
//
// "LCD-MM": the same LCD peripheral the IDF backend uses, driven by our own GDMA layer instead of
// esp_lcd — which is the whole difference a user is choosing between. No chip conditional here: this
// backend only ever registers on LCD_CAM silicon, so the peripheral is always the LCD one.
inline const bool kMoonI80PeripheralRegistered =
    ParallelLedDriver::registerPeripheral("LCD-MM", []() -> LedPeripheral* { return new MoonI80Peripheral(); });

} // namespace mm
