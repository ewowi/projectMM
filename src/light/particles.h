#pragma once

#include "core/math16.h"      // sin16/cos16 for angleEmit, hashInt for spray, isqrt for attract
#include "core/AudioFrame.h"   // AudioFrame: the spectrum audioDrive() reads
#include "core/AudioService.h" // latestFrame(): the live spectrum stepDriven() consumes
#include "light/draw.h"       // pos_t, splat, Canvas — particles render through the sub-pixel writer
#include "light/Palette.h"    // colorFromPalette

namespace mm::particles {

// The particle kernel: one home for "things that move under forces and get drawn".
//
// This exists to make effects that DO NOT EXIST YET cheap to write. Anything that behaves like
// matter — sparks, rain, snow, smoke, confetti, a fountain, a swarm, debris from an impact, an
// audio band throwing off embers — is the same handful of forces over the same state, and the part
// that differs between them is which forces are applied and how particles are emitted, not the
// physics. Handing an effect writer a working integrator, wall behavior and emitter means a new
// look is a few lines of composition rather than a re-derivation of Euler integration.
//
// Retrofitting the effects that already hand-roll this is a real second benefit — several carry
// their own representation (structs with floats, parallel arrays, a private 12.4 pair) and each
// re-derived the same integration and the same wall bounce — but it is a consequence of the kernel
// being right, not the reason to build it. The kernel is designed for what comes next; existing
// effects move onto it when their behavior is judged on the bench, one at a time.
//
// **Structure of arrays, not array of structs.** Each field is its own contiguous run, so a pass
// that touches only velocity walks only velocity. On a chip with a small cache and no prefetcher
// that is the difference between streaming and thrashing, and it is what lets `step()` be a tight
// loop over two arrays rather than a stride over a fat struct.
//
// **Positions are `pos_t`** — the same 24.8 sub-pixel type `draw::splat` takes. A particle at
// x=3.5 lands half on pixel 3 and half on pixel 4 rather than snapping, which is what makes slow
// motion on a coarse grid look smooth instead of stepping. Velocities are the same units per frame.
//
// **No allocation after prepare().** The pool is a view over the caller's ScratchBuffers; it never
// owns memory and never resizes during a frame. An effect sizes its pool to what it wants and the
// framework frees it on teardown — the pay-for-what-you-use rule.
//
// Integration is semi-implicit (symplectic) Euler: velocity first, then position from the NEW
// velocity. It is one line different from the explicit form and markedly more stable under a
// constant force, which is why every game physics engine uses it (Fiedler, "Integration Basics").
//
// **Frame order matters, and it is the caller's to get right:**
//
//     forces (gravity / force / forceSmall / drag / attract)
//     -> collide()       (before moving, so a push cannot resolve out of bounds)
//     -> step()
//     -> bounce() or killOutside()
//     -> age()
//     -> render()
//
// Collisions run BEFORE the move for the reason WLED-PS documents: resolving an overlap after
// integrating can shove a particle outside the grid, which the wall pass has already gone past.
//
// Prior art: the WLED Particle System by Damian Schneider (@DedeHai), whose per-frame vocabulary
// (emitters, forces, walls, a renderer over one pool) is the shape this follows, and Reeves 1983
// for the name. Written fresh in fixed point against those descriptions.

/// Converts real elapsed time into a per-frame scale factor, so physics runs at the same SPEED on
/// every target while still using every frame the hardware can render.
///
/// **The bug this prevents, and the trap next to it.** A pool advanced by a fixed amount each frame
/// has physics that are a property of the HARDWARE: the desktop renders tens of thousands of frames
/// a second and an ESP32 a few hundred, so one gravity setting is an explosion on one and a drift on
/// the other. The obvious fix — run a fixed 60 Hz simulation and skip the frames in between — is
/// WRONG for a light effect: it throws away exactly the smoothness the extra frames were rendered
/// for, so a fast device shows 60 Hz motion drawn 5000 times instead of motion 80x smoother.
///
/// So time is not quantised, it is SCALED. Every force and velocity is expressed per reference
/// frame (1/60 s), and `scale()` reports how much of a reference frame this frame covered in 8.8
/// fixed point: 256 at exactly 60 fps, 26 at 600 fps, 2560 after a 16-frame stall. A faster device
/// therefore takes many small steps where a slow one takes a few large ones — same trajectory, more
/// resolution along it.
class FrameTime {
public:
    /// `referenceHz` is the rate the effect's numbers are written against; 60 is the convention.
    explicit FrameTime(uint16_t referenceHz = 60) : refHz_(referenceHz > 0 ? referenceHz : 60) {}

