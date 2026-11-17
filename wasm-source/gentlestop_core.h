// GentleStop core logic -- platform independent (no MSFS headers).
// This is a line-by-line port of the braking logic from the original GentleStop.js
// (blockReason / updateGate / updateBraking / axisToBrakeValue), so behaviour is identical.
#pragma once
#include <math.h>   // no C++ std headers: libc++ include paths break against the SDK wasi sysroot

namespace gs {

inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double mx(double a, double b) { return a < b ? b : a; }   // == std::max for doubles
inline double mn(double a, double b) { return b < a ? b : a; }   // == std::min for doubles
inline double nz(double v) { return v != v ? 0.0 : v; }   // JS: (value || 0); NaN check without std::isnan

enum BlockReason {
    BLOCK_NONE = 0, BLOCK_DISABLED = 1, BLOCK_AIRBORNE = 2, BLOCK_PARKING = 3,
    BLOCK_STROBES = 4, BLOCK_THROTTLE = 5, BLOCK_SPEED = 6, BLOCK_ACTIVATION = 7
};

struct Settings {
    bool   enabled         = true;
    bool   invert          = false;
    bool   strobeLock      = false;
    bool   speedLimitOn    = false;
    double speedLimitKt    = 60.0;
    bool   throttleLimitOn = true;
    double throttleLimit   = 50.0;
    double deadzone        = 0.10;   // 0..0.5
    int    curve           = 0;      // 0 = linear, 1 = exponential ("smooth start")
    double smoothing       = 0.30;   // 0..1
    double jumpResponse    = 0.70;   // 0..1

    void clamp() {
        deadzone     = clampd(deadzone, 0.0, 0.5);
        smoothing    = clampd(smoothing, 0.0, 1.0);
        jumpResponse = clampd(jumpResponse, 0.0, 1.0);
        speedLimitKt = clampd(speedLimitKt, 10.0, 200.0);
        throttleLimit = clampd(throttleLimit, 0.0, 100.0);
        if (curve != 0 && curve != 1) curve = 0;
    }
};

struct Inputs {
    double yokeY;      // YOKE Y POSITION (MSFS: forward = negative).
                       // Unit "position" per SDK; actual delivered range is
                       // aircraft-dependent and must not be assumed normalized.
    double onGround;   // SIM ON GROUND (bool)
    double airspeedKt; // AIRSPEED INDICATED
    double strobe;     // LIGHT STROBE (bool)
    double throttle1;  // GENERAL ENG THROTTLE LEVER POSITION:1 (%)
    double throttle2;  // GENERAL ENG THROTTLE LEVER POSITION:2 (%)
    double parking;    // BRAKE PARKING POSITION (bool)
    bool activation = true; // true when no learned button exists, or the learned toggle is armed
};

class Controller {
public:
    Settings s;
    double brake = 0.0;       // commanded (smoothed) brake 0..1
    bool   gate  = false;     // braking requested and allowed
    double stick = 0.0;       // forward = positive after invert setting
    BlockReason reason = BLOCK_NONE;

    void step(const Inputs& in, double dt) {
        onGround   = in.onGround == 1.0;
        airspeedKt = nz(in.airspeedKt);
        strobeOn   = nz(in.strobe) > 0.0;
        throttlePct = mx(nz(in.throttle1), nz(in.throttle2));
        activation = in.activation;
        double rawStick = nz(in.yokeY);
        stick = s.invert ? rawStick : -rawStick;
        parking = nz(in.parking) > 0.0;

        updateGate();
        updateBraking(dt);
        reason = blockReason();
    }

    // AXIS_*_BRAKE_SET range: -16383 (released) .. +16383 (full)
    static int axisValue(double v) { return (int)floor(-16383.0 + 2.0 * 16383.0 * v + 0.5); }

private:
    bool onGround = false, parking = false, strobeOn = false;
    bool activation = true;
    double airspeedKt = 0.0, throttlePct = 0.0;

    static constexpr double MAX_SMOOTH_TAU = 1.0;
    static constexpr double JUMP_ERR_LO = 0.10;
    static constexpr double JUMP_ERR_HI = 0.60;

    BlockReason blockReason() const {
        if (!s.enabled) return BLOCK_DISABLED;
        if (!activation) return BLOCK_ACTIVATION;
        if (!onGround) return BLOCK_AIRBORNE;
        if (parking) return BLOCK_PARKING;
        if (s.strobeLock && strobeOn) return BLOCK_STROBES;
        if (s.throttleLimitOn && throttlePct >= s.throttleLimit) return BLOCK_THROTTLE;
        if (s.speedLimitOn && airspeedKt >= s.speedLimitKt) return BLOCK_SPEED;
        return BLOCK_NONE;
    }

    void updateGate() {
        double fwd = mx(0.0, stick);
        double dz = s.deadzone;
        bool wants = gate ? fwd > dz * 0.7 : fwd > dz;   // hysteresis
        gate = blockReason() == BLOCK_NONE && wants;
    }

    static double smoothstep(double a, double b, double x) {
        double t = clampd((x - a) / (b - a), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }

    void updateBraking(double dt) {
        double target = 0.0;
        if (gate) {
            double v = clampd(stick, 0.0, 1.0);
            target = s.curve == 1 ? v * v : v;
        }
        bool blocked = blockReason() != BLOCK_NONE;
        if (blocked || s.smoothing <= 0.0) {          // safety: let go quickly when not allowed
            const double riseRate = 8.0, fallRate = 10.0;
            if (target > brake) brake = mn(target, brake + riseRate * dt);
            else                brake = mx(target, brake - fallRate * dt);
            return;
        }
        double err = fabs(target - brake);
        double tau = s.smoothing * MAX_SMOOTH_TAU *
                     (1.0 - s.jumpResponse * smoothstep(JUMP_ERR_LO, JUMP_ERR_HI, err));
        if (tau < 0.001) tau = 0.001;
        double alpha = 1.0 - exp(-dt / tau);
        brake += (target - brake) * alpha;
        if (fabs(target - brake) < 0.004) brake = target;
    }
};

} // namespace gs
