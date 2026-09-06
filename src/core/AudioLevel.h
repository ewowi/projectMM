#pragma once

#include "core/AudioFrame.h"

#include <cmath>     // log10 for the shared dB mapping
#include <cstddef>
#include <cstdint>

#include "core/math16.h"   // isqrt64 — the shared integer root

namespace mm {

// Shared magnitude → 0..255 mapping on a LOGARITHMIC (decibel) scale — used by
// BOTH the level path (this file) and the spectrum path (AudioBands.h) so a VU
// meter and a spectrum bar share one consistent scaling. dB = 20·log10(m) is
// mapped through a window [floorDb, floorDb+spanDb] → [0,255]:
//   - `noiseFloor` sets the window bottom: floorDb = 60 + noiseFloor/2 dB.
//   - `gain` is sensitivity, the INTUITIVE direction — HIGHER gain = NARROWER
//     window = a sound fills more of the range: spanDb = (255-gain)/4 + 4.
// Human hearing is logarithmic and FFT/RMS magnitudes span a huge range, so a
// linear map crushes the quiet or saturates the loud; this is the standard fix.
/// The display window in dB: where it starts and how wide it is. One home for the two knobs.
inline float windowFloorDb(uint16_t noiseFloor) { return 60.0f + static_cast<float>(noiseFloor) * 0.5f; }
inline float windowSpanDb(uint16_t gain)        { return static_cast<float>(255 - gain) * 0.25f + 4.0f; }

/// A value already in dB onto 0..255 through the window. The band path conditions in dB first
/// (AudioBands.h, BandConditioner) and then comes here, so the window means one thing everywhere.
inline uint8_t dbToByte(float db, uint16_t noiseFloor, uint16_t gain) {
    const float t = (db - windowFloorDb(noiseFloor)) / windowSpanDb(gain);
    if (t <= 0.0f) return 0;
    if (t >= 1.0f) return 255;
    return static_cast<uint8_t>(t * 255.0f);
}

inline uint8_t magToByte(float m, uint16_t noiseFloor, uint16_t gain) {
    if (m <= 1.0f) return 0;
    return dbToByte(20.0f * std::log10(m), noiseFloor, gain);
}

// DC-blocker: the standard one-pole/one-zero high-pass that removes the constant
// (DC) offset and sub-bass rumble from the sample stream before any analysis —
// y[n] = x[n] - x[n-1] + R·y[n-1]. R near 1 sets the cutoff: R = 0.99 ≈ 40 Hz at
// 22 kHz. State (the two delay registers) persists across blocks, so the filter
// is continuous frame to frame. Hot-path-trivial: one subtract + one multiply-add
// per sample, two floats of state, no allocation. Host-tested.
struct DcBlocker {
    float xPrev = 0.0f;   // x[n-1]
    float yPrev = 0.0f;   // y[n-1]

    void reset() { xPrev = 0.0f; yPrev = 0.0f; }