    /// Call once per frame. Returns the scale in 8.8 fixed point (256 == one reference frame).
    uint32_t advance(uint32_t nowMs) {
        if (!started_) { started_ = true; lastMs_ = nowMs; return kOne; }
        const uint32_t dt = nowMs - lastMs_;      // unsigned: correct across the millis() wrap
        lastMs_ = nowMs;
        // A frame faster than the timer can resolve reports dt == 0. Carrying the remainder means
        // those frames still add up rather than freezing the animation — the same sub-millisecond
        // trap BeatPhase solves by dividing late.
        // Carry the NUMERATOR, not milliseconds, and divide late — the rule BeatPhase follows. One
        // unit is 1000/(256*60) = 0.065 ms, so a millisecond carry cannot represent the remainder at
        // all: it gets truncated away, and because the truncation differs by render rate (11 units a
        // frame at 60 fps, under 1 at 200) the discarded time IS a framerate dependency, the exact
        // fault this class removes. Deriving the period as `1000 / refHz` has the same shape of bug:
        // it truncates to 16 ms, a 62.5 Hz reference that runs every 60-fps setting ~4% fast.
        num_ += static_cast<uint64_t>(dt) * kOne * refHz_;
        const uint64_t units = num_ / 1000u;
        if (units == 0) return 0;                 // not enough time yet; the numerator keeps it
        num_ -= units * 1000u;
        uint32_t s = static_cast<uint32_t>(units > kMaxScale ? kMaxScale : units);
        // Cap a long stall (WiFi reconnect, reflash) so one frame cannot teleport every particle.
        return s > kMaxScale ? kMaxScale : s;
    }

    void reset() { started_ = false; num_ = 0; }

