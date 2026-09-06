#pragma once

#include "light/effects/EffectBase.h"   // and core/noise.h with it: fbm8, curl16


namespace mm {

// Nebula: a cloud that is generated in one place and carried in another.
//
// The composition phase 4 exists to demonstrate, and the first effect built from two power
// functions rather than one. A noise field decides WHERE light is born: thresholded hard, so only
// the top of the field survives and the rest is black. A curl flow decides where it GOES. Neither
// half is new; what is new is that the emitter is a field rather than a handful of dots, so the
// light enters everywhere at once and the flow shapes a whole cloud instead of drawing trails.
//
// That is why it reads as a nebula rather than as either of its parts: the birth is structured (the
// field's own shape) and the transport is divergence-free (curl noise, so nothing piles up), and
// between them the cloud keeps folding into itself without ever collapsing or thinning out.
//
// **Two cost levers, because this is the effect that needs them.** The field is the expensive half,
// so `fieldScale` computes it at half or quarter resolution and stretches it (measured 3x and 6.6x
// on a curl field; `draw::upscale16` carries the numbers), and `fieldRate` recomputes it only every
// N frames. Both leave the MOTION alone: the flow still carries the plane every frame and the
// oscillators still advance, so a cheaper field costs detail rather than smoothness.
//
// Cost: the flow is one curl per light per frame (4 noise samples), the field is one fbm per light
// at whatever scale and rate the levers ask for. Targets in performance.md.
// @card NebulaEffect.png
/// Effect: a noise field births light, a curl flow carries it, and the two make a folding cloud.
class NebulaEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🖌️💨🌫️"; }   // power-function showcase
    Dim dimensions() const override { return Dim::D3; }   // the flow and the field both carry z

    uint8_t speed       = 40;   // how fast the medium moves, and with it the whole cloud
    uint8_t scale       = 40;   // the field's cell size: low = broad clouds, high = wisps
    /// The window: only the top of the field is born, so the cloud is mostly dark with structure
    /// in it. Placed against the field's OWN measured range, so this means the same thing on a
    /// panel and on a cube. Measured across the slider, 64x64 against a 20-cube: 0 floods both
    /// (99% and 97%), 128 is a haze (79% and 66%), 192 is a cloud (35% and 31%), 255 leaves a few
    /// wisps (7% and 6%). The two track each other at every setting, where an absolute threshold
    /// lit a quarter of the panel and almost none of the cube.
    uint8_t contrast    = 192;
    uint8_t persistence = 140;  // how long light survives once it is in the flow
    uint8_t octaves     = 2;    // detail in the field, and its cost knob
    uint8_t fieldScale  = 1;    // 1 = full, 2 = half, 4 = quarter: the field's resolution
    uint8_t fieldRate   = 1;    // recompute the field every N frames

    void defineControls() override {
        controls_.addControl("speed", speed, 0, 255);
        controls_.addControl("scale", scale, 1, 255);
        controls_.addControl("contrast", contrast, 0, 255);
        controls_.addControl("persistence", persistence, 0, 255);
        controls_.addControl("octaves", octaves, 1, 4);
        controls_.addControl("fieldScale", fieldScale, 1, 4);
        controls_.addControl("fieldRate", fieldRate, 1, 8);
    }

    bool affectsPrepare(const char* name) const override {
        // `fieldScale` alone resizes the field plane. `fieldRate` only skips frames and every other
        // control is read per frame, so none of them needs a rebuild.
        return std::strcmp(name, "fieldScale") == 0;
    }

    /// The lever, clamped once: prepare() sizes the field from it and tick() picks its path by it.
    uint8_t fieldScaleClamped() const { return fieldScale < 1 ? 1 : (fieldScale > 4 ? 4 : fieldScale); }

