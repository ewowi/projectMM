#pragma once
// Flying toasters: the classic screensaver, reborn as a light-wall effect. Chrome toasters with
// flapping wings and slices of toast drift diagonally across the dark, forever.
//
// Inspired by After Dark's Flying Toasters (Berkeley Systems, 1989), brought to projectMM at
// the suggestion of Frank (softhack007), who proposed sprite support for classic screensavers.
// Inspiration only: the sprite art below is drawn fresh for this effect (the original art is
// Berkeley Systems', and famously litigated), and nothing is copied from any derived data.
//
// The construction is the sprites-spec's division of labor: movement is the existing
// particles::Pool (constant diagonal velocity, zero forces, respawn-wrap), appearance is the
// stateless draw::sprite power function. The wing flap is a BeatPhase with a per-toaster phase
// offset so the flock never syncs.
// Author: projectMM original

#include "core/math16.h"      // BeatPhase: the shared wing-flap clock
#include "core/math8.h"       // Random8: fixed-seed spawn variation, golden-reproducible
#include "light/effects/EffectBase.h"
#include "light/particles.h"  // Pool: the movable-things kernel the toasters ride

namespace mm {

namespace toasterart {

// Palette: 0 transparent, then chrome greys, slot dark, wing grey, toast browns.
inline constexpr RGB kPalette[] = {
    {0, 0, 0},        // 0: transparent key (never read)
    {188, 192, 200},  // 1: chrome body
    {126, 130, 142},  // 2: chrome shade
    {235, 238, 245},  // 3: chrome highlight
    {40, 42, 50},     // 4: slot / outline dark
    {214, 216, 224},  // 5: wing
    {196, 140, 72},   // 6: toast crust
    {230, 186, 118},  // 7: toast face
};
inline constexpr uint8_t kPaletteCount = sizeof(kPalette) / sizeof(kPalette[0]);

// Toaster, 12x9, 4 frames (wing up / mid / down / mid). Rows top to bottom; the wing sits on
// the upper-left, the slot on top of the chrome loaf, feet below.
inline constexpr uint8_t W = 12, H = 9, F = 4;
inline constexpr uint8_t kToaster[] = {
    // frame 0: wing up
    0,5,5,0,0,4,4,4,4,4,0,0,
    0,5,5,0,4,1,1,1,1,1,4,0,
    0,5,5,4,1,3,1,1,1,1,1,4,
    0,0,5,4,1,1,1,1,1,1,1,4,
    0,0,0,4,1,1,1,1,1,1,2,4,
    0,0,0,4,1,1,1,1,1,2,2,4,
    0,0,0,4,2,1,1,1,2,2,2,4,
    0,0,0,0,4,4,4,4,4,4,4,0,
    0,0,0,0,0,4,0,0,0,4,0,0,
    // frame 1: wing mid
    0,0,0,0,0,4,4,4,4,4,0,0,
    0,0,0,0,4,1,1,1,1,1,4,0,
    5,5,5,4,1,3,1,1,1,1,1,4,
    0,5,5,4,1,1,1,1,1,1,1,4,
    0,0,0,4,1,1,1,1,1,1,2,4,
    0,0,0,4,1,1,1,1,1,2,2,4,
    0,0,0,4,2,1,1,1,2,2,2,4,
    0,0,0,0,4,4,4,4,4,4,4,0,
    0,0,0,0,0,4,0,0,0,4,0,0,
    // frame 2: wing down
    0,0,0,0,0,4,4,4,4,4,0,0,
    0,0,0,0,4,1,1,1,1,1,4,0,
    0,0,0,4,1,3,1,1,1,1,1,4,
    0,5,5,4,1,1,1,1,1,1,1,4,
    0,5,5,4,1,1,1,1,1,1,2,4,
    5,5,0,4,1,1,1,1,1,2,2,4,
    0,0,0,4,2,1,1,1,2,2,2,4,
    0,0,0,0,4,4,4,4,4,4,4,0,
    0,0,0,0,0,4,0,0,0,4,0,0,
    // frame 3: wing mid (return stroke)
    0,0,0,0,0,4,4,4,4,4,0,0,
    0,0,0,0,4,1,1,1,1,1,4,0,
    5,5,5,4,1,3,1,1,1,1,1,4,
    0,5,5,4,1,1,1,1,1,1,1,4,
    0,0,0,4,1,1,1,1,1,1,2,4,
    0,0,0,4,1,1,1,1,1,2,2,4,
    0,0,0,4,2,1,1,1,2,2,2,4,
    0,0,0,0,4,4,4,4,4,4,4,0,
    0,0,0,0,0,4,0,0,0,4,0,0,
};
static_assert(sizeof(kToaster) == static_cast<size_t>(W) * H * F, "toaster: 4 frames of 12x9");

inline constexpr uint8_t TW = 6, TH = 6;
inline constexpr uint8_t kToast[] = {
    0,6,6,6,6,0,
    6,7,7,7,7,6,
    6,7,7,7,7,6,
    6,7,7,7,7,6,
    6,7,7,7,7,6,
    0,6,6,6,6,0,
};
static_assert(sizeof(kToast) == static_cast<size_t>(TW) * TH, "toast: one 6x6 frame");

inline constexpr draw::sprites::Sprite kToasterSprite{kToaster, kPalette, W, H, F, kPaletteCount};
inline constexpr draw::sprites::Sprite kToastSprite{kToast, kPalette, TW, TH, 1, kPaletteCount};

}  // namespace toasterart

/// Effect: chrome toasters with flapping wings and toast drift diagonally across the dark.
/// @card FlyingToastersEffect.gif
class FlyingToastersEffect : public EffectBase {
public:
    static constexpr uint8_t kPool = 20;   // 12 toasters + 8 toast, the control maxima

