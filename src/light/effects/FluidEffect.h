#pragma once

#include "light/effects/EffectBase.h"
#include "light/fluid.h"               // the Stam solver: the medium itself

namespace mm {

// Fluid: light poured into a simulated medium, and carried by it.
//
// Why a solver rather than another field: `light/fluid.h`. What it looks like and what the controls
// do: the catalog card. Here: how the three parts are wired.
//
// Three parts, and only the first is new:
//
//   - `Fluid` solves the medium (Stam 1999: diffuse, project, advect, project). Its own header
//     explains why that algorithm and why Q16.16.
//   - Jets pour velocity AND dye in together, so light enters where the medium is being pushed.
//   - `draw::advect16` carries the dye along the finished field, and `draw::decay16` fades it, which
//     is exactly what Trails does. The dye plane is 16-bit for the same reason: a value multiplied
//     by slightly less than one, many times a second, has nowhere to go at 8 bits.
//
// **Cost is the honest problem.** The solver is several passes over the grid per frame and the
// pressure solve is `iterations` of them, so this is a desktop and P4 effect. An S3 runs it on a
// small grid; the numbers per target are in performance.md rather than promised here.
// @card FluidEffect.png
/// Effect: dye poured into a simulated fluid, carried by the flow the medium itself works out.
class FluidEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🖌️🌊💨"; }   // power-function showcase
    Dim dimensions() const override { return Dim::D3; }   // a medium per slice, jets wander in z

    static constexpr uint8_t kMaxJets = 4;
    static constexpr uint8_t kClocks = 4;   ///< per jet: position, reach, aim, depth

    uint8_t jets        = 2;    // how many places light is poured in
    uint8_t force       = 120;  // how hard each one pushes
    uint8_t swirl       = 90;   // how fast the jets sweep, which is what makes vortices
    uint8_t viscosity   = 20;   // how much the medium drags on itself
    uint8_t persistence = 150;  // how long dye survives, as a half-life
    uint8_t iterations  = 5;    // pressure-solve effort, and the cost knob

    void defineControls() override {
        controls_.addControl("jets", jets, 1, kMaxJets);
        controls_.addControl("force", force, 0, 255);
        controls_.addControl("swirl", swirl, 0, 255);
        controls_.addControl("viscosity", viscosity, 0, 255);
        controls_.addControl("persistence", persistence, 0, 255);
        controls_.addControl("iterations", iterations, 1, 20);
    }

    void prepare() override {
        const lengthType w = width(), h = height(), d = depth();
        // On a cube every slice is its own medium and the jets wander through them, the same
        // per-slice shape Trails has: nothing is carried between slices, which a volumetric solve
        // would do and is a different solver. A panel is depth 1 and pays nothing for it.
        const bool medium = fluid_.resize(w, h, d);
        const size_t n = medium ? static_cast<size_t>(w) * h * d * 3 : 0;
        const size_t had = dyeA_.count();
        dyeA_.resize(n);
        dyeB_.resize(n);
        carry_.resize(n);        // the dither's error, sized here: tick() is MM_NONBLOCKING
        // Same sample count but a different shape (8x16 to 16x8, or a cube reshaped): resize() kept
        // the samples and they are laid out for the old geometry, so they would smear. BOTH planes,
        // because the ping-pong swaps the spare one in on the very next frame and clearing only the
        // live one leaves the stale picture one frame away. (A changed count needs no clear:
        // resize() zero-fills when it reallocates.) The guard Trails and Nebula carry.
        if (n > 0 && n == had && (w != planeW_ || h != planeH_ || d != planeD_)) {
            std::memset(dyeA_.data(), 0, dyeA_.bytes());
            std::memset(dyeB_.data(), 0, dyeB_.bytes());
            // The dither's error is per LIGHT, so it is laid out for the old geometry as much as
            // the dye is: carrying it into the new shape seeds the first frames with another
            // picture's rounding.
            if (carry_) std::memset(carry_.data(), 0, carry_.bytes());
        }
        planeW_ = w; planeH_ = h; planeD_ = d;
        started_ = false;
        pourCarry_ = 0;
        poured_ = false;
    }

