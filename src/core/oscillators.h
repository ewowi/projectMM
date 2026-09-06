#pragma once

#include "core/math16.h"          // BeatPhase, sin16, triwave16, angle16

#include <cstdint>

// An oscillator bank: N independent low-frequency oscillators advanced once per frame and read as
// often as an effect likes.
//
// Every animated quantity in a generative field is an oscillator: where a shape sits, how far a
// coordinate is displaced, how fast a layer rotates, how bright it is this instant. Written by
// hand that is a BeatPhase member, a waveform call and a range map per quantity, which is why
// effects that animate several things at once grow a row of near-identical members and a block of
// arithmetic in tick(). The bank is that pattern owned once: declare how many oscillators, set each
// one's rate, waveform and output range, advance the bank, then read.
//
// Two properties make a bank different from N loose oscillators, and both matter for the shaders
// this exists for. Advancing is ONE pass, so cost is per frame and not per pixel: a pixel loop
// reads values that are already computed, which is the whole point when a field samples the same
// oscillator 16,384 times. And phases are held together, so oscillators keep their relationships:
// two layers at the same rate a quarter-cycle apart stay a quarter-cycle apart for as long as the
// device runs, which is what makes a composition read as deliberate rather than as drift.
//
// Standard shapes only, all from math16: sine, triangle, sawtooth, square. Anything richer is a
// COMBINATION of oscillators (that is the vocabulary Stefan Petrick's shaders are written in), not
// a new waveform here.

namespace mm {

/// The shape an oscillator traces over its cycle.
enum class Wave : uint8_t {
    Sine,        ///< the default: smooth, no corners, the shape most motion wants
    Triangle,    ///< linear out and back; sharper turns than a sine at the same rate
    Saw,         ///< ramp up then jump back: rotation, scrolling, anything that only goes one way
    Square,      ///< low half the cycle, high the other: switching, strobing, hard alternation
};

/// One oscillator's settings. `rate` is BPM-like (cycles per minute), so 60 is one cycle a second
/// and 0 holds the phase still. `low`/`high` are the output range, inclusive, and may be inverted
/// (low > high) to run the shape backwards. `phaseOffset` is an angle16 added at read time, so two
/// oscillators can share a rate and sit a fixed fraction of a cycle apart: 16384 is a quarter turn.
struct Oscillator {
    uint16_t rate = 0;
    int32_t  low = 0;
    int32_t  high = 65535;
    angle16  phaseOffset = 0;
    Wave     wave = Wave::Sine;
};

/// A fixed-size bank of oscillators. `N` is a compile-time count because an effect knows how many
/// quantities it animates, and a fixed member array costs no allocation and no indirection.
///
/// Usage, once per frame: `bank.advanceTo(elapsed())`, then `bank.value(i)` wherever the value is
/// needed, including inside a pixel loop. Configure with `set(i, {...})` in prepare() or whenever a
/// control changes; a rate change takes effect from that frame without jumping the phase, which is
/// what live reconfiguration requires.
template <uint8_t N>
class OscillatorBank {
public:
    static constexpr uint8_t kCount = N;

    /// Configure oscillator `i`. Out-of-range indices are ignored rather than trapping: an effect
    /// driving the bank from a control must not be able to crash the device with a bad index.
    void set(uint8_t i, const Oscillator& osc) {
        if (i < N) osc_[i] = osc;
    }

    /// Read one oscillator's settings, for a caller that adjusts a single field.
    const Oscillator& get(uint8_t i) const { return osc_[i < N ? i : N - 1]; }

    /// Advance every oscillator to the current time. Call once per frame, before reading. Takes
    /// the CURRENT TIME rather than a frame delta (BeatPhase::advanceTo owns the why).
    /// Each phase accumulates its own dt*rate numerator, so a rate of 0 holds and a rate changed
    /// mid-run continues from where the phase stands (BeatPhase owns the why).
    void advanceTo(uint32_t nowMs) {
        for (uint8_t i = 0; i < N; i++) phase_[i].advanceTo(nowMs, osc_[i].rate);
    }

    /// Oscillator `i`'s current phase as an angle16, offset included. The raw cycle position, for a
    /// caller that wants to drive something other than the configured range.
    angle16 phase(uint8_t i) const {
        if (i >= N) return 0;
        return static_cast<angle16>(phase_[i].phase(65536) + osc_[i].phaseOffset);
    }

    /// Oscillator `i`'s current value, mapped into its configured range. Cheap enough to call per
    /// pixel, though an effect whose value is constant across the frame should hoist it.
    int32_t value(uint8_t i) const {
        if (i >= N) return 0;
        return map(osc_[i], unit(osc_[i].wave, phase(i)));
    }

    /// Oscillator `i`'s value as a 0..65535 unit position, ignoring the configured range. For a
    /// caller doing its own mapping, and the form `value` is built on.
    uint16_t unitValue(uint8_t i) const {
        if (i >= N) return 0;
        return unit(osc_[i].wave, phase(i));
    }

    /// Restart every phase at zero, keeping the configuration. For an effect whose composition must
    /// begin from a known state.
    void reset() {
        for (uint8_t i = 0; i < N; i++) phase_[i].reset();
    }

private:
    /// The waveform, as a 0..65535 position through the cycle.
    static uint16_t unit(Wave wave, angle16 theta) {
        switch (wave) {
            // sin16 is signed around zero; shift it into the unsigned range the mapping expects.
            case Wave::Sine:     return static_cast<uint16_t>(sin16(theta) + 32768);
            case Wave::Triangle: return triwave16(theta);
            case Wave::Saw:      return theta;
            case Wave::Square:   return theta < 32768 ? 0 : 65535;
        }
        return 0;
    }

    /// Map a unit position onto [low, high], inclusive at BOTH ends. Dividing by 65535 rather than
    /// shifting by 16 is the difference between an oscillator that reaches its stated maximum and
    /// one that stops one step short of it forever: an effect sweeping a hue to 255 would never
    /// arrive. 64-bit intermediate, because a range spanning the full int32 overflows a 32-bit
    /// product, and a caller mapping onto pixel coordinates times a scale gets there sooner than it
    /// looks.
    static int32_t map(const Oscillator& osc, uint16_t u) {
        const int64_t span = static_cast<int64_t>(osc.high) - osc.low;
        return static_cast<int32_t>(osc.low + (span * u) / 65535);
    }

    Oscillator osc_[N];
    BeatPhase  phase_[N];
};

}  // namespace mm