    const char* tags() const override { return "💫🎶✨👾"; }  // audio-reactive when audioReactive is set
    Dim dimensions() const override { return Dim::D2; }

    /// How many of each fly, and how fast the flock drifts.
    uint8_t toasters = 5;
    uint8_t toast    = 3;
    uint8_t speed    = 96;
    uint8_t spriteSize = 0;   // 0 = auto: scale with the grid; both toasters and toast use it
    bool audioReactive = false;  // move to the music: each sprite on its own band, still in silence

    void defineControls() override {
        controls_.addControl("toasters", toasters, 1, 12);
        controls_.addControl("toast", toast, 0, 8);
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("spriteSize", spriteSize, 0, 12);
        controls_.addControl("audioReactive", audioReactive);
    }

    void prepare() override {
        // Alloc-all-then-check (the BallpitEffect rule), then wire the SoA pool.
        const bool ok = x_.resize(kPool) && y_.resize(kPool) && vx_.resize(kPool) &&
                        vy_.resize(kPool) && ttl_.resize(kPool) && kind_.resize(kPool);
        if (!ok) { pool_ = particles::Pool{}; return; }
        pool_ = particles::Pool{};
        pool_.x = &x_[0]; pool_.y = &y_[0];
        pool_.vx = &vx_[0]; pool_.vy = &vy_[0];
        pool_.ttl = &ttl_[0];
        pool_.hue = &kind_[0];   // reused as the sprite-kind flag: 0 toaster, 1 toast
        pool_.count = kPool;
        pool_.clear();
        rng_.seed(kSeed);
        for (uint16_t i = 0; i < wanted(); i++) launch(i, /*anywhere=*/true);
        time_.reset();
        flap_ = BeatPhase{};
    }

    /// The sprite magnification: the `size` control, or grid-proportional when 0 (auto), so a
    /// toaster reads as a toaster on a 1024-wide wall and stays 1:1 on a small matrix.
    uint8_t spriteScale() const {
        if (spriteSize > 0) return spriteSize;
        const lengthType m = width() < height() ? width() : height();
        const lengthType autoScale = m / 40;
        return static_cast<uint8_t>(autoScale < 1 ? 1 : (autoScale > 12 ? 12 : autoScale));
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType h = height();
        // No grid-size guard (the no-grid-guards rule): draw::sprite clips per pixel, so a
        // grid smaller than a toaster shows a clipped toaster rather than going dark.
        if (!pool_.valid()) return;
        const uint8_t sc = spriteScale();

        draw::fill(cv, RGB{0, 0, 0});

        const uint32_t scale = time_.advance(elapsed());
        if (scale > 0) pool_.stepDriven(scale, audioReactive, wanted());

        // The wing flap: one shared BeatPhase, offset per toaster so the flock never syncs.
        flap_.advanceTo(elapsed(), 180);   // ~3 flaps per second across the 4-frame cycle

        for (uint16_t i = 0; i < pool_.count; i++) {
            if (!pool_.ttl[i]) continue;
            const lengthType px = draw::toPixel(pool_.x[i]);
            const lengthType py = draw::toPixel(pool_.y[i]);
            // Left the lower-left band: respawn into the upper-right spawn band.
            if (px < -static_cast<lengthType>(toasterart::W) * sc || py > h) {
                launch(i, /*anywhere=*/false);
                continue;
            }
            if (pool_.hue[i] == 0) {
                const uint8_t frame =
                    static_cast<uint8_t>((flap_.phase(4) + (i * 7) % 4) & 0x03);
                draw::sprite(cv, toasterart::kToasterSprite, frame, px, py, sc);
            } else {
                draw::sprite(cv, toasterart::kToastSprite, 0, px, py, sc);
            }
        }
        // The count controls apply live without a full re-prepare storm: top up or trim.
        syncPopulation();
    }

private:
    static constexpr uint32_t kSeed = 0x70A57E25u;   // "TOASTES", fixed: goldens reproduce

