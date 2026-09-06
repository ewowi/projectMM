#pragma once
// Space Invaders, in attract mode: the formation marches, the cannon answers, nobody plays.
//
// The arcade's own attract mode is the model, and it is the right one for an effect: a game that
// waits for a player is a black screen, while a game playing itself is the thing people recognize
// from across a room. So the cannon tracks and fires on its own, the ranks march and drop, and a
// hit takes an invader out of the formation.
//
// The march is the signature. In the arcade it speeds up as the ranks thin, because the machine
// stepped every invader once per frame and fewer invaders meant a shorter loop: an accident of
// 1978 hardware that became the game's defining tension. It is reproduced here deliberately rather
// than inherited, since our loop has no such limit.
// Author: projectMM original (Space Invaders, Taito 1978, is the inspiration)

#include "core/AudioService.h"   // latestFrame: the beat the march steps on
#include "core/math16.h"
#include "light/draw.h"
#include "light/effects/EffectBase.h"

namespace mm {

namespace invart {

// Palette LAYOUT: the sprites index these and the effect fills them, so one drawing serves every
// rank in a different color. Slot 0 is draw::sprite's transparent key.
enum : uint8_t { kClear = 0, kBody = 1, kDark = 2, kEye = 3 };
inline constexpr uint8_t kPaletteCount = 4;

// The squid (top rank): 8x8, 2 frames of the tentacle wiggle that IS the march animation. Two
// frames is not a shortcut; the arcade had exactly two, alternating on each step, and the
// stop-motion quality is what the march looks like.
inline constexpr uint8_t W = 8, H = 8, F = 2;
inline constexpr uint8_t kSquid[] = {
    // frame 0: tentacles out
    0,0,0,1,1,0,0,0,
    0,0,1,1,1,1,0,0,
    0,1,1,1,1,1,1,0,
    1,1,3,1,1,3,1,1,
    1,1,1,1,1,1,1,1,
    0,0,1,0,0,1,0,0,
    0,1,0,1,1,0,1,0,
    1,0,1,0,0,1,0,1,
    // frame 1: tentacles tucked
    0,0,0,1,1,0,0,0,
    0,0,1,1,1,1,0,0,
    0,1,1,1,1,1,1,0,
    1,1,3,1,1,3,1,1,
    1,1,1,1,1,1,1,1,
    0,1,0,1,1,0,1,0,
    1,0,0,0,0,0,0,1,
    0,1,0,0,0,0,1,0,
};
static_assert(sizeof(kSquid) == static_cast<size_t>(W) * H * F, "squid: 2 frames of 8x8");

// The crab (middle ranks): 11x8, the widest of the three, arms up and arms down.
inline constexpr uint8_t CW = 11, CH = 8, CF = 2;
inline constexpr uint8_t kCrab[] = {
    // frame 0: arms up
    0,0,1,0,0,0,0,0,1,0,0,
    0,0,0,1,0,0,0,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,3,1,1,1,3,1,1,0,
    1,1,1,1,1,1,1,1,1,1,1,
    1,0,1,1,1,1,1,1,1,0,1,
    1,0,1,0,0,0,0,0,1,0,1,
    0,0,0,1,1,0,1,1,0,0,0,
    // frame 1: arms down
    0,0,1,0,0,0,0,0,1,0,0,
    1,0,0,1,0,0,0,1,0,0,1,
    1,0,1,1,1,1,1,1,1,0,1,
    1,1,1,3,1,1,1,3,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    0,1,1,1,1,1,1,1,1,1,0,
    0,0,1,0,0,0,0,0,1,0,0,
    0,1,0,0,0,0,0,0,0,1,0,
};
static_assert(sizeof(kCrab) == static_cast<size_t>(CW) * CH * CF, "crab: 2 frames of 11x8");

// The octopus (bottom ranks): 12x8, the squat one worth the fewest points and seen the longest.
inline constexpr uint8_t OW = 12, OH = 8, OF = 2;
inline constexpr uint8_t kOcto[] = {
    // frame 0: legs apart
    0,0,0,0,1,1,1,1,0,0,0,0,
    0,1,1,1,1,1,1,1,1,1,1,0,
    1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,3,3,1,1,3,3,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,
    0,0,0,1,1,0,0,1,1,0,0,0,
    0,0,1,1,0,1,1,0,1,1,0,0,
    1,1,0,0,0,0,0,0,0,0,1,1,
    // frame 1: legs together
    0,0,0,0,1,1,1,1,0,0,0,0,
    0,1,1,1,1,1,1,1,1,1,1,0,
    1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,3,3,1,1,3,3,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,
    0,0,1,1,1,0,0,1,1,1,0,0,
    0,1,1,0,0,1,1,0,0,1,1,0,
    0,0,1,1,0,0,0,0,1,1,0,0,
};
static_assert(sizeof(kOcto) == static_cast<size_t>(OW) * OH * OF, "octopus: 2 frames of 12x8");

// The cannon: 13x8, one frame. It does not animate; it moves.
inline constexpr uint8_t GW = 13, GH = 8, GF = 1;
inline constexpr uint8_t kCannon[] = {
    0,0,0,0,0,0,1,0,0,0,0,0,0,
    0,0,0,0,0,1,1,1,0,0,0,0,0,
    0,0,0,0,0,1,1,1,0,0,0,0,0,
    0,1,1,1,1,1,1,1,1,1,1,1,0,
    1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,
};
static_assert(sizeof(kCannon) == static_cast<size_t>(GW) * GH * GF, "cannon: one 13x8 frame");

}  // namespace invart

/// Effect: Space Invaders in attract mode, marching and firing on its own.
/// @card SpaceInvadersEffect.gif
class SpaceInvadersEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🎵👾"; }   // audio-reactive when audioReactive is set
    Dim dimensions() const override { return Dim::D2; }

