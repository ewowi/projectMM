#pragma once
// Pacman: the arcade cast crossing the wall. Pacman chomps along with the four ghosts, each in
// its own color, wrapping around the edges forever.
//
// ITERATION 1 (this one): the characters move independently and do not notice each other. The
// maze, the pellets and the chase are iteration 2; the shapes, the animation and the movement
// grid below are what that will be built on, so the step is a foundation rather than a mock-up.
//
// Inspired by Namco's Pac-Man (1980). Inspiration only: the pixel art is drawn fresh for this
// effect, at a size and palette of its own.
//
// The construction follows FlyingToastersEffect and FishTankEffect: movement is a particles::Pool
// entry per character (constant velocity, wrap at the edges), appearance is the stateless
// draw::sprite power function. Pacman's chomp and the ghosts' foot-shuffle are one shared
// BeatPhase, offset per character so the cast does not pulse in unison.
// Author: projectMM original

#include "core/math16.h"      // BeatPhase: the chomp / shuffle clock
#include "core/math8.h"       // Random8: fixed-seed spawn variation, golden-reproducible
#include "light/effects/EffectBase.h"
#include "light/particles.h"  // Pool: the movable-things kernel the cast rides

namespace mm {

namespace pacart {

// Palette LAYOUT: the sprites index these and the effect fills them per character, so a ghost is
// one drawing in four colors rather than four drawings. Slot 0 is draw::sprite's transparent key.
enum : uint8_t { kClear = 0, kBody = 1, kEye = 2, kPupil = 3, kDark = 4 };
inline constexpr uint8_t kPaletteCount = 5;

// Pacman: 11x11, 4 frames of chomp (open, half, closed, half). Drawn facing RIGHT; draw::sprite's
// flipX serves leftward travel, so one drawing walks both ways.
inline constexpr uint8_t W = 11, H = 11, F = 4;
inline constexpr uint8_t kPac[] = {
    // frame 0: mouth wide
    0,0,0,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,0,0,0,
    1,1,1,1,1,1,1,0,0,0,0,
    1,1,1,1,1,1,0,0,0,0,0,
    1,1,1,1,1,0,0,0,0,0,0,
    1,1,1,1,1,1,0,0,0,0,0,
    1,1,1,1,1,1,1,0,0,0,0,
    0,1,1,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,0,0,1,1,1,1,1,0,0,0,
    // frame 1: mouth half
    0,0,0,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,1,1,0,
    1,1,1,1,1,1,1,1,1,0,0,
    1,1,1,1,1,1,1,1,0,0,0,
    1,1,1,1,1,1,1,0,0,0,0,
    1,1,1,1,1,1,1,1,0,0,0,
    1,1,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,1,1,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,0,0,1,1,1,1,1,0,0,0,
    // frame 2: mouth closed (a full disc)
    0,0,0,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,1,1,0,
    1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    0,1,1,1,1,1,1,1,1,1,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,0,0,1,1,1,1,1,0,0,0,
    // frame 3: mouth half again (the return stroke)
    0,0,0,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,1,1,0,
    1,1,1,1,1,1,1,1,1,0,0,
    1,1,1,1,1,1,1,1,0,0,0,
    1,1,1,1,1,1,1,0,0,0,0,
    1,1,1,1,1,1,1,1,0,0,0,
    1,1,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,1,1,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,0,0,1,1,1,1,1,0,0,0,
};
static_assert(sizeof(kPac) == static_cast<size_t>(W) * H * F, "pacman: 4 frames of 11x11");

// A ghost: 11x11, 2 frames whose skirt alternates, which is the arcade original's whole walk
// animation. The eyes look RIGHT; flipX mirrors them with the body.
inline constexpr uint8_t GW = 11, GH = 11, GF = 2;
inline constexpr uint8_t kGhost[] = {
    // frame 0: skirt down-up-down
    0,0,0,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,1,1,0,
    0,1,2,2,1,1,2,2,1,1,0,
    1,1,2,2,2,1,2,2,2,1,1,
    1,1,2,3,3,1,2,3,3,1,1,
    1,1,2,3,3,1,2,3,3,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    1,0,1,1,0,1,1,0,1,1,0,
    // frame 1: skirt up-down-up
    0,0,0,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,1,1,0,
    0,1,2,2,1,1,2,2,1,1,0,
    1,1,2,2,2,1,2,2,2,1,1,
    1,1,2,3,3,1,2,3,3,1,1,
    1,1,2,3,3,1,2,3,3,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    0,1,1,0,1,1,0,1,1,0,1,
};
static_assert(sizeof(kGhost) == static_cast<size_t>(GW) * GH * GF, "ghost: 2 frames of 11x11");

}  // namespace pacart

/// Effect: Pacman and the ghosts cross the wall, chomping.
/// @card PacmanEffect.gif
class PacmanEffect : public EffectBase {
public:
    static constexpr uint8_t kPool = 12;

