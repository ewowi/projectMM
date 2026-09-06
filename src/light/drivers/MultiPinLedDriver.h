#pragma once

#include "light/drivers/ParallelLedDriver.h"   // shared driver body + LedPeripheral
#include "platform/platform.h"


namespace mm {

/// Output driver: parallel 8-or-16-lane WS2812B over the ESP-IDF [esp_lcd i80 bus](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/lcd/index.html)
/// — the parallel scale path on **all three i80-capable ESP32 families**. RMT gives a chip 4-8
/// channels; this gives 8-16 lanes for the wall time of one. The magic is that ESP-IDF exposes ONE
/// public i80 API (`esp_i80_new_i80_bus` / `esp_i80_panel_io_tx_color`) and routes it to whichever
/// peripheral the silicon has — so this single backend serves every chip:
///  - **ESP32-S3 / -P4 / -S31:** backed by the dedicated **LCD_CAM** peripheral.
///  - **classic ESP32:** backed by the **I2S peripheral in i80/LCD mode** (the classic has no LCD_CAM;
///    I2S-i80 is its only >8-lane route). IDF's own CMake picks the backend by chip; the two are
///    mutually exclusive per silicon, so `lanesAvailable()` reads whichever lane-count constant is
///    non-zero. Named for the **i80 bus** (the shared API), not a peripheral, since it isn't one
///    peripheral — same reason its sibling backend is named `Parlio` (its own API).
///
/// The shared body (slicing, the whole-frame async double-buffer DMA, the fused encode, the loopback
/// self-test, the `frameTime` KPI) lives in ParallelLedDriver; this backend adds only the i80-specific
/// pieces:
///  - The sacrificial WR (pixel clock) + DC GPIOs the i80 bus mandates even though WS2812 ignores
///    both, and the "exactly 8 or 16 pins" rule (the i80 layer rejects a partial bus). A sub-16 board
///    parks unused lanes + WR/DC on one spare GPIO (the ghost-pin trick).
///  - **The 3-slot-per-bit wire contract:** each WS2812 bit becomes three bus slots at 2.67 MHz
///    (slot = 375 ns): all-active-lanes HIGH, the data bits, then all LOW — so a `1` is HIGH 750 ns
///    and a `0` 375 ns, approximating RMT's 700/350. The slot is deliberately NOT the lineage's
///    ~416 ns: newer WS2812B revisions spec T0H max ≈ 380 ns, and a longer `0` on a direct 3.3 V line
///    gets misread as `1` (the strip washes white). One bus word per slot (bus bit L = the L-th pin);
///    unequal strands idle LOW once exhausted. Slot layout: ParallelSlots.h.
///  - Both silicon paths do **whole-frame chained DMA** (autonomous, CPU out of the timing loop), so the
///    classic I2S path is WiFi-underrun-immune by construction — it does NOT need the ISR-refilled
///    ring / large `nbDmaBuffer` cushion the raw-register I2S-clockless lineage requires.
///  - The platform::i80Ws2812* calls (ESP-IDF's esp_lcd i80 bus + GDMA).
///
/// Prior art: Adafruit's LCD_CAM discovery, hpwit's I2SClockless lineage (classic-ESP32 I2S parallel),
/// FastLED's S3 driver — architecture studied, never copied. We build on IDF's maintained esp_lcd i80
/// abstraction rather than tracing the raw-register I2S driver (*Industry standards, our own code*).
class I80Peripheral : public LedPeripheral {
public:
    // Data pins + loopback pin default to UNSET: they are user-soldered (the strand
    // runs to whatever GPIOs the user wired), so a hard-coded default would be a
    // guess that could drive a pin the user committed elsewhere — empty until set,
    // the driver idles meanwhile (the "default only when it cannot do harm" rule;
    // see lessons.md). The ESP32-S3 N16R8 Dev bench wiring is pins "1,2,4,5,6,7,8,9",
    // loopbackRxPin 12 (kept clear of the octal-PSRAM pins 26-37, USB 19/20, and
    // strapping pins) — set those again to reproduce the bench. (The orchestrator declares
    // pins="" and loopbackRxPin=0, so the empty default needs no code here.)

