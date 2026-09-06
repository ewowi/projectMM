#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

// VuMeters: sixteen needles, one per band, each on a real meter's mechanics.
//
// The thing that makes a VU meter beautiful is not the dial, it is the NEEDLE'S MASS. A physical
// meter is a spring and a damper: the needle accelerates toward the signal, overshoots a peak,
// swings back, and settles. That overshoot is why a mechanical meter reads as alive where a bar
// graph reads as a readout, and it is why the standard (IEC 60268-17) specifies 300 ms to 99% with
// 1 to 1.5% overshoot rather than a smoothing constant.
//
// So each band drives a damped harmonic oscillator, integrated per frame. `damping` sets how much
// the needle overshoots: high is a critically damped studio meter, low is a loose needle that
// swings past and bounces. The bass needles are deliberately heavier than the treble ones, as they
// are on a real multi-meter bridge, so the low end swings and the high end flickers.
//
// Each needle sweeps its own arc in its own column, with a peak marker held at the highest
// reading and falling slowly (a peak-hold, the other half of the standard meter), and a red zone
// past three quarters.
// @card VuMetersEffect.png
/// Effect: sixteen VU needles with real mass, one per band, with peak-hold and a red zone.
class VuMetersEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🎶🖌️"; }
    Dim dimensions() const override { return Dim::D3; }   // a plane; a cube gets one bank per slice

    uint8_t damping   = 150;  // how much the needle overshoots: low swings, high is critical
    uint8_t response  = 120;  // how quickly it chases the signal at all
    uint8_t peakHold  = 200;  // how long the peak marker stays up
    bool    smooth    = false;// drive from the ballistic rather than the raw band

    void defineControls() override {
        controls_.addControl("damping", damping, 0, 255);
        controls_.addControl("response", response, 1, 255);
        controls_.addControl("peakHold", peakHold, 0, 255);
        controls_.addControl("smooth", smooth);
    }

    void prepare() override {
        for (uint8_t b = 0; b < 16; b++) { pos_[b] = 0; vel_[b] = 0; peak_[b] = 0; }
        started_ = false;
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height(), dep = depth();
        if (w < 2 || h < 2) return;
        const uint32_t now = elapsed();
        const uint32_t dt = started_ ? now - lastMs_ : 0u;
        lastMs_ = now;
        started_ = true;

        const AudioFrame* f = AudioService::latestFrame();

        // The needles: a damped spring per band, integrated in fixed point. Time-stepped so the
        // mechanics are the same at any framerate, and clamped so a stall cannot explode it.
        const uint32_t step = dt > 100u ? 100u : dt;
        const int32_t k = 4 + static_cast<int32_t>(response) / 4;          // spring: how hard it pulls
        const int32_t c = 1 + static_cast<int32_t>(damping) / 12;          // damper: how much it fights
        for (uint8_t b = 0; b < 16; b++) {
            const int32_t target = static_cast<int32_t>(f ? (smooth ? f->bandsSmoothed[b] : f->bands[b]) : 0) << 8;
            // Heavier at the bass end, as a real meter bridge is: the low needles swing, the high
            // ones flicker. A sixteenth of the response per band is enough to read as different.
            const int32_t mass = 16 + (15 - b);
            const int32_t accel = ((target - pos_[b]) * k) / mass - (vel_[b] * c) / 16;
            vel_[b] += (accel * static_cast<int32_t>(step)) / 64;
            pos_[b] += (vel_[b] * static_cast<int32_t>(step)) / 64;
            if (pos_[b] < 0) { pos_[b] = 0; if (vel_[b] < 0) vel_[b] = -vel_[b] / 3; }   // bounce off the pin
            const int32_t full = 255 << 8;
            if (pos_[b] > full) { pos_[b] = full; if (vel_[b] > 0) vel_[b] = 0; }
            const uint8_t reading = static_cast<uint8_t>(pos_[b] >> 8);
            // Peak-hold: takes a new maximum at once, falls back slowly.
            // Peak-hold falls by a half-life, so "how long it stays up" is stated in seconds and
            // holds at any framerate, the same rule the trail effects decay by.
            // The real elapsed time, not `step`: the cap exists to stop a stall throwing the
            // spring, but a half-life is stated in seconds and must count every millisecond that
            // passed, or the peak hangs longer than asked for after any hitch.
            const uint16_t keep = halfLifeKeep(dt, 200u + static_cast<uint32_t>(peakHold) * 12u);
            peak_[b] = reading > peak_[b]
                     ? reading
                     : static_cast<uint8_t>((static_cast<uint32_t>(peak_[b]) * keep) >> 16);
        }

        // The bank is a GRID of dials, not a row of slits: sixteen meters tiled so each cell is as
        // square as the panel allows. A 64x64 panel gives 4x4 cells of 16x16, a 256x64 wall gives
        // 8x2 cells of 32x32, and a strip degrades to one row. Each meter then owns a real dial
        // rather than a column a few pixels wide.
        lengthType cols = 16, rows = 1;
        bestTiling(w, h, cols, rows);
        const lengthType cellW = w / cols, cellH = h / rows;
        draw::fill(cv, RGB{0, 0, 0});
        if (cellW < 3 || cellH < 3) return;            // no room for a dial at all
        for (lengthType z = 0; z < dep; z++)
            for (lengthType i = 0; i < 16; i++) {
                const lengthType cxi = i % cols, cyi = i / cols;
                if (cyi >= rows) break;                // fewer cells than bands: draw what fits
                drawMeter(cv, cxi * cellW, cyi * cellH, cellW, cellH, z,
                          static_cast<uint8_t>(i), pos_[i] >> 8, peak_[i]);
            }
    }