    /// Steps per minute at full strength. The arcade had no such control: its tempo WAS the
    /// invader count. Here that relationship is kept (see stepInterval) and this sets the top.
    uint8_t marchBpm = 60;
    /// Pixels the formation slides per step, and rows it drops when it reaches an edge.
    uint8_t stepX = 2;
    uint8_t dropY = 3;
    /// Draw at this many pixels per art pixel. A 64-wide panel wants 1; a wall can afford 2.
    uint8_t size = 1;

    /// March on the music. The arcade's own four-note loop already reads as rhythm, so stepping
    /// the formation on the beat is the natural mapping rather than an imposed one, and the cannon
    /// fires on a transient. In silence the formation HOLDS: a still invasion is the honest render
    /// of no music, and it is what makes the mode read as reactive.
    bool audioReactive = false;

    void defineControls() override {
        controls_.addControl("marchBpm", marchBpm, 10, 240);
        controls_.addControl("stepX", stepX, 1, 8);
        controls_.addControl("dropY", dropY, 1, 12);
        controls_.addControl("size", size, 1, 4);
        controls_.addControl("audioReactive", audioReactive);
    }

    void prepare() override {
        formation_ = BeatPhase{};
        reset();
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        if (width() == 0 || height() == 0) return;
        draw::fill(cv, RGB{0, 0, 0});

        const AudioFrame* audio = audioReactive ? AudioService::latestFrame() : nullptr;
        const bool live = audio && audio->levelSmoothed >= kSilence;
        // A transient is the cannon's trigger, and it is also what makes a beat-driven march step:
        // reading `level` against its own smoothed average is the same beat test the moving-head
        // effect uses, so "a beat" means one thing across the project.
        const bool beat = live && audio->level > audio->levelSmoothed + kBeatMargin;

        advance(beat, audio != nullptr, live);
        drawFormation(cv);
        drawShots(cv);
        drawCannon(cv);
    }

private:
    // The formation, as the arcade laid it out: one rank of squids, two of crabs, two of octopuses.
    static constexpr uint8_t kCols = 8;
    static constexpr uint8_t kRows = 5;
    static constexpr uint8_t kShots = 6;
    static constexpr uint16_t kSilence = 8;
    static constexpr uint16_t kBeatMargin = 8;

    /// One rank's species. Rank 0 is the squid at the top, the way the machine dealt them.
    enum : uint8_t { kSquid = 0, kCrab, kOcto };
    static uint8_t speciesFor(uint8_t row) { return row == 0 ? kSquid : (row < 3 ? kCrab : kOcto); }