    // Filter `n` samples in place. R is the pole (0..1); higher = lower cutoff.
    void process(int32_t* samples, size_t n, float r = 0.99f) {
        if (!samples) return;
        for (size_t i = 0; i < n; i++) {
            const float x = static_cast<float>(samples[i]);
            const float y = x - xPrev + r * yPrev;
            xPrev = x;
            yPrev = y;
            // Clamp before narrowing: casting a float outside the int32 range to
            // int32_t is undefined behaviour. A settling transient (or a degenerate
            // r) can briefly push y past the bounds, so saturate first. The positive
            // bound is 2147483520.0f (2^31 - 128), NOT INT32_MAX: INT32_MAX
            // (2147483647) has no exact float and rounds UP to 2^31, which is itself
            // out of int32 range — clamping to it would still cast out of range.
            // 2147483520 is the largest float strictly below 2^31, so the cast is
            // always defined. -2^31 (INT32_MIN) is exactly representable, so it's fine.
            const float clamped = y < -2147483648.0f ? -2147483648.0f
                                : y >  2147483520.0f ?  2147483520.0f : y;
            samples[i] = static_cast<int32_t>(clamped);
        }
    }
};

// Sound-level (loudness) analysis for one block of I2S microphone samples — pure
// domain math, no platform header, so it is host-tested without an ESP32 (the
// platform owns only the I2S read that produces these samples; see platform.h
// audioMic*). The same host-testable shape as RmtSymbol.h / ParallelSlots.h.
//
// Two facts about an I2S MEMS microphone drive the math here, both straight from
// how the part behaves (e.g. the INMP441 datasheet), not from any tuning recipe:
//   - It carries a DC bias. The 24-bit sample stream sits on a large constant
//     offset, so a plain RMS is dominated by the bias, not the sound — a silent
//     room would read "loud". Subtract the block mean first.
//   - Its quietest output is hiss, not zero. A `noiseFloor` threshold treats any
//     level below it as silence so idle hiss doesn't twitch the LEDs; `gain` then
//     scales what's left.
//
// INMP441 sample format: 24-bit signed data left-justified in a 32-bit slot, so
// the magnitude lives in the top bits. We arithmetic-shift right by 8 to land the
// 24-bit value in an int32, then accumulate in 64-bit so a full block can't
// overflow.

// The 64-bit integer square root this RMS path needs lives in core/math16.h beside the other
// integer roots (isqrt), so there is one implementation rather than two.

// Analyse `n` samples into `frame.level` — the overall RMS loudness mapped
// through the same log/dB window the bands use (magToByte), so the VU meter and
// the spectrum share one scaling and the noiseFloor/gain knobs mean the same
// thing for both. Empty/null input yields zero (silence), never a crash.
//
/// The narrowest dynamic range a learned follower credits its input with, shared by the level and
/// the per-band conditioners so the two paths behave alike. It exists only to bound
/// `windowSpan / range`, which a collapsed follower would otherwise drive toward infinity.
///
/// Deliberately SMALL, because the silence gate is what keeps a quiet room quiet and this is not a
/// second mechanism for the same job. A large value flattens real music instead: at 12 dB a band
/// swinging 6 dB filled only half the display, which reads as "vivid bands, no dynamic range".
/// The gate can tell silence from a quiet passage, which a range clamp fundamentally cannot, so
/// the gate does that work and this stays out of the way.
inline constexpr float kConditionerMinRangeDb = 3.0f;

/// The level's own minimum, larger than a band's for the same reason its gate is lower: this
/// follows a whole block's RMS, which swings far less than any single band's peak. At the band
/// value the meter stretched that small natural variation to full scale and sat pinned at 255.
inline constexpr float kLevelMinRangeDb = 20.0f;

/// The manual level window's width at full `gain`. The level is scaled BY gain rather than sized
/// from it: `gain` sizes the band window directly, but a block RMS covers far more dB than a single
/// bin's peak, so feeding one raw number to both left the VU in the bottom third of the meter at
/// the settings that made the spectrum look right. Scaling keeps the knob meaning what it means
/// (higher gain = narrower window = hotter meter) in both paths. 20 dB is the room's measured
/// speech-to-quiet range on the bench parts.
inline constexpr float kLevelWindowSpanDb = 20.0f;

/// The manual level window's span for a given `gain`: the base at gain 255, widening to twice that
/// as gain falls to 0, so the control spans a useful range either side of its midpoint.
inline float levelWindowSpanDb(uint16_t gain) {
    return kLevelWindowSpanDb * (2.0f - static_cast<float>(gain) / 255.0f);
}

/// How far below the display window a level has to fall before it counts as silence rather than a
/// quiet passage. The window floor is what a manual setup shows as its lowest visible level, so
/// anything at it is audible; the margin is what separates "quiet" from "nothing at all".
inline constexpr float kMuteMarginDb = 20.0f;

/// The level's own floor and peak, learned the way BandConditioner learns a band's. With `levels`
/// automatic the display window is measured rather than dialed in, so the VU levels itself along
/// with the bands and the manual sliders are genuinely manual-only. Same followers and the same
/// minimum range as the band tables, so the two paths behave alike and cannot disagree.
struct LevelConditioner {
    static constexpr float kMinRangeDb = kLevelMinRangeDb;