    /// WR (pixel clock) and DC: the IDF i80 bus *requires* both on real GPIOs
    /// (esp_i80_panel_io_i80.c: `wr_gpio_num >= 0 && dc_gpio_num >= 0`), yet the WS2812 strands
    /// ignore both — they are peripheral-fixed, not user-strand wiring, so a sensible overridable
    /// default cannot do harm (same class as the chip-fixed Ethernet pins). The data pins gate
    /// startup, so the bus stays idle until the user sets them regardless. (Dropping WR/DC entirely
    /// needs a direct-LCD_CAM backend that bypasses esp_lcd, hpwit-style — that is MoonI80Peripheral,
    /// backlogged as this increment's sibling.)
    ///
    /// **`clockPin` is ONE pin doing TWO jobs, and with a 74HCT595 expander the second one is
    /// load-bearing.** WR toggles once per bus word in hardware — which is exactly what a '595's
    /// SRCLK (shift clock) needs — so the same wire serves both: the i80 pixel clock IS the shift
    /// clock. That is the trick that makes the expander cost zero DMA bytes for its clock (hpwit
    /// routes `LCD_PCLK_IDX` straight to the register's clock pin for the same reason).
    ///
    /// Two consequences worth knowing before editing this pin:
    ///  - Every bus word clocks a bit INTO the shift register. That is why the LATCH must ride a
    ///    *data lane* (a bit in every bus word) rather than a second clock output — the peripheral
    ///    only gives us one — and why the latch's word position is so delicate (ParallelSlots.h).
    ///  - In shift mode this pin is wired to the physical '595 clock line on the expander board.
    ///    Changing it means re-wiring hardware, not just re-configuring.
    ///
    /// **Unset (-1) means "no pin", and what that costs depends on the chip.** Nothing on a WS2812
    /// strand reads WR or DC (bench-proven: 4096 lights over 16 lanes and 1440 through a '595 both
    /// render with neither line wired), so the only question is what the peripheral is given for
    /// the GPIO number it insists on.
    ///
    /// **Naming.** WR and DC are the i8080 bus's own signal names (WR = the write strobe that
    /// clocks each bus word, DC = the data/command select), which is what the datasheets and
    /// `esp_lcd` call them; `clockPin`/`dcPin` are the control names a user sees. Every message
    /// about them names both, as "clockPin (WR)" and "dcPin (DC)", so the UI and the datasheet
    /// can be read together.
    ///
    /// **WR can be unset on the classic ESP32; DC cannot, anywhere.** WR reaches its pad through the
    /// GPIO matrix, so the platform sinks an unset one onto an input-only pad and no usable GPIO is
    /// spent. DC is toggled in SOFTWARE by esp_lcd on every transfer, and that call on a pad with no
    /// output driver logs an error from a context where logging aborts, so DC always needs a real
    /// pin: 33 on the classic (free on every package here), 11 on the LCD_CAM chips. On the LCD_CAM
    /// chips (S3/P4/S31) both need a real pad,
    /// because an invalid number reaches the ROM's matrix routine and the P4's writes a quarter
    /// megabyte past the GPIO block (the S3 happens to ignore it, which is luck, not a contract);
    /// there the default is 10/11, free on the S3 these were chosen on, and MoonI80Peripheral is
    /// the way to spend no GPIO at all (owning the DMA below esp_lcd, it holds DC constant and
    /// routes WR only when a '595 needs it as SRCLK).
    ///
    /// A board that needs WR on a real pin (a '595's shift clock) sets it; the platform refuses a
    /// pin its package lacks or has wired to flash or PSRAM before the peripheral can touch it.
    /// That refusal is what turned the QuinLED Dig-Next-2's old default of 18/23 from a silent
    /// watchdog reset (the ESP32-PICO-V3-02 has no such pads) into a status naming the pin.
    /// `i2sLanes > 0` IS "this is the classic-ESP32 i80" (the two backends are mutually exclusive
    /// per silicon), the same discriminator dmaBudgetBytes() below keys on.
    int8_t clockPin = platform::i2sLanes > 0 ? -1 : 10;
    int8_t dcPin    = platform::i2sLanes > 0 ? 33 : 11;

