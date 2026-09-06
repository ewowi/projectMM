#pragma once
// Sprite fountain: the cast of the sprite effects thrown up from the floor and falling back.
//
// A fountain is a particle system, and the sprite effects already own a cast: fish, Pacman and his
// ghosts, toasters and their toast. This effect is the two put together, so the pixel art is
// SHARED rather than copied. Each art pack already lives in its own namespace and each is a plain
// header, so drawing a fish here is an include and a draw::sprite call: no second copy of the
// pixels, and a fix to a fish fixes it in both places.
//
// The particle pool carries one byte per particle (`hue`, its palette index elsewhere), and that
// byte is what chooses the sprite here. Widening the pool for a sprite id would cost every particle
// system in the project memory for a field only this effect reads.
// Author: projectMM original

#include "core/AudioService.h"   // latestFrame: the spectrum the audio-reactive mode reads
#include "core/math16.h"
#include "light/draw.h"
#include "light/effects/EffectBase.h"
#include "light/effects/SpriteCast.h"           // the shared cast: every sprite, one draw call
#include "light/particles.h"

namespace mm {

/// Effect: a fountain of sprites, thrown up and falling back under gravity.
/// @card SpriteFountainEffect.gif
class SpriteFountainEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🎶✨👾"; }   // audio-reactive when audioReactive is set
    Dim dimensions() const override { return Dim::D2; }

    /// How hard the nozzle throws, and how hard gravity pulls back. Both scale with the grid, so
    /// the plume fills a small panel and a wall alike rather than being tuned for one size.
    ///
    /// Gentler than a spark fountain's, and deliberately: a spark is one pixel and reads as a
    /// streak however fast it goes, while a 12x8 toaster crossing the panel in half a second is a
    /// blur. These are set so a sprite is legible for its whole arc, which is the point of throwing
    /// sprites rather than sparks.
    /// Measured rather than guessed: at pull 7 a sprite was aloft 0.9 s, which is not long enough
    /// to read a 12x8 toaster, and the plume thinned because nothing stayed up. pull 3 gives a 2 s
    /// arc and a steady dozen sprites in the air.
    uint8_t lift = 45;
    uint8_t pull = 3;
    /// Sprites launched per BEAT of the emit clock, not per frame. Per frame meant the rate rose
    /// with the frame rate: the same setting was a trickle on an ESP32 and a firehose on a desktop
    /// running ten times faster.
    uint8_t rate = 2;
    /// Launches per minute. The nozzle's own tempo, so the plume's density is a choice rather than
    /// a side effect of how fast the device happens to run.
    uint8_t emitBpm = 120;
    /// Draw each sprite at this many pixels per art pixel. 1 on a small panel; a wall can afford 2.
    uint8_t size = 1;

    /// Let the music decide what comes out. A BAND launches a sprite when it is loud, and the band
    /// picks WHICH sprite, so the cast maps onto the spectrum: bass throws toasters, treble throws
    /// tiny fish. That is the difference between a fountain that pulses and one you can read the
    /// music off. In silence nothing is thrown, which is what makes the mode look reactive rather
    /// than merely animated.
    bool audioReactive = false;

    void defineControls() override {
        controls_.addControl("lift", lift, 20, 200);
        controls_.addControl("pull", pull, 2, 60);   // min below the tuned 3, which a min of 4 clamped away
        controls_.addControl("rate", rate, 1, 6);
        controls_.addControl("emitBpm", emitBpm, 10, 240);
        controls_.addControl("size", size, 1, 4);
        controls_.addControl("audioReactive", audioReactive);
    }

