#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

// Trails: dots thrown into a moving medium, leaving tails that the flow carries and bends.
//
// The composition is three power functions and nothing else: an EMITTER draws a few bright points,
// `draw::advect` carries the whole plane along a velocity field, and `draw::decay16` dims it by a
// half-life. Run every frame, that loop is what a trail IS. Nothing here paints a tail: the tail is
// last frame's dots, moved and dimmed, which is why the shape of the flow is visible in it.
//
// This is the advection idiom the way Aurora is the shader idiom. The vocabulary it demonstrates is
// transport rather than sampling: a field that says where the medium is going, applied to whatever
// happens to be there. Prior art: the flow-field family (4wheeljive's FlowFields, from a Stefan
// Petrick concept), and Stam's backward advection for the transport step itself.
//
// **The plane is 16-BIT, and that is the point.** A trail is a value multiplied by slightly less
// than one, hundreds of times a second. At 8 bits that either truncates to nothing (the tail dies
// early: measured, 48 of 64 cells) or rounds back up to where it started (the trail never fades and
// the effect turns solid). Both were measured; see `draw::decay`. So the plane the effect owns is
// wider than the layer it writes to, and narrows once on the way out. The precision belongs in the
// ACCUMULATOR, not in the frame buffer.
//
// Cost: one advect (a bilinear sample per light) plus one noise sample per light for the flow, so
// the flow rule dominates. Volumetric: on a cube every slice gets its OWN flow, sampled at that
// slice's depth, so the slices differ rather than one plane repeating. Light is carried WITHIN a
// slice and not yet between them: `advect16`'s rule yields vx and vy only, so a trail does not
// travel through the volume. 3D transport needs a trilinear sampler and a vz, which is its own
// change.
// @card TrailsEffect.png
/// Effect: bright dots thrown into a flowing medium, leaving tails the flow carries and bends.
class TrailsEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🖌️💨🌫️"; }   // power-function showcase
    Dim dimensions() const override { return Dim::D3; }   // per-slice: each slice gets its own flow

    static constexpr uint8_t kMaxDots = 8;
    static constexpr uint32_t kMaxEmitters = 64;   ///< the ceiling once `dots` is scaled by the grid

    /// The head's radius for a fixture: one light on a panel, larger as the grid grows, capped so a
    /// wall does not spend the frame drawing one head.
    static lengthType spanRadius(lengthType span) {
        const lengthType r = static_cast<lengthType>(span / 256 + 1);
        return r > 4 ? 4 : r;
    }

    uint8_t speed      = 40;   // how fast the medium moves, and with it every tail
    uint8_t dots       = 3;    // emitter DENSITY: the count scales with the grid (see emitDots)
    uint8_t scale      = 30;   // the flow field's cell size: low = broad sweeps, high = eddies
    uint8_t persistence = 90;  // how long a tail survives, as a half-life (see halfLifeMs)
    uint8_t breathe    = 40;   // how much the flow's strength rises and falls

    void defineControls() override {
        controls_.addControl("speed", speed, 0, 255);
        controls_.addControl("dots", dots, 1, kMaxDots);
        controls_.addControl("scale", scale, 1, 255);
        controls_.addControl("persistence", persistence, 0, 255);
        controls_.addControl("breathe", breathe, 0, 255);
    }

    void prepare() override {
        // The plane is the effect's own state and holds THREE channels per light whatever the
        // layer's width is: a trail is about its own history, not about the fixture's wiring.
        const lengthType w = width(), h = height(), d = depth();
        const size_t needed = static_cast<size_t>(w) * h * d * 3;
        const size_t had = plane_.count();
        plane_.resize(needed);
        // Same sample count but a different shape (8x16 -> 16x8, or a cube reshaped): resize() kept
        // the old samples, and they are laid out for the old geometry, so they would smear. Clear.
        scratch_.resize(needed);
        carry_.resize(needed);         // the dither's error, one byte per sample
        // Same sample count but a different shape: BOTH planes hold samples laid out for the old
        // geometry, and the ping-pong swaps the spare one in on the very next frame, so clearing
        // only the live one leaves the stale picture one frame away. (A changed sample count needs
        // no clear: resize() zero-fills when it reallocates.)
        if (needed > 0 && needed == had && (w != planeW_ || h != planeH_ || d != planeD_)) {
            std::memset(plane_.data(), 0, plane_.bytes());
            std::memset(scratch_.data(), 0, scratch_.bytes());
        }
        planeW_ = w; planeH_ = h; planeD_ = d;
        started_ = false;      // the next tick is the first: it has no previous frame to measure
    }

    void tick() MM_NONBLOCKING override {
        if (!plane_ || !scratch_) return;               // a zero grid, or an allocation that failed
        const lengthType w = width(), h = height(), d = depth();
        // One reading of the clock, and a zero delta on the first tick: `lastMs_` has no previous
        // frame to measure against, so `elapsed() - 0` would hand the flow the whole uptime and
        // teleport the trail (and decay it to nothing) on the frame it starts.
        const uint32_t now = elapsed();
        const uint32_t dt = started_ ? now - lastMs_ : 0u;
        lastMs_ = now;
        started_ = true;

        // Two oscillators: one walks the emitters around, one breathes the flow's strength so the
        // composition swells and settles instead of running at one rate forever.
        bank_.set(0, {.rate = static_cast<uint16_t>(8 + speed / 8), .low = 0, .high = 65535,
                      .phaseOffset = 0, .wave = Wave::Saw});
        bank_.set(1, {.rate = 7, .low = static_cast<int32_t>(256 - breathe),
                      .high = static_cast<int32_t>(256 + breathe),
                      .phaseOffset = 0, .wave = Wave::Sine});
        // advanceTo() takes an ABSOLUTE timestamp and computes its own delta (math16.h BeatPhase),
        // so passing this frame's dt feeds it the CHANGE in frame time: a few percent of the right
        // motion on a jittery device, and none at all where the frame time is steady.
        bank_.advanceTo(now);

        // Which buffer currently HOLDS the trail alternates: a ScratchBuffer is deliberately fixed
        // to its module (non-movable, it owns a slot in the module's free list), so the ping-pong
        // swaps a flag rather than the buffers.
        ScratchBuffer<uint16_t>& src = front_ ? plane_ : scratch_;
        ScratchBuffer<uint16_t>& dst = front_ ? scratch_ : plane_;

        // 1. Transport: carry what is already there along the flow. Backward-sampled, so this both
        //    moves the trail and is what bends it, since neighboring pixels take different paths.
        const uint32_t t = now;      // the same reading the delta came from
        const uint32_t cells = static_cast<uint32_t>(scale) * 256u;
        const int32_t strength = static_cast<int32_t>(bank_.value(1));   // the breathing multiplier
        const uint32_t step = (static_cast<uint32_t>(speed) * dt) / 8u;  // sub-pixels this frame
        draw::advect16(dst.data(), src.data(), w, h, d,
                       [&](lengthType x, lengthType y, lengthType z,
                           draw::pos_t& vx, draw::pos_t& vy) {
                           flowAt(x, y, z, t, cells, strength, step, vx, vy);
                       }, draw::Edge::Clamp);
        front_ = !front_;                       // the destination now holds the trail
        ScratchBuffer<uint16_t>& moved = front_ ? plane_ : scratch_;

        // 2. Decay: a half-life, so the tail is the same length in SECONDS on any device. This is
        //    the step that needs the wide plane (draw::decay's own note has the measurements).
        draw::decay16(moved.data(), moved.count(), halfLifeMs(), dt);

        // 3. Emit: the bright heads, drawn after the transport so this frame's dots are sharp and
        //    only the previous ones have been carried.
        emitDots(moved.data(), w, h, d, dt);

        // 4. Narrow onto the layer: the one place the wide plane meets the fixture's width.
        // Dithered, like the other wide-plane effects: a trail's slow fade is exactly where an
        // 8-bit truncation bands, and the carry is what removes it.
        draw::blit16(canvas(), moved.data(), w, h, d, carry_ ? carry_.data() : nullptr);
    }

