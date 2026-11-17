const fs = require("fs");
const path = require("path");
const frames = [], els = {}, commands = [], listeners = {}, docEvents = {};
let clock = 1000, domCreates = 0, domBoxAppends = 0, lastCreated = null;
const mkEl = () => ({ style: {}, _a: {}, _events: {}, textContent: "", innerHTML: "", value: "0",
  addEventListener(ev, fn) { this._events[ev] = fn; },
  click() { if (this._events.click) this._events.click({}); },
  setAttribute(k, v) { this._a[k] = v; }, getAttribute(k) { return this._a[k] || ""; } });
global.document = {
  getElementById: id => els[id] || (els[id] = mkEl()),
  querySelectorAll: () => [],
  addEventListener(ev, fn) { docEvents[ev] = fn; },
  createElement: () => { domCreates++; lastCreated = { style: {}, textContent: "", appendChild() {} }; return lastCreated; },
  body: { appendChild() { domBoxAppends++; } }
};
// gsdebug.js opt-in refresh must not keep the node process alive under test
global.setInterval = () => 0;
global.window = global; global.addEventListener = function () {};
global.localStorage = { _d: {}, getItem(k) { return this._d[k] || null; }, setItem(k, v) { this._d[k] = v; } };
global.performance = { now: () => clock };
global.requestAnimationFrame = f => { frames.push(f); };
let triggerFails = false, callFails = false, wasmFails = false;
// Route switch: GS_TEST_ROUTE=legacy runs the 2020 RegisterViewListener mock,
// GS_TEST_ROUTE=new runs the 2024 RegisterCommBusListener/callWasm mock (all
// commands must then use the documented .callWasm route).
const TEST_ROUTE = process.env.GS_TEST_ROUTE || "legacy";
// The panel sources to evaluate. Defaults to the canonical public package
// (gentlestop-stage-panel/) when the test is run directly (no argv[2]); the
// parent process passes the panel source explicitly. Sibling files are
// resolved from the panel source's own directory, so an explicit path works
// from any current directory and never needs a private staging/ folder.
const repoRoot = path.resolve(__dirname, "..", "..");
const defaultPanelJs = path.join(repoRoot, "gentlestop-stage-panel", "html_ui",
  "InGamePanels", "GentleStopToolbarControl", "GentleStop.js");
const PANEL_JS = process.argv[2] ? path.resolve(process.argv[2]) : defaultPanelJs;
const PANEL_DIR = path.dirname(PANEL_JS);
const DISPLAY_JS = path.join(PANEL_DIR, "gsbrakedisplay.js");
const DEBUG_JS = path.join(PANEL_DIR, "gsdebug.js");
for (const required of [PANEL_JS, DISPLAY_JS, DEBUG_JS]) {
  if (!fs.existsSync(required)) {
    process.stderr.write("PANEL TEST SETUP FAILED: missing canonical panel file: " + required + "\n");
    process.exit(2);
  }
}
const useCallWasm = TEST_ROUTE === "new" ? 1 : 0;
const UNBOUND = TEST_ROUTE === "legacy-unbound";   // sim-host case: the legacy
// listener exists but the engine ASYNCHRONOUSLY rejects the call route (the
// 'COMM_BUS_WASM_CALLBACK not bound' CoherentError); the panel must deliver
// every command exactly once via the trigger fallback.
const dbg = (...x) => process.stderr.write("DBG " + x.map(v => JSON.stringify(v)).join(" ") + "\n");
// The panel loads the REAL display-math module in the sim (script tag), the
// test must exercise that same code, not a mock.
const gsBrake = require(DISPLAY_JS);
global.GentleStopDisplayMath = gsBrake;
// MSFS 2024-documented registration; when useCallWasm the panel must take this route.
global.RegisterCommBusListener = function () {
  if (useCallWasm === 0) throw new Error("API unavailable on this build");
  return {
    on(event, fn) {
      if (event !== "GentleStopStatus") throw new Error("unexpected status subscription");
      listeners[event] = fn;
    },
    callWasm(event, payload) {
      if (event !== "GentleStopCommand") throw new Error("unexpected wasm call");
      if (wasmFails) throw new Error("Communication Bus refuses the wasm call");
      commands.push(JSON.parse(payload));
      return Promise.resolve();
    }
  };
};
global.RegisterViewListener = name => {
  if (useCallWasm === 1) throw new Error("legacy API should not be used when the new one exists");
  if (name !== "JS_LISTENER_COMM_BUS") throw new Error("unexpected listener: " + name);
  return {
    on(event, fn) { listeners[event] = fn; },
    call(method, event, payload) {
      window.__gsCallSends = (window.__gsCallSends || 0) + 1;
      if (method !== "COMM_BUS_WASM_CALLBACK" || event !== "GentleStopCommand") throw new Error("unexpected bus call");
      if (UNBOUND)   // the sim host: async rejection, nothing delivered
        return Promise.reject(new Error("NoSuchMethod: COMM_BUS_WASM_CALLBACK not bound. Call engine.trigger instead of engine.call if you do not bind nor need the result."));
      if (callFails) throw new Error("Communication Bus refuses the call");   // sync: route absent -> caller falls back
      commands.push(JSON.parse(payload));   // same bus as trigger: one logical send
      return undefined;   // (a real Coherent call returns a promise; test does not await it)
    }
  };
};
global.Coherent = { trigger(method, event, payload) {
  if (method !== "COMM_BUS_WASM_CALLBACK" || event !== "GentleStopCommand") throw new Error("unexpected bus trigger");
  if (triggerFails) {
    window.__gsTriggerRefusals = (window.__gsTriggerRefusals || 0) + 1;
    throw new Error("Communication Bus refuses the command");
  }
  commands.push(JSON.parse(payload));
  window.__gsTriggerSends = (window.__gsTriggerSends || 0) + 1;
} };
global.TemplateElement = undefined; global.console = { log() {}, warn() {}, error() {} };

