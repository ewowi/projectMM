#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

// Tunnel: the demoscene classic — a texture mapped onto the inside of an infinite tube, so the
// viewer appears to fly down it forever.
//
// The trick is that nothing is 3D. For each pixel, its ANGLE around the center becomes one texture
// coordinate and the RECIPROCAL of its distance becomes the other. Because 1/r grows without bound
// as you approach the center, the texture compresses toward a vanishing point, and adding time to
// that coordinate pulls it toward the viewer. Perspective for the price of a divide.
//
// It is the polar vocabulary carried to its conclusion: `atan16` and `dist16` give the angle and
// radius, and the perspective is one reciprocal on top of them. (For the GATHER primitive —
// reading the grid itself as a texture — see EchoEffect, which feeds a transformed previous frame
// back through `sampleWrap`.)
//
// Cost: one divide, one atan and one noise sample per pixel. The divide is the expensive part on a
// chip without hardware division; `depth` sets how fine the wall texture is, not how much it costs.
//
// Prior art: the standard demoscene tunnel (angle + 1/r texture mapping), and Iñigo Quilez's
// write-ups of it. Implemented fresh in fixed point.
// @card TunnelEffect.png
/// Effect: a texture-mapped tunnel flying toward a vanishing point.
class TunnelEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🖌️🌫️🎡"; }   // power-function showcase
    Dim dimensions() const override { return Dim::D3; }  // volumetric: the wall recedes through depth

    uint8_t bpm      = 20;   // how fast the tunnel flies past
    uint8_t depth    = 60;   // texture scale along the tunnel: higher = finer rings.
                             // Shadows EffectBase::depth() (the fixture's), which is why the few
                             // uses of that one below are qualified.
    uint8_t twist    = 40;   // rotation per unit depth, so the tunnel corkscrews
    uint8_t segments = 1;    // kaleidoscope the wall; 1 leaves it plain
    uint8_t octaves  = 2;    // wall texture detail, and the cost knob
    bool    vignette = true; // darken toward the vanishing point so it reads as receding

    /// The polar address: whether to read it from a table, at what precision, and how a
    /// volumetric fixture's coordinates become an angle and a radius (light/polar.h).
    PolarLut::Controls polar;

    void defineControls() override {
        controls_.addControl("bpm", bpm, 0, 120);
        controls_.addControl("depth", depth, 1, 255);
        controls_.addControl("twist", twist, 0, 255);
        controls_.addControl("segments", segments, 1, 16);
        controls_.addControl("octaves", octaves, 1, 4);
        controls_.addControl("vignette", vignette);
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
        const lengthType w = width(), h = height(), dep = EffectBase::depth();

        phase_.advanceTo(elapsed(), bpm);
        const uint32_t t = phase_.phase(65536);

        const int32_t cx = w / 2, cy = h / 2, cz = dep / 2;

        // The polar address does not change between frames, so it is read from a table; if the
        // device cannot spare the memory the effect computes it per pixel and looks the same.
        const bool table = lut_.ready();

        std::size_t i = 0;
        for (lengthType z = 0; z < dep; z++)
        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++, i++) {
                uint32_t r;
                angle16 a;
                // How far along the tube this light sits. A volumetric fixture is a real tube, so
                // depth is literally distance down it: the wall texture scrolls past each slice at
                // its own offset rather than every slice showing the same ring.
                int32_t along;
                if (table) {
                    a = lut_.angle(i);
                    r = lut_.radiusPixels(i);
                    along = lut_.mapping() == PolarLut::Mapping::Spherical
                          ? static_cast<int32_t>(lut_.pitch(i) >> 6)
                          : static_cast<int32_t>(z) - cz;
                } else {
                    // The same address the table would have held, under the same mapping.
                    const auto m = PolarLut::mappingOf(polar);
                    const auto ad = PolarLut::addressOf(m, static_cast<int32_t>(x) - cx,
                                                        static_cast<int32_t>(y) - cy,
                                                        static_cast<int32_t>(z) - cz);
                    a = ad.angle;
                    r = ad.radius;
                    along = m == PolarLut::Mapping::Spherical ? static_cast<int32_t>(ad.pitch >> 6)
                                                              : static_cast<int32_t>(z) - cz;
                }

                // 1/r is the depth coordinate: distant wall (small r) compresses toward the center,
                // which is exactly the perspective foreshortening a real tunnel has. The +1 keeps
                // the pixel at the very center from dividing by zero.
                const uint32_t depthCoord = (static_cast<uint32_t>(depth) * 4096u) / (r + 1);

                // The wall corkscrews: rotating by depth means each ring is turned a little more
                // than the one behind it.
                a = static_cast<angle16>(a + ((depthCoord * twist) >> 6));
                a = kaleido(a, segments);

                // Sample the wall texture in (angle, depth) space, with time pulling the depth
                // coordinate toward the viewer.
                const uint32_t u = (static_cast<uint32_t>(a) >> 5);
                // Depth down the tube adds to the texture's own depth coordinate, so a light further
                // in shows wall that is further away. On a panel `along` is 0 and this is the flat
                // tunnel exactly.
                const uint32_t v = depthCoord + (t >> 5) + static_cast<uint32_t>(along * 256);
                const uint8_t tex = fbm8(u, v, octaves);

                // Vignette by distance so the center reads as far away rather than merely small.
                uint8_t bri = 255;
                if (vignette) {
                    const uint32_t maxR = static_cast<uint32_t>(cx > cy ? cx : cy) + 1;
                    const uint32_t rel = r > maxR ? 255u : (r * 255u) / maxR;
                    bri = static_cast<uint8_t>(rel < 20 ? 20 : rel);
                }

                draw::pixel(cv, {x, y, z}, colorFromPalette(*Palettes::active(), tex, bri));
            }
        }
    }

private:
    PolarLut  lut_{*this};
    BeatPhase phase_;
};

}  // namespace mm
