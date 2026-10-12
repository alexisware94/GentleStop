/**
 * GentleStop — MSFS Toolbar Panel
 * Pitch-axis braking + digital button time-ramping brakes.
 *
 * Uses the Coherent JS bridge (SimVar.GetSimVarValue / SetSimVarValue)
 * available inside MSFS InGamePanels (2020 & 2024).
 */
"use strict";

class GentleStop {
    constructor() {
        this.settings = {
            modeA: false,          // Pitch-to-Brake
            modeB: false,          // Button Ramping
            deadzone: 0.10,        // 0..0.5
            rampDuration: 1.5,     // seconds
            speedLimitKt: 60,      // knots, disable above this on takeoff roll
            curve: "linear",       // "linear" | "exponential"
            preset: "taxi",        // "taxi" | "landing"
        };

        // Runtime state
        this.brakeLeft = 0;        // 0..1 logical brake pressure
        this.brakeRight = 0;
        this.rampActive = false;
        this.rampStart = 0;
        this.pitchInput = 0;       // -1..+1 elevator axis
        this.onGround = false;
        this.airspeedKt = 0;
        this.lastHudActivity = 0;
        this.hudVisible = false;
        this.running = false;
        this._rafId = null;
        this._lastNow = null;
        this._writePending = false;

        this._els = {};
        this._bindUI();
        this.start();
    }

    /* ------------------------------------------------------------------ */
    /* UI Binding                                                          */
    /* ------------------------------------------------------------------ */
    _bindUI() {
        const q = (id) => document.getElementById(id);
        this._els = {
            status: q("gs-status"),
            modeA: q("gs-mode-a-toggle"),
            modeB: q("gs-mode-b-toggle"),
            deadzone: q("gs-deadzone"),
            deadzoneVal: q("gs-deadzone-val"),
            rampDuration: q("gs-ramp-duration"),
            rampDurationVal: q("gs-ramp-duration-val"),
            speedLimit: q("gs-speed-limit"),
            speedLimitVal: q("gs-speed-limit-val"),
            curve: q("gs-curve-select"),
            preset: q("gs-preset-select"),
            hud: q("gs-hud"),
            fillL: q("gs-brake-fill-left"),
            fillR: q("gs-brake-fill-right"),
            valL: q("gs-brake-value-left"),
            valR: q("gs-brake-value-right"),
        };

        const toggle = (el, key) => {
            if (!el) return;
            el.addEventListener("click", () => {
                this.settings[key] = !this.settings[key];
                el.dataset.enabled = String(this.settings[key]);
                el.setAttribute("aria-checked", String(this.settings[key]));
            });
        };
        toggle(this._els.modeA, "modeA");
        toggle(this._els.modeB, "modeB");

        if (this._els.deadzone) {
            this._els.deadzone.addEventListener("input", (e) => {
                this.settings.deadzone = Number(e.target.value) / 100;
                this._els.deadzoneVal.textContent = `${e.target.value}%`;
            });
        }
        if (this._els.rampDuration) {
            this._els.rampDuration.addEventListener("input", (e) => {
                this.settings.rampDuration = Number(e.target.value);
                this._els.rampDurationVal.textContent = `${e.target.value}s`;
            });
        }
        if (this._els.speedLimit) {
            this._els.speedLimit.addEventListener("input", (e) => {
                this.settings.speedLimitKt = Number(e.target.value);
                this._els.speedLimitVal.textContent = `${e.target.value}kt`;
            });
        }
        if (this._els.curve) {
            this._els.curve.addEventListener("change", (e) => {
                this.settings.curve = e.target.value;
            });
        }
        if (this._els.preset) {
            this._els.preset.addEventListener("change", (e) => {
                const p = e.target.value;
                this.settings.preset = p;
                // Preset defaults — user can still tweak sliders afterwards
                if (p === "taxi") {
                    this.settings.deadzone = 0.10;
                    this.settings.rampDuration = 1.5;
                } else if (p === "landing") {
                    this.settings.deadzone = 0.05;
                    this.settings.rampDuration = 0.8;
                }
                this._els.deadzone.value = String(this.settings.deadzone * 100);
                this._els.deadzoneVal.textContent = `${this._els.deadzone.value}%`;
                this._els.rampDuration.value = String(this.settings.rampDuration);
                this._els.rampDurationVal.textContent = `${this.settings.rampDuration}s`;
            });
        }
    }

    /* ------------------------------------------------------------------ */
    /* SimVar bridge                                                       */
    /* ------------------------------------------------------------------ */
    _pollSim() {
        if (typeof SimVar === "undefined") return; // graceful fallback outside sim

        this.onGround = SimVar.GetSimVarValue("SIM ON GROUND", "Bool") === 1;
        this.airspeedKt = SimVar.GetSimVarValue("AIRSPEED INDICATED", "Knots") || 0;
        // ELEVATOR POSITION is read-only: negative = stick forward
        this.pitchInput = SimVar.GetSimVarValue("ELEVATOR POSITION", "Position") || 0;
    }

    /* ------------------------------------------------------------------ */
    /* Ramping engine                                                      */
    /* ------------------------------------------------------------------ */
    _applyCurve(v) {
        // v in 0..1, returns shaped 0..1
        if (this.settings.curve === "exponential") return v * v;
        return v;
    }

