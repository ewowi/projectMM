#pragma once
// Fish tank: an aquarium screensaver. Fish of several species swim across a dark tank, each
// tinted from the active palette, the smaller ones drifting slower as if further back.
//
// Inspired by the aquarium screensavers of the After Dark era (and the SereneScreen Marine
// Aquarium that followed). Inspiration only: the art below is drawn fresh for this effect.
//
// The construction is the sprites-spec's division of labor, the same one FlyingToastersEffect
// uses: movement is the existing particles::Pool (constant velocity, zero forces, respawn-wrap),
// appearance is the stateless draw::sprite power function. What differs is COLOR. A Sprite holds
// a pointer to its palette rather than owning one, so each fish is drawn through a palette this
// effect fills per fish from the user's active palette: one sprite shape, as many colorways as
// there are fish. That is why the art below is a shape with SHADE indices (body, dark, light)
// rather than fixed colors.
// Author: projectMM original

#include "core/math16.h"      // BeatPhase: the shared tail-beat clock
#include "core/math8.h"       // Random8: fixed-seed spawn variation, golden-reproducible
#include "light/effects/EffectBase.h"
#include "light/particles.h"  // Pool: the movable-things kernel the fish ride

namespace mm {

namespace fishart {

// Palette LAYOUT, not colors: every fish sprite indexes these slots and the effect fills them
// per fish (see FishTankEffect::paletteFor). Slot 0 is the transparent key draw::sprite skips.
enum : uint8_t { kClear = 0, kBody = 1, kDark = 2, kLight = 3, kFin = 4, kEye = 5, kBand = 6 };
inline constexpr uint8_t kPaletteCount = 7;

// A broad reef fish (the angelfish/clownfish build): tall, blunt-nosed, a deep body with a
// vertical band. 16x11, 3 frames of tail beat. Drawn facing RIGHT; draw::sprite's flipX serves
// the other direction, so one drawing swims both ways.
inline constexpr uint8_t W = 16, H = 11, F = 3;
inline constexpr uint8_t kFish[] = {
    // frame 0: tail spread
    0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,
    0,0,0,0,2,2,1,1,6,1,2,2,0,0,0,0,
    0,0,4,4,2,1,1,6,6,1,1,1,2,0,0,0,
    0,4,4,4,2,1,1,6,6,1,1,1,1,2,0,0,
    0,0,4,2,1,1,6,6,1,1,1,1,1,1,2,0,
    2,2,2,1,1,1,6,6,1,1,1,3,5,1,1,2,
    0,0,4,2,1,1,6,6,1,1,1,1,1,1,2,0,
    0,4,4,4,2,1,1,6,6,1,1,1,1,2,0,0,
    0,0,4,4,2,1,1,6,6,1,1,1,2,0,0,0,
    0,0,0,0,2,2,1,1,6,1,2,2,0,0,0,0,
    0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,
    // frame 1: tail up
    0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,
    0,0,4,0,2,2,1,1,6,1,2,2,0,0,0,0,
    0,4,4,4,2,1,1,6,6,1,1,1,2,0,0,0,
    0,0,4,4,2,1,1,6,6,1,1,1,1,2,0,0,
    0,0,4,2,1,1,6,6,1,1,1,1,1,1,2,0,
    2,2,2,1,1,1,6,6,1,1,1,3,5,1,1,2,
    0,0,2,2,1,1,6,6,1,1,1,1,1,1,2,0,
    0,0,0,2,2,1,1,6,6,1,1,1,1,2,0,0,
    0,0,0,0,2,1,1,6,6,1,1,1,2,0,0,0,
    0,0,0,0,2,2,1,1,6,1,2,2,0,0,0,0,
    0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,
    // frame 2: tail down
    0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,
    0,0,0,0,2,2,1,1,6,1,2,2,0,0,0,0,
    0,0,0,2,2,1,1,6,6,1,1,1,2,0,0,0,
    0,0,2,2,1,1,1,6,6,1,1,1,1,2,0,0,
    0,0,4,2,1,1,6,6,1,1,1,1,1,1,2,0,
    2,2,2,1,1,1,6,6,1,1,1,3,5,1,1,2,
    0,0,4,2,1,1,6,6,1,1,1,1,1,1,2,0,
    0,0,4,4,2,1,1,6,6,1,1,1,1,2,0,0,
    0,4,4,4,2,1,1,6,6,1,1,1,2,0,0,0,
    0,0,4,0,2,2,1,1,6,1,2,2,0,0,0,0,
    0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,
};
static_assert(sizeof(kFish) == static_cast<size_t>(W) * H * F, "fish: 3 frames of 16x11");

// A slender fish (the tetra/danio build): long, low, a forked tail. A different SILHOUETTE, not
// a recolor: a tank of one outline reads as a repeat however the colors vary. 13x7, 3 frames.
inline constexpr uint8_t SW = 13, SH = 7, SF = 3;
inline constexpr uint8_t kSlim[] = {
    0,0,0,0,0,2,2,2,2,2,0,0,0,
    0,4,2,2,2,1,1,1,1,1,2,2,0,
    4,4,2,1,1,1,1,1,1,1,1,1,2,
    4,2,1,1,1,1,1,1,1,3,5,1,2,
    4,4,2,1,1,1,1,1,1,1,1,1,2,
    0,4,2,2,2,1,1,1,1,1,2,2,0,
    0,0,0,0,0,2,2,2,2,2,0,0,0,
    // tail up
    0,0,4,0,0,2,2,2,2,2,0,0,0,
    0,4,4,2,2,1,1,1,1,1,2,2,0,
    0,0,2,1,1,1,1,1,1,1,1,1,2,
    0,2,1,1,1,1,1,1,1,3,5,1,2,
    0,0,2,1,1,1,1,1,1,1,1,1,2,
    0,0,2,2,2,1,1,1,1,1,2,2,0,
    0,0,0,0,0,2,2,2,2,2,0,0,0,
    // tail down
    0,0,0,0,0,2,2,2,2,2,0,0,0,
    0,0,2,2,2,1,1,1,1,1,2,2,0,
    0,0,2,1,1,1,1,1,1,1,1,1,2,
    0,2,1,1,1,1,1,1,1,3,5,1,2,
    0,0,2,1,1,1,1,1,1,1,1,1,2,
    0,4,4,2,2,1,1,1,1,1,2,2,0,
    0,0,4,0,0,2,2,2,2,2,0,0,0,
};
static_assert(sizeof(kSlim) == static_cast<size_t>(SW) * SH * SF, "slim: 3 frames of 13x7");

// A tiny schooling fish: 6x4, one frame. Too small for a tail beat to read, and a school is
// several of these moving together, the shape the reference image's cluster has.
inline constexpr uint8_t TW = 6, TH = 4;
inline constexpr uint8_t kTiny[] = {
    0,0,2,2,2,0,
    4,2,1,1,1,2,
    4,2,1,5,1,2,
    0,0,2,2,2,0,
};
static_assert(sizeof(kTiny) == static_cast<size_t>(TW) * TH, "tiny: one 6x4 frame");

}  // namespace fishart

/// Effect: colorful fish swim across a dark tank, each tinted from the active palette.
/// @card FishTankEffect.gif
class FishTankEffect : public EffectBase {
public:
    static constexpr uint8_t kPool = 24;   // the control maxima, summed

