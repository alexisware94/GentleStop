// test_core_assert.cpp - deterministic assertions for gs::Controller core braking logic.
// Compiles with the same stubs as test_core.cpp; drives gs::Controller::step() directly
// and compares actual output against expected values derived from the production algorithm.
#include "../gentlestop_core.h"
#include <cstdio>
#include <cmath>
#include <limits>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond) do { \
    ++g_checks; if (!(cond)) { ++g_fails; printf("FAIL line %d: %s\n", __LINE__, #cond); } \
} while (0)

static gs::Controller makeCtl(bool enabled, bool invert, double dz, double sm, double jr,
                              bool spo, double spl, bool tho, double thl, bool sl) {
    gs::Controller c;
    c.s.enabled = enabled;
    c.s.invert = invert;
    c.s.strobeLock = sl;
    c.s.speedLimitOn = spo;
    c.s.speedLimitKt = spl;
    c.s.throttleLimitOn = tho;
    c.s.throttleLimit = thl;
    c.s.deadzone = dz;
    c.s.curve = 0;
    c.s.smoothing = sm;
    c.s.jumpResponse = jr;
    c.s.clamp();
    return c;
}

static void step(gs::Controller& c, double yokeY, bool onGround, double airspeedKt,
                 double throttlePct, bool parking, double dt = 0.05) {
    gs::Inputs in;
    in.yokeY = yokeY;
    in.onGround = onGround ? 1.0 : 0.0;
    in.airspeedKt = airspeedKt;
    in.strobe = 0.0;
    in.throttle1 = throttlePct;
    in.throttle2 = 0.0;
    in.parking = parking ? 1.0 : 0.0;
    in.activation = true;
    c.step(in, dt);
}

int main() {
    // Neutral stick
    {
        gs::Controller c = makeCtl(true, false, 0.0, 0.0, 0.0, false, 60, false, 50, false);
        for (int i = 0; i < 5; i++) step(c, 0.0, true, 0.0, 0.0, false);
        CHECK(c.brake == 0.0);
        CHECK(c.gate == false);
        CHECK(c.reason == (int)gs::BLOCK_NONE);
    }
    // Full forward stick
    {
        gs::Controller c = makeCtl(true, false, 0.0, 0.0, 0.0, false, 60, false, 50, false);
        for (int i = 0; i < 20; i++) step(c, -1.0, true, 0.0, 0.0, false);
        CHECK(c.brake > 0.99);
        CHECK(c.brake <= 1.0);
        CHECK(c.gate == true);
    }
    // Full aft stick
    {
        gs::Controller c = makeCtl(true, false, 0.0, 0.0, 0.0, false, 60, false, 50, false);
        for (int i = 0; i < 5; i++) step(c, +1.0, true, 0.0, 0.0, false);
        CHECK(c.brake == 0.0);
        CHECK(c.gate == false);
    }
    // Inverted: full aft produces braking
    {
        gs::Controller c = makeCtl(true, true, 0.0, 0.0, 0.0, false, 60, false, 50, false);
        for (int i = 0; i < 20; i++) step(c, +1.0, true, 0.0, 0.0, false);
        CHECK(c.brake > 0.99);
        CHECK(c.brake <= 1.0);
    }
    // Deadzone below threshold
    {
        gs::Controller c = makeCtl(true, false, 0.2, 0.0, 0.0, false, 60, false, 50, false);
        for (int i = 0; i < 5; i++) step(c, -0.1, true, 0.0, 0.0, false);
        CHECK(c.brake == 0.0);
        CHECK(c.gate == false);
    }
    // Deadzone above threshold
    {
        gs::Controller c = makeCtl(true, false, 0.2, 0.0, 0.0, false, 60, false, 50, false);
        for (int i = 0; i < 20; i++) step(c, -0.5, true, 0.0, 0.0, false);
        CHECK(c.brake > 0.0);
    }
    // Airborne protection
    {
        gs::Controller c = makeCtl(true, false, 0.0, 0.0, 0.0, false, 60, false, 50, false);
        for (int i = 0; i < 5; i++) step(c, -1.0, false, 0.0, 0.0, false);
        CHECK(c.brake == 0.0);
        CHECK(c.reason == (int)gs::BLOCK_AIRBORNE);
    }
    // Parking brake protection
    {
        gs::Controller c = makeCtl(true, false, 0.0, 0.0, 0.0, false, 60, false, 50, false);
        for (int i = 0; i < 5; i++) step(c, -1.0, true, 0.0, 0.0, true);
        CHECK(c.brake == 0.0);
        CHECK(c.reason == (int)gs::BLOCK_PARKING);
    }
    // Disabled master switch
    {
        gs::Controller c = makeCtl(false, false, 0.0, 0.0, 0.0, false, 60, false, 50, false);
        for (int i = 0; i < 5; i++) step(c, -1.0, true, 0.0, 0.0, false);
        CHECK(c.brake == 0.0);
        CHECK(c.reason == (int)gs::BLOCK_DISABLED);
    }
    // Output bounds and finite
    {
        gs::Controller c = makeCtl(true, false, 0.0, 0.30, 0.70, false, 60, false, 50, false);
        for (int i = 0; i < 50; i++) step(c, -1.0, true, 0.0, 0.0, false);
        CHECK(c.brake >= 0.0 && c.brake <= 1.0);
        CHECK(std::isfinite(c.brake));
    }
    // Smoothing ramps gradually
    {
        gs::Controller c = makeCtl(true, false, 0.0, 0.30, 0.0, false, 60, false, 50, false);
        step(c, -1.0, true, 0.0, 0.0, false, 0.05);
        CHECK(c.brake > 0.0 && c.brake < 1.0);
        for (int i = 0; i < 100; i++) step(c, -1.0, true, 0.0, 0.0, false, 0.05);
        CHECK(c.brake > 0.99);
    }
    // Speed limit protection
    {
        gs::Controller c = makeCtl(true, false, 0.0, 0.0, 0.0, true, 60.0, false, 50, false);
        for (int i = 0; i < 5; i++) step(c, -1.0, true, 70.0, 0.0, false);
        CHECK(c.brake == 0.0);
        CHECK(c.reason == (int)gs::BLOCK_SPEED);
    }
    // Throttle limit protection
    {
        gs::Controller c = makeCtl(true, false, 0.0, 0.0, 0.0, false, 60, true, 50.0, false);
        for (int i = 0; i < 5; i++) step(c, -1.0, true, 0.0, 80.0, false);
        CHECK(c.brake == 0.0);
        CHECK(c.reason == (int)gs::BLOCK_THROTTLE);
    }
    // Intermediate forward position
    {
        gs::Controller c = makeCtl(true, false, 0.0, 0.0, 0.0, false, 60, false, 50, false);
        for (int i = 0; i < 20; i++) step(c, -0.5, true, 0.0, 0.0, false);
        CHECK(c.brake > 0.4 && c.brake < 0.6);
    }
    // Release: brake falls back to 0
    {
        gs::Controller c = makeCtl(true, false, 0.0, 0.0, 0.0, false, 60, false, 50, false);
        for (int i = 0; i < 20; i++) step(c, -1.0, true, 0.0, 0.0, false);
        double peakBrake = c.brake;
        CHECK(peakBrake > 0.9);
        for (int i = 0; i < 20; i++) step(c, 0.0, true, 0.0, 0.0, false);
        CHECK(c.brake == 0.0);
    }
    // Results
    printf("CORE ASSERT TESTS %s (%d checks)\n",
           g_fails == 0 ? "PASSED" : "FAILED", g_checks);
    if (g_fails > 0) printf("FAILED: %d / %d checks failed\n", g_fails, g_checks);
    return g_fails == 0 ? 0 : 1;
}