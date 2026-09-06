// @module oscillators
// @also math16

// The oscillator bank: the animated quantities of a generative field, advanced once per frame and
// read per pixel. These pin what an effect author relies on: that a value stays inside the range
// they asked for, that two oscillators sharing a rate hold their relationship for as long as the
// device runs, that changing a rate mid-run does not jump the picture, and that the bank costs one
// pass per frame rather than one per read.

#include "doctest.h"
#include "core/oscillators.h"

using namespace mm;

namespace {

/// Run a bank forward to `untilMs` on the device clock, in `dtMs` steps, the way a render loop
/// does. `from` is where the clock already stands, so successive calls continue rather than
/// restarting: the first advance of a fresh bank only establishes the time base (BeatPhase's
/// first-tick guard), and calling this again from 0 would establish it a second time.
template <uint8_t N>
uint32_t run(OscillatorBank<N>& bank, uint32_t untilMs, uint32_t dtMs, uint32_t from = 0) {
    if (from == 0) bank.advanceTo(0);
    uint32_t t = from;
    while (t + dtMs <= untilMs) { t += dtMs; bank.advanceTo(t); }
    return t;
}

}  // namespace

TEST_CASE("an oscillator stays inside the range the effect asked for") {
    OscillatorBank<1> bank;
    bank.set(0, {.rate = 120, .low = 10, .high = 200, .phaseOffset = 0, .wave = Wave::Sine});
    int32_t lo = 1 << 30, hi = -(1 << 30);
    uint32_t t = 0;
    for (uint32_t f = 0; f < 2000; f++) {
        bank.advanceTo(t);
        t += 7;                                    // an irregular frame time, as a real loop has
        lo = bank.value(0) < lo ? bank.value(0) : lo;
        hi = bank.value(0) > hi ? bank.value(0) : hi;
    }
    CHECK(lo >= 10);
    CHECK(hi <= 200);
    CHECK(lo < 20);                                // and it actually reaches the ends
    CHECK(hi > 190);
}

TEST_CASE("a range given backwards runs the shape backwards") {
    OscillatorBank<2> bank;
    bank.set(0, {.rate = 60, .low = 0, .high = 100, .phaseOffset = 0, .wave = Wave::Saw});
    bank.set(1, {.rate = 60, .low = 100, .high = 0, .phaseOffset = 0, .wave = Wave::Saw});
    run(bank, 200, 5);
    // Same phase, mirrored ranges: the two are reflections, so they sum to the span.
    CHECK(bank.value(0) + bank.value(1) == 100);
}

TEST_CASE("two oscillators at the same rate hold their phase relationship indefinitely") {
    // The property a composition depends on: layers set a quarter-cycle apart must not drift, or a
    // deliberate arrangement decays into noise over an evening.
    OscillatorBank<2> bank;
    bank.set(0, {.rate = 45, .low = 0, .high = 65535, .phaseOffset = 0,     .wave = Wave::Sine});
    bank.set(1, {.rate = 45, .low = 0, .high = 65535, .phaseOffset = 16384, .wave = Wave::Sine});

    run(bank, 1600, 16);
    const int32_t earlyGap = static_cast<int32_t>(bank.phase(1)) - static_cast<int32_t>(bank.phase(0));
    uint32_t t = 1600;
    for (uint32_t f = 0; f < 200000; f++) { t += 16; bank.advanceTo(t); }   // ~an hour of frames
    const int32_t lateGap = static_cast<int32_t>(bank.phase(1)) - static_cast<int32_t>(bank.phase(0));
    CHECK(earlyGap == lateGap);
}

TEST_CASE("a rate of zero holds the picture still") {
    OscillatorBank<1> bank;
    bank.set(0, {.rate = 0, .low = 0, .high = 65535, .phaseOffset = 0, .wave = Wave::Sine});
    run(bank, 8000, 16);
    CHECK(bank.phase(0) == 0);
    CHECK(bank.value(0) == 32768);                  // a sine at phase zero sits at its midpoint
}