    const char* tags() const override { return "💫🎶✨👾"; }  // audio-reactive when audioReactive is set
    Dim dimensions() const override { return Dim::D2; }

    /// How many of each swim, and how fast.
    uint8_t fish  = 3;     // the broad tropical shape
    uint8_t slim  = 3;     // the slender shape
    uint8_t tiny  = 5;     // the school
    uint8_t speed = 80;
    uint8_t spriteSize = 0;   // 0 = auto: scale with the grid, as FlyingToasters does
    bool audioReactive = false;  // move to the music: each sprite on its own band, still in silence

    void defineControls() override {
        controls_.addControl("fish", fish, 0, 8);
        controls_.addControl("slim", slim, 0, 8);
        controls_.addControl("school", tiny, 0, 8);
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("spriteSize", spriteSize, 0, 12);
        controls_.addControl("audioReactive", audioReactive);
    }

    void prepare() override {
        const bool ok = x_.resize(kPool) && y_.resize(kPool) && vx_.resize(kPool) &&
                        vy_.resize(kPool) && ttl_.resize(kPool) && kind_.resize(kPool) &&
                        entry_.resize(kPool);
        if (!ok) { pool_ = particles::Pool{}; return; }
        pool_ = particles::Pool{};
        pool_.x = &x_[0]; pool_.y = &y_[0];
        pool_.vx = &vx_[0]; pool_.vy = &vy_[0];
        pool_.ttl = &ttl_[0];
        pool_.hue = &kind_[0];   // the SPECIES; the palette entry has its own array (entry_)
        pool_.count = kPool;
        pool_.clear();
        rng_.seed(kSeed);
        for (uint16_t i = 0; i < wanted(); i++) launch(i, /*anywhere=*/true);
        time_.reset();
        beat_ = BeatPhase{};
    }