    // --- LedPeripheral descriptors ---

    /// The number of i80 lanes this chip provides (0 = no i80 bus on this chip); the orchestrator's
    /// inert-on-wrong-chip guards key off it. Reads whichever backend the silicon has —
    /// `lcdLanes` (LCD_CAM, S3/P4/S31) or `i2sLanes` (I2S-i80, classic ESP32) — which are mutually
    /// exclusive per chip (at most one is non-zero), so the sum picks the right one.
    uint8_t lanesAvailable() const MM_NONBLOCKING override { return platform::lcdLanes + platform::i2sLanes; }
    bool powerOfTwoBus() const override { return true; }   // the BUS rounds to 8/16; the pin count is free

    /// Whole-frame DMA byte budget. On the classic ESP32 the i80 is the I2S peripheral: its DMA is
    /// INTERNAL-RAM only (no PSRAM) and it holds the whole frame (no streaming ring), so a frame larger
    /// than the free internal DMA block simply cannot allocate — and the failing esp_lcd path can busy-
    /// wait to a watchdog reset. reinit() pre-checks against this and idles with a clear status instead.
    /// Budget = the largest free internal block minus a fixed reserve for the bus descriptors + other
    /// allocations that land between this query and the alloc; sized for ONE frame, since busInit()
    /// downgrades the optional second (doubleBuffer) buffer on its own when only one fits. The classic
    /// path is bounded by construction, so it never returns 0 (which means "no bound"): when the block
    /// is at or under the reserve, it reports a small positive floor so the fit gate still rejects.
    /// On the LCD_CAM chips (S3/P4/S31) the DMA reaches PSRAM → 0 = no bound (the interface default). COLD PATH.
    size_t dmaBudgetBytes() const override {
        if constexpr (platform::i2sLanes > 0) {
            const size_t block = platform::maxInternalAllocBlock();
            constexpr size_t kReserve = 16 * 1024;   // descriptors + headroom for allocs after this query
            constexpr size_t kMinBudget = 1;         // never 0 on the bounded path (0 == "no bound")
            return block > kReserve ? block - kReserve : kMinBudget;
        } else {
            return 0;   // LCD_CAM (S3/P4/S31): PSRAM DMA, no whole-frame ceiling
        }
    }

    // The i80 loopback can't build a 1-lane private bus, so it rebuilds the FULL-WIDTH bus and
    // carries the pattern on lane 0 — the loopback frame must be encoded at the operational bus
    // width (16-bit for a 16-lane driver) to match. (Parlio can do a 1-lane unit, so its backend
    // sets false.)
    bool loopbackFullWidth() const override { return true; }
    /// The classic ESP32's esp_lcd-i80 backend IS the I2S peripheral; on the LCD_CAM chips (S3/P4/S31) it is LcdCam (shared
    /// with MoonI80, which is why the two conflict). Keys on the same i2sLanes flag lanesAvailable does.
    LedHwBlock hwBlock() const override {
        if constexpr (platform::i2sLanes > 0) return LedHwBlock::I2s;
        else return LedHwBlock::LcdCam;
    }
    /// The classic ESP32's i80 IS an I2S peripheral, and this bus always drives from instance 1,
    /// leaving instance 0 (the only one with a PDM converter) for audio. Ask the platform whether
    /// instance 1 is free now; on the LCD_CAM chips nothing is shared and this is a compile-time
    /// false.
    bool busContentionCleared() const override {
        if constexpr (platform::i2sLanes > 0) return platform::i80Ws2812SharedBusFree();
        else return false;
    }

    const char* initFailMsg() const override {
        // The backend's own reason when it has one (a peripheral another module holds); else the
        // generic line, naming the same peripheral the label does so the error and dropdown agree.
        if (const char* why = platform::i80Ws2812LastError()) return why;
        return (platform::i2sLanes > 0) ? "I2S-IDF: bus init failed, check pins / memory"
                                       : "LCD-IDF: bus init failed, check pins / memory";
    }

