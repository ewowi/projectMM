#pragma once

#include "core/math16.h"            // BeatPhase — the shared BPM accumulator
#include "light/effects/EffectBase.h"

namespace mm {

// Two interfering sine waves whose sum drives the hue — a flowing, moiré-like color
// field. The horizontal and vertical waves run at independent frequencies and slightly
// different time rates, so they beat against each other. 2D (Layer::extrude lifts it to
// 3D). Ported from WLED's "Distortion Waves".
//
// Integer-only: angles are uint8_t (256 = full turn), sin8() returns 0..255. The two
// sines are averaged into a hue byte. WLED runs the vertical wave's time ~1.3× the
// horizontal; we approximate 1.3 as (t*333)>>8 = t*1.301..., staying in integer math.
// The hue indexes the global active palette via colorFromPalette (WLED used an hsvToRgb sweep).
//
// Prior art: MoonLight E_WLED.h (the WLED port); projectMM v1/v2 DistortionWaves (those
// used float sinf — this is the integer-sin8 equivalent).
// Author: ldirko & blazoncek (WLED port) — https://editor.soulmatelights.com/gallery/1089-distorsion-waves , https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h
/// Interference effect: overlaid moving waves distorting the field.
class DistortionWavesEffect : public EffectBase {
public:
    const char* tags() const override { return "💫"; }  // MoonLight / WLED origin
    Dim dimensions() const override { return Dim::D2; }

    uint8_t freq_x = 3;   // horizontal wave frequency, 1..8
    uint8_t freq_y = 3;   // vertical wave frequency, 1..8
    uint8_t speed = 50;   // animation speed, 0..100 (0 = frozen)

    void defineControls() override {
        controls_.addControl("freq_x", freq_x, 1, 8);
        controls_.addControl("freq_y", freq_y, 1, 8);
        controls_.addControl("speed", speed, 0, 100);
    }

    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        const lengthType w = width();
        const lengthType h = height();
        const uint8_t cpl = channelsPerLight();

        // speed 0 freezes: BeatPhase still tracks the time base, so resuming does not jump by the
        // pause. Two scales are read from the ONE accumulator (see ty below) — the reason phase()
        // takes the scale at the read rather than baking it into the accumulate.
        phase_.advanceTo(elapsed(), speed);
        const uint8_t t = static_cast<uint8_t>(phase_.phase(256));
        // ty is the y-axis time phase, running ~1.3× t. Reading the SAME accumulator at a different
        // scale (not deriving it from the already-wrapped uint8 t) keeps it CONTINUOUS: computing ty
        // as (t*333)>>8 made ty jump by ~76 every time t wrapped 255→0 (~once a second), because 1.3
        // isn't an integer multiple of 256 — a visible shift. From the raw phase the wrap is seamless.
        const uint8_t ty = static_cast<uint8_t>(phase_.phase(333));   // ~1.3·t, continuous across wraps

        for (lengthType y = 0; y < h; y++) {
            const uint8_t sy = sin8(static_cast<uint8_t>(static_cast<uint8_t>(y) * freq_y + ty));
            uint8_t* row = buf + static_cast<size_t>(y) * static_cast<size_t>(w) * cpl;
            for (lengthType x = 0; x < w; x++) {
                const uint8_t sx = sin8(static_cast<uint8_t>(static_cast<uint8_t>(x) * freq_x + t));
                // Average the two sines (each 0..255) → a hue byte. The interference of
                // the two frequencies + the 1.3× time skew is what makes the pattern move.
                const uint8_t hue = static_cast<uint8_t>((static_cast<uint16_t>(sx) + sy) >> 1);
                const RGB c = colorFromPalette(*Palettes::active(), hue);
                if (cpl >= 1) row[0] = c.r;
                if (cpl >= 2) row[1] = c.g;
                if (cpl >= 3) row[2] = c.b;
                row += cpl;
            }
        }
    }

private:
    BeatPhase phase_;
};

} // namespace mm
