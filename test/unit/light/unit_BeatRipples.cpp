// @module BeatRipplesEffect
// @also golden_frame

// The wave surface. A golden hash pins which pixels light, but it passes just as happily on a
// frame that is entirely black, which is how this effect once shipped rendering nothing at all:
// the stone pressed into the surface was scaled in units of tens while the render reads a slope
// in units of thousands, so every ripple fell below the threshold that lights a pixel. What is
// pinned here is that water is VISIBLE and that it MOVES.

#include "doctest.h"
#include "golden_frame.h"
#include "light/effects/BeatRipplesEffect.h"

using namespace mm;

namespace {
/// Brightest byte anywhere in the frame: how strongly the surface catches the light.
int brightestLight(const Layer& layer) {
    const auto& b = layer.buffer();
    int peak = 0;
    for (size_t i = 0; i < b.bytes(); i++) if (b.data()[i] > peak) peak = b.data()[i];
    return peak;
}
}  // namespace

TEST_CASE("still water shows its ripples, and they spread") {
    golden::ScopedTestClock clock(1000);
    Layouts layouts; GridLayout grid; Layer layer; BeatRipplesEffect e;
    grid.width = 32; grid.height = 32; grid.depth = 1;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&e);
    layer.applyState();

    // With no audio at all the idle rain is the only source, and the first drop lands on the
    // opening frame rather than after a silent interval of black water.
    for (int f = 0; f < 40; f++) { platform::setTestNowMs(1000 + f * 20u); layer.tick(); }
    const int early = brightestLight(layer);
    CHECK_MESSAGE(early > 16, "idle rain must light the surface, not leave it black");

    // And the surface keeps ringing rather than settling immediately: a wave that vanished in a
    // frame would satisfy the check above while still looking like nothing.
    for (int f = 40; f < 200; f++) { platform::setTestNowMs(1000 + f * 20u); layer.tick(); }
    CHECK_MESSAGE(brightestLight(layer) > 16, "the water keeps moving while the rain falls");
}
