#pragma once

#include "core/math16.h"              // hashInt, easeInOutQuad, BeatPhase
#include "light/effects/EffectBase.h"

namespace mm {

// Dissolve: two palette fields trade places pixel by pixel, in an order that looks random but is
// computed rather than stored.
//
// The point is what it does NOT need. A dissolve is normally done by shuffling a list of pixel
// indices — memory proportional to the fixture, and a shuffle whose result must then be kept in
// step across devices. Here each pixel asks `hashInt(x, y, generation)` for its own threshold and
// compares it against the transition's progress. No array, no shuffle, no per-pixel state, and two
// devices agree without exchanging anything because the hash is a pure function of position.
//
// That is the supersync property made visible: a stream RNG (`Random8`) advances per CALL, so a
// device that renders one extra frame or has a different light count diverges permanently.
// Position-addressed randomness cannot drift.
//
// `easeInOutQuad` shapes the progress so the transition starts gently, sweeps through the middle,
// and settles — a linear wipe reads as mechanical by comparison.
//
// Cost: one hash and one compare per pixel. No state, no allocation, nothing that scales with the
// fixture beyond the pixel count itself.
//
// Prior art: the classic dissolve transition; the position-addressed (rather than shuffled) form is
// the standard shader approach.
// @card DissolveEffect.png
/// Effect: two color fields trading places pixel by pixel, with no per-pixel state.
class DissolveEffect : public EffectBase {
public:
    const char* tags() const override { return "💫"; }   // power-function showcase
    Dim dimensions() const override { return Dim::D2; }  // writes the z=0 slice; extrude fills z

    uint8_t bpm      = 12;    // how fast one transition completes
    uint8_t spread   = 60;    // how much of the transition pixels are mid-flight (0 = a hard edge)
    bool    eased    = true;  // ease the progress instead of sweeping linearly
    bool    scatter  = true;  // random order; off gives a positional wipe instead

    void defineControls() override {
        controls_.addControl("bpm", bpm, 1, 120);
        controls_.addControl("spread", spread, 0, 255);
        controls_.addControl("eased", eased);
        controls_.addControl("scatter", scatter);
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();

        phase_.advanceTo(elapsed(), bpm);
        const uint32_t raw = phase_.phase(65536);

        // Each complete sweep is one "generation": the generation number seeds the hash, so every
        // pass dissolves in a DIFFERENT random order rather than repeating the same one.
        const uint32_t generation = raw >> 16;
        uint16_t progress = static_cast<uint16_t>(raw & 0xFFFF);
        if (eased) progress = easeInOutQuad(progress);

        // The two fields being traded. They are palette reads a generation apart, so the effect
        // cycles through the palette as it dissolves.
        const uint8_t hueA = static_cast<uint8_t>(generation * kHueStep);

        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++) {
                // Each pixel's own threshold. With scatter off the threshold comes from position
                // instead, which turns the same code into a diagonal wipe.
                const uint16_t threshold = scatter
                    ? hashInt(static_cast<uint32_t>(x), static_cast<uint32_t>(y), generation)
                    : static_cast<uint16_t>(((x + y) * 65535u) / (w + h > 0 ? w + h : 1));

                // Compare progress against the threshold. `spread` widens the comparison into a
                // ramp, so pixels cross over gradually instead of snapping.
                uint8_t mix;
                if (spread == 0) {
                    mix = progress > threshold ? 255 : 0;
                } else {
                    const int32_t band = static_cast<int32_t>(spread) * 128;
                    const int32_t d = static_cast<int32_t>(progress) - threshold;
                    mix = d <= -band ? 0 : (d >= band ? 255
                        : static_cast<uint8_t>(((d + band) * 255) / (2 * band)));
                }

                // Interpolate the KNOWN 40-step delta, not the difference of the truncated bytes:
                // when hueA and hueB straddle the 256 wrap, `hueB - hueA` is -216 rather than +40
                // and the transition runs backwards through the whole palette.
                const uint8_t idx = static_cast<uint8_t>(hueA + ((kHueStep * mix) >> 8));
                // Brightness dips where a pixel is mid-flight, so the transition reads as a
                // shimmer crossing the panel rather than a flat crossfade.
                const uint8_t bri = static_cast<uint8_t>(255 - (mix > 128 ? 255 - mix : mix) / 2);
                draw::pixel(cv, {x, y, 0}, colorFromPalette(*Palettes::active(), idx, bri));
            }
        }
    }

private:
    static constexpr int32_t kHueStep = 40;   // palette advance per generation

    BeatPhase phase_;
};

}  // namespace mm