    static constexpr uint32_t kOne = 256;         ///< one reference frame
    static constexpr uint32_t kMaxScale = 256 * 8;

private:
    uint16_t refHz_;
    uint64_t num_ = 0;   // undivided time: dt * kOne * refHz_, spent in whole units
    uint32_t lastMs_ = 0;
    bool started_ = false;
};

/// Scale a signed value by `num/den` WITHOUT a right shift.
///
/// The documented rounding trap in every integer particle system: `-1 >> 1` is `-1`, not `0`, so a
/// signed right shift rounds negative values away from zero while rounding positive ones toward it.
/// Applied per frame to a velocity, that asymmetry is a slow drift — particles moving left creep
/// further than particles moving right. A divide rounds toward zero on both signs and costs one
/// cycle on the ESP32, so it is the correct primitive here, not an optimisation to undo.
inline constexpr int32_t scaleSigned(int32_t v, int32_t num, int32_t den) {
    return static_cast<int32_t>((static_cast<int64_t>(v) * num) / den);
}

/// How a particle is drawn.
enum class RenderStyle : uint8_t {
    Splat,   ///< sub-pixel, additive — the default: smooth motion, light adds where particles overlap
    Hard,    ///< one whole pixel, replacing — the retro look, and cheaper
};

/// A pool of particles as a view over caller-owned arrays. POD: copy it freely, it owns nothing.
///
/// `ttl` is the unit of life: a particle with ttl 0 is dead and is skipped by every pass. Effects
/// that want immortal particles simply never decrement it.
/// Map slot `i` of `slots` onto a distinct lane in [0, extent), interleaved so that consecutive
/// slots are NOT adjacent lanes.
///
/// Effects assign slots by species or role, and species differ in speed, so a straight stride
/// sorts the scene: the fast ones bunch at one edge within seconds. Interleaving mixes them.
/// The step must be COPRIME with `slots` or the mapping collapses: `(i * 5) % 5` is zero for
/// every i, which stacked an entire cast on one row (bench: five Pacman characters in a single
/// line). Stepping by a value chosen coprime to the count keeps the lanes distinct at any count.
inline lengthType spreadLane(uint16_t i, uint16_t slots, lengthType extent) {
    if (slots == 0) return 0;
    // A step coprime with `slots`, chosen as near HALF of it as possible. Coprimality alone only
    // guarantees the lanes are distinct: `slots - 1` is always coprime, but it is congruent to
    // -1, so consecutive slots land on ADJACENT lanes (descending) and the species this is meant
    // to interleave still bunch together. A step near half the count puts the widest gap between
    // consecutive slots, which is the actual goal. Searching outward from the midpoint always
    // terminates: 1 is coprime with everything and ends the walk.
    uint16_t step = 1;
    if (slots > 2) {
        for (uint16_t d = 0; d < slots / 2; d++) {
            const uint16_t lo = static_cast<uint16_t>(slots / 2 - d);
            const uint16_t hi = static_cast<uint16_t>(slots / 2 + d);
            uint16_t pick = 0;
            for (uint16_t c : {hi, lo}) {
                if (c <= 1 || c >= slots) continue;
                uint16_t a = c, b = slots;
                while (b) { const uint16_t t = a % b; a = b; b = t; }
                if (a == 1) { pick = c; break; }     // gcd(c, slots) == 1
            }
            if (pick) { step = pick; break; }
        }
    }
    const uint16_t lane = static_cast<uint16_t>((static_cast<uint32_t>(i) * step) % slots);
    return static_cast<lengthType>((static_cast<int32_t>(extent) * lane) / slots);
}

/// Per-sprite audio drive: how fast sprite `i` of `slots` should move for the sound playing now,
/// as a multiplier of FrameTime::kOne (so 0 = frozen, kOne = its normal speed).
///
/// Shared by every sprite effect with a `audioReactive` checkbox (FlyingToasters, FishTank,
/// Pacman): the behavior a viewer expects is identical in all three, so it is written once.
///
/// Each sprite gets its OWN band, spread across the 16 the FFT produces, so a scene breathes with
/// the music instead of surging as one block: the bass sprites lurch on the kick while the treble
/// ones flutter on the hats. The overall level gates it, so SILENCE STANDS THE SCENE STILL - the
/// requirement that makes the mode read as audio-reactive rather than merely speed-varying, since
/// a per-band value alone still drifts on noise between tracks.
///
/// Returns kOne unchanged when there is no audio at all (no microphone, service not running), so
/// an effect never freezes on a device that simply cannot hear.
inline uint32_t audioDrive(const AudioFrame* frame, uint16_t i, uint16_t slots) {
    if (!frame) return FrameTime::kOne;       // no audio source: move normally, never freeze

    // Below this the input is room noise or a gap between tracks, not music. The scene stands
    // still rather than creeping, which is what "if no music they should stand still" asks for.
    constexpr uint16_t kSilence = 8;
    if (frame->levelSmoothed < kSilence) return 0;

    const uint8_t band = slots ? static_cast<uint8_t>((static_cast<uint32_t>(i) * 16u) / slots) : 0;
    const uint32_t mag = frame->bands[band > 15 ? 15 : band];

    // A floor under the band keeps a sprite whose own band is quiet drifting slowly rather than
    // frozen mid-air while the music plays; the rest scales with that band, and a loud band runs
    // the sprite at about twice its normal speed.
    return FrameTime::kOne / 4 + (mag * FrameTime::kOne * 7) / (255 * 4);
}

struct Pool {
    draw::pos_t* x = nullptr;
    draw::pos_t* y = nullptr;
    draw::pos_t* vx = nullptr;
    draw::pos_t* vy = nullptr;
    /// Lifetime in reference frames; 0 = dead. SIXTEEN bits, not eight: a byte caps life at 255
    /// frames — about 4.25 s at 60 Hz — so slow smoke, drifting snow and long fades were simply not
    /// expressible. WLED-PS uses the same width for the same reason.
    uint16_t*    ttl = nullptr;
    uint8_t*     hue = nullptr;    ///< palette index per particle
    /// Per-particle radius in WHOLE pixels, 0 = a single sub-pixel splat. OPTIONAL: null renders
    /// every particle as a point. Size-varying blobs are the signature look of a particle system,
    /// and a pool that can only draw points reads as limited however good its physics is.
    uint8_t*     size = nullptr;
    /// Sub-unit force accumulator, 3.4 fixed point, one nibble per axis (low = x, high = y).
    /// OPTIONAL: null disables the accumulating form of `force()`. See `forceSmall()`.
    uint8_t*     acc = nullptr;
    uint16_t     count = 0;        ///< how many slots the arrays hold
    uint32_t     ageCarry_ = 0;    ///< sub-frame aging remainder (see age())
    int64_t      gCarry_ = 0;      ///< sub-unit gravity remainder (see gravity())

    bool valid() const { return x && y && vx && vy && ttl && hue && count > 0; }
    // `acc` and `size` are optional: a pool without them simply loses sub-unit forces
    // and per-particle radius, rather than being invalid.

    /// Kill every particle — the state a pool starts in, and what prepare() should leave behind.
    void clear() {
        for (uint16_t i = 0; i < count; i++) {
            ttl[i] = 0; x[i] = y[i] = vx[i] = vy[i] = 0; hue[i] = 0;
            if (acc) acc[i] = 0;
            if (size) size[i] = 0;
        }
    }

    /// Index of a free slot, or `count` when the pool is full. Linear: pools are small and the scan
    /// stops at the first hole, so a mostly-empty pool finds one immediately.
    uint16_t findFree() const {
        for (uint16_t i = 0; i < count; i++)
            if (ttl[i] == 0) return i;
        return count;
    }