    _updateRamping(now, dt) {
        const { modeA, modeB, deadzone, rampDuration, speedLimitKt } = this.settings;
        const speedOk = this.airspeedKt < speedLimitKt;
        const canBrake = this.onGround && speedOk;

        let target = 0;

        // Mode A — Pitch-to-Brake
        if (modeA && canBrake) {
            // Forward stick (negative elevator deflection) -> brake pressure
            const fwd = Math.max(0, -this.pitchInput);
            if (fwd > deadzone) {
                const shaped = this._applyCurve((fwd - deadzone) / (1 - deadzone));
                target = Math.max(target, shaped);
            }
        }

        // Mode B — Button Ramping
        if (modeB && canBrake && this.rampActive) {
            const elapsed = (now - this.rampStart) / (rampDuration * 1000);
            const shaped = this._applyCurve(Math.min(1, Math.max(0, elapsed)));
            target = Math.max(target, shaped);
        }

        // Smooth toward target (slow rise, fast release) with proper delta-time
        const riseRate = 8;   // per second
        const fallRate = 6;
        const rate = target > this.brakeLeft ? riseRate : fallRate;
        let next;
        if (target > this.brakeLeft) {
            next = Math.min(target, this.brakeLeft + rate * dt);
        } else {
            next = Math.max(target, this.brakeLeft - rate * dt);
        }
        this.brakeLeft = next;
        this.brakeRight = next;

        // Elevator override prevention (requirement 2):
        // While Mode A is actively braking on the ground, neutralize the
        // user's elevator axis so the nosewheel isn't slammed into the tarmac.
        this.elevatorOverride = modeA && canBrake && target > 0;
        return target > 0;
    }

    /* ------------------------------------------------------------------ */
    /* Sim writes                                                          */
    /* ------------------------------------------------------------------ */
    // Brake events: K:AXIS_LEFT_BRAKE_SET / K:AXIS_RIGHT_BRAKE_SET
    // Range: -16383 (released) .. +16383 (max). Nonlinear: 0 ~= 27% braking.
    _axisToBrakeValue(v) {
        return Math.round(v * 16383);
    }

    _writeSimVars() {
        if (typeof SimVar === "undefined") {
            this._writePending = false;
            return;
        }
        if (this._writePending) return; // don't stack async writes
        this._writePending = true;

        const left = this._axisToBrakeValue(this.brakeLeft);
        const right = this._axisToBrakeValue(this.brakeRight);

        Promise
            .all([
                SimVar.SetSimVarValue("K:AXIS_LEFT_BRAKE_SET", "number", left),
                SimVar.SetSimVarValue("K:AXIS_RIGHT_BRAKE_SET", "number", right),
                // Neutralize elevator while pitch-braking (centered axis)
                SimVar.SetSimVarValue("K:AXIS_ELEVATOR_SET", "number",
                    this.elevatorOverride ? 0 : undefined),
            ])
            .catch((e) => console.error("GentleStop write error", e))
            .finally?.(() => { this._writePending = false; });
    }

    /* ------------------------------------------------------------------ */
    /* HUD                                                                 */
    /* ------------------------------------------------------------------ */
    _updateHud(active, now) {
        if (!this._els.hud) return;
        if (active) {
            this.hudVisible = true;
            this.lastHudActivity = now;
            this._els.hud.dataset.visible = "true";
        } else if (this.hudVisible && now - this.lastHudActivity > 2000) {
            this.hudVisible = false;
            this._els.hud.dataset.visible = "false";
        }
        if (this.hudVisible) {
            const pctL = Math.round(this.brakeLeft * 100);
            const pctR = Math.round(this.brakeRight * 100);
            this._els.fillL.style.setProperty("--pct", pctL);
            this._els.fillR.style.setProperty("--pct", pctR);
            this._els.valL.textContent = `${pctL}%`;
            this._els.valR.textContent = `${pctR}%`;
        }
    }

    /* ------------------------------------------------------------------ */
    /* Main loop                                                            */
    /* ------------------------------------------------------------------ */
    _tick = (now) => {
        if (!this.running) return;
        try {
            const dt = this._lastNow === null ? 0 : Math.min(0.1, (now - this._lastNow) / 1000);
            this._lastNow = now;

            this._pollSim();
            const active = this._updateRamping(now, dt);
            this._writeSimVars();
            this._updateHud(active, now);
        } catch (e) {
            console.error("GentleStop tick error", e);
        }
        this._rafId = requestAnimationFrame(this._tick);
    };

    start() {
        if (this.running) return;
        this.running = true;
        this._lastNow = null;
        this._rafId = requestAnimationFrame(this._tick);
    }

    stop() {
        this.running = false;
        if (this._rafId !== null) {
            cancelAnimationFrame(this._rafId);
            this._rafId = null;
        }
        // Safety: release brakes on stop
        this.brakeLeft = this.brakeRight = 0;
        this.elevatorOverride = false;
        this._writePending = false;
        this._writeSimVars();
    }
}

/* Boot when Coherent bridge is ready */
function gentleStopMain() {
    if (window.gentlestop) window.gentlestop.stop();
    window.gentlestop = new GentleStop();
}

if (typeof SimVar !== "undefined") {
    gentleStopMain();
} else if (typeof Coherent !== "undefined") {
    Coherent.on("onViewChanged", gentleStopMain);
    // Some panel contexts fire LoadingTips ready — also try deferred boot
    document.addEventListener("DOMContentLoaded", () => {
        setTimeout(() => {
            if (!window.gentlestop) gentleStopMain();
        }, 1000);
    });
} else {
    document.addEventListener("DOMContentLoaded", gentleStopMain);
}
