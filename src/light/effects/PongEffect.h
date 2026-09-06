#pragma once
// Pong: two paddles, a ball, and a rally that never ends.
//
// The attract-mode reading of the 1972 original, so both paddles play themselves. A perfect tracker
// would rally forever and never look like a game, so each paddle has a REACTION DELAY and a small
// aiming error: it starts moving a moment after the ball turns, and aims slightly off center. That
// is what produces near-misses, edge hits and the occasional point, which is the whole appeal.
//
// The ball is either the classic square or one of the shared sprite cast, which is why that cast
// lives in its own header: a Pacman crossing the court between two paddles is the same code path as
// the fountain throwing one.
// Author: projectMM original

#include "core/AudioService.h"   // latestFrame: the beat the audio-reactive mode volleys on
#include "core/math16.h"
#include "light/draw.h"
#include "light/effects/EffectBase.h"
#include "light/effects/SpriteCast.h"   // the shared cast, when the ball is a sprite

namespace mm {

/// Effect: two self-playing paddles rallying a ball across the grid.
/// @card PongEffect.gif
class PongEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🎵👾"; }   // audio-reactive when audioReactive is set
    Dim dimensions() const override { return Dim::D2; }

    /// Rally speed, in ball crossings per minute rather than pixels per frame: the court is a
    /// different width on every grid, so a pixel rate would be a sprint on one panel and a crawl on
    /// the next. This is how long the ball takes to get from one paddle to the other.
    uint8_t rallyBpm = 40;
    /// Paddle length as a percentage of the court's height. Short paddles miss more, which is what
    /// makes points happen at all.
    uint8_t paddle = 30;
    /// How sharply a paddle chases the ball, 0 sluggish to 255 instant. Below full speed it lags
    /// behind a fast ball, which is where the misses come from.
    uint8_t reflex = 150;
    /// Draw the ball at this many pixels per art pixel, when it is a sprite.
    uint8_t size = 1;
    /// Swap the square ball for a member of the shared sprite cast, re-picked on every hit.
    bool spriteBall = false;
    /// Let the music drive the rally: the ball only advances on the beat, so it crosses the court
    /// in time with the track and stands still in silence.
    bool audioReactive = false;

    void defineControls() override {
        controls_.addControl("rallyBpm", rallyBpm, 5, 200);
        controls_.addControl("paddle", paddle, 10, 60);
        controls_.addControl("reflex", reflex, 40, 255);
        controls_.addControl("size", size, 1, 4);
        controls_.addControl("spriteBall", spriteBall);
        controls_.addControl("audioReactive", audioReactive);
    }

    void setup() override {
        EffectBase::setup();
        serve(true);
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        if (width() == 0 || height() == 0) return;
        draw::fill(cv, RGB{0, 0, 0});

        const AudioFrame* audio = audioReactive ? AudioService::latestFrame() : nullptr;
        // The same beat test the rest of the project uses: level against its own smoothed average.
        // Reading it once per frame keeps the audio path off the per-pixel work.
        const bool live = audio && audio->levelSmoothed >= kSilence;
        const bool beat = live && audio->level > audio->levelSmoothed + kBeatMargin;

        // Motion by ELAPSED TIME. `rallyBpm` is crossings per minute, so a frame's share of a
        // crossing is the same on a 60 fps board and a 1200 fps desktop: the ball travels the court
        // in the same wall-clock time on both, rather than 20x faster on the quick one.
        rally_.advanceTo(elapsed(), rallyBpm);
        const uint32_t travel = rally_.phase(kCourtScale);
        // The two modes keep two different clocks: free-running counts elapsed time, reactive
        // counts beats. Switching between them mid-rally would hand step() a jump between the two
        // -- the ball teleporting across the court one way, and an unsigned underflow to a
        // four-billion step the other. Rebase both to the current position instead, so the toggle
        // is seamless and the ball carries on from where it is.
        if (audioReactive != wasReactive_) {
            wasReactive_ = audioReactive;
            lastTravel_ = travelAt_;
        }
        if (audioReactive) {
            // On the beat the ball JUMPS a slice of the court and then waits. Silence holds it
            // still, which is what makes the mode read as reactive rather than merely animated.
            if (beat) travelAt_ += kCourtScale / kBeatSteps;
        } else {
            // Free-running: adopt the elapsed-time clock by moving the baseline with it, so the
            // difference step() sees is this frame's travel and not the whole history.
            travelAt_ += travel - lastFreeTravel_;
        }
        lastFreeTravel_ = travel;
        step();
        render(cv);
    }

private:
    /// One update of the ball and both paddles, in fixed point across a court of kCourtScale units.
    void step() {
        const uint32_t now = travelAt_;
        const uint32_t moved = now - lastTravel_;
        if (moved == 0) return;                  // no time passed, or the beat has not landed yet
        lastTravel_ = now;

        // The ball advances along its axis and drifts across it; both are fractions of the court,
        // so the trajectory is identical whatever the grid.
        bx_ += static_cast<int32_t>(moved) * dirX_;
        by_ += static_cast<int32_t>(moved) * driftY_ / kDriftScale;

        // Top and bottom walls: reflect, the way the original does.
        if (by_ < 0) { by_ = -by_; driftY_ = static_cast<int16_t>(-driftY_); }
        if (by_ > kCourtScale) { by_ = 2 * kCourtScale - by_; driftY_ = static_cast<int16_t>(-driftY_); }

        chase(0, moved);
        chase(1, moved);

        // The paddles sit at the ends of the court. A hit sends the ball back with a new drift, so
        // no two rallies repeat; a miss is a point and a fresh serve from the other side.
        if (dirX_ < 0 && bx_ <= kPaddleX) {
            if (hits(0)) bounce(0); else serve(false);
        } else if (dirX_ > 0 && bx_ >= kCourtScale - kPaddleX) {
            if (hits(1)) bounce(1); else serve(true);
        }
    }