    /// Bring one particle to life. Returns false when the pool is full, so an emitter can simply
    /// stop rather than overwrite a living particle.
    bool spawn(draw::pos_t px, draw::pos_t py, draw::pos_t svx, draw::pos_t svy,
               uint16_t life, uint8_t colour, uint8_t radius = 0) {
        const uint16_t i = findFree();
        if (i >= count) return false;
        x[i] = px; y[i] = py; vx[i] = svx; vy[i] = svy;
        ttl[i] = life == 0 ? 1 : life;      // life 0 would be born dead; clamp so a spawn always shows
        hue[i] = colour;
        if (size) size[i] = radius;
        return true;
    }

    // --- Forces ---------------------------------------------------------------------------------
    //
    // A force is a change to velocity, applied to every LIVE particle. They are separate calls
    // rather than flags on step() so an effect composes exactly the ones it wants and pays for
    // nothing else — the cost of a force is one pass over one array.

    /// Constant acceleration, the usual case being gravity. `g` is in sub-pixel units per frame².
    /// Applied unconditionally to every live particle: branching per particle costs more than the
    /// add it would skip.
    /// `scale` is FrameTime's 8.8 factor: 256 means "one reference frame's worth".
    void gravity(draw::pos_t g, uint32_t scale = FrameTime::kOne) {
        // Accumulate the sub-unit part rather than dropping it: at a high framerate one frame's
        // share of gravity rounds to zero, and truncating it would leave a fast device with NO
        // gravity at all — the same late-divide rule BeatPhase follows.
        gCarry_ += static_cast<int64_t>(g) * scale;
        const int32_t dv = static_cast<int32_t>(gCarry_ / FrameTime::kOne);
        if (dv == 0) return;
        gCarry_ -= static_cast<int64_t>(dv) * FrameTime::kOne;
        for (uint16_t i = 0; i < count; i++)
            if (ttl[i]) vy[i] = static_cast<draw::pos_t>(vy[i] + dv);
    }

    /// A constant push in any direction — wind, a tilt control, a thrust.
    void force(draw::pos_t fx, draw::pos_t fy, uint32_t scale = FrameTime::kOne) {
        const int32_t dx = scaleSigned(fx, static_cast<int32_t>(scale), FrameTime::kOne);
        const int32_t dy = scaleSigned(fy, static_cast<int32_t>(scale), FrameTime::kOne);
        for (uint16_t i = 0; i < count; i++)
            if (ttl[i]) {
                vx[i] = static_cast<draw::pos_t>(vx[i] + dx);
                vy[i] = static_cast<draw::pos_t>(vy[i] + dy);
            }
    }

    /// A force too SMALL to move a velocity by one unit per frame, accumulated until it does.
    ///
    /// With integer velocities, any acceleration below one unit/frame truncates to zero and simply
    /// does nothing — so gentle wind, a slow drift or a weak attractor are all invisible. The
    /// standard fix (WLED-PS) is a per-particle 3.4 fixed-point accumulator: the fractional part
    /// builds up over frames and spills into a whole ±1 velocity step when it overflows. That is
    /// what makes a light breeze read as inertia rather than as a stutter.
    ///
    /// `fx`/`fy` are in sixteenths of a velocity unit per frame, so 16 == +1/frame == `force()`.
    /// Requires `acc`; without it the sub-unit part is simply dropped.
    void forceSmall(int8_t fx, int8_t fy) {
        if (!acc) return;
        for (uint16_t i = 0; i < count; i++) {
            if (!ttl[i]) continue;
            int32_t ax = (acc[i] & 0x0F) + fx;         // x accumulator, 4 bits
            int32_t ay = ((acc[i] >> 4) & 0x0F) + fy;  // y accumulator
            // FLOOR toward negative infinity, not toward zero. C++ integer division truncates, so
            // `-1 / 16` is 0 and `-1 % 16` is -1: a negative force would leave a negative remainder
            // that the nibble cannot hold, and the borrow would be silently lost — a leftward wind
            // simply never moving anything. Flooring makes the carry symmetric in both directions.
            const int32_t cx = (ax >= 0) ? (ax / 16) : -((15 - ax) / 16);
            const int32_t cy = (ay >= 0) ? (ay / 16) : -((15 - ay) / 16);
            vx[i] = static_cast<draw::pos_t>(vx[i] + cx);
            vy[i] = static_cast<draw::pos_t>(vy[i] + cy);
            ax -= cx * 16; ay -= cy * 16;              // the remainder is now always 0..15
            acc[i] = static_cast<uint8_t>(((ay & 0x0F) << 4) | (ax & 0x0F));
        }
    }

