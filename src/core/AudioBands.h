#pragma once

#include "core/AudioFrame.h"
#include "core/AudioLevel.h"   // magToByte: the shared log/dB mapping
#include "core/math16.h"       // ballistic: the per-band meter shape

#include <cmath>     // cosf/powf — band math is inherently float (so is the
                     // audioFft seam it feeds); the recognisable DSP choice.
#include <cstddef>
#include <cstdint>

namespace mm {

// Frequency analysis for one block of mic samples — the FFT *post-processing*,
// pure domain math with no platform header (the FFT kernel itself is the one
// seam, platform.h audioFft). Host-tested: feed a synthesized sine through
// applyWindow -> audioFft (the desktop naive DFT) -> magnitudesToBands and assert
// the dominant band + peak frequency, all in CI without an ESP32.
//
// Textbook real-signal spectrum analysis, nothing exotic:
//   - applyWindow: a Hann window — the standard general-purpose DSP window.
//     Tapering the block's edges to zero stops spectral leakage (a tone smearing
//     across many bins because the block isn't a whole number of cycles). Also
//     DC-strips the 24-in-32 samples to floats for the FFT.
//   - magnitudesToBands: groups the n/2 FFT magnitude bins into 16 log-spaced
//     bands (pitch is logarithmic — bass gets few bins, treble many) with a plain
//     geometric (equal-ratio) bin split, normalizes to 0..255, and picks the
//     single loudest bin as the dominant peak.

// Hann window coefficient at sample `i` of `n`: w(i) = 0.5 - 0.5*cos(2πi/(n-1)).
inline float hannWindow(size_t i, size_t n) {
    if (n <= 1) return 1.0f;
    const float x = 6.28318530718f * static_cast<float>(i) / static_cast<float>(n - 1);
    return 0.5f - 0.5f * std::cos(x);
}

// Window `n` samples (24-in-32, DC stripped) into `out` floats ready for the FFT.
// DC removal here mirrors AudioLevel's — a windowed-but-DC-biased block dumps all
// its energy into bin 0 and swamps the real peak.
inline void applyWindow(const int32_t* samples, size_t n, float* out) {
    if (!samples || !out || n == 0) return;
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += (samples[i] >> 8);
    const float mean = static_cast<float>(sum) / static_cast<float>(n);
    for (size_t i = 0; i < n; i++) {
        const float s = static_cast<float>(samples[i] >> 8) - mean;
        out[i] = s * hannWindow(i, n);
    }
}

// Group `nMag` FFT magnitudes (covering DC..Nyquist over `sampleRate`) into 16
// log-spaced bands (0..255 each) and report the dominant peak (`peakHz` = its
// frequency, `peakMag` = its 0..255 magnitude). Robust to nMag==0 (all zero).
//
// `noiseFloor` and `gain` condition the bands exactly like the level path
// (AudioLevel.h): each band's scaled magnitude has `noiseFloor` subtracted (so a
// quiet idle spectrum — the mic's own noise — gates to 0 instead of flickering
// the LEDs) and is then multiplied by `gain`/16 (16 = unity) for live brightness
// control. Same knobs, same meaning, both the level and the spectrum.
/// The 17 bin-index edges of the 16 bands, `edge[b]..edge[b+1]` per band.
///
/// A band is only a band if it OWNS bins. A pure geometric split (`edge[e] = nMag^(e/16)`, equal
/// frequency ratio per band, the textbook mapping of linear bins onto pitch) is scale-free, but the
/// FFT is not: below twice the bin width there are no distinct bins to hand out, so the low edges
/// collide. Measured at the shipped shape (256 bins of 43.1 Hz): bands 0 and 2 owned NO bins and
/// bands 1 and 4 owned one, while band 15 owned 75. A quarter of the display could not respond to
/// anything, and it was the quarter where music has its energy.
///
/// So the geometric curve is kept, and then made monotonic by construction: every band gets at least
/// one bin, taken from the wide top bands that have bins to spare. The result stays log-shaped where
/// the FFT can afford it and degrades to one-bin-per-band where it cannot, which is the honest
/// answer at the bottom of the range: no split can separate 60 Hz from 80 Hz when they share a bin.
///
/// Computed once per rate change, never per frame: 17 values, and the summing loop below costs the
/// same `nMag` additions wherever the edges sit.
/// The lowest frequency the spectrum shows. Below this is infrasound: mains hum, a microphone's DC
/// drift, footfall and traffic rumble, none of it audible and none of it music. At 22 kHz with a
/// 1024-bin FFT the first band would otherwise cover 11-22 Hz and display that rumble as bass.
inline constexpr float kLowestAudibleHz = 40.0f;

inline void audioBandEdges(size_t nMag, uint32_t sampleRate, size_t edge[17]) {
    if (nMag < 17) {                        // pathologically small: one bin each, as far as it goes
        for (uint8_t e = 0; e <= 16; e++) edge[e] = e < nMag ? e : nMag;
        return;
    }
    // The geometric ideal, in floating point so the collisions are visible before rounding.
    for (uint8_t e = 0; e <= 16; e++) {
        const float frac = static_cast<float>(e) / 16.0f;
        float ix = std::pow(static_cast<float>(nMag), frac);
        edge[e] = static_cast<size_t>(ix);
    }
    // Start at the lowest audible bin rather than at bin 1: bin 0 is DC and the bins just above it
    // are infrasound (see kLowestAudibleHz). Falls back to bin 1 when the rate is unknown.
    size_t firstBin = 1;
    if (sampleRate > 0) {
        const float binHz = static_cast<float>(sampleRate) / (2.0f * static_cast<float>(nMag));
        if (binHz > 0.0f) {
            firstBin = static_cast<size_t>(kLowestAudibleHz / binHz);
            if (firstBin < 1) firstBin = 1;
            if (firstBin > nMag / 2) firstBin = nMag / 2;   // never eat half the spectrum
        }
    }
    edge[0] = firstBin;
    edge[16] = nMag;
    // Forward pass: push each edge up so every band owns a bin. This is what the geometric split
    // could not do, and it costs the top bands a bin each, which they have in abundance.
    for (uint8_t e = 1; e <= 16; e++)
        if (edge[e] <= edge[e - 1]) edge[e] = edge[e - 1] + 1;
    // Backward pass: if the forward pass ran past the end (a small FFT), pull the edges back down.
    // Both passes together guarantee strictly increasing edges inside 1..nMag whenever nMag >= 17.
    edge[16] = nMag;
    for (uint8_t e = 16; e >= 1; e--)
        if (edge[e] <= edge[e - 1]) edge[e - 1] = edge[e] - 1;
}

/// The PPM ballistic over all 16 bands: `smoothed` follows `raw` fast on a rise and slowly on a
/// fall, each band on its own. One call per block, after whichever path produced the raw bands
/// (mic, simulation or a received sync packet), so every consumer sees the same meter.
///
/// `rise` and `fall` are `smoothFollow` rates. The defaults below are a broadcast meter's shape:
/// a hit arrives in one block, and a bar then takes about forty blocks (a second) to fall to zero,
/// long enough for the eye to read the peak and short enough not to smear the next one.
constexpr uint8_t kBandRise = 200;
constexpr uint8_t kBandFall = 24;
inline void smoothBands(const uint8_t raw[16], uint8_t smoothed[16],
                        uint8_t rise = kBandRise, uint8_t fall = kBandFall) {
    for (uint8_t b = 0; b < 16; b++) smoothed[b] = ballistic(smoothed[b], raw[b], rise, fall);
}

/// Spectral flux: how much the spectrum ROSE since the last block, 0..255. The standard onset
/// detection function (Bello et al. 2005, Dixon 2006): sum the positive per-band differences and
/// ignore the falls, so a hit reads high, a decay reads zero and a held tone reads zero. Sixteen
/// subtractions on bands already computed, so it costs nothing and lands with the block.
inline uint8_t spectralFlux(const uint8_t prev[16], const uint8_t cur[16]) {
    uint32_t sum = 0;
    for (uint8_t b = 0; b < 16; b++) if (cur[b] > prev[b]) sum += static_cast<uint32_t>(cur[b] - prev[b]);
    sum /= 16;                                    // sixteen bands of 255 fold back onto 0..255
    return static_cast<uint8_t>(sum > 255 ? 255 : sum);
}

/// Turns a flux stream into onsets: one per hit, none for a swell. The decision is the textbook
/// one, flux against its own recent MEAN (Dixon 2006) rather than an absolute threshold, so a loud
/// room and a quiet one fire on the same kind of event; a refractory window then makes one hit one
/// onset however many blocks it spans. The mean is an EMA in 8.8 fixed point, advanced AFTER the
/// decision so a hit does not raise the bar it is being judged against.
struct OnsetDetector {
    uint16_t mean_ = 0;            ///< EMA of the flux, 8.8 fixed point
    uint32_t lastMs_ = 0;          ///< when the last onset fired
    bool     fired_ = false;       ///< whether one has fired yet (lastMs_ of 0 is a valid time)