    /// The sprite magnification: the `spriteSize` control, or grid-proportional when 0, so a fish
    /// reads as a fish on a 768-wide desktop grid AND on a 16x16 matrix (where x1 already fills
    /// most of the width). Same rule as FlyingToasters, so the two agree on any wall.
    uint8_t spriteScale() const {
        if (spriteSize > 0) return spriteSize;
        const lengthType m = width() < height() ? width() : height();
        const lengthType autoScale = m / 40;
        return static_cast<uint8_t>(autoScale < 1 ? 1 : (autoScale > 12 ? 12 : autoScale));
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width();
        // No grid-size guard (the no-grid-guards rule): draw::sprite clips per pixel.
        if (!pool_.valid()) return;
        const uint8_t sc = spriteScale();

        draw::fill(cv, RGB{0, 0, 0});

        const uint32_t scale = time_.advance(elapsed());
        if (scale > 0) pool_.stepDriven(scale, audioReactive, wanted());

        // One shared tail-beat clock, offset per fish so the tank never pulses in unison.
        beat_.advanceTo(elapsed(), 200);

        syncPopulation();

        for (uint16_t i = 0; i < pool_.count; i++) {
            if (!pool_.ttl[i]) continue;
            const lengthType px = draw::toPixel(pool_.x[i]);
            const lengthType py = draw::toPixel(pool_.y[i]);
            const uint8_t species = pool_.hue[i];
            const uint8_t entry   = entry_[i];      // the FULL byte: 256 places on the palette

            // Swum off the left edge: respawn at the right, a new fish in the same slot.
            if (px < -static_cast<lengthType>(fishart::W) * sc) { launch(i, /*anywhere=*/false); continue; }
            if (px > w + fishart::W * sc) { launch(i, /*anywhere=*/false); continue; }

            RGB pal[fishart::kPaletteCount];
            paletteFor(entry, pal);
            const uint8_t frame = static_cast<uint8_t>((beat_.phase(3) + (i * 5) % 3) % 3);
            // The art faces RIGHT, so a fish swimming left is drawn mirrored. A tank where every
            // fish faces the same way regardless of travel reads as wallpaper, not as swimming.
            const bool flip = pool_.vx[i] < 0;

            if (species == kTiny) {
                const draw::sprites::Sprite s{fishart::kTiny, pal, fishart::TW, fishart::TH, 1,
                                              fishart::kPaletteCount};
                draw::sprite(cv, s, 0, px, py, sc, flip);
            } else if (species == kSlim) {
                const draw::sprites::Sprite s{fishart::kSlim, pal, fishart::SW, fishart::SH,
                                              fishart::SF, fishart::kPaletteCount};
                draw::sprite(cv, s, frame, px, py, sc, flip);
            } else {
                const draw::sprites::Sprite s{fishart::kFish, pal, fishart::W, fishart::H,
                                              fishart::F, fishart::kPaletteCount};
                draw::sprite(cv, s, frame, px, py, sc, flip);
            }
        }
    }

private:
    enum : uint8_t { kBroad = 0, kSlim = 1, kTiny = 2 };
    static constexpr uint32_t kSeed = 0x0F157A9Bu;

    /// Fill a sprite palette for one fish from the ACTIVE palette. The sprite art carries shade
    /// roles (body / dark / light / fin / eye / band) rather than colors, so one shape yields as
    /// many colorways as there are palette entries: the reference aquarium's appeal is the mix,
    /// and a tank of identically colored fish is not that.
    void paletteFor(uint8_t entry, RGB (&pal)[fishart::kPaletteCount]) const {
        const RGB body = colorFromPalette(*Palettes::active(), entry);
        pal[fishart::kClear] = RGB{0, 0, 0};                 // never read
        pal[fishart::kBody]  = body;
        pal[fishart::kDark]  = blend(body, RGB{0, 0, 0}, 150);   // outline / shading
        pal[fishart::kLight] = blend(body, RGB{255, 255, 255}, 120);
        pal[fishart::kFin]   = blend(body, RGB{255, 255, 255}, 60);
        pal[fishart::kEye]   = RGB{20, 20, 24};
        // The band is the fish's marking: a much paler version of its own color, the way a
        // clownfish's white band works. A second palette PICK was tried and read as two fish
        // fused together, because an arbitrary entry clashes rather than contrasts.
        pal[fishart::kBand]  = blend(body, RGB{255, 255, 255}, 200);
    }

