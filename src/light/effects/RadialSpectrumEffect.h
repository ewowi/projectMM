#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

// RadialSpectrum: the spectrum as ripples. Each band owns a sector around the center, mirrored
// left and right; sound is born at the center and travels outward, so the radius is TIME and a
// ring's length is that band's recent history. A radial spectrogram, the circular visualizer the
// music-video world settled on, and here also the diagnostic the bar analyzer is: every sector is
// one band, so a band that is stuck or pinned shows as a sector that never moves or never dims.
//
// No transport is involved. The effect keeps a short history of band frames (one entry per
// ring) and every light READS it: its angle picks the band, its radius picks the age. That
// is a LUT read and a table read per light, cheaper than drawing bars, and it is what makes the
// effect volumetric for free: under the spherical polar mapping a cube's radius is the distance
// from its center, so the ripples become expanding shells.
//
// `beat` adds the onset detector's hits as a white shockwave born at the center on every hit,
// traveling out with the ripples. `smooth` switches the source between the raw bands and the
// meter ballistic, which is the comparison a person tuning the audio path wants to see.
// @card RadialSpectrumEffect.png
/// Effect: the spectrum as ripples, one sector per band, radius as time; a shockwave per beat.
class RadialSpectrumEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🎶🖌️🎡"; }
    Dim dimensions() const override { return Dim::D3; }

    static constexpr uint16_t kMaxHistory = 128;   ///< rings of history, and the largest radius read

    uint8_t speed       = 85;    // how fast sound travels outward
    uint8_t persistence = 128;   // how far out a ripple stays visible
    bool    smooth      = false;  // read the meter ballistic rather than the raw bands
    bool    beat        = true;  // a white shockwave on every detected onset
    PolarLut::Controls polar;    // the address, and cylindrical / spherical / radial on a cube

    void defineControls() override {
        controls_.addControl("speed", speed, 5, 100);   // higher is faster, as everywhere else
        controls_.addControl("persistence", persistence, 0, 255);
        controls_.addControl("smooth", smooth);
        controls_.addControl("beat", beat);
        PolarLut::addControls(controls_, polar);
    }

    void prepare() override {
        lut_.prepareFor(polar, width(), height(), EffectBase::depth());
        std::memset(history_, 0, sizeof(history_));
        std::memset(beats_, 0, sizeof(beats_));
        head_ = 0;
        carry_ = 0;
        started_ = false;
        onsetSeen_ = false;
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height(), dep = depth();
        const uint32_t now = elapsed();
        const uint32_t dt = started_ ? now - lastMs_ : 0u;
        lastMs_ = now;
        started_ = true;

        const AudioFrame* f = AudioService::latestFrame();
        const uint8_t* src = f ? (smooth ? f->bandsSmoothed : f->bands) : nullptr;
        // A hit is one block of onset != 0 and ticks outrun blocks: edge-detect, and latch it
        // until the next ring is born so a hit between rings is not lost.
        const bool onsetNow = f && f->onset != 0;
        if (onsetNow && !onsetSeen_) pendingBeat_ = 255;
        onsetSeen_ = onsetNow;

        // Time-paced history: a ring every `ringMs` whatever the framerate, the remainder
        // carried. A stall births a burst of rings, capped so it cannot wipe the history.
        //
        // The control is a RATE, so turning it up speeds the ripples up, which is what every other
        // speed in the tree does. The period it drives is the inverse: 5 gives a slow 105 ms ring,
        // 100 a fast 10 ms one.
        const uint32_t ringMs = 110u - static_cast<uint32_t>(speed);
        carry_ += dt;
        uint8_t born = 0;
        while (carry_ >= ringMs && born < 8) {
            carry_ -= ringMs;
            head_ = static_cast<uint16_t>((head_ + 1) % kMaxHistory);
            if (src) std::memcpy(history_[head_], src, 16);
            else     std::memset(history_[head_], 0, 16);
            beats_[head_] = beat ? pendingBeat_ : 0;
            pendingBeat_ = 0;
            born++;
        }
        if (born > 0) carry_ %= ringMs;

        // The fade with age, as a per-ring keep fraction in 1/256ths: persistence 255 keeps all,
        // 0 shows only the newest ring. Precomputed per ring so the light loop is a lookup.
        // The keep fraction per ring, and the range is what makes the default work. A ripple has
        // to survive as many rings as the fixture's corner is far: 45 on a 64x64 panel. So the
        // scale is centered on that rather than reaching it only at the top, and the DEFAULT of 128
        // gives 61 rings, filling a panel with room to spare. 92%..99.6% per ring: the low end is
        // a tight halo around the center, the high end reaches the corner of a large wall.
        // (An earlier range started at 50% per ring, which died after 19 rings at ANY setting, so
        // the effect could never fill a panel however far the control was pushed.)
        // Rebuilt only when `persistence` moves: the table depends on nothing else, and at 128
        // entries per frame it was the effect's largest fixed cost after the light loop itself.
        if (persistence != fadeFor_) {
            const uint32_t keep = 236u + (static_cast<uint32_t>(persistence) * 19u) / 255u;
            uint32_t k = 256;
            for (uint16_t r = 0; r < kMaxHistory; r++) { fade_[r] = static_cast<uint8_t>(k > 255 ? 255 : k); k = (k * keep) >> 8; }
            fadeFor_ = persistence;
        }

        const bool table = lut_.ready();
        const int32_t cx = w / 2, cy = h / 2, cz = dep / 2;
        const auto m = PolarLut::mappingOf(polar);
        std::size_t idx = 0;
        for (lengthType z = 0; z < dep; z++)
            for (lengthType y = 0; y < h; y++)
                for (lengthType x = 0; x < w; x++, idx++) {
                    angle16 a; uint32_t r;
                    if (table) { a = lut_.angle(idx); r = lut_.radiusPixels(idx); }
                    else {
                        const auto ad = PolarLut::addressOf(m, static_cast<int32_t>(x) - cx,
                                                            static_cast<int32_t>(y) - cy,
                                                            static_cast<int32_t>(z) - cz);
                        a = ad.angle; r = ad.radius;
                    }
                    // The band from the angle, folded so left and right mirror: bass at the top
                    // and bottom, treble at the sides, the symmetric form the circular visualizer
                    // uses because a spectrum has no left and right of its own.
                    const uint32_t folded = a < 32768u ? a : 65535u - a;            // 0..32767
                    const uint8_t band = static_cast<uint8_t>((folded * 16u) >> 15);  // 0..15
                    // The age from the radius: ring r was born r steps ago.
                    if (r >= kMaxHistory) { draw::pixel(cv, {x, y, z}, RGB{0, 0, 0}); continue; }
                    const uint16_t slot = static_cast<uint16_t>((head_ + kMaxHistory - r) % kMaxHistory);
                    const uint32_t v = (static_cast<uint32_t>(history_[slot][band]) * fade_[r]) >> 8;
                    RGB c = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(band * 16u),
                                             static_cast<uint8_t>(v));
                    // The shockwave: a white ring on the beat, fading with the same age.
                    const uint32_t bw = (static_cast<uint32_t>(beats_[slot]) * fade_[r]) >> 8;
                    if (bw) {
                        c.r = static_cast<uint8_t>(c.r + bw > 255u ? 255u : c.r + bw);
                        c.g = static_cast<uint8_t>(c.g + bw > 255u ? 255u : c.g + bw);
                        c.b = static_cast<uint8_t>(c.b + bw > 255u ? 255u : c.b + bw);
                    }
                    draw::pixel(cv, {x, y, z}, c);
                }
    }

private:
    PolarLut lut_{*this};
    uint8_t  history_[kMaxHistory][16] = {};   ///< the rings: one band frame per step
    uint8_t  beats_[kMaxHistory] = {};         ///< the shockwave strength born with each ring
    uint8_t  fade_[kMaxHistory] = {};           ///< the keep fraction per ring of age
    uint8_t  fadeFor_ = 255;                    ///< the `persistence` fade_[] was built for
    uint16_t head_ = 0;                         ///< the newest ring
    uint32_t carry_ = 0;                        ///< time owed toward the next ring, in ms
    uint8_t  pendingBeat_ = 0;                  ///< a hit waiting for the next ring
    bool     started_ = false;
    bool     onsetSeen_ = false;
    uint32_t lastMs_ = 0;
};

}  // namespace mm