let ok = true;
async function runScenario() {
  for (const k of Object.keys(listeners)) delete listeners[k];
  localStorage._d = {};
  commands.length = 0; frames.length = 0;
  if (els["GentleStopRoot"]) delete els["GentleStopRoot"].__gsWired;   // simulate a reopened panel (fresh elements)
  eval(fs.readFileSync(PANEL_JS, "utf8"));
  window.__gentleStopInit();
  const flush = () => new Promise(resolve => setImmediate(resolve));
  await flush(); await flush();
  const chk = (c, m) => { if (!c) { ok = false; process.stderr.write("[" + TEST_ROUTE + "] FAIL: " + m + "\n"); } };
  const learnArm1 = () => commands.filter(c => c.cmd === "learn" && c.arm === 1).length;
  const run = async () => { clock += 250; const f = frames.pop(); f(); await flush(); };
  const runN = async n => { for (let i = 0; i < n; i++) await run(); };
  const emit = state => listeners.GentleStopStatus(JSON.stringify(state));
  const initial = commands[0];
  chk(initial && initial.cmd === "configSnapshot", "panel did not send initial configSnapshot via CommBus");
  // Stage A regression: the real display-math module (the one the panel evals
  // against) exports barStep with a step alias, and the tick loop starts clean.
  chk(gsBrake && typeof gsBrake.barStep === "function" && gsBrake.step === gsBrake.barStep,
      "gsbrakedisplay must export barStep plus an aliased step: got " + Object.keys(gsBrake || {}).join(","));
  chk(Array.isArray(window.__gsTickErrors) && window.__gsTickErrors.length === 0,
      "tick loop recorded errors on startup: " + JSON.stringify(window.__gsTickErrors || null));
  if (UNBOUND) chk(window.__gsTriggerSends === commands.length, "initial push was not delivered exactly once via the trigger fallback");
  // Overlay (gsdebug.js) is opt-in: in the NORMAL configuration it must not
  // create or render anything; with gsDebug=1 (localStorage) it must.
  const gsDebugSrc = fs.readFileSync(DEBUG_JS, "utf8");
  domCreates = 0; domBoxAppends = 0; lastCreated = null;
  eval(gsDebugSrc);                                   // normal configuration
  (docEvents.DOMContentLoaded || (() => {}))();
  chk(domCreates === 0 && domBoxAppends === 0, "yellow DIAG overlay was created in the normal configuration");
  const hiddenLog = window.__gsDebugLog;
  hiddenLog("hidden diag probe");                     // collection still works
  hiddenLog("hidden diag probe");                     // ...but stays hidden
  chk(domBoxAppends === 0, "hidden overlay rendered visible content");
  localStorage._d["gsDebug"] = "1";
  eval(gsDebugSrc);                                   // opt-in configuration
  window.__gsDebugLog("hidden diag probe");           // the opt-in closure renders it
  chk(domBoxAppends >= 1 && lastCreated && lastCreated.textContent.indexOf("GS DIAG STEP 2") !== -1 &&
      lastCreated.textContent.indexOf("hidden diag probe") !== -1,
      "opt-in (gsDebug=1) did not show the DIAG overlay");
  delete localStorage._d["gsDebug"];
  // Config snapshot: module's persisted config is authoritative. Panel applies
  // it on first receipt and only sends deliberate user edits afterwards.
  emit({
    alive: 5, brake: 0, block: 0, gate: 0, activation: 1, learning: 0,
    activationEvent: -1, dbgMsgs: 2, dbgExc: 0,
    // full configuration snapshot
    enabled: 0, invert: 1, strobeLock: 1, speedLimitOn: 1, speedLimitKt: 120,
    throttleLimitOn: 0, throttleLimit: 30, deadzone: 0.25, curve: 1,
    smoothing: 0.6, jumpResponse: 0.4, activationCleared: 0, settingsSeq: 7
  });
  await run();
  // Panel applied the snapshot: settings match module values, not defaults
  chk(els["gs-enabled-toggle"].getAttribute("data-enabled") === "false", "snapshot: enabled not applied");
  chk(els["gs-invert-toggle"].getAttribute("data-enabled") === "true", "snapshot: invert not applied");
  chk(els["gs-deadzone"].value === "25", "snapshot: deadzone not applied");
  chk(els["gs-throttle-limit"].value === "30", "snapshot: throttleLimit not applied");
  chk(els["gs-smoothing"].value === "60", "snapshot: smoothing not applied in panel state");
  // Task C: Stick display convention - forward stick shows positive.
  // MSFS reports YOKE Y POSITION forward as negative; the panel negates only
  // the displayed "Stick" value while rawStick keeps the raw sim value.
  emit({ alive: 5, brake: 0, block: 0, gate: 0, activation: 1, learning: 0,
         activationEvent: -1, dbgMsgs: 2, dbgExc: 0, stick: -0.5 });
  await run();
  chk(els["gs-diag"].innerHTML.indexOf("Stick: <b>50%</b>") !== -1,
      "stick display: forward stick shows positive (got: " + els["gs-diag"].innerHTML + ")");
  chk(els["gs-diag"].innerHTML.indexOf("raw -50%") !== -1,
      "stick raw keeps the sim value (forward negative) (got: " + els["gs-diag"].innerHTML + ")");
  // Task C: Panel telemetry extraction from status message
  emit({ alive: 5, brake: 0, block: 0, gate: 0, activation: 1, learning: 0,
         activationEvent: -1, dbgMsgs: 2, dbgExc: 0,
         simBrake: 0.25, throttlePct: 50.0, airspeedKt: 123.4 });
  await run();
  chk(els["gs-diag"].innerHTML.indexOf("Sim brake: <b>25%</b>") !== -1,
      "sim brake display from status (got: " + els["gs-diag"].innerHTML + ")");
  chk(els["gs-diag"].innerHTML.indexOf("Throttle: <b>50%</b>") !== -1,
      "throttle display from status (got: " + els["gs-diag"].innerHTML + ")");
  chk(els["gs-diag"].innerHTML.indexOf("Speed: <b>123 kt</b>") !== -1,
      "airspeed display from status (got: " + els["gs-diag"].innerHTML + ")");
  // Legitimate zero payload updates displayed values to zero
  emit({ alive: 5, brake: 0, block: 0, gate: 0, activation: 1, learning: 0,
         activationEvent: -1, dbgMsgs: 2, dbgExc: 0,
         simBrake: 0, throttlePct: 0, airspeedKt: 0 });
  await run();
  chk(els["gs-diag"].innerHTML.indexOf("Sim brake: <b>0%</b>") !== -1,
      "sim brake zero update (got: " + els["gs-diag"].innerHTML + ")");
  chk(els["gs-diag"].innerHTML.indexOf("Throttle: <b>0%</b>") !== -1,
      "throttle zero update (got: " + els["gs-diag"].innerHTML + ")");
  chk(els["gs-diag"].innerHTML.indexOf("Speed: <b>0 kt</b>") !== -1,
      "airspeed zero update (got: " + els["gs-diag"].innerHTML + ")");
  // Nonzero payload before missing-fields test, so a faulty reset-to-zero
  // implementation would be caught (values should be preserved, not reset).
  emit({ alive: 5, brake: 0, block: 0, gate: 0, activation: 1, learning: 0,
         activationEvent: -1, dbgMsgs: 2, dbgExc: 0,
         simBrake: 0.4, throttlePct: 65, airspeedKt: 145 });
  await run();
  chk(els["gs-diag"].innerHTML.indexOf("Sim brake: <b>40%</b>") !== -1,
      "sim brake nonzero pre-check (got: " + els["gs-diag"].innerHTML + ")");
  chk(els["gs-diag"].innerHTML.indexOf("Throttle: <b>65%</b>") !== -1,
      "throttle nonzero pre-check (got: " + els["gs-diag"].innerHTML + ")");
  chk(els["gs-diag"].innerHTML.indexOf("Speed: <b>145 kt</b>") !== -1,
      "airspeed nonzero pre-check (got: " + els["gs-diag"].innerHTML + ")");
  // Missing fields do not produce NaN or break the handler; previous values are preserved.
  emit({ alive: 5, brake: 0, block: 0, gate: 0, activation: 1, learning: 0,
         activationEvent: -1, dbgMsgs: 2, dbgExc: 0 });
  await run();
  chk(els["gs-diag"].innerHTML.indexOf("Sim brake: <b>40%</b>") !== -1,
      "missing simBrake keeps last valid value (got: " + els["gs-diag"].innerHTML + ")");
  chk(els["gs-diag"].innerHTML.indexOf("Throttle: <b>65%</b>") !== -1,
      "missing throttlePct keeps last valid value (got: " + els["gs-diag"].innerHTML + ")");
  chk(els["gs-diag"].innerHTML.indexOf("Speed: <b>145 kt</b>") !== -1,
      "missing airspeedKt keeps last valid value (got: " + els["gs-diag"].innerHTML + ")");
  // Non-finite values are rejected. Invoke the handler directly with a JS object
  // (bypassing JSON.stringify, which coerces NaN/Infinity to null) so the
  // defensive isFinite guard is genuinely exercised.
  listeners.GentleStopStatus({ alive: 5, brake: 0, block: 0, gate: 0, activation: 1, learning: 0,
                               activationEvent: -1, dbgMsgs: 2, dbgExc: 0,
                               simBrake: NaN, throttlePct: Infinity, airspeedKt: -Infinity });
  await run();
  chk(els["gs-diag"].innerHTML.indexOf("Sim brake: <b>40%</b>") !== -1,
      "non-finite simBrake rejected (got: " + els["gs-diag"].innerHTML + ")");
  chk(els["gs-diag"].innerHTML.indexOf("Throttle: <b>65%</b>") !== -1,
      "non-finite throttlePct rejected (got: " + els["gs-diag"].innerHTML + ")");
  chk(els["gs-diag"].innerHTML.indexOf("Speed: <b>145 kt</b>") !== -1,
      "non-finite airspeedKt rejected (got: " + els["gs-diag"].innerHTML + ")");
  // A subsequent valid status message updates the display again
  emit({ alive: 5, brake: 0, block: 0, gate: 0, activation: 1, learning: 0,
         activationEvent: -1, dbgMsgs: 2, dbgExc: 0,
         simBrake: 0.75, throttlePct: 87.5, airspeedKt: 210.0 });
  await run();
  chk(els["gs-diag"].innerHTML.indexOf("Sim brake: <b>75%</b>") !== -1,
      "sim brake re-update (got: " + els["gs-diag"].innerHTML + ")");
  chk(els["gs-diag"].innerHTML.indexOf("Throttle: <b>88%</b>") !== -1,
      "throttle re-update (got: " + els["gs-diag"].innerHTML + ")");
  chk(els["gs-diag"].innerHTML.indexOf("Speed: <b>210 kt</b>") !== -1,
      "airspeed re-update (got: " + els["gs-diag"].innerHTML + ")");
  // No settings push happens without dirty: snapshot applied, nothing to send
  const nSettingsPre = commands.filter(c => c.cmd === "settings").length;
  chk(nSettingsPre === 0, "panel pushed settings before any user edit (snapshot should prevent overwrite)");
  // Status polling continues normally
  chk(commands.filter(c => c.cmd === "status").length >= 1, "status polling stopped after snapshot");
  // Dirty-state push: a user toggle sets dirty, tick loop pushes settings
  els["gs-enabled-toggle"].click(); await flush(); await flush();
  const settingsCommands = commands.filter(c => c.cmd === "settings");
  chk(settingsCommands.length === 1, "user toggle did not trigger settings push");
  chk(settingsCommands[settingsCommands.length - 1].enabled === 1, "pushed settings do not reflect user toggle");
  // After successful push, dirty is cleared: no repeat push on subsequent ticks
  await runN(4);
  chk(commands.filter(c => c.cmd === "settings").length === settingsCommands.length, "settings kept pushing after successful push (dirty not cleared)");
  els["gs-learn-activation"].click(); await flush(); await flush();
  chk(learnArm1() === 1, "learn command was not sent through CommBus");
  chk(els["gs-learn-activation"].textContent === "Listening...", "first click did not show Listening: " + els["gs-learn-activation"].textContent);
  chk(els["gs-learn-activation"].textContent === "Listening...", "first click did not show Listening: " + els["gs-learn-activation"].textContent);
  await run(); chk(commands.some(c => c.cmd === "status"), "panel did not request module status");
  // a status that was already in flight when Learn was clicked still says learning:0 and must not cancel "Listening..."
  emit({ alive: 4, brake: 0, block: 0, gate: 0, activation: 1, learning: 0, activationEvent: -1, dbgMsgs: 2, dbgExc: 0 });
  await runN(3);   // 750 ms, well past the old 400 ms display hold; the provisional window is 3 s now
  chk(els["gs-activation-status"].textContent === "Listening for the next MSFS-mapped control...", "stale status hid Listening: " + els["gs-activation-status"].textContent);
  // After the first response the dirty-state loop is armed (no automatic pushes).
  const nSettingsAnswered = commands.filter(c => c.cmd === "settings").length;
  await runN(8);
  chk(commands.filter(c => c.cmd === "settings").length === nSettingsAnswered, "module answered but settings kept pushing without user edit");
  // Stage A: a recoverable HUD error is caught, counted (deduped by message)
  // and the loop keeps running - polls continue, the bars snap to the target.
  const nStatusPre = commands.filter(c => c.cmd === "status").length;
  const realRound = Math.round;
  await runN(1);
  const errsBefore = window.__gsTickErrors.length;
  Math.round = () => { throw new Error("simulated HUD failure"); };   // updateHud cannot count
  await runN(3);
  Math.round = realRound;
  const tickErrs = window.__gsTickErrors;
  chk(tickErrs.length === errsBefore + 1 && tickErrs[tickErrs.length - 1].count === 3,
      "recoverable HUD error not caught+counted: " + JSON.stringify(tickErrs));
  const nStatusPost = commands.filter(c => c.cmd === "status").length;
  chk(nStatusPost >= nStatusPre + 4, "tick loop died after a recoverable HUD error (" + (nStatusPost - nStatusPre) + " polls in 4 frames)");
  await runN(1);
  // learning:1 is the authoritative confirmation and keeps the listener display
  emit({ alive: 5, brake: 0, block: 0, gate: 0, activation: 1, learning: 1, activationEvent: -1, dbgMsgs: 3, dbgExc: 0 });
  await run(); chk(els["gs-activation-status"].textContent === "Listening for the next MSFS-mapped control...", "confirmed learning dropped Listening: " + els["gs-activation-status"].textContent);
  emit({ alive: 6, brake: 0.42, block: 0, gate: 1, activation: 1, learning: 0, activationEvent: 4242, dbgMsgs: 3, dbgExc: 0 });
  await run(); chk(els["gs-status"].textContent === "Braking", "status braking: " + els["gs-status"].textContent);
  chk(els["gs-brake-value-left"].textContent === "42%", "HUD value");
  chk(els["gs-activation-status"].textContent.indexOf("Event 4242") !== -1, "learned event was not shown");
  emit({ alive: 7, brake: 0, block: 5, gate: 0, activation: 1, learning: 0, activationEvent: 4242, dbgMsgs: 4, dbgExc: 0 });
  await run(); chk(els["gs-status"].textContent === "Throttle high", "status blocked: " + els["gs-status"].textContent);
  // -2 is the learned "default brake control" (brake pressure), not "always armed"
  emit({ alive: 8, brake: 0, block: 0, gate: 0, activation: 1, learning: 0, activationEvent: -2, dbgMsgs: 5, dbgExc: 0 });
  await run(); chk(els["gs-activation-status"].textContent === "Default brake control: armed", "-2 armed label: " + els["gs-activation-status"].textContent);
  emit({ alive: 9, brake: 0, block: 0, gate: 0, activation: 0, learning: 0, activationEvent: -2, dbgMsgs: 6, dbgExc: 0 });
  await run(); chk(els["gs-activation-status"].textContent === "Default brake control: off", "-2 off label: " + els["gs-activation-status"].textContent);
  emit({ alive: 10, brake: 0, block: 0, gate: 0, activation: 1, learning: 1, activationEvent: -2, dbgMsgs: 7, dbgExc: 0 });
  await run(); chk(els["gs-activation-status"].textContent === "Listening for the next MSFS-mapped control...", "learn listening label: " + els["gs-activation-status"].textContent);
  // one physical click (the label forwards to the button) must send exactly one learn command
  clock += 500;
  const nLearn = learnArm1();
  els["gs-learn-activation"].click(); await flush(); await flush();
  els["gs-learn-activation"].click(); await flush(); await flush();   // within 400 ms: swallowed
  dbg("dblclick timeline", commands.slice(-8).map(c => c.cmd + (c.arm !== undefined ? ":" + c.arm : "")));
  chk(learnArm1() === nLearn + 1, "double click sent duplicate learn commands");
  // clicking the text beside the button (the label row) must start learning too
  clock += 500;
  const nLearn2 = learnArm1();
  els["gs-activation-status"].click(); await flush(); await flush();
  chk(learnArm1() === nLearn2 + 1, "clicking the activation text did not start learning");

  // two deliberate clicks 800 ms apart must both reach the bus (the 400 ms dedupe only swallows rapid ones)
  clock += 800;
  const nDeliberate = learnArm1();
  els["gs-learn-activation"].click(); await flush(); await flush();
  chk(learnArm1() === nDeliberate + 1, "second deliberate click did not reach the bus");

  // P1: a click while the bus refuses commands must retry until exactly one command gets through
  // Before the refusal scenarios every send used the primary routes only:
  chk(!window.__gsCallSends, "legacy call route was invoked while trigger was healthy (recreates the debugger error)");
  clock += 800; triggerFails = callFails = wasmFails = true;
  const nRefused = learnArm1();
  els["gs-learn-activation"].click(); await flush(); await flush();
  chk(learnArm1() === nRefused, "a refused click still logged a command");
  await runN(3);                                   // 750 ms of refusals
  chk(learnArm1() === nRefused, "refused retry attempts logged commands");
  chk(els["gs-activation-status"].textContent === "Listening for the next MSFS-mapped control...", "provisional Listening lost during refusals: " + els["gs-activation-status"].textContent);
  triggerFails = false;
  await runN(3);                                   // a retry slot passes: one command gets through
  chk(learnArm1() === nRefused + 1, "retry did not deliver exactly one learn command");
  const confirmedCb = els["gs-activation-status"]; 
  emit({ alive: 11, brake: 0, block: 0, gate: 0, activation: 1, learning: 1, activationEvent: -2, dbgMsgs: 8, dbgExc: 0 });
  await run(); chk(confirmedCb.textContent === "Listening for the next MSFS-mapped control...", "retried learn was not confirmed: " + confirmedCb.textContent);
  emit({ alive: 12, brake: 0, block: 0, gate: 0, activation: 1, learning: 0, activationEvent: -2, dbgMsgs: 9, dbgExc: 0 });
  await run(); chk(confirmedCb.textContent === "Default brake control: armed", "confirm+clear went back to the event label: " + confirmedCb.textContent);

  // P1: a learn command the module never confirms runs out: failure text shows, the listener is retracted
  clock += 800;
  const nTimeout = learnArm1();
  const retractsBefore = commands.filter(c => c.cmd === "learn" && c.arm === 0).length;
  els["gs-learn-activation"].click(); await flush(); await flush();
  chk(learnArm1() === nTimeout + 1, "timeout-test click did not reach the bus");
  await runN(13);                                  // 3.25 s with no learning:1
  chk(els["gs-activation-status"].textContent.indexOf("failed") !== -1, "timeout did not show the failure text: " + els["gs-activation-status"].textContent);
  chk(commands.filter(c => c.cmd === "learn" && c.arm === 0).length === retractsBefore + 1, "timeout did not retract the learn request");
  await runN(11);                                  // 2.75 s after the failure started
  chk(els["gs-activation-status"].textContent === "Default brake control: armed", "failure text did not revert: " + els["gs-activation-status"].textContent);

  // P1: a bus that never accepts anything is given up on (bounded retries), and does not fire again later
  clock += 800; triggerFails = callFails = wasmFails = true;
  const nGiveUp = learnArm1();
  els["gs-learn-activation"].click(); await flush(); await flush();
  await runN(13);                                  // 3.25 s: confirm window over, all retries refused
  chk(learnArm1() === nGiveUp, "refused retries logged commands");
  chk(els["gs-activation-status"].textContent.indexOf("failed") !== -1, "refused learn never showed the failure text: " + els["gs-activation-status"].textContent);
  triggerFails = false;
  await runN(30);                                  // everything spent; failure text expires
  chk(learnArm1() === nGiveUp, "panel resent learn after giving up");
  chk(els["gs-activation-status"].textContent === "Default brake control: armed", "label did not revert after the give-up expired: " + els["gs-activation-status"].textContent);

  // Stage B (unbound host): every send went through call(rejected) -> trigger
  // exactly once - no duplicates, no losses, for the WHOLE scenario.
  // call is a deliberate FALLBACK only when trigger itself refuses: every
  // call-route use must be matched by a trigger refusal in the same scenario.
  chk((window.__gsCallSends || 0) <= (window.__gsTriggerRefusals || 0),
      "call route used without a trigger refusal preceding it (" + (window.__gsCallSends || 0) + " vs " + (window.__gsTriggerRefusals || 0) + ")");
  if (UNBOUND)
    chk(window.__gsTriggerSends === commands.length,
        "unbound host delivery is not one-per-command: " + window.__gsTriggerSends + " trigger sends vs " + commands.length + " commands committed");

}
if (process.env.GS_TEST_ROUTE) {
  // child: run one scenario in an isolated process (all state per route is fresh)
  (async () => {
    await runScenario();
    if (ok) process.stdout.write("PANEL SMOKE TEST PASSED (" + TEST_ROUTE + " route)\n");
    process.exitCode = ok ? 0 : 1;
  })();
} else {
  // parent: spawn each route clean so module/global state cannot leak between them
  const { spawnSync } = require("child_process");
  let failed = false;
  for (const route of ["legacy", "legacy-unbound", "new"]) {
    const r = spawnSync(process.execPath, [__filename, PANEL_JS],
      { env: Object.assign({}, process.env, { GS_TEST_ROUTE: route }), encoding: "utf8" });
    process.stdout.write((r.stdout || "").trim() + "\n");
    if (r.stderr) process.stderr.write(r.stderr);
    if (r.status !== 0) failed = true;
  }
  process.stdout.write(failed ? "PANEL SMOKE TEST FAILED\n" : "PANEL SMOKE TEST PASSED (all routes)\n");
  process.exitCode = failed ? 1 : 0;
}
