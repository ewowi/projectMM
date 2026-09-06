// @module AudioService
// @also AudioSpectrumEffect

#include "doctest.h"
#include "core/AudioBands.h"
#include "platform/platform.h"   // platform::audioFft (desktop naive DFT)

#include <cmath>
#include <cstring>   // std::memcpy: clang finds it transitively, GCC does not (CI's sanitizer builds)
#include <numbers>
#include <vector>

// The success spec for the frequency path, written RED before AudioService's FFT
// call exists. The whole pipeline runs host-side: synthesize a sine ->
// applyWindow -> platform::audioFft (the desktop reference DFT) ->
// magnitudesToBands, then assert the energy lands in the right band and the
// reported peak frequency tracks the tone. This is the coverage that lets the
// band-map tuning happen in CI instead of on the bench over months.

namespace {

constexpr size_t kN = 256;            // FFT size (power of two)
constexpr uint32_t kRate = 22050;     // default sample rate

// Build kN samples of a sine at `freqHz`, 24-bit amplitude, in the int32 slot.
std::vector<int32_t> tone(double freqHz, double amp24 = (1 << 21)) {
    constexpr double kPi = std::numbers::pi_v<double>;
    std::vector<int32_t> v(kN);
    const double cycles = freqHz * kN / kRate;
    for (size_t i = 0; i < kN; i++) {
        const double s = amp24 * std::sin(2.0 * kPi * cycles * static_cast<double>(i) / kN);
        const int64_t sample = static_cast<int64_t>(s) << 8;   // avoid signed <<8 UB
        v[i] = static_cast<int32_t>(sample);
    }
    return v;
}

// Run the full window -> FFT -> bands pipeline; return the frame fields of
// interest. noiseFloor=80 / gain=80 set a dB window (~100 dB floor, ~40 dB span)
// that brackets the test tones' ~125 dB level, so a dominant band stands clearly
// above the others instead of every band saturating at the top of the window.
void analyse(const std::vector<int32_t>& samples, uint8_t (&bands)[16],
             uint16_t& peakHz, uint16_t& peakMag) {
    std::vector<float> windowed(kN), mag(kN / 2);
    mm::applyWindow(samples.data(), kN, windowed.data());
    mm::platform::audioFft(windowed.data(), kN, mag.data());
    mm::magnitudesToBands(mag.data(), kN / 2, kRate, /*noiseFloor*/80, /*gain*/80, bands, peakHz, peakMag);
}

// Index of the loudest band.
int dominantBand(const uint8_t (&bands)[16]) {
    int best = 0;
    for (int b = 1; b < 16; b++) if (bands[b] > bands[best]) best = b;
    return best;
}

} // namespace

TEST_CASE("AudioBands: silence yields all-zero bands and no peak") {
    std::vector<int32_t> s(kN, 0);
    uint8_t bands[16];
    uint16_t peakHz = 9999, peakMag = 9999;
    analyse(s, bands, peakHz, peakMag);
    for (int b = 0; b < 16; b++) CHECK(bands[b] == 0);
    CHECK(peakHz == 0);
    CHECK(peakMag == 0);
}

TEST_CASE("AudioBands: a low tone lands in a low band, a high tone in a high band") {
    uint8_t lo[16], hi[16];
    uint16_t hz, mag;
    analyse(tone(500.0), lo, hz, mag);     // bass
    analyse(tone(8000.0), hi, hz, mag);    // treble (under the ~11 kHz Nyquist)
    const int loBand = dominantBand(lo);
    const int hiBand = dominantBand(hi);
    CHECK(hiBand > loBand);                // higher frequency → higher band index
}