    void reset() {
        alive_ = 0;
        for (uint8_t r = 0; r < kRows; r++)
            for (uint8_t c = 0; c < kCols; c++) { grid_[r][c] = 1; alive_++; }
        ox_ = 0;
        oy_ = 0;
        dir_ = 1;
        frame_ = 0;
        // The march latch belongs with the clock it tracks: prepare() zeroes formation_, and a
        // latch left at the old phase disagrees with it, so the first step after a reset fires
        // immediately instead of waiting out a beat.
        lastPhase_ = 0;
        cannonX_ = 0;
        for (uint8_t i = 0; i < kShots; i++) shotTtl_[i] = 0;
    }

    /// Step the whole formation one march tick, and run everything that happens between steps.
    void advance(bool beat, bool audioOn, bool live) {
        // Shots fly every frame; only the MARCH is quantised, which is what gives the game its
        // stop-motion look against continuously moving bullets.
        moveShots();

        if (audioOn) {
            if (!live) return;                       // silence holds the invasion still
            if (!beat) return;                       // the beat is the clock
        } else {
            formation_.advanceTo(elapsed(), stepInterval());
            const uint32_t phase = formation_.phase(2);
            if (phase == lastPhase_) return;
            lastPhase_ = phase;
        }
        stepFormation();
    }

    /// Steps per minute, rising as the ranks thin. THE defining mechanic: the arcade sped up
    /// because fewer invaders meant a shorter loop, and a version that marches at a constant rate
    /// is missing the tension that made the game.
    uint8_t stepInterval() const {
        const uint16_t total = static_cast<uint16_t>(kRows) * kCols;
        const uint16_t left = alive_ == 0 ? 1 : alive_;
        // Four times the base tempo when one invader is left, which is roughly the arcade's own
        // ratio between a full screen and the last runner.
        const uint32_t bpm = static_cast<uint32_t>(marchBpm) * (total + 3 * (total - left)) / total;
        return static_cast<uint8_t>(bpm > 240 ? 240 : bpm);
    }

    void stepFormation() {
        frame_ ^= 1;                                 // the two-frame wiggle IS the step
        const int16_t span = formationWidth();
        const int16_t nx = static_cast<int16_t>(ox_ + dir_ * static_cast<int16_t>(stepX));
        // Turn and drop at the wall, the way the formation always has. On a panel NARROWER than the
        // formation the right wall sits behind the left one, so the naive test fires at every
        // position: the ranks would never move sideways, just drop and reset on a loop. There the
        // formation scrolls THROUGH the panel instead, turning when its far edge clears the screen.
        const int16_t rightWall = span > static_cast<int16_t>(width())
                                      ? static_cast<int16_t>(width())   // scroll the block past
                                      : static_cast<int16_t>(width()) - span;
        const int16_t leftWall = span > static_cast<int16_t>(width())
                                     ? static_cast<int16_t>(width() - span)   // negative: keep going
                                     : 0;
        if (nx < leftWall || nx > rightWall) {
            dir_ = static_cast<int8_t>(-dir_);
            oy_ = static_cast<int16_t>(oy_ + dropY);
            // Landed: the invasion succeeds and the board resets, which is the attract loop.
            if (oy_ + formationHeight() >= static_cast<int16_t>(height())) reset();
        } else {
            ox_ = nx;
        }
        fireInvaderShot();
        aimCannon();
    }

    // --- Geometry --------------------------------------------------------------------------
    //
    // Cell size comes from the WIDEST sprite, so the ranks line up in columns however their own
    // art differs. A per-species pitch would make the formation ragged.
    uint8_t cell() const { return static_cast<uint8_t>((invart::OW + 2) * scale()); }
    uint8_t rowPitch() const { return static_cast<uint8_t>((invart::OH + 2) * scale()); }
    uint8_t scale() const { return size == 0 ? 1 : size; }
    int16_t formationWidth() const { return static_cast<int16_t>(cell() * kCols); }
    int16_t formationHeight() const { return static_cast<int16_t>(rowPitch() * kRows); }