    void tick() MM_NONBLOCKING override {
        // Readiness is READ from the buffers every frame, never cached: MoonModule::release()
        // frees them when this module or an ancestor is disabled, and the re-enable only requests
        // a prepare for the next loop, so one frame can tick between the two. A cached flag said
        // "ready" on that frame and pour() wrote to a null plane (SIGSEGV on the desktop).
        if (!fluid_.valid() || !dyeA_ || !dyeB_) return;
        const lengthType w = width(), h = height(), d = depth();
        const uint32_t now = elapsed();
        // A zero delta on the first tick: the whole uptime would push the solver in one step.
        const uint32_t dt = started_ ? now - lastMs_ : 0u;
        lastMs_ = now;
        started_ = true;

        // Each jet gets its own clocks: where it sits, how far out it wanders, which way it aims,
        // and on a cube which slice it is in. They are separate for the reason pour() documents.
        // Rates are deliberately not multiples of each other.
        for (uint8_t i = 0; i < kMaxJets; i++) {
            const uint16_t rate = static_cast<uint16_t>(3 + swirl / 12 + i * 2);
            bank_.set(static_cast<uint8_t>(i * kClocks + 0),
                      {.rate = rate, .low = 0, .high = 65535,
                       .phaseOffset = static_cast<angle16>(i * 16384), .wave = Wave::Saw});
            // The radius breathes from the middle out to near the wall, so the forcing covers the
            // panel over time instead of a band.
            bank_.set(static_cast<uint8_t>(i * kClocks + 1),
                      {.rate = static_cast<uint16_t>(rate / 3 + 1), .low = 0, .high = 255,
                       .phaseOffset = static_cast<angle16>(i * 9000), .wave = Wave::Sine});
            // And the aim swings around the tangent, so a jet drives across the middle rather than
            // only along its own orbit.
            bank_.set(static_cast<uint8_t>(i * kClocks + 2),
                      {.rate = static_cast<uint16_t>(rate / 2 + 1), .low = 0, .high = 65535,
                       .phaseOffset = static_cast<angle16>(i * 21000), .wave = Wave::Sine});
            // And on a cube the jet drifts through the slices, so each slice is visited in turn.
            bank_.set(static_cast<uint8_t>(i * kClocks + 3),
                      {.rate = static_cast<uint16_t>(rate / 4 + 1), .low = 0, .high = 65535,
                       .phaseOffset = static_cast<angle16>(i * 30000), .wave = Wave::Sine});
        }
        // advanceTo() takes an ABSOLUTE timestamp and computes its own delta (math16.h BeatPhase),
        // so passing this frame's dt feeds it the CHANGE in frame time: a few percent of the right
        // motion on a jittery device, and none at all where the frame time is steady.
        bank_.advanceTo(now);

        uint16_t* live = front_ ? dyeA_.data() : dyeB_.data();
        uint16_t* spare = front_ ? dyeB_.data() : dyeA_.data();

        // Paced by TIME, not by frames: a jet firing every frame pours at the framerate, so a fast
        // device fills the panel while a slow one trickles (measured 1.58 at 1200 fps against 60).
        // The dye is SET rather than accumulated, so scaling its brightness by dt does not work
        // either, which is the trap Trails' emitter documents. A budget with a carry is the fix.
        constexpr uint32_t kPourMs = 20;
        pourCarry_ += dt;
        // The first pour is unconditional: dt is deliberately 0 on the opening tick, so a purely
        // time-paced jet never fires on it, and on a fast device it then takes many frames to owe
        // a whole period. The panel stays black in the meantime, which a resize repeats every
        // time. One jet at startup is what the medium needs anyway.
        if (!poured_) {
            poured_ = true;                       // the opening pour, see above
            pour(live, w, h, d, now);
        }
        // Subtracting the period rather than taking a modulo of it: the remainder IS the time
        // already owed toward the next pour, and discarding it drops a different fraction at every
        // framerate. The debt is CAPPED, though: after a long stall (a WiFi scan, a filesystem
        // write) the owed time can be seconds, and pouring all of it in one frame is a burst that
        // both blows the frame budget and floods the medium. Beyond the cap the missed pours are
        // dropped, which is what "degrade visibly, never crash" means here.
        constexpr uint32_t kMaxCatchUp = 4;                  // 80 ms of jets in one frame, at most
        if (pourCarry_ > kPourMs * kMaxCatchUp) pourCarry_ = kPourMs * kMaxCatchUp;
        while (pourCarry_ >= kPourMs) {
            pourCarry_ -= kPourMs;
            pour(live, w, h, d, now);
        }

        // The medium first, then the dye it carries: the field must be solved before anything is
        // moved along it, or the dye would follow the PREVIOUS frame's flow.
        const int32_t dtQ = static_cast<int32_t>((static_cast<uint64_t>(dt) * Fluid::kOne) / 1000u);
        fluid_.step(static_cast<int32_t>(viscosity) * (Fluid::kOne / 4096), dtQ, iterations);

        const int32_t* vx = fluid_.velocityX();
        const int32_t* vy = fluid_.velocityY();
        const size_t plane = fluid_.plane();
        draw::advect16(spare, live, w, h, d,
                       [&](lengthType x, lengthType y, lengthType z, draw::pos_t& ox, draw::pos_t& oy) {
                           const size_t i = static_cast<size_t>(z) * plane + static_cast<size_t>(y) * w + x;
                           // Q16.16 cells per second into sub-pixels this frame: one cell is one
                           // light, and draw::pos_t counts 256 to the light.
                           ox = static_cast<draw::pos_t>((static_cast<int64_t>(vx[i]) * dtQ) >> 24);
                           oy = static_cast<draw::pos_t>((static_cast<int64_t>(vy[i]) * dtQ) >> 24);
                       }, draw::Edge::Clamp);
        front_ = !front_;
        uint16_t* dye = front_ ? dyeA_.data() : dyeB_.data();

        draw::decay16(dye, dyeA_.count(), 40u + static_cast<uint32_t>(persistence) * persistence / 10u, dt);
        draw::blit16(canvas(), dye, w, h, d, carry_ ? carry_.data() : nullptr);
    }

