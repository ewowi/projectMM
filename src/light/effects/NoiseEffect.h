#pragma once

#include "core/math16.h"            // BeatPhase: the shared BPM accumulator
#include "light/effects/EffectBase.h"

namespace mm {

// Noise: a gradient-noise field indexed straight into the palette, the plainest way to turn the
// field into light and the reference every other noise effect is a variation on.
//
// One control decides what MOVES, which is the whole character of the effect and was previously the
// difference between two separate effects:
//
//   drift  - the sample coordinates scroll, so the field slides across the fixture like weather.
//            Each axis scrolls at a slightly different rate, so it flows rather than translating
//            rigidly. On a volumetric fixture the third axis is the light's own z, so the slices
//            differ and the field has real depth.
//   morph  - the coordinates hold still and TIME is the third axis, so the field changes in place
//            without going anywhere. On a panel this is the classic plasma-like wash. On a
//            volumetric fixture there is no axis left for depth, so every slice is the same.
//
// Author: FastLED inoise field (Mark Kriegsman), and MoonLight's Noise2D for the morph form.
/// Effect: a gradient-noise field through the palette, drifting across the fixture or morphing in place.
/// @card NoiseEffect.gif
class NoiseEffect : public EffectBase {
public:
    const char* tags() const override { return "⚡️💫🌙🐙🌫️"; }  // FastLED + MoonLight lineage
    Dim dimensions() const override { return Dim::D3; }

    static constexpr const char* kMotionOptions[] = {"drift", "morph"};

    uint8_t motion = 0;   // 0 = drift (the field moves), 1 = morph (the field changes in place)
    uint8_t scale = 4;    // spatial frequency: lower is broader, higher is finer
    uint8_t bpm = 60;     // how fast it moves, in beats per minute

    void defineControls() override {
        controls_.addSelect("motion", motion, kMotionOptions, 2);
        controls_.addControl("scale", scale, 1, 255);
        controls_.addControl("bpm", bpm, 1, 255);
    }

    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        const lengthType w = width(), h = height(), d = depth();
        const uint8_t cpl = channelsPerLight();
        const nrOfLightsType count = nrOfLights();
        const nrOfLightsType wh = static_cast<nrOfLightsType>(w) * h;
        if (cpl == 0 || w == 0 || h == 0) return;

        // Accumulate the phase incrementally, so changing bpm does not jump the field. phase_ holds
        // the RAW numerator and the divide runs at the sampling point: dividing per tick truncates
        // sub-unit progress to zero on a fast board or a small grid, stalling the field. The rate
        // carries the grid width, so it is fed pre-scaled. BeatPhase owns the first-tick seed and
        // the divide-late rule.
        phase_.advanceScaled(elapsed(), static_cast<uint64_t>(bpm) * w * 64);
        const uint32_t t = phase_.phase(1);
        const uint8_t sc = scale ? scale : 1;

        for (nrOfLightsType i = 0; i < count; i++) {
            const nrOfLightsType rem = i % wh;
            const lengthType x = static_cast<lengthType>(rem % w);
            const lengthType y = static_cast<lengthType>(rem / w);
            const lengthType z = static_cast<lengthType>(i / wh);

            uint8_t n;
            if (motion == 1) {
                // Morph: space is still and TIME is the third axis, with no depth term, so the
                // field changes without traveling and a volumetric fixture shows the same field in
                // every slice. On a panel it is the classic plasma wash. Drift spends that axis on
                // depth instead, which is what separates the two.
                n = inoise8(static_cast<uint32_t>(x) * 256u / sc,
                            static_cast<uint32_t>(y) * 256u / sc, t);
            } else {
                // Drift: the coordinates scroll, each axis at its own rate so the field flows
                // instead of sliding rigidly. On a volumetric fixture z is the light's own depth,
                // so the slices differ; on a panel that term is constant and costs nothing.
                const uint32_t nx = (static_cast<uint32_t>(x) * 256u + t) / sc;
                const uint32_t ny = (static_cast<uint32_t>(y) * 256u + t / 3u) / sc;
                n = d > 1 ? inoise8(nx, ny, (static_cast<uint32_t>(z) * 256u + t / 5u) / sc)
                          : inoise8(nx, ny);
            }

            const RGB c = colorFromPalette(*Palettes::active(), n);
            const size_t offset = static_cast<size_t>(i) * cpl;
            if (cpl >= 1) buf[offset + 0] = c.r;
            if (cpl >= 2) buf[offset + 1] = c.g;
            if (cpl >= 3) buf[offset + 2] = c.b;
        }
    }

private:
    BeatPhase phase_;
    // The field itself (lattice hash, gradient dot products, quintic fade, bi- or trilinear blend)
    // is the shared inoise8 in core/noise.h: this effect scales coordinates into it and colors the
    // result through the palette.
};

} // namespace mm
