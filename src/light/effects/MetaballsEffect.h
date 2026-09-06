#pragma once

#include "core/math16.h"            // BeatPhase — the shared BPM accumulator
#include "light/effects/EffectBase.h"

namespace mm {

// Author: projectMM original (metaballs)
/// Metaballs effect: smooth merging blobs via a scalar field.
/// @card MetaballsEffect.png
class MetaballsEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🦅"; }  // MoonLight origin · David Jupijn / Rising Step
    // Iterates y and x only; Layer::extrude fills z on 3D layers.
    Dim dimensions() const override { return Dim::D2; }

    uint8_t bpm = 30;
    uint8_t radius = 28;
    uint8_t count = 4;   // number of balls (1..MAX_BALLS); each follows its own sine path
    uint8_t hue_shift = 0;

    static constexpr uint8_t MAX_BALLS = 8;

    void defineControls() override {
        controls_.addControl("bpm", bpm, 1, 255);
        controls_.addControl("radius", radius, 4, 255);
        controls_.addControl("count", count, 1, MAX_BALLS);
        controls_.addControl("hue_shift", hue_shift, 0, 255);
    }

    // Class scope, not function-local: -Wfunction-effects flags ANY static local in a
    // nonblocking function, including a constexpr that needs no guard variable. Same
    // storage and value here, and these are per-effect constants anyway.
    // This effect's own orbit constants; the shared field kernel takes them as a parameter so each
    // effect keeps its distinct motion.
    static constexpr draw::BlobPath BLOB_PATHS[MAX_BALLS] = {
        {1,   0,  64}, {2,  30,  94}, {3,  60, 124}, {1, 120, 184},
        {2, 160,  16}, {3, 200, 210}, {1,  90, 150}, {2, 220,  40},
    };

    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();
        uint32_t now = elapsed();
        // Shared accumulator: raw dt·bpm in 64 bits, divided only at the read, so a sub-millisecond
        // frame does not round to zero and freeze the animation (mm::BeatPhase owns that rule now).
        phase_.advanceTo(now, bpm);
        const uint8_t t = static_cast<uint8_t>(phase_.phase(256));

        const uint8_t n = count < MAX_BALLS ? count : MAX_BALLS;
        int16_t bx[MAX_BALLS];
        int16_t by[MAX_BALLS];
        draw::blobCenters(BLOB_PATHS, n, t, w, h, bx, by);

        // Field strength: sum of r^2 / (d^2 + 1)
        int32_t r2 = static_cast<int32_t>(radius) * radius;

        for (lengthType y = 0; y < h; y++) {
            uint8_t* row = buf + static_cast<size_t>(y) * w * cpl;
            for (lengthType x = 0; x < w; x++) {
                const uint32_t field = draw::blobField(x, y, bx, by, n, r2);
                uint8_t bright = field > 255 ? 255 : static_cast<uint8_t>(field);
                uint8_t hue = static_cast<uint8_t>((field >> 1) + hue_shift);
                RGB c = colorFromPalette(*Palettes::active(), hue, bright);

                if (cpl >= 1) row[0] = c.r;
                if (cpl >= 2) row[1] = c.g;
                if (cpl >= 3) row[2] = c.b;
                row += cpl;
            }
        }
    }

private:
    // Numerator-only accumulator (units of dt*bpm). See tick() for why.
    BeatPhase phase_;
};

} // namespace mm