    /// Velocity damping: `v *= (256 - k) / 256`. Air resistance, and the thing that stops a pool
    /// under constant force from accelerating forever.
    void drag(uint8_t k, uint32_t scale = FrameTime::kOne) {
        if (k == 0) return;
        // Exponential decay per unit TIME, not per frame: at 10x the framerate each frame must damp
        // a tenth as hard or the pool stops dead. Linear in the slice is close enough to the true
        // exponential for the slices a render loop produces, and costs one multiply.
        int32_t loss = static_cast<int32_t>((static_cast<uint32_t>(k) * scale) / FrameTime::kOne);
        if (loss > 255) loss = 255;
        const int32_t keep = 256 - loss;
        for (uint16_t i = 0; i < count; i++)
            if (ttl[i]) {
                vx[i] = static_cast<draw::pos_t>(scaleSigned(vx[i], keep, 256));
                vy[i] = static_cast<draw::pos_t>(scaleSigned(vy[i], keep, 256));
            }
    }

    /// Pull every particle toward a point with an inverse-square falloff, clamped near the centre so
    /// a particle sitting on the attractor does not receive an unbounded impulse.
    void attract(draw::pos_t ax, draw::pos_t ay, int32_t strength) {
        for (uint16_t i = 0; i < count; i++) {
            if (!ttl[i]) continue;
            const int32_t dx = ax - x[i];
            const int32_t dy = ay - y[i];
            // Work in whole pixels: the squared sub-pixel distance would overflow, and the field is
            // smooth enough that pixel resolution is plenty for a force.
            const int32_t px = dx / draw::kSubOne, py = dy / draw::kSubOne;   // divide, not shift
            int32_t d2 = px * px + py * py;
            if (d2 < 1) d2 = 1;                       // the near-field clamp
            const int32_t a = strength / d2;
            // Direction times magnitude, normalised by the distance so diagonal pull is not stronger.
            const int32_t d = static_cast<int32_t>(isqrt(static_cast<uint32_t>(d2)));
            if (d == 0) continue;
            vx[i] += static_cast<draw::pos_t>((px * a) / d);
            vy[i] += static_cast<draw::pos_t>((py * a) / d);
        }
    }

    // --- Integration ----------------------------------------------------------------------------

    /// Advance every live particle by one frame: position from the CURRENT velocity.
    ///
    /// Semi-implicit Euler means the caller applies forces (which update velocity) and then calls
    /// this, so position always integrates the already-updated velocity. That ordering is what makes
    /// it stable under a constant force where explicit Euler drifts.
    /// Advance every live particle. Velocities are in SUB-PIXEL units per reference frame, so a
    /// caller wanting fine motion expresses it in the velocity rather than relying on the step to
    /// keep a fraction: a velocity of 256 is one pixel per frame, and 1 is 1/256th of a pixel.
    ///
    /// That is why no carry is needed here where `gravity` has one. Gravity accumulates a force
    /// whose per-frame slice can round to nothing; a position already carries 8 bits of fraction, so
    /// the same slice lands in those bits instead of being lost. A caller passing whole-pixel
    /// velocities on a fast device is the case that stalls, and the fix is to scale the velocity —
    /// `draw::toSub(1)` per frame, not 1.
    void step(uint32_t scale = FrameTime::kOne) {
        for (uint16_t i = 0; i < count; i++)
            if (ttl[i]) {
                x[i] = static_cast<draw::pos_t>(x[i] + scaleSigned(vx[i], static_cast<int32_t>(scale), FrameTime::kOne));
                y[i] = static_cast<draw::pos_t>(y[i] + scaleSigned(vy[i], static_cast<int32_t>(scale), FrameTime::kOne));
            }
    }

    /// step() with a PER-PARTICLE time scale: `drive(i)` returns particle i's own multiplier of
    /// FrameTime::kOne. Audio-reactive sprite effects use it to move each sprite on its own audio
    /// band (see audioDrive) - the whole point being that the sprites do NOT move as one block.
    /// The frame scale still multiplies in, so speed stays frame-rate independent either way.
    /// step(), optionally driven by the music: with `audioReactive` set, each of the `live`
    /// sprites moves on its own frequency band and the scene stands still in silence; otherwise
    /// the whole pool steps together. The one place the audio-reactive rule lives, so the sprite
    /// effects share it rather than each carrying a copy of the branch.
    ///
    /// `live` is the number of sprites actually in play, NOT the pool capacity: the bands are
    /// spread across the sprites that exist, so passing the capacity would crowd every sprite
    /// into the low bands and leave the treble driving nothing.
    void stepDriven(uint32_t scale, bool audioReactive, uint16_t live) {
        if (!audioReactive) { step(scale); return; }
        const AudioFrame* f = AudioService::latestFrame();
        stepEach(scale, [f, live](uint16_t i) { return audioDrive(f, i, live); });
    }