    void drawFormation(const draw::Canvas& cv) {
        RGB pal[invart::kPaletteCount];
        for (uint8_t r = 0; r < kRows; r++) {
            // One palette entry per rank, so the ranks read as different creatures without a
            // second drawing. The arcade used one color per rank for the same reason.
            paletteFor(pal, static_cast<uint8_t>(r * 40 + 30));
            for (uint8_t c = 0; c < kCols; c++) {
                if (!grid_[r][c]) continue;
                const lengthType px = static_cast<lengthType>(ox_ + c * cell());
                const lengthType py = static_cast<lengthType>(oy_ + r * rowPitch());
                drawInvader(cv, speciesFor(r), pal, px, py);
            }
        }
    }

    void drawInvader(const draw::Canvas& cv, uint8_t species,
                     const RGB (&pal)[invart::kPaletteCount], lengthType px, lengthType py) {
        const uint8_t sc = scale();
        if (species == kSquid) {
            const draw::sprites::Sprite s{invart::kSquid, pal, invart::W, invart::H,
                                          invart::F, invart::kPaletteCount};
            draw::sprite(cv, s, frame_, px, py, sc);
        } else if (species == kCrab) {
            const draw::sprites::Sprite s{invart::kCrab, pal, invart::CW, invart::CH,
                                          invart::CF, invart::kPaletteCount};
            draw::sprite(cv, s, frame_, px, py, sc);
        } else {
            const draw::sprites::Sprite s{invart::kOcto, pal, invart::OW, invart::OH,
                                          invart::OF, invart::kPaletteCount};
            draw::sprite(cv, s, frame_, px, py, sc);
        }
    }

    void drawCannon(const draw::Canvas& cv) {
        RGB pal[invart::kPaletteCount];
        // The cannon is green in every version of this game, and it is the one color here that is
        // not the palette's to choose: a cannon in the formation's own color reads as an invader.
        pal[invart::kClear] = RGB{0, 0, 0};
        pal[invart::kBody]  = RGB{40, 230, 60};
        pal[invart::kDark]  = RGB{20, 120, 30};
        pal[invart::kEye]   = RGB{200, 255, 200};
        const uint8_t sc = scale();
        // Clamped: on a panel shorter than the scaled cannon this goes negative and the cannon is
        // drawn off the top edge instead of standing on the floor.
        const int32_t top = static_cast<int32_t>(height()) - invart::GH * sc;
        const lengthType py = static_cast<lengthType>(top < 0 ? 0 : top);
        const draw::sprites::Sprite s{invart::kCannon, pal, invart::GW, invart::GH,
                                      invart::GF, invart::kPaletteCount};
        draw::sprite(cv, s, 0, static_cast<lengthType>(cannonX_), py, sc);
    }

    // --- Shots -----------------------------------------------------------------------------
    //
    // A shot is three numbers rather than a particle: they fly straight up or straight down, and a
    // pool with forces and drag would be machinery for a bullet that needs none.

    void moveShots() {
        for (uint8_t i = 0; i < kShots; i++) {
            if (shotTtl_[i] == 0) continue;
            shotY_[i] = static_cast<int16_t>(shotY_[i] + shotDir_[i] * kShotSpeed);
            if (shotY_[i] < 0 || shotY_[i] >= static_cast<int16_t>(height())) { shotTtl_[i] = 0; continue; }
            if (shotTtl_[i] > 0) shotTtl_[i]--;
            if (shotDir_[i] < 0) hitTest(i);          // only the cannon's shot can hit an invader
        }
    }

    /// A rising shot takes out the invader whose cell it is in. Cell arithmetic rather than a
    /// per-sprite test: the formation IS a grid, so asking which cell a point is in is the whole
    /// collision, and a pixel-accurate hit would not read differently at this size.
    void hitTest(uint8_t i) {
        const int16_t rx = static_cast<int16_t>(shotX_[i] - ox_);
        const int16_t ry = static_cast<int16_t>(shotY_[i] - oy_);
        if (rx < 0 || ry < 0) return;
        const int16_t c = static_cast<int16_t>(rx / cell());
        const int16_t r = static_cast<int16_t>(ry / rowPitch());
        if (c >= kCols || r >= kRows) return;
        if (!grid_[r][c]) return;
        grid_[r][c] = 0;
        if (alive_ > 0) alive_--;
        shotTtl_[i] = 0;
        // Cleared: the attract loop starts over rather than leaving an empty sky.
        if (alive_ == 0) reset();
    }