    void prepare() override {
        const lengthType w = width(), h = height(), d = depth();
        const size_t n = static_cast<size_t>(w) * h * d * 3;
        const size_t had = planeA_.count();
        // The cloud itself: two planes, because advection reads one and writes the other.
        planeA_.resize(n);
        planeB_.resize(n);
        // Everything the render path writes into is sized HERE: tick() is MM_NONBLOCKING, and a
        // resize() on a changed size reallocates and zero-fills, which is heap work in the hot path.
        carry_.resize(n);
        scratch_.resize(fieldScaleClamped() > 1 ? n : 0);
        taps_.resize(static_cast<size_t>(w));
        // Same sample count but a different shape (8x16 -> 16x8, or a cube reshaped): both planes
        // hold samples laid out for the old geometry and the ping-pong swaps the spare one in on
        // the next frame, so clearing only the live one leaves the stale picture one frame away.
        // (A changed count needs no clear: resize() zero-fills when it reallocates.) The same guard
        // Trails carries, for the same reason.
        if (n > 0 && n == had && (w != planeW_ || h != planeH_ || d != planeD_)) {
            std::memset(planeA_.data(), 0, planeA_.bytes());
            std::memset(planeB_.data(), 0, planeB_.bytes());
        }
        // The field is computed at its own resolution and stretched. At fieldScale 1 it IS the
        // fixture, and no intermediate plane is allocated: the lever costs nothing when it is off.
        const uint8_t s = fieldScaleClamped();
        fw_ = w / s > 0 ? w / s : 1;
        fh_ = h / s > 0 ? h / s : 1;
        fd_ = d / s > 0 ? d / s : 1;
        field_.resize(s > 1 ? static_cast<size_t>(fw_) * fh_ * fd_ * 3 : 0);
        planeW_ = w; planeH_ = h; planeD_ = d;
        frame_ = 0;
        started_ = false;
    }