    /// A paddle chases the ball's height, but only after its reaction delay has run out and only
    /// as fast as `reflex` allows. Both are what keep it from being a perfect wall.
    void chase(uint8_t p, uint32_t moved) {
        if (delay_[p] > moved) { delay_[p] = static_cast<uint16_t>(delay_[p] - moved); return; }
        delay_[p] = 0;
        // Aim at the ball plus this paddle's own error, so it meets the ball off-center and
        // sometimes at the very edge. A paddle aiming true would never miss.
        const int32_t target = by_ + aim_[p];
        const int32_t gap = target - py_[p];
        const int32_t stepBy = static_cast<int32_t>(moved) * reflex / 255;
        if (gap > stepBy)       py_[p] += stepBy;
        else if (gap < -stepBy) py_[p] -= stepBy;
        else                    py_[p] = target;
        if (py_[p] < 0) py_[p] = 0;
        if (py_[p] > kCourtScale) py_[p] = kCourtScale;
    }

    /// Did paddle `p` get there in time? Its reach is half its length either side of its center.
    bool hits(uint8_t p) const {
        const int32_t half = kCourtScale * paddle / 200;   // paddle% of the court, halved
        const int32_t d = by_ - py_[p];
        return d >= -half && d <= half;
    }

    /// Send the ball back. WHERE on the paddle it landed sets the new drift, which is the one piece
    /// of skill the original had: the edges angle it away, the middle sends it back flat.
    void bounce(uint8_t p) {
        const int32_t half = kCourtScale * paddle / 200;
        const int32_t off = half == 0 ? 0 : (by_ - py_[p]) * kMaxDrift / half;
        driftY_ = static_cast<int16_t>(off);
        dirX_ = static_cast<int8_t>(-dirX_);
        bx_ = dirX_ > 0 ? kPaddleX : kCourtScale - kPaddleX;
        // Both paddles react to the turn, the far one with the longer delay: it has the length of
        // the court to get there, and starting instantly is what makes an unbeatable player.
        delay_[0] = react(0);
        delay_[1] = react(1);
        pickBall();
        seed_++;
    }

    /// A new character for the ball. Re-rolled on every HIT, not only on a point: the swap then
    /// lands on the impact, which reads as the paddle knocking one thing away and another back,
    /// rather than as the ball changing its mind mid-court.
    void pickBall() {
        kind_ = static_cast<uint8_t>(hashInt(seed_, 5, 11) % spritecast::kKindCount);
        entry_ = static_cast<uint8_t>(hashInt(seed_, 9, 13) & 0xFF);
    }

    /// A point: the ball restarts from the middle heading at whoever just conceded.
    void serve(bool toRight) {
        bx_ = kCourtScale / 2;
        by_ = kCourtScale / 2;
        dirX_ = toRight ? 1 : -1;
        driftY_ = static_cast<int16_t>(static_cast<int32_t>(hashInt(seed_, 3, 7) % (2 * kMaxDrift)) - kMaxDrift);
        py_[0] = py_[1] = kCourtScale / 2;
        delay_[0] = react(0);
        delay_[1] = react(1);
        pickBall();
        seed_++;
    }

