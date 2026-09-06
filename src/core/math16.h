#pragma once

// 16-bit fixed-point tier: the power-function contract's numeric vocabulary.
//
// Why a 16-bit tier alongside math8.h: an 8-bit result positions to 256 levels, which visibly steps
// on a large fixture (a 12K-light wall shows the staircase in a slow gradient or a slow-moving
// blob). Effects must look smooth at every size, so the contract every effect and script writes
// against is 16-bit; math8.h stays for genuinely 8-bit domains (palette index and hue are mod-256 by
// design) and as the internal fast path.
//
// Cost discipline, measured rather than assumed: interpolating the existing 8-bit `sin8_lut` was
// tried first (zero new bytes) and REJECTED: rounding the endpoints to 8 bits distorts the segments
// the interpolation runs between, giving 1.1% of amplitude, worse than the 0.69% of FastLED's
// classic `lib8tion` sin16. A 130-byte quarter-wave 16-bit table with the same linear interpolation
// measures 0.031%: 22x better than lib8tion at a cost that rounds to nothing. FastLED master's
// `fl::sin32` is near-exact but spends 1040 bytes plus two int64 multiplies per call; this is the
// middle that keeps large-fixture gradients smooth without that. A quadratic core can swap in behind
// this same name if a field-heavy effect ever needs it.

#include "core/math8.h"

#include <cstdint>