    /// Spare bus lanes (shift mode, when the board has fewer data pins than the bus is wide) park on
    /// WR: the peripheral already drives it and the board already wires it, so the lane is inert.
    /// (Overrides the interface default; this is the "ghost pin" the platform layer uses for the same
    /// reason.)
    uint16_t clockPinForBus() const override {
        return clockPin < 0 ? platform::kBusPinUnset : static_cast<uint16_t>(clockPin);
    }

    /// Bind the i80-specific bus controls: the sacrificial WR (clockPin) and DC pins
    /// the peripheral mandates.
    void addBusControls(ControlList& controls) override {
        controls.addPin("clockPin", clockPin);
        controls.addPin("dcPin", dcPin);
    }
    /// A clockPin or dcPin change triggers a bus rebuild via the prepare sweep.
    bool busControlTriggersBuild(const char* name) const override {
        return std::strcmp(name, "clockPin") == 0 || std::strcmp(name, "dcPin") == 0;
    }

    /// FATAL bus-pin check → routed to the ERROR path (idles the driver), unlike validateBusPins'
    /// per-lane WARNINGS. WR and DC on the SAME GPIO breaks the i80 bus outright (it needs two
    /// distinct control lines — the bus won't init), so it can't be a warn-and-run like a data-lane
    /// collision (which only corrupts that one lane). null = no fatal condition.
    const char* validateBusFatal() const override {
        // Unset (-1) is fine on the classic ESP32, where the platform sinks the line onto an
        // input-only pad (see the clockPin doc), and fatal on the LCD_CAM chips, where the number
        // would reach the ROM. The int8 -> uint16 cast in busInit is exactly why this is checked
        // here: an unguarded -1 becomes 65535 and slips IDF's own `>= 0` test.
        // WR may be unset on the classic ESP32 (the platform sinks it onto an input-only pad, which
        // the GPIO matrix drives harmlessly); DC may never be, on any chip, because esp_lcd toggles
        // it in software every frame and that call aborts on a pad with no output driver.
        if (platform::i2sLanes == 0 && clockPin < 0)
            return "clockPin (WR) is unset - the i80 bus needs a write-strobe GPIO on this chip";
        if (dcPin < 0) return "dcPin (DC) is unset - the i80 bus toggles it every frame, so it needs a real GPIO";
        if (clockPin >= 0 && clockPin == dcPin)
            return "clockPin (WR) and dcPin (DC) are the same GPIO - they must differ";
        // Neither may sit on a pin the chip wired to flash or PSRAM, nor on one this PACKAGE does
        // not have: routing a signal there corrupts the device or wedges its flash cache, and
        // both fail silently. The driver's own sweep covers the bus LANES, but WR only rides that
        // list when there are spare lanes to park it on and DC never does, so the pair is checked
        // here, where it lives.
        if (clockPin >= 0) {
            const auto cap = platform::gpioCapability(static_cast<uint8_t>(clockPin));
            if (!cap.validGpio) return "clockPin (WR) does not exist on this chip package - pick another pin";
            if (cap.reserved)   return "clockPin (WR) is wired to flash/PSRAM on this chip - pick another pin";
        }
        if (dcPin >= 0) {
            const auto cap = platform::gpioCapability(static_cast<uint8_t>(dcPin));
            if (!cap.validGpio) return "dcPin (DC) does not exist on this chip package - pick another pin";
            if (cap.reserved)   return "dcPin (DC) is wired to flash/PSRAM on this chip - pick another pin";
        }
        // The '595 latch is a BUS LANE, so it needs its own GPIO: sharing it with WR would make the
        // pixel clock double as the latch (the '595 would present a byte on every shift cycle), and
        // sharing it with DC would latch on the command phase. Both are fatal — the bus builds, but
        // the strands get garbage — so this is an error, not a warning. (Bench-found: WR defaults to
        // GPIO 10, which is the first pin a user reaches for when picking a latch.)
        if (owner_->pinExpanderMode() && owner_->latchPin >= 0) {
            if (owner_->latchPin == clockPin)
                return "latchPin is on clockPin (WR) — the latch needs its own GPIO";
            if (owner_->latchPin == dcPin)
                return "latchPin is on dcPin (DC) - the latch needs its own GPIO";
        }
        return nullptr;
    }

