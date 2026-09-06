#pragma once

#include <cstddef>
#include <cstdint>
#include "core/Control.h"          // ControlList — a backend appends its own controls into the shared list
#include "platform/platform.h"     // RmtLoopbackResult

namespace mm {

class ParallelLedDriver;   // the orchestrator; a backend reads shared state through this back-pointer

/// The physical peripheral block a backend drives. Two backends on the SAME block conflict — the chip
/// has exactly one of each — so the orchestrator refuses to bring up a second driver already claiming a
/// block a sibling holds. Note esp_lcd-i80 (on the S3/P4) and MoonI80 BOTH drive LcdCam: they are the
/// same silicon reached two ways, so they conflict with each other, not only with themselves. The
/// classic-ESP32 i80 is the I2S peripheral, a different block.
enum class LedHwBlock : uint8_t { None = 0, LcdCam, I2s, Parlio };

/// A parallel-WS2812 output peripheral, behind a runtime strategy interface.
///
/// `ParallelLedDriver` (the one MoonModule) owns the controls, lifecycle, tick, and the shared
/// slice/encode/double-buffer/expander/loopback machinery, and drives ONE `LedPeripheral` chosen at
/// runtime (Parlio, esp_lcd i80, or MoonI80 own-GDMA). The peripheral supplies only the variant
/// operations — bring the bus up, hand back its DMA buffer, transmit a frame, tear down — plus a few
/// static descriptors (lane count, expander support, bus-width rounding) as virtuals, so the
/// orchestrator reads them through the one `LedPeripheral*` rather than a compile-time type.
///
/// **Not a hot-path virtual boundary.** Every method here is called per-FRAME or per-reinit, never
/// per-light: the per-light encode operates on the raw `uint8_t*` `busBuffer()` hands back and never
/// calls into the peripheral. One vcall per frame against ~thousands of µs of frame work is free — the
/// dispatch that mattered (per-light) stays a direct call inside the shared encode.
///
/// **Shared state via a back-pointer.** A backend reaches the orchestrator's parsed lane list, latch
/// bit, correction, and loopback pin through `owner()` (set once by `attach()`), replacing what CRTP
/// inheritance gave for free. The orchestrator exposes exactly those as public const accessors.
///
/// Prior art: the Strategy / pluggable-backend pattern; the projectMM `ListSource` / `DevicePlugin`
/// adapter shape (a generic owner + a variant object that travels with its own state).
class LedPeripheral {
public:
    virtual ~LedPeripheral() = default;

    /// Bind the peripheral to its orchestrator. Called once, right after construction, before any
    /// bus operation. The backend reads shared state (bus pin list, latch, correction, loopback pin)
    /// through this pointer.
    void attach(ParallelLedDriver* owner) { owner_ = owner; }

    // --- Static descriptors: per-peripheral constants the orchestrator reads through the interface ---
    /// Parallel lanes this peripheral's silicon provides on the current chip (0 = not this chip).
    virtual uint8_t lanesAvailable() const MM_NONBLOCKING = 0;
    /// Can this peripheral host the 74HCT595 pin expander? (Needs a DMA that reaches PSRAM.)
    virtual bool supportsPinExpander() const = 0;
    /// Can this peripheral run the async double-buffer (a second whole-frame buffer, encode overlaps
    /// wire)? Default yes — i80/Parlio route through a real transaction queue (esp_lcd / the Parlio
    /// driver), so a second in-flight transfer is handled for us. MoonI80 owns its GDMA with no queue,
    /// and its hand-rolled two-buffer completion handshake races; it overrides this false and runs
    /// single-buffer (its speed comes from the ring, not from double-buffering a whole frame).
    virtual bool supportsDoubleBuffer() const { return true; }
    /// Does the bus width round up to a power of two (8/16), or is it the exact pin count?
    virtual bool powerOfTwoBus() const = 0;
    /// The status message when bus init fails on this peripheral.
    virtual const char* initFailMsg() const = 0;

    /// True when a PREVIOUS init failed only because another module held a peripheral this backend
    /// needs, and that peripheral is now free: the driver then rebuilds itself, so the loser of a
    /// contended claim recovers without the user touching anything. Cheap enough for tick1s (a
    /// registry read, never an init). Default false: a backend with nothing to share never retries,
    /// which keeps a genuinely bad config from re-attempting once a second forever.
    virtual bool busContentionCleared() const { return false; }
    /// Must the loopback self-test build a full-width bus (true) or can it run on a private 1-lane
    /// unit (false)? esp_lcd i80 / MoonI80 need the full width; Parlio can do a single lane.
    virtual bool loopbackFullWidth() const = 0;
    /// The physical peripheral block this backend drives — the orchestrator's claim guard refuses two
    /// live drivers on the same block. On the classic ESP32 the esp_lcd-i80 backend is the I2S block;
    /// on the S3/P4 it (and MoonI80) is LcdCam.
    virtual LedHwBlock hwBlock() const = 0;

