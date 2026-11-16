const fs = require("fs"), cp = require("child_process");
const src = fs.readFileSync(process.argv[2], "utf8");
const a = src.indexOf("// Why braking is NOT allowed"), b = src.indexOf("function writeSimVars");
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
  const out = cp.execFileSync("./test_core", { input: hdr + "\n" + rows.join("\n") + "\n" }).toString().trim().split("\n");
  ins.forEach((i, k) => { const r = ref(i, i.dt), [cb, cg, cr, ca] = out[k].split(" ").map(Number); total++;
    const d = Math.abs(r.brake - cb); worst = Math.max(worst, d);
    if (d > 1e-9 || (r.gate?1:0) !== cg || codes[r.reason] !== cr || Math.abs(r.axis - ca) > 0) { mism++; if (mism < 5) console.log("MISMATCH run", run, "step", k, r, out[k]); } });
}
console.log(`steps compared: ${total}, mismatches: ${mism}, worst |brake diff|: ${worst}`);