    void prepare() override {
        // Caller-owned arrays, wired into the pool view: particles::Pool is a view over storage the
        // effect owns, not a container, so a ScratchBuffer per field is the shape every particle
        // effect uses (and what keeps dynamicBytes honest).
        px_.resize(kPoolSize); py_.resize(kPoolSize);
        vx_.resize(kPoolSize); vy_.resize(kPoolSize);
        ttl_.resize(kPoolSize); hue_.resize(kPoolSize);
        pool_ = particles::Pool{};
        if (px_ && py_ && vx_ && vy_ && ttl_ && hue_) {
            pool_.x = px_.data(); pool_.y = py_.data();
            pool_.vx = vx_.data(); pool_.vy = vy_.data();
            pool_.ttl = ttl_.data(); pool_.hue = hue_.data();
            pool_.count = kPoolSize;
            pool_.clear();
        }
        launch_ = BeatPhase{};
        emit_ = BeatPhase{};
        lastEmit_ = 0;
        seed_ = 0;
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        if (width() == 0 || height() == 0) return;
        if (!pool_.valid()) return;

        draw::fill(cv, RGB{0, 0, 0});

        // Throw from the middle of the floor, leaning either side of straight up. 47152 is just
        // under vertical in angle16, the same nozzle the MoonLive fountain script uses.
        launch_.advanceTo(elapsed(), 9);
        const angle16 aim = static_cast<angle16>(
            47152 + (sin16(static_cast<uint16_t>(launch_.phase(65536))) / 8));
        const draw::pos_t speed = static_cast<draw::pos_t>(lift) * height() / 4;
        const draw::pos_t ox = static_cast<draw::pos_t>(width() / 2) * draw::kSubOne;
        const draw::pos_t oy = static_cast<draw::pos_t>(height() - 1) * draw::kSubOne;
        // Audio is read ONCE per frame: the spectrum is the same for every sprite, and a per-sprite
        // read would be the same work times the rate.
        const AudioFrame* audio = audioReactive ? AudioService::latestFrame() : nullptr;
        // The emit CLOCK, separate from the nozzle's sweep: firing every frame tied the plume's
        // density to the frame rate, so the same settings looked completely different on two
        // devices. A beat phase makes `emitBpm` mean launches per minute on any of them.
        emit_.advanceTo(elapsed(), emitBpm);
        const uint32_t tick = emit_.phase(2);
        const bool due = tick != lastEmit_;
        lastEmit_ = tick;
        if (audio) { if (due) emitByBand(*audio, ox, oy, aim, speed); }
        else if (due) emitSteady(ox, oy, aim, speed);

        // Physics by ELAPSED TIME, not per frame. The pool's three verbs each take a FrameTime
        // scale (256 == one 60 fps frame) for exactly this: without it a 1200 fps desktop took 20
        // steps where a 60 fps board took one, so the same fountain emptied 20x faster and showed a
        // fraction of the sprites. Same idiom as ParticlesEffect.
        const uint32_t slice = time_.advance(elapsed());
        if (slice > 0) {
            pool_.gravity(static_cast<draw::pos_t>(pull) * height() / 16, slice);
            pool_.drag(2, slice);
            pool_.step(slice);
        }
        // Free the slot of anything that has left the panel. WITHOUT THIS the fountain dies: a
        // sprite thrown clear of the top keeps its ttl forever, the pool fills, and every later
        // emit silently fails because there is no free slot. It ran for three seconds and then
        // stopped. The margin is generous so a big sprite is not culled while half of it still
        // shows.
        pool_.killOutside(static_cast<draw::pos_t>(width()) * draw::kSubOne,
                          static_cast<draw::pos_t>(height()) * draw::kSubOne,
                          static_cast<draw::pos_t>(24) * draw::kSubOne);
        pool_.age(1, slice);   // ttl counts reference frames, so it ages by time like the physics

        drawSprites(cv);
    }

private:
    /// The steady fountain: `rate` sprites a frame, species picked at random.
    void emitSteady(draw::pos_t ox, draw::pos_t oy, angle16 aim, draw::pos_t speed) {
        for (uint8_t k = 0; k < rate; k++) {
            // The `hue` byte carries WHICH SPRITE, not a color: the cast is picked once at launch
            // so a particle keeps its identity for its whole flight. A sprite that changed species
            // mid-arc would read as a glitch.
            const uint8_t kind = static_cast<uint8_t>(hashInt(seed_, k, 7) % spritecast::kKindCount);
            // One at a time: angleEmit's own loop would give every sprite in a frame the same
            // species, and the cast is the point of this effect.
            pool_.angleEmit(ox, oy, aim, speed, /*cone=*/3000, /*n=*/1, /*life=*/160, kind, seed_);
            seed_++;
        }
    }

