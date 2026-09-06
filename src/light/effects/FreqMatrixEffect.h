#pragma once

#include "core/math16.h"            // map32 — the shared, fencepost-safe range map
#include "light/effects/EffectBase.h"

namespace mm {

// Freq Matrix: a 1D vertical shift register driven by the dominant audio frequency. Every frame the
// whole column scrolls one pixel away from the source end and a single new pixel is painted at that
// end, its HUE derived from the music's major peak frequency (mapped through a tunable
// lowBin..highBin frequency window) and its BRIGHTNESS from the overall loudness. The result is a
// scrolling "waterfall" where pitch becomes color and loudness becomes intensity — speak or play a
// rising tone and a colored streak climbs the strip. Silence (or a sub-bass-only signal) paints
// black, so quiet rooms scroll dark.
//
// The column itself IS the shift register: each loop reads pixel y-1 into pixel y (from the far end
// back toward the source) and writes the freshly-computed color at y=0, so no separate history
// buffer is needed — the look is entirely in the Buffer's own scroll. As a D1 effect it writes only
// the x=0 column running along Y (the project's "1D runs along Y" contract, docs/architecture.md);
// Layer::extrude fans that single column across x (and z on a cube) on wider layers, so the same
// code renders a strip or tiles a panel.
//
// Prior art: WLED's "Freqmatrix" audio-reactive effect (Andrew Tuline / the WLED SR fork), carried
// into MoonLight (E_MoonModules / MoonModules). The shift-register scroll, the
// pixVal = level·fx·sensitivity/256 brightness, the 80 Hz / quarter-volume gate, the
// upperLimit = 80 + 42·highBin / lowerLimit = 80 + 3·lowBin frequency window, and the
// map(peakHz, lower, upper, 0, 255) hue index are reproduced here, written fresh on EffectBase + the
// shared draw/palette primitives. Reads AudioService::latestFrame() (null-safe via the static silence
// frame); safe on any target and grid size.
// Author: Andrew Tuline (WLED-SR) — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h
//
// Fidelity-scale note: WLED's volume threshold is `volumeSmth > 0.25` on a 0..~1 normalised volume,
// and its brightness divisor is 256.0 on that same 0..1 scale. projectMM's AudioFrame::level is a
// pre-scaled 0..255-ish integer (the RMS VU value), not a 0..1 float. The threshold is therefore
// reproduced as level > 64 (≈0.25·255) and the brightness math is done on the 0..255 level with a
// /2560 divisor so a full-scale level·fx·sensitivity lands near 255 — the same response curve on our
// integer level. These two scale conversions are the only deviations from the verbatim WLED math;
// every constant (80 Hz, 0.25, 42·highBin, 3·lowBin) is otherwise preserved.
/// Audio-reactive effect: scrolls the dominant frequency as a color column.
class FreqMatrixEffect : public EffectBase {
public:
    const char* tags() const override { return "🐙🎶"; }  // WLED origin · audio
    Dim dimensions() const override { return Dim::D1; }   // writes the x=0 column, runs along Y (1D)

    // Defaults from WLED Freqmatrix (speed=255, fx/intensity=128, lowBin/custom1=18,
    // highBin/custom2=48, sensitivity/custom3=30, audioSpeed off).
    uint8_t speed       = 255;   // scroll throttle: higher = the column scrolls more often
    uint8_t fx          = 128;   // brightness intensity scaler (WLED "intensity")
    uint8_t lowBin      = 18;    // lower edge of the frequency→hue window (WLED custom1)
    uint8_t highBin     = 48;    // upper edge of the frequency→hue window (WLED custom2)
    uint8_t sensitivity = 30;    // brightness sensitivity (WLED custom3, 10..100)
    bool    audioSpeed  = false; // when set, the audio level modulates the scroll rate

    void defineControls() override {
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("fx", fx, 0, 255);
        controls_.addControl("lowBin", lowBin, 0, 255);
        controls_.addControl("highBin", highBin, 0, 255);
        controls_.addControl("sensitivity", sensitivity, 10, 100);
        controls_.addControl("audioSpeed", audioSpeed);
    }