namespace mm {

// ---- Angles and fractions --------------------------------------------------
// angle16: 65536 = one full turn. Overflow IS the wrap, so phase arithmetic never needs a modulo.
using angle16 = uint16_t;
// frac16: 0..65535 as a 0..1 fraction (interpolation weights, easing in/out).
using frac16  = uint16_t;

/// Quarter-wave sine table: sin(pi/2 * i/64) * 32767, 65 entries (0..64 inclusive, so the last
/// segment has both endpoints). 130 bytes in flash; the other three quadrants come from symmetry.
inline constexpr int16_t sin16_quarter[65] = {
        0,   804,  1608,  2410,  3212,  4011,  4808,  5602,
     6393,  7179,  7962,  8739,  9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767
};

/// Sine over a 16-bit angle, returning SIGNED -32767..32767: FastLED's sin16 contract.
///
/// Verified against the two codebases users port FROM, at master rather than at a release, because
/// FastLED 4.0 is close and the contract we bind to should be the one users will have:
///   - FastLED master (6a120dedc9, 2026-08-07): `fl/math/sin32.h` declares `i16 sin16lut(u16)`,
///     commented "output is between -32767 and 32767".
///   - WLED main (c1838ed4, 2026-08-05): `constexpr int16_t (*sinFunction)(uint16_t) = &sin16_t`,
///     and its effects write `sin16_t(x) + 32768` when they want an unsigned value: precisely the
///     arithmetic that broke silently against our old unsigned return.
///
/// The unsigned 0..65535 form (32768 at the zero crossing) was tried first, to match `sin8`. It is
/// the wrong call: `sin16` is the most-ported symbol in the LED world, so any FastLED or WLED
/// snippet doing arithmetic on it breaks SILENTLY, everything offset by half scale. Eleven of our
/// own call sites were already subtracting 32768 to undo the convention, which is the same signal
/// from the inside. A standard name must carry the standard contract; `sin8` keeps its own because
/// 8-bit palette/hue domains are genuinely unsigned.
///
/// Quarter-wave lookup with linear interpolation: the top 2 bits of the angle pick the quadrant,
/// the next 6 the table entry, the low 8 the position between entries. Mirroring the index in odd
/// quadrants and negating in the upper half reconstructs the full wave from a quarter of the table.
constexpr int16_t sin16(angle16 theta) {
    const uint8_t quadrant = static_cast<uint8_t>(theta >> 14);       // 0..3
    const uint16_t pos     = static_cast<uint16_t>(theta & 0x3FFF);   // position within the quadrant
    // Odd quadrants run the quarter wave backwards (sin descends from the peak).
    const uint16_t walk = (quadrant & 1) ? static_cast<uint16_t>(0x4000 - pos) : pos;
    // uint16 for the index, NOT uint8: walk reaches 0x4000 exactly at a quadrant boundary in an odd
    // quadrant, and >>8 is then 64: which a uint8 holds fine but only by luck of the cast order.
    // Keeping it wide makes the "table has 65 entries so idx==64 is valid" invariant explicit.
    const uint16_t idx  = static_cast<uint16_t>(walk >> 8);           // 0..64
    const uint8_t frac  = static_cast<uint8_t>(walk & 0xFF);
    const int32_t a = sin16_quarter[idx];
    // At idx==64 the wave is exactly at the peak and frac is 0, so the b term contributes nothing;
    // clamping keeps the read inside the 65-entry table without a branch on the hot path.
    const int32_t b = sin16_quarter[idx < 64 ? idx + 1 : 64];
    int32_t v = a + (((b - a) * frac) >> 8);
    if (quadrant >= 2) v = -v;                                        // lower half of the circle
    return static_cast<int16_t>(v);
}

/// Cosine: a quarter turn ahead of sine. Signed, like `sin16`.
constexpr int16_t cos16(angle16 theta) { return sin16(static_cast<angle16>(theta + 16384)); }

// ---- Range mapping ---------------------------------------------------------

/// Map `v` from [inLo,inHi] to [outLo,outHi] in 64-bit intermediate, clamped to the input range.
///
/// The fencepost lives HERE, once: six effects hand-rolled this and each carried its own comment
/// about the off-by-one (mapping to `n-1` vs `n` when the output is a grid extent). Callers map to
/// the extent (`0..w`) and the result is clamped to `w-1` by the pixel writer, which is the form
/// that does not lose the last column.
constexpr int32_t map32(int32_t v, int32_t inLo, int32_t inHi, int32_t outLo, int32_t outHi) {
    if (inHi == inLo) return outLo;                       // zero span: no meaningful ratio
    if (inHi > inLo) { if (v <= inLo) return outLo; if (v >= inHi) return outHi; }
    else             { if (v >= inLo) return outLo; if (v <= inHi) return outHi; }
    // Every operand widens BEFORE arithmetic: a full-width span (INT32_MIN..INT32_MAX) does not fit
    // in an int32 subtraction, so computing the spans in 32 bits overflows at the extremes. The
    // product of two 32-bit spans fits in int64 for every range this maps between: an extent, a
    // byte, a band count, a frequency. Mapping a FULL int32 range onto another full int32 range
    // would exceed it (offset and outSpan both approach 2^32), which no caller does: the widest
    // real case is a 16-bit phase onto a grid extent, five orders of magnitude short of it.
    const int64_t inSpan  = static_cast<int64_t>(inHi)  - static_cast<int64_t>(inLo);
    const int64_t outSpan = static_cast<int64_t>(outHi) - static_cast<int64_t>(outLo);
    const int64_t num     = (static_cast<int64_t>(v) - static_cast<int64_t>(inLo)) * outSpan;
    return static_cast<int32_t>(static_cast<int64_t>(outLo) + num / inSpan);
}

// ---- Beat phase ------------------------------------------------------------

/// A resolution- and framerate-independent BPM phase accumulator.
///
/// Nine effects hand-rolled this identically, and the shape is subtle enough to be worth owning
/// once: the per-tick product `dt * bpm * scale / 60000` rounds to ZERO when dt is under a
/// millisecond (every desktop frame, and an ESP32 running a small fixture fast), so the animation
/// silently freezes. The fix all nine converged on is to accumulate the RAW numerator in 64 bits and
/// divide only at the read: which is what this does.
///
/// Usage: one member per animated quantity; call `advanceTo(nowMs, rate)` once per frame, then
/// read as often as needed. `rate` is BPM-like: the caller's speed control, whatever its units.
class BeatPhase {
public:
    /// Accumulate this frame's contribution. Safe to call with a rate of 0 (the phase holds).
    /// The first call only establishes the time base, so a large `elapsed` at startup cannot jump
    /// the phase: the same first-tick guard three of the nine effects carried by hand.
    ///
    /// Takes the CURRENT TIME, not a frame delta: the delta is computed here. Named `advanceTo`
    /// for that reason, because `advance(dt)` reads naturally and is wrong, which four of five
    /// oscillator callers wrote before the name said otherwise. Passing a delta feeds this the
    /// change in frame time, so the phase creeps on a jittery device and stops dead on a steady
    /// one; the effects it drove looked frozen while every test stayed green.
    void advanceTo(uint32_t nowMs, uint32_t rate) {
        if (!started_) { started_ = true; lastMs_ = nowMs; return; }
        const uint32_t dt = nowMs - lastMs_;       // unsigned: correct across the millis() wrap
        lastMs_ = nowMs;
        num_ += static_cast<uint64_t>(dt) * rate;
    }