TEST_CASE("changing the rate continues from where the phase stands, without jumping") {
    // Live reconfiguration: turning a speed control must not make the picture leap.
    OscillatorBank<1> bank;
    bank.set(0, {.rate = 30, .low = 0, .high = 65535, .phaseOffset = 0, .wave = Wave::Saw});
    uint32_t t = 0;
    for (uint32_t f = 0; f < 50; f++) { bank.advanceTo(t); t += 16; }
    const uint16_t before = bank.unitValue(0);
    CHECK(before > 0);                               // it is genuinely mid-cycle, not at the start

    Oscillator faster = bank.get(0);
    faster.rate = 240;
    bank.set(0, faster);
    bank.advanceTo(t);                                 // the very next frame, one 16 ms step
    const uint16_t after = bank.unitValue(0);

    // One frame at the new rate moves the phase by a frame's worth, not to a new place entirely.
    const int32_t step = static_cast<int32_t>(after) - static_cast<int32_t>(before);
    CHECK(step >= 0);
    CHECK(step < 6000);                              // 16 ms at 240 BPM is ~4.3% of a cycle
}

TEST_CASE("the four waveforms have the shapes their names promise") {
    OscillatorBank<4> bank;
    for (uint8_t i = 0; i < 4; i++)
        bank.set(i, {.rate = 60, .low = 0, .high = 65535, .phaseOffset = 0, .wave = static_cast<Wave>(i)});

    // A quarter of a cycle in (60 BPM is one cycle a second, so 250 ms): the sine is at its peak,
    // the triangle halfway up its rise, the saw a quarter of the way along, the square still low.
    uint32_t t = run(bank, 250, 10);
    CHECK(bank.unitValue(0) > 64000);                                   // Sine
    CHECK(bank.unitValue(1) == doctest::Approx(32768).epsilon(0.05));   // Triangle
    CHECK(bank.unitValue(2) == doctest::Approx(16384).epsilon(0.05));   // Saw
    CHECK(bank.unitValue(3) == 0);                                      // Square

    // Half a cycle: the sine is back through its midpoint on the way down, the triangle at ITS peak
    // (it turns at the half, where the sine turned at the quarter), the square now high.
    t = run(bank, 500, 10, t);
    CHECK(bank.unitValue(0) == doctest::Approx(32768).epsilon(0.02));
    CHECK(bank.unitValue(1) > 64000);
    CHECK(bank.unitValue(3) == 65535);

    // And a full cycle brings every shape back to where it started.
    run(bank, 1000, 10, t);
    CHECK(bank.unitValue(1) == 0);
    CHECK(bank.unitValue(2) == 0);
}

TEST_CASE("a square wave is only ever fully on or fully off") {
    OscillatorBank<1> bank;
    bank.set(0, {.rate = 90, .low = 0, .high = 255, .phaseOffset = 0, .wave = Wave::Square});
    uint32_t t = 0;
    bool sawLow = false, sawHigh = false;
    for (uint32_t f = 0; f < 300; f++) {
        bank.advanceTo(t);
        t += 11;                                     // 3.3 s, about five cycles at 90 BPM
        const int32_t v = bank.value(0);
        CHECK((v == 0 || v == 255));                 // never anything in between
        sawLow = sawLow || v == 0;
        sawHigh = sawHigh || v == 255;
    }
    CHECK(sawLow);
    CHECK(sawHigh);                                  // and it does switch
}

TEST_CASE("an out-of-range oscillator index reads as zero rather than crashing the device") {
    // An effect driving the bank from a user control must not be able to fault it.
    OscillatorBank<2> bank;
    bank.set(9, {.rate = 60, .low = 0, .high = 100, .phaseOffset = 0, .wave = Wave::Sine});
    run(bank, 160, 16);
    CHECK(bank.value(9) == 0);
    CHECK(bank.phase(9) == 0);
    CHECK(bank.unitValue(200) == 0);
}

TEST_CASE("the first frame establishes the time base instead of jumping the phase") {
    // An effect enabled after the device has been up for an hour starts at zero, not an hour in.
    OscillatorBank<1> bank;
    bank.set(0, {.rate = 60, .low = 0, .high = 65535, .phaseOffset = 0, .wave = Wave::Saw});
    bank.advanceTo(3600000u);                          // first call, an hour on the clock
    CHECK(bank.phase(0) == 0);
    bank.advanceTo(3600016u);
    // One 16 ms frame in, not an hour: at 60 BPM that is 16/1000 of a turn, about 1048.
    CHECK(bank.phase(0) > 1000);
    CHECK(bank.phase(0) < 1100);
}

TEST_CASE("reset restarts the motion without losing the configuration") {
    OscillatorBank<1> bank;
    bank.set(0, {.rate = 120, .low = 5, .high = 9, .phaseOffset = 0, .wave = Wave::Triangle});
    run(bank, 1600, 16);
    bank.reset();
    CHECK(bank.phase(0) == 0);
    CHECK(bank.get(0).rate == 120);
    CHECK(bank.get(0).high == 9);
}