TEST_CASE("AudioBands: the reported peak frequency tracks the played tone") {
    for (double f : {1000.0, 3000.0, 6000.0}) {
        uint8_t bands[16];
        uint16_t peakHz, peakMag;
        analyse(tone(f), bands, peakHz, peakMag);
        REQUIRE(peakMag > 0);
        // Within one FFT bin (rate/N ≈ 86 Hz) of the true tone.
        const double binHz = static_cast<double>(kRate) / kN;
        CHECK(std::abs(static_cast<double>(peakHz) - f) <= binHz * 1.5);
    }
}

TEST_CASE("AudioBands: a single tone concentrates energy, not smears it everywhere") {
    uint8_t bands[16];
    uint16_t peakHz, peakMag;
    analyse(tone(2000.0), bands, peakHz, peakMag);
    const int dom = dominantBand(bands);
    // The dominant band is clearly above the average — the window/FFT isolated it.
    int sum = 0;
    for (int b = 0; b < 16; b++) sum += bands[b];
    const int avg = sum / 16;
    CHECK(bands[dom] > avg * 2);
}

TEST_CASE("AudioBands: noiseFloor gates a low idle spectrum to zero, gain scales it back") {
    // A weak tone that lands a small but nonzero band magnitude at unity gain.
    auto quiet = tone(2000.0, 1 << 12);   // small amplitude
    std::vector<float> windowed(kN), mag(kN / 2);
    mm::applyWindow(quiet.data(), kN, windowed.data());
    mm::platform::audioFft(windowed.data(), kN, mag.data());

    uint8_t open[16], gated[16];
    uint16_t hz, pm;
    mm::magnitudesToBands(mag.data(), kN / 2, kRate, /*noiseFloor*/0, 16, open, hz, pm);
    int openMax = 0;
    for (int b = 0; b < 16; b++) if (open[b] > openMax) openMax = open[b];
    REQUIRE(openMax > 0);   // something lights with the gate open

    // A noiseFloor above that level zeroes every band — the fix for idle flicker.
    mm::magnitudesToBands(mag.data(), kN / 2, kRate,
                          static_cast<uint16_t>(openMax + 50), 16, gated, hz, pm);
    for (int b = 0; b < 16; b++) CHECK(gated[b] == 0);
}

TEST_CASE("AudioBands: zero / degenerate input never crashes") {
    uint8_t bands[16];
    uint16_t peakHz, peakMag;
    mm::magnitudesToBands(nullptr, 8, kRate, 0, 16, bands, peakHz, peakMag);
    CHECK(peakMag == 0);
    float mag1 = 1.0f;
    mm::magnitudesToBands(&mag1, 0, kRate, 0, 16, bands, peakHz, peakMag);   // nMag 0
    CHECK(peakMag == 0);
    mm::magnitudesToBands(&mag1, 1, 0, 0, 16, bands, peakHz, peakMag);       // rate 0
    CHECK(peakMag == 0);

    // applyWindow on degenerate sizes is a no-op, not a crash.
    float out1 = 123.0f;
    int32_t s = 7 << 8;
    mm::applyWindow(&s, 0, &out1);
    mm::applyWindow(nullptr, 4, &out1);
    CHECK(true);
}

// The band SPLIT itself, rather than what lands in a band. A 16-band display is only 16 bands if
// every band owns bins of its own: a band whose edges collapse onto the same bin index can never
// light, whatever the signal, and a band with one bin reads a sixteenth of what a band with 75 does
// under the same energy. Both were true of the geometric split (edge[e] = nMag^(e/16)) at the
// shipped shape, which is what these pin.

TEST_CASE("every band owns at least one FFT bin, so no band is dark whatever the music") {
    // The shipped shape: 512-sample FFT at 22050 Hz, so 256 bins of 43.1 Hz. The geometric split
    // gave bands 0 and 2 no bins at all (edges 1-1 and 2-2) and bands 1 and 4 a single bin, which
    // is a quarter of the display that cannot respond.
    const size_t nMag = 256;
    const uint32_t sampleRate = 22050;
    size_t edges[17];
    mm::audioBandEdges(nMag, sampleRate, edges);
    for (uint8_t b = 0; b < 16; b++) {
        INFO("band ", b, " spans bins ", edges[b], "..", edges[b + 1]);
        CHECK(edges[b + 1] > edges[b]);
    }
    CHECK(edges[0] >= 1);            // bin 0 is DC, never part of a band
    CHECK(edges[16] == nMag);        // and the top band reaches the Nyquist end
}

