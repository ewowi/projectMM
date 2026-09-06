#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

// BeatRipples: every beat is a stone dropped in water.
//
// The surface is a real wave simulation, the classic two-buffer scheme (Gomez 2000, the water
// effect the demoscene settled on): each cell's next height is the average of its four neighbors
// doubled, minus its previous height, damped. That is the discrete wave equation, and it gives
// what a hand-drawn expanding circle cannot: ripples that pass THROUGH each other, reflect off the
// walls, and interfere into the standing patterns that make water look like water.
//
// A detected onset drops a stone. The loudest band decides where, so a bass hit lands near the
// center and a treble hit out at the rim, and the strength of the hit sets how deep the stone
// falls. Between beats the surface keeps ringing on its own, which is why this reads as water
// rather than as a flash.
//
// The height field is rendered by SLOPE, not by height: a surface is visible because it bends
// light, so the difference between neighboring cells is what lights a pixel. That is also what
// makes the crests read as bright lines rather than as blobs.
//
// Volumetric: `Layer::extrude` fills a cube with the plane, the same choice Particles and Wave
// make. The wave equation itself is 2D, and a 3D one is a different effect rather than a flag.
// @card BeatRipplesEffect.png
/// Effect: a wave surface where every detected beat drops a stone, rippling and interfering.
class BeatRipplesEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🎶🖌️"; }
    Dim dimensions() const override { return Dim::D2; }

    uint8_t damping   = 200;  // how long the water keeps ringing
    uint8_t drop      = 180;  // how deep a beat's stone falls
    uint8_t rain      = 30;   // idle drops when there is no music at all
    uint8_t shine     = 150;  // how strongly the slope lights the surface

    void defineControls() override {
        controls_.addControl("damping", damping, 0, 255);
        controls_.addControl("drop", drop, 0, 255);
        controls_.addControl("rain", rain, 0, 255);
        controls_.addControl("shine", shine, 0, 255);
    }

    void prepare() override {
        const lengthType w = width(), h = height();
        const size_t n = static_cast<size_t>(w) * h;
        cur_.resize(n); prev_.resize(n);
        if (cur_) std::memset(cur_.data(), 0, cur_.bytes());
        if (prev_) std::memset(prev_.data(), 0, prev_.bytes());
        onsetSeen_ = false;
        started_ = false;
        seq_ = 0;
        // The rain clock starts full, so the first drop lands on the opening frame. Starting at
        // zero leaves the pool empty for up to two seconds, which reads as a broken effect.
        carry_ = 2000;
    }

    void tick() MM_NONBLOCKING override {
        if (!cur_ || !prev_) return;
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();
        if (w < 3 || h < 3) return;
        const uint32_t now = elapsed();
        const uint32_t dt = started_ ? now - lastMs_ : 0u;
        lastMs_ = now;
        started_ = true;

        const AudioFrame* f = AudioService::latestFrame();
        const bool onsetNow = f && f->onset != 0;
        if (onsetNow && !onsetSeen_) {
            // Where the stone lands: the loudest band picks the radius, so bass falls near the
            // center and treble out at the rim. The angle walks so successive hits spread out.
            uint8_t loudest = 0, best = 0;
            for (uint8_t b = 0; b < 16; b++) if (f->bands[b] > best) { best = f->bands[b]; loudest = b; }
            const angle16 a = static_cast<angle16>(hashInt(seq_, 7) << 8);
            const int32_t maxR = (w < h ? w : h) / 2 - 2;
            const int32_t r = (maxR * (loudest + 1)) / 17;
            const lengthType sx = static_cast<lengthType>(w / 2 + (static_cast<int32_t>(cos16(a)) * r) / 32768);
            const lengthType sy = static_cast<lengthType>(h / 2 + (static_cast<int32_t>(sin16(a)) * r) / 32768);
            // How hard the beat hit scales the stone, so a loud onset makes a bigger wave. The
            // floor keeps a weak but real onset visible rather than silent.
            const int32_t hit = 96 + (static_cast<int32_t>(f->onset) * 159) / 255;
            splash(sx, sy, static_cast<int16_t>(-(static_cast<int32_t>(drop) * kSplashScale / 255) * hit / 255));
            seq_++;
        }
        onsetSeen_ = onsetNow;

        // Idle rain, so the surface is alive with no music. Time-paced, not per frame.
        if (rain > 0) {
            carry_ += dt;
            const uint32_t every = 2000u - static_cast<uint32_t>(rain) * 7u;
            if (carry_ >= every) {
                carry_ = 0;
                const lengthType sx = static_cast<lengthType>(hashInt(seq_, 11) % static_cast<uint32_t>(w));
                const lengthType sy = static_cast<lengthType>(hashInt(seq_, 13) % static_cast<uint32_t>(h));
                splash(sx, sy, static_cast<int16_t>(-(static_cast<int32_t>(rain) * kSplashScale) / 255));
                seq_++;
            }
        }

        // The wave equation runs on a FIXED timestep, not once per frame. A wave simulation's
        // speed IS its step count, so stepping per frame makes the water ring at the framerate:
        // measured 16x faster at 1200 fps than at 60. The accumulator gives the same physics on
        // any device, and the cap stops a long stall from spending a second catching up.
        stepCarry_ += dt;
        constexpr uint32_t kStepMs = 16;          // ~60 physics steps a second
        uint8_t steps = 0;
        while (stepCarry_ >= kStepMs && steps < 4) { stepCarry_ -= kStepMs; steps++; }
        if (stepCarry_ > kStepMs * 4) stepCarry_ = 0;
        for (uint8_t it = 0; it < steps; it++) waveStep();
        renderSurface(cv, w, h);
    }

    /// One step of the wave equation: next = (neighbors / 2) - previous, damped. The one line
    /// that makes ripples pass THROUGH each other rather than merely expand.
    void waveStep() {
        const lengthType w = width(), h = height();
        int16_t* c = cur_.data();
        int16_t* p = prev_.data();
        const int32_t keep = 224 + static_cast<int32_t>(damping) / 8;      // 224..255 of 256
        for (lengthType y = 1; y < h - 1; y++)
            for (lengthType x = 1; x < w - 1; x++) {
                const size_t o = static_cast<size_t>(y) * w + x;
                const int32_t sum = static_cast<int32_t>(c[o - 1]) + c[o + 1] + c[o - w] + c[o + w];
                int32_t v = (sum / 2) - static_cast<int32_t>(p[o]);
                v = (v * keep) / 256;
                p[o] = static_cast<int16_t>(v < -20000 ? -20000 : (v > 20000 ? 20000 : v));
            }
        // The two buffers exchange roles: `prev_` now holds the new surface.
        for (size_t i = 0, n = cur_.count(); i < n; i++) { const int16_t tmp = cur_[i]; cur_[i] = prev_[i]; prev_[i] = tmp; }
    }

    /// Render by SLOPE: a water surface is visible because it bends light, so the gradient
    /// between neighbors is what lights a pixel, not the height itself.
    void renderSurface(const draw::Canvas& cv, lengthType w, lengthType h) {
        const int16_t* s = cur_.data();
        for (lengthType y = 0; y < h; y++)
            for (lengthType x = 0; x < w; x++) {
                const size_t o = static_cast<size_t>(y) * w + x;
                const int32_t gx = (x > 0 && x < w - 1) ? static_cast<int32_t>(s[o + 1]) - s[o - 1] : 0;
                const int32_t gy = (y > 0 && y < h - 1) ? static_cast<int32_t>(s[o + w]) - s[o - w] : 0;
                int32_t mag = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
                mag = (mag * shine) / 512;
                const uint8_t bri = static_cast<uint8_t>(mag > 255 ? 255 : mag);
                // The palette index follows the HEIGHT, so a crest and a trough differ in color
                // while the slope decides how brightly either shows.
                const int32_t hgt = static_cast<int32_t>(s[o]);
                const uint8_t index = static_cast<uint8_t>(128 + (hgt > 4000 ? 127 : (hgt < -4000 ? -128 : (hgt * 127) / 4000)));
                draw::pixel(cv, {x, y, 0}, colorFromPalette(*Palettes::active(), index, bri));
            }
    }

