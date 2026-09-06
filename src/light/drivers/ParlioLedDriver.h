#pragma once

#include "light/drivers/ParallelLedDriver.h"   // shared driver body + LedPeripheral
#include "platform/platform.h"


namespace mm {

/// Output driver: parallel WS2812B over the ESP32-P4 Parlio (Parallel IO) TX peripheral — the P4's
/// scale path, sibling of MultiPinLedDriver. The shared body (slicing, encode, single-shot DMA, loopback)
/// lives in ParallelLedDriver; Parlio is the SIMPLER peripheral, so this backend adds LESS than the
/// i80 backend:
///  - NO clockPin/dcPin: Parlio generates the pixel clock itself (kClockHz), so there are no
///    sacrificial WR/DC lines (addBusControls is empty).
///  - powerOfTwoBus() = false: Parlio's bus width IS the pin count (any 1..16), so nothing rounds. The
///    i80 bus is 8 or 16 bits wide whatever the pin count, and parks the lanes it doesn't need on WR.
///    Either way the user names only the pins that drive a strand.
///
/// Prior art: the ESP32-P4 Parlio peripheral, the hpwit/FastLED parallel-WS2812 lineage —
/// architecture studied, never copied.
class ParlioPeripheral : public LedPeripheral {
public:
    // All controls default to UNSET — pins="", ledsPerPin="" (= all lights on the
    // first lane, even-split with one lane), loopbackRxPin=-1 — so no constructor is
    // needed (the orchestrator default-initialises them). Pins/loopback are unset because the
    // strand is user-soldered: a hard-coded pin would guess the user's wiring and
    // could drive a pin committed elsewhere ("default only when it cannot do harm",
    // see lessons.md). The P4-NANO bench uses pins "20,21,22,23,24,25,26,27",
    // loopbackRxPin 33 (clear of the NANO's strapping 34-38, Ethernet RMII
    // 28-31/49-52, C6 SDIO 14-19/54, I2C 7-8 — clear GPIOs are 20-27, 32-33, 39-48).

    // --- LedPeripheral descriptors ---

    /// The number of Parlio lanes this chip provides (0 = not this chip); the orchestrator's
    /// inert-on-wrong-chip guards key off it.
    uint8_t lanesAvailable() const MM_NONBLOCKING override { return platform::parlioLanes; }
    bool powerOfTwoBus() const override { return false; }   // 1..16 lanes all valid
    // Parlio builds a 1-lane private unit for the loopback, so the loopback frame stays 8-bit
    // regardless of the operational bus width (unlike i80, which needs a full-width bus).
    bool loopbackFullWidth() const override { return false; }
    /// Parlio is its own TX peripheral block, distinct from LcdCam/I2S — it coexists with an i80 or
    /// MoonI80 driver on the same chip (the P4 has both).
    LedHwBlock hwBlock() const override { return LedHwBlock::Parlio; }
    const char* initFailMsg() const override { return "Parlio init failed, check pins / memory"; }

    // The WS2812 slot rate (375 ns @ 2.67 MHz) — identical to the LCD backend's;
    // the P4 Parlio's 160 MHz PLL clock divides to it exactly (/60).
    static constexpr uint32_t kClockHz = 2'666'666;

    /// No bus controls: Parlio has no sacrificial clock/DC pins (the bus rebuilds on
    /// a data-pin change alone).
    void addBusControls(ControlList&) override {}
    /// No extra bus controls, so none can trigger a rebuild.
    bool busControlTriggersBuild(const char*) const override { return false; }
    /// No extra pins to record (Parlio has no WR/DC).
    void recordBusPins() override {}
    /// No extra pins to track, so they are always current.
    bool extraBusPinsCurrent() const override { return true; }