    void tick() MM_NONBLOCKING override {
        if (!planeA_ || !planeB_) return;
        const lengthType w = width(), h = height(), d = depth();
        // One clock reading, and a zero delta on the first tick: without it `now - 0` hands the
        // flow the whole uptime, which teleports the cloud and decays it away on the frame it
        // starts. Trails carries the same guard for the same reason.
        const uint32_t now = elapsed();
        const uint32_t t = now;              // the flow field's third axis, read inside the lambda
        const uint32_t dt = started_ ? now - lastMs_ : 0u;
        lastMs_ = now;
        started_ = true;

        bank_.set(0, {.rate = static_cast<uint16_t>(4 + speed / 6), .low = 0, .high = 65535,
                      .phaseOffset = 0, .wave = Wave::Saw});
        // advanceTo() takes an ABSOLUTE timestamp and computes its own delta (math16.h BeatPhase),
        // so passing this frame's dt feeds it the CHANGE in frame time: a few percent of the right
        // motion on a jittery device, and none at all where the frame time is steady.
        bank_.advanceTo(now);

        uint16_t* live = front_ ? planeA_.data() : planeB_.data();
        uint16_t* spare = front_ ? planeB_.data() : planeA_.data();

        // 1. Transport, every frame: this is the motion, and it is what fieldRate must not skip.
        const uint32_t cells = static_cast<uint32_t>(scale) * 256u;
        const int32_t push = static_cast<int32_t>(speed) * static_cast<int32_t>(dt) / 4;
        draw::advect16(spare, live, w, h, d,
                       [&](lengthType x, lengthType y, lengthType z,
                           draw::pos_t& vx, draw::pos_t& vy) {
                           int32_t cx = 0, cy = 0;
                           // eps MUST be at least the distance between neighboring samples.
                           // Left at its default (4096) while a pixel step is `cells` (10240 at
                           // scale 40), the central difference measures inside the gap BETWEEN two
                           // pixels, so neighbors get uncorrelated flow directions and the cloud
                           // comes out striped rather than smooth. Half a pixel step is the
                           // smallest value that still sees the field the pixels sample.
                           curl16(static_cast<uint32_t>(x) * cells,
                                  static_cast<uint32_t>(y) * cells,
                                  static_cast<uint32_t>(z) * cells + t / 8u, push, cx, cy,
                                  cells / 2u);
                           vx = static_cast<draw::pos_t>(cx);
                           vy = static_cast<draw::pos_t>(cy);
                       }, draw::Edge::Clamp);
        front_ = !front_;
        uint16_t* cloud = front_ ? planeA_.data() : planeB_.data();

        // 2. Decay, every frame: the half-life is what makes the cloud a cloud rather than a smear.
        draw::decay16(cloud, planeA_.count(), 40u + static_cast<uint32_t>(persistence) * persistence / 12u, dt);

        // 3. Birth, every fieldRate frames: the expensive half, and the one the levers exist for.
        const uint8_t rate = fieldRate < 1 ? 1 : fieldRate;
        if (frame_ % rate == 0) birth(cloud, w, h, d, now);
        frame_++;

        // 4. Onto the layer, narrowing once. Dithered temporally: the cloud's dark half is where
        //    banding lives, and carrying the error is what keeps a slow fade smooth.
        draw::blit16(canvas(), cloud, w, h, d, carry_ ? carry_.data() : nullptr);
    }

private:
    /// Where light is born: the field, thresholded so only its top survives, added to the cloud.
    void birth(uint16_t* cloud, lengthType w, lengthType h, lengthType d, uint32_t now) {
        const uint32_t drift = bank_.unitValue(0) >> 6;
        const uint32_t cells = static_cast<uint32_t>(scale) * 192u;
        const uint8_t oct = octaves < 1 ? 1 : (octaves > 4 ? 4 : octaves);
        // At fieldScale 1 the field is written straight into the cloud; above it, into the small
        // plane and stretched. One code path either way: `dst` and its extents are what differ.
        // The window sits at `contrast` of the way up the field's OWN range, measured on the
        // previous frame, rather than at an absolute value. A fixed threshold is tuned for one
        // fixture and wrong on every other: measured, fbm8 over an 8-cube's 512 samples peaks near
        // 223 while a 64x64 panel's 4096 reach much higher, so a threshold of 200 lit a quarter of
        // the panel and almost nothing of the cube. Tracking the range means `contrast` says what
        // FRACTION of the field is born, on any fixture. (Aurora places its window the same way.)
        // The control spans the TOP HALF of the range, not all of it. Nebula's field is an emitter
        // that runs every frame and accumulates in the cloud, so anything below about 60% of the
        // range floods the fixture: measured at 64x64, a window at 73% lights 96% of the panel and
        // one at 93% lights 24%. Mapping 0..255 onto 60%..100% puts the whole useful span on the
        // slider instead of compressing it into its last few values. (Aurora draws its field
        // directly rather than accumulating, so its own mapping starts at the bottom.)
        const uint8_t lowEnd = floor_ < peak_ ? floor_ : static_cast<uint8_t>(peak_ > 8 ? peak_ - 8 : 0);
        const uint32_t span16 = static_cast<uint32_t>(peak_ - lowEnd);
        const uint32_t frac = 154u + (static_cast<uint32_t>(contrast) * 101u) / 255u;   // 60%..100%
        const uint8_t threshold = static_cast<uint8_t>(lowEnd + (span16 * frac) / 255u);
        uint8_t frameMax = 0, frameMin = 255;

        const bool small = field_.count() > 0;
        uint16_t* dst = small ? field_.data() : cloud;
        const lengthType dw = small ? fw_ : w, dh = small ? fh_ : h, dd = small ? fd_ : d;
        const lengthType sx = w / (dw > 0 ? dw : 1), sy = h / (dh > 0 ? dh : 1), sz = d / (dd > 0 ? dd : 1);
        std::size_t i = 0;
        for (lengthType z = 0; z < dd; z++)
            for (lengthType y = 0; y < dh; y++)
                for (lengthType x = 0; x < dw; x++, i += 3) {
                    const uint8_t v = fbm8(static_cast<uint32_t>(x * sx) * cells / 256u + drift,
                                           static_cast<uint32_t>(y * sy) * cells / 256u,
                                           static_cast<uint32_t>(z * sz) * cells / 256u + now / 32u, oct);
                    // The window: below `contrast` nothing is born at all, and what is above it is
                    // stretched back over the full range, so a small part of the field becomes all
                    // of the light. This is what makes a cloud rather than an even haze.
                    if (v > frameMax) frameMax = v;
                    if (v < frameMin) frameMin = v;
                    if (v <= threshold) {
                        if (small) { dst[i] = dst[i + 1] = dst[i + 2] = 0; }
                        continue;
                    }
                    const uint32_t span = peak_ > threshold ? peak_ - threshold : 1u;
                    const uint32_t over = static_cast<uint32_t>(v - threshold) * 255u / span;
                    const uint8_t bri = static_cast<uint8_t>(over > 255u ? 255u : over);
                    // The palette index is the field's position WITHIN THE WINDOW, stretched back
                    // over the full range, not the raw field value. Two reasons, and the second is
                    // what makes the effect look like its palette at all:
                    //
                    //  - no time term: adding one walks every born pixel through the palette
                    //    together, which reads as the cloud changing color rather than as a cloud.
                    //    A wisp should keep the color it was born with as the flow carries it.
                    //  - stretched: the window admits only the top of the field (values above
                    //    `contrast`), so the raw value spans about a fifth of the palette and every
                    //    wisp comes out the same hue. Rescaling that slice to 0..255 gives the same
                    //    cloud in every color the palette has.
                    //
                    // `over` is already that position, 0 at the window's edge and 255 at the
                    // field's peak, so the index and the brightness come from one computation.
                    const RGB c = colorFromPalette(*Palettes::active(), bri, bri);
                    // Written at full width, so the decay has somewhere to go.
                    if (small) {
                        dst[i + 0] = static_cast<uint16_t>((c.r << 8) | c.r);
                        dst[i + 1] = static_cast<uint16_t>((c.g << 8) | c.g);
                        dst[i + 2] = static_cast<uint16_t>((c.b << 8) | c.b);
                    } else {
                        // Straight into the cloud: ADD, so new light joins what is already flowing
                        // rather than erasing it.
                        addWide(dst, i + 0, static_cast<uint16_t>((c.r << 8) | c.r));
                        addWide(dst, i + 1, static_cast<uint16_t>((c.g << 8) | c.g));
                        addWide(dst, i + 2, static_cast<uint16_t>((c.b << 8) | c.b));
                    }
                }
        // Carry this frame's range into the next one's window, eased so a single bright frame
        // cannot make the whole cloud flinch.
        peak_  = static_cast<uint8_t>((peak_ * 7u + (frameMax < 32 ? 32 : frameMax)) / 8u);
        floor_ = static_cast<uint8_t>((floor_ * 7u + frameMin) / 8u);

        if (!small) return;
        // Stretch the small field over the cloud, adding as it goes.
        if (!scratch_ || !taps_) return;          // sized in prepare(); the render path never allocates
        draw::upscale16(scratch_.data(), w, h, d, field_.data(), fw_, fh_, fd_,
                        taps_.data(), taps_.count());
        const size_t n = static_cast<size_t>(w) * h * d * 3;
        for (size_t k = 0; k < n; k++) addWide(cloud, k, scratch_[k]);
    }

