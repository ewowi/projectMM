// @module AudioService
// @also WledAudioSyncPacket

// Drives AudioService's WLED audio-sync socket lifecycle on the host through the public
// tick(), the same entry the scheduler calls on-device. Covers: lazy open once per mode
// (syncEnsureSocket latches), the send path reaching "sending", send throttling, and the
// receive path over a real localhost UDP round-trip (frame replacement + the fresh→stale
// listening fallback of the pure sink). platform::networkReady() is true on desktop, so the lazy open fires
// on the first tick, mirroring a device once its interface is up.
//
// Time is driven deterministically with platform::setTestNowMs() (the animation-test idiom)
// so the throttle/fallback windows are exact and the suite never sleeps a real second; only
// the actual UDP delivery is real, polled with a bounded retry (the NetworkReceiveEffect
// localhost-round-trip pattern). A test port (not 11988) avoids colliding with a running
// projectMM desktop app that would hold the real sync port.

#include "doctest.h"
#include "core/AudioService.h"
#include "light/WLEDAudioSyncPacket.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstring>

using namespace mm;

namespace {
constexpr uint16_t kTestSyncPort = 21988;   // a free high port, not the real 11988

// The status read-out is published by tick1s(); call it after a tick() so the assertions
// below see the current state string rather than the setup() baseline.
const char* status(AudioService& a) { a.tick1s(); return a.syncStatusForTest(); }

// A guard that freezes virtual time for a case and restores the real clock on scope exit,
// so a thrown assertion can't leak frozen time into the next case.
struct FrozenClock {
    FrozenClock(uint32_t ms) { platform::setTestNowMs(ms); }
    ~FrozenClock() { platform::setTestNowMs(0); }
    void advance(uint32_t ms) { now_ += ms; platform::setTestNowMs(now_); }
    uint32_t now_ = 1;
};
}  // namespace

// Regression: the mic/capture status is a LOCAL-mode read-out. Switching to Receive network /
// Simulate must clear it so a stale message doesn't linger on the status row, those modes report
// through the separate "sync status" row and have no input to diagnose. Before the fix,
// prepare()'s non-Local branch deinit()'d the peripheral but left the status string set.
// What Local mode leaves depends on the host: a capture-capable desktop usually inits cleanly
// (no status at all, capture IS live), a locked-down one reports "capture init failed", an I2S
// target with unset pins reports "mic: set sckPin / wsPin / sdPin". The rule under test is the
// same in every case: whatever Local left, leaving Local clears it.
TEST_CASE("AudioService: switching out of Local mode clears the mic status") {
    AudioService a;
    a.mode = 0;                      // Local audio
    a.applyState();
    // Any of the three Local outcomes above is legitimate; only note which one happened.
    const bool localLeftStatus = a.status() != nullptr && a.status()[0] != 0;
    (void)localLeftStatus;

    // What must not survive is the MIC message: a wiring diagnosis for hardware this mode does not
    // use would send the user to a pin that is not the problem. The line itself is not required to
    // be empty, because sync reports there too (receive says it is waiting for the network).
    auto noMicMessage = [&]() {
        const char* s = a.status();
        return s == nullptr || std::strstr(s, "mic") == nullptr;
    };

    a.mode = 1;                      // receive network
    a.applyState();                  // prepare() non-Local branch must clear the stale mic status
    CHECK(noMicMessage());

    // And back to Simulate, same rule (no mic there either).
    a.mode = AudioService::kSimMode;
    a.applyState();
    CHECK(noMicMessage());
    // Simulate has no sync either, so here the line IS empty. Null or empty: a module that has
    // never reported anything has a null status, which is the same "nothing to say".
    CHECK((a.status() == nullptr || a.status()[0] == 0));

    a.release();
}

TEST_CASE("AudioService Local+send: lazy-opens once and reports sending") {
    FrozenClock clk(1);
    AudioService a;
    a.mode = 0;
    a.send = true;   // local audio, broadcasting
    a.syncPort = kTestSyncPort;
    a.applyState();                  // build: syncReinit(), socket NOT opened here (boot-safe)
    CHECK(std::strstr(status(a), "waiting") != nullptr);   // no tick() yet → still waiting

    a.tick();                        // networkReady() true on desktop → opens now
    CHECK(std::strcmp(status(a), "sending") == 0);
    CHECK(a.syncOpenForTest());

    // Idempotent: a second tick doesn't re-open (the latch holds).
    a.tick();
    CHECK(a.syncOpenForTest());
    CHECK(std::strcmp(status(a), "sending") == 0);

    a.release();
    CHECK_FALSE(a.syncOpenForTest());
}