    void tick() MM_NONBLOCKING override {
        // D1: the scroll runs down the x=0 column; draw::scroll reads the length from the Canvas.

        const draw::Canvas cv = canvas();

        const AudioFrame* f = AudioService::latestFrame();
        if (!f) return;   // static silence frame is non-null in practice; guard for safety

        // --- Scroll throttle (WLED: secondHand = micros()/(256-speed)/500 % 16; act only when it
        // ticks). RECONSTRUCTED: WLED gates the whole effect on a micros-derived 0..15 "second hand"
        // changing, so a higher speed scrolls more often. We reproduce the same gate from elapsed():
        // the throttle period (in ms) is (256 - speed), so speed=255 scrolls almost every frame and a
        // low speed scrolls rarely. With audioSpeed, the louder the sound the shorter the period
        // (WLED's optional audio-driven speed), down to a 1 ms floor.
        uint32_t period = static_cast<uint32_t>(256 - speed);              // RECONSTRUCTED
        if (audioSpeed) {                                                  // RECONSTRUCTED
            const uint32_t lvl = f->levelSmoothed > 255 ? 255 : f->levelSmoothed;
            period = period > (lvl >> 2) ? period - (lvl >> 2) : 1;       // louder → faster scroll
        }
        if (period == 0) period = 1;
        const uint32_t now = elapsed();
        if (now - lastScrollMs_ < period) return;                         // not time to scroll yet
        lastScrollMs_ = now;

        // --- Brightness of the new pixel (WLED: pixVal = volumeSmth * intensity * sensitivity
        // / 256.0, clamped 255). WLED uses the SMOOTHED volume here (this is a flowing scroll, not a
        // transient meter), so read f->levelSmoothed. On our 0..255 integer level the divisor becomes
        // /2560 so a full-scale level·fx·sensitivity maps near 255 (see the fidelity-scale note above).
        const uint32_t level = f->levelSmoothed > 255 ? 255 : f->levelSmoothed;
        uint32_t pixVal = (level * fx * sensitivity) / 2560u;
        if (pixVal > 255) pixVal = 255;
        const uint8_t bri = static_cast<uint8_t>(pixVal);

        // --- Color of the new pixel. Black unless there is a real tone above 80 Hz and the (smoothed)
        // volume is above a quarter scale (WLED: peakHz > 80 && volumeSmth > 0.25). 0.25 on WLED's
        // 0..1 volume is reproduced as level > 64 on our 0..255 smoothed level (fidelity-scale note).
        RGB newColor{0, 0, 0};
        if (f->peakHz > 80 && level > 64) {
            // Frequency window → 0..255 hue index. WLED:
            //   upperLimit = 80 + 42*highBin; lowerLimit = 80 + 3*lowBin;
            //   i = (lowerLimit != upperLimit) ? map(peakHz, lowerLimit, upperLimit, 0, 255) : peakHz;
            //   i = abs(i) & 0xFF;
            const int upperLimit = 80 + 42 * static_cast<int>(highBin);
            const int lowerLimit = 80 + 3 * static_cast<int>(lowBin);
            int idx;
            if (lowerLimit != upperLimit)
                idx = map32(static_cast<int>(f->peakHz), lowerLimit, upperLimit, 0, 255);
            else
                idx = static_cast<int>(f->peakHz);
            if (idx < 0) idx = -idx;                     // WLED: abs(i)
            const uint8_t i = static_cast<uint8_t>(idx & 0xFF);
            newColor = colorFromPalette(*Palettes::active(), i, bri);
        }

        // --- Shift the column one pixel away from the source end (WLED: for i = SEGLEN-1 .. 1,
        // setPixelColor(i, getPixelColor(i-1))), then paint the new color at y=0. The effect writes
        // only x=0; Layer::extrude duplicates this column across x (and z) on wider layers.
        draw::scroll(cv, /*axis=*/1, 1);
        draw::pixel(cv, {0, 0, 0}, newColor);
    }

private:
    // depth>0 ? depth : 1 — the dims z-extent, so draw clipping/indexing is correct on a 3D layer.

    uint32_t lastScrollMs_ = 0;   // last elapsed() ms the column scrolled (throttle state)
};

} // namespace mm