    // --- Required core (no default; every backend implements) ---
    /// Create the bus + its DMA buffer(s) sized for `frameBytes`; `wantSecondBuffer` requests the
    /// async double-buffer's second frame (allocated only if it fits). Returns whether init succeeded.
    virtual bool busInit(size_t frameBytes, bool wantSecondBuffer) = 0;
    /// Tear down the bus and its DMA buffer(s).
    virtual void busDeinit() = 0;
    /// DMA buffer `i` (0/1) the encoder writes into; buffer 1 is null in single-buffer mode.
    virtual uint8_t* busBuffer(uint8_t i) = 0;
    /// Per-buffer byte capacity (fixed at bus creation; both buffers equal).
    virtual size_t busCapacity() const = 0;
    /// Kick off the autonomous transfer of the first `bytes` of DMA buffer `i`; returns whether it
    /// started.
    virtual bool busTransmit(uint8_t i, size_t bytes) = 0;
    /// Block up to `ms` for buffer `i`'s in-flight transfer to complete.
    virtual bool busWait(uint8_t i, uint32_t ms) = 0;
    /// The most recent DMA transfer's wire time (µs) — the WS2812 output floor.
    virtual uint32_t busLastTransmitUs() const = 0;
    /// Run the loopback self-test on this peripheral (each builds its own bus, per loopbackFullWidth).
    virtual platform::RmtLoopbackResult busLoopback(const uint8_t* frame, size_t frameBytes,
                                                    size_t dataBytes, uint8_t rowBits) = 0;

    // --- Ring cluster (default: no ring — only the MoonI80 backend overrides) ---
    /// Bring the bus up as a streaming ring for `totalRows` rows of `rowBytes`; false if it won't fit.
    virtual bool busInitRing(size_t /*rowBytes*/, uint32_t /*totalRows*/) { return false; }
    /// Send one frame on the ring (prime + arm + ISR refill). False if the ring isn't up.
    virtual bool busTransmitRing() { return false; }
    /// Is the live bus a streaming ring?
    virtual bool busIsRing() const MM_NONBLOCKING { return false; }
    /// Should reinit build a ring for the current config instead of the whole-frame path?
    virtual bool wantsRing() const { return false; }
    /// The ring's active-mode label for the status line, or nullptr for the whole-frame path.
    virtual const char* busRingMode() const { return nullptr; }
    /// Append this peripheral's ring controls into the shared list. Default: none.
    virtual void addRingControls(ControlList& /*controls*/) {}
    /// Refresh any peripheral-specific read-only KPIs (the ring diagnostic). Default: none.
    virtual void refreshBusKpi() {}
    /// Is the core-0 fork-join snapshot helper up (dual-core prime)? Default: no helper.
    virtual bool snapHelperReady() const { return false; }

    // --- Bus-pin cluster (default: no extra pins — Parlio) ---
    /// Append this peripheral's bus-pin controls (WR/DC clock pins) into the shared list. Default: none.
    virtual void addBusControls(ControlList& /*controls*/) {}
    /// Does a change to control `name` require a bus rebuild (a bus pin changed)? Default: no.
    virtual bool busControlTriggersBuild(const char* /*name*/) const { return false; }
    /// Snapshot the current bus pins so extraBusPinsCurrent can detect a later change. Default: none.
    virtual void recordBusPins() {}
    /// Are the recorded bus pins still current (no un-applied change)? Default: always current.
    virtual bool extraBusPinsCurrent() const { return true; }
    /// A per-peripheral fatal validation (returns a status message) run before bus init. Default: ok.
    virtual const char* validateBusFatal() const { return nullptr; }
    /// A per-peripheral lane-pin validation (returns a warning) — e.g. a data pin colliding with WR.
    virtual const char* validateBusPins(const uint16_t* /*lanes*/, uint8_t /*n*/) const { return nullptr; }
    /// The GPIO the bus parks spare (unused) lanes on (only reached when powerOfTwoBus rounds the
    /// bus wider than the data-pin count — i80/MoonI80, which both override this to their WR pin).
    /// The default 0 is never used by a peripheral whose bus is the exact pin count (Parlio,
    /// powerOfTwoBus=false, never pads), so no owner lookup is needed here.
    virtual uint16_t clockPinForBus() const { return 0; }
    /// Must a spare (unused) bus lane be parked on a REAL GPIO? `esp_lcd` rejects an NC data pin, so
    /// the i80 backend has to give every lane a pad and parks the spares on WR: a ghost claim that
    /// drives a pin the board never wired. A backend that owns its own GPIO routing does not pay that
    /// tax: an unrouted lane simply stays inside the peripheral. Answering false keeps a spare lane
    /// off the pin map entirely, which is what stops a padded lane from silently driving a pad another
    /// peripheral owns (an S31's RGMII bus, for one).
    virtual bool spareLanesNeedPad() const { return true; }
    /// The whole-frame DMA byte budget: 0 = "no bound" (PSRAM-capable). A bounded peripheral (the
    /// classic-ESP32 i80 = internal-RAM-only I2S) returns a positive ceiling. Default: no bound.
    virtual size_t dmaBudgetBytes() const { return 0; }

protected:
    ParallelLedDriver* owner_ = nullptr;
};

} // namespace mm