    /// Feed one block's flux at time `nowMs`. Returns true on the block an onset is detected.
    /// A hit is flux above `num/den` of the mean plus `margin`; `refractoryMs` is the minimum gap.
    bool feed(uint8_t flux, uint32_t nowMs, uint8_t num = 3, uint8_t den = 2,
              uint8_t margin = 20, uint16_t refractoryMs = 100) {
        const uint32_t meanNow = mean_ >> 8;
        const bool above = flux > (meanNow * num) / den + margin;
        mean_ = static_cast<uint16_t>(mean_ - (mean_ >> 4) + (static_cast<uint16_t>(flux) << 4));
        if (above && (!fired_ || nowMs - lastMs_ >= refractoryMs)) {
            lastMs_ = nowMs;
            fired_ = true;
            return true;
        }
        return false;
    }
};

/// Per-band conditioning in the dB domain: the tier of the audio roadmap's design that learns the
/// RIG rather than chasing the music.
///
/// Every band carries two learned numbers. Its **floor**, the level it reads with no program
/// material (mic self-noise, mains hum, the room), followed as a running MINIMUM that drifts up
/// slowly so a room that gets noisier is re-learned. Its **peak**, followed with an instant attack
/// and a release of seconds, so it settles to the band's typical loudest level rather than
/// tracking every beat. Between the two is the band's own dynamic range, and the correction maps
/// that range onto the display window instead of leaving the treble at a fourteenth of the bass
/// under spectrally balanced material (measured: a peak-per-band reading of pink noise falls as
/// 1/sqrt(f) across the sixteen bands).
///
/// The correction is applied with a compressor's `ratio`: N:1 removes (1 - 1/N) of a band's
/// deviation from the window, so 1:1 is off and the music's balance is untouched, 2:1 halves the
/// rig's coloration, and a high ratio flattens it. Slow on purpose: a fast per-band gain is what
/// causes cross-spectral pumping, and every source on multiband dynamics says not to (the audio
/// roadmap, § two tiers). `maxGain` caps the lift so a silent band is never amplified into its own
/// noise. `learning` off freezes both tables, which is the deterministic mode a show wants.
///
/// State is 32 floats; the work is 16 logs and a handful of multiplies per block, on the audio
/// block path and never per light.
struct BandConditioner {
    /// The narrowest dynamic range a band is credited with: the shared follower minimum, so the
    /// band and level paths cannot drift apart (kConditionerMinRangeDb, AudioLevel.h).
    static constexpr float kMinRangeDb = kConditionerMinRangeDb;