private:
    /// The height field is int16 and rings up to +/-20000, so a stone is pressed in units of
    /// thousands. The 0..255 `drop` and `rain` knobs scale onto that range here: at 255 a beat
    /// presses the surface a third of the way to the clamp, which is deep enough to read as a
    /// wave and shallow enough that interfering ripples do not saturate.
    static constexpr int32_t kSplashScale = 6000;

    /// A stone: a small dish pressed into the surface, so the ripple starts as a real displacement
    /// rather than a single spike that the simulation would smear into noise.
    void splash(lengthType cx, lengthType cy, int16_t depth) {
        const lengthType w = width(), h = height();
        const lengthType rad = static_cast<lengthType>((w < h ? w : h) / 24 + 1);
        const int32_t r2 = static_cast<int32_t>(rad) * rad;
        for (lengthType dy = -rad; dy <= rad; dy++)
            for (lengthType dx = -rad; dx <= rad; dx++) {
                const int32_t q = static_cast<int32_t>(dx) * dx + static_cast<int32_t>(dy) * dy;
                if (q > r2) continue;
                const lengthType x = cx + dx, y = cy + dy;
                if (x < 1 || y < 1 || x >= w - 1 || y >= h - 1) continue;
                const int32_t fall = ((r2 - q) * 100) / (r2 > 0 ? r2 : 1);
                // Accumulated, not assigned: a stone landing on a live ripple adds to it. Writing
                // the dish flat would erase whatever wave was already passing through, which is
                // the one thing this simulation exists to show.
                const size_t o = static_cast<size_t>(y) * w + x;
                const int32_t v = static_cast<int32_t>(cur_[o]) + (depth * fall) / 100;
                cur_[o] = static_cast<int16_t>(v < -20000 ? -20000 : (v > 20000 ? 20000 : v));
            }
    }

    ScratchBuffer<int16_t> cur_{*this}, prev_{*this};
    bool     onsetSeen_ = false, started_ = false;
    uint32_t lastMs_ = 0, carry_ = 2000, seq_ = 0, stepCarry_ = 0;
};

}  // namespace mm
