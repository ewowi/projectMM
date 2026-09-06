#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

// PolarNoise: a warped noise field addressed in polar coordinates, folded into a kaleidoscope.
//
// The look this reaches for is the one Stefan Petrick made recognisable in the LED world: not a
// texture scrolling past the panel, but a field that seems to turn and breathe inside it. Three
// power functions compose to get there, and the effect itself is mostly parameter choices:
//
//   - `PolarLut` addresses the grid by ANGLE and RADIUS instead of x and y, which is what makes
//     the motion rotate around the center rather than slide across it. The address is the same one
//     `atan16` and `dist16` compute, read from a table: it does not change between frames, and
//     computing it per pixel measured 39% of this effect's frame on an ESP32-S3.
//   - `warp8` displaces the sample coordinate by another noise field, so the field flows and
//     marbles instead of merely drifting (Quilez's domain warping).
//   - `kaleido` folds the angle into n mirrored wedges, turning the field into a mandala with a
//     single modulo — the symmetry is free because it happens before the field is ever sampled.
//
// Cost: `warp` is 2 noise samples plus its inner fbm, so at octaves=2 this is ~4 samples/pixel.
// The polar address is a table read rather than an `atan16` plus a `dist16`.
// That is a rich-field effect, appropriate on small and medium fixtures and on desktop; on a large
// wall drop `octaves` to 1 (or `warp` to 0) and it degrades to a plain polar noise that still
// reads well. The controls are deliberately the cost knobs, not just the look knobs.
//
// Prior art: Stefan Petrick's polar/noise effect vocabulary (a friend of projectMM) and Iñigo
// Quilez's domain-warping article. Implemented fresh in fixed point over our own noise.
// @card PolarNoiseEffect.png
/// Effect: a warped, kaleidoscopic noise field in polar coordinates.
class PolarNoiseEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🖌️🌫️🎡"; }   // power-function showcase
    Dim dimensions() const override { return Dim::D3; }  // volumetric: the field turns through depth

    uint8_t bpm      = 8;    // how fast the field drifts
    uint8_t scale    = 40;   // noise cells across the grid: low = broad shapes, high = fine detail
    uint8_t segments = 6;    // kaleidoscope wedges; 1 disables the fold
    uint8_t warp     = 90;   // domain-warp strength; 0 is a plain (unwarped) field
    uint8_t octaves  = 2;    // fbm octaves — the main cost knob
    uint8_t twist    = 30;   // how much the radius shears the angle, giving the field a spiral set

    /// The polar address: whether to read it from a table, at what precision, and how a
    /// volumetric fixture's coordinates become an angle and a radius (light/polar.h).
    PolarLut::Controls polar;

    void defineControls() override {
        controls_.addControl("bpm", bpm, 0, 60);
        controls_.addControl("scale", scale, 1, 255);
        controls_.addControl("segments", segments, 1, 16);
        controls_.addControl("warp", warp, 0, 255);
        controls_.addControl("octaves", octaves, 1, 4);
        controls_.addControl("twist", twist, 0, 255);
        // The address tradeoff, as controls because it is the user's to make: the table costs 2
        // bytes per pixel (4 when wide) and buys back the per-pixel atan16 and dist16.
        PolarLut::addControls(controls_, polar);
    }
    void prepare() override {
        // The polar address is built here, not in tick(): prepare() is where a module builds state
        // and where allocation is allowed, and it runs again on every resize and control change, so
        // the table is always current without the render path ever allocating.
        lut_.prepareFor(polar, width(), height(), EffectBase::depth());
    }


    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height(), dep = depth();

        // The drift is an oscillator: a sawtooth that only ever moves forward, which is what makes
        // the field breathe outward rather than rock back and forth.
        drift_.set(0, {.rate = bpm, .low = 0, .high = 65535, .phaseOffset = 0, .wave = Wave::Saw});
        drift_.advanceTo(elapsed());
        const uint32_t t = drift_.unitValue(0);

        // Build the address table if the grid changed; a rebuild is the only frame that pays for it.
        // If it cannot be allocated the effect still renders, computing the address per pixel.
        const bool table = lut_.ready();
        const int32_t cx = w / 2, cy = h / 2, cz = dep / 2;

        std::size_t i = 0;
        for (lengthType z = 0; z < dep; z++)
        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++, i++) {
                // The polar address, and the radius in the pixel units the field is scaled in.
                angle16 a;
                uint32_t r;
                // The axis the field is sampled along through the fixture. Zero at every light on a
                // panel, so the volumetric form reduces exactly to the flat one there.
                int32_t along;
                if (table) {
                    a = lut_.angle(i);
                    r = lut_.radiusPixels(i);
                    along = lut_.mapping() == PolarLut::Mapping::Spherical
                          ? static_cast<int32_t>(lut_.pitch(i) >> 6)
                          : static_cast<int32_t>(z) - cz;
                } else {
                    // The same address the table would have held, under the same mapping: a device
                    // too tight for the table must show the same composition, not a different one.
                    const auto m = PolarLut::mappingOf(polar);
                    const auto ad = PolarLut::addressOf(m, static_cast<int32_t>(x) - cx,
                                                        static_cast<int32_t>(y) - cy,
                                                        static_cast<int32_t>(z) - cz);
                    a = ad.angle;
                    r = ad.radius;
                    along = m == PolarLut::Mapping::Spherical ? static_cast<int32_t>(ad.pitch >> 6)
                                                              : static_cast<int32_t>(z) - cz;
                }

                // The twist shears the angle by the radius, which is what turns concentric rings
                // into spiral arms.
                a = static_cast<angle16>(a + (r * twist));

                // Fold into wedges BEFORE sampling, so the symmetry costs one modulo rather than a
                // second pass over the field.
                a = kaleido(a, segments);

                // Sample the field in (angle, radius) space: the angle drives one axis and the
                // radius the other, so the field wraps around the center. Time moves the radius
                // axis, which reads as the pattern breathing outward.
                const uint32_t fx = (static_cast<uint32_t>(a) >> 6) * scale / 16u;
                const uint32_t fy = (r * scale) + (t >> 6);
                const uint32_t fz = static_cast<uint32_t>(along * static_cast<int32_t>(scale));

                const uint8_t v = warp > 0 ? warp8(fx, fy, fz, static_cast<uint16_t>(warp) * 4, octaves)
                                           : fbm8(fx, fy, fz, octaves);

                const RGB c = colorFromPalette(*Palettes::active(), v, 255);
                draw::pixel(cv, {x, y, z}, c);
            }
        }
    }

private:
    PolarLut            lut_{*this};
    OscillatorBank<1>   drift_;
};

}  // namespace mm