TEST_CASE("AudioService Local+send: broadcasts are throttled to ~kSyncSendIntervalMs") {
    FrozenClock clk(1);
    AudioService a;
    a.mode = 0;
    a.send = true;   // local audio, broadcasting
    a.syncPort = kTestSyncPort;
    a.applyState();
    a.tick();                        // opens + first send
    REQUIRE(a.syncOpenForTest());

    // More ticks within the same interval must not each emit: the send count does not advance
    // while the throttle window is open.
    const uint32_t c0 = a.syncSendCountForTest();
    a.tick();
    a.tick();
    CHECK(a.syncSendCountForTest() == c0);   // throttled: no send per tick

    // After the interval elapses, exactly one more send is allowed.
    clk.advance(AudioService::syncSendIntervalMsForTest() + 5);
    a.tick();
    CHECK(a.syncSendCountForTest() - c0 == 1);

    a.release();
}

// The fleet-source contract: a desktop in Local mode with "send audio" on captures its own
// audio AND broadcasts, send fires from the same tick() that runs the capture path, so the
// capture gate no longer starves the sender (the pre-capture desktop returned from tick()
// before ever sending in Local mode was impossible: sends ran first, this pins that the two
// paths now COEXIST on a capture host: capture may be live, and sends still fire throttled).
TEST_CASE("AudioService Local+send on a capture host: capture and broadcast coexist") {
    if constexpr (!platform::hasAudioCapture) return;
    FrozenClock clk(1);
    AudioService a;
    a.mode = 0;
    a.send = true;
    a.syncPort = kTestSyncPort;
    a.applyState();   // may or may not bring capture up (host permission dependent), both fine
    a.tick();         // opens the socket + first send, then runs the local capture path
    REQUIRE(a.syncOpenForTest());
    CHECK(std::strcmp(status(a), "sending") == 0);
    const uint8_t c0 = a.syncSendCountForTest();
    clk.advance(AudioService::syncSendIntervalMsForTest() + 5);
    a.tick();         // capture read + throttled send in one tick, no early return
    CHECK(a.syncSendCountForTest() - c0 == 1);
    a.release();
}

TEST_CASE("AudioService Receive: a localhost WLED packet drives frame_, then holds it and reports listening") {
    FrozenClock clk(1);
    AudioService a;
    a.mode = 1;   // receive network
    a.syncPort = kTestSyncPort;
    a.applyState();
    a.tick();                        // binds kTestSyncPort
    REQUIRE(a.syncOpenForTest());
    CHECK(std::strcmp(status(a), "listening") == 0);

    // Send a real WLED v2 packet to the bound port over loopback.
    AudioFrame peer;
    peer.level = 222; peer.levelSmoothed = 111; peer.peakHz = 660; peer.peakMag = 55;
    for (int i = 0; i < 16; i++) peer.bands[i] = static_cast<uint8_t>(i * 8);
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    buildWledAudioSync(pkt, peer, /*peak=*/false);

    platform::UdpSocket tx;
    REQUIRE(tx.open());
    REQUIRE(tx.connect("127.0.0.1", kTestSyncPort));
    REQUIRE(tx.sendTo(pkt, WLED_SYNC_PACKET_SIZE));

    // Loopback delivery is async in real time, poll tick() until the peer frame lands
    // (bounded, ≤100 iterations). Virtual time stays frozen, so the frame counts as fresh.
    bool landed = false;
    for (int i = 0; i < 100 && !landed; i++) {
        a.tick();
        landed = a.audioFrame()->level == 222 && a.audioFrame()->peakHz == 660;
        if (!landed) platform::delayMs(1);   // real wait for the datagram, not virtual time
    }
    CHECK(landed);
    CHECK(a.audioFrame()->levelSmoothed == 111);
    // The ballistic is OURS, not the packet's: the smoothed bands rise toward the received raw
    // bands, and they survive the whole-frame copy the next packet makes (a copy that zeroed them
    // forty times a second would leave nothing to fall slowly).
    CHECK(a.audioFrame()->bandsSmoothed[15] > 80);   // peer.bands[15] is 120; one block of rise
    AudioFrame quiet;                                  // then the peer goes silent
    buildWledAudioSync(pkt, quiet, /*peak=*/false);
    REQUIRE(tx.sendTo(pkt, WLED_SYNC_PACKET_SIZE));
    bool fell = false;
    for (int i = 0; i < 100 && !fell; i++) {
        a.tick();
        fell = a.audioFrame()->level == 0;
        if (!fell) platform::delayMs(1);
    }
    CHECK(fell);
    CHECK(a.audioFrame()->bandsSmoothed[15] > 40);   // still falling, not reset by the copy
    // Named, not just "receiving": the packet came from loopback, so the status has to say so. A
    // receiver that cannot name its source looks identical to one locked onto the wrong device.
    CHECK(std::strcmp(status(a), "receiving from 127.0.0.1") == 0);

    // Receive is a pure network sink: advance virtual time past the fallback window with no new
    // packet, the peer goes stale and the status falls back to "listening" (bound, no fresh peer).
    // The last frame is held; the local mic never runs in this mode. Deterministic: no real sleep.
    clk.advance(AudioService::syncFallbackMsForTest() + 20);
    a.tick();
    CHECK(std::strcmp(status(a), "listening") == 0);

    tx.close();
    a.release();
}

