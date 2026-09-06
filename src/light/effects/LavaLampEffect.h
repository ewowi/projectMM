#pragma once

#include "core/math16.h"            // BeatPhase — the shared BPM accumulator
#include "light/effects/EffectBase.h"

namespace mm {

// Atmospheric lava-lamp: three slow blobs whose summed field is mapped
// through a black → red → orange → yellow → white palette.
// Distinct from MetaballsEffect (which is fast, HSV-colored).
// Author: projectMM original (metaball lava lamp)
/// Lava-lamp effect: slow rising/merging palette blobs.
/// @card LavaLampEffect.gif
class LavaLampEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🦅"; }  // MoonLight origin · David Jupijn / Rising Step
    // Iterates y and x only; Layer::extrude fills z on 3D layers.
    Dim dimensions() const override { return Dim::D2; }

    static constexpr uint8_t NUM_BLOBS = 3;

    uint8_t bpm = 8;
    uint8_t radius = 36;
    uint8_t intensity = 200;

    void defineControls() override {
        controls_.addControl("bpm", bpm, 1, 255);
        controls_.addControl("radius", radius, 8, 255);
        controls_.addControl("intensity", intensity, 1, 255);
    }

    // Class scope, not function-local: -Wfunction-effects flags ANY static local in a
    // nonblocking function, including a constexpr that needs no guard variable. Same
    // storage and value here, and these are per-effect constants anyway.
    // The orbit constants are this effect's own: they are what makes it read as a lava lamp rather
    // than as the other field effects, so they stay here and the shared kernel takes them.
    static constexpr draw::BlobPath BLOB_PATHS[NUM_BLOBS] = {
        {1,   0,  64},
        {2,  80, 200},
        {1, 160, 100},
    };

    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();

        // Shared accumulator: raw dt·bpm in 64 bits, divided only at the read, so a sub-millisecond
        // frame does not round to zero and freeze the animation (mm::BeatPhase owns that rule now).
        phase_.advanceTo(elapsed(), bpm);
        const uint8_t t = static_cast<uint8_t>(phase_.phase(256));

        int16_t bx[NUM_BLOBS] = {};
        int16_t by[NUM_BLOBS] = {};
        draw::blobCenters(BLOB_PATHS, NUM_BLOBS, t, w, h, bx, by);
        int32_t r2 = static_cast<int32_t>(radius) * radius;

        for (lengthType y = 0; y < h; y++) {
            uint8_t* row = buf + static_cast<size_t>(y) * static_cast<size_t>(w) * cpl;
            for (lengthType x = 0; x < w; x++) {
                const uint32_t field = draw::blobField(x, y, bx, by, NUM_BLOBS, r2);
                uint32_t scaled = (field * intensity) >> 8;
                uint8_t idx = scaled > 255 ? 255 : static_cast<uint8_t>(scaled);
                // The metaball field value (0 = between blobs, 255 = blob core) is the palette index,
                // so the lamp takes the active palette. Lava gives the classic molten look (its low
                // end is black, so the space between blobs stays dark); any palette recolors the blobs.
                const RGB c = colorFromPalette(*Palettes::active(), idx);
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
