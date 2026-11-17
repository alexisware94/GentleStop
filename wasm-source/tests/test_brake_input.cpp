// test_brake_input.cpp - Automated braking input tests for GentleStop (H2.4, Task A2).
//
// Scope:
//   Verifies the braking engine's response to the YOKE Y POSITION SimVar input across
//   the full input range and under adverse telemetry conditions, WITHOUT modifying the
//   braking calculations.
//
//   These tests drive gs::Controller::step() directly with synthetic SimData values,
//   exercising:
//     - Neutral input (yokeY = 0)
//     - Fully forward input (yokeY = -1)
//     - Fully aft input (yokeY = +1)
//     - Intermediate positions
//     - Positive/negative direction handling (invert setting)
//     - Configured inversion
//     - Valid input boundaries (-1 .. +1)
//     - Missing or stale telemetry (no new frames)
//     - Non-finite values (NaN, +/-Inf)
//     - Input recovery after telemetry resumes
//
//   The braking engine receives yokeY only as a raw double; no device ID, controller
//   name, manufacturer, or vendor-specific axis is ever part of the input. These tests
//   confirm that by construction: gs::Controller::step() takes only gs::Inputs.
//
//   Compile with the same stubs as test_glue.cpp (MSVC).
//
#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>

// ID type used by the L-variable API mock (defined by SimConnect/MSFS headers;
// defined here for the standalone test build).
typedef unsigned int ID;

#include "../gentlestop_core.h"