    float floorDb = 0.0f;
    float peakDb = 0.0f;
    bool  primed = false;

    /// Learn from this block's RMS and return the dB window [floor, floor+span] to display it in.
    void observe(float db, uint32_t dtMs, float& windowFloor, float& windowSpan,
                 float floorRiseDbPerS = 1.0f, float peakReleaseDbPerS = 3.0f) {
        if (!primed) { floorDb = db; peakDb = db + kMinRangeDb; primed = true; }
        const float dt = static_cast<float>(dtMs) / 1000.0f;
        floorDb = db < floorDb ? db : floorDb + floorRiseDbPerS * dt;
        peakDb  = db > peakDb  ? db : peakDb - peakReleaseDbPerS * dt;
        if (peakDb < floorDb + kMinRangeDb) peakDb = floorDb + kMinRangeDb;
        windowFloor = floorDb;
        windowSpan  = peakDb - floorDb;
    }
};

inline void computeLevel(const int32_t* samples, size_t n,
                         uint16_t noiseFloor, uint16_t gain, AudioFrame& frame,
                         LevelConditioner* cond = nullptr, uint32_t dtMs = 23) {
    if (!samples || n == 0) {
        frame.level = 0;
        return;
    }

    // DC mean of the block. 64-bit sum: n * 2^23 fits easily.
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += (samples[i] >> 8);
    const int64_t mean = sum / static_cast<int64_t>(n);

    // RMS of the DC-removed signal.
    uint64_t sqSum = 0;
    for (size_t i = 0; i < n; i++) {
        const int64_t v = (samples[i] >> 8) - mean;
        sqSum += static_cast<uint64_t>(v * v);
    }
    const uint64_t meanSq = sqSum / static_cast<uint64_t>(n);
    const uint64_t rms = isqrt64(meanSq);

    // Automatic: the window is the level's own learned range, so a quiet room and a loud one both
    // fill the meter. Manual: the floor/gain sliders, exactly as before.
    if (cond) {
        const float db = rms <= 1 ? 0.0f : 20.0f * std::log10(static_cast<float>(rms));
        // Silence for the LEVEL is not the same number as silence for a band, and the caller has
        // already halved `floor` for that reason: a band gate reads a single bin's PEAK magnitude
        // while this reads the whole block's RMS, which for real music sits well below the
        // strongest bin. Gating both at the band threshold left the spectrum lively with the VU
        // pinned at zero (measured: flux 32-100 against level 0). The window floor is the level a
        // manual setup DISPLAYS, so it is audible by definition; silence is kMuteMarginDb below it.
        const float gateDb = windowFloorDb(noiseFloor) - kMuteMarginDb;
        if (db < gateDb) { frame.level = 0; return; }
        float wFloor = 0.0f, wSpan = 1.0f;
        cond->observe(db, dtMs, wFloor, wSpan);
        const float t = (db - wFloor) / wSpan;
        const float clamped = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        frame.level = static_cast<uint8_t>(clamped * 255.0f + 0.5f);
        return;
    }
    // Manual. `floor` positions the window and `gain` scales its width, both as they do for the
    // bands, but from the level's own base span (see levelWindowSpanDb).
    const float db = rms <= 1 ? 0.0f : 20.0f * std::log10(static_cast<float>(rms));
    const float wFloor = windowFloorDb(noiseFloor);
    const float t = (db - wFloor) / levelWindowSpanDb(gain);
    const float clamped = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    frame.level = rms <= 1 ? 0 : static_cast<uint8_t>(clamped * 255.0f + 0.5f);
}

} // namespace mm