    /// This paddle's reaction delay and aiming error for the coming exchange, both re-rolled every
    /// time so one paddle is not permanently the weaker player.
    uint16_t react(uint8_t p) {
        aim_[p] = static_cast<int16_t>(static_cast<int32_t>(hashInt(seed_, p, 17) % (2 * kMaxAim)) - kMaxAim);
        return static_cast<uint16_t>(hashInt(seed_, p + 2, 19) % kMaxDelay);
    }

    void render(const draw::Canvas& cv) {
        const lengthType w = width(), h = height();
        const int32_t courtW = w > 1 ? w - 1 : 1;
        const int32_t courtH = h > 1 ? h - 1 : 1;
        const RGB fg = colorFromPalette(*Palettes::active(), 200);

        // The net: the dashed center line the original drew, and the only thing on screen that says
        // this is a court rather than two blocks and a dot.
        const lengthType netX = static_cast<lengthType>(w / 2);
        const RGB net = blend(fg, RGB{0, 0, 0}, 170);
        for (lengthType y = 0; y < h; y += 3) draw::pixel(cv, {netX, y, 0}, net);

        // Paddles: a column at each end, `paddle` percent of the court tall.
        const int32_t half = courtH * paddle / 200;
        for (uint8_t p = 0; p < 2; p++) {
            const lengthType px = p == 0 ? 0 : static_cast<lengthType>(w - 1);
            const int32_t cy = py_[p] * courtH / kCourtScale;
            for (int32_t y = cy - half; y <= cy + half; y++)
                if (y >= 0 && y < h) draw::pixel(cv, {px, static_cast<lengthType>(y), 0}, fg);
        }

        const lengthType bxp = static_cast<lengthType>(bx_ * courtW / kCourtScale);
        const lengthType byp = static_cast<lengthType>(by_ * courtH / kCourtScale);
        if (spriteBall) {
            // The sprite faces its travel, like every other sprite in the project.
            spritecast::draw(cv, kind_, entry_, bxp, byp, size == 0 ? 1 : size, dirX_ < 0,
                             static_cast<uint8_t>(rally_.phase(4) & 0xFF));
        } else {
            draw::pixel(cv, {bxp, byp, 0}, fg);
        }
    }

    /// The court is fixed point rather than pixels so the game plays identically on a 16x16 panel
    /// and a 256-wide wall: every position is a fraction of the court, scaled to the grid only when
    /// it is drawn.
    static constexpr int32_t  kCourtScale = 4096;
    static constexpr int32_t  kPaddleX    = 96;     ///< how far in from each end a paddle sits
    static constexpr int32_t  kDriftScale = 256;    ///< drift is a fraction of forward travel
    static constexpr int32_t  kMaxDrift   = 320;    ///< the steepest angle a bounce can produce
    static constexpr int32_t  kMaxAim     = 220;    ///< how far off center a paddle aims
    static constexpr uint16_t kMaxDelay   = 260;    ///< the longest reaction delay, in court units
    static constexpr uint8_t  kBeatSteps  = 12;     ///< beats to cross the court in reactive mode
    static constexpr uint16_t kSilence    = 8;      ///< below this the room is quiet, not playing
    static constexpr uint16_t kBeatMargin = 24;     ///< a transient this far over the average is a beat

    BeatPhase rally_;
    uint32_t  travelAt_ = 0;     ///< how far the rally has travelled, in court units
    uint32_t  lastTravel_ = 0;
    uint32_t  lastFreeTravel_ = 0;   ///< last free-running reading, so a toggle costs no distance
    bool      wasReactive_ = false;  ///< which clock ran last frame; a change rebases the baseline
    int32_t   bx_ = kCourtScale / 2, by_ = kCourtScale / 2;
    int8_t    dirX_ = 1;
    int16_t   driftY_ = 90;
    int32_t   py_[2] = {kCourtScale / 2, kCourtScale / 2};
    uint16_t  delay_[2] = {0, 0};
    int16_t   aim_[2] = {0, 0};
    uint8_t   kind_ = 0, entry_ = 0;
    uint32_t  seed_ = 1;
};

}  // namespace mm
