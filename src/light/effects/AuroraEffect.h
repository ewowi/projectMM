#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

// Aurora: several noise fields, each drifting on its own clock, read in polar coordinates and
// composited into light.
//
// This is the shader idiom rather than a picture of anything: no aurora is simulated. Layers of the
// same field, sampled at different scales and moved by different oscillators, are combined, and the
// interference between them is what reads as curtains of light folding through each other. Nothing
// here is specific to auroras, which is the point: it is the vocabulary this library exists to
// provide, and the effect is a composition in it.
//
// Four pieces, each a power function:
//
//   - `PolarLut` gives every pixel its angle and radius once, so the field is addressed around the
//     center rather than across the grid, and the address is not recomputed per frame.
//   - `OscillatorBank` drives the motion. Each layer has its own drift, its own breathing scale and
//     its own rotation, and because the bank holds their phases together the layers keep their
//     relationships instead of sliding into each other over an evening.
//   - `fbm8` over gradient noise is the field itself: octaves of noise, so each layer has both a
//     broad shape and fine structure on it.
//   - A contrast window then decides what is visible. This is what separates a shader from a blur:
//     most of the field is pushed to black and only the top of it lights, so the result reads as
//     distinct curtains rather than as an evenly cloudy panel.
//
// The layers are combined by taking the strongest at each pixel, and the pixel's color comes from
// WHICH layer won and how far it exceeded the window, read through the palette. So the palette
// controls the mood and the layers control the structure, which is the split that makes the effect
// worth handing someone: every palette gives a different aurora and none of them look wrong.
//
// Cost: one fbm per layer per pixel, so `layers` is the cost knob and `octaves` multiplies it. The
// polar address is a table read. Targets in performance.md.
// @card AuroraEffect.png
/// Effect: layered noise curtains in polar coordinates, each layer on its own oscillators.
class AuroraEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🖌️🌫️🎡"; }   // power-function showcase
    Dim dimensions() const override { return Dim::D3; }  // volumetric: the curtains have depth

    static constexpr uint8_t kMaxLayers = 4;

    uint8_t speed    = 30;   // master rate: every layer's motion scales from this
    uint8_t scale    = 40;   // noise cells across the grid: low = broad curtains, high = fine detail
    uint8_t layers   = 3;    // how many fields are composited, and the main cost knob
    uint8_t warp     = 60;   // how far the field displaces its own sample angle
    uint8_t twist    = 40;   // how much the radius shears the angle, giving the curtains their lean
    uint8_t segments = 1;    // kaleidoscope fold; 1 leaves the composition unfolded
    uint8_t contrast = 140;  // the visibility window: higher = fewer, sharper curtains
    uint8_t octaves  = 2;    // detail within each layer, multiplying the cost knob

    /// The polar address: whether to read it from a table, at what precision, and how a
    /// volumetric fixture's coordinates become an angle and a radius (light/polar.h).
    PolarLut::Controls polar;

    void defineControls() override {
        controls_.addControl("speed", speed, 0, 120);
        controls_.addControl("scale", scale, 1, 255);
        controls_.addControl("layers", layers, 1, kMaxLayers);
        controls_.addControl("warp", warp, 0, 255);
        controls_.addControl("twist", twist, 0, 255);
        controls_.addControl("segments", segments, 1, 16);
        controls_.addControl("contrast", contrast, 0, 255);
        controls_.addControl("octaves", octaves, 1, 4);
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
        const uint8_t n = layers < 1 ? 1 : (layers > kMaxLayers ? kMaxLayers : layers);

        // Each layer gets three oscillators: how far it has drifted, how its scale breathes, and how
        // far it has rotated. Rates are deliberately unequal and not multiples of each other, so the
        // layers drift in and out of alignment for a long time before repeating.
        for (uint8_t i = 0; i < kMaxLayers; i++) {
            const uint16_t rate = static_cast<uint16_t>(speed) * (7 + i * 5) / 10;
            bank_.set(static_cast<uint8_t>(i * 3 + 0),
                      {.rate = rate, .low = 0, .high = 65535, .phaseOffset = 0, .wave = Wave::Saw});
            bank_.set(static_cast<uint8_t>(i * 3 + 1),
                      {.rate = static_cast<uint16_t>(rate / 3), .low = 192, .high = 320,
                       .phaseOffset = static_cast<angle16>(i * 12000), .wave = Wave::Sine});
            bank_.set(static_cast<uint8_t>(i * 3 + 2),
                      {.rate = static_cast<uint16_t>(rate / 5), .low = -8192, .high = 8192,
                       .phaseOffset = static_cast<angle16>(i * 20000), .wave = Wave::Sine});
        }
        bank_.advanceTo(elapsed());

        const bool table = lut_.ready();
        const int32_t cx = w / 2, cy = h / 2, cz = dep / 2;

        // Hoist everything constant across the frame: the pixel loop should read, not compute.
        uint32_t drift[kMaxLayers];
        uint32_t layerScale[kMaxLayers];
        int32_t  rotate[kMaxLayers];
        for (uint8_t i = 0; i < n; i++) {
            drift[i]      = bank_.unitValue(static_cast<uint8_t>(i * 3 + 0));
            layerScale[i] = static_cast<uint32_t>(bank_.value(static_cast<uint8_t>(i * 3 + 1))) * scale / 256u;
            rotate[i]     = bank_.value(static_cast<uint8_t>(i * 3 + 2));
            if (layerScale[i] == 0) layerScale[i] = 1;
        }

        // The window sits at `contrast` of the way up the field's own range, which the previous
        // frame measured. The field drifts slowly, so last frame's range is this frame's to within a
        // step, and the alternative (a pass to measure, then a pass to draw) doubles the cost.
        // Anchored on the measured LOW, not the midpoint, so contrast 0 lights the whole field and
        // contrast 255 leaves only its peaks.
        const uint8_t lowEnd = floor_ < peak_ ? floor_ : static_cast<uint8_t>(peak_ > 8 ? peak_ - 8 : 0);
        const uint8_t threshold = static_cast<uint8_t>(
            lowEnd + (static_cast<uint32_t>(peak_ - lowEnd) * contrast) / 272u);
        uint8_t frameMax = 0, frameMin = 255;

        std::size_t idx = 0;
        for (lengthType z = 0; z < dep; z++)
        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++, idx++) {
                angle16 baseAngle;
                uint32_t r;
                // The depth coordinate the field samples along. Under cylindrical the address is
                // the same at every height, so this is what separates the slices; under the other
                // two the address already carries depth and this rides along with it.
                int32_t along;
                if (table) {
                    baseAngle = lut_.angle(idx);
                    r = lut_.radiusPixels(idx);
                    along = lut_.mapping() == PolarLut::Mapping::Spherical
                          ? static_cast<int32_t>(lut_.pitch(idx) >> 6)
                          : static_cast<int32_t>(z) - cz;
                } else {
                    // The same address the table would have held, under the same mapping.
                    const auto m = PolarLut::mappingOf(polar);
                    const auto ad = PolarLut::addressOf(m, static_cast<int32_t>(x) - cx,
                                                        static_cast<int32_t>(y) - cy,
                                                        static_cast<int32_t>(z) - cz);
                    baseAngle = ad.angle;
                    r = ad.radius;
                    along = m == PolarLut::Mapping::Spherical ? static_cast<int32_t>(ad.pitch >> 6)
                                                              : static_cast<int32_t>(z) - cz;
                }

                // The strongest layer at this pixel wins, and how far it exceeds the visibility
                // window is the brightness. Taking the maximum rather than a sum is what keeps the
                // curtains distinct: summing would average them into an even haze.
                uint8_t best = 0;
                uint8_t winner = 0;
                for (uint8_t i = 0; i < n; i++) {
                    // Each layer leans its own way and turns at its own rate.
                    angle16 a = static_cast<angle16>(baseAngle + (r * twist) + rotate[i]);
                    a = kaleido(a, segments);

                    const uint32_t fx = (static_cast<uint32_t>(a) >> 6) * layerScale[i] / 16u;
                    const uint32_t fy = (r * layerScale[i]) + (drift[i] >> 6) + i * 4096u;
                    // Depth is the field's third axis, so a volumetric fixture samples through the
                    // field rather than repeating one slice. On a panel `along` is 0 at every light
                    // and this reduces to the 2D sample exactly.
                    const uint32_t fz = static_cast<uint32_t>(along * static_cast<int32_t>(layerScale[i]));

                    // The field displaces its own sample angle, which is what makes a curtain fold
                    // over itself rather than merely sweep past.
                    const uint8_t v = warp > 0 ? warp8(fx, fy, fz, static_cast<uint16_t>(warp) * 4, octaves)
                                               : fbm8(fx, fy, fz, octaves);
                    if (v > best) { best = v; winner = i; }
                }

                // The visibility window: everything below the threshold is dark, and what is above
                // it is stretched back over the full range, so a small part of the field becomes the
                // whole of the light. This is the control that decides curtains against cloud.
                //
                // The window is placed against the field's OWN range, measured on the previous
                // frame, not against 0..255. A field of noise rarely reaches either end on a small
                // grid, so a fixed threshold left the brightest curtain at two thirds of full and no
                // setting could fix it. Tracking the range means `contrast` says what fraction of
                // the field lights, on any grid, at any octave count, in any palette.
                uint8_t bri = 0;
                if (best > threshold) {
                    const uint32_t span = peak_ > threshold ? peak_ - threshold : 1u;
                    const uint32_t over = static_cast<uint32_t>(best - threshold) * 255u / span;
                    bri = static_cast<uint8_t>(over > 255u ? 255u : over);
                }
                if (best > frameMax) frameMax = best;
                if (best < frameMin) frameMin = best;

                // Which layer won picks the region of the palette: the layers stay separable
                // colors rather than one averaged hue. Nothing else enters the index. An earlier
                // version added a term from the field's own value, which moved the hue as the
                // brightness moved and wrapped past the end of the palette, so a brightening
                // curtain jumped from one end of it to the other. Brightness belongs in the
                // brightness.
                const uint8_t index = static_cast<uint8_t>((winner * 255u) / n);
                draw::pixel(cv, {x, y, z}, colorFromPalette(*Palettes::active(), index, bri));
            }
        }

        // Carry this frame's range into the next one's window. Eased rather than assigned, so a
        // single bright frame cannot make the whole composition flinch.
        peak_  = static_cast<uint8_t>((peak_ * 7u + (frameMax < 32 ? 32 : frameMax)) / 8u);
        floor_ = static_cast<uint8_t>((floor_ * 7u + frameMin) / 8u);
    }

private:
    PolarLut                        lut_{*this};
    OscillatorBank<kMaxLayers * 3>  bank_;
    uint8_t                         peak_ = 200;   ///< the field's high water mark, eased per frame
    uint8_t                         floor_ = 40;   ///< and its low, so the window spans what is there
};

}  // namespace mm
