#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

// Author: projectMM original (rotating spiral)
/// Effect winding a lit spiral up a conical layout.
/// @card SpiralEffect.png
class SpiralEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🦅🖌️🎡"; }  // MoonLight origin · David Jupijn / Rising Step
    // Iterates y and x only; Layer::extrude fills z on 3D layers.
    Dim dimensions() const override { return Dim::D2; }

    uint8_t bpm = 40;
    uint8_t twist = 4;
    uint8_t hue_shift = 0;

    /// The polar address: whether to read it from a table, at what precision, and how a
    /// volumetric fixture's coordinates become an angle and a radius (light/polar.h).
    PolarLut::Controls polar;

    void defineControls() override {
        controls_.addControl("bpm", bpm, 1, 255);
        controls_.addControl("twist", twist, 1, 255);
        controls_.addControl("hue_shift", hue_shift, 0, 255);
        PolarLut::addControls(controls_, polar);
    }
    void prepare() override {
        // The polar address is built here, not in tick(): prepare() is where a module builds state
        // and where allocation is allowed, and it runs again on every resize and control change, so
        // the table is always current without the render path ever allocating.
        lut_.prepareFor(polar, width(), height(), depth());
    }


    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();

        uint32_t now = elapsed();
        // Shared accumulator: raw dt·rate in 64 bits, divided only at the read, so a sub-millisecond
        // frame does not round to zero and freeze the animation (mm::BeatPhase owns that rule now).
        phase_.advanceTo(now, bpm);
        // Accumulate the raw (dt * bpm) product; divide only at the read site.
        // Per-tick `dt*bpm*256/60000` rounds to 0 on desktop (dt ≈ 0..1ms) and
        // freezes the animation; see MetaballsEffect for the same fix.
        uint8_t t = static_cast<uint8_t>(phase_.phase(256));

        int16_t cx = static_cast<int16_t>(w >> 1);
        int16_t cy = static_cast<int16_t>(h >> 1);

        // The polar address is the same every frame, so it is read from a table; if the device
        // cannot spare the memory the effect computes it per pixel and looks the same.
        const bool table = lut_.ready();

        std::size_t i = 0;
        for (lengthType y = 0; y < h; y++) {
            int16_t dy = static_cast<int16_t>(y) - cy;
            uint8_t* row = buf + static_cast<size_t>(y) * static_cast<size_t>(w) * cpl;
            for (lengthType x = 0; x < w; x++, i++) {
                int16_t dx = static_cast<int16_t>(x) - cx;
                // Angle and radius, both taken down to 8 bits here because hue is mod-256 by design.
                // A true radius rather than the octagon an 8-bit distance approximates, which showed
                // as visible corners on a large panel.
                uint8_t angle, dist;
                if (table) {
                    angle = static_cast<uint8_t>(lut_.angle(i) >> 8);
                    dist  = static_cast<uint8_t>(lut_.radiusPixels(i));
                } else {
                    angle = static_cast<uint8_t>(atan16(dy, dx) >> 8);
                    dist  = static_cast<uint8_t>(dist16(dx, dy));
                }
                uint8_t hue = static_cast<uint8_t>(
                    angle + static_cast<uint8_t>(dist * twist) - t + hue_shift);
                RGB c = colorFromPalette(*Palettes::active(), hue);

                if (cpl >= 1) row[0] = c.r;
                if (cpl >= 2) row[1] = c.g;
                if (cpl >= 3) row[2] = c.b;
                row += cpl;
            }
        }
    }

private:
    PolarLut lut_{*this};
    // Numerator-only accumulator (units of dt*bpm). See tick() for why.
    BeatPhase phase_;
};

} // namespace mm
