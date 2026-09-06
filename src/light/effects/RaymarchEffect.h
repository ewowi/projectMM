#pragma once

#include "light/effects/EffectBase.h"   // pulls in platform_config.h via light_types.h

// Compiled only where the SoC declares a hardware FPU — see raymarch.h for why the float exception
// is bounded to a header rather than weakening the rule in place.
#if MM_HEAVY_COMPUTE

#include "core/math16.h"              // BeatPhase
#include "light/raymarch.h"
#include "light/shader.h"

namespace mm {

// Raymarch: a lit 3D scene of melting spheres above a floor.
//
// The effect is deliberately THIN, and that is the point of it. Every piece of machinery — the
// sphere-tracing loop, the distance primitives, the gradient normal, the camera, the lighting —
// lives in `raymarch.h` where any effect can use it. What remains here is the part that makes this
// effect this effect: a `scene()` describing two spheres and a floor, and a choice of colours.
//
// Writing a different 3D effect means writing a different `scene()`. That is the whole API.
//
// Cost is per PIXEL, not per chip. Measured: desktop 5.1 ms at 128x128 (177 fps); an ESP32-S3 at
// 4096 lights runs it in 1.64 ms and still holds 409 fps, about 0.4 us per pixel. `steps` caps how
// far each ray may search, so halving it roughly halves the cost.
//
// Prior art: Iñigo Quilez's distance-function and raymarching articles (iquilezles.org).
// @card RaymarchEffect.png
/// Effect: a raymarched 3D scene of melting spheres, lit by a normal derived from the field.
class RaymarchEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🖌️"; }   // power-function showcase
    Dim dimensions() const override { return Dim::D2; }  // writes the z=0 slice; extrude fills z

    uint8_t bpm      = 10;   // how fast the scene animates
    uint8_t steps    = 40;   // how far each ray may search: the quality/cost knob
    uint8_t blend    = 80;   // how much the two spheres melt into each other
    uint8_t cameraY  = 128;  // camera height above the floor
    bool    showFloor = true;

    void defineControls() override {
        controls_.addControl("bpm", bpm, 0, 60);
        controls_.addControl("steps", steps, 8, 96);
        controls_.addControl("blend", blend, 0, 255);
        controls_.addControl("cameraY", cameraY, 0, 255);
        controls_.addControl("showFloor", showFloor);
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        if (cv.dims.x < 1 || cv.dims.y < 1) return;

        phase_.advanceTo(elapsed(), bpm);

        // Each oscillator reads the phase at ITS OWN rate and wraps in its own 16-bit angle, so
        // every term is continuous across the wrap. Scaling one wrapped angle by 1.3 or 0.8 (the
        // obvious way to write this) snaps at every cycle boundary — measured: the x1.3 and x0.8
        // terms jumped 0.95 of full scale once per orbit, which reads as a hiccup in the motion.
        const auto wave = [this](uint32_t rate, uint16_t offset) {
            const angle16 a = static_cast<angle16>((phase_.phase(65536) * rate) / 100u + offset);
            return static_cast<float>(static_cast<int32_t>(sin16(a))) / 32768.0f;
        };

        // The two spheres orbit each other; `blend` sets how far apart they can be and still merge.
        a_ = {wave(100, 16384) * 1.2f, 0.3f + wave(130, 0) * 0.4f, wave(100, 0) * 1.2f};
        b_ = {-wave(80, 16384) * 1.1f, 0.5f + wave(110, 16384) * 0.3f, -wave(80, 0) * 1.1f};
        k_ = 0.05f + static_cast<float>(blend) / 255.0f * 1.2f;

        // The camera LOOKS AT the subject rather than staring level: a camera at height camY aimed
        // horizontally puts the spheres and the floor below the view centre, leaving the top of the
        // panel as empty sky (measured on a 32x32 preview: rows 0-16 entirely black).
        raymarch::Camera cam;
        cam.position = {0.0f, 0.4f + static_cast<float>(cameraY) / 255.0f * 2.5f, -3.0f};
        cam.target   = {0.0f, 0.2f, 0.0f};
        cam.focal    = 1.1f;

        const raymarch::Vec3 light{0.5f, 0.8f, -0.3f};

        shader::each(cv, 0, [&](int32_t sx, int32_t sy, angle16) -> RGB {
            const raymarch::Hit h = raymarch::march(
                [this](raymarch::Vec3 p) { return scene(p); },
                cam.position, cam.ray(sx, sy), steps);
            if (!h.hit) return RGB{0, 0, 0};
            // Depth tints the palette, so nearer surfaces read differently from far ones and the
            // scene gains a sense of space.
            const uint8_t idx = static_cast<uint8_t>(static_cast<int>(h.dist * 40.0f) & 0xFF);
            return colorFromPalette(*Palettes::active(), idx,
                                    raymarch::diffuse(h.normal, light));
        });
    }

private:
    /// The scene: the distance to the nearest surface. This function IS the world — change it and
    /// the picture changes, with no renderer edit.
    float scene(raymarch::Vec3 p) const {
        float d = raymarch::smin(raymarch::sdSphere(p, a_, 0.8f),
                                 raymarch::sdSphere(p, b_, 0.7f), k_);
        if (showFloor) d = raymarch::opUnion(d, raymarch::sdPlane(p, -1.0f));
        return d;
    }

    raymarch::Vec3 a_, b_;
    float k_ = 0.5f;
    BeatPhase phase_;
};

}  // namespace mm

#endif  // MM_HEAVY_COMPUTE