    /// Reject a data lane that collides with the WR (clockPin) or DC pin. The i80
    /// peripheral routes a distinct output signal to each of the 8 data lanes plus
    /// WR + DC via the GPIO matrix; IDF does NOT check that they differ, so a data
    /// pin equal to clockPin/dcPin gets two signals on one GPIO and that lane emits
    /// the clock/DC waveform instead of pixel data (silent corruption — the strip on
    /// that lane shows garbage). Fail loud + idle instead, same shape as the other
    /// parse errors.
    // Returns a WARNING string (not an error) if a data lane sits on clockPin (WR) or
    // dcPin: that lane emits the bus-control waveform instead of pixel data. It's a
    // warning because on a board that wires all 8/16 lanes but drives fewer strands,
    // parking WR/DC on an unused data pin is a valid choice — only a lane driving a
    // real strand shows garbage. The orchestrator routes this to setConfigWarn, the driver
    // keeps running. null when the WR/DC pins are clear of the data set.
    const char* validateBusPins(const uint16_t* lanes, uint8_t n) const override {
        for (uint8_t i = 0; i < n; i++) {
            // clockPin/dcPin are int8_t (-1 = unset); only a real GPIO can collide.
            if (clockPin >= 0 && lanes[i] == static_cast<uint16_t>(clockPin))
                return "a LED pin is on clockPin (WR) — that lane carries the clock, not pixels";
            if (dcPin >= 0 && lanes[i] == static_cast<uint16_t>(dcPin))
                return "a LED pin is on dcPin — that lane carries DC, not pixels";
        }
        return nullptr;
    }

    /// Create the i80 bus + its DMA buffer(s) sized for `frameBytes` on the current data lanes plus
    /// the WR/DC pins; `wantSecondBuffer` requests the async double-buffer's second frame buffer
    /// (allocated only if it fits — else single-buffer). Returns whether init succeeded.
    /// **LCD_CAM** is the one silicon path that can host the 74HCT595 expander: it reaches PSRAM (so the
    /// ×8 frame fits), it has no single-transfer cap, and its WR pixel-clock pin IS the shift clock a
    /// '595 needs. The classic ESP32 shares this backend but not that silicon path — its i80 is the I2S
    /// peripheral, whose DMA cannot read PSRAM at all, so a 154 KB frame has nowhere to live. Keying
    /// the flag on `platform::hasLcdCam` makes the refusal a compile-time property of the silicon
    /// rather than a runtime surprise, and the orchestrator then reports it as a config error instead
    /// of letting the bus die at init with "check pins / memory". `hasLcdCam`, not `lcdLanes`: the
    /// two agree on ESP32 (it is defined as `lcdLanes > 0` there) but not on the desktop, which
    /// declares the capability outright so the emulated build can exercise this path.
    bool supportsPinExpander() const override { return platform::hasLcdCam; }