    /// Saturating add at 16 bits: light adds, and a bright cloud must not wrap to black.
    static void addWide(uint16_t* p, size_t i, uint16_t v) {
        const uint32_t s = static_cast<uint32_t>(p[i]) + v;
        p[i] = static_cast<uint16_t>(s > 65535u ? 65535u : s);
    }


    ScratchBuffer<uint16_t> planeA_{*this};    ///< the cloud; the two alternate roles
    ScratchBuffer<uint16_t> planeB_{*this};
    ScratchBuffer<uint16_t> field_{*this};     ///< the field at its own resolution (empty at 1:1)
    ScratchBuffer<uint16_t> scratch_{*this};   ///< the stretched field, when one is used
    ScratchBuffer<draw::UpscaleTap> taps_{*this};  ///< upscale16's per-column blend table
    ScratchBuffer<uint8_t>  carry_{*this};     ///< the dither's per-channel error
    OscillatorBank<1>       bank_;
    lengthType              planeW_ = 0, planeH_ = 0, planeD_ = 0;
    lengthType              fw_ = 0, fh_ = 0, fd_ = 0;
    bool                    front_ = true;
    uint32_t                lastMs_ = 0;
    uint32_t                frame_ = 0;
    uint8_t                 peak_ = 200;    ///< the field's high water mark, eased per frame
    uint8_t                 floor_ = 40;    ///< and its low, so the window spans what is there
    bool                    started_ = false;   ///< false until a frame has been timed
};

}  // namespace mm