TEST_CASE("band edges rise with frequency, so a band is a range rather than a reshuffle") {
    const size_t nMag = 256;
    size_t edges[17];
    mm::audioBandEdges(nMag, 22050, edges);
    for (uint8_t e = 1; e <= 16; e++) CHECK(edges[e] > edges[e - 1]);
}

TEST_CASE("a small FFT still yields sixteen usable bands, because a fixture may run one") {
    // 128 bins is the smallest shape worth supporting; a geometric split there is degenerate over
    // half its range. Sixteen bands must still each own a bin.
    const size_t nMag = 128;
    size_t edges[17];
    mm::audioBandEdges(nMag, 22050, edges);
    for (uint8_t b = 0; b < 16; b++) {
        INFO("band ", b, " spans bins ", edges[b], "..", edges[b + 1]);
        CHECK(edges[b + 1] > edges[b]);
    }
}

TEST_CASE("the low bands keep the resolution the FFT can actually deliver") {
    // Above the bin width the split is free to place edges anywhere; below it there is nothing to
    // place. The lowest band starts at the first non-DC bin and the early bands stay narrow, so the
    // bass keeps what resolution exists rather than being folded into one wide band.
    const size_t nMag = 256;
    const uint32_t sampleRate = 22050;
    size_t edges[17];
    mm::audioBandEdges(nMag, sampleRate, edges);
    const float binHz = static_cast<float>(sampleRate) / (2.0f * static_cast<float>(nMag));
    CHECK(edges[0] == 1);                                  // starts just above DC
    CHECK(edges[4] * binHz < 400.0f);                      // four bands inside the bass
    CHECK(edges[8] * binHz < 2000.0f);                     // half the display below 2 kHz
}

// The BALLISTIC of a band. A meter that rises and falls at the same speed is the wrong instrument:
// it makes the attack as sluggish as the decay and rounds off exactly the drum hit an audio effect
// exists to show. Broadcast meters (PPM, IEC 60268-10) rise fast and fall slowly, and WLED, FastLED
// and LedFx each arrived at the same asymmetric form independently.

TEST_CASE("a band rises to a transient at once and falls back slowly, the PPM ballistic") {
    uint8_t v = 0;
    // A hit: one block takes it most of the way up, because a drum must not be smoothed away.
    v = mm::ballistic(v, 200, /*rise*/ 200, /*fall*/ 24);
    CHECK(v > 150);
    const uint8_t afterRise = v;
    // Silence after it: the fall is gradual, so the bar decays rather than dropping out.
    v = mm::ballistic(v, 0, 200, 24);
    CHECK(v < afterRise);
    CHECK(v > (afterRise * 3) / 4);    // one block takes off a tenth: the bar decays, it does not drop
    for (int i = 0; i < 60; i++) v = mm::ballistic(v, 0, 200, 24);
    CHECK(v == 0);                     // and it does reach zero rather than sticking just above it
}

TEST_CASE("the ballistic reaches its target exactly, so a held level does not sit one short") {
    uint8_t v = 0;
    for (int i = 0; i < 60; i++) v = mm::ballistic(v, 255, 200, 24);
    CHECK(v == 255);
    for (int i = 0; i < 200; i++) v = mm::ballistic(v, 0, 200, 24);
    CHECK(v == 0);
}

TEST_CASE("equal rise and fall reduce to a symmetric follower, so the ballistic is a superset") {
    for (uint8_t rate : {uint8_t(8), uint8_t(64), uint8_t(200)}) {
        uint8_t a = 40, b = 40;
        for (int i = 0; i < 10; i++) {
            a = mm::ballistic(a, 200, rate, rate);
            b = mm::smoothFollow(b, 200, rate);
            CHECK(a == b);
        }
    }
}

