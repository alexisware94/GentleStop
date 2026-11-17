// On-screen diagnostics, drawn OUTSIDE <ingame-ui> so it is visible even if the panel is hidden.
// The overlay is OPT-IN: it was shown unconditionally while the panel was in
// diagnostic staging, which is no longer wanted in normal panel use. Nothing
// is created or drawn unless the user opts in via localStorage "gsDebug"="1"
// or the URL carries ?gsdebug=1. Error/rejection collection still runs so the
// data exists when an opt-in session is opened.
(function () {
  var lines = [];
  var box;
  var DEBUG = false;
  try {
    DEBUG = (typeof localStorage !== "undefined" && !!localStorage &&
             localStorage.getItem("gsDebug") === "1") ||
            (/(\?|&|,)gsdebug=(1|true)/.test(location.href));
  } catch (e) { DEBUG = false; }
  function ensureBox() {
    if (box || !document.body) return;
    box = document.createElement("pre");
    box.style.cssText = "position:fixed;left:0;top:0;z-index:2147483647;margin:0;padding:8px;" +
      "background:#ffff00;color:#000;font:14px monospace;border:3px solid #f00;max-width:60%;white-space:pre-wrap;";
    document.body.appendChild(box);
  }
  function log(s) { lines.push(s); if (lines.length > 40) lines.shift(); if (DEBUG) render(); }
  // External hook: other panel scripts can surface recoverable errors here
  // (deduped by the caller) instead of losing them to a swallowed catch.
  window.__gsDebugLog = log;
  function render() { ensureBox(); if (box) box.textContent = "GS DIAG STEP 2\n" + lines.join("\n"); }
  window.addEventListener("error", function (e) {
    log("ERR: " + e.message + " @" + String(e.filename).split("/").pop() + ":" + e.lineno);
  });
  window.addEventListener("unhandledrejection", function (e) { log("REJ: " + e.reason); });
  function rect(el) {
    if (!el) return "null";
    var r = el.getBoundingClientRect(), cs = getComputedStyle(el);
    return Math.round(r.left) + "," + Math.round(r.top) + " " + Math.round(r.width) + "x" + Math.round(r.height) +
      " disp=" + cs.display + " vis=" + cs.visibility + " op=" + cs.opacity;
  }
  function report() {
    var ui = document.getElementById("GentleStopPanel");
    var ms = document.getElementById("mainSection");
    var st = document.getElementById("gs-status");
    lines = [];
    lines.push("href: " + location.href);
    lines.push("viewport: " + innerWidth + "x" + innerHeight);
    lines.push("TemplateElement: " + (typeof TemplateElement));
    lines.push("SimVar: " + (typeof SimVar) + "  Coherent: " + (typeof Coherent));
    lines.push("ingame-ui defined: " + !!(window.customElements && customElements.get("ingame-ui")));
    lines.push("ingamepanel-custom defined: " + !!(window.customElements && customElements.get("ingamepanel-custom")));
    lines.push("ingame-ui class: " + (ui ? ui.className : "NOT FOUND"));
    lines.push("ingame-ui rect: " + rect(ui));
    lines.push("mainSection rect: " + rect(ms));
    lines.push("gs-status text: " + (st ? st.textContent : "NOT FOUND") + "  rect: " + rect(st));
    lines.push("gs init ran: " + (window.__gentleStopInit ? "init fn present" : "NO"));
    render();
  }
  document.addEventListener("DOMContentLoaded", function () { if (!DEBUG) return; ensureBox(); report(); });
  if (DEBUG) setInterval(report, 1500);   // the 1.5 s geometry refresh is opt-in only
})();
