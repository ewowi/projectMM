#pragma once

#include "core/math16.h"              // BeatPhase, sin16 — the shared time base and oscillator
#include "light/effects/EffectBase.h"

namespace mm {

// SdfShapes: two shapes orbiting and melting into each other, drawn from signed distance fields
// rather than by rasterising outlines.
//
// The point of the effect is what ONE number buys. For each pixel it asks the distance to a circle
// and to a box, blends the two with `smin` so they flow together like mercury instead of merely
// overlapping, and then reads three different looks off that single distance:
//   - `coverage(d)` fills the shape with a soft, anti-aliased edge (no staircase on a curve),
//   - `|d| - width` is an outline, drawn without a second pass or a second algorithm,
//   - the distance itself indexes the palette, so the surrounding field glows outward.
// A rasteriser would need separate code for each of those; here they are three reads of `d`.
//
// It is also the family's dimension proof: `length(p) - r` is a circle on a matrix and a sphere in a
// volume from identical code, which is why the shapes are addressed in sub-pixel coordinates and the
// z axis simply collapses on a 2D layer.
//
// Cost, measured on an ESP32-S3 at 128×128 (2026-08-06): the squared-distance forms are ~14
// cycles/pixel and `sdBox` ~6, against 305–692 for the effects already shipping at that size — so
// the shapes are cheap and the palette lookup dominates. `blend` controls the melt radius; at 0 the
// shapes union hard, which is the classic "two circles" look.
//
// Prior art: Iñigo Quilez's 2D distance-function catalogue and his polynomial smooth-minimum
// (iquilezles.org). Implemented fresh in fixed point against those descriptions.
// @card SdfShapesEffect.png
/// Effect: two SDF shapes orbiting and melting together, with a soft edge and an outline.
class SdfShapesEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🖌️"; }   // power-function showcase
    Dim dimensions() const override { return Dim::D2; }  // writes the z=0 slice; extrude fills z

    uint8_t bpm = 12;         // orbit speed
    uint8_t radius = 60;      // circle radius, as a fraction of the short side (0..255)
    uint8_t boxSize = 45;     // box half-extent, same scale
    uint8_t blend = 90;       // smin radius: 0 = hard union, higher = a longer melt
    uint8_t outline = 0;      // 0 = filled; higher draws an outline of that width instead
    bool    glow = true;      // tint the surrounding field by distance

    void defineControls() override {
        controls_.addControl("bpm", bpm, 1, 120);
        controls_.addControl("radius", radius, 0, 255);
        controls_.addControl("boxSize", boxSize, 0, 255);
        controls_.addControl("blend", blend, 0, 255);
        controls_.addControl("outline", outline, 0, 64);
        controls_.addControl("glow", glow);
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();

        phase_.advanceTo(elapsed(), bpm);
        const angle16 t = static_cast<angle16>(phase_.phase(65536));

        // The short side sets the scale, so the composition looks the same on any aspect ratio.
        const lengthType shortSide = w < h ? w : h;
        const draw::pos_t scale = draw::toSub(shortSide);

        // Two orbits in antiphase: the shapes swing toward and past each other, so the melt happens
        // at the crossing rather than at a fixed point.
        const draw::pos_t cxA = midX(w) + oscillate(t, scale / 4);
        const draw::pos_t cyA = midY(h) + oscillate(static_cast<angle16>(t + 16384), scale / 6);
        const draw::pos_t cxB = midX(w) - oscillate(t, scale / 4);
        const draw::pos_t cyB = midY(h) - oscillate(static_cast<angle16>(t + 16384), scale / 6);

        const draw::pos_t r  = (scale * radius) / 1020;    // /1020 = /255/4: 255 is a quarter of the side
        const draw::pos_t bs = (scale * boxSize) / 1020;
        const int32_t k = static_cast<int32_t>((scale * blend) / 2040);
        const draw::pos_t outlineW = draw::toSub(1) * outline / 8;

        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++) {
                const draw::pos_t px = draw::toSub(x), py = draw::toSub(y);
                // Two true distances, melted into one field. The sqrt form is used here because the
                // effect wants a real distance for the outline width and the glow falloff; a plain
                // fill would use the cheaper squared form.
                const int32_t dCircle = draw::sdCircle(px, py, cxA, cyA, r);
                const int32_t dBox    = draw::sdBox(px, py, cxB, cyB, bs, bs);
                int32_t d = draw::smin(dCircle, dBox, k);

                // An outline is the same distance, folded: |d| - width is negative only in a band
                // around the edge.
                if (outlineW > 0) d = (d < 0 ? -d : d) - outlineW;

                const uint8_t cov = draw::coverage(d);
                // Write black rather than skipping: the Layer does not clear between frames, so a
                // pixel this effect never touches keeps whatever was there — the shape's own path
                // from earlier frames, or the previous effect's whole picture.
                if (cov == 0 && !glow) { draw::pixel(cv, {x, y, 0}, RGB{0, 0, 0}); continue; }

                // Palette index rides the distance, so the shape reads as a lit body with the field
                // falling away around it; time offsets the whole ramp so the colour drifts.
                const int32_t dPix = d >> draw::kSubShift;
                const uint8_t idx = static_cast<uint8_t>((t >> 8) + static_cast<uint8_t>(dPix * 4));
                const uint8_t bri = glow ? (cov > 0 ? cov : glowFalloff(dPix)) : cov;
                if (bri == 0) { draw::pixel(cv, {x, y, 0}, RGB{0, 0, 0}); continue; }

                const RGB c = colorFromPalette(*Palettes::active(), idx, bri);
                draw::pixel(cv, {x, y, 0}, c);
            }
        }
    }

private:
    /// A signed swing of ±`amp` around zero, from the shared 16-bit sine (-32767..32767).
    static draw::pos_t oscillate(angle16 a, draw::pos_t amp) {
        const int32_t s = static_cast<int32_t>(sin16(a));   // -32768..32767
        return static_cast<draw::pos_t>((static_cast<int64_t>(s) * amp) / 32768);
    }
    static draw::pos_t midX(lengthType w) { return draw::toSub(w) / 2; }
    static draw::pos_t midY(lengthType h) { return draw::toSub(h) / 2; }

    /// Brightness of the field OUTSIDE the shape: bright near the edge, dark further out. Integer
    /// reciprocal-ish falloff — cheap, and the shape of it matters more than its exactness.
    static uint8_t glowFalloff(int32_t dPixels) {
        if (dPixels <= 0) return 255;
        if (dPixels > 16) return 0;
        return static_cast<uint8_t>(255 / (1 + dPixels * dPixels / 2));
    }

    BeatPhase phase_;
};

}  // namespace mm