    template <typename Drive>
    void stepEach(uint32_t scale, Drive drive) {
        for (uint16_t i = 0; i < count; i++)
            if (ttl[i]) {
                const uint32_t s = (scale * drive(i)) / FrameTime::kOne;
                x[i] = static_cast<draw::pos_t>(x[i] + scaleSigned(vx[i], static_cast<int32_t>(s), FrameTime::kOne));
                y[i] = static_cast<draw::pos_t>(y[i] + scaleSigned(vy[i], static_cast<int32_t>(s), FrameTime::kOne));
            }
    }

    /// Count down every particle's life; a particle reaching zero is dead and its slot is reusable.
    /// `rate` of 0 makes the pool immortal.
    void age(uint16_t rate = 1, uint32_t scale = FrameTime::kOne) {
        if (rate == 0) return;
        // ttl counts reference frames, so a fast device must age a fraction per frame. The carry
        // makes those fractions add up instead of rounding to nothing and living forever.
        ageCarry_ += static_cast<uint32_t>(rate) * scale;
        const uint32_t whole = ageCarry_ / FrameTime::kOne;
        if (whole == 0) return;
        ageCarry_ -= whole * FrameTime::kOne;
        const uint16_t d = whole > 65535 ? 65535 : static_cast<uint16_t>(whole);
        for (uint16_t i = 0; i < count; i++)
            if (ttl[i]) ttl[i] = ttl[i] > d ? static_cast<uint16_t>(ttl[i] - d) : 0;
    }

    // --- Boundaries -----------------------------------------------------------------------------

    /// Reflect particles off the walls of a `w` by `h` grid, keeping a fraction `e` of the speed
    /// (restitution: 256 is a perfect bounce, 0 stops dead). `roughness` scatters the reflected
    /// angle so a pile of particles does not end up moving in lockstep.
    void bounce(draw::pos_t w, draw::pos_t h, uint16_t e, uint8_t roughness = 0, uint32_t seed = 0) {
        for (uint16_t i = 0; i < count; i++) {
            if (!ttl[i]) continue;
            bool hit = false;
            if (x[i] < 0)      { x[i] = 0;     vx[i] = static_cast<draw::pos_t>(-scaleSigned(vx[i], e, 256)); hit = true; }
            else if (x[i] > w) { x[i] = w;     vx[i] = static_cast<draw::pos_t>(-scaleSigned(vx[i], e, 256)); hit = true; }
            if (y[i] < 0)      { y[i] = 0;     vy[i] = static_cast<draw::pos_t>(-scaleSigned(vy[i], e, 256)); hit = true; }
            else if (y[i] > h) { y[i] = h;     vy[i] = static_cast<draw::pos_t>(-scaleSigned(vy[i], e, 256)); hit = true; }
            if (hit && roughness) {
                // Scatter the tangent component. hashInt keeps this reproducible across devices,
                // which a stream RNG would not be.
                const int32_t jitter = (static_cast<int32_t>(hashInt(i, x[i], y[i], seed) & 0xFF) - 128)
                                     * roughness / 128;
                vx[i] = static_cast<draw::pos_t>(vx[i] + jitter);
            }
        }
    }

    /// Reduce one coordinate into 0..span, in constant time.
    ///
    /// A while-loop reduction costs one iteration per span crossed, so a particle given a large
    /// velocity (or a long stall) walks the loop thousands of times on the render tick. Modulo is
    /// one divide whatever the distance. Exactly `span` stays put rather than folding to 0, which
    /// is the far edge being inclusive — matching `bounce`, where `> span` is also the test.
    static draw::pos_t wrapCoord(draw::pos_t v, draw::pos_t span) {
        if (v >= 0 && v <= span) return v;                       // the common case, no divide
        // The two edges are NOT symmetric, and the loops this replaced are why: reducing from above
        // stops at `span` (`while (v > span)`), while climbing from below stops at 0 (`while
        // (v < 0)`). So an exact multiple lands on `span` coming down and on 0 coming up. Both name
        // the same point on a wrapped axis; keeping each side where it was is what makes this a
        // speed change and nothing more.
        const draw::pos_t m = static_cast<draw::pos_t>(v % span);
        if (v > span) return static_cast<draw::pos_t>(m == 0 ? span : m);
        return static_cast<draw::pos_t>(m == 0 ? 0 : m + span);
    }

    /// Wrap particles around the grid edges: a particle leaving one side re-enters the other.
    ///
    /// The third wall behavior, alongside `bounce` and `killOutside`, and the one snow, rain and
    /// marquee effects need — those want an endless field, not a box to rattle inside or a cliff to
    /// fall off. Per axis, so a snowfall can wrap horizontally while still dying at the floor.
    void wrap(draw::pos_t w, draw::pos_t h, bool wrapX = true, bool wrapY = true) {
        for (uint16_t i = 0; i < count; i++) {
            if (!ttl[i]) continue;
            if (wrapX && w > 0) x[i] = wrapCoord(x[i], w);
            if (wrapY && h > 0) y[i] = wrapCoord(y[i], h);
        }
    }