    /// Read-only access to the dye planes, for the reshape test: a rendered frame pours fresh dye
    /// over a stale plane, so the picture cannot tell a cleared plane from an uncleared one.
    std::size_t dyeSamples() const { return dyeA_.count() + dyeB_.count(); }
    uint16_t dyeAt(std::size_t i) const {
        const std::size_t n = dyeA_.count();
        if (i < n) return dyeA_.data()[i];
        const std::size_t j = i - n;
        return j < dyeB_.count() ? dyeB_.data()[j] : 0;   // past the end reads 0, never off it
    }

private:
    /// The jets: velocity and dye together, so light enters where the medium is pushed.
    void pour(uint16_t* dye, lengthType w, lengthType h, lengthType d, uint32_t now) {
        const uint8_t n = jets < 1 ? 1 : (jets > kMaxJets ? kMaxJets : jets);
        for (uint8_t i = 0; i < n; i++) {
            const angle16 a = static_cast<angle16>(bank_.unitValue(static_cast<uint8_t>(i * kClocks + 0)));
            // The radius wanders between the center and the wall rather than sitting on one circle.
            const uint32_t reach = bank_.value(static_cast<uint8_t>(i * kClocks + 1));
            const int32_t rx = static_cast<int32_t>((w / 2 - 1) * reach / 320u);
            const int32_t ry = static_cast<int32_t>((h / 2 - 1) * reach / 320u);
            const lengthType jx = static_cast<lengthType>(w / 2 + (static_cast<int32_t>(cos16(a)) * rx) / 32768);
            const lengthType jy = static_cast<lengthType>(h / 2 + (static_cast<int32_t>(sin16(a)) * ry) / 32768);
            // The aim swings either side of the tangent, so the jet has a radial component and
            // drives dye through the middle. Pure tangent is what produced a hollow ring.
            const int32_t lean = static_cast<int32_t>(bank_.value(static_cast<uint8_t>(i * kClocks + 2))) - 32768;
            const lengthType jz = d > 1
                ? static_cast<lengthType>((bank_.value(static_cast<uint8_t>(i * kClocks + 3)) * (d - 1)) / 65535u)
                : 0;
            // Odd jets sweep the other way, so they meet the even ones head-on: colliding jets are
            // what roll up vortex pairs, while jets all turning together sum into one rotation.
            const int32_t sense = (i & 1) ? -1 : 1;
            const angle16 dir = static_cast<angle16>(a + static_cast<angle16>(sense * (16384 + lean / 3)));
            // The push covers the same disc as the dye and scales with the fixture, in cells per
            // second. A single-cell push is mostly divergence, which the projection removes, and
            // `force / 64` cells/s on a 64-wide panel measured a mean of 0.45 cells/s: the dye sat
            // where it was poured and decayed in place. At this scale the nozzle peaks near 1.5
            // lights per frame, which is what reads as paint being jetted rather than dabbed.
            const lengthType span = w > h ? w : h;
            const lengthType rad = span / 32 + 1;
            const lengthType rz = d > 1 ? rad : 0;             // a sphere on a cube, a disc on a panel
            const int32_t mag = static_cast<int32_t>(
                (static_cast<int64_t>(force) * span * Fluid::kOne) / (255 * 2));
            const int32_t dvx = static_cast<int32_t>((static_cast<int64_t>(cos16(dir)) * mag) >> 15);
            const int32_t dvy = static_cast<int32_t>((static_cast<int64_t>(sin16(dir)) * mag) >> 15);
            for (lengthType dz = -rz; dz <= rz; dz++)
                for (lengthType dy = -rad; dy <= rad; dy++)
                    for (lengthType dx = -rad; dx <= rad; dx++)
                        if (dx * dx + dy * dy + dz * dz <= rad * rad)
                            fluid_.addVelocity(jx + dx, jy + dy, dvx, dvy, jz + dz);

            const RGB c = colorFromPalette(*Palettes::active(),
                                           static_cast<uint8_t>(i * (255u / n) + (now >> 6)));
            splatDye(dye, w, h, d, jx, jy, jz, c);
        }
    }