    void drawShots(const draw::Canvas& cv) {
        for (uint8_t i = 0; i < kShots; i++) {
            if (shotTtl_[i] == 0) continue;
            const RGB col = shotDir_[i] < 0 ? RGB{200, 255, 200} : RGB{255, 200, 80};
            for (uint8_t k = 0; k < 3; k++)
                draw::pixel(cv, {static_cast<lengthType>(shotX_[i]),
                                 static_cast<lengthType>(shotY_[i] + k), 0}, col);
        }
    }

    void spawnShot(int16_t x, int16_t y, int8_t dir) {
        for (uint8_t i = 0; i < kShots; i++) {
            if (shotTtl_[i] != 0) continue;
            shotX_[i] = x; shotY_[i] = y; shotDir_[i] = dir; shotTtl_[i] = 255;
            return;
        }
    }

    /// The lowest invader in a random column fires, which is what the arcade does: a shot from
    /// behind the formation would pass through its own ranks.
    void fireInvaderShot() {
        const uint8_t c = static_cast<uint8_t>(hashInt(tickSeed_++, 3) % kCols);
        for (int8_t r = kRows - 1; r >= 0; r--) {
            if (!grid_[r][c]) continue;
            spawnShot(static_cast<int16_t>(ox_ + c * cell() + cell() / 2),
                      static_cast<int16_t>(oy_ + r * rowPitch() + rowPitch()), +1);
            return;
        }
    }

    /// The cannon tracks the nearest live column and fires when it is lined up. Tracking rather
    /// than teleporting: the slide is most of what the cannon does on screen.
    void aimCannon() {
        int16_t target = cannonX_;
        for (uint8_t r = 0; r < kRows; r++) {
            bool found = false;
            for (uint8_t c = 0; c < kCols; c++) {
                if (!grid_[r][c]) continue;
                target = static_cast<int16_t>(ox_ + c * cell());
                found = true;
                break;
            }
            if (found) break;
        }
        const int16_t step = static_cast<int16_t>(cell() / 2 + 1);
        if (cannonX_ < target) cannonX_ = static_cast<int16_t>(cannonX_ + step);
        else if (cannonX_ > target) cannonX_ = static_cast<int16_t>(cannonX_ - step);
        const int16_t maxX = static_cast<int16_t>(width() - invart::GW * scale());
        if (cannonX_ < 0) cannonX_ = 0;
        if (cannonX_ > maxX) cannonX_ = maxX > 0 ? maxX : 0;

        // Fire when roughly lined up, and only sometimes. A cannon that hits on every step clears
        // a full board in seconds, which is a progress bar rather than a game: the arcade takes a
        // minute, and the watching is the point. One shot in four leaves the formation on screen
        // long enough to see it march, thin, and speed up.
        if (target - cannonX_ < step && cannonX_ - target < step
            && (hashInt(tickSeed_++, 9) & 3) == 0)
            spawnShot(static_cast<int16_t>(cannonX_ + (invart::GW * scale()) / 2),
                      static_cast<int16_t>(height() - invart::GH * scale()), -1);
    }

    void paletteFor(RGB (&pal)[invart::kPaletteCount], uint8_t entry) const {
        const RGB body = colorFromPalette(*Palettes::active(), entry);
        pal[invart::kClear] = RGB{0, 0, 0};
        pal[invart::kBody]  = body;
        pal[invart::kDark]  = blend(body, RGB{0, 0, 0}, 140);
        pal[invart::kEye]   = RGB{10, 10, 14};
    }

    static constexpr int16_t kShotSpeed = 2;

    uint8_t  grid_[kRows][kCols] = {};
    uint16_t alive_ = 0;
    int16_t  ox_ = 0, oy_ = 0;
    int8_t   dir_ = 1;
    uint8_t  frame_ = 0;
    uint32_t lastPhase_ = 0;   // full counter: a uint8_t copy stopped matching past 255 beats
    int16_t  cannonX_ = 0;
    int16_t  shotX_[kShots] = {}, shotY_[kShots] = {};
    int8_t   shotDir_[kShots] = {};
    uint8_t  shotTtl_[kShots] = {};
    uint32_t tickSeed_ = 0;
    BeatPhase formation_;
};

}  // namespace mm