    /// The bus pin list comes from the orchestrator: in shift mode it appends the latch to the data pins
    /// (the latch is a bus lane), so the peripheral drives it. busClockMultiplier() tells the platform
    /// how many bus words one WS2812 slot is shifted out over, so it can scale the pixel clock and the
    /// slot keeps its wire duration.
    bool busInit(size_t frameBytes, bool wantSecondBuffer) override {
        return platform::i80Ws2812Init(i80_, owner_->busPinList(), owner_->busPinCount(),
                                       clockPinForBus(),
                                       dcPin < 0 ? platform::kBusPinUnset : static_cast<uint16_t>(dcPin),
                                       frameBytes, wantSecondBuffer, owner_->busClockMultiplier());
    }
    /// DMA buffer `i` (0/1) the orchestrator encodes into; buffer 1 is null when the second
    /// buffer didn't fit (single-buffer mode). Both are the same size (busCapacity).
    uint8_t* busBuffer(uint8_t i) override        { return platform::i80Ws2812Buffer(i80_, i); }
    /// The per-buffer byte capacity (fixed at bus creation; both buffers equal).
    size_t   busCapacity() const override         { return platform::i80Ws2812BufferCapacity(i80_); }
    /// Kick off the autonomous transfer of the first `bytes` of DMA buffer `i`;
    /// returns whether it started.
    bool  busTransmit(uint8_t i, size_t bytes) override { return platform::i80Ws2812Transmit(i80_, i, bytes); }
    /// Block up to `ms` for buffer `i`'s in-flight transfer to complete.
    bool  busWait(uint8_t i, uint32_t ms) override      { return platform::i80Ws2812Wait(i80_, i, ms); }
    /// The most recent DMA transfer's wire time (µs) — the WS2812 output floor.
    uint32_t busLastTransmitUs() const override         { return platform::i80Ws2812LastTransmitUs(i80_); }
    /// Tear down the i80 bus and its DMA buffer.
    void     busDeinit() override                 { platform::i80Ws2812Deinit(i80_); }

    /// Run the loopback self-test. The i80 layer requires all 8 data GPIOs valid, so
    /// a 1-lane private bus is impossible; the loopback builds the full-width bus and
    /// carries the pattern on lane 0. Passes the WR/DC pins the init needs.
    platform::RmtLoopbackResult busLoopback(const uint8_t* frame, size_t frameBytes,
                                            size_t dataBytes, uint8_t rowBits) override {
        // The private bus is built from the orchestrator's bus pin list (which appends the latch in
        // shift mode — the latch is a bus lane) and at the shift-mode pclk, so the test transmits
        // exactly what the render loop does. In direct mode both reduce to today's behavior.
        return platform::i80Ws2812Loopback(owner_->busPinList(), owner_->busPinCount(),
                                           static_cast<uint16_t>(clockPin),
                                           static_cast<uint16_t>(dcPin),
                                           static_cast<uint16_t>(owner_->loopbackRxPin),
                                           frame, frameBytes, dataBytes, rowBits,
                                           owner_->busClockMultiplier());
    }

    /// Store WR/DC alongside the data pins, so a clockPin/dcPin edit rebuilds the
    /// bus too (not just a data-pin change).
    void recordBusPins() override { lastClockPin_ = clockPin; lastDcPin_ = dcPin; }
    /// Whether the live bus's WR/DC pins still match the current clockPin/dcPin (so
    /// the orchestrator can skip a rebuild).
    bool extraBusPinsCurrent() const override {
        return lastClockPin_ == clockPin && lastDcPin_ == dcPin;
    }

private:
    platform::I80Ws2812Handle i80_;
    int8_t lastClockPin_ = -1;
    int8_t lastDcPin_ = -1;
};

// Register the esp_lcd-i80 backend into the ParallelLedDriver peripheral registry once, at static-init.
// The label is what the `peripheral` Select shows. Gated by this header's own CONFIG_SOC include in
// main.cpp, so it only registers on i80-capable silicon. There is no separate driver class: the one
// registered ParallelLedDriver drives every backend, selected by the `peripheral` control.
//
// It names the PERIPHERAL, not the bus protocol. "i80" is the Intel 8080 bus shape esp_lcd speaks, and
// it matches nothing a user can look up: on the classic ESP32 this backend IS the I2S peripheral, on
// LCD_CAM chips it is the LCD peripheral (see hwBlock()). So the label follows the silicon — and says
// `-IDF` because this backend drives it through esp_lcd, against the MoonI80 backend's `-MM` (our
// own GDMA layer below esp_lcd). Peripheral + who drives it is the whole choice a user is making.
inline constexpr const char* kI80Label = (platform::i2sLanes > 0) ? "I2S-IDF" : "LCD-IDF";
inline const bool kI80PeripheralRegistered =
    ParallelLedDriver::registerPeripheral(kI80Label, []() -> LedPeripheral* { return new I80Peripheral(); });

} // namespace mm