    /// Dye into a soft disc, so a jet has a body rather than a single pixel the flow immediately
    /// spreads to nothing. Written at the plane's full width, which is where the decay lives.
    void splatDye(uint16_t* dye, lengthType w, lengthType h, lengthType d,
                  lengthType cx, lengthType cy, lengthType cz, RGB c) {
        // The jet's body scales with the fixture. A fixed radius is a fixed NUMBER of lights, and
        // advection spreads them over an area that grows with the grid, so on a large panel the dye
        // arrives at a fraction of a count and the whole effect reads as a faint smear. This is the
        // same arithmetic Trails' emitter head carries, for the same reason.
        const lengthType span = w > h ? w : h;
        const lengthType rad = span / 32 + 1;
        const lengthType rz = d > 1 ? rad : 0;                 // a sphere on a cube, a disc on a panel
        const int32_t r2 = static_cast<int32_t>(rad) * rad;
        const uint16_t wide[3] = {static_cast<uint16_t>((c.r << 8) | c.r),
                                  static_cast<uint16_t>((c.g << 8) | c.g),
                                  static_cast<uint16_t>((c.b << 8) | c.b)};
        for (lengthType dz = -rz; dz <= rz; dz++)
          for (lengthType dy = -rad; dy <= rad; dy++)
            for (lengthType dx = -rad; dx <= rad; dx++) {
                // A DISC, not a square. A square block is an axis-aligned slab, and advection
                // carries its corners into the blocky patches the panel shows as squares.
                const int32_t d2 = static_cast<int32_t>(dx) * dx + static_cast<int32_t>(dy) * dy
                                 + static_cast<int32_t>(dz) * dz;
                if (d2 > r2) continue;
                const lengthType x = cx + dx, y = cy + dy, z = cz + dz;
                if (x < 0 || y < 0 || z < 0 || x >= w || y >= h || z >= d) continue;
                // Full at the center, tapering to nothing at the rim: a flat slab has a hard edge
                // that stays hard however far the flow carries it.
                const uint32_t fall = static_cast<uint32_t>(((r2 - d2) * 255) / (r2 > 0 ? r2 : 1));
                const size_t o = ((static_cast<size_t>(z) * h + y) * w + x) * 3;
                for (size_t k = 0; k < 3; k++) {
                    // ADDED, not assigned. Overwriting stamps a solid block of one color, which is
                    // why the jets read as flat patches rather than as dye entering a medium;
                    // accumulating lets a jet build up where it lingers and blend where two meet.
                    const uint32_t add = (static_cast<uint32_t>(wide[k]) * fall) / 255u;
                    const uint32_t sum = static_cast<uint32_t>(dye[o + k]) + add;
                    dye[o + k] = static_cast<uint16_t>(sum > 65535u ? 65535u : sum);
                }
            }
    }


    Fluid                   fluid_{*this};
    ScratchBuffer<uint16_t> dyeA_{*this};     ///< the dye; the two alternate roles
    ScratchBuffer<uint16_t> dyeB_{*this};
    ScratchBuffer<uint8_t>  carry_{*this};    ///< the dither's per-channel error
    OscillatorBank<kMaxJets * kClocks> bank_;
    bool                    front_ = true;
    bool                    started_ = false;
    uint32_t                lastMs_ = 0;
    uint32_t                pourCarry_ = 0;   ///< time owed to the jets, in ms
    lengthType              planeW_ = 0, planeH_ = 0, planeD_ = 0;   ///< the shape the planes hold
    bool                    poured_ = false;  ///< has the opening pour happened yet
};

}  // namespace mm