private:
    /// The tiling: sixteen cells, as square as this panel allows. Picks the factor pair of 16
    /// whose cell aspect is closest to 1, so a square panel is 4x4 and a wide one 8x2.
    static void bestTiling(lengthType w, lengthType h, lengthType& cols, lengthType& rows) {
        int32_t bestNum = -1, bestDen = 1;
        for (lengthType c = 1; c <= 16; c++) {
            if (16 % c) continue;
            const lengthType r = 16 / c;
            const int32_t cw = w / c, ch = h / r;
            if (cw < 1 || ch < 1) continue;
            const int32_t lo = cw < ch ? cw : ch, hi = cw < ch ? ch : cw;
            // Compare lo/hi as a fraction, without floating point.
            if (bestNum < 0 || static_cast<int64_t>(lo) * bestDen > static_cast<int64_t>(bestNum) * hi) {
                bestNum = lo; bestDen = hi; cols = c; rows = r;
            }
        }
        if (bestNum < 0) { cols = 16; rows = 1; }
    }

    /// One meter inside its cell: a needle from a pivot at the bottom center, sweeping an arc that
    /// fits the cell, plus its peak marker and the red zone.
    void drawMeter(const draw::Canvas& cv, lengthType x0, lengthType y0, lengthType colW,
                   lengthType cellH, lengthType z, uint8_t band, int32_t reading, uint8_t peak) const {
        const lengthType px = x0 + colW / 2;                 // pivot, centered in its own cell
        const lengthType py = static_cast<lengthType>(y0 + cellH - 1);
        // The needle is as long as the cell is tall, and the SWEEP fits the cell's width: a dial
        // is as wide as it is tall when the cell is square, and narrows when it is not.
        const lengthType len = static_cast<lengthType>(cellH - 1 < 2 ? 2 : cellH - 1);
        // 0..255 onto a 120 degree sweep centered on straight up: 0 points up-LEFT, 128 straight
        // up, 255 up-RIGHT. With dx = sin(a) and dy = -cos(a), angle 0 is straight up, so the
        // sweep runs -60 to +60 degrees (10922 of 65536 is 60).
        // The half-sweep that keeps the tip inside the column: asin(halfWidth / len), and at most
        // 60 degrees so a wide column does not splay. Computed from the geometry rather than
        // fixed, so one meter on a wide panel sweeps a proper arc and sixteen on a narrow one
        // stay in their lanes.
        const int32_t halfW = colW / 2;
        int32_t half = len > 0 ? (10922 * halfW) / len : 10922;   // small-angle: proportional
        if (half > 10922) half = 10922;                            // never more than 60 degrees
        if (half < 1000) half = 1000;                              // and always a visible swing
        const auto angleFor = [half](int32_t v) -> angle16 {
            const int32_t clamped = v < 0 ? 0 : (v > 255 ? 255 : v);
            return static_cast<angle16>(static_cast<int32_t>(-half + (clamped * 2 * half) / 255));
        };
        const angle16 a = angleFor(reading);
        const lengthType nx = static_cast<lengthType>(px + (static_cast<int32_t>(sin16(a)) * len) / 32768);
        const lengthType ny = static_cast<lengthType>(py - (static_cast<int32_t>(cos16(a)) * len) / 32768);
        // The scale: a faint arc, red past three quarters.
        for (int32_t s = 0; s <= 255; s += 8) {
            const angle16 sa = angleFor(s);
            const lengthType sx = static_cast<lengthType>(px + (static_cast<int32_t>(sin16(sa)) * len) / 32768);
            const lengthType sy = static_cast<lengthType>(py - (static_cast<int32_t>(cos16(sa)) * len) / 32768);
            constexpr uint8_t base = 24;
            const RGB tick = s > 191 ? RGB{static_cast<uint8_t>(base + 40), 0, 0}
                                     : RGB{base, base, static_cast<uint8_t>(base + 8)};
            draw::pixel(cv, {sx, sy, z}, tick);
        }
        // The needle, in the band's palette color, and its peak marker above it.
        const RGB c = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(band * 16u), 255);
        draw::line(cv, {px, py, z}, {nx, ny, z}, c);
        if (peak > 4) {
            const angle16 pa = angleFor(peak);
            const lengthType mx = static_cast<lengthType>(px + (static_cast<int32_t>(sin16(pa)) * len) / 32768);
            const lengthType my = static_cast<lengthType>(py - (static_cast<int32_t>(cos16(pa)) * len) / 32768);
            draw::pixel(cv, {mx, my, z}, RGB{255, 255, 255});
        }
    }

    int32_t pos_[16] = {};      ///< needle position, 8.8 fixed point
    int32_t vel_[16] = {};      ///< needle velocity: the mass that makes it overshoot
    uint8_t peak_[16] = {};     ///< the peak-hold marker
    bool    started_ = false;
    uint32_t lastMs_ = 0;
};

}  // namespace mm