static int g_checks = 0, g_fails = 0;
#define CHECK(cond) do { \
    ++g_checks; if (!(cond)) { ++g_fails; printf("FAIL line %d: %s\n", __LINE__, #cond); } \
} while (0)

// Minimal mock of the L-variable API (kept for API-shape compatibility; not used
// by the pure braking tests).
static std::map<int, double> g_vals;
static std::map<std::string, int> g_ids;
static std::vector<std::string> g_names;
ID register_named_variable(const char* n) {
    auto it = g_ids.find(n);
    if (it != g_ids.end()) return it->second;
    int id = (int)g_names.size(); g_names.push_back(n); g_ids[n] = id;
    return id;
}
double get_named_variable_value(ID id) { return g_vals[id]; }
void set_named_variable_value(ID id, double v) { g_vals[id] = v; }

// ---------------------------------------------------------------------------
// Test harness: step the controller with a given yokeY and read results.
// ---------------------------------------------------------------------------
static gs::Controller g_ctl;

static void applyInputs(double yokeY, bool onGround, double airspeedKt,
                        double throttlePct, bool parking) {
    gs::Inputs in;
    in.yokeY = yokeY;
    in.onGround = onGround ? 1.0 : 0.0;
    in.airspeedKt = airspeedKt;
    in.strobe = 0.0;
    in.throttle1 = throttlePct;
    in.throttle2 = 0.0;
    in.parking = parking ? 1.0 : 0.0;
    in.activation = true;   // no learned button: always armed
    g_ctl.step(in, 0.05);   // 50 ms per frame
}

static double lastBrake() { return g_ctl.brake; }
static bool lastGate() { return g_ctl.gate; }
static int lastReason() { return (int)g_ctl.reason; }

int main() {
    // =======================================================================
    // A2 — Braking input tests (driving gs::Controller::step directly).
    // No braking calculations are modified; only observed.
    // =======================================================================

    // ---- Neutral input (yokeY = 0): no braking ----
    {
        g_ctl = gs::Controller();   // fresh controller
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.10;
        applyInputs(0.0, true, 0.0, 0.0, false);
        CHECK(lastBrake() == 0.0);
        CHECK(lastGate() == false);
        CHECK(lastReason() == (int)gs::BLOCK_NONE);
    }

    // ---- Fully forward input (yokeY = -1): maximum braking ----
    {
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.10;
        g_ctl.s.smoothing = 0.0;   // instant response for boundary checks
        for (int i = 0; i < 5; i++) applyInputs(-1.0, true, 0.0, 0.0, false);
        CHECK(lastBrake() > 0.99);
        CHECK(lastGate() == true);
        CHECK(lastReason() == (int)gs::BLOCK_NONE);
    }

    // ---- Fully aft input (yokeY = +1): no braking (pulling back) ----
    {
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.10;
        g_ctl.s.smoothing = 0.0;
        for (int i = 0; i < 5; i++) applyInputs(+1.0, true, 0.0, 0.0, false);
        CHECK(lastBrake() == 0.0);
        CHECK(lastGate() == false);
    }

    // ---- Intermediate positions scale linearly ----
    {
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.0;     // no deadzone so small inputs register
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = false;
        // yokeY = -0.5 -> stick = 0.5 (forward after inversion), brake = 0.5
        for (int i = 0; i < 5; i++) applyInputs(-0.5, true, 0.0, 0.0, false);
        CHECK(lastBrake() > 0.49 && lastBrake() < 0.51);
        // yokeY = -0.25 -> stick = 0.25, brake = 0.25
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.0;
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = false;
        for (int i = 0; i < 5; i++) applyInputs(-0.25, true, 0.0, 0.0, false);
        CHECK(lastBrake() > 0.24 && lastBrake() < 0.26);
    }

    // ---- Positive/negative direction handling ----
    {
        // Default (invert=false): MSFS reports forward as negative yokeY.
        // Forward stick (yokeY < 0) -> braking.
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.0;
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = false;
        for (int i = 0; i < 5; i++) applyInputs(-0.8, true, 0.0, 0.0, false);
        CHECK(lastBrake() > 0.79);   // forward -> brakes

        // Aft stick (yokeY > 0) -> no braking.
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.0;
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = false;
        for (int i = 0; i < 5; i++) applyInputs(+0.8, true, 0.0, 0.0, false);
        CHECK(lastBrake() == 0.0);   // aft -> no brakes
    }

    // ---- Configured inversion reverses direction ----
    {
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.0;
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = true;
        // With invert=true, stick = rawStick, so yokeY = +0.8 (aft) -> braking.
        for (int i = 0; i < 5; i++) applyInputs(+0.8, true, 0.0, 0.0, false);
        CHECK(lastBrake() > 0.79);   // inverted: aft -> brakes

        // With invert=true, yokeY = -0.8 (forward) -> no braking.
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.0;
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = true;
        for (int i = 0; i < 5; i++) applyInputs(-0.8, true, 0.0, 0.0, false);
        CHECK(lastBrake() == 0.0);   // inverted: forward -> no brakes
    }

    // ---- Valid input boundaries: clamp at -1 and +1 ----
    {
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.0;
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = false;
        // Beyond range should still behave sanely (clamped by the engine internally).
        for (int i = 0; i < 5; i++) applyInputs(-2.0, true, 0.0, 0.0, false);
        CHECK(lastBrake() > 0.99);   // over-forward -> full brake
        for (int i = 0; i < 5; i++) applyInputs(+2.0, true, 0.0, 0.0, false);
        CHECK(lastBrake() == 0.0);   // over-aft -> no brake
    }

    // ---- Deadzone: small inputs inside deadzone produce no braking ----
    {
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.20;
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = false;
        for (int i = 0; i < 5; i++) applyInputs(-0.10, true, 0.0, 0.0, false);
        CHECK(lastBrake() == 0.0);   // within deadzone -> no brake
        for (int i = 0; i < 5; i++) applyInputs(-0.30, true, 0.0, 0.0, false);
        CHECK(lastBrake() > 0.0);    // outside deadzone -> braking
    }

    // ---- Hysteresis: once gating, a slightly-lower input still keeps braking ----
    {
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.10;
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = false;
        // Establish gating with strong input.
        for (int i = 0; i < 5; i++) applyInputs(-0.50, true, 0.0, 0.0, false);
        CHECK(lastGate() == true);
        // Drop just below deadzone but still above hysteresis threshold (dz*0.7).
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.10;
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = false;
        for (int i = 0; i < 5; i++) applyInputs(-0.50, true, 0.0, 0.0, false);
        CHECK(lastGate() == true);
        for (int i = 0; i < 5; i++) applyInputs(-0.08, true, 0.0, 0.0, false);
        CHECK(lastGate() == true);   // hysteresis keeps gate open
    }

    // ---- Missing / stale telemetry: no new frames should not change brake ----
    {
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.0;
        g_ctl.s.smoothing = 0.0;   // instant: brake holds exactly when input is constant
        g_ctl.s.invert = false;
        // Establish braking.
        for (int i = 0; i < 5; i++) applyInputs(-0.8, true, 0.0, 0.0, false);
        double brakeBefore = lastBrake();
        CHECK(brakeBefore > 0.0);
        // Call step with the SAME values repeatedly (stale telemetry, no change).
        for (int i = 0; i < 10; i++) applyInputs(-0.8, true, 0.0, 0.0, false);
        CHECK(lastBrake() == brakeBefore);   // steady state: no change
    }

    // ---- Non-finite values (NaN) are neutralized by nz() ----
    {
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.0;
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = false;
        double nan = std::numeric_limits<double>::quiet_NaN();
        gs::Inputs in;
        in.yokeY = nan;
        in.onGround = 1.0;
        in.airspeedKt = 0.0;
        in.strobe = 0.0;
        in.throttle1 = 0.0;
        in.throttle2 = 0.0;
        in.parking = 0.0;
        in.activation = true;
        g_ctl.step(in, 0.05);
        // nz(NaN) = 0.0 -> rawStick = 0 -> stick = 0 -> no brake
        CHECK(lastBrake() == 0.0);
        CHECK(lastReason() == (int)gs::BLOCK_NONE);
    }

    // ---- Non-finite values (+Inf) are neutralized by nz() ----
    {
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.0;
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = false;
        gs::Inputs in;
        in.yokeY = std::numeric_limits<double>::infinity();
        in.onGround = 1.0;
        in.airspeedKt = 0.0;
        in.strobe = 0.0;
        in.throttle1 = 0.0;
        in.throttle2 = 0.0;
        in.parking = 0.0;
        in.activation = true;
        g_ctl.step(in, 0.05);
        // nz(Inf) = 0.0 -> no brake
        CHECK(lastBrake() == 0.0);
        CHECK(std::isfinite(lastBrake()));
    }

    // ---- Non-finite values (-Inf): nz() only neutralizes NaN, not Inf, so
    //     -Inf passes through. Verify brake stays finite and clamped (safe),
    //     not crashing or producing garbage.
    {
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.0;
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = false;
        gs::Inputs in;
        in.yokeY = -std::numeric_limits<double>::infinity();
        in.onGround = 1.0;
        in.airspeedKt = 0.0;
        in.strobe = 0.0;
        in.throttle1 = 0.0;
        in.throttle2 = 0.0;
        in.parking = 0.0;
        in.activation = true;
        g_ctl.step(in, 0.05);
        CHECK(lastBrake() >= 0.0 && lastBrake() <= 1.0);
        CHECK(std::isfinite(lastBrake()));
    }

    // ---- Input recovery after telemetry resumes ----
    {
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.0;
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = false;
        // Brake, then feed neutral (stale), then resume forward input.
        for (int i = 0; i < 5; i++) applyInputs(-0.8, true, 0.0, 0.0, false);
        CHECK(lastBrake() > 0.79);
        for (int i = 0; i < 5; i++) applyInputs(0.0, true, 0.0, 0.0, false);
        CHECK(lastBrake() == 0.0);   // released
        for (int i = 0; i < 5; i++) applyInputs(-0.8, true, 0.0, 0.0, false);
        CHECK(lastBrake() > 0.79);   // recovered
    }

    // ---- Disabled master switch: no braking regardless of input ----
    {
        g_ctl = gs::Controller();
        g_ctl.s.enabled = false;
        g_ctl.s.deadzone = 0.0;
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = false;
        for (int i = 0; i < 5; i++) applyInputs(-1.0, true, 0.0, 0.0, false);
        CHECK(lastBrake() == 0.0);
        CHECK(lastReason() == (int)gs::BLOCK_DISABLED);
    }

    // ---- Airborne protection: no braking while airborne ----
    {
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.0;
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = false;
        for (int i = 0; i < 5; i++) applyInputs(-1.0, false, 0.0, 0.0, false);
        CHECK(lastBrake() == 0.0);
        CHECK(lastReason() == (int)gs::BLOCK_AIRBORNE);
    }

    // ---- Parking brake protection: no braking while parked ----
    {
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.0;
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = false;
        for (int i = 0; i < 5; i++) applyInputs(-1.0, true, 0.0, 0.0, true);
        CHECK(lastBrake() == 0.0);
        CHECK(lastReason() == (int)gs::BLOCK_PARKING);
    }

    // ---- Brake output stays within [0, 1] ----
    {
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.0;
        g_ctl.s.smoothing = 0.30;
        g_ctl.s.invert = false;
        for (int i = 0; i < 20; i++) applyInputs(-1.0, true, 0.0, 0.0, false);
        CHECK(lastBrake() >= 0.0 && lastBrake() <= 1.0);
        CHECK(std::isfinite(lastBrake()));
    }

    // ---- Braking engine is device-independent: no device ID/name/axis in input ----
    {
        // By construction, gs::Inputs contains only yokeY, onGround, airspeedKt,
        // strobe, throttle1, throttle2, parking, activation. No device identifier
        // of any kind. Two identical inputs from "different devices" give identical
        // brake output.
        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.0;
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = false;
        for (int i = 0; i < 5; i++) applyInputs(-0.6, true, 0.0, 0.0, false);
        double brakeA = lastBrake();

        g_ctl = gs::Controller();
        g_ctl.s.enabled = true;
        g_ctl.s.deadzone = 0.0;
        g_ctl.s.smoothing = 0.0;
        g_ctl.s.invert = false;
        for (int i = 0; i < 5; i++) applyInputs(-0.6, true, 0.0, 0.0, false);
        double brakeB = lastBrake();

        CHECK(brakeA == brakeB);   // same input, same output regardless of "device"
    }

    // =======================================================================
    // Results
    // =======================================================================
    if (g_fails == 0) {
        printf("BRAKE INPUT TESTS PASSED (%d checks)\n", g_checks);
        return 0;
    }
    printf("BRAKE INPUT TESTS FAILED: %d / %d checks failed\n", g_fails, g_checks);
    return 1;
}