    const char* tags() const override { return "💫🎶✨👾"; }  // audio-reactive when audioReactive is set
    Dim dimensions() const override { return Dim::D2; }

    /// How many of each, and how fast they travel.
    uint8_t pacmen = 1;
    uint8_t ghosts = 4;       // the arcade cast: Blinky, Pinky, Inky, Clyde
    uint8_t speed  = 96;
    uint8_t spriteSize = 0;   // 0 = auto: scale with the grid
    bool audioReactive = false;  // move to the music: each sprite on its own band, still in silence

    void defineControls() override {
        controls_.addControl("pacmen", pacmen, 0, 4);
        controls_.addControl("ghosts", ghosts, 0, 8);
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("spriteSize", spriteSize, 0, 12);
        controls_.addControl("audioReactive", audioReactive);
    }

    void prepare() override {
        const bool ok = x_.resize(kPool) && y_.resize(kPool) && vx_.resize(kPool) &&
                        vy_.resize(kPool) && ttl_.resize(kPool) && kind_.resize(kPool);
        if (!ok) { pool_ = particles::Pool{}; return; }
        pool_ = particles::Pool{};
        pool_.x = &x_[0]; pool_.y = &y_[0];
        pool_.vx = &vx_[0]; pool_.vy = &vy_[0];
        pool_.ttl = &ttl_[0];
        pool_.hue = &kind_[0];   // 0 = pacman, 1..N = ghost color index
        pool_.count = kPool;
        pool_.clear();
        rng_.seed(kSeed);
        for (uint16_t i = 0; i < wanted(); i++) launch(i, /*anywhere=*/true);
        time_.reset();
        chomp_ = BeatPhase{};
    }

    /// Sprite magnification: the control, or grid-proportional when 0, so the cast reads on a
    /// 16x16 matrix and on a 768-wide desktop grid alike. Same rule as the other sprite effects.
    uint8_t spriteScale() const {
        if (spriteSize > 0) return spriteSize;
        const lengthType m = width() < height() ? width() : height();
        const lengthType autoScale = m / 40;
        return static_cast<uint8_t>(autoScale < 1 ? 1 : (autoScale > 12 ? 12 : autoScale));
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width();
        if (!pool_.valid()) return;
        const uint8_t sc = spriteScale();

        draw::fill(cv, RGB{0, 0, 0});

        const uint32_t scale = time_.advance(elapsed());
        if (scale > 0) pool_.stepDriven(scale, audioReactive, wanted());

        // One clock for the chomp AND the ghosts' shuffle: in the arcade they run at the same
        // rate, and a single phase keeps them in step without a second accumulator.
        chomp_.advanceTo(elapsed(), 420);

        syncPopulation();

        for (uint16_t i = 0; i < pool_.count; i++) {
            if (!pool_.ttl[i]) continue;
            const lengthType px = draw::toPixel(pool_.x[i]);
            const lengthType py = draw::toPixel(pool_.y[i]);
            const uint8_t role = pool_.hue[i];

            // Off an edge: re-enter from the far side. The arcade maze wraps through its tunnel,
            // and a cast that vanished for good would empty the wall within a minute.
            if (px < -static_cast<lengthType>(pacart::W) * sc || px > w + pacart::W * sc) {
                launch(i, /*anywhere=*/false);
                continue;
            }

            RGB pal[pacart::kPaletteCount];
            const bool flip = pool_.vx[i] < 0;

            if (role == 0) {
                pacmanPalette(pal);
                const draw::sprites::Sprite s{pacart::kPac, pal, pacart::W, pacart::H,
                                              pacart::F, pacart::kPaletteCount};
                draw::sprite(cv, s, static_cast<uint8_t>(chomp_.phase(4) & 0x03), px, py, sc, flip);
            } else {
                ghostPalette(static_cast<uint8_t>(role - 1), pal);
                const draw::sprites::Sprite s{pacart::kGhost, pal, pacart::GW, pacart::GH,
                                              pacart::GF, pacart::kPaletteCount};
                const uint8_t f = static_cast<uint8_t>((chomp_.phase(2) + i) & 0x01);
                draw::sprite(cv, s, f, px, py, sc, flip);
            }
        }
    }

private:
    static constexpr uint32_t kSeed = 0x9AC3A17Du;

    /// Pacman is yellow, the one color in this cast that is not negotiable: a differently colored
    /// Pacman is not Pacman. The palette drives the ghosts instead (see ghostPalette).
    void pacmanPalette(RGB (&pal)[pacart::kPaletteCount]) const {
        pal[pacart::kClear] = RGB{0, 0, 0};
        pal[pacart::kBody]  = RGB{255, 214, 0};
        pal[pacart::kEye]   = RGB{0, 0, 0};
        pal[pacart::kPupil] = RGB{0, 0, 0};
        pal[pacart::kDark]  = RGB{140, 118, 0};
    }

