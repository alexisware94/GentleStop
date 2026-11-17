// run_parity.js - randomized parity check of the C++ braking core against the
// reference JavaScript braking engine.
//
// Usage:
//   node run_parity.js [panel-js] [test-core-executable]
//   GS_TEST_CORE_EXE=<path> node run_parity.js [panel-js]
//
//   panel-js             source file containing the reference engine.
//                        Defaults to the canonical panel package of this repository.
//   test-core-executable binary built from wasm-source/tests/test_core.cpp.
//                        Required (argument 2 or GS_TEST_CORE_EXE): this harness
//                        never expects a compiled binary inside the repository.
//                        tools/run_tests.ps1 builds it in a temporary directory
//                        and passes this path.
//
// Relative paths are resolved against the current working directory, so the
// script can be started from the repository root or anywhere else.
//
// Exit code: 0 = every step matched, 1 = mismatch or harness failure.

const fs = require("fs"), cp = require("child_process"), path = require("path");

const repoRoot = path.resolve(__dirname, "..", "..");
const defaultPanel = path.join(repoRoot, "gentlestop-stage-panel", "html_ui", "InGamePanels",
  "GentleStopToolbarControl", "GentleStop.js");

const panelPath = path.resolve(process.argv[2] || defaultPanel);
const coreExe = path.resolve(process.argv[3] || process.env.GS_TEST_CORE_EXE || "");

function fail(msg) {
  console.error("PARITY TEST FAILED: " + msg);
  process.exit(1);
}

if (!fs.existsSync(panelPath)) fail("reference panel source not found: " + panelPath);
if (!coreExe) fail("no test_core executable was given. Build it with tools/run_tests.ps1 " +
  "and pass its path as the second argument (or set GS_TEST_CORE_EXE).");
if (!fs.existsSync(coreExe)) fail("test_core executable not found: " + coreExe);

const src = fs.readFileSync(panelPath, "utf8");
const startMarker = "// Why braking is NOT allowed", endMarker = "function writeSimVars";
const a = src.indexOf(startMarker), b = src.indexOf(endMarker);
if (a < 0 || b < 0 || b <= a) {
  fail("the reference braking-engine markers (" + JSON.stringify(startMarker) + " .. " +
    JSON.stringify(endMarker) + ") were not found in " + panelPath + ". " +
    "This harness compares that reference engine against the C++ core; the canonical " +
    "panel package no longer provides it. Supply an explicit panel-js argument that " +
    "contains the reference engine, or retire this harness by owner decision.");
}
const extracted = src.slice(a, b);
const clampSrc = "function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }";
const codes = { "": 0, "Disabled": 1, "Airborne": 2, "Parking brake": 3, "Strobes on": 4, "Throttle high": 5, "Above speed limit": 6 };
let seed = 12345; const rnd = () => (seed = (seed * 1664525 + 1013904223) % 4294967296) / 4294967296;
let worst = 0, mism = 0, total = 0;
for (let run = 0; run < 300; run++) {
  const S = { enabled: rnd() > 0.1, invert: rnd() > 0.8, strobeLock: rnd() > 0.7, speedLimitOn: rnd() > 0.6, speedLimitKt: 10 + Math.floor(rnd()*190),
    throttleLimitOn: rnd() > 0.3, throttleLimit: Math.floor(rnd()*100), deadzone: rnd()*0.5, curve: rnd() > 0.5 ? "exponential" : "linear",
    smoothing: rnd() > 0.15 ? rnd() : 0, jumpResponse: rnd() };
  const ref = new Function("settings", clampSrc + "; var brake=0,gate=false,stick=0,rawStick=0,parking=false,onGround=false,airspeedKt=0,throttlePct=0,strobeOn=false;" +
    extracted + "; return function(i,dt){ onGround=i.g===1; airspeedKt=i.sp||0; strobeOn=(i.st||0)>0; throttlePct=Math.max(i.t1||0,i.t2||0); rawStick=i.y||0;" +
    "stick=settings.invert?rawStick:-rawStick; parking=(i.pk||0)>0; updateGate(); updateBraking(dt); return {brake:brake,gate:gate,reason:blockReason(),axis:axisToBrakeValue(brake>0.005?brake:0)}; };")(S);
  let y = 0, g = 1, sp = 0, st = 0, t1 = 0, t2 = 0, pk = 0; const rows = [], hdr = [S.enabled?1:0,S.invert?1:0,S.strobeLock?1:0,S.speedLimitOn?1:0,S.speedLimitKt,S.throttleLimitOn?1:0,S.throttleLimit,S.deadzone,S.curve==="exponential"?1:0,S.smoothing,S.jumpResponse].join(" ");
  const ins = [];
  for (let k = 0; k < 600; k++) {
    if (rnd() < 0.05) y = (rnd()*2-1); else y = Math.max(-1, Math.min(1, y + (rnd()-0.5)*0.1));
    if (rnd() < 0.01) g = g ? 0 : 1; if (rnd() < 0.01) pk = pk ? 0 : 1; if (rnd() < 0.02) st = st ? 0 : 1;
    if (rnd() < 0.05) { t1 = rnd()*100; t2 = rnd() > 0.5 ? t1 : 0; } sp = Math.max(0, sp + (rnd()-0.5)*8);
    const dt = 0.004 + rnd()*0.196; ins.push({ y, g, sp, st, t1, t2, pk, dt });
    rows.push([y,g,sp,st,t1,t2,pk,dt].map(x => x.toPrecision(17)).join(" "));
  }
  let out;
  try {
    out = cp.execFileSync(coreExe, { input: hdr + "\n" + rows.join("\n") + "\n",
      cwd: path.dirname(coreExe), maxBuffer: 64 * 1024 * 1024 }).toString().trim().split("\n");
  } catch (e) {
    fail("could not execute " + coreExe + " (exit " + (e.status === undefined ? "?" : e.status) +
      "): " + (e.message || e));
  }
  if (out.length !== ins.length) {
    fail("the core returned " + out.length + " steps for " + ins.length + " inputs (run " + run + ")");
  }
  ins.forEach((i, k) => { const r = ref(i, i.dt), parts = out[k].split(" ").map(Number), cb = parts[0], cg = parts[1], cr = parts[2], ca = parts[3]; total++;
    const d = Math.abs(r.brake - cb); worst = Math.max(worst, d);
    if (d > 1e-9 || (r.gate?1:0) !== cg || codes[r.reason] !== cr || Math.abs(r.axis - ca) > 0) { mism++; if (mism < 5) console.log("MISMATCH run", run, "step", k, r, out[k]); } });
}
console.log(`steps compared: ${total}, mismatches: ${mism}, worst |brake diff|: ${worst}`);
if (mism !== 0) {
  console.error("PARITY TEST FAILED: the C++ core does not match the reference engine.");
  process.exit(1);
}
console.log("PARITY TEST PASSED");