    /// Kill any particle that has left the `w` by `h` grid — the alternative to bouncing, for
    /// fountains and sparks that should simply be gone once they fall off.
    void killOutside(draw::pos_t w, draw::pos_t h, draw::pos_t margin = 0) {
        for (uint16_t i = 0; i < count; i++) {
            if (!ttl[i]) continue;
            if (x[i] < -margin || x[i] > w + margin || y[i] < -margin || y[i] > h + margin) ttl[i] = 0;
        }
    }

    // --- Emitters -------------------------------------------------------------------------------

    /// Emit `n` particles from a point in a cone around `angle`, at `speed` ± `spread`. The classic
    /// spark/firework burst. `seed` makes the pattern reproducible: two devices emitting on the same
    /// frame produce the same burst.
    void angleEmit(draw::pos_t px, draw::pos_t py, angle16 angle, draw::pos_t speed,
                   angle16 cone, uint8_t n, uint16_t life, uint8_t colour, uint32_t seed) {
        for (uint8_t k = 0; k < n; k++) {
            const uint16_t r1 = hashInt(k, seed, 1);
            const uint16_t r2 = hashInt(k, seed, 2);
            // Spread the emission across the cone rather than all along one ray.
            const angle16 a = static_cast<angle16>(angle + static_cast<int32_t>(r1 % (cone ? cone : 1)) - cone / 2);
            // Vary the speed so the burst has depth instead of a single expanding ring.
            const draw::pos_t s = speed - static_cast<draw::pos_t>(scaleSigned(speed, r2 & 0x3F, 256));
            const int32_t cx = static_cast<int32_t>(cos16(a));   // -32768..32767
            const int32_t sy = static_cast<int32_t>(sin16(a));
            if (!spawn(px, py,
                       static_cast<draw::pos_t>((cx * s) / 32768),
                       static_cast<draw::pos_t>((sy * s) / 32768),
                       life, colour)) return;                            // pool full: stop emitting
        }
    }

    /// Emit `n` particles from a point with random velocities inside a box — a fountain, a spray,
    /// a burst of confetti. `angleEmit` gives a directed cone; this gives an undirected scatter,
    /// which is what a splash or an explosion of debris actually looks like.
    void spray(draw::pos_t px, draw::pos_t py, draw::pos_t speed,
               uint8_t n, uint16_t life, uint8_t colour, uint32_t seed) {
        for (uint8_t k = 0; k < n; k++) {
            // hashInt rather than a stream RNG: two devices emitting on the same frame produce the
            // same spray without exchanging anything (the supersync rule).
            const int32_t rx = static_cast<int32_t>(hashInt(k, seed, 3) & 0x1FF) - 256;   // -256..255
            const int32_t ry = static_cast<int32_t>(hashInt(k, seed, 4) & 0x1FF) - 256;
            if (!spawn(px, py,
                       static_cast<draw::pos_t>((rx * speed) / 256),
                       static_cast<draw::pos_t>((ry * speed) / 256),
                       life, colour)) return;                     // pool full: stop emitting
        }
    }

    // --- Collisions -------------------------------------------------------------------------------
    //
    // OPTIONAL and off by default: an N-body check is the one part of the kernel whose cost is not
    // linear, so an effect that does not need particles to notice each other should not pay for it.
    //
    // Broad phase is a sort-free sweep along X ONLY. WLED-PS documents trying y-binning as well and
    // measuring it not worth the bookkeeping on this class of hardware, where pools are hundreds of
    // particles rather than thousands — so this keeps the cheaper structure deliberately, not by
    // omission. Narrow phase is an axis-separated distance test; the response is the textbook equal
    // -mass elastic impulse (exchange the velocity component along the line of centres).

