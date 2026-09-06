#pragma once

#include "core/math16.h"              // BeatPhase, hashInt
#include "light/effects/EffectBase.h"
#include "light/shader.h"

namespace mm {

// Truchet: a maze of interlocking arcs that never repeats, drawn without storing a single tile.
//
// This is the textbook 2D shader, and a better introduction to the form than the raymarcher: no 3D,
// no rays, no float. It is one function of (position, time) evaluated per pixel, and it shows the
// three moves that most shader effects are built out of.
//
// **1. Space folding.** `repeat` chops the plane into cells and hands back the position INSIDE the
// current cell. Suddenly the shader only has to know how to draw one tile — the plane takes care of
// there being hundreds. Nothing is stored, nothing is looped over; the coordinate does the work.
//
// **2. Per-cell variation from position alone.** `hashInt` on the cell index decides which way that
// tile is turned. It is a pure function, so tile (7, 3) is always the same tile without an array
// remembering it — and two devices rendering the same wall agree without exchanging anything.
//
// **3. Distance plus smoothstep.** Each tile is two quarter-circle arcs, drawn as the distance to a
// ring. `smoothstep` across that distance gives a soft, anti-aliased edge for free, which is what
// stops a curve from reading as a staircase on a coarse panel.
//
// The classic result (Truchet 1704, revived by Cyril Smith in 1987) is that randomly-rotated tiles
// with arcs at their edges join up into continuous winding paths across the whole surface. The
// pattern looks designed; nothing designed it.
//
// Cost: two SDF evaluations and a hash per pixel, all integer. Cheap enough for any target, which is
// the other half of why this is the representative example and the raymarcher is not.
//
// Prior art: Sébastien Truchet's tiling; the shader formulation follows the standard
// fract/hash/smoothstep idiom (Iñigo Quilez, Shadertoy convention).
// @card TruchetEffect.png
/// Effect: randomly-turned arc tiles that join into endless winding paths.
class TruchetEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🖌️"; }   // power-function showcase
    Dim dimensions() const override { return Dim::D2; }  // writes the z=0 slice; extrude fills z

    uint8_t bpm      = 6;    // how fast the pattern drifts
    uint8_t scale    = 3;    // tiles across the short side
    uint8_t thickness = 60;  // how fat the arcs are
    uint8_t softness = 40;   // edge softness: the anti-aliasing width
    uint8_t shuffle  = 0;    // reshuffles which way the tiles face
    bool    drift    = true; // slide the pattern instead of holding still

    void defineControls() override {
        controls_.addControl("bpm", bpm, 0, 60);
        controls_.addControl("scale", scale, 1, 16);
        controls_.addControl("thickness", thickness, 5, 200);
        controls_.addControl("softness", softness, 1, 200);
        controls_.addControl("shuffle", shuffle, 0, 255);
        controls_.addControl("drift", drift);
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        if (cv.dims.x < 1 || cv.dims.y < 1) return;

        phase_.advanceTo(elapsed(), bpm);
        const uint32_t t = phase_.phase(65536);

        // One cell is this many 16.16 units across. `scale` counts tiles over the short side, which
        // spans -1..1 in shader space, so the cell size is 2/scale.
        const int32_t cell = (2 * 65536) / (scale > 0 ? scale : 1);
        // Drifting moves the whole plane rather than the tiles, so paths slide through the frame
        // while staying connected.
        const int32_t driftX = drift ? static_cast<int32_t>(t / 4u) : 0;
        const int32_t driftY = drift ? static_cast<int32_t>(t / 7u) : 0;

        const int32_t arcR = cell / 2;                                    // arcs meet at cell edges
        const int32_t thick = (static_cast<int32_t>(thickness) * cell) / 1000;
        const int32_t soft = (static_cast<int32_t>(softness) * cell) / 2000;

        shader::each(cv, 0, [&](int32_t sx, int32_t sy, angle16) -> RGB {
            const int32_t wx = sx + driftX;
            const int32_t wy = sy + driftY;

            // Which cell, and where inside it. floorDiv so negative space tiles the same way as
            // positive — a plain `/` truncates toward zero and puts a seam through the origin.
            const int32_t cx = floorDiv(wx, cell);
            const int32_t cy = floorDiv(wy, cell);
            const int32_t lx = shader::repeat(wx, cell);
            const int32_t ly = shader::repeat(wy, cell);

            // The tile's orientation is a pure function of WHICH tile it is. Flip the diagonal and
            // the two arcs swap corners, which is the whole Truchet trick.
            const bool flip = (hashInt(static_cast<uint32_t>(cx + 32768),
                                       static_cast<uint32_t>(cy + 32768), 0, shuffle) & 1) != 0;
            const int32_t ax = flip ? -lx : lx;

            // Two quarter arcs, centred on opposite corners of the cell. Their edges meet the cell
            // boundary at the same two points whichever way the tile is turned, so neighbouring
            // tiles always connect — that is why the paths run unbroken across the whole grid.
            const int32_t half = cell / 2;
            const int32_t d1 = ringDistance(ax + half, ly + half, arcR);
            const int32_t d2 = ringDistance(ax - half, ly - half, arcR);
            const int32_t d = shader::opUnion(d1, d2) - thick;

            // smoothstep across the distance: inside the arc is lit, outside is dark, and the
            // transition is a soft ramp rather than a jagged edge.
            const frac16 cover = 65535 - shader::smoothstep(-soft, soft, d);
            if (cover == 0) return RGB{0, 0, 0};

            // Colour rides the cell index and time, so neighbouring paths differ and the whole
            // pattern cycles through the palette.
            const uint8_t idx = static_cast<uint8_t>(
                hashInt(static_cast<uint32_t>(cx + 32768), static_cast<uint32_t>(cy + 32768), 1, shuffle)
                + (t >> 8));
            return colorFromPalette(*Palettes::active(), idx, static_cast<uint8_t>(cover >> 8));
        });
    }

private:
    /// Distance to a ring of radius `r` centred at the origin — negative inside the ring's line,
    /// positive outside it. An arc is just this, clipped by the cell.
    static int32_t ringDistance(int32_t x, int32_t y, int32_t r) {
        const int32_t d = shader::length(static_cast<draw::pos_t>(x), static_cast<draw::pos_t>(y));
        return d - r < 0 ? r - d : d - r;
    }

    /// Divide flooring toward negative infinity, for a POSITIVE divisor. C++ truncates toward zero,
    /// which would make the cell index jump by one either side of the origin and put a visible seam
    /// through the middle. `b` is always a tile size here, so the positive-only form is the one
    /// needed; a negative `b` returns 0 rather than a plausible-looking wrong index.
    static int32_t floorDiv(int32_t a, int32_t b) {
        if (b <= 0) return 0;
        // Adjust the truncated quotient rather than negating `a`: -INT32_MIN has no int32
        // representation, so the negate-and-round form is undefined behaviour at the bottom of the
        // range — reachable from an unclamped coordinate.
        const int32_t q = a / b;
        return (a % b != 0 && a < 0) ? q - 1 : q;
    }

    BeatPhase phase_;
};

}  // namespace mm