TEST_CASE("every band gets its own ballistic, so a hit in the bass does not smooth the treble") {
    uint8_t raw[16] = {}, sm[16] = {};
    raw[0] = 255;                                  // a bass hit, treble silent
    mm::smoothBands(raw, sm);
    CHECK(sm[0] > 150);                            // the hit shows in one block
    for (uint8_t b = 1; b < 16; b++) CHECK(sm[b] == 0);   // and touches no other band
    raw[0] = 0;                                    // silence
    mm::smoothBands(raw, sm);
    CHECK(sm[0] > 100);                            // the bar is still falling, not gone
    CHECK(sm[0] < 200);
}

// Onset detection. The standard onset detection function is SPECTRAL FLUX (Bello 2005, Dixon
// 2006): the sum over bands of the positive change since the last block. A rise across the
// spectrum is a hit; a fall is not, and a steady tone is not. It is 16 subtractions on bands we
// already have, so it costs nothing and lands with the block's own latency.

TEST_CASE("spectral flux reads a rise, ignores a fall, and is zero on a steady spectrum") {
    uint8_t prev[16] = {}, cur[16] = {};
    CHECK(mm::spectralFlux(prev, cur) == 0);               // silence to silence
    for (uint8_t b = 0; b < 16; b++) cur[b] = 200;
    const uint8_t hit = mm::spectralFlux(prev, cur);        // everything rose
    CHECK(hit > 150);
    std::memcpy(prev, cur, 16);
    CHECK(mm::spectralFlux(prev, cur) == 0);               // held: no flux
    for (uint8_t b = 0; b < 16; b++) cur[b] = 0;
    CHECK(mm::spectralFlux(prev, cur) == 0);               // a fall is not an onset
}

TEST_CASE("an onset fires once per hit, not once per block the hit lasts, and not on a swell") {
    // A hit is flux well above its own recent average; a refractory window makes one hit one
    // onset. A slow swell raises the average with it and never exceeds it enough to fire.
    mm::OnsetDetector d;
    int onsets = 0;
    for (int block = 0; block < 200; block++) {
        const bool hitBlock = (block % 20 == 0);            // a hit every 20 blocks (~half a second)
        const uint8_t flux = hitBlock ? 200 : 5;            // background flux between hits
        if (d.feed(flux, static_cast<uint32_t>(block) * 23u)) onsets++;
    }
    CHECK(onsets == 10);                                    // ten hits, ten onsets
    mm::OnsetDetector s;
    int swell = 0;
    for (int block = 0; block < 200; block++)               // a slow linear swell
        if (s.feed(static_cast<uint8_t>(block / 2), static_cast<uint32_t>(block) * 23u)) swell++;
    CHECK(swell <= 1);                                      // the first block may fire; nothing after
}

// Per-band conditioning: the learner. Each band learns its own floor and peak in dB; `ratio`
// decides how much of the rig's coloration is removed. Fed directly with dB so the tests say
// what they mean.

namespace {
void feedBlocks(mm::BandConditioner& c, const float db[16], float out[16], int blocks, uint8_t ratio,
                float maxGain = 24.0f, bool learning = true) {
    // gate 0: these cases test the conditioner's mapping, so nothing is gated as silence. The
    // gate has its own case below.
    for (int i = 0; i < blocks; i++) c.process(db, out, 23, 60.0f, 40.0f, ratio, maxGain, learning, 0.0f);
}
}

TEST_CASE("at ratio 1:1 the conditioner changes nothing, so the music's own balance is untouched") {
    mm::BandConditioner c; float db[16], out[16];
    for (uint8_t b = 0; b < 16; b++) db[b] = 80.0f - b * 1.5f;
    feedBlocks(c, db, out, 100, 1);
    for (uint8_t b = 0; b < 16; b++) CHECK(out[b] == doctest::Approx(db[b]));
}