    float floorDb[16];
    float peakDb[16];
    bool  primed = false;

    BandConditioner() { for (uint8_t b = 0; b < 16; b++) { floorDb[b] = 0.0f; peakDb[b] = 0.0f; } }

    /// Condition one block. `db` in, `out` the corrected dB for the display window
    /// [windowFloor, windowFloor + windowSpan]. `dtMs` is the block interval, for the time
    /// constants. `ratioN` is the N of N:1 (1 = off). `learning` false freezes the tables.
    /// `gateDb` is the silence threshold: a band below it carries no program material, so it
    /// reads zero and is NOT learned from. Both halves matter. Without the gate the lift is
    /// dominated by `windowFloor - db`, which relocates a silent band up into the window as
    /// eagerly as a quiet instrument, and an empty room is displayed at full scale (measured on a
    /// Dig-Next-2: the raw path read flux 0-3 while the learner made 33-68 of it). And learning
    /// from silence drags the floor table down to the noise, so the next wobble reads as music.
    void process(const float db[16], float out[16], uint32_t dtMs, float windowFloor,
                 float windowSpan, uint8_t ratioN, float maxGainDb, bool learning, float gateDb,
                 float floorRiseDbPerS = 1.0f, float peakReleaseDbPerS = 3.0f) {
        if (!primed) {
            for (uint8_t b = 0; b < 16; b++) {
                floorDb[b] = db[b];
                peakDb[b] = db[b] + kMinRangeDb;
            }
            primed = true;
        }
        const float dt = static_cast<float>(dtMs) / 1000.0f;
        const float amount = ratioN <= 1 ? 0.0f : 1.0f - 1.0f / static_cast<float>(ratioN);
        for (uint8_t b = 0; b < 16; b++) {
            // Silence: nothing to show and nothing to learn. Held before the followers so the
            // tables keep describing the music rather than the room's noise floor.
            if (db[b] < gateDb) { out[b] = 0.0f; continue; }
            if (learning) {
                // Floor: a minimum follower that drifts UP slowly, so it forgets a quiet moment
                // over seconds but takes a new low at once.
                floorDb[b] = db[b] < floorDb[b] ? db[b] : floorDb[b] + floorRiseDbPerS * dt;
                // Peak: instant attack, slow release, so it settles on the band's typical top.
                peakDb[b] = db[b] > peakDb[b] ? db[b] : peakDb[b] - peakReleaseDbPerS * dt;
                // Bound the stretch so a collapsed follower cannot divide by ~zero. Small on
                // purpose: the silence gate above is what keeps a quiet room quiet, and a large
                // minimum here would flatten real music instead (see kConditionerMinRangeDb).
                if (peakDb[b] < floorDb[b] + kMinRangeDb) peakDb[b] = floorDb[b] + kMinRangeDb;
            }
            // Where this band's own range would put the value inside the window, and how far
            // from the raw value that is; `amount` decides how much of that move is taken.
            const float range = peakDb[b] - floorDb[b];
            const float normalized = windowFloor + (db[b] - floorDb[b]) * (windowSpan / range);
            float shift = (normalized - db[b]) * amount;
            if (shift > maxGainDb) shift = maxGainDb;          // never lift a band into its noise
            out[b] = db[b] + shift;
        }
    }
};

inline void magnitudesToBands(const float* mag, size_t nMag, uint32_t sampleRate,
                              uint16_t noiseFloor, uint16_t gain,
                              uint8_t bands[16], uint16_t& peakHz, uint16_t& peakMag,
                              BandConditioner* cond = nullptr, uint32_t dtMs = 23,
                              uint8_t ratioN = 1, float maxGainDb = 24.0f, bool learning = true) {
    for (uint8_t b = 0; b < 16; b++) bands[b] = 0;
    peakHz = 0;
    peakMag = 0;
    if (!mag || nMag == 0 || sampleRate == 0) return;

    // Hz per bin = sampleRate / (2 * nMag).
    const float binHz = static_cast<float>(sampleRate) / (2.0f * static_cast<float>(nMag));

    size_t edge[17];
    audioBandEdges(nMag, sampleRate, edge);
    float bandDb[16];

    // Magnitude → 0..255 on the shared LOGARITHMIC (dB) scale (magToByte, in
    // AudioLevel.h) — the same mapping the level/VU path uses, so noiseFloor/gain
    // mean one thing across both. These are the generic per-display knobs (not
    // per-band) — the recognisable "range + sensitivity" pair an analyser exposes.
    auto toByte = [noiseFloor, gain](float m) -> uint8_t {
        return magToByte(m, noiseFloor, gain);
    };

    float peakVal = 0.0f;
    size_t peakBin = 0;
    for (size_t i = 1; i < nMag; i++)              // single peak scan (skip DC)
        if (mag[i] > peakVal) { peakVal = mag[i]; peakBin = i; }

    for (uint8_t b = 0; b < 16; b++) {
        size_t lo = edge[b], hi = edge[b + 1];
        if (hi <= lo) hi = lo + 1;
        if (hi > nMag) hi = nMag;
        // Peak (not average) magnitude in the band: a narrow tone shouldn't be
        // diluted by the empty bins of a wide treble band — this is what makes a
        // single tone light ONE band instead of smearing across many.
        float best = 0.0f;
        for (size_t i = lo; i < hi; i++) if (mag[i] > best) best = mag[i];
        bandDb[b] = best <= 1.0f ? 0.0f : 20.0f * std::log10(best);
    }
    // Per-band conditioning in dB, then the shared window onto bytes. Without a conditioner this
    // is exactly magToByte per band, so the two paths cannot disagree.
    if (cond) {
        float out[16];
        cond->process(bandDb, out, dtMs, windowFloorDb(noiseFloor), windowSpanDb(gain), ratioN,
                      maxGainDb, learning, windowFloorDb(noiseFloor));
        for (uint8_t b = 0; b < 16; b++) bands[b] = bandDb[b] <= 0.0f ? 0 : dbToByte(out[b], noiseFloor, gain);
    } else {
        for (uint8_t b = 0; b < 16; b++) bands[b] = bandDb[b] <= 0.0f ? 0 : dbToByte(bandDb[b], noiseFloor, gain);
    }

    if (peakVal > 0.0f) {
        peakHz = static_cast<uint16_t>(static_cast<float>(peakBin) * binHz);
        peakMag = toByte(peakVal);
    }
}

} // namespace mm