    /// Make live particles bounce off each other. `radius` is the contact distance in sub-pixel
    /// units; `e` is restitution (256 = perfectly elastic, lower loses energy on every contact).
    ///
    /// Call BEFORE `step()`: resolving an overlap after integrating can push a particle outside the
    /// grid that the wall pass has already checked (the frame order at the top of this file).
    void collide(draw::pos_t radius, uint16_t e = 200, uint32_t seed = 0) {
        if (radius <= 0) return;
        const int64_t r2 = static_cast<int64_t>(radius) * radius;
        for (uint16_t i = 0; i < count; i++) {
            if (!ttl[i]) continue;
            for (uint16_t j = static_cast<uint16_t>(i + 1); j < count; j++) {
                if (!ttl[j]) continue;
                // Broad phase: reject on X before touching Y. Most pairs fail here, and this is the
                // whole reason the loop is affordable.
                const int32_t dx = x[j] - x[i];
                if (dx > radius || dx < -radius) continue;
                const int32_t dy = y[j] - y[i];
                if (dy > radius || dy < -radius) continue;
                // 64-bit: a large contact radius squares past int32 (dx and dy are sub-pixel).
                const int64_t d2 = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
                if (d2 > r2 || d2 == 0) continue;               // not touching, or exactly coincident

                // Elastic response along the line of centres, equal masses: the pair swaps the
                // component of velocity that points at the other particle and keeps the tangential
                // part. Scaled by `e` so contacts can lose energy.
                const int32_t d = static_cast<int32_t>(isqrt64(static_cast<uint64_t>(d2)));
                if (d == 0) continue;
                const int32_t nx = (dx * 256) / d;              // unit normal, 8.8
                const int32_t ny = (dy * 256) / d;
                const int32_t dvx = vx[j] - vx[i];
                const int32_t dvy = vy[j] - vy[i];
                const int32_t along = (dvx * nx + dvy * ny) / 256;
                if (along > 0) continue;                        // already separating; leave them be

                const int32_t impulse = scaleSigned(along, e, 256);
                vx[i] = static_cast<draw::pos_t>(vx[i] + (impulse * nx) / 256);
                vy[i] = static_cast<draw::pos_t>(vy[i] + (impulse * ny) / 256);
                vx[j] = static_cast<draw::pos_t>(vx[j] - (impulse * nx) / 256);
                vy[j] = static_cast<draw::pos_t>(vy[j] - (impulse * ny) / 256);

                // Separate the overlap by moving exactly ONE of the pair, chosen by a free
                // pseudo-random bit. Pushing both oscillates: each shove creates the overlap the
                // other resolves, and the pair jitters forever instead of settling (WLED-PS
                // documents the same trap).
                const int32_t overlap = radius - d;
                if (overlap > 0) {
                    const bool pushJ = (hashInt(i, j, 0, seed) & 1) != 0;
                    const int32_t ox = (nx * overlap) / 256;
                    const int32_t oy = (ny * overlap) / 256;
                    if (pushJ) { x[j] = static_cast<draw::pos_t>(x[j] + ox); y[j] = static_cast<draw::pos_t>(y[j] + oy); }
                    else       { x[i] = static_cast<draw::pos_t>(x[i] - ox); y[i] = static_cast<draw::pos_t>(y[i] - oy); }
                }
            }
        }
    }

    // --- Rendering ------------------------------------------------------------------------------

    /// Draw every live particle. Additive sub-pixel by default, so overlapping particles brighten
    /// and motion is smooth; `Hard` writes one whole pixel for the retro look.
    ///
    /// Brightness rides `ttl`, so a particle fades as it dies without the effect tracking a second
    /// quantity. Trails are deliberately NOT here: decay stays the layer's existing
    /// `fadeToBlackBy`, so the system has one decay path rather than a second hidden in the pool.
    void render(const draw::Canvas& cv, uint16_t maxLife = 255,
                RenderStyle style = RenderStyle::Splat) const {
        const uint16_t scale = maxLife == 0 ? 1 : maxLife;
        for (uint16_t i = 0; i < count; i++) {
            if (!ttl[i]) continue;
            const uint8_t bri = ttl[i] >= scale
                ? 255
                : static_cast<uint8_t>((static_cast<uint32_t>(ttl[i]) * 255u) / scale);
            const RGB c = colorFromPalette(*Palettes::active(), hue[i], bri);
            const uint8_t radius = size ? size[i] : 0;
            if (radius > 0) {
                // A sized particle is a disc, which is what makes a pool read as blobs rather than
                // as a scatter of points — the signature look of a particle system.
                draw::fillCircle(cv, static_cast<lengthType>(draw::toPixel(x[i])),
                                 static_cast<lengthType>(draw::toPixel(y[i])),
                                 static_cast<lengthType>(radius), c);
            } else if (style == RenderStyle::Splat) {
                draw::splat(cv, x[i], y[i], c);
            } else {
                draw::pixel(cv, {static_cast<lengthType>(draw::toPixel(x[i])),
                                 static_cast<lengthType>(draw::toPixel(y[i])), 0}, c);
            }
        }
    }

    /// How many particles are alive — for a status line, or an effect that tops the pool up.
    uint16_t liveCount() const {
        uint16_t n = 0;
        for (uint16_t i = 0; i < count; i++) if (ttl[i]) n++;
        return n;
    }
};

}  // namespace mm::particles