    uint16_t wanted() const { return static_cast<uint16_t>(toasters + toast); }

    /// (Re)spawn slot i: toasters first, then toast. `anywhere` scatters across the whole
    /// screen (first fill); otherwise the upper-right off-screen band, the classic entry.
    void launch(uint16_t i, bool anywhere) {
        const bool isToast = i >= toasters;
        const lengthType w = width(), h = height();
        const uint8_t sc = spriteScale();
        // Diagonal toward lower-left, magnitude from `speed` with +-25% per-entry variation.
        // Scaled with the sprite so the flight READS the same on any grid (same sprite-widths
        // per second, more pixels per second on a big wall).
        const int32_t base = static_cast<int32_t>(speed) * 2 * sc;
        const int32_t vary = base / 4;
        // next16, not below(uint8_t): the span is base/2, which passes 255 at any real sprite
        // scale, and an 8-bit draw would silently clamp it: every large toaster flying at
        // almost exactly the same speed instead of the documented +-25%.
        const uint32_t span = static_cast<uint32_t>(vary) * 2;
        const int32_t v = base - vary + (span > 0 ? static_cast<int32_t>(rng_.next16() % span) : 0);
        draw::pos_t px, py;
        if (anywhere) {
            px = draw::toSub(static_cast<lengthType>(rng_.next16() % (w > 0 ? w : 1)));
            py = draw::toSub(static_cast<lengthType>(rng_.next16() % (h > 0 ? h : 1)));
        } else if (rng_.next8() & 1) {
            // Off the top edge, anywhere along x.
            px = draw::toSub(static_cast<lengthType>(rng_.next16() % (w + toasterart::W * sc)));
            py = draw::toSub(static_cast<lengthType>(-toasterart::H * sc));
        } else {
            // Off the right edge, upper half of y.
            px = draw::toSub(static_cast<lengthType>(w));
            py = draw::toSub(static_cast<lengthType>(rng_.next16() % (h > 1 ? h / 2 + 1 : 1)));
        }
        pool_.ttl[i] = 0;
        pool_.spawn(px, py, -v, v / 2, 65535, isToast ? 1 : 0);
        // spawn() picked the first free slot, which is i because we just freed it - except when
        // the pool briefly holds a different shape during a count change; syncPopulation heals.
    }

    /// Top up newly wanted slots and trim no-longer-wanted ones, so the count controls apply
    /// live (a full respawn on every slider step would blink the whole flock).
    void syncPopulation() {
        for (uint16_t i = 0; i < pool_.count; i++) {
            const bool want = i < wanted();
            if (want && !pool_.ttl[i]) launch(i, false);
            else if (!want && pool_.ttl[i]) pool_.ttl[i] = 0;
            if (pool_.ttl[i]) pool_.hue[i] = i >= toasters ? 1 : 0;
        }
    }

    ScratchBuffer<draw::pos_t> x_{*this}, y_{*this}, vx_{*this}, vy_{*this};
    ScratchBuffer<uint16_t> ttl_{*this};
    ScratchBuffer<uint8_t> kind_{*this};
    particles::Pool pool_;
    particles::FrameTime time_{60};
    BeatPhase flap_;
    Random8 rng_{kSeed};
};

}  // namespace mm