TEST_CASE("a spectrally tilted rig reads flat at a high ratio, once the learner has settled") {
    // Pink noise through a peak-per-band reading tilts 1/sqrt(f): the treble far below the bass.
    // Each band has the same DYNAMICS (a 20 dB swing), only its level differs. After settling,
    // the conditioned tops line up within a couple of dB, so a balanced signal shows as balanced.
    mm::BandConditioner c; float lo[16], hi[16], out[16];
    for (uint8_t b = 0; b < 16; b++) { hi[b] = 90.0f - b * 1.5f; lo[b] = hi[b] - 20.0f; }
    // The cap is not under test here (it has its own case below), so it sits above the tilt.
    for (int i = 0; i < 400; i++) { feedBlocks(c, hi, out, 1, 20, 40.0f); feedBlocks(c, lo, out, 1, 20, 40.0f); }
    feedBlocks(c, hi, out, 1, 20, 40.0f);
    float mn = 1e9f, mx = -1e9f;
    for (uint8_t b = 0; b < 16; b++) { if (out[b] < mn) mn = out[b]; if (out[b] > mx) mx = out[b]; }
    CHECK(mx - mn < 2.0f);
    CHECK(out[15] == doctest::Approx(100.0f).epsilon(0.03));   // the top lands at the window's top
}

TEST_CASE("each band learns its own floor, so a hum in one band does not raise the others") {
    mm::BandConditioner c; float silence[16], out[16];
    for (uint8_t b = 0; b < 16; b++) silence[b] = 30.0f;
    silence[1] = 55.0f;                                        // mains hum in band 1
    feedBlocks(c, silence, out, 200, 20);
    CHECK(c.floorDb[1] == doctest::Approx(55.0f).epsilon(0.05));
    CHECK(c.floorDb[0] == doctest::Approx(30.0f).epsilon(0.05));
    CHECK(c.floorDb[8] == doctest::Approx(30.0f).epsilon(0.05));
}

TEST_CASE("maxGain caps the lift, so a silent band is never amplified into its own noise") {
    mm::BandConditioner c; float db[16], out[16];
    for (uint8_t b = 0; b < 16; b++) db[b] = 80.0f;
    db[15] = 40.0f;                                            // one band forty dB down
    feedBlocks(c, db, out, 300, 20, /*maxGain*/ 6.0f);
    CHECK(out[15] <= 40.0f + 6.0f + 0.01f);
}

TEST_CASE("learning off freezes the tables, the deterministic mode a show wants") {
    mm::BandConditioner c; float a[16], b2[16], out[16];
    for (uint8_t b = 0; b < 16; b++) { a[b] = 70.0f; b2[b] = 90.0f; }
    feedBlocks(c, a, out, 100, 20);
    const float peakBefore = c.peakDb[3], floorBefore = c.floorDb[3];
    feedBlocks(c, b2, out, 100, 20, 24.0f, /*learning*/ false);
    CHECK(c.peakDb[3] == peakBefore);
    CHECK(c.floorDb[3] == floorBefore);
}

TEST_CASE("the peak releases over seconds, not blocks, so one loud bar does not re-level the display") {
    mm::BandConditioner c; float loud[16], quiet[16], out[16];
    for (uint8_t b = 0; b < 16; b++) { loud[b] = 90.0f; quiet[b] = 60.0f; }
    feedBlocks(c, loud, out, 10, 20);
    feedBlocks(c, quiet, out, 10, 20);                          // a quarter of a second later
    CHECK(c.peakDb[0] > 85.0f);                                // still remembers the loud bar
    feedBlocks(c, quiet, out, 400, 20);                         // ten seconds later
    // It has let go: the peak sits at the band's floor plus the minimum range, rather than
    // anywhere near the loud bar it was holding.
    // Converging on the minimum range: the peak falls while the floor drifts up to meet it.
    CHECK(c.peakDb[0] - c.floorDb[0] < mm::BandConditioner::kMinRangeDb + 2.0f);
}

