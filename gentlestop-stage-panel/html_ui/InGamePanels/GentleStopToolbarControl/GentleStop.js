/**
 * GentleStop -- MSFS Toolbar Panel (settings + status UI)
 *
 * The braking itself now runs in the WASM module (modules/gentlestop.wasm), so it keeps
 * working while this panel is closed. This panel only:
 *   - edits the 3 settings profiles (saved automatically, as before)
 *   - pushes the active profile to the module through L:GS_* variables
 *   - shows the module's live status (brake %, why braking is blocked)
 */
"use strict";

(function () {
    var META_KEY = "gentlestop.meta.v5";
    var PROFILE_KEY = "gentlestop.profile.v5.";
    var PROFILE_COUNT = 3;

    function defaults() {
        return {
            enabled: true,         // master switch for stick-forward braking
            invert: false,         // OFF = the normal, already-corrected direction (MSFS reports forward as negative)
            strobeLock: false,     // no braking while the strobe lights are on
            speedLimitOn: false,   // optional speed limit
            speedLimitKt: 60,      // knots, only used when speedLimitOn
            throttleLimitOn: true, // optional throttle limit
            throttleLimit: 50,     // %, only used when throttleLimitOn
            deadzone: 0.10,        // 0..0.5 of stick travel
            curve: "linear",       // "linear" (1:1) | "exponential" (softer start)
            smoothing: 0.30,       // 0..1, how much the brake lags the stick (0 = off / instant)
            jumpResponse: 0.70,    // 0..1, how much smoothing is removed for big stick moves (0 = same smoothing always)
            activationEvent: -1    // FsEventId; -1 means stick-forward braking is always armed
        };
    }

    function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }

    function init() {
        var statusEl = document.getElementById("gs-status");
        var mainSection = document.getElementById("mainSection");
        var rootEl = document.getElementById("GentleStopRoot");
        var ingameUiEl = document.getElementById("GentleStopPanel");

        // Wait until the template is stamped and the elements exist
        if (!statusEl || !mainSection || !rootEl || !ingameUiEl) {
            setTimeout(init, 100);
            return;
        }
        // The panel element is re-created when the toolbar panel is closed and reopened. Wiring the same
        // elements twice would send the module two commands per click (and restart Learn mid-press).
        // Wire once per panel instance. A reopened panel gets fresh elements, so a new root element must be
        // wired again (a global flag would leave the rebuilt panel with dead buttons).
        if (rootEl.__gsWired) return;
        rootEl.__gsWired = true;

        // ---------------------------------------------------------------
        // Storage: prefer the sim's own data store when present, always mirror to localStorage
        // ---------------------------------------------------------------
        var simStoreOk = (typeof SetStoredData === "function" && typeof GetStoredData === "function") ||
            (typeof DataStore !== "undefined" && DataStore && typeof DataStore.set === "function" && typeof DataStore.get === "function");
        var localOk = false;
        try { localOk = !!window.localStorage; } catch (e) { localOk = false; }
        var storageKind = (simStoreOk ? "sim" : "") + (simStoreOk && localOk ? "+" : "") + (localOk ? "browser" : "");
        if (!storageKind) storageKind = "none";

        function storeGet(key) {
            var v = null;
            try {
                if (typeof GetStoredData === "function") v = GetStoredData(key);
                else if (typeof DataStore !== "undefined" && DataStore && typeof DataStore.get === "function") v = DataStore.get(key);
            } catch (e) { v = null; }
            if (typeof v === "string" && v.length) return v;
            try { if (localOk) { v = localStorage.getItem(key); if (typeof v === "string" && v.length) return v; } } catch (e) { /* ignore */ }
            return null;
        }
        function storeSet(key, val) {
            try {
                if (typeof SetStoredData === "function") SetStoredData(key, val);
                else if (typeof DataStore !== "undefined" && DataStore && typeof DataStore.set === "function") DataStore.set(key, val);
            } catch (e) { /* ignore */ }
            try { if (localOk) localStorage.setItem(key, val); } catch (e) { /* ignore */ }
        }

        // ---------------------------------------------------------------
        // Profiles
        // ---------------------------------------------------------------
        var settings = defaults();
        var activeProfile = 1;

        function loadProfile(n) {
            activeProfile = n;
            settings = defaults();
            try {
                var raw = storeGet(PROFILE_KEY + n);
                if (raw) {
                    var o = JSON.parse(raw);
                    for (var k in settings) {
                        if (Object.prototype.hasOwnProperty.call(o, k) && typeof o[k] === typeof settings[k]) settings[k] = o[k];
                    }
                }
            } catch (e) { /* ignore: keep defaults */ }
            settings.deadzone = clamp(settings.deadzone, 0, 0.5);
            settings.smoothing = clamp(settings.smoothing, 0, 1);
            settings.jumpResponse = clamp(settings.jumpResponse, 0, 1);
            settings.speedLimitKt = clamp(settings.speedLimitKt, 10, 200);
            settings.throttleLimit = clamp(settings.throttleLimit, 0, 100);
            if (settings.curve !== "linear" && settings.curve !== "exponential") settings.curve = "linear";
            if (typeof settings.activationEvent !== "number" || !isFinite(settings.activationEvent) || settings.activationEvent < -3) settings.activationEvent = -1;
            else settings.activationEvent = Math.floor(settings.activationEvent);
            storeSet(META_KEY, JSON.stringify({ active: n }));
        }
        function saveSettings() { dirty = true; storeSet(PROFILE_KEY + activeProfile, JSON.stringify(settings)); pushSettings(); }

        try {
            var meta = JSON.parse(storeGet(META_KEY) || "{}");
            if (meta && meta.active >= 1 && meta.active <= PROFILE_COUNT) activeProfile = meta.active;
        } catch (e) { activeProfile = 1; }
        loadProfile(activeProfile);

        // ---------------------------------------------------------------
        // Runtime state
        // ---------------------------------------------------------------
        var brake = 0;           // commanded (smoothed) 0..1
        var gate = false;        // braking requested and allowed
        var stick = 0;           // forward = positive after the invert setting
        var rawStick = 0;
        var simBrake = 0;
        var parking = false;
        var onGround = false;
        var airspeedKt = 0;
        var throttlePct = 0;
        var strobeOn = false;
        var running = true;

        // ---------------------------------------------------------------
        // Config snapshot / dirty-state tracking
        // ---------------------------------------------------------------
        // The module's persisted configuration is the authoritative source of
        // truth. The panel applies the module's snapshot on first receipt and
        // only sends deliberate user edits afterwards (dirty flag). A stale
        // snapshot that predates user edits is never applied.
        var snapshotApplied = false;   // module's config snapshot has been applied
        var dirty = false;             // user-made changes not yet pushed to module
        var lastAppliedSeq = 0;        // settingsSeq of the last applied snapshot

        function $(id) { return document.getElementById(id); }
        function on(el, ev, fn) {
            if (!el) return;
            if (!el.__gsHandlers) el.__gsHandlers = {};
            if (el.__gsHandlers[ev]) return;   // never wire the same element twice
            el.__gsHandlers[ev] = fn;
            el.addEventListener(ev, fn);
        }

        var els = {
            status: statusEl,
            fillL: $("gs-brake-fill-left"), fillR: $("gs-brake-fill-right"),
            valL: $("gs-brake-value-left"), valR: $("gs-brake-value-right"),
            diag: $("gs-diag"),
            reset: $("gs-reset-btn"),
            activationStatus: $("gs-activation-status"),
            learnActivation: $("gs-learn-activation"),
            clearActivation: $("gs-clear-activation"),
            copyDiagnostics: $("gs-copy-diagnostics"),
            advancedToggle: $("gs-advanced-toggle")
        };

        // ---------------------------------------------------------------
        // UI wiring (toggles / sliders / segmented buttons)
        // ---------------------------------------------------------------
        function setSwitch(el, v) {
            if (!el) return;
            el.setAttribute("data-enabled", String(v));
            el.setAttribute("aria-checked", String(v));
        }

        var toggles = [
            { el: $("gs-enabled-toggle"), key: "enabled" },
            { el: $("gs-invert-toggle"), key: "invert" },
            { el: $("gs-strobe-toggle"), key: "strobeLock" },
            { el: $("gs-speed-toggle"), key: "speedLimitOn" },
            { el: $("gs-throttle-toggle"), key: "throttleLimitOn" }
        ];

        var sliders = [
            { el: $("gs-deadzone"), val: $("gs-deadzone-val"), key: "deadzone",
              toUi: function (v) { return Math.round(v * 100); }, fromUi: function (v) { return v / 100; },
              fmt: function (v) { return Math.round(v * 100) + "%"; } },
            { el: $("gs-smoothing"), val: $("gs-smoothing-val"), key: "smoothing",
              toUi: function (v) { return Math.round(v * 100); }, fromUi: function (v) { return v / 100; },
              fmt: function (v) { return v <= 0 ? "Off" : Math.round(v * 100) + "%"; } },
            { el: $("gs-jump"), val: $("gs-jump-val"), key: "jumpResponse",
              toUi: function (v) { return Math.round(v * 100); }, fromUi: function (v) { return v / 100; },
              fmt: function (v) { return v <= 0 ? "Off" : Math.round(v * 100) + "%"; } },
            { el: $("gs-speed-limit"), val: $("gs-speed-limit-val"), key: "speedLimitKt",
              toUi: function (v) { return v; }, fromUi: function (v) { return v; },
              fmt: function (v) { return Math.round(v) + " kt"; } },
            { el: $("gs-throttle-limit"), val: $("gs-throttle-limit-val"), key: "throttleLimit",
              toUi: function (v) { return v; }, fromUi: function (v) { return v; },
              fmt: function (v) { return Math.round(v) + "%"; } }
        ];

        var speedRow = $("gs-speed-limit-row");
        var throttleRow = $("gs-throttle-limit-row");
        var curveBtns = document.querySelectorAll("#gs-curve-seg .gs-seg-btn");
        var profileBtns = document.querySelectorAll("#gs-profile-seg .gs-seg-btn");
        var learning = false;          // authoritative: the module itself reports it is listening
        var learnArm = false;          // our learn command is sent but not confirmed by the module yet
        var learnSent = false;         // at least one learn command provably left this panel
        var learnAttempts = 0;         // bounded retries while the bus refuses the command
        var learnPendingAt = 0;        // when the provisional "Listening..." started
        var learnNextAt = 0;           // earliest moment the next retry may leave
        var learnFailedAt = 0;         // when the confirmation window ran out (failure text showing)
        var learnRequestedAt = 0;      // a status already in flight when Learn was clicked must not cancel it
        var activationActive = true;

        var LEARN_CONFIRM_MS = 3000;   // a learn command must produce learning:1 within this window
        var LEARN_FAIL_SHOW_MS = 2500; // how long the failure text stays visible after a timeout
        var LEARN_RETRY_MS = 300;      // pause between send retries
        var LEARN_RETRY_MAX = 8;       // after this many refused sends the panel stops trying

        // The module's learning:1 is the only confirmation. Until it arrives (or the window runs out) the
        // UI shows the provisional "Listening...", so one click is enough even while the first status
        // poll is still on its way.
        function learnActive() { return learning || (learnArm && learnFailedAt === 0); }

        // Writes are guarded (the value is only assigned when it changed): the panel used to rewrite this
        // text and the diag HTML 60 times a second, and that churn made clicks flaky.
        var lastActivationStatus = null, lastLearnLabel = null, lastLearnDl = null;
        // Authoritative activation state, updated from the module's status poll.
        // The learned binding and the armed state come from the module, never
        // from optimistic local state alone (local settings only bridge the gap
        // until the first status arrives).
        var moduleBind = "";          // learned input string as reported by the module ("" = none known)
        var adoptHoldUntil = 0;       // suppresses status adoption right after a Clear/Reset click
        function refreshActivationUi() {
            var listening = learnActive();
            var failing = learnFailedAt !== 0 && (performance.now() - learnFailedAt < LEARN_FAIL_SHOW_MS);
            var text;
            if (els.activationStatus) {
                if (failing) text = "Learn failed - no response from the module";
                else if (listening) text = "Listening for the next MSFS-mapped control...";
                else if (settings.activationEvent === -3)
                    // Learned mode: the binding display comes from the module's
                    // status (moduleBind); the armed state from its gate.
                    text = (moduleBind ? "Button " + moduleBind : "Learned activation button") +
                           (activationActive ? " \u00B7 ON" : " \u00B7 OFF");
                else if (settings.activationEvent === -1)
                    // Mode -1 is "always armed" only when the module really is
                    // (fresh start). After Clear/Reset the module reports the
                    // explicit cleared-off state, which must be shown as such.
                    text = activationActive ? "Automatic (no button configured)"
                                            : "No activation button configured \u00B7 OFF";
                else if (settings.activationEvent === -2) text = "Default brake control" + (activationActive ? ": armed" : ": off");
                else text = "Event " + settings.activationEvent + (activationActive ? ": armed" : ": off");
                if (text !== lastActivationStatus) { els.activationStatus.textContent = text; lastActivationStatus = text; }
            }
            // Clear affordance follows the module's authoritative binding: only a
            // learned binding (reported by the module, or provably just learned)
            // makes Clear actionable. No binding -> greyed out, no command.
            if (els.clearActivation) {
                var hasBind = moduleBind !== "" || settings.activationEvent === -3;
                if (els.clearActivation.disabled !== !hasBind) els.clearActivation.disabled = !hasBind;
                var dis = hasBind ? "false" : "true";
                if (els.clearActivation.getAttribute("data-disabled") !== dis)
                    els.clearActivation.setAttribute("data-disabled", dis);
            }
            if (els.learnActivation) {
                var label = listening ? "Listening..." : "Learn";
                if (label !== lastLearnLabel) { els.learnActivation.textContent = label; lastLearnLabel = label; }
                var dl = String(listening);
                if (dl !== lastLearnDl) { els.learnActivation.setAttribute("data-learning", dl); lastLearnDl = dl; }
            }
        }

        function refreshAllUi() {
            var i;
            for (i = 0; i < toggles.length; i++) setSwitch(toggles[i].el, settings[toggles[i].key]);
            for (i = 0; i < sliders.length; i++) {
                var s = sliders[i];
                if (s.el) s.el.value = String(s.toUi(settings[s.key]));
                if (s.val) s.val.textContent = s.fmt(settings[s.key]);
            }
            if (speedRow) speedRow.style.display = settings.speedLimitOn ? "flex" : "none";
            if (throttleRow) throttleRow.style.display = settings.throttleLimitOn ? "flex" : "none";
            for (i = 0; i < curveBtns.length; i++) {
                curveBtns[i].setAttribute("data-active", String(curveBtns[i].getAttribute("data-value") === settings.curve));
            }
            for (i = 0; i < profileBtns.length; i++) {
                profileBtns[i].setAttribute("data-active", String(parseInt(profileBtns[i].getAttribute("data-value"), 10) === activeProfile));
            }
            refreshActivationUi();
        }

        toggles.forEach(function (t) {
            on(t.el, "click", function () {
                settings[t.key] = !settings[t.key];
                saveSettings();
                refreshAllUi();
            });
        });

        sliders.forEach(function (s) {
            on(s.el, "input", function () {
                settings[s.key] = s.fromUi(parseFloat(s.el.value));
                if (s.val) s.val.textContent = s.fmt(settings[s.key]);
                saveSettings();
            });
        });

        for (var ci = 0; ci < curveBtns.length; ci++) {
            curveBtns[ci].addEventListener("click", function () {
                settings.curve = this.getAttribute("data-value");
                saveSettings();
                refreshAllUi();
            });
        }

        for (var pi = 0; pi < profileBtns.length; pi++) {
            profileBtns[pi].addEventListener("click", function () {
                loadProfile(parseInt(this.getAttribute("data-value"), 10));
                refreshAllUi();
                dirty = true;   // profile switch is a deliberate settings change; tick loop pushes after snapshot
            });
        }

        // Reset needs a second click within 3 s, so it cannot be hit by accident
        var resetArmed = false, resetTimer = null;
        on(els.reset, "click", function () {
            if (!resetArmed) {
                resetArmed = true;
                els.reset.textContent = "Click again to confirm";
                resetTimer = setTimeout(function () { resetArmed = false; els.reset.textContent = "Reset profile"; }, 3000);
            } else {
                clearTimeout(resetTimer);
                resetArmed = false;
                els.reset.textContent = "Reset profile";
                settings = defaults();
                adoptHoldUntil = performance.now() + 600;
                activationActive = false;   // Reset also leaves activation explicitly OFF
                saveSettings();
                refreshAllUi();
                // A confirmed Reset restores the complete factory state, so the
                // learned joystick binding and its saved device identity must not
                // survive either. The panel-local -1 settings push alone cannot do
                // that: while the module is in the learned mode (-3) it deliberately
                // ignores stale activationEvent=-1 pushes, so the explicit clear
                // command is what actually forgets the binding (and persists it).
                sendModuleCommand({ cmd: "clearActivation" }).catch(function () {});
            }
        });

        function sendLearnNow(armVal) {
            // resolves true when the command provably left this panel, false when the bus refused it
            return sendModuleCommand({ cmd: "learn", arm: armVal })
                .then(function () { return true; })
                .catch(function () { return false; });
        }
        // The Learn control is the button plus the text beside it (the label). The label forwards clicks to
        // the button in some hosts and not in others, so all of them are wired and a single physical click
        // still sends exactly one learn command through the shared debounce. Retries only fire while the
        // first command has provably NOT left the panel, so one click can never produce two commands.
        function learnClick() {
            var now = performance.now();
            if (now - learnRequestedAt < 400) return;
            learnRequestedAt = now;
            // A click (re)starts the learn state machine: clear any stale learning=true from an older
            // confirmation so the provisional window, the retries and the confirm timeout apply again.
            // A module that is really listening re-confirms with learning:1 on the next status poll.
            learning = false;
            learnArm = true;
            learnSent = false;
            learnAttempts = 0;
            learnPendingAt = now;
            learnNextAt = now;
            learnFailedAt = 0;
            sendLearnNow(1).then(function (sent) { if (sent) learnSent = true; });
            refreshActivationUi();
        }
        on(els.learnActivation, "click", learnClick);
        on(els.activationStatus, "click", learnClick);
        on(typeof document.querySelector === "function" ? document.querySelector('label[for="gs-learn-activation"]') : null, "click", learnClick);
        on(els.clearActivation, "click", function () {
            if (!moduleBind && settings.activationEvent !== -3) return;   // disabled affordance: no command
            adoptHoldUntil = performance.now() + 600;
            settings.activationEvent = -1;
            activationActive = false;   // cleared = explicit OFF (the module's status confirms)
            learning = false;
            learnArm = false;
            learnSent = false;
            learnFailedAt = 0;
            saveSettings();
            refreshAllUi();
            // Clear must also tell the module to forget the learned binding and
            // device identity. A settings push with activationEvent=-1 alone is
            // deliberately ignored by the module while it is in the learned mode
            // (-3) - a panel that never saw the learning must not be able to clear
            // it by accident - so this command is what really clears the binding
            // (and schedules the cfg save on the module side).
            sendModuleCommand({ cmd: "clearActivation" }).catch(function () {});
        });

        // Helper: show temporary feedback on the Copy Diagnostics button
        function showCopyFeedback(message, success) {
            if (success === undefined) success = true;
            var btn = els.copyDiagnostics;
            if (!btn) return;
            var originalText = btn.textContent;
            var originalBg = btn.style.backgroundColor;
            var originalColor = btn.style.color;
            btn.textContent = message;
            btn.style.backgroundColor = success ? "#28a745" : "#dc3545";
            btn.style.color = "#fff";
            btn.disabled = true;
            setTimeout(function () {
                btn.textContent = originalText;
                btn.style.backgroundColor = originalBg;
                btn.style.color = originalColor;
                btn.disabled = false;
            }, 2000);
        }

        // Helper: show selectable diagnostic text as fallback when clipboard fails
        function showDiagnosticFallback(text) {
            var existing = document.getElementById("gs-diag-fallback");
            if (existing) existing.remove();

            var container = document.createElement("div");
            container.id = "gs-diag-fallback";
            container.style.cssText = "margin-top:8px;padding:8px;background:#fff3cd;border:1px solid #ffc107;border-radius:4px;font-size:11px;color:#856404;";

            var label = document.createElement("div");
            label.style.cssText = "margin-bottom:4px;font-weight:bold;";
            label.textContent = "Clipboard copy failed. Select and copy manually (Ctrl+A, Ctrl+C):";
            container.appendChild(label);

            var textarea = document.createElement("textarea");
            textarea.style.cssText = "width:100%;height:200px;font-family:monospace;font-size:10px;padding:4px;border:1px solid #ccc;border-radius:3px;box-sizing:border-box;background:#fff;";
            textarea.readOnly = true;
            textarea.value = text;
            container.appendChild(textarea);

            var closeBtn = document.createElement("button");
            closeBtn.type = "button";
            closeBtn.style.cssText = "margin-top:4px;padding:4px 8px;font-size:10px;cursor:pointer;";
            closeBtn.textContent = "Close";
            closeBtn.onclick = function () { container.remove(); };
            container.appendChild(closeBtn);

            var btn = els.copyDiagnostics;
            if (btn && btn.parentNode) {
                btn.parentNode.insertBefore(container, btn.nextSibling);
            } else {
                document.body.appendChild(container);
            }

            setTimeout(function () { textarea.select(); }, 0);
        }

        on(els.copyDiagnostics, "click", function () {
            // Gather the diagnostics the module reports in its status and copy
            // them to the clipboard for bug reports. Internal identifiers only;
            // not part of the normal panel flow (this section is hidden by default).
            var parts = [];
            parts.push("GentleStop diagnostics");
            parts.push("module: " + (typeof moduleState !== "undefined" ? moduleState : "unknown"));
            parts.push("simState: " + (typeof simState !== "undefined" ? simState : "unknown"));
            parts.push("simInput: " + (typeof moduleBind !== "undefined" && moduleBind ? moduleBind : ""));
            parts.push("armed: " + (typeof activationActive !== "undefined" ? (activationActive ? "1" : "0") : "?"));
            parts.push("enabled: " + (settings.enabled ? "1" : "0"));
            var report = parts.join("\n");
            
            // Try clipboard with explicit success/failure feedback
            var success = false;
            try {
                if (typeof navigator !== "undefined" && navigator.clipboard && navigator.clipboard.writeText) {
                    navigator.clipboard.writeText(report).then(
                        function () { 
                            success = true; 
                            showCopyFeedback("Diagnostics copied"); 
                        },
                        function () { 
                            showCopyFeedback("Clipboard write failed - select and copy manually", false); 
                        }
                    );
                } else if (typeof window.clipboardData !== "undefined") {
                    window.clipboardData.setData("Text", report);
                    success = true;
                    showCopyFeedback("Diagnostics copied");
                } else {
                    showCopyFeedback("Clipboard API unavailable - select and copy manually", false);
                }
            } catch (e) {
                showCopyFeedback("Clipboard error: " + e.message + " - select and copy manually", false);
            }
            
            // Fallback: if clipboard failed, show selectable text
            if (!success) {
                showDiagnosticFallback(report);
            }
        });
        // Advanced disclosure: expand/collapse the Advanced → Support section.
        on(els.advancedToggle, "click", function () {
            var section = $("gs-advanced-section");
            var expanded = section.style.display !== "none";
            section.style.display = expanded ? "none" : "block";
            els.advancedToggle.textContent = expanded ? "Advanced (close)" : "Advanced";
        });

        refreshAllUi();

        // ---------------------------------------------------------------
        // Link to the GentleStop WASM module through the SDK's Communication Bus.
        // simvar.js is instrument-only and is not available in an in-game toolbar panel.
        // ---------------------------------------------------------------
        var BLOCK_TEXT = ["", "Disabled", "Airborne", "Parking brake", "Strobes on", "Throttle high", "Above speed limit", "Activation button off"];
        var moduleAlive = 0, moduleAliveChangedAt = performance.now(), moduleState = "missing", moduleDbgMsgs = 0, moduleDbgExc = 0;
        var blockCode = 0;
        var commBusListener = null;
        var commBusReady = false;
        var commBusInitPending = false;
        var lastStatusRequest = 0;
        // Startup sync (staging): the first settings push left this panel once
        // the bus listener existed, but in-sim evidence (21 Oct session) showed
        // the module's FIRST received command can be a learn click seconds
        // later - the open-time settings push and early status polls were
        // silently lost (nothing in the CommBus API provably rejects a send).
        // Recovery: re-push settings every second until the module's status
        // response (WASM -> JS broadcast, the only readiness evidence the
        // protocol provides - status is request-driven, nothing else) arrives,
        // bounded to SYNC_MAX attempts, then stop with "not responding".
        var moduleAnswered = false;      // sticky: first handled status response
        var syncAttempts = 0;
        var syncLastAt = 0;
        var SYNC_RETRY_MS = 1000;
        var SYNC_MAX = 20;

        function sendModuleCommand(message) {
            if (!commBusReady || typeof Coherent === "undefined") {
                return Promise.reject(new Error("GentleStop Communication Bus is not ready"));
            }
            // JS -> WASM delivery routes, per host build. In-sim evidence
            // (26 Oct, compat host 'communityfolderfs20'): the legacy
            // listener.call route is rejected by the engine ("COMM_BUS_WASM_
            // CALLBACK not bound"), while Coherent.trigger DELIVERS (panel
            // commands arrive, panelCmds climbs) - and the engine's own
            // error text names trigger as the route to use for an unbound
            // method. Routing decision:
            //   - callWasm (2024 RegisterCommBusListener) primary when the
            //     host offers it;
            //   - legacy listener: Coherent.trigger FIRST (the sim's own
            //     advice); listener.call is now ONLY a last-resort fallback
            //     for a hypothetical host that binds the call route but has
            //     no trigger. This removes the per-send "NoSuchMethod"
            //     debugger error in the compat host without giving up the
            //     2024/legacy send capability.
            // Exactly-once semantics are unchanged: a route is abandoned only
            // on hard evidence that it did NOT deliver (throw / rejection).
            var payload = JSON.stringify(message);
            var triggerRoute = function () {
                if (typeof Coherent.trigger !== "function")
                    return Promise.reject(new Error("GentleStop: no Coherent.trigger route"));
                try {
                    Coherent.trigger("COMM_BUS_WASM_CALLBACK", "GentleStopCommand", payload);
                    return Promise.resolve();
                } catch (e) { return Promise.reject(e); }
            };
            if (commBusListener && typeof commBusListener.callWasm === "function") {
                try {
                    var rw = commBusListener.callWasm("GentleStopCommand", payload);
                    if (rw && typeof rw.then === "function")
                        return rw.then(function () {}, function () {
                            // async rejection: the 2024-native route was
                            // refused by the engine -> single trigger resend
                            return triggerRoute();
                        });
                    return Promise.resolve();
                } catch (e) { /* not routable this way: fall through */ }
            }
            // Legacy listener (2020 + compat hosts): trigger is the engine's
            // recommended (and in-sim proven) route, so it is primary.
            var legacyCallRoute = function () {
                if (typeof commBusListener.call !== "function")
                    return Promise.reject(new Error("GentleStop: no legacy call route"));
                try {
                    var c = commBusListener.call("COMM_BUS_WASM_CALLBACK", "GentleStopCommand", payload);
                    if (c && typeof c.then === "function")
                        return c.then(function () {}, function () {
                            return Promise.reject(new Error("GentleStop: call route rejected"));
                        });
                    return Promise.resolve();
                } catch (e) { return Promise.reject(e); }
            };
            if (commBusListener) {
                if (typeof Coherent.trigger === "function") {
                    try {
                        Coherent.trigger("COMM_BUS_WASM_CALLBACK", "GentleStopCommand", payload);
                        return Promise.resolve();
                    } catch (e) {
                        // trigger itself refused: legacy call route as last resort
                        return legacyCallRoute();
                    }
                }
                return legacyCallRoute();
            }
            return triggerRoute();
        }

        function pushSettings() {
            return sendModuleCommand({
                cmd: "settings",
                enabled: settings.enabled ? 1 : 0,
                invert: settings.invert ? 1 : 0,
                strobeLock: settings.strobeLock ? 1 : 0,
                speedLimitOn: settings.speedLimitOn ? 1 : 0,
                speedLimitKt: settings.speedLimitKt,
                throttleLimitOn: settings.throttleLimitOn ? 1 : 0,
                throttleLimit: settings.throttleLimit,
                deadzone: settings.deadzone,
                curve: settings.curve === "exponential" ? 1 : 0,
                smoothing: settings.smoothing,
                jumpResponse: settings.jumpResponse,
                activationEvent: settings.activationEvent
            }).then(function () { dirty = false; }).catch(function (e) {
                if (commBusReady) moduleState = "Communication Bus error";
                return Promise.reject(e);
            });
        }

        function handleModuleStatus(message) {
            var state = message;
            try { if (typeof message === "string") state = JSON.parse(message); } catch (e) { return; }
            if (!state || typeof state !== "object") return;
            if (!moduleAnswered) moduleAnswered = true;   // readiness: the protocol has no other ack
            brake = Number(state.brake) || 0;
            blockCode = Math.round(Number(state.block) || 0);
            gate = Number(state.gate) > 0.5;
            activationActive = Number(state.activation) > 0.5;
            // Task C: stick display - MSFS reports YOKE Y POSITION forward as
            // negative; the panel negates for the displayed "Stick" value while
            // rawStick keeps the raw sim value.
            stick = -Number(state.stick) || 0;
            rawStick = Number(state.stick) || 0;
            // Task C: panel telemetry - extract sim brake, throttle, and airspeed
            // from the module's status message. These are displayed by the HUD
            // and were previously never parsed, so they remained at zero.
            var stSimBrake = Number(state.simBrake);
            if (isFinite(stSimBrake)) simBrake = stSimBrake;
            var stThrottlePct = Number(state.throttlePct);
            if (isFinite(stThrottlePct)) throttlePct = stThrottlePct;
            var stAirspeedKt = Number(state.airspeedKt);
            if (isFinite(stAirspeedKt)) airspeedKt = stAirspeedKt;
            var moduleLearning = Number(state.learning) > 0.5;
            if (moduleLearning) {
                // Authoritative confirmation: ends the provisional window and the retry machinery.
                learning = true;
                learnArm = false;
                learnSent = false;
                learnAttempts = 0;
                learnFailedAt = 0;
            } else if (!learnArm) {
                // Not pending: the module really is not listening. While learnArm is still true a
                // learning:0 is stale (it was already on its way when our learn command arrived) and it
                // must not cancel the provisional "Listening...".
                learning = false;
            }
            moduleDbgMsgs = Math.round(Number(state.dbgMsgs) || 0);
            moduleDbgExc = Math.round(Number(state.dbgExc) || 0);
            var learnedEvent = Number(state.activationEvent);
            if (isFinite(learnedEvent) && learnedEvent >= -3 && Math.floor(learnedEvent) !== settings.activationEvent &&
                performance.now() >= adoptHoldUntil) {
                // -3 (learned mode) must be adopted like any other module state:
                // without it the panel used to keep its local -1 and show
                // "Always armed (no button learned)" while a binding was live.
                // adoptHoldUntil suppresses one stale poll right after Clear/Reset.
                settings.activationEvent = Math.floor(learnedEvent);
                storeSet(PROFILE_KEY + activeProfile, JSON.stringify(settings));
            }
            // Authoritative learned binding (the module sends it with every status).
            var reportedBind = typeof state.simInput === "string" ? state.simInput : "";
            if (reportedBind !== moduleBind) moduleBind = reportedBind;
            // ---- Configuration snapshot: module is authoritative source of truth ----
            var snapshotSeq = Number(state.settingsSeq);
            if (isFinite(snapshotSeq)) {
                var applySnapshot = false;
                if (!snapshotApplied) {
                    // First snapshot: adopt the module's persisted configuration.
                    applySnapshot = true;
                } else if (!dirty && snapshotSeq > lastAppliedSeq) {
                    // Module has newer settings (e.g. after restart reload): adopt,
                    // but only when the panel has no pending user edits.
                    applySnapshot = true;
                }
                if (applySnapshot) {
                    settings.enabled         = Number(state.enabled) !== 0;
                    settings.invert          = Number(state.invert) !== 0;
                    settings.strobeLock      = Number(state.strobeLock) !== 0;
                    settings.speedLimitOn    = Number(state.speedLimitOn) !== 0;
                    settings.speedLimitKt    = Number(state.speedLimitKt);
                    settings.throttleLimitOn = Number(state.throttleLimitOn) !== 0;
                    settings.throttleLimit   = Number(state.throttleLimit);
                    settings.deadzone        = Number(state.deadzone);
                    settings.curve           = Number(state.curve) > 0.5 ? "exponential" : "linear";
                    settings.smoothing       = Number(state.smoothing);
                    settings.jumpResponse    = Number(state.jumpResponse);
                    if (typeof state.activationCleared !== "undefined") {
                        // activationCleared is module-only, not in panel settings; track via activationEvent
                    }
                    settings.activationEvent = Math.floor(Number(state.activationEvent));
                    storeSet(PROFILE_KEY + activeProfile, JSON.stringify(settings));
                    snapshotApplied = true;
                    lastAppliedSeq = snapshotSeq;
                    refreshAllUi();
                }
            }
            var alive = Number(state.alive) || 0;
            var now = performance.now();
            if (alive !== moduleAlive) { moduleAlive = alive; moduleAliveChangedAt = now; }
            if (alive === 0) moduleState = "not loaded";
            else if (alive < 0) moduleState = "init: unknown marker " + alive;
            else moduleState = "running";
            refreshActivationUi();
        }

        function initCommBus() {
            if (commBusReady || commBusInitPending) return;
            // MSFS 2024 panel<->WASM CommBus: the documented registration is
            // RegisterCommBusListener(); the legacy 2020 pattern is
            // RegisterViewListener("JS_LISTENER_COMM_BUS"). Either object only
            // needs .on for receiving; sending prefers .callWasm (2024) then
            // .call then a bare Coherent.trigger. Both registration APIs are
            // tried so this works on both sim generations.
            var registerFns = [
                (typeof RegisterCommBusListener === "function") ? RegisterCommBusListener : null,
                (typeof RegisterViewListener === "function") ? function () { return RegisterViewListener("JS_LISTENER_COMM_BUS"); } : null
            ];
            registerFns = registerFns.filter(Boolean);
            if (registerFns.length === 0) {
                moduleState = "Communication Bus unavailable";
                setTimeout(initCommBus, 250);
                return;
            }
            commBusInitPending = true;
            for (var i = 0; i < registerFns.length; i++) {
                try {
                    commBusListener = registerFns[i]();
                    if (!commBusListener || typeof commBusListener.on !== "function") throw new Error("listener unavailable");
                    commBusListener.on("GentleStopStatus", handleModuleStatus);
                    commBusReady = true;
                    commBusInitPending = false;
                    // Request the module's persisted configuration as the
                    // authoritative snapshot. The panel applies it before
                    // enabling settings synchronization, so a panel whose store
                    // was lost cannot overwrite the module's persisted values.
                    sendModuleCommand({ cmd: "configSnapshot" }).catch(function () {});
                    return;
                } catch (e) {
                    commBusListener = null;   // try the next registration API
                }
            }
            commBusInitPending = false;
            moduleState = "Communication Bus unavailable";
            setTimeout(initCommBus, 250);
        }

        // ---------------------------------------------------------------
        // HUD
        // ---------------------------------------------------------------
        function pct(v) { return Math.round(v * 100); }

        // Brake-bar display interpolation. The module computes a fully-smoothed
        // brake value every sim frame and reports it with 6 decimals, but the
        // status poll delivers snapshots only every 250 ms - snapping the bars
        // to each snapshot (plus integer-per-cent rounding and a 0.05 s width
        // transition) produced the visible steps. The displayed value now
        // animates every frame toward the latest authoritative snapshot
        // (gsbrakedisplay.js), in sync with the real brake transient and quick
        // to settle exactly at 0 when braking stops. The real braking logic is
        // untouched: this only redraws what the module already commanded.
        var GSMath = (typeof GentleStopDisplayMath !== "undefined" && GentleStopDisplayMath) ||
                     { step: function (shown, target) { return target; } };   // file missing: previous snap behavior
        // The real module exports barStep (the "step" alias also exists); call
        // barStep explicitly so a stub without the alias still works.
        var gsBarStep = GSMath.barStep || GSMath.step;
        var barShown = 0.0;

        // Per-frame write caches: 60 fps ticks must not touch the DOM when nothing changed.
        var lastFillW = null, lastVals = null, lastStatusText = null, lastStatusActive = null, lastDiag = null;

        function updateHud(dtSec) {
            barShown = gsBarStep(barShown, brake, dtSec);
            var w = (Math.round(barShown * 10000) / 100).toFixed(2) + "%";
            if (lastFillW !== w) {
                if (els.fillL) els.fillL.style.width = w;
                if (els.fillR) els.fillR.style.width = w;
                lastFillW = w;
            }
            var tv = pct(barShown) + "%";
            if (lastVals !== tv) {
                if (els.valL) els.valL.textContent = tv;
                if (els.valR) els.valR.textContent = tv;
                lastVals = tv;
            }

            var text;
            if (moduleState === "not loaded") text = "Module not loaded";
            else if (moduleState.indexOf("init") === 0) text = "Module " + moduleState;
            else if (moduleState !== "running") text = "Module idle";
            else if (blockCode > 0 && BLOCK_TEXT[blockCode]) text = BLOCK_TEXT[blockCode];
            else text = brake > 0.01 ? "Braking" : "Armed";
            if (lastStatusText !== text) { els.status.textContent = text; lastStatusText = text; }
            var actv = String(gate && moduleState === "running");
            if (lastStatusActive !== actv) { els.status.setAttribute("data-active", actv); lastStatusActive = actv; }

            if (els.diag) {
                var diag = "Stick: <b>" + pct(stick) + "%</b> (raw " + pct(rawStick) + "%) · Sim brake: <b>" + pct(simBrake) + "%</b><br>" +
                    "Throttle: <b>" + Math.round(throttlePct) + "%</b> · Speed: <b>" + Math.round(airspeedKt) + " kt</b><br>" +
                    "Module: <b>" + moduleState + "</b> (msgs " + moduleDbgMsgs + ", exc " + moduleDbgExc + ") · Storage: <b>" + storageKind + "</b>";
                if (diag !== lastDiag) { els.diag.innerHTML = diag; lastDiag = diag; }
            }
            refreshActivationUi();
        }

        var lastTick = 0;
        // Recoverable tick errors (staging): a throw inside the loop body used
        // to kill the loop entirely - requestAnimationFrame(tick) is the LAST
        // statement, so every status poll, settings sync, learn retry and the
        // HUD died with it (in-sim evidence: 'GSMath.step is not a function').
        // Ticking continues after a recoverable error; each distinct error is
        // recorded once (test hook + overlay) - never silently hidden.
        var tickErrors = [];
        window.__gsTickErrors = tickErrors;
        function noteTickError(e) {
            var msg = (e && (e.message || String(e))) || "unknown tick error";
            var counted = 0;
            for (var i = 0; i < tickErrors.length; i++) if (tickErrors[i].msg === msg) { tickErrors[i].count++; counted = tickErrors[i].count; }
            if (!counted) tickErrors.push({ msg: msg, count: 1 });
            if (typeof window.__gsDebugLog === "function")
                window.__gsDebugLog("TICK err" + (counted ? " x" + (counted + 1) : "") + ": " + msg);
        }
        function tick() {
            if (!running) return;
            var now = performance.now();
            var dt = lastTick ? (now - lastTick) / 1000 : 0;
            lastTick = now;
            try {
            if (commBusReady && now - lastStatusRequest >= 250) {
                lastStatusRequest = now;
                sendModuleCommand({ cmd: "status" }).catch(function () {
                    moduleState = "Communication Bus error";
                });
            }
            // Dirty-state settings push: only deliberate user edits are sent to
            // the module, and only after the config snapshot has been applied.
            // This prevents a panel whose store was lost from overwriting the
            // module's persisted configuration with defaults at startup.
            if (commBusReady && snapshotApplied && dirty) {
                pushSettings().catch(function () {});
            }
            // Bounded retry loop for an unconfirmed learn command. It runs in this tick loop (not in
            // setTimeout) so the tests can drive it with the fake clock, and it is bounded both in time
            // (LEARN_CONFIRM_MS) and in attempts (LEARN_RETRY_MAX).
            if (learnArm && !learning) {
                if (now - learnPendingAt >= LEARN_CONFIRM_MS) {
                    // No learning:1 confirmation in time: stop pending, show the failure text and
                    // retract the learn request (best effort) so the panel and the module agree.
                    learnArm = false;
                    learnFailedAt = now;
                    sendLearnNow(0).catch(function () {});
                    refreshActivationUi();
                } else if (!learnSent && learnAttempts < LEARN_RETRY_MAX && now >= learnNextAt) {
                    learnNextAt = now + LEARN_RETRY_MS;
                    learnAttempts++;
                    sendLearnNow(1).then(function (sent) { if (sent) learnSent = true; });
                }
            }
            if (learnFailedAt !== 0 && now - learnFailedAt >= LEARN_FAIL_SHOW_MS) learnFailedAt = 0;
            } catch (e) { noteTickError(e); }
            try { updateHud(dt); } catch (e) { noteTickError(e); }
            requestAnimationFrame(tick);       // always re-armed: a recoverable
        }                                      // error cannot stop the loop

        initCommBus();                  // hand the active profile to the module once the bus listener is ready
        requestAnimationFrame(tick);

        window.addEventListener("beforeunload", function () { running = false; });

        console.log("GentleStop panel initialized (braking runs in the WASM module).");
    }

    window.__gentleStopInit = init;
})();

if (typeof TemplateElement === "function") {
    class IngamePanelGentleStop extends TemplateElement {
        constructor() {
            super();
            this.started = false;
        }
        connectedCallback() {
            super.connectedCallback();
            this.initialize();
        }
        initialize() {
            if (this.started) return;
            this.started = true;
            window.__gentleStopInit();
        }
    }
    window.customElements.define("ingamepanel-custom", IngamePanelGentleStop);
} else {
    console.error("GentleStop: TemplateElement missing (/JS/common.js not loaded?)");
}

if (typeof checkAutoload === "function") {
    checkAutoload();
}