    uint16_t wanted() const {
        const uint16_t n = static_cast<uint16_t>(fish) + slim + tiny;
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

        // Trading one species for another leaves the TOTAL unchanged, so nothing above respawns
        // and the slots keep the species they launched with: the controls would say four slim
        // fish while the tank still swam four broad ones. Restock only the slots whose species no
        // longer matches the moved boundary; the rest keep their positions and momentum.
        for (uint16_t i = 0; i < pool_.count; i++)
            if (pool_.ttl[i] && pool_.hue[i] != speciesFor(i)) launch(i, /*anywhere=*/true);
    }

    /// The species slot `i` should hold: the first `fish` slots are broad, the next `slim`
    /// slender, the rest the school. One home for the rule launch() applies.
    uint8_t speciesFor(uint16_t i) const {
        if (i < fish) return kBroad;
        if (i < static_cast<uint16_t>(fish) + slim) return kSlim;
        return kTiny;
    }

    /// Put fish `i` into the tank: a species by slot order, a palette entry of its own, a speed
    /// that follows its size (a small fish drifts slower, which reads as depth).
    void launch(uint16_t i, bool anywhere) {
        const lengthType w = width(), h = height();
        const uint8_t sc = spriteScale();

        // Species by slot: the first `fish` slots are broad, the next `slim` slender, the rest
        // the school. Keeping it positional means a count change moves one boundary, and the
        // fish already in the tank keep their identity.
        const uint8_t species = speciesFor(i);
        pool_.hue[i] = species;
        entry_[i] = rng_.next8();        // its own place on the palette, full 8-bit spread

        const lengthType sw = species == kBroad ? fishart::W
                            : species == kSlim  ? fishart::SW : fishart::TW;

        // Right-to-left, the direction the art faces. Speed scales with the sprite so the motion
        // READS the same on any grid (same body-lengths per second), and with the species so the
        // small ones trail behind.
        const int32_t base = static_cast<int32_t>(speed) * sc *
                             (species == kBroad ? 3 : species == kSlim ? 2 : 1) / 2;
        const int32_t vary = base / 4;
        const uint32_t span = static_cast<uint32_t>(vary) * 2;
        const int32_t v = base - vary + (span > 0 ? static_cast<int32_t>(rng_.next16() % span) : 0);

        // Half swim each way. Direction is picked per fish, and the spawn edge follows it, so a
        // fish always enters from the side it is heading away from.
        const bool leftward = (rng_.next8() & 1) != 0;
        pool_.vx[i] = static_cast<draw::pos_t>(leftward ? -v : v);
        // A slight vertical drift, so the tank does not read as horizontal lanes.
        pool_.vy[i] = static_cast<draw::pos_t>(static_cast<int16_t>(rng_.next8()) - 128) / 16;

        // On the initial fill, STRIDE across the width rather than scattering: uniform random
        // x clumps, and two fish a few pixels apart read as one shape rather than two. Later
        // respawns enter from the edge the fish faces.
        const uint16_t slots = wanted() ? wanted() : 1;
        // A stride by SLOT sorts the tank by species, and since species differ in speed the fast
        // ones bunch at one edge within seconds. spreadLane interleaves them, with a step chosen
        // coprime to the count so the lanes stay distinct however many fish there are.
        const lengthType lane = particles::spreadLane(i, slots, w);
        pool_.x[i] = draw::toSub(anywhere
            ? static_cast<lengthType>(lane + static_cast<lengthType>(rng_.next8() % 16) - 8)
            : (leftward ? static_cast<lengthType>(w + sw * sc)
                        : static_cast<lengthType>(-sw * sc)));
        // Vertical lanes too, so the tank fills top to bottom instead of banding.
        const lengthType vlane = particles::spreadLane(static_cast<uint16_t>(i * 2), slots, h);
        pool_.y[i] = draw::toSub(static_cast<lengthType>(
            anywhere ? vlane : static_cast<lengthType>(rng_.next16() % (h > 0 ? h : 1))));
        pool_.ttl[i] = 0xFFFF;   // fish leave by swimming out, not by expiring
    }

    particles::Pool pool_;
    ScratchBuffer<draw::pos_t> x_{*this}, y_{*this}, vx_{*this}, vy_{*this};
    ScratchBuffer<uint16_t> ttl_{*this};
    ScratchBuffer<uint8_t> kind_{*this};    // species
    ScratchBuffer<uint8_t> entry_{*this};   // palette entry, one per fish
    particles::FrameTime time_;
    BeatPhase beat_;
    Random8 rng_;
};

}  // namespace mm