TEST_CASE("AudioService Receive: a failed bind backs off instead of retrying every tick") {
    FrozenClock clk(1);
    // Force the bind to fail deterministically. The obvious approach, hog the port with a second
    // socket, is NOT portable: on Linux, SO_REUSEADDR on a UDP socket bound to INADDR_ANY permits
    // the overlapping bind, so the hog succeeds and the failure never happens. (That silently broke
    // this test on Linux for as long as it existed; nothing caught it because CI did not compile the
    // C++ tests until the sanitizer job.) A privileged port is no better, modern macOS lets a
    // non-root process bind port 80.
    platform::setTestBindFails(true);

    AudioService a;
    a.mode = 1;   // receive network
    a.syncPort = kTestSyncPort;
    a.applyState();
    a.tick();                        // first bring-up attempt → bind fails
    CHECK_FALSE(a.syncOpenForTest());
    CHECK(std::strcmp(status(a), "receive: bind failed") == 0);

    // Within the backoff window, further ticks must NOT retry, the socket stays closed and
    // the status is unchanged (no per-tick socket() churn). tick1s() only reasserts the
    // baseline while syncOpen_ is false, so the string staying put is the observable proof.
    a.tick();
    a.tick();
    CHECK_FALSE(a.syncOpenForTest());
    CHECK(std::strcmp(a.syncStatusForTest(), "receive: bind failed") == 0);

    // Let the bind succeed and advance past the backoff, the next tick retries and succeeds.
    platform::setTestBindFails(false);
    clk.advance(AudioService::syncOpenRetryMsForTest() + 5);
    a.tick();
    CHECK(a.syncOpenForTest());
    CHECK(std::strcmp(status(a), "listening") == 0);

    a.release();
}

TEST_CASE("AudioService Local (not sending): no socket, reports off") {
    FrozenClock clk(1);
    AudioService a;
    a.mode = 0;   // local audio, not sending (sync == off)
    a.applyState();
    a.tick();
    CHECK_FALSE(a.syncOpenForTest());
    // Nothing about SYNC on the status line: it is off, and the `mode` control already says so. The
    // line is not required to be empty, because this is Local mode and a mic that cannot be opened
    // reports there: CI has no capture device, so it says so, and that message must survive.
    const char* s = status(a);
    CHECK(std::strstr(s, "waiting for network") == nullptr);
    CHECK(std::strstr(s, "listening on") == nullptr);
    CHECK(std::strstr(s, "from ") == nullptr);
    a.release();
}

// Regression: a persisted `send` must NOT broadcast once the module switches to Simulate mode, Simulate
// has no captured frame worth sending, so sync() (and thus the socket) must go quiet. Pins the mode==0
// guard on the send leg of sync().
TEST_CASE("AudioService Local+send → Simulate: send stops, no socket") {
    FrozenClock clk(1);
    AudioService a;
    a.mode = 0; a.send = true;   // local audio, broadcasting
    a.syncPort = kTestSyncPort;
    a.applyState();
    a.tick();                    // opens the send socket
    REQUIRE(a.syncOpenForTest());

    // Switch to Simulate WITHOUT clearing the persisted send flag.
    a.mode = AudioService::kSimMode;
    a.applyState();              // re-prepare: syncReinit closes the socket for the new (no-socket) mode
    a.tick();
    CHECK_FALSE(a.syncOpenForTest());   // socket closed, nothing broadcasting
    CHECK(status(a)[0] == 0);           // and quiet on the status line in Simulate
    a.release();
}