// A quiet passage is not silence, and must keep its dynamics. The range clamp used to be the
// anti-noise mechanism and was set high enough (12 dB) to squash real music: a band swinging 6 dB
// filled only half the display, which reads as vivid bands with no dynamic range. The silence gate
// took that job over, so a band with real swing now uses the whole window.
TEST_CASE("a quietly played band still fills the display, so soft passages keep their dynamics") {
    mm::BandConditioner c; float soft[16], loud[16], out[16];
    for (uint8_t b = 0; b < 16; b++) { soft[b] = 70.0f; loud[b] = 76.0f; }   // a 6 dB swing

    // Settle on that swing, above the gate throughout: this is music, not a silent room.
    for (int i = 0; i < 200; i++) {
        c.process(soft, out, 23, 60.0f, 40.0f, 20, 40.0f, true, 65.0f);
        c.process(loud, out, 23, 60.0f, 40.0f, 20, 40.0f, true, 65.0f);
    }
    const float atLoud = out[0];
    c.process(soft, out, 23, 60.0f, 40.0f, 20, 40.0f, true, 65.0f);
    const float atSoft = out[0];

    // The 6 dB swing is stretched across most of the 40 dB window, not left as 6 dB of it.
    CHECK(atLoud - atSoft > 20.0f);
}

// The level path levels itself in automatic mode, the other half of the one `levels` decision:
// the learner measures the VU's window the way it measures each band's, so the manual floor/gain
// sliders are genuinely manual-only rather than still shaping the picture from behind a hidden row.
TEST_CASE("in automatic mode a quiet room and a loud one both fill the level meter") {
    const size_t n = 512;
    int32_t quiet[n], loud[n];
    for (size_t i = 0; i < n; i++) {
        const float ph = static_cast<float>(i) * 0.1f;
        // Both above the silence gate (60 dB at floor 0), a hundred times apart: the test is
        // that each fills its OWN window, not that one of them is silent.
        quiet[i] = static_cast<int32_t>(std::sin(ph) * 20000000.0f);     // a quiet room
        loud[i]  = static_cast<int32_t>(std::sin(ph) * 2000000000.0f);   // a hundred times louder
    }

    // Music, not a test tone: the level has to VARY for a learned window to mean anything, so
    // each room alternates a soft passage with a loud one. A steady tone correctly reads zero
    // once the floor follower catches up to it, which is what "nothing is changing" looks like.
    int32_t quietSoft[n], loudSoft[n];
    for (size_t i = 0; i < n; i++) { quietSoft[i] = quiet[i] / 2; loudSoft[i] = loud[i] / 2; }

    mm::AudioFrame f{};
    mm::LevelConditioner a, b;
    for (int i = 0; i < 100; i++) {
        mm::computeLevel(quietSoft, n, 0, 128, f, &a, 23);
        mm::computeLevel(quiet, n, 0, 128, f, &a, 23);
    }
    const uint16_t quietLevel = f.level;
    for (int i = 0; i < 100; i++) {
        mm::computeLevel(loudSoft, n, 0, 128, f, &b, 23);
        mm::computeLevel(loud, n, 0, 128, f, &b, 23);
    }
    const uint16_t loudLevel = f.level;

    // The two rooms read the SAME, though one is a hundred times louder: each is mapped onto its
    // own learned window, which is the whole point of levelling the VU automatically. Both sit
    // above the silence gate (floor 0 here), so this measures the levelling, not the gate.
    CHECK(quietLevel > 0);
    CHECK(quietLevel == loudLevel);

    // And manual mode still maps absolutely: the loud room reads higher than the quiet one.
    mm::computeLevel(quiet, n, 50, 128, f, nullptr, 23);
    const uint16_t quietManual = f.level;
    mm::computeLevel(loud, n, 50, 128, f, nullptr, 23);
    CHECK(f.level > quietManual);
}

