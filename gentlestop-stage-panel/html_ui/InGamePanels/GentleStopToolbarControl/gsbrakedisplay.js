/**
 * GentleStop -- panel brake-bar display interpolation (pure math, no DOM).
 *
 * The WASM module already smooths the real brake command every sim frame and
 * reports it with full precision; the panel's snapshots arrive from the 250 ms
 * status poll plus the module's own adaptive broadcast while the brake value is
 * moving. Snapping the bars to each snapshot stepped visibly. This file
 * animates the DISPLAYED bar value between snapshots on the panel's existing
 * 60 fps loop:
 *   - exponential approach at a rate sized against the module's real transient
 *     (gentlestop_core.h updateBraking: ~11/s worst-case move), so the displayed
 *     bar stays in narrow sync with the actual brake action;
 *   - a faster rate while releasing, plus a snap floor near zero, so the bars
 *     return promptly to exactly 0 when braking stops (no lingering tail
 *     beyond the module's own drain).
 * The real braking logic in the module is never touched by this.
 *
 * UMD-style: the offline Node test (tools/test_panel_brake_display.js) can
 * require() this file, the browser gets the GentleStopDisplayMath global.
 */
(function (root) {
    "use strict";

    // Rates are per second, sized against the module's real transient: with the
    // production defaults (smoothing=0.3, jumpResponse=0.7) the smoothed brake
    // can move at up to ~11/s (tau floor ~0.09 s at large error), so a chase
    // rate K leaves a steady lag of about (real rate)/K during fast moves.
    // 25/50 caps that at ~0.35 of the bar for the instant of a full-stick slam
    // while staying visibly smooth - and both settle exactly on the anchor.
    var K_RISE = 25.0;
    var K_FALL = 50.0;                 // release: prompt return to zero
    var SNAP_EPS = 0.0015;             // close enough to a live target: settle exactly
    var SNAP_ZERO = 0.005;             // releasing: settle exactly at 0 sooner

    function barStep(shown, target, dtSec) {
        var s = (typeof shown === "number" && isFinite(shown)) ? shown : 0;
        var t = (typeof target === "number" && isFinite(target)) ? target : 0;
        if (t < 0) t = 0; else if (t > 1) t = 1;
        if (!(dtSec > 0)) dtSec = 1 / 60;
        if (dtSec > 0.5) dtSec = 0.5;  // panel returned from being hidden: converge at once
        var k = t > s ? K_RISE : K_FALL;
        var v = s + (t - s) * (1 - Math.exp(-k * dtSec));
        if (v < 0) v = 0; else if (v > 1) v = 1;
        var eps = t <= 0 ? SNAP_ZERO : SNAP_EPS;
        if (Math.abs(t - v) < eps) return t;   // settle exactly; no endless micro-motion
        return v;
    }

    var api = { barStep: barStep,
                // "step" alias: the panel's legacy call-site name (kept so any
                // external caller using step() stays valid).
                step: barStep,
                K_RISE: K_RISE, K_FALL: K_FALL, SNAP_EPS: SNAP_EPS, SNAP_ZERO: SNAP_ZERO };
    if (typeof module === "object" && module.exports && typeof module.exports === "object") {
        module.exports = api;
    } else {
        root.GentleStopDisplayMath = api;
    }
})(typeof window !== "undefined" ? window : (typeof globalThis !== "undefined" ? globalThis : this));