    /// The reactive fountain: ONE SPRITE PER BAND, thrown when that band is loud.
    ///
    /// The 16 bands map onto the 10 sprite kinds in order, so a species belongs to a part of the
    /// spectrum: the bass bands throw fish, the treble bands throw invaders, and the plume's
    /// makeup tells you what the music is doing. A band also throws HARDER when it is louder, so a kick launches
    /// its sprite over the top of the panel while a quiet passage lobs one just clear of the
    /// nozzle. Silence throws nothing at all.
    void emitByBand(const AudioFrame& audio, draw::pos_t ox, draw::pos_t oy,
                    angle16 aim, draw::pos_t speed) {
        if (audio.levelSmoothed < kSilence) return;      // a quiet room is a still fountain
        for (uint8_t b = 0; b < kBandCount; b++) {
            const uint8_t mag = audio.bands[b];
            if (mag < kBandFloor) continue;              // that band is not playing
            // Rate throttles the reactive mode too, so a busy mix does not fill the pool in one
            // frame: it is the odds of a loud band actually firing.
            if ((hashInt(seed_, b, 5) & 0x0F) >= rate * 3) continue;
            const uint8_t kind = static_cast<uint8_t>((b * spritecast::kKindCount) / kBandCount);
            // Loud throws high: 50% of the nozzle's speed at the floor, full speed at the top.
            const draw::pos_t v = static_cast<draw::pos_t>(speed / 2 + (speed * mag) / 512);
            pool_.angleEmit(ox, oy, aim, v, /*cone=*/3000, /*n=*/1, /*life=*/160, kind, seed_);
            seed_++;
        }
    }

    static constexpr uint16_t kSilence   = 8;    ///< below this the room is quiet, not playing
    static constexpr uint8_t  kBandFloor = 40;   ///< a band under this is not throwing anything
    static constexpr uint8_t  kBandCount = 16;   ///< AudioFrame's spectrum width

    /// Draw one particle as its sprite, centered on the particle's position. Sprites are drawn
    /// from their top-left, so the half-extents come off first: a fountain of sprites hanging
    /// below and right of where the physics says they are looks like a lag, not an offset.
    void drawSprites(const draw::Canvas& cv) {
        const uint8_t sc = size == 0 ? 1 : size;
        const uint8_t beat = static_cast<uint8_t>(launch_.phase(4) & 0xFF);
        for (uint16_t i = 0; i < pool_.count; i++) {
            if (pool_.ttl[i] == 0) continue;
            const lengthType px = static_cast<lengthType>(pool_.x[i] / draw::kSubOne);
            const lengthType py = static_cast<lengthType>(pool_.y[i] / draw::kSubOne);
            // Facing follows travel, as in the source effects: a cast all facing one way while
            // half of them fly the other reads as wallpaper.
            const bool flip = pool_.vx[i] < 0;
            // `hue` is spent on the sprite kind, so the COLOR index comes from the particle's
            // own slot: stable for its whole flight (so it does not flicker between frames) and
            // spread across the palette so a plume is not one color.
            const uint8_t entry = static_cast<uint8_t>(hashInt(i, 11) & 0xFF);
            spritecast::draw(cv, pool_.hue[i], entry, px, py, sc, flip, beat);
        }
    }

    /// Enough headroom that emission never stalls. A sprite is up to 16x11 art pixels, so far
    /// fewer are readable on screen than a spark fountain's few hundred, but the POOL has to hold
    /// every sprite still in flight: sized to what is visible, a full pool starves the nozzle and
    /// the plume arrives in pulses instead of flowing.
    static constexpr uint16_t kPoolSize = 192;
    ScratchBuffer<draw::pos_t> px_{*this}, py_{*this}, vx_{*this}, vy_{*this};
    ScratchBuffer<uint16_t> ttl_{*this};
    ScratchBuffer<uint8_t> hue_{*this};
    particles::Pool pool_;
    BeatPhase launch_;
    BeatPhase emit_;
    // The FULL phase counter, not a truncated copy: phase() returns uint32_t and keeps counting,
    // so a uint8_t latch stopped comparing equal after 255 beats and the fountain fired every
    // frame from then on -- the exact frame-rate coupling the beat clock exists to remove.
    uint32_t  lastEmit_ = 0;
    particles::FrameTime time_;   // the physics clock: one step per reference frame, not per frame
    uint32_t seed_ = 0;
};

}  // namespace mm