// The silence gate, the fix for a learner that levelled an empty room up to full scale. Measured on
// a Dig-Next-2: the raw path read flux 0-3 in a quiet room while the conditioner made 33-68 of it,
// because the lift is dominated by relocating a quiet band up into the display window and silence
// was relocated as eagerly as music.
TEST_CASE("a room below the floor reads silent, however hard the learner is asked to level") {
    mm::BandConditioner c; float quiet[16], out[16];
    for (uint8_t b = 0; b < 16; b++) quiet[b] = 55.0f;          // below a gate of 60

    // Settle, then ask for the most aggressive levelling available.
    for (int i = 0; i < 400; i++) c.process(quiet, out, 23, 60.0f, 40.0f, 20, 40.0f, true, 60.0f);
    for (uint8_t b = 0; b < 16; b++) CHECK(out[b] == 0.0f);

    // And the tables were not dragged down to the room's noise: real music still reads.
    float music[16];
    for (uint8_t b = 0; b < 16; b++) music[b] = 80.0f;
    c.process(music, out, 23, 60.0f, 40.0f, 20, 40.0f, true, 60.0f);
    for (uint8_t b = 0; b < 16; b++) CHECK(out[b] > 0.0f);
}

// Flux is a difference against the PREVIOUS block and the onset detector carries a running mean,
// so a source that stops and starts must not measure its first new block against the last block of
// the old one: that reports a hit nobody played. AudioService::deinit clears both with the frame.
TEST_CASE("a restarted source reports no onset from the block that preceded it") {
    // The history the old source left behind: a loud spectrum.
    uint8_t prev[16], now[16];
    for (uint8_t b = 0; b < 16; b++) { prev[b] = 200; now[b] = 200; }
    CHECK(mm::spectralFlux(prev, now) == 0);            // steady: no flux, by definition

    // Cleared history (what deinit leaves) reads the same block as a full-scale RISE, which is why
    // deinit also publishes a silent frame: the first block after a restart is what that silences.
    // What clearing buys is a DEFINED reference for the block after it, rather than a spectrum the
    // old source left behind.
    uint8_t cleared[16] = {};
    const uint16_t againstStale = mm::spectralFlux(prev, now);
    const uint16_t againstCleared = mm::spectralFlux(cleared, now);
    CHECK(againstStale == 0);
    CHECK(againstCleared > againstStale);   // which is why the frame is published silent too
}

// The gate that ships, exercised through magnitudesToBands rather than process() directly: every
// conditioner test above hands `process` a hand-picked gateDb, so none covers the value the caller
// actually passes. It sits AT the display window's floor, deliberately: a band reports its bins'
// PEAK while the level path reports an RMS, so the level's 20 dB silence margin is a far larger
// concession here. Measured on a Dig-Next-2, a 20 dB margin took a quiet room from flux 1-2 to
// 49-102 with onsets firing.
TEST_CASE("a room below the display window shows nothing on the spectrum") {
    const size_t nMag = 256;
    const uint32_t rate = 22050;
    float room[nMag];
    // 100 dB: below the window floor (110 dB at `floor` 100), above where a 20 dB margin would sit.
    for (size_t i = 0; i < nMag; i++) room[i] = 100000.0f;

    mm::BandConditioner cond;
    uint8_t bands[16]; uint16_t peakHz = 0, peakMag = 0;
    for (int i = 0; i < 20; i++)
        mm::magnitudesToBands(room, nMag, rate, /*noiseFloor*/ 100, /*gain*/ 128,
                              bands, peakHz, peakMag, &cond, 23, 4, 24.0f, true);

    for (uint8_t b = 0; b < 16; b++) CHECK(bands[b] == 0);
}
