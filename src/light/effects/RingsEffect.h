#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

// Expanding concentric rings from random centre points.
// Each ring grows continuously and respawns at a fresh random
// position once it leaves the visible area. Multiple rings overlap.
// (Renamed from RipplesEffect: the Ripples name now holds the MoonLight
// sine-wave water-surface port; this concentric-rings effect is Rings.)
// Author: projectMM original (concentric rings)
/// Effect of expanding concentric rings from random centres.
/// @card RingsEffect.gif
class RingsEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🦅🖌️🎡"; }  // MoonLight origin · David Jupijn / Rising Step
    // Iterates y and x only; Layer::extrude fills z on 3D layers.
    Dim dimensions() const override { return Dim::D2; }

    static constexpr uint8_t MAX_RIPPLES = 8;

    // Calm defaults: a couple of slow rings read as clean expanding circles; more/faster reads as
    // chaos. Raise count/speed in the UI for a busier field.
    uint8_t count = 2;
    uint8_t speed = 30;
    uint8_t thickness = 3;
    uint8_t hue_shift = 0;

    void defineControls() override {
        controls_.addControl("count", count, 1, 255);
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("thickness", thickness, 1, 255);
        controls_.addControl("hue_shift", hue_shift, 0, 255);
    }

    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();

        // Visible radius limit: a TRUE distance to the far corner (dist16), where the 8-bit form
        // approximated an octagon and saturated at 255 — so on a panel wider than ~255 lights the
        // limit stopped growing and the rings stalled short of the edge. Clamped to a byte because
        // the per-ripple radius state is 8-bit.
        // Kept WIDE, not clamped to a byte: on a panel whose far corner is more than 255 lights
        // away the ceiling stopped growing, so every ripple died before reaching the edge.
        const uint32_t maxR32 = dist16(static_cast<int32_t>(w), static_cast<int32_t>(h));
        const uint16_t maxR = static_cast<uint16_t>(maxR32 < 1 ? 1 : (maxR32 > 65535 ? 65535 : maxR32));

        if (!initialized_) {
            for (uint8_t i = 0; i < MAX_RIPPLES; i++) {
                spawn(i, w, h);
                // Stagger initial radii so ripples are spread across all sizes
                radius_[i] = static_cast<uint16_t>((i * maxR) / MAX_RIPPLES);
            }
            initialized_ = true;
        }

        uint32_t now = elapsed();
        uint32_t dt = now - lastElapsed_;
        lastElapsed_ = now;
        // growth in radius units per frame (scaled by speed control + dt)
        uint16_t growth = static_cast<uint16_t>((static_cast<uint32_t>(speed) * dt) >> 7);
        if (growth == 0) growth = 1;

        for (uint8_t i = 0; i < count && i < MAX_RIPPLES; i++) {
            uint32_t next = static_cast<uint32_t>(radius_[i]) + growth;
            if (next > maxR) {
                spawn(i, w, h);
            } else {
                radius_[i] = static_cast<uint16_t>(next);
            }
        }

        for (lengthType y = 0; y < h; y++) {
            uint8_t* row = buf + static_cast<size_t>(y) * static_cast<size_t>(w) * cpl;
            for (lengthType x = 0; x < w; x++) {
                uint16_t r_acc = 0, g_acc = 0, b_acc = 0;
                for (uint8_t i = 0; i < count && i < MAX_RIPPLES; i++) {
                    const int32_t dx = static_cast<int32_t>(x) - cx_[i];
                    const int32_t dy = static_cast<int32_t>(y) - cy_[i];
                    // Kept wide for the same reason as maxR: clamping the per-pixel distance to a
                    // byte made every light past 255 from a ripple's center read as exactly 255, so
                    // the ring never appeared out there at all.
                    //
                    // Computed rather than read from a PolarLut, unlike the other radial effects:
                    // this distance is from a RIPPLE's center, not the grid's, and each ripple moves
                    // and respawns. A table per ripple would cost N times the memory and a rebuild
                    // whenever one respawns, which is more than the dist16 it would save.
                    const uint32_t d = dist16(dx, dy);
                    int32_t diff = static_cast<int32_t>(d) - static_cast<int32_t>(radius_[i]);
                    if (diff < 0) diff = -diff;   // stays int32: narrowing truncated large distances
                    if (diff < thickness) {
                        // Brightness peaks at ring centre, falls off with distance from ring.
                        uint8_t falloff = static_cast<uint8_t>(((thickness - diff) * 255) / thickness);
                        // Older ripples (large radius) fade out.
                        uint8_t age_fade = static_cast<uint8_t>(255 - ((radius_[i] * 255u) / maxR));
                        uint8_t intensity = scale8(falloff, age_fade);
                        RGB c = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(hue_[i] + hue_shift), intensity);
                        r_acc = static_cast<uint16_t>(r_acc + c.r);
                        g_acc = static_cast<uint16_t>(g_acc + c.g);
                        b_acc = static_cast<uint16_t>(b_acc + c.b);
                    }
                }
                if (cpl >= 1) row[0] = r_acc > 255 ? 255 : static_cast<uint8_t>(r_acc);
                if (cpl >= 2) row[1] = g_acc > 255 ? 255 : static_cast<uint8_t>(g_acc);
                if (cpl >= 3) row[2] = b_acc > 255 ? 255 : static_cast<uint8_t>(b_acc);
                row += cpl;
            }
        }
    }

private:
    lengthType cx_[MAX_RIPPLES] = {};
    lengthType cy_[MAX_RIPPLES] = {};
    uint16_t radius_[MAX_RIPPLES] = {};   // wide: a large panel's far corner exceeds 255
    uint8_t hue_[MAX_RIPPLES] = {};
    bool initialized_ = false;
    uint32_t lastElapsed_ = 0;
    Random8 rng_{0xC0DECAFEu};   // the shared PRNG; rand8() adapts it to the call shape below
    uint8_t rand8() { return rng_.next8(); }

    void spawn(uint8_t i, lengthType w, lengthType h) {
        cx_[i] = static_cast<lengthType>((static_cast<uint16_t>(rand8()) * w) >> 8);
        cy_[i] = static_cast<lengthType>((static_cast<uint16_t>(rand8()) * h) >> 8);
        radius_[i] = 0;
        hue_[i] = rand8();
    }
};

} // namespace mm
