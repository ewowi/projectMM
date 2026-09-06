#pragma once

#include "core/math16.h"            // BeatPhase — the shared BPM accumulator
#include "light/effects/EffectBase.h"

namespace mm {

// A 3D color sine field: R, G, B each follow a sine along one axis (x, y, z) with a
// 120° phase offset between channels, so the box glows through shifting colors that
// scroll over time. True 3D — every axis drives a channel; on a 2D grid the z term is
// constant (Layer::extrude handles a lower-dim layer), so it reads as a 2D R/G wash.
//
// Integer-only: angles are uint8_t (256 = full turn), sin8() returns 0..255 with 128 at
// the zero crossing — already the spec's (sin+1)/2 mapped to a byte. The 120° / 240°
// channel offsets are 256/3 ≈ 85 / 170 in that angle unit. `amplitude` scales the byte
// (255 = full, DMX-aligned). No float, reusing the project sin8 LUT (cf. PlasmaEffect).
//
// Prior art: projectMM v1/v2 SineEffect (same 3D sine; those used float sinf and a
// KvStore brightness publish we don't carry).
// Author: MoonLight (Sinus, AI-generated) — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
/// Effect of a moving sine wave across the layer.
class SineEffect : public EffectBase {
public:
    const char* tags() const override { return "💫"; }
    Dim dimensions() const override { return Dim::D3; }

    uint8_t frequency = 1;     // spatial frequency (waves across the box), 1..20
    uint8_t amplitude = 255;   // peak brightness, 0..255 (255 = full)
    uint8_t bpm = 30;          // scroll speed (reshuffles per minute of the phase)

    void defineControls() override {
        controls_.addControl("frequency", frequency, 1, 20);
        controls_.addControl("amplitude", amplitude, 0, 255);
        controls_.addControl("bpm", bpm, 1, 255);
    }

    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        const lengthType w = width();
        const lengthType h = height();
        const lengthType d = depth();
        const uint8_t cpl = channelsPerLight();

        const uint32_t now = elapsed();
        // Shared accumulator: raw dt·rate in 64 bits, divided only at the read, so a sub-millisecond
        // frame does not round to zero and freeze the animation (mm::BeatPhase owns that rule now).
        phase_.advanceTo(now, bpm);
        // Accumulate dt*bpm and read the high bits as the scroll phase (uint8 angle) —
        // the same integer accumulator the other effects use so a sub-ms dt isn't lost.
        const uint8_t t = static_cast<uint8_t>(phase_.phase(256));

        for (lengthType z = 0; z < d; z++) {
            const uint8_t bz = chan(static_cast<uint8_t>(z), t, 170);   // B: z axis, +240°
            for (lengthType y = 0; y < h; y++) {
                const uint8_t gy = chan(static_cast<uint8_t>(y), t, 85);  // G: y axis, +120°
                uint8_t* row = buf
                    + (static_cast<size_t>(z) * static_cast<size_t>(h) + static_cast<size_t>(y))
                      * static_cast<size_t>(w) * cpl;
                for (lengthType x = 0; x < w; x++) {
                    const uint8_t rx = chan(static_cast<uint8_t>(x), t, 0);   // R: x axis
                    if (cpl >= 1) row[0] = rx;
                    if (cpl >= 2) row[1] = gy;
                    if (cpl >= 3) row[2] = bz;
                    row += cpl;
                }
            }
        }
    }

private:
    // One channel's value at axis coordinate `coord`: sin8 of (freq*coord + time +
    // phaseOffset), then scaled by amplitude. sin8 is already 0..255; scale by
    // amplitude/255 via the *x+1+(>>8))>>8 rounding idiom (cf. scale8 in color.h).
    uint8_t chan(uint8_t coord, uint8_t t, uint8_t phaseOffset) const {
        const uint8_t angle = static_cast<uint8_t>(coord * frequency + t + phaseOffset);
        const uint16_t s = sin8(angle);
        return static_cast<uint8_t>((s * amplitude + 255) >> 8);
    }

    BeatPhase phase_;
};

} // namespace mm