private:
    /// The persistence control as a half-life in milliseconds. Deliberately not linear: the
    /// interesting range is short, and a slider that spends half its travel between four and eight
    /// seconds would waste it. 0 is a bare head with no tail at all.
    uint32_t halfLifeMs() const {
        return 20u + static_cast<uint32_t>(persistence) * static_cast<uint32_t>(persistence) / 16u;
    }

    /// The flow: where the medium is going at this point, in sub-pixels this frame.
    ///
    /// A noise field read at two offsets, one per axis, which is the decoupled form: reading ONE
    /// field for both axes moves everything along a diagonal, since the two components would rise
    /// and fall together. The third axis is sampled too, so a cube's slices flow differently rather
    /// than the same plane repeating through the volume.
    void flowAt(lengthType x, lengthType y, lengthType z, uint32_t t, uint32_t cells,
                int32_t strength, uint32_t step, draw::pos_t& vx, draw::pos_t& vy) const {
        const uint32_t fx = static_cast<uint32_t>(x) * cells;
        const uint32_t fy = static_cast<uint32_t>(y) * cells;
        const uint32_t fz = static_cast<uint32_t>(z) * cells + t / 4u;
        // Centered on zero, so the field pushes both ways rather than only along the axes.
        const int32_t nx = static_cast<int32_t>(inoise16(fx, fy, fz)) - 32768;
        const int32_t ny = static_cast<int32_t>(inoise16(fx + 0x9E37u, fy + 0x7C15u, fz)) - 32768;
        // strength is the breathing multiplier in 1/256ths; step is the frame's travel budget.
        const int32_t amp = static_cast<int32_t>(step) * strength / 256;
        // 64-bit for the product, as curl16 carries for the same reason: `amp` grows with a
        // stalled dt, and a signed 32-bit multiply wraps rather than saturating, which reverses the
        // flow instead of merely overdriving it.
        vx = static_cast<draw::pos_t>((static_cast<int64_t>(nx) * amp) >> 15);
        vy = static_cast<draw::pos_t>((static_cast<int64_t>(ny) * amp) >> 15);
    }

    /// The heads: a few bright points walking their own paths, each on the palette.
    ///
    /// Emission is paced by TIME, not by frames, and the heads stay at full brightness.
    ///
    /// Writing a head every frame injects light at the framerate: measured, a 1200 fps device lit a
    /// third more of the panel than a 60 fps one for the same settings. Scaling the brightness by
    /// dt instead is the obvious fix and is WRONG here, because `writeWide` SETS the pixel rather
    /// than accumulating into it: twenty dim writes do not add up to one bright one, and the fast
    /// device came out twice as dark instead (1.96 the other way). So the head keeps its full
    /// value and fires on a budget, with the remainder carried, which is the pattern
    /// ParticlesEffect already uses for its fade.
    void emitDots(uint16_t* plane, lengthType w, lengthType h, lengthType d, uint32_t dt) {
        // `dots` is a DENSITY, not a count: the emitters scale with the fixture, because a handful
        // of heads that fill a 64x64 panel are lost on a wall. Measured against the script, which
        // already did this: three fixed dots put 1/567th of the light on a 768x348 panel.
        const lengthType span = w > h ? w : h;
        const lengthType rad = spanRadius(span);
        const uint32_t area = static_cast<uint32_t>(rad * 2 + 1) * (rad * 2 + 1);
        const uint8_t want = dots < 1 ? 1 : (dots > kMaxDots ? kMaxDots : dots);
        uint32_t scaled = static_cast<uint32_t>(want) * w * h / (4096u * (area > 8u ? area / 9u : 1u));
        if (scaled < 1) scaled = 1;
        if (scaled > kMaxEmitters) scaled = kMaxEmitters;
        const uint8_t n = static_cast<uint8_t>(scaled);
        constexpr uint32_t kReferenceMs = 20;
        emitCarry_ += dt;
        if (emitCarry_ < kReferenceMs) return;      // not enough time has passed for a head yet
        // One head per frame at most, and the remainder carries: a slow frame must not emit the
        // burst it owes, because every head is a full disc and a stall would flood the plane. Below
        // 50 fps this does cost heads (30/s at 30 fps against 50/s above 50), which is the trade
        // taken deliberately; Fluid's jets cap the same debt rather than dropping it.
        emitCarry_ %= kReferenceMs;
        const uint32_t walk = bank_.unitValue(0);
        for (uint8_t i = 0; i < n; i++) {
            // Each dot rides the same clock at its own offset and its own ratio, so they never
            // bunch: a Lissajous walk, which visits the whole grid rather than circling one spot.
            const angle16 a = static_cast<angle16>(walk + i * (65536u / n));
            const angle16 b = static_cast<angle16>(walk * 3u + i * 9973u);
            const lengthType px = mapAxis(sin16(a), w);
            const lengthType py = mapAxis(cos16(b), h);
            const lengthType pz = d > 1 ? mapAxis(sin16(static_cast<angle16>(b * 2u)), d) : 0;
            const RGB c = colorFromPalette(*Palettes::active(),
                                           static_cast<uint8_t>(i * (255u / n) + (walk >> 9)));
            // Written at the plane's width, so a head starts at full precision and the decay has
            // somewhere to go: writing a byte would put the whole tail in the top 8 bits.
            writeWide(plane, w, h, d, px, py, pz, c, rad);
        }
    }

    /// A signed 16-bit sine mapped onto an axis, centered, with the ends reachable.
    static lengthType mapAxis(int16_t s, lengthType extent) {
        if (extent <= 1) return 0;
        const int32_t v = (static_cast<int32_t>(s) + 32768) * (extent - 1) / 65535;
        return static_cast<lengthType>(v);
    }

    /// One light at the plane's full width: an 8-bit color widened by repeating the byte, so 255
    /// becomes 65535 rather than 65280 and a full-brightness head is genuinely full.
    static void writeWide(uint16_t* plane, lengthType w, lengthType h, lengthType d,
                          lengthType x, lengthType y, lengthType z, RGB c, lengthType rad) {
        // A DISC, not a pixel. Advection spreads a head bilinearly, so after N frames one light's
        // worth of brightness covers roughly N pixels: a single-pixel head on a multi-second tail
        // arrives at about 1 part in 255 and the panel reads as a faint smear. The radius grows
        // with the fixture, because the area a tail is spread over does too.
        const lengthType rz = d > 1 ? rad : 0;
        for (lengthType dz = -rz; dz <= rz; dz++)
            for (lengthType dy = -rad; dy <= rad; dy++)
                for (lengthType dx = -rad; dx <= rad; dx++) {
                    if (dx * dx + dy * dy + dz * dz > rad * rad) continue;
                    const lengthType px = x + dx, py = y + dy, pz = z + dz;
                    if (px < 0 || py < 0 || pz < 0 || px >= w || py >= h || pz >= d) continue;
                    const size_t off = (static_cast<size_t>(pz) * h * w
                                      + static_cast<size_t>(py) * w + px) * 3;
                    plane[off + 0] = static_cast<uint16_t>((c.r << 8) | c.r);
                    plane[off + 1] = static_cast<uint16_t>((c.g << 8) | c.g);
                    plane[off + 2] = static_cast<uint16_t>((c.b << 8) | c.b);
                }
    }


    ScratchBuffer<uint16_t> plane_{*this};     ///< the trail itself, three samples per light
    ScratchBuffer<uint16_t> scratch_{*this};   ///< advect's destination; the two alternate roles
    ScratchBuffer<uint8_t>  carry_{*this};     ///< the dither's per-channel error
    bool                    front_ = true;     ///< which of the two currently holds the trail
    OscillatorBank<2>       bank_;
    lengthType              planeW_ = 0, planeH_ = 0, planeD_ = 0;
    uint32_t                lastMs_ = 0;
    bool                    started_ = false;   ///< false until a frame has been timed
    uint32_t                emitCarry_ = 0;     ///< time owed to the emitters, in ms
};

}  // namespace mm