    /// A ghost takes its body color from the active palette, spread so four ghosts land far apart
    /// on it rather than in one narrow arc. The eyes stay white with dark pupils, which is what
    /// makes a colored blob read as a ghost at all.
    void ghostPalette(uint8_t which, RGB (&pal)[pacart::kPaletteCount]) const {
        pal[pacart::kClear] = RGB{0, 0, 0};
        pal[pacart::kBody]  = colorFromPalette(*Palettes::active(),
                                               static_cast<uint8_t>(which * 64 + 16));
        pal[pacart::kEye]   = RGB{255, 255, 255};
        pal[pacart::kPupil] = RGB{30, 30, 160};
        pal[pacart::kDark]  = blend(pal[pacart::kBody], RGB{0, 0, 0}, 150);
    }

    uint16_t wanted() const {
        const uint16_t n = static_cast<uint16_t>(pacmen) + ghosts;
        return n > kPool ? kPool : n;
    }

    /// Top up or retire slots when a count control changes, live, without a re-prepare.
    void syncPopulation() {
        const uint16_t want = wanted();
        uint16_t alive = 0;
        for (uint16_t i = 0; i < pool_.count; i++) if (pool_.ttl[i]) alive++;
        for (uint16_t i = 0; i < pool_.count && alive < want; i++)
            if (!pool_.ttl[i]) { launch(i, /*anywhere=*/true); alive++; }
        for (uint16_t i = pool_.count; i-- > 0 && alive > want;)
            if (pool_.ttl[i]) { pool_.ttl[i] = 0; alive--; }

        // Trading ghosts for Pacmen leaves the TOTAL unchanged, so nothing above respawns and the
        // slots keep the roles they launched with: the controls would say three Pacmen while the
        // wall still showed one. Re-role the live slots that sit on the wrong side of the moved
        // boundary, and only those, so the rest keep their positions and momentum.
        for (uint16_t i = 0; i < pool_.count; i++)
            if (pool_.ttl[i] && pool_.hue[i] != roleFor(i)) launch(i, /*anywhere=*/true);
    }

    /// The role slot `i` should hold: slots are positional, so the first `pacmen` are Pacman and
    /// the rest cycle through the four ghost colors. One home for the rule launch() applies.
    uint8_t roleFor(uint16_t i) const {
        return (i < pacmen) ? 0 : static_cast<uint8_t>(1 + ((i - pacmen) & 0x03));
    }

    /// Put character `i` on the wall. Slots are positional: the first `pacmen` are Pacman, the
    /// rest ghosts, so changing a count moves one boundary and leaves the others as they were.
    void launch(uint16_t i, bool anywhere) {
        const lengthType w = width(), h = height();
        const uint8_t sc = spriteScale();

        const uint8_t role = roleFor(i);
        pool_.hue[i] = role;

        // Speed scales with the sprite so travel READS the same on any grid, and Pacman is a
        // touch quicker than the ghosts, as in the arcade.
        const int32_t base = static_cast<int32_t>(speed) * sc * (role == 0 ? 5 : 4) / 4;
        const bool leftward = (rng_.next8() & 1) != 0;
        pool_.vx[i] = static_cast<draw::pos_t>(leftward ? -base : base);
        pool_.vy[i] = 0;   // ITERATION 1: straight lines. The maze in iteration 2 turns them.

        const uint16_t slots = wanted() ? wanted() : 1;
        // Rows, not a scatter: the cast reads as characters travelling lanes, and two sprites on
        // the same pixels read as one shape. particles::spreadLane keeps the lanes distinct at
        // any count (a naive `(i * 5) % slots` collapses to row 0 when slots is 5).
        const lengthType row = particles::spreadLane(i, slots,
                                                     static_cast<lengthType>(h - pacart::H * sc));
        pool_.x[i] = draw::toSub(anywhere
            ? particles::spreadLane(static_cast<uint16_t>(i * 2), slots, w)
            : (leftward ? static_cast<lengthType>(w + pacart::W * sc)
                        : static_cast<lengthType>(-pacart::W * sc)));
        pool_.y[i] = draw::toSub(row);
        pool_.ttl[i] = 0xFFFF;   // they leave by walking off, not by expiring
    }

    particles::Pool pool_;
    ScratchBuffer<draw::pos_t> x_{*this}, y_{*this}, vx_{*this}, vy_{*this};
    ScratchBuffer<uint16_t> ttl_{*this};
    ScratchBuffer<uint8_t> kind_{*this};
    particles::FrameTime time_;
    BeatPhase chomp_;
    Random8 rng_;
};

}  // namespace mm