    /// No 74HCT595 expander on Parlio: its single-shot transfer caps at 65,535 bytes
    /// (PARLIO_LL_TX_MAX_BITS_PER_FRAME), and the ×8 fan-out frame is ~145 KB — 2.2× over. The
    /// orchestrator refuses shift mode here with a status rather than emitting a frame the peripheral
    /// drops. (The P4's route to the expander is its LCD_CAM/i80 bus, which I80Peripheral already
    /// drives; lifting this needs the chunked-transfer work, not a flag flip.)
    bool supportsPinExpander() const override { return false; }

    /// Create the Parlio bus + its DMA buffer(s) sized for `frameBytes` on the current lanes, driving
    /// the pixel clock at kClockHz; `wantSecondBuffer` requests the async double-buffer's second frame
    /// buffer (allocated only if it fits). Returns whether init succeeded.
    bool busInit(size_t frameBytes, bool wantSecondBuffer) override {
        return platform::parlioWs2812Init(parlio_, owner_->laneList(), owner_->laneCount(),
                                          kClockHz, frameBytes, wantSecondBuffer);
    }
    /// DMA buffer `i` (0/1) the orchestrator encodes into; buffer 1 is null when the second
    /// buffer didn't fit (single-buffer mode). Both are the same size (busCapacity).
    uint8_t* busBuffer(uint8_t i) override        { return platform::parlioWs2812Buffer(parlio_, i); }
    /// The per-buffer byte capacity (fixed at bus creation; both buffers equal).
    size_t   busCapacity() const override         { return platform::parlioWs2812BufferCapacity(parlio_); }
    // Parlio sends a frame in ONE transfer, and the peripheral caps that. The cap is a HARDWARE
    // fact (derived from PARLIO_LL_TX_MAX_BITS_PER_FRAME), so the platform owns the number and this
    // asks for it — reinit() then refuses an oversized frame with an actionable status BEFORE
    // busInit tries (and fails) to allocate it, which is what left the LEDs frozen with a healthy
    // UI (issue #44).
    size_t   dmaBudgetBytes() const override      { return platform::parlioMaxTransferBytes(); }
    /// Kick off the autonomous transfer of the first `bytes` of DMA buffer `i`;
    /// returns whether it started.
    bool  busTransmit(uint8_t i, size_t bytes) override { return platform::parlioWs2812Transmit(parlio_, i, bytes); }
    /// Block up to `ms` for buffer `i`'s in-flight transfer to complete.
    bool  busWait(uint8_t i, uint32_t ms) override      { return platform::parlioWs2812Wait(parlio_, i, ms); }
    /// The most recent DMA transfer's wire time (µs) — the WS2812 output floor.
    uint32_t busLastTransmitUs() const override         { return platform::parlioWs2812LastTransmitUs(parlio_); }
    /// Tear down the Parlio bus and its DMA buffer.
    void     busDeinit() override                 { platform::parlioWs2812Deinit(parlio_); }

    /// Run the loopback self-test. Parlio runs on a single lane, so the loopback
    /// builds its own private 1-lane unit on lane 0 (no i80 full-bus workaround
    /// needed; no WR/DC to pass).
    platform::RmtLoopbackResult busLoopback(const uint8_t* frame, size_t frameBytes,
                                            size_t dataBytes, uint8_t rowBits) override {
        return platform::parlioWs2812Loopback(owner_->laneList(), owner_->laneCount(),
                                              static_cast<uint16_t>(owner_->loopbackRxPin),
                                              frame, frameBytes, dataBytes, rowBits);
    }

private:
    platform::ParlioWs2812Handle parlio_;
};

// Register the Parlio backend into the peripheral registry once, at static-init. Gated by this header's
// CONFIG_SOC include in main.cpp (Parlio-capable silicon = the P4). No separate driver class — the one
// ParallelLedDriver drives it, chosen via the `peripheral` control.
inline const bool kParlioPeripheralRegistered =
    ParallelLedDriver::registerPeripheral("Parlio", []() -> LedPeripheral* { return new ParlioPeripheral(); });

} // namespace mm