    /// The phase scaled by `scale` and divided late: `phase(256)` is the uint8 angle form
    /// (256 = full turn) the effects use, `phase(65536)` the angle16 form.
    /// Returns the raw scaled value; the caller truncates to its angle width, which is where the
    /// free wrap happens.
    uint32_t phase(uint32_t scale) const {
        return static_cast<uint32_t>((num_ * scale) / 60000u);
    }

    /// The undivided numerator, for a caller that scales differently (NoiseEffect multiplies the
    /// rate by the grid width before accumulating; DistortionWaves reads one accumulator at two
    /// scales for its second axis).
    uint64_t numerator() const { return num_; }

    /// Feed a pre-scaled product directly: for the callers whose rate already carries a factor.
    void advanceScaled(uint32_t elapsedMs, uint64_t scaledRate) {
        if (!started_) { started_ = true; lastMs_ = elapsedMs; return; }
        const uint32_t dt = elapsedMs - lastMs_;
        lastMs_ = elapsedMs;
        num_ += static_cast<uint64_t>(dt) * scaledRate;
    }

    void reset() { num_ = 0; lastMs_ = 0; started_ = false; }

private:
    uint64_t num_ = 0;        ///< raw dt·rate numerator; divided by 60000 only at the read
    uint32_t lastMs_ = 0;
    bool     started_ = false;
};

/// Integer square root of a 32-bit value, rounded down. Binary restoring method: no divide, no
/// float, ~16 iterations worst case: which matters because the ESP32 has no fast divide and effects
/// call this per pixel when they need a true distance.
///
/// PaintBrushEffect hand-rolled this; it lives here so a caller reaching for a distance gets the
/// same one. Where a comparison against a threshold will do (is this point inside a circle?), prefer
/// comparing SQUARED values and skip this entirely: measured on hardware, the squared form is ~14
/// cycles/pixel against ~108 for the sqrt.
/// Integer square root of a 64-bit value: the wide form, for values whose square exceeds 32 bits.
/// `dist16` needs it: two coordinates of only 70000 already square-and-sum past UINT32_MAX, so the
/// 32-bit form saturated at 65535 and reported the wrong distance well inside normal range.
///
/// Newton's method, which converges in a handful of steps over this range. Promoted here from
/// AudioLevel.h (its RMS path is the other caller) so the integer roots have one home.
constexpr uint64_t isqrt64(uint64_t x) {
    if (x < 2) return x;                      // 0 and 1 are their own roots
    // Start from a power-of-two bound rather than from x itself: the Newton step `r + x/r`
    // OVERFLOWS when r starts at x, which for UINT64_MAX made the first iteration wrap and the
    // function return 0. Halving the bit width gives a starting point above the true root and
    // inside the range, so every iteration stays representable.
    uint64_t r = 1;
    for (uint64_t v = x; v > 0; v >>= 2) r <<= 1;
    uint64_t last;
    do {
        last = r;
        r = (r + x / r) >> 1;
    } while (r < last);
    return last;
}

constexpr uint32_t isqrt(uint32_t v) {
    uint32_t rem = v, root = 0, bit = 1u << 30;
    while (bit > rem) bit >>= 2;
    while (bit) {
        if (rem >= root + bit) { rem -= root + bit; root = (root >> 1) + bit; }
        else                   { root >>= 1; }
        bit >>= 2;
    }
    return root;
}

// --- Polar ---------------------------------------------------------------------------------
//
// `atan2_8`/`dist8` in math8.h are the 8-bit forms: an octant atan2 and an OCTAGONAL distance
// (max + half-min), which is up to ~12% off a true radius and saturates at 255. On a small panel
// neither shows; on a large one the octagon reads as visible corners on what should be a circle,
// and the saturation flattens everything past 255 lights from the center.
//
// These are the 16-bit forms for addressing a grid in polar coordinates: the vocabulary behind
// every radial look: rings, spirals, rotation, kaleidoscopes, tunnels, radial wipes, a spectrum
// bent around a circle. `atan16` returns an angle16 (65536 = one turn) so it feeds sin16 and the
// palette directly; `dist16` returns a true Euclidean radius via isqrt. Both are general grid
// arithmetic, not helpers for the handful of effects that happen to call them first.

/// Arctangent over one octant: atan(i/32)/atan(1) · 8192, so entry 32 closes the octant exactly at
/// an eighth turn. 33 entries, 66 bytes in flash; the other seven octants come from symmetry.
inline constexpr int16_t atan16_octant[33] = {
        0,   326,   651,   975,  1297,  1617,  1933,  2246,
     2555,  2860,  3159,  3453,  3742,  4025,  4302,  4572,
     4836,  5094,  5344,  5589,  5826,  6058,  6282,  6500,
     6712,  6917,  7117,  7310,  7498,  7679,  7856,  8026,
     8192
};

/// Angle of (x, y) as an angle16, measured counter-clockwise from +x. Full 16-bit resolution, so a
/// gradient swept around the circle has no visible steps on a large fixture.
inline angle16 atan16(int32_t y, int32_t x) {
    if (x == 0 && y == 0) return 0;                       // the center has no direction
    // Fold into the first octant, remembering which one, then interpolate the arctangent there.
    // Unsigned magnitudes: negating INT32_MIN has no int32 representation, so the obvious
    // `x < 0 ? -x : x` is undefined behavior at exactly one input per axis. Widening first keeps
    // the fold exact across the whole range.
    //
    // `0u - v` rather than `-v`: the unary form is well-defined on an unsigned (it is modular
    // arithmetic, which is exactly what is wanted here), but MSVC reports it as C4146 and the
    // Windows build treats warnings as errors. Subtracting from zero is the same operation with no
    // diagnostic on any compiler.
    uint32_t ax = x < 0 ? 0u - static_cast<uint32_t>(x) : static_cast<uint32_t>(x);
    uint32_t ay = y < 0 ? 0u - static_cast<uint32_t>(y) : static_cast<uint32_t>(y);
    const bool swap = ay > ax;
    if (swap) { const uint32_t t = ax; ax = ay; ay = t; }
    // ratio = ay/ax in 0..65535; within one octant arctan is near-linear, so a linear read with a
    // small cubic correction is well inside a pixel of error at any grid size we drive.
    const uint32_t ratio = static_cast<uint32_t>((static_cast<uint64_t>(ay) << 16) / (ax ? ax : 1u));
    // Table lookup with linear interpolation, the same shape as sin16 above and for the same reason:
    // a fitted polynomial was tried first and measured 9.6 degrees of error at the octant boundary,
    // where a 66-byte table is exact at every entry and closes precisely at 8192.
    const uint16_t idx  = static_cast<uint16_t>(ratio >> 11);           // 0..32
    const uint16_t frac = static_cast<uint16_t>((ratio >> 3) & 0xFF);
    const int32_t lo = atan16_octant[idx];
    const int32_t hi = atan16_octant[idx < 32 ? idx + 1 : 32];
    uint32_t oct = static_cast<uint32_t>(lo + (((hi - lo) * frac) >> 8));
    uint16_t a = swap ? static_cast<uint16_t>(16384 - oct) : static_cast<uint16_t>(oct);
    if (x < 0) a = static_cast<uint16_t>(32768 - a);                    // reflect into quadrant 2/3
    if (y < 0) a = static_cast<uint16_t>(65536 - a);                    // and below the axis
    return static_cast<angle16>(a);
}

/// True Euclidean distance from the origin to (dx, dy): a real radius, not the octagon `dist8`
/// approximates, and it does not saturate at 255.
inline uint32_t dist16(int32_t dx, int32_t dy) {
    // Squared in UNSIGNED 64-bit and rooted with the 64-bit isqrt. The obvious 32-bit form is wrong
    // far inside normal range, not just at the extremes: two coordinates of 70000 already square-
    // and-sum past UINT32_MAX, so it saturated and reported 65535 for a distance of 70000. Taking
    // the magnitude first also keeps INT32_MIN representable, which negating in place does not.
    const uint64_t ax = static_cast<uint64_t>(dx < 0 ? -static_cast<int64_t>(dx) : dx);
    const uint64_t ay = static_cast<uint64_t>(dy < 0 ? -static_cast<int64_t>(dy) : dy);
    const uint64_t d2 = ax * ax + ay * ay;
    const uint64_t d = isqrt64(d2);
    return static_cast<uint32_t>(d > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : d);
}

// --- Easing --------------------------------------------------------------------------------
//
// Robert Penner's easing curves, the industry-standard names. An easing takes a 0..65535 progress
// and bends it, so motion accelerates and settles instead of running at constant speed. Anything
// that travels, grows, fades or transitions reads better through one; a linear ramp is the thing
// that makes an animation look mechanical.

/// Quadratic ease in-out: accelerate from rest, decelerate to rest. The default choice.
constexpr frac16 easeInOutQuad(frac16 t) {
    const uint32_t v = t;
    if (v < 32768) return static_cast<frac16>((2u * v * v) >> 16);
    const uint32_t u = 65535u - v;
    return static_cast<frac16>(65535u - ((2u * u * u) >> 16));
}

/// Cubic ease in-out: the same shape with a longer, softer settle.
constexpr frac16 easeInOutCubic(frac16 t) {
    const uint64_t v = t;
    if (v < 32768) return static_cast<frac16>((4ull * v * v * v) >> 32);
    const uint64_t u = 65535ull - v;
    return static_cast<frac16>(65535ull - ((4ull * u * u * u) >> 32));
}

/// Quadratic ease out: fast start, gentle settle. The "arrives and rests" curve.
///
/// The squared term divides by 65535 rather than shifting by 16: `(65535·65535) >> 16` is 65534,
/// which would leave the curve one unit short at t=0 and start a fade barely lit.
constexpr frac16 easeOutQuad(frac16 t) {
    const uint32_t u = 65535u - t;
    return static_cast<frac16>(65535u - ((u * u) / 65535u));
}

// --- Followers and meters ---------------------------------------------------------------------

/// A one-pole low-pass follower: `current` moves a fraction of the way toward `target` each frame.
/// This is how a value stops jittering: a meter, a control, a beat-driven size. `rate` is 0..255,
/// where 255 snaps instantly and small values glide.
///
/// Deliberately frame-rate dependent in its simple form, which is what every LED codebase uses; a
/// caller needing true time-independence scales `rate` by its own dt.
constexpr uint8_t smoothFollow(uint8_t current, uint8_t target, uint8_t rate) {
    if (rate == 0 || current == target) return current;
    if (rate == 255) return target;                   // full rate arrives, rather than stopping one short
    const int32_t delta = static_cast<int32_t>(target) - current;
    // Round the step AWAY from zero so every nonzero rate makes progress. A plain shift truncates
    // toward zero, which for a small rate means an upward step rounds to 0 and the value never
    // rises, while a downward step of the same size rounds to -1 and does move: a follower that
    // could fall but not climb (measured: rate 1 moved 100->99 but left 0 at 0).
    const int32_t step = (delta * rate) / 256;
    if (step != 0) return static_cast<uint8_t>(current + step);
    return static_cast<uint8_t>(current + (delta > 0 ? 1 : -1));
}

/// A meter's ballistic: rise at one rate, fall at another. `smoothFollow` with two time constants.
///
/// A follower with a single rate is the wrong instrument for anything reactive: it makes the attack
/// as sluggish as the decay and rounds off the transient the effect exists to show. Every meter
/// standard separates the two, and a broadcast peak programme meter (PPM, IEC 60268-10) is the
/// shape wanted here: rise almost at once, fall over a comfortable time so the eye can read the
/// peak. WLED, FastLED and LedFx each arrived at the same asymmetric form independently.
///
/// `rise` and `fall` are `smoothFollow` rates (0 = frozen, 255 = instant). Equal values reduce to
/// `smoothFollow` exactly, so this is a superset rather than a second mechanism.
constexpr uint8_t ballistic(uint8_t current, uint8_t target, uint8_t rise, uint8_t fall) {
    return smoothFollow(current, target, target > current ? rise : fall);
}

/// The falling-peak meter: rise INSTANTLY to a new high, then decay slowly. The asymmetry is the
/// whole point: a peak that eased upward would miss transients, and one that dropped instantly
/// would show nothing to read. Every VU meter with a floating peak dot is this function.
constexpr uint8_t peakHold(uint8_t peak, uint8_t value, uint8_t decay) {
    if (value > peak) return value;                    // instant attack
    return peak > decay ? static_cast<uint8_t>(peak - decay) : 0;
}

// --- Position-addressable randomness -----------------------------------------------------------

/// A hash of up to four integers to 0..65535: random-LOOKING but a pure function of its inputs, so
/// the same pixel gets the same value on every device and every frame.
///
/// This is what a stream RNG cannot do: `Random8` advances per CALL, so a device that renders one
/// extra frame or has a different light count diverges forever. Addressing randomness by position
/// instead keeps a dissolve, a starfield or a per-pixel offset identical across devices, which is
/// the supersync requirement (see the determinism section in the power-function docs).
constexpr uint16_t hashInt(uint32_t a, uint32_t b = 0, uint32_t c = 0, uint32_t seed = 0) {
    uint32_t h = a * 0x9E3779B1u;                      // the golden-ratio constant, standard mixer
    h ^= (b + 0x85EBCA6Bu + (h << 6) + (h >> 2));
    h ^= (c + 0xC2B2AE35u + (h << 6) + (h >> 2));
    h ^= (seed + 0x27D4EB2Fu + (h << 6) + (h >> 2));
    h ^= h >> 15; h *= 0x2545F491u; h ^= h >> 13;      // avalanche
    return static_cast<uint16_t>(h >> 16);
}

/// Fold an angle into `segments` mirrored wedges: the kaleidoscope. Any field sampled through
/// this gains n-fold symmetry, which is why a mediocre noise field becomes a mandala for the cost
/// of one modulo: the fold is applied to the ANGLE, and everything downstream is unchanged.
///
/// Mirroring alternate wedges (rather than merely repeating them) is what makes the seams join;
/// a plain repeat leaves a visible discontinuity at every wedge boundary.
inline angle16 kaleido(angle16 a, uint8_t segments) {
    if (segments < 2) return a;                        // one segment is the identity
    const uint32_t wedge = 65536u / segments;
    uint32_t within = a % wedge;                       // position inside this wedge
    const uint32_t index = a / wedge;
    // Reflect alternate wedges and return the FOLDED coordinate: deliberately one wedge wide, not
    // the original angle. That is what a kaleidoscope is: every wedge maps onto the same range, so a
    // field sampled through it repeats n times around the circle, and mirroring every other wedge is
    // what makes the seams join rather than showing a hard edge. A caller that wants the full turn
    // simply does not fold.
    //
    // The `- 1` matters: `wedge - within` maps 0 to `wedge`, one past the end, which puts a
    // one-unit discontinuity at every seam.
    if (index & 1) within = wedge - 1 - within;
    return static_cast<angle16>(within);
}

// --- 16-bit waveforms ------------------------------------------------------------------------
// The 8-bit forms in math8.h cap at 255, which is short of the fixtures this drives: a 128x128 wall
// is 16384 lights, and an animation indexed through a 0..255 ramp quantises to coarse steps across
// it. These are the same textbook shapes at full 16-bit range, so a position scales to any axis
// length without the caller rescaling.

// Triangle wave: 0 to 65535 over the first half of the cycle and back over the second. The fold of
// a ramp, the 16-bit twin of triwave8. Cheaper and sharper than a sine where an effect wants a
// linear sweep out and back.
constexpr uint16_t triwave16(uint16_t i) {
    return i < 32768 ? static_cast<uint16_t>(i * 2)
                     : static_cast<uint16_t>((65535 - i) * 2);
}

// beat16: a 0..65535 sawtooth completing `bpm` cycles per minute, measured from `timebase`.
// The 16-bit twin of beat8: same FastLED semantics, full range, so `beat16(bpm) * n >> 16` lands
// on any axis length evenly rather than in 1/256ths.
constexpr uint16_t beat16(uint8_t bpm, uint32_t ms, uint32_t timebase = 0) {
    if (bpm == 0) return 0;
    const uint32_t period = 60000u / bpm;
    if (period == 0) return 0;
    const uint32_t pos = (ms - timebase) % period;
    return static_cast<uint16_t>((pos * 65536u) / period);
}

/// Half-life decay: the fraction of a value that SURVIVES after `dtMs`, as a 0..65536 weight.
///
/// `0.5^(dt / halfLifeMs)`, the textbook exponential decay written so the caller states the thing
/// they actually mean: "half of it is gone after N milliseconds". Framerate independence is then a
/// property of the formula rather than of the caller's arithmetic, because the exponent carries dt:
/// two 10 ms steps and one 20 ms step reach the same place, which `decay(2·dt) == decay(dt)²` pins.
///
/// This exists because the per-frame form does not survive a fast device. A "keep 240 of 255 each
/// frame" fade is a decay whose half-life moves with the framerate, so the same setting is a long
/// tail at 60 fps and an instant clear at 1200; the effects that hit this carry a remainder by hand
/// (ParticlesEffect's `fadeCarry_`, and the Layer's own) to stop the fade truncating to nothing.
///
/// Returns 65536 (unchanged) for a zero dt or a zero half-life, and 0 once the value could not
/// survive rounding anyway: a caller multiplies by this and shifts down 16.
inline uint32_t halfLifeKeep(uint32_t dtMs, uint32_t halfLifeMs) {
    if (dtMs == 0 || halfLifeMs == 0) return 65536;
    // 2^-x by table lookup on the fraction, then a shift for the whole halvings. 33 entries at
    // 1/32 of a half-life, linearly interpolated: measured worst error 3.95 of 65536 (0.006%),
    // which is under one part in 255 at the byte width every caller narrows to.
    // Held as the DROP below 65536, so the first entry (a full 65536, one past uint16_t) fits the
    // same 16-bit table as the rest rather than widening every entry to carry one value.
    static constexpr uint16_t kPow2Drop[33] = {
            0,  1404,  2779,  4123,  5439,  6727,  7987,  9220,
        10427, 11608, 12763, 13894, 15001, 16084, 17143, 18180,
        19195, 20188, 21160, 22111, 23041, 23952, 24843, 25715,
        26568, 27403, 28220, 29020, 29802, 30568, 31317, 32050,
        32768,
    };
    // How many half-lives have passed, in 1/32nds. A long stall shifts the result to zero rather
    // than wrapping: 32 halvings is already below one part in 4 billion.
    const uint64_t units = (static_cast<uint64_t>(dtMs) * 32u) / halfLifeMs;
    const uint32_t whole = static_cast<uint32_t>(units >> 5);
    if (whole >= 32) return 0;
    const uint32_t i = static_cast<uint32_t>(units & 31u);
    // Interpolate between the table's entries on the sub-1/32 remainder, which the multiply above
    // has already discarded, so recover it at the finer scale the division allows.
    const uint64_t fine = (static_cast<uint64_t>(dtMs) * 32u * 256u) / halfLifeMs;
    const uint32_t f = static_cast<uint32_t>(fine & 255u);
    const uint32_t a = kPow2Drop[i], b = kPow2Drop[i + 1];
    const uint32_t drop = a + (((b - a) * f) >> 8);
    return (65536u - drop) >> whole;
}

}  // namespace mm
