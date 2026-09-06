#pragma once

#include "core/math16.h"              // BeatPhase, sin16/cos16
#include "light/effects/EffectBase.h"
#include "light/shader.h"             // project, depthFade, rotate

namespace mm {

// VectorBalls: a rotating 3D object drawn as shaded spheres — the demoscene classic that gave the
// technique its name.
//
// It is the smallest complete demonstration of putting 3D on a panel, and every step is a shared
// power function rather than something this effect invented:
//
//   rotate the object   -> two `shader::rotate` calls, one per axis
//   put it on screen    -> `shader::project`, the one divide that IS perspective
//   sort back to front  -> a painter's-order pass, so near balls cover far ones
//   shade by distance   -> `shader::depthFade`, which is what gives the scene depth
//   draw                -> `draw::fillCircle`, sized by the same 1/z that placed it
//
// Painter's order matters more than it sounds. Without it the balls draw in arbitrary order and a
// far ball can paint over a near one, which reads as the object turning inside out. Sorting by
// depth costs one insertion sort over a handful of points and fixes it completely.
//
// The shape is a cube's eight corners plus its six face centres — enough that the rotation is
// legible, few enough that the sort is trivial. A different shape is a different point table; the
// machinery does not change.
//
// Cost: 14 points, each a rotate, a divide and a small disc. Trivial on any target — the cost is in
// the discs, so `size` is the knob.
//
// Prior art: the demoscene vector-ball effect (Amiga era); the projection and shading are the
// library's.
// @card VectorBallsEffect.png
/// Effect: a rotating 3D object of shaded spheres, drawn with real perspective.
class VectorBallsEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🖌️"; }   // power-function showcase
    Dim dimensions() const override { return Dim::D2; }  // writes the z=0 slice; extrude fills z

    uint8_t bpm      = 12;   // rotation speed
    uint8_t size     = 2;    // ball radius at the object's centre, in pixels
    uint8_t spread   = 90;   // how far apart the balls sit
    uint8_t distance = 200;  // how far the object is from the viewer
    bool    fade     = true; // dim the far balls, which is what reads as depth

    void defineControls() override {
        controls_.addControl("bpm", bpm, 0, 60);
        controls_.addControl("size", size, 1, 12);
        controls_.addControl("spread", spread, 20, 255);
        controls_.addControl("distance", distance, 40, 255);
        controls_.addControl("fade", fade);
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();

        phase_.advanceTo(elapsed(), bpm);
        const uint32_t t = phase_.phase(65536);
        const angle16 yaw   = static_cast<angle16>(t);
        const angle16 pitch = static_cast<angle16>(t * 2 / 3);   // a second axis, so it tumbles

        const int32_t s = static_cast<int32_t>(spread) * 256;
        const int32_t camZ = static_cast<int32_t>(distance) * 512;

        // Rotate and project every point, keeping depth so the draw can be sorted.
        struct Ball { int32_t sx, sy, z; uint8_t hue; };
        Ball balls[kPoints];
        uint8_t live = 0;
        for (uint8_t i = 0; i < kPoints; i++) {
            int32_t px = kShape[i][0] * s;
            int32_t py = kShape[i][1] * s;
            int32_t pz = kShape[i][2] * s;

            // Two rotations, both through the shared helper: yaw about y (x and z move), then
            // pitch about x (y and z move).
            shader::rotate(px, pz, yaw);
            shader::rotate(py, pz, pitch);

            int32_t sx, sy;
            if (!shader::project(px, py, pz + camZ, 65536, sx, sy)) continue;   // behind the viewer
            balls[live++] = {sx, sy, pz + camZ, static_cast<uint8_t>(i * (256 / kPoints))};
        }

        // Painter's order: draw far balls first so near ones cover them. Without this the object
        // reads as turning inside out. An insertion sort is right for a handful of points.
        for (uint8_t i = 1; i < live; i++) {
            const Ball key = balls[i];
            int8_t j = static_cast<int8_t>(i - 1);
            while (j >= 0 && balls[j].z < key.z) { balls[j + 1] = balls[j]; j--; }
            balls[j + 1] = key;
        }

        draw::fill(cv, RGB{0, 0, 0});

        const int32_t shortSide = (w < h ? w : h);
        for (uint8_t i = 0; i < live; i++) {
            // Shader space back to pixels: the short side spans -1..1, so a circle stays circular.
            const lengthType px = static_cast<lengthType>(
                w / 2 + (balls[i].sx * shortSide) / (2 * 65536));
            const lengthType py = static_cast<lengthType>(
                h / 2 + (balls[i].sy * shortSide) / (2 * 65536));

            // The ball shrinks with the same 1/z that placed it, so near ones are genuinely larger
            // rather than merely brighter. Scaled against the NEAR plane (camZ) so `size` means the
            // radius of a ball at the object's centre, in pixels — the earlier form multiplied by
            // 65536 against a divisor already reduced to ~155, giving a radius of 1260 that clamped
            // to a disc covering the whole grid.
            const int32_t r = (static_cast<int32_t>(size) * camZ) / (balls[i].z > 0 ? balls[i].z : 1);
            const lengthType maxR = static_cast<lengthType>(shortSide / 4);
            const lengthType radius = static_cast<lengthType>(r < 1 ? 1 : (r > maxR ? maxR : r));

            const uint8_t bri = fade ? shader::depthFade(balls[i].z, camZ * 3) : 255;
            if (bri == 0) continue;
            draw::fillCircle(cv, px, py, radius,
                             colorFromPalette(*Palettes::active(), balls[i].hue, bri));
        }
    }

private:
    static constexpr uint8_t kPoints = 14;

    /// A cube's eight corners plus its six face centres, in units of `spread`.
    static constexpr int8_t kShape[kPoints][3] = {
        {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
        {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1},
        { 0,  0, -2}, { 0,  0,  2}, { 0, -2,  0}, { 0,  2,  0}, {-2,  0,  0}, { 2,  0,  0},
    };


    BeatPhase phase_;
};

}  // namespace mm
