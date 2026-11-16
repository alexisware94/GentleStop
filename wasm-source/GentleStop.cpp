// GentleStop -- standalone WASM module for MSFS 2024.
//
// Does the actual brake control so it keeps working whether or not the toolbar panel is open.
// The toolbar panel is now only a settings UI + status display. It talks to this module through
// the MSFS Communication Bus, which remains available in the in-game panel context.
//   GS_ALIVE: 0 = module never ran | -1 = module_init ran (waiting for frames) | -2 = SimConnect_Open failed
//             | -3 = a SimConnect setup call failed | >0 = frame counter (data frames are arriving)
//   diagnostics: L:GS_DBG_MSGS (every dispatch callback), L:GS_DBG_EXC (last SimConnect exception id)
// Settings are also saved to \work\gentlestop.cfg so they survive a sim restart without opening the panel.

#include <MSFS/MSFS.h>
#include <MSFS/MSFS_WindowsTypes.h>
#include <MSFS/MSFS_Events.h>
#include <MSFS/MSFS_CommBus.h>
#include <MSFS/Legacy/gauges.h>
#include <SimConnect.h>
#include "gentlestop_api.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "gentlestop_core.h"

#ifdef GS_USE_LEARN
#include "input_event_api.h"   // gslearn state machine + SimConnect adapter (D1)
#endif

#include "elevator_suppress.h" // staging-only ground-braking pitch isolation (empty without GS_USE_ELEV_SUPPRESS)
#include "yaxis_probe.h"      // staging-only Y-axis diagnostic probe (empty without GS_YAXIS_PROBE)

#ifndef GS_CFG_PATH
#define GS_CFG_PATH "\\work\\gentlestop.cfg"
#endif
#ifndef GS_VERSION
#define GS_VERSION "4.5"
#endif

namespace {

const DWORD DEF_SIMDATA = 1;
const DWORD REQ_SIMDATA = 1;
const DWORD EVT_LEFT_BRAKE  = 1;
const DWORD EVT_RIGHT_BRAKE = 2;
// A controller binding for the simulator's normal brake action is observable as
// brake pressure, even when MSFS does not publish its underlying button or Key Event.
const int ACTIVATION_DEFAULT_BRAKE = -2;
// Press thresholds as fractions of the travel the learned brake control really produces (0 = released,
// 1 = fully pressed). A press is a RISE above the recent trough, not an absolute level: this way a short
// tap counts without having to hold the brake until the pressure climbs high. The decay of our own stick
// braking only lowers the trough, so it can never look like a press. The fraction below was lowered from
// 0.20 to 0.12, then 0.08, and now 0.05 as a provisional experiment: the real question is what rise values
// the user's physical brake control actually produces on a short tap, and that can only be answered from
// the sim log (see the new diagnostic prints below). The floor stays at 0.06 until idle-noise measurements
// justify a lower value.
const double PRESS_RISE_FRACTION = 0.05;
const double PRESS_RISE_MIN = 0.06;   // floor, so a brake with a tiny learned travel is not noise-triggered
// Learning measures a press instead of demanding a fixed level: the controller's brake control may only
// produce partial pressure, and some aircraft report a slightly different range.
const double BRAKE_LEARN_MIN_RISE = 0.30;   // pressure travel that counts as a press
const double BRAKE_LEARN_MIN_PEAK = 0.30;   // never learn from sensor noise on a released brake
const double BRAKE_LEARN_SETTLE   = 0.25;   // s after the last rise before the peak is recorded

// Must match the order of the AddToDataDefinition calls below.
struct SimData {
    double yokeY, onGround, airspeedKt, strobe, throttle1, throttle2, parking, brakeLeft, brakeRight, absTime;
};

struct Vars {
    ID enabled, invert, strobeLock, speedOn, speedKt, throttleOn, throttle, deadzone, curve,
       smoothing, jump, activationEvent, seq, learnArm, learnSeq, learnResult, learnResultSeq,
       outBrake, outBlock, outGate, outActivation, outLearning, alive, dbgMsgs, dbgExc;
} v;

HANDLE g_sc = 0;
gs::Controller g_ctl;
bool   g_wasBraking = false;
double g_lastSeq = 0.0;
bool   g_haveTime = false;
double g_lastTime = 0.0;
double g_frames = 0.0;
double g_msgs = 0.0;
bool   g_saveDue = false;
double g_saveAt = 0.0;
enum ActivationMode {
    MODE_AUTOMATIC   = -1,
    MODE_BUTTON_CTRL = -3,
    MODE_INVALID     = -2
};

int    g_activationMode = MODE_AUTOMATIC;
bool   g_armed          = true;
int    g_activationEvent = FS_EVENT_INVALID_ID;
int    g_simState       = 0;

void resetLearnWindow();
void saveSettingsFile(const gs::Settings& s);
void loadSettingsFile(gs::Settings& s);
bool   g_learning = false;
bool   g_capturePending = false;
int    g_capturedEvent = FS_EVENT_INVALID_ID;
bool   g_togglePending = false;
bool   g_keyHandlerRegistered = false;
bool   g_commBusRegistered = false;
int    g_panelCmds = 0;          // staging diag: total commands received over CommBus
double g_lastLearnSeq = 0.0;
double g_learnResultSeq = 0.0;
double g_lastObservedBrake = 0.0;
// Brake level our own AXIS_*_BRAKE_SET write commanded on the previous frame (0 when not transmitting).
// BRAKE *_POSITION follows it, so the press detector only looks at the residual above this level.
double g_ownSent = 0.0;
// Levels the learned brake control really produces (persisted, so the toggle works after a restart).
// The thresholds are derived from them, so tuning the fractions does not require learning again.
double g_brakeBase = 0.0;      // pressure with the control released
 double g_brakePeak = 1.0;     // pressure with the control fully pressed
// Derived press sensitivity (rise above the trough that counts as a press) and the detector state.
double g_pressRise = PRESS_RISE_MIN;
double g_toggleLow = 0.0;      // pressure trough since the detector armed
double g_togglePeak = 0.0;     // highest reading of the press in progress
bool   g_toggleFired = false;  // this press was acted on; re-arms when the pressure drops back
// Current learn window.
double g_learnBase = 0.0, g_learnPeak = 0.0, g_learnLastRise = -1.0;
bool   g_learnHaveBase = false;
int    g_learnPrints = 0;
double g_learnLastPrint = -1.0;
double g_lastSkipPrint = -1.0;
// Diagnostic print throttle: bounded diagnostics so the sim console is not flooded. Prints at most
// once every 0.5 s, and only when something detector-relevant is happening (state change, residual
// approaching threshold, or a recognised press).
double g_lastDiagPrint = -1.0;
double g_lastBlockPrint = -1.0;   // Task B diagnostic: throttle-limit block cadence

// Stage 4.3: throttle-limit path diagnostics. Tracks the selected value at each
// stage so the panel can display panel-selected vs module-used vs braking-used.
double g_tLimitSet     = -1.0;    // throttle limit last set via panelCommand (CommBus)
double g_tLimitRead    = -1.0;    // throttle limit last read by pollPanelSettings (L: vars)
bool   g_tLimitOnSet   = false;   // throttleLimitOn last set via panelCommand
bool   g_tLimitNaN     = false;   // set when a NaN throttle limit was detected and rejected
double g_tLimitBlock   = -1.0;    // throttle limit in force when BLOCK_THROTTLE last occurred
double g_tPctAtBlock   = -1.0;    // throttle pct when BLOCK_THROTTLE last occurred
double g_tBlockTime    = -1.0;    // sim time when BLOCK_THROTTLE last occurred
bool   g_tBlockNow     = false;   // whether the throttle guard is blocking on this frame

// Stage 4.3: activation restoration diagnostics. Tracks the saved-binding
// lifecycle so the panel can display bind/restore/delivery state.
bool   g_bindRestored  = false;   // a saved binding was successfully restored via bind()
bool   g_bindRegistered = false;  // the restored input event was registered (mapInput+addNotify)
// Binding type of the active activation binding. Distinguishes SimConnect
// joystick/controller button bindings from keyboard (fsEvent) bindings so the
// activation gate can apply the correct readiness check per type.
enum BindingType { BINDING_NONE = 0, BINDING_JOYSTICK, BINDING_KEYBOARD, BINDING_INVALID };
BindingType g_bindingType = BINDING_NONE;
int    g_pressRx       = 0;       // physical press down-events received in LEARN_PROD
bool   g_pressRejected = false;   // the last press was rejected (anomaly: down without up)

// Stage 4.5: extended diagnostics for Issue 1 (throttle NaN source tracing).
// These capture the raw value at each stage so the panel can display exactly
// where NaN originates (panel -> CommBus -> L:var -> braking guard).
double g_tLimitSetRaw   = -1.0;   // raw throttle limit from panelCommand (CommBus)
double g_tLimitReadRaw  = -1.0;   // raw throttle limit from pollPanelSettings (L: vars)
bool   g_tLimitFieldOk  = false;  // whether tLimit was present in last status build
bool   g_statusParseOk  = false;  // whether last status message built successfully

// Stage 4.5: extended diagnostics for Issue 2 (activation binding restoration).
unsigned g_ctlCount     = 0;      // number of controllers enumerated
int      g_matchCount   = 0;      // number of identity matches in last restoreFromList
unsigned g_resolvedDevId = 0;     // currently resolved device ID (may differ from saved)
bool     g_regAttempted = false;  // whether SimConnect input registration was attempted
long     g_regHr        = 0;      // last registration HRESULT
bool     g_regOk        = false;  // whether registration succeeded
bool     g_gateState    = false;  // current activation gate state
bool     g_bindingLoaded = false; // whether a saved binding string was loaded from cfg

// Task C: panel telemetry - cached sim values for status messages.
// Defined outside #ifdef GS_USE_LEARN so sendPanelStatus() (base message)
// can always report them, even in non-LEARN builds.
double g_lastStick     = 0.0;       // yoke Y position (-1..1, forward = negative)
double g_lastSimBrake  = 0.0;       // sim brake pressure (0..1)
double g_lastThrottlePct = 0.0;     // max throttle lever position (0..100)
double g_lastAirspeedKt = 0.0;      // indicated airspeed (knots)

#ifdef GS_USE_LEARN
// ---- learned SimConnect discrete input (key / controller button toggle) -----
// Approved integration. g_activationEvent gains mode -3: a learned keyboard
// key or controller button toggles the braking-armed state through
// gslearn::ActivationInput (pinned design). The brake-pressure detector is
// observation-only in this mode (see observeDefaultBrake) and keyboard keys
// keep using the existing fsEvents handler (activationKeyEvent).
const int ACTIVATION_SIMINPUT = -3;
// Evidence states (the E1 silent episode showed S_OK alone is no delivery
// proof, so registered and verified are kept strictly apart):
//   0 none | 1 saved | 2 registered | 3 verified | 4 unavailable | 5 ambiguous
gsim::SimApi             g_simApi;    // handle set in module_init
gslearn::ActivationInput g_actInput(&g_simApi);
// Persisted identity: the input string plus the device identity it was learned
// on. Restore requires an enumerable device matching EVERY persisted field;
// missing (4) or ambiguous (5) devices are never silently rebound.
char g_simInStr[48]   = {0};         // design string, e.g. "joystick:6:button:0"
// Derive the binding type from a saved/learned input string.
// "joystick:<dev>:button:<n>" -> JOYSTICK, "key:<eventId>" -> KEYBOARD,
// empty -> NONE, anything else -> INVALID.
static BindingType bindingTypeFromStr(const char* s) {
    if (!s || !s[0]) return BINDING_NONE;
    if (strncmp(s, "joystick:", 9) == 0) return BINDING_JOYSTICK;
    if (strncmp(s, "key:", 4) == 0) return BINDING_KEYBOARD;
    return BINDING_INVALID;
}
// Whether the active activation binding is ready to gate braking.
// Joystick bindings require successful SimConnect registration.
// Keyboard (fsEvent) bindings need no SimConnect registration; a non-empty
// "key:<eventId>" string is sufficient because the fsEvents handler delivers
// the key directly.
static bool bindingReady() {
    switch (g_bindingType) {
        case BINDING_JOYSTICK: return g_bindRegistered && g_simInStr[0] != 0;
        case BINDING_KEYBOARD: return g_simInStr[0] != 0;
        default:               return false;
    }
}
char g_simDevName[64] = {0};
struct DeviceVersion { unsigned short Major, Minor, Revision, Build; };
DeviceVersion g_simDevHw = {0, 0, 0, 0};
unsigned g_simDevPid = 0, g_simDevCid = 0;
// CONTROLLERS_LIST snapshot: fan source + identity resolution + diagnostics.
// H2.5: the SDK sizes SIMCONNECT_RECV_CONTROLLERS_LIST by dwArraySize and can
// return more than 8 entries; the 8-slot array was a GentleStop-imposed limit,
// not an SDK maximum. We use a larger bounded array, record the actual received
// count (g_ctlCount), the stored count (g_nCtlList), and a truncation flag
// (g_ctlTruncated) so devices are never silently omitted.
static const unsigned GS_CTL_LIST_MAX = 64;
struct SimCtlItem {
    unsigned deviceId, productId, compositeId;
    DeviceVersion hw;
    char name[64];
};
SimCtlItem g_ctlList[GS_CTL_LIST_MAX];
int  g_nCtlList      = 0;       // number actually stored (bounded by GS_CTL_LIST_MAX)
bool g_ctlTruncated  = false;   // true if cumulative entries exceeded GS_CTL_LIST_MAX
bool g_fanArmed      = false;   // the fan opens once per learn window
bool g_reenumPending = false;       // D3: learn start / panel-open proxy only
// Multi-packet enumeration state (Task C). dwArraySize is the entry count of the
// *current received packet*, not the total. dwEntryNumber/dwOutOf sequence the
// transmissions. We accumulate across packets and only act when the enumeration
// is complete (dwEntryNumber + 1 == dwOutOf). An incomplete enumeration is
// never treated as a complete controller list.
static unsigned g_enumEntry   = 0;  // next expected packet index (0-based)
static unsigned g_enumOutOf   = 1;  // total packets in current enumeration
static bool     g_enumComplete = false;  // true once final packet received
static bool     g_enumReset    = true;   // force reset on next packet (new enumeration)
// Ghost-press window (staging-only fix): the fan capture auto-confirms while
// the user is still holding the capture button, and MSFS then re-delivers ONE
// DOWN edge of that very same press through the fresh production binding
// (verified in the step-2 logs, 4/4 windows). The capture press exists only to
// teach, so exactly ONE such delivery is swallowed - and only while this
// window is open. Any other delivery (UP edge, a second DOWN) or expiry
// closes it, so a real release/press can never be mistaken for the ghost.
bool   g_ghostExpected = false;     // set at confirm; first delivery consumes it
double g_ghostUntil    = 0.0;       // g_lastTime clock (same as the learn deadlines)
// Transactional Learn candidate and backup state (v2.2.1 redesign)
struct BindingBackup {
    int activationMode;
    bool armed;
    bool bindRegistered;
    BindingType bindingType;
    int simState;
    char simInStr[48];
    char simDevName[64];
    unsigned simDevPid;
    unsigned simDevCid;
    DeviceVersion simDevHw;
};

static BindingBackup g_learnBackup = { MODE_AUTOMATIC, true, false, BINDING_NONE, 0, {0}, {0}, 0, 0, {0,0,0,0} };
static BindingBackup g_candidateBinding = { MODE_BUTTON_CTRL, true, false, BINDING_NONE, 0, {0}, {0}, 0, 0, {0,0,0,0} };

static void startLearn(double absTime) {
    (void)absTime;
    g_learnBackup.activationMode = g_activationMode;
    g_learnBackup.armed = g_armed;
    g_learnBackup.bindRegistered = g_bindRegistered;
    g_learnBackup.bindingType = g_bindingType;
    g_learnBackup.simState = g_simState;
    memcpy(g_learnBackup.simInStr, g_simInStr, sizeof(g_simInStr));
    memcpy(g_learnBackup.simDevName, g_simDevName, sizeof(g_simDevName));
    g_learnBackup.simDevPid = g_simDevPid;
    g_learnBackup.simDevCid = g_simDevCid;
    g_learnBackup.simDevHw = g_simDevHw;

    memset(&g_candidateBinding, 0, sizeof(g_candidateBinding));
    g_candidateBinding.activationMode = MODE_BUTTON_CTRL;
    g_candidateBinding.armed = g_armed; // preserve pre-learn armed state
    g_learning = true;
    g_fanArmed = false;
    resetLearnWindow();
}

static void rollbackLearn() {
    g_learning = false;
    g_fanArmed = false;
    g_ghostExpected = false;
    memset(&g_candidateBinding, 0, sizeof(g_candidateBinding));

    if (g_learnBackup.activationMode == MODE_BUTTON_CTRL && g_learnBackup.simInStr[0]) {
        g_activationMode = g_learnBackup.activationMode;
        g_armed = g_learnBackup.armed;
        g_gateState = g_armed;
        g_bindRegistered = g_learnBackup.bindRegistered;
        g_bindingType = g_learnBackup.bindingType;
        g_simState = g_learnBackup.simState;
        memcpy(g_simInStr, g_learnBackup.simInStr, sizeof(g_simInStr));
        memcpy(g_simDevName, g_learnBackup.simDevName, sizeof(g_simDevName));
        g_simDevPid = g_learnBackup.simDevPid;
        g_simDevCid = g_learnBackup.simDevCid;
        g_simDevHw = g_learnBackup.simDevHw;
        if (g_bindRegistered || g_bindingType == BINDING_KEYBOARD) {
            g_actInput.bind(g_simInStr);
        } else {
            g_actInput.cancel();
        }
        printf("GentleStop: learn cancelled/failed; previous binding restored\n");
    } else {
        g_activationMode = MODE_AUTOMATIC;
        g_armed = g_learnBackup.armed;
        g_gateState = g_armed;
        g_bindRegistered = false;
        g_bindingType = BINDING_NONE;
        g_simState = 0;
        g_simInStr[0] = 0;
        g_simDevName[0] = 0;
        g_simDevPid = g_simDevCid = 0;
        memset(&g_simDevHw, 0, sizeof(g_simDevHw));
        g_actInput.cancel();
        printf("GentleStop: first-time learn cancelled/failed; returned to automatic mode, armed=%d\n", g_armed ? 1 : 0);
    }
}
// Adaptive status cadence (stage-1 bar-display fix): the smoothed brake
// transient completes in ~0.3 s, so the panel's 250 ms status poll alone
// represents it as visible jumps. While the brake value is actually moving,
// this build broadcasts its own status snapshot (at most every 50 ms, only
// when it moved by >= 2% since the last one); nothing extra is sent when
// static - the panel's regular poll still covers everything else.
double g_statusSentAt   = 0.0;      // sim time of the last adaptive broadcast
double g_statusSentBrake = 0.0;     // brake value at that broadcast
unsigned g_settingsSeq = 0;         // settings change sequence; published in status so the panel
                                    // can detect and reject stale snapshots that predate user edits.
double g_reenumLastAt  = 0.0;       // sim time of last controller re-enumeration attempt (g_lastTime clock)
double g_reenumInterval = 5.0;      // s between re-enumeration attempts while g_simState == 4 (unavailable)
bool   g_captNoIdent   = false;     // one-shot log for a vanished snapshot entry

// Persistence helpers (frame clock feeds learn deadlines: g_lastTime).
static void cfgString(const char* s, char* out, unsigned n) {
    unsigned i = 0;
    for (; s[i] && s[i] != '\n' && s[i] != '\r' && i + 1 < n; ++i) out[i] = s[i];
    out[i] = 0;
}
static bool parseSimInput(const char* s, unsigned* dev, int* btn) {
    return s && sscanf(s, "joystick:%u:button:%d", dev, btn) == 2;
}
// Fan source filter (probe v15 audit pattern): only real controller devices
// enter the fan; keyboard/mouse/TrackIR enumeration entries are not buttons.
static bool joystickDevice(const char* name) {
    return name[0] && !strstr(name, "Mouse") && !strstr(name, "Keyboard") &&
           !strstr(name, "TrackIR") && !strstr(name, "mouse") &&
           !strstr(name, "keyboard");
}
// Controller identity matching (H2.5 correction).
//
// MSFS 2024 SDK documents SIMCONNECT_CONTROLLER_ITEM.HardwareVersion as always
// zero for currently-enumerated devices, and CompositeID as identifying
// recognized components of composite USB devices - not guaranteed to uniquely
// identify every physical controller. Consequently the fields that actually
// distinguish controllers are DeviceName + ProductId, with CompositeID as a
// secondary discriminator when non-zero. HardwareVersion fields compare equal
// (all-zero) for every device and therefore contribute no discrimination.
//
// The comparison below keeps all fields for structural completeness, but the
// effective identity key is (name, productId, compositeId). Fail-safe ambiguity
// handling is preserved: zero matches -> unavailable (state 4), >= 2 matches ->
// ambiguous (state 5), never a silent rebind to another device. Identical
// physical devices (same name + PID + CID) cannot be distinguished and will be
// reported ambiguous - this is a documented limitation, not a defect.
static bool identMatch(const SimCtlItem& it) {
    return strcmp(it.name, g_simDevName) == 0 && it.productId == g_simDevPid &&
           it.compositeId == g_simDevCid &&
           it.hw.Major == g_simDevHw.Major && it.hw.Minor == g_simDevHw.Minor &&
           it.hw.Revision == g_simDevHw.Revision &&
           it.hw.Build == g_simDevHw.Build;
}
// Restore/revalidation: match the saved identity in the fresh enumeration and
// rebuild the design string with the CURRENT DeviceId (DeviceId can move
// across sessions - that is what EX1 documents; identity does not). Unique
// match only: 0 matches -> 'unavailable', >= 2 -> 'ambiguous', never a silent
// rebind to another device.
static void restoreFromList() {
    if (!g_simInStr[0] || g_activationMode != MODE_BUTTON_CTRL) return;
    // Keyboard (fsEvent) bindings are ready without controller enumeration
    // or SimConnect registration; no joystick device matching applies.
    if (g_bindingType == BINDING_KEYBOARD) {
        g_bindRegistered = false;
        g_simState = 2;            // binding ready
        g_regAttempted = false;
        g_regOk = true;
        g_bindRestored = true;
        g_matchCount = 1;
        g_resolvedDevId = 0;
        printf("GentleStop: restore keyboard binding '%.40s' (state 2)\n", g_simInStr);
        return;
    }
    unsigned savedDev = 0;
    int savedBtn = -1;
    if (!parseSimInput(g_simInStr, &savedDev, &savedBtn) || savedBtn < 0) {
        printf("GentleStop: saved input malformed -> off\n");
        g_activationMode = MODE_INVALID;
        g_armed = false;
        g_simState = 0;
        return;
    }
    printf("GentleStop: restoreFromList state=%d input='%.40s' dev=%u btn=%d ndev=%d\n",
           g_simState, g_simInStr, savedDev, savedBtn, g_nCtlList);
    int first = -1, n = 0;
    for (int i = 0; i < g_nCtlList; ++i) {
        const SimCtlItem& it = g_ctlList[i];
        bool match = identMatch(it);
        printf("GentleStop:   dev[%d] id=%u pid=%u cid=%u name='%.40s' match=%d\n",
               i, it.deviceId, it.productId, it.compositeId, it.name, match ? 1 : 0);
        if (match) { if (first < 0) first = i; ++n; }
    }
    printf("GentleStop:   matches=%d -> ", n);
    g_matchCount = n;
    if (n == 0) {
        g_simState = 4;
        g_bindRegistered = false;
        if (g_armed) {
            g_armed = false;
            g_gateState = false;
            saveSettingsFile(g_ctl.s);
        }
        printf("unavailable (state 4); binding inactive, disarmed\n");
        return;
    }
    if (n > 1) {
        g_simState = 5;
        g_bindRegistered = false;
        if (g_armed) {
            g_armed = false;
            g_gateState = false;
            saveSettingsFile(g_ctl.s);
        }
        printf("ambiguous (state 5); not rebound, disarmed\n");
        return;
    }
    if (g_simState == 2 || g_simState == 3) return;   // stable: no churn
    char sim[48];
    snprintf(sim, sizeof(sim), "joystick:%u:button:%d",
             (unsigned)g_ctlList[first].deviceId, savedBtn);
    if (g_actInput.bind(sim)) {
        g_simState = 2;
        // Redesign v2.2.1: Reconnecting the controller restores registration,
        // but does NOT automatically re-arm GentleStop. It remains disarmed
        // until a deliberate subsequent physical press.
        g_armed = false;
        g_gateState = false;
        saveSettingsFile(g_ctl.s);
        g_resolvedDevId = g_ctlList[first].deviceId;
        g_regAttempted = true;
        g_regOk = true;
        g_regHr = g_simApi.lastHr();
        g_bindRestored = true;
        g_bindRegistered = true;
        printf("bound (state 2) on dev %u '%s' (disarmed until physical press)\n",
               (unsigned)g_ctlList[first].deviceId, sim);
        snprintf(g_simInStr, sizeof(g_simInStr), "%.46s", sim);
        printf("GentleStop: registered on dev %u (not verified)\n",
               (unsigned)g_ctlList[first].deviceId);
    } else {
        g_regAttempted = true;
        g_regOk = false;
        g_regHr = g_simApi.lastHr();
        g_bindRegistered = false;
        printf("GentleStop: bind '%s' failed (see lines above); state stays %d\n",
               sim, g_simState);
    }
}
// CONTROLLERS_LIST: copy the snapshot; then open the learn fan (learn window)
// or run the restore/revalidation (module init / panel-open proxy).
//
// Multi-packet handling (Task C): dwArraySize is the number of entries in the
// *current received packet*, not the total across all packets. dwEntryNumber is
// the zero-based index of this transmission; dwOutOf is the total number of
// transmissions. We accumulate entries across packets, respecting ordering, and
// only act (fan open / restore) once the enumeration is complete
// (dwEntryNumber + 1 == dwOutOf). An incomplete enumeration is never treated as
// a complete controller list.
static void noteControllersList(const SIMCONNECT_RECV_CONTROLLERS_LIST* list) {
    const DWORD dwSize = list->dwSize;
    const unsigned n = list->dwArraySize;
    const unsigned entry = list->dwEntryNumber;
    const unsigned outOf = list->dwOutOf;

    // Buffer bounds validation: dwArraySize entries must fit within the
    // received buffer (after the fixed header). Clamp to avoid overrun.
    const size_t headerSize = sizeof(SIMCONNECT_RECV_CONTROLLERS_LIST) - sizeof(SIMCONNECT_CONTROLLER_ITEM);
    const size_t maxPayload = (dwSize > headerSize) ? (dwSize - headerSize) : 0;
    const unsigned maxEntriesBySize = (unsigned)(maxPayload / sizeof(SIMCONNECT_CONTROLLER_ITEM));
    const unsigned entriesToCopy = (n < maxEntriesBySize) ? n : maxEntriesBySize;
    if (entriesToCopy < n) {
        printf("GentleStop: noteControllersList dwArraySize=%u exceeds buffer bound, clamping to %u\n",
               n, entriesToCopy);
    }

    // New enumeration detection: dwEntryNumber == 0 starts a fresh enumeration.
    // Reset the accumulator so a previous (possibly incomplete) enumeration is discarded.
    if (entry == 0 || g_enumReset) {
        g_nCtlList = 0;
        g_ctlTruncated = false;
        g_enumComplete = false;
        g_enumOutOf = (outOf > 0) ? outOf : 1;
        g_enumEntry = 0;
        g_enumReset = false;
        g_ctlCount = n;
        printf("GentleStop: noteControllersList new-enumeration outOf=%u\n", g_enumOutOf);
    }

    // Packet sequencing: detect out-of-order / duplicate / missing packets.
    bool droppedThisPacket = false;   // declared before goto to avoid jump-over-init
    if (entry != g_enumEntry) {
        printf("GentleStop: noteControllersList out-of-order entry=%u expected=%u outOf=%u\n",
               entry, g_enumEntry, g_enumOutOf);
        if (entry < g_enumEntry) {
            // Duplicate packet: do not re-accumulate; still evaluate completion.
            if (entry + 1u == g_enumOutOf && !g_enumComplete) {
                g_enumComplete = true;
                goto enumerateComplete;
            }
            return;
        }
        // entry > g_enumEntry: gap; continue accumulating.
    }

    // Accumulate entries (bounded by GS_CTL_LIST_MAX). Track whether any
    // received entries were dropped because the snapshot is full.
    for (unsigned i = 0; i < entriesToCopy; ++i) {
        const SIMCONNECT_CONTROLLER_ITEM& it = list->rgData[i];
        if (g_nCtlList >= GS_CTL_LIST_MAX) {
            droppedThisPacket = true;
            continue;
        }
        SimCtlItem& d = g_ctlList[g_nCtlList++];
        snprintf(d.name, sizeof(d.name), "%.60s", it.DeviceName);
        d.deviceId = it.DeviceId; d.productId = it.ProductId;
        d.compositeId = it.CompositeID;
        d.hw.Major = it.HardwareVersion.Major;
        d.hw.Minor = it.HardwareVersion.Minor;
        d.hw.Revision = it.HardwareVersion.Revision;
        d.hw.Build = it.HardwareVersion.Build;
        printf("GentleStop: dev id=%u pid=%u cid=%u hw=%u.%u.%u.%u '%.32s' [pkt %u/%u]\n",
               it.DeviceId, it.ProductId, it.CompositeID,
               it.HardwareVersion.Major, it.HardwareVersion.Minor,
               it.HardwareVersion.Revision, it.HardwareVersion.Build, d.name,
               entry + 1, outOf);
    }
    if (droppedThisPacket) g_ctlTruncated = true;
    if (n > entriesToCopy) g_ctlTruncated = true;   // buffer-bound clamp dropped entries
    if (n > GS_CTL_LIST_MAX) g_ctlTruncated = true; // single packet exceeds capacity

    if (entry >= g_enumEntry) g_enumEntry = entry + 1;

    // Completion check: final packet received.
    if (entry + 1u >= outOf) {
        g_enumComplete = true;
    }

    if (!g_enumComplete) {
        printf("GentleStop: noteControllersList pkt=%u/%u stored=%u waiting\n",
               entry + 1, outOf, g_nCtlList);
        return;
    }

enumerateComplete:
    // Enumeration complete: act on the full accumulated list.
    g_ctlCount = g_nCtlList;
    printf("GentleStop: noteControllersList COMPLETE stored=%u truncated=%d\n",
           g_nCtlList, g_ctlTruncated ? 1 : 0);

    // Reset for the next enumeration session.
    g_enumReset = true;

    if (g_learning) {
        if (g_fanArmed) return;
        g_fanArmed = true;
        gslearn::ControllerInfo ci[GS_CTL_LIST_MAX];
        int m = 0;
        for (int i = 0; i < g_nCtlList; ++i) {
            if (!joystickDevice(g_ctlList[i].name)) continue;
            ci[m].deviceId = g_ctlList[i].deviceId;
            ci[m].name.assign(g_ctlList[i].name);
            ++m;
        }
        if (m == 0) {
            printf("GentleStop: learn: no controller device found\n");
            return;
        }
        if (g_actInput.beginLearn(ci, m, g_lastTime))
            printf("GentleStop: learn fan armed (buttons 0..15 per device)\n");
        else
            printf("GentleStop: learn fan failed (budget? no device)\n");
        return;
    }
    restoreFromList();
}
#endif // GS_USE_LEARN

#ifndef GS_USE_LEARN
char g_simInStr[48] = {0};
char g_simDevName[64] = {0};
unsigned g_simDevPid = 0, g_simDevCid = 0;
struct DeviceVersion { unsigned short Major, Minor, Revision, Build; };
DeviceVersion g_simDevHw = {0, 0, 0, 0};
bool g_ghostExpected = false;
static void startLearn(double absTime) { (void)absTime; g_learning = true; }
static void rollbackLearn() { g_learning = false; }
const int ACTIVATION_SIMINPUT = -3;
#endif

bool activationAllowed() {
    if (g_learning) return false;

    switch (g_activationMode) {
        case MODE_AUTOMATIC:
            return g_armed;

        case MODE_BUTTON_CTRL:
            return g_armed && bindingReady();

        default:
            return false;
    }
}

void recomputePressLevels() {
    double travel = g_brakePeak - g_brakeBase;
    if (!(travel > 0.05)) travel = 1.0;   // nothing measurable learned yet: assume a full 0..1 travel
    g_pressRise = PRESS_RISE_FRACTION * travel;
    if (g_pressRise < PRESS_RISE_MIN) g_pressRise = PRESS_RISE_MIN;
}

// The pressure shown on top of whatever our own stick braking commanded. Our own braking raises the very
// same BRAKE *_POSITION simvars (AXIS_*_BRAKE_SET feeds back), so only this residual can be the user's
// button; our own ramp-up never shows as a rise here because the simvar tracks the commanded level.
double residualBrake(double observed) {
    double r = observed - g_ownSent;
    return r > 0.0 ? r : 0.0;
}

void resetLearnWindow() {
    g_learnHaveBase = false;
    g_learnBase = g_learnPeak = 0.0;
    g_learnLastRise = -1.0;
    g_learnPrints = 0;
    g_learnLastPrint = -1.0;
}

bool jsonNumber(const char* json, const char* key, double& value) {
    // Match the complete JSON key token ("key":), so "throttleLimit" cannot
    // match inside "throttleLimitOn". The settings message sends
    // "throttleLimitOn" before "throttleLimit"; strstr on the bare key used to
    // land on the "On" suffix, strtod then failed (end == p) and the parse
    // returned false - leaving throttleLimit at its previous value (50%) while
    // the panel showed the user's new selection. Braking kept using 50%.
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ') ++p;
    // Accept both quoted legacy form "key":"70" and unquoted form "key":70.
    bool quoted = (*p == '"');
    if (quoted) ++p;
    char* end = nullptr;
    value = strtod(p, &end);
    if (end == p) return false;   // no digits parsed
    // Validate that the value is properly terminated (no trailing junk).
    while (*end == ' ') ++end;
    if (quoted) {
        if (*end != '"') return false;   // quoted value must close with "
    } else {
        if (*end != ',' && *end != '}' && *end != '\0') return false;
    }
    return true;
}

bool jsonCommand(const char* json, const char* command) {
    char needle[64];
    snprintf(needle, sizeof needle, "\"cmd\":\"%s\"", command);
    return strstr(json, needle) != nullptr;
}

void sendPanelStatus() {
    char message[1024];
    snprintf(message, sizeof message,
             "{\"alive\":%.0f,\"brake\":%.6f,\"block\":%d,\"gate\":%d,\"activation\":%d,\"learning\":%d,\"activationEvent\":%d,\"activationMode\":%d,\"armed\":%d,\"dbgMsgs\":%.0f,\"dbgExc\":%.0f,\"stick\":%.4f,\"simBrake\":%.4f,\"throttlePct\":%.4f,\"airspeedKt\":%.4f,"
             "\"tLimit\":%.3f,\"tLimitOn\":%d,\"tBlock\":%d,\"tLimitNaN\":%d,\"tLimitSetRaw\":%.3f,\"tLimitReadRaw\":%.3f,\"version\":\"%s\"}",
             g_frames, g_ctl.brake, (int)g_ctl.reason, g_ctl.gate ? 1 : 0,
             activationAllowed() ? 1 : 0,
             g_learning ? 1 : 0, g_activationEvent, g_activationMode, g_armed ? 1 : 0, g_msgs,
             get_named_variable_value(v.dbgExc),
             // Task C: panel telemetry - sim stick, brake pressure, throttle, airspeed
             g_lastStick, g_lastSimBrake, g_lastThrottlePct, g_lastAirspeedKt,
             g_ctl.s.throttleLimit, g_ctl.s.throttleLimitOn ? 1 : 0,
             g_tBlockNow ? 1 : 0, g_tLimitNaN ? 1 : 0,
             g_tLimitSetRaw, g_tLimitReadRaw,
             GS_VERSION);
#ifdef GS_USE_LEARN
    size_t len = strlen(message);
    if (len > 0 && message[len - 1] == '}')
        snprintf(message + len - 1, sizeof(message) - (len - 1),
                 ",\"activationMode\":%d,\"armed\":%d,"
                 "\"simState\":%d,\"simInput\":\"%.40s\","
                 "\"simDevice\":\"%.30s\","
                 "\"enabled\":%d,\"invert\":%d,\"strobeLock\":%d,"
                 "\"speedLimitOn\":%d,\"speedLimitKt\":%.3f,"
                 "\"throttleLimitOn\":%d,\"throttleLimit\":%.3f,"
                 "\"deadzone\":%.5f,\"curve\":%d,\"smoothing\":%.5f,\"jumpResponse\":%.5f,"
                 "\"activationCleared\":%d,\"settingsSeq\":%u,"
                 // Stage 4.3: activation restoration diagnostics
                 "\"bindRestored\":%d,\"bindRegistered\":%d,\"pressRx\":%d,\"pressRejected\":%d,"
                 // Stage 4.5: activation restoration extended diagnostics
// H2.5: ctlStored / ctlTruncated expose controller snapshot truncation.
                 "\"ctlCount\":%u,\"ctlTruncated\":%d,\"matchCount\":%d,\"resolvedDevId\":%u,"
                 "\"regAttempted\":%d,\"regHr\":%ld,\"regOk\":%d,\"gateState\":%d,\"bindingLoaded\":%d}",
                 g_activationMode, g_armed ? 1 : 0,
                 g_simState, g_simInStr, g_simDevName,
                 g_ctl.s.enabled ? 1 : 0, g_ctl.s.invert ? 1 : 0, g_ctl.s.strobeLock ? 1 : 0,
                 g_ctl.s.speedLimitOn ? 1 : 0, g_ctl.s.speedLimitKt,
                 g_ctl.s.throttleLimitOn ? 1 : 0, g_ctl.s.throttleLimit,
                 g_ctl.s.deadzone, g_ctl.s.curve, g_ctl.s.smoothing, g_ctl.s.jumpResponse,
                 !g_armed ? 1 : 0, g_settingsSeq,
                 g_bindRestored ? 1 : 0, g_bindRegistered ? 1 : 0,
                 g_pressRx, g_pressRejected ? 1 : 0,
                 g_ctlCount, g_ctlTruncated ? 1 : 0, g_matchCount, g_resolvedDevId,
                 g_regAttempted ? 1 : 0, (long)g_regHr, g_regOk ? 1 : 0,
                 g_armed ? 1 : 0, g_bindingLoaded ? 1 : 0);
#endif
    fsCommBusCall("GentleStopStatus", message, (unsigned int)strlen(message), FsCommBusBroadcast_JS);
}

// Stage 4.4: returns true when the finite double v is safe to use as a
// settings value (rejects NaN from unset L: variables in real MSFS).
static bool validLvar(double v) { return v == v; }   // v == v is false iff NaN
// Stage 4.4: returns true when v is NaN-free AND infinity-free, so a corrupt
// L: variable can never propagate into the braking controller.
static bool isFiniteSetting(double v) { return v == v && v != INFINITY && v != -INFINITY; }

void panelCommand(const char* buffer, unsigned int bufferSize, void* context) {
    (void)context;
    char json[512];
    unsigned int n = bufferSize < sizeof(json) - 1 ? bufferSize : sizeof(json) - 1;
    memcpy(json, buffer, n); json[n] = 0;
#ifdef GS_USE_LEARN
    // Stage-1 diagnostics: prove (or disprove) that panel commands reach the
    // module. Bounded: the first 8 commands then stay silent (the status poll
    // would otherwise flood the console).
    { static int s_cmdLog = 0; ++g_panelCmds;
      if (s_cmdLog < 8) printf("GentleStop: panelCommand #%d: %.80s\n", ++s_cmdLog, json); }
#endif
    if (jsonCommand(json, "settings")) {
        double x;
        if (jsonNumber(json, "enabled", x) && validLvar(x)) g_ctl.s.enabled = x > 0.5;
        if (jsonNumber(json, "invert", x) && validLvar(x)) g_ctl.s.invert = x > 0.5;
        if (jsonNumber(json, "strobeLock", x) && validLvar(x)) g_ctl.s.strobeLock = x > 0.5;
        if (jsonNumber(json, "speedLimitOn", x) && validLvar(x)) g_ctl.s.speedLimitOn = x > 0.5;
        if (jsonNumber(json, "speedLimitKt", x) && validLvar(x)) g_ctl.s.speedLimitKt = x;
        if (jsonNumber(json, "throttleLimitOn", x) && validLvar(x)) g_ctl.s.throttleLimitOn = x > 0.5;
        if (jsonNumber(json, "throttleLimit", x) && validLvar(x) && isFiniteSetting(x)) {
            g_ctl.s.throttleLimit = x;
            g_tLimitSet = x;
            g_tLimitSetRaw = x;   // Stage 4.5: raw value received from panel
            g_tLimitNaN = false;   // a valid value cleared any earlier NaN rejection
            g_tLimitOnSet = g_ctl.s.throttleLimitOn;
            g_tLimitOnSet = g_ctl.s.throttleLimitOn;
            g_tLimitOnSet = g_ctl.s.throttleLimitOn;
            // Task B diagnostic: the panel's throttle-limit selection actually reached
            // the module and parsed. Logged once per distinct value to avoid flooding.
            static double s_lastLoggedLimit = -1.0;
            if (x != s_lastLoggedLimit) {
                s_lastLoggedLimit = x;
                printf("GentleStop: settings throttleLimit -> %.3f (on=%d)\n",
                       x, (int)g_ctl.s.throttleLimitOn);
            }
        } else {
            // Key present but value was NaN/corrupt - reject and flag.
            char needle[64];
            snprintf(needle, sizeof needle, "\"throttleLimit\":");
            if (strstr(json, needle) != nullptr) g_tLimitNaN = true;
        }
        if (jsonNumber(json, "deadzone", x) && validLvar(x)) g_ctl.s.deadzone = x;
        if (jsonNumber(json, "curve", x) && validLvar(x)) g_ctl.s.curve = (int)x;
        if (jsonNumber(json, "smoothing", x) && validLvar(x)) g_ctl.s.smoothing = x;
        if (jsonNumber(json, "jumpResponse", x) && validLvar(x)) g_ctl.s.jumpResponse = x;
        if (jsonNumber(json, "activationMode", x) && validLvar(x)) {
            int mode = (int)x;
            if (g_activationMode == MODE_BUTTON_CTRL) {
                // Button-controlled mode cannot be bypassed by settings
            } else if (mode == MODE_AUTOMATIC) {
                g_activationMode = MODE_AUTOMATIC;
            }
        }
        if (jsonNumber(json, "armed", x) && validLvar(x)) {
            int a = (int)x;
            if (g_activationMode == MODE_AUTOMATIC && (a == 0 || a == 1)) {
                g_armed = (a == 1);
                g_gateState = g_armed;
            }
        }
        if (jsonNumber(json, "activationEvent", x) && validLvar(x)) {
            int eventId = (int)x;
#ifdef GS_USE_LEARN
            if (g_activationMode == MODE_BUTTON_CTRL && eventId != ACTIVATION_SIMINPUT) {
                g_reenumPending = true;
            } else if (eventId == ACTIVATION_SIMINPUT && !g_simInStr[0]) {
                printf("GentleStop: stale -3 push ignored (no learned binding)\n");
            } else if (eventId == -1) {
                g_activationMode = MODE_AUTOMATIC;
            }
#endif
        }
        g_ctl.s.clamp(); g_saveDue = true; g_saveAt = 0.0;
#ifdef GS_USE_LEARN
        g_settingsSeq++;
        // Stage 4.3: write settings to L: variables so pollPanelSettings reads
        // consistent values and cannot overwrite CommBus-set values with stale
        // L: variable defaults (e.g. throttleLimit=50 left in \work\gentlestop.cfg).
        set_named_variable_value(v.seq, (double)g_settingsSeq);
        set_named_variable_value(v.enabled, g_ctl.s.enabled ? 1.0 : 0.0);
        set_named_variable_value(v.invert, g_ctl.s.invert ? 1.0 : 0.0);
        set_named_variable_value(v.strobeLock, g_ctl.s.strobeLock ? 1.0 : 0.0);
        set_named_variable_value(v.speedOn, g_ctl.s.speedLimitOn ? 1.0 : 0.0);
        set_named_variable_value(v.speedKt, g_ctl.s.speedLimitKt);
        set_named_variable_value(v.throttleOn, g_ctl.s.throttleLimitOn ? 1.0 : 0.0);
        set_named_variable_value(v.throttle, g_ctl.s.throttleLimit);
        set_named_variable_value(v.deadzone, g_ctl.s.deadzone);
        set_named_variable_value(v.curve, (double)g_ctl.s.curve);
        set_named_variable_value(v.smoothing, g_ctl.s.smoothing);
        set_named_variable_value(v.jump, g_ctl.s.jumpResponse);
        set_named_variable_value(v.activationEvent, (double)g_activationEvent);
#endif
    } else if (jsonCommand(json, "setArmed")) {
        double armedVal;
        if (!jsonNumber(json, "armed", armedVal) || !validLvar(armedVal)) {
            printf("GentleStop: setArmed rejected (missing or invalid parameter)\n");
            return;
        }
        int desired = (int)armedVal;
        if (desired != 0 && desired != 1) {
            printf("GentleStop: setArmed rejected (value %d must be 0 or 1)\n", desired);
            return;
        }
        if (g_activationMode == MODE_BUTTON_CTRL) {
            printf("GentleStop: setArmed rejected in button-controlled mode (physical button required)\n");
            return;
        }
        if (g_activationMode != MODE_AUTOMATIC) {
            printf("GentleStop: setArmed rejected (mode %d not automatic)\n", g_activationMode);
            return;
        }
        bool newArmed = (desired == 1);
        if (g_armed != newArmed) {
            g_armed = newArmed;
            g_gateState = g_armed;
            saveSettingsFile(g_ctl.s);
            printf("GentleStop: setArmed accepted -> armed=%d\n", g_armed ? 1 : 0);
        } else {
            printf("GentleStop: setArmed idempotent -> already armed=%d\n", g_armed ? 1 : 0);
        }
    } else if (jsonCommand(json, "learn")) {
        double arm = 0.0; jsonNumber(json, "arm", arm);
        if (arm > 0.5 && g_learning) {
            printf("GentleStop: CommBus learn -> already listening\n");
        } else if (arm > 0.5) {
            startLearn(g_lastTime);
            printf("GentleStop: CommBus learn -> listening\n");
            printf("GentleStop: learn rise=%.2f\n", g_pressRise);
            set_named_variable_value(v.learnResult, (double)FS_EVENT_INVALID_ID);
#ifdef GS_USE_LEARN
            g_fanArmed = false;
            g_captNoIdent = false;
            if (g_sc) SimConnect_EnumerateControllers(g_sc);
#endif
        } else {
            printf("GentleStop: CommBus learn -> cancelled\n");
            rollbackLearn();
        }
    } else if (jsonCommand(json, "clearActivation")) {
        g_activationMode = MODE_AUTOMATIC;
        g_armed = true;
        g_activationEvent = FS_EVENT_INVALID_ID;
        g_gateState = true;
        g_learning = false;
        g_ghostExpected = false;
        g_simInStr[0] = 0; g_simDevName[0] = 0;
        g_simDevPid = 0; g_simDevCid = 0;
        g_simDevHw.Major = g_simDevHw.Minor = g_simDevHw.Revision = g_simDevHw.Build = 0;
        g_simState = 0;
#ifdef GS_USE_LEARN
        g_actInput.cancel();
        g_bindRestored = false;
        g_bindRegistered = false;
        g_bindingType = BINDING_NONE;
#endif
        g_ctl.s.clamp();
        saveSettingsFile(g_ctl.s);
        printf("GentleStop: clearActivation (panel) -> binding removed, automatic mode, armed=1\n");
#ifdef GS_USE_LEARN
        g_settingsSeq++;
#endif
    }
    if (jsonCommand(json, "status")) sendPanelStatus();
    if (jsonCommand(json, "configSnapshot")) sendPanelStatus();
}

// ---------------------------------------------------------------- settings file
struct CfgLine {
    char key[64];
    char val[128];
};

static const char* findCfgVal(const char* key, const CfgLine* entries, int n) {
    for (int i = 0; i < n; ++i) {
        if (!strcmp(entries[i].key, key)) return entries[i].val;
    }
    return nullptr;
}

void saveSettingsFile(const gs::Settings& s) {
    FILE* f = fopen(GS_CFG_PATH, "w");
    if (!f) return;
    char buf[1024];
    int pos = snprintf(buf, sizeof(buf),
             "enabled=%d\ninvert=%d\nstrobeLock=%d\nspeedLimitOn=%d\nspeedLimitKt=%.3f\n"
             "throttleLimitOn=%d\nthrottleLimit=%.3f\ndeadzone=%.5f\ncurve=%d\nsmoothing=%.5f\njumpResponse=%.5f\n"
             "activationMode=%d\narmed=%d\nactivationSimInput=%s\n",
             s.enabled ? 1 : 0, s.invert ? 1 : 0, s.strobeLock ? 1 : 0,
             s.speedLimitOn ? 1 : 0, s.speedLimitKt,
             s.throttleLimitOn ? 1 : 0, s.throttleLimit,
             s.deadzone, s.curve, s.smoothing, s.jumpResponse,
             g_activationMode, g_armed ? 1 : 0,
             g_simInStr);
#ifdef GS_USE_LEARN
    if (g_simInStr[0] && g_activationMode == MODE_BUTTON_CTRL) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "activationDeviceName=%s\n"
                        "activationDeviceIdent=%u,%u,%u,%u,%u,%u\n",
                        g_simDevName, g_simDevPid, g_simDevCid,
                        (unsigned)g_simDevHw.Major, (unsigned)g_simDevHw.Minor,
                        (unsigned)g_simDevHw.Revision, (unsigned)g_simDevHw.Build);
    }
#endif
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "activationBrakeBase=%.4f\nactivationBrakePeak=%.4f\n",
                    g_brakeBase, g_brakePeak);
    fputs(buf, f);
    fclose(f);
}

void loadSettingsFile(gs::Settings& s) {
    FILE* f = fopen(GS_CFG_PATH, "r");
    if (!f) {
        g_activationMode = MODE_AUTOMATIC;
        g_armed = true;
        g_activationEvent = FS_EVENT_INVALID_ID;
        g_gateState = true;
        g_simInStr[0] = 0;
        g_bindingLoaded = false;
        g_simState = 0;
        recomputePressLevels();
        s.clamp();
        return;
    }

    CfgLine entries[64];
    int nEntries = 0;
    char line[128];
    bool hasAnyValidKey = false;

    while (fgets(line, sizeof line, f) && nEntries < 64) {
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\r' || line[l-1] == '\n')) line[--l] = 0;
        if (l == 0) continue;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        snprintf(entries[nEntries].key, sizeof(entries[nEntries].key), "%s", line);
        snprintf(entries[nEntries].val, sizeof(entries[nEntries].val), "%s", eq + 1);
        hasAnyValidKey = true;
        nEntries++;
    }
    fclose(f);

    if (!hasAnyValidKey) {
        printf("GentleStop: corrupt configuration file -> fail closed\n");
        g_activationMode = MODE_INVALID;
        g_armed = false;
        g_gateState = false;
        g_simState = 0;
        recomputePressLevels();
        s.clamp();
        return;
    }

    const char* val;
    if ((val = findCfgVal("enabled", entries, nEntries))) s.enabled = (atof(val) != 0);
    if ((val = findCfgVal("invert", entries, nEntries))) s.invert = (atof(val) != 0);
    if ((val = findCfgVal("strobeLock", entries, nEntries))) s.strobeLock = (atof(val) != 0);
    if ((val = findCfgVal("speedLimitOn", entries, nEntries))) s.speedLimitOn = (atof(val) != 0);
    if ((val = findCfgVal("speedLimitKt", entries, nEntries))) s.speedLimitKt = atof(val);
    if ((val = findCfgVal("throttleLimitOn", entries, nEntries))) s.throttleLimitOn = (atof(val) != 0);
    if ((val = findCfgVal("throttleLimit", entries, nEntries))) s.throttleLimit = atof(val);
    if ((val = findCfgVal("deadzone", entries, nEntries))) s.deadzone = atof(val);
    if ((val = findCfgVal("curve", entries, nEntries))) s.curve = (int)atof(val);
    if ((val = findCfgVal("smoothing", entries, nEntries))) s.smoothing = atof(val);
    if ((val = findCfgVal("jumpResponse", entries, nEntries))) s.jumpResponse = atof(val);
    if ((val = findCfgVal("activationBrakeBase", entries, nEntries))) g_brakeBase = atof(val);
    if ((val = findCfgVal("activationBrakePeak", entries, nEntries))) g_brakePeak = atof(val);

#ifdef GS_USE_LEARN
    if ((val = findCfgVal("activationSimInput", entries, nEntries))) {
        cfgString(val, g_simInStr, sizeof(g_simInStr));
        g_bindingLoaded = (g_simInStr[0] != 0);
        // Determine binding type from the saved string so keyboard bindings
        // are not mistaken for joystick bindings and do not require SimConnect
        // registration to be considered ready.
        g_bindingType = bindingTypeFromStr(g_simInStr);
        // For keyboard bindings, restore g_activationEvent from "key:<id>" so
        // the fsEvents handler can match incoming key presses after restart.
        if (g_bindingType == BINDING_KEYBOARD) {
            const char* colon = strchr(g_simInStr, ':');
            if (colon && colon[1]) g_activationEvent = atoi(colon + 1);
        }
    } else {
        g_simInStr[0] = 0;
        g_bindingLoaded = false;
        g_bindingType = BINDING_NONE;
    }
    if ((val = findCfgVal("activationDeviceName", entries, nEntries))) {
        cfgString(val, g_simDevName, sizeof(g_simDevName));
    }
    if ((val = findCfgVal("activationDeviceIdent", entries, nEntries))) {
        sscanf(val, "%u,%u,%hu,%hu,%hu,%hu", &g_simDevPid, &g_simDevCid,
               &g_simDevHw.Major, &g_simDevHw.Minor,
               &g_simDevHw.Revision, &g_simDevHw.Build);
    }
#endif

    bool needsMigrationPersist = false;
    const char* vMode = findCfgVal("activationMode", entries, nEntries);
    const char* vLegacyEvent = findCfgVal("activationEvent", entries, nEntries);

    if (vMode) {
        if (!strcmp(vMode, "-1")) {
            g_activationMode = MODE_AUTOMATIC;
            g_activationEvent = FS_EVENT_INVALID_ID;
        } else if (!strcmp(vMode, "-3")) {
            g_activationMode = MODE_BUTTON_CTRL;
        } else {
            printf("GentleStop: invalid activationMode=%s -> fail closed\n", vMode);
            g_activationMode = MODE_INVALID;
            g_armed = false;
        }
    } else if (vLegacyEvent) {
        if (!strcmp(vLegacyEvent, "-1")) {
            g_activationMode = MODE_AUTOMATIC;
            g_activationEvent = FS_EVENT_INVALID_ID;
            needsMigrationPersist = true;
        } else if (!strcmp(vLegacyEvent, "-3")) {
            g_activationMode = MODE_BUTTON_CTRL;
            needsMigrationPersist = true;
        } else if (!strcmp(vLegacyEvent, "-2")) {
            g_activationMode = MODE_BUTTON_CTRL;
            needsMigrationPersist = true;
        } else if (atoi(vLegacyEvent) > 0) {
            g_activationMode = MODE_BUTTON_CTRL;
            needsMigrationPersist = true;
        } else {
            printf("GentleStop: invalid legacy activationEvent=%s -> fail closed\n", vLegacyEvent);
            g_activationMode = MODE_INVALID;
            g_armed = false;
        }
    } else {
        g_activationMode = MODE_AUTOMATIC;
        g_activationEvent = FS_EVENT_INVALID_ID;
    }

    const char* vArmed = findCfgVal("armed", entries, nEntries);
    const char* vLegacyCleared = findCfgVal("activationCleared", entries, nEntries);

    if (vArmed) {
        if (!strcmp(vArmed, "1")) {
            g_armed = true;
        } else if (!strcmp(vArmed, "0")) {
            g_armed = false;
        } else {
            printf("GentleStop: invalid armed=%s -> disarmed with diagnostic\n", vArmed);
            g_armed = false;
        }
    } else if (vLegacyCleared) {
        if (!strcmp(vLegacyCleared, "1")) {
            g_armed = false;
            needsMigrationPersist = true;
        } else if (!strcmp(vLegacyCleared, "0")) {
            g_armed = true;
            needsMigrationPersist = true;
        } else {
            g_armed = false;
        }
    } else {
        g_armed = true;
    }

    if (g_activationMode == MODE_INVALID) {
        g_armed = false;
    }

    g_gateState = g_armed;

#ifdef GS_USE_LEARN
    g_simState = (g_activationMode == MODE_BUTTON_CTRL && g_simInStr[0]) ? 1 : 0;
#endif

    if (!(g_brakePeak - g_brakeBase > 0.05)) { g_brakeBase = 0.0; g_brakePeak = 1.0; }
    recomputePressLevels();
    s.clamp();

    if (needsMigrationPersist) {
        printf("GentleStop: legacy configuration migrated -> persisting new canonical fields\n");
        saveSettingsFile(s);
    }
}

void registerVars() {
    v.enabled    = register_named_variable("GS_ENABLED");
    v.invert     = register_named_variable("GS_INVERT");
    v.strobeLock = register_named_variable("GS_STROBELOCK");
    v.speedOn    = register_named_variable("GS_SPEEDLIMIT_ON");
    v.speedKt    = register_named_variable("GS_SPEEDLIMIT_KT");
    v.throttleOn = register_named_variable("GS_THROTTLELIMIT_ON");
    v.throttle   = register_named_variable("GS_THROTTLELIMIT");
    v.deadzone   = register_named_variable("GS_DEADZONE");
    v.curve      = register_named_variable("GS_CURVE");
    v.smoothing  = register_named_variable("GS_SMOOTHING");
    v.jump       = register_named_variable("GS_JUMP");
    v.activationEvent = register_named_variable("GS_ACTIVATION_EVENT");
    v.seq        = register_named_variable("GS_SETTINGS_SEQ");
    v.learnArm   = register_named_variable("GS_LEARN_ARM");
    v.learnSeq   = register_named_variable("GS_LEARN_SEQ");
    v.learnResult = register_named_variable("GS_LEARN_RESULT");
    v.learnResultSeq = register_named_variable("GS_LEARN_RESULT_SEQ");
    v.outBrake   = register_named_variable("GS_OUT_BRAKE");
    v.outBlock   = register_named_variable("GS_OUT_BLOCK");
    v.outGate    = register_named_variable("GS_OUT_GATE");
    v.outActivation = register_named_variable("GS_OUT_ACTIVATION");
    v.outLearning = register_named_variable("GS_OUT_LEARNING");
    v.alive      = register_named_variable("GS_ALIVE");
    v.dbgMsgs    = register_named_variable("GS_DBG_MSGS");
    v.dbgExc     = register_named_variable("GS_DBG_EXC");
}

// Returns true when the panel has pushed a new settings set.
bool pollPanelSettings(gs::Settings& s) {
    double seq = get_named_variable_value(v.seq);
    // Stage 4.4: unset L: variables in real MSFS report NaN, which is neither
    // 0.0 nor equal to g_lastSeq. Reject NaN so we never apply corrupt values.
    if (!validLvar(seq) || seq == 0.0 || seq == g_lastSeq) return false;
    g_lastSeq = seq;
    double x;
    x = get_named_variable_value(v.enabled);         if (validLvar(x)) s.enabled         = x > 0.5;
    x = get_named_variable_value(v.invert);          if (validLvar(x)) s.invert          = x > 0.5;
    x = get_named_variable_value(v.strobeLock);      if (validLvar(x)) s.strobeLock      = x > 0.5;
    x = get_named_variable_value(v.speedOn);         if (validLvar(x)) s.speedLimitOn    = x > 0.5;
    x = get_named_variable_value(v.speedKt);         if (validLvar(x)) s.speedLimitKt    = x;
    x = get_named_variable_value(v.throttleOn);      if (validLvar(x)) s.throttleLimitOn = x > 0.5;
    x = get_named_variable_value(v.throttle);        if (validLvar(x)) { s.throttleLimit = x; g_tLimitRead = s.throttleLimit; g_tLimitReadRaw = x; g_tLimitNaN = false; } else { g_tLimitNaN = true; }
    x = get_named_variable_value(v.deadzone);        if (validLvar(x)) s.deadzone        = x;
    x = get_named_variable_value(v.curve);           if (validLvar(x)) s.curve           = x > 0.5 ? 1 : 0;
    x = get_named_variable_value(v.smoothing);       if (validLvar(x)) s.smoothing       = x;
    x = get_named_variable_value(v.jump);            if (validLvar(x)) s.jumpResponse    = x;
    x = get_named_variable_value(v.activationEvent); if (validLvar(x)) {
        int activationEvent = (int)x;
#ifdef GS_USE_LEARN
        if (g_activationMode == MODE_BUTTON_CTRL) {
            g_reenumPending = true;
        } else {
            if (activationEvent == -1) g_activationMode = MODE_AUTOMATIC;
        }
#else
        if (activationEvent != g_activationEvent) {
            g_activationEvent = activationEvent;
        }
#endif
    }
    s.clamp();
    return true;
}

// The event API is observational: the event continues to its normal MSFS action.
// Therefore the UI must only learn an event that the user intentionally bound for this purpose.
void activationKeyEvent(FsEventId eventId, FsVarParamArray* params, void* userParam) {
    (void)params; (void)userParam;
    if (eventId == FS_EVENT_INVALID_ID) return;
    if (g_learning) {
        printf("GentleStop: learned KeyEvent id %d (armed)\n", eventId);
        g_capturedEvent = eventId;
        g_capturePending = true;
        g_learning = false;
        g_activationMode = MODE_BUTTON_CTRL;
        g_activationEvent = eventId;
        g_armed = true;
        g_gateState = true;
        g_bindingType = BINDING_KEYBOARD;  // keyboard key needs no SimConnect input registration
        snprintf(g_simInStr, sizeof(g_simInStr), "key:%u", (unsigned)eventId);
#ifdef GS_USE_LEARN
        g_actInput.cancel();
#endif
        saveSettingsFile(g_ctl.s);
        return;
    }
    if (eventId == g_activationEvent) {
        printf("GentleStop: activation KeyEvent %d\n", eventId);
        g_togglePending = true;
    }
}

// BRAKE *_POSITION is 0..1 in "position" units on most aircraft, but a few report the raw
// 0..16383 axis range (sometimes with a negative sign), so normalise before comparing.
double normalizedBrake(double v) {
    if (v < 0.0) v = -v;
    if (v > 1.5) v /= 16383.0;
    return v > 1.0 ? 1.0 : v;
}

// Learn window: watch the brake pressure the user's normal BRAKES control produces and remember the levels
// it really reaches, instead of demanding a fixed 0..1 travel. Diagnostic lines are printed (throttled) so a
// log showing an unchanged reading immediately tells us the simvar is not following the controller at all.
void observeDefaultBrake(const SimData& d) {
    double b = normalizedBrake(d.brakeLeft > d.brakeRight ? d.brakeLeft : d.brakeRight);
    if (g_learning) {
#ifdef GS_USE_LEARN
        // Approved scope: a continuous brake-pressure change must NOT win the
        // capture race - this feature learns a discrete key/button only. The
        // keyboard (fsEvents) and the controller fan stay active. Without
        // GS_USE_LEARN the historic brake-pressure learning below is
        // unchanged (the 183-check suite covers it).
        g_lastObservedBrake = b;
        return;
#endif
        if (!g_learnHaveBase) {
            g_learnBase = g_learnPeak = b; g_learnHaveBase = true;
            printf("GentleStop: learn watch b=%.2f park=%.0f\n", b, d.parking);
        } else if (b < g_learnBase) {
            g_learnBase = g_learnPeak = b; g_learnLastRise = -1.0;   // trough: the next rise is a fresh press
        } else if (b > g_learnPeak + 0.01) {
            g_learnPeak = b; g_learnLastRise = d.absTime;
        }
        if (g_learnPrints < 40 && (g_learnLastPrint < 0.0 || b > g_learnLastPrint + 0.1 || b < g_learnLastPrint - 0.1)) {
            printf("GentleStop: L=%.0f R=%.0f b=%.2f pk=%.2f\n", d.brakeLeft, d.brakeRight, b, g_learnPeak);
            g_learnPrints++; g_learnLastPrint = b;
        }
        // The parking brake drives the same pressure up and is not the user's button.
        g_lastObservedBrake = b;
        if (d.parking > 0.5) return;
        if (g_learnPeak - g_learnBase < BRAKE_LEARN_MIN_RISE || g_learnPeak < BRAKE_LEARN_MIN_PEAK) return;
        if (g_learnLastRise < 0.0 || d.absTime - g_learnLastRise < BRAKE_LEARN_SETTLE) return;  // let the press settle
        g_brakeBase = g_learnBase;
        g_brakePeak = g_learnPeak;
        recomputePressLevels();
        printf("GentleStop: learned brake %.2f..%.2f rise=%.2f (armed)\n", g_brakeBase, g_brakePeak, g_pressRise);
        g_capturedEvent = ACTIVATION_DEFAULT_BRAKE;
        g_capturePending = true;
        g_learning = false;
        // The press that taught the control already proves it works: arm now, so one press is enough.
        g_armed = true;
        g_gateState = true;
        // Do not let the tail of that same hold count as a toggle of the state we just armed.
        // (Own output cannot transmit while learning, so the residual equals the raw pressure here.)
        g_toggleFired = true;
        g_togglePeak = residualBrake(b);
        g_toggleLow = residualBrake(b);
        return;
    }
    // Press detector, run on the RESIDUAL pressure: what the brake pressure shows above the level our
    // own stick braking commanded last frame. Two states:
    //   IDLE   (g_toggleFired == false): the trough tracks the minimum residual. A press fires when the
    //          residual rises g_pressRise above the trough.
    //   PRESSED (g_toggleFired == true): the peak tracks the maximum residual. Re-arm happens only when
    //          the residual falls g_pressRise below the peak — that pressure drop is the evidence the
    //          button was released.
    // A held button therefore produces exactly one toggle: pressure stays high, so the residual never
    // reaches peak - g_pressRise and the detector never re-arms mid-hold. Two distinct taps produce two
    // toggles when the pressure signal can distinguish them (it drops below peak - g_pressRise between
    // taps). If it cannot — pressure lingers too high — the second tap is not counted: we accept that
    // limitation rather than double-toggling a single hold. Time alone is never re-arm evidence.
    // One hold is one press; back-to-back taps are not swallowed once the pressure has dropped enough.
    g_lastObservedBrake = b;
    // The parking brake drives the same pressure up and is not the user's button.
    if (d.parking > 0.5) return;
    double r = residualBrake(b);
    if (g_toggleFired) {
        if (r > g_togglePeak) g_togglePeak = r;
        if (r <= g_togglePeak - g_pressRise) { g_toggleFired = false; g_toggleLow = r; }
        return;
    }
    if (r < g_toggleLow) g_toggleLow = r;
    double rise = r - g_toggleLow;
    if (rise < g_pressRise) {
        // IDLE and below threshold: occasionally log residual vs threshold so the sim console shows
        // how close a physical tap gets to firing. Bounded to one print per 0.5 s.
        if (d.absTime - g_lastDiagPrint > 0.5) { g_lastDiagPrint = d.absTime;
            printf("GentleStop: diag residual=%.2f trough=%.2f rise=%.2f threshold=%.2f state=idle panelCmds=%d\n",
                   r, g_toggleLow, rise, g_pressRise, g_panelCmds); }
        return;
    }
    g_toggleFired = true;
    g_togglePeak = r;
    // A press while this module is itself stick-braking is exactly how the user tells the module to
    // stop: the residual separates it and the press toggles the arm state like any other press.
    if (g_activationEvent != ACTIVATION_DEFAULT_BRAKE) {
        // The learned binding is a Key Event: this pressure press belongs to the default brake control.
        if (d.absTime - g_lastSkipPrint > 0.2) { g_lastSkipPrint = d.absTime; printf("GentleStop: press seen, activation=%d\n", g_activationEvent); }
        return;
    }
    if (d.absTime - g_lastSkipPrint > 0.2) { g_lastSkipPrint = d.absTime; printf("GentleStop: brake pressed (rise %.2f threshold=%.2f trough=%.2f peak=%.2f%s)\n", rise, g_pressRise, g_toggleLow, r, g_ctl.gate ? " braking" : ""); }
    g_togglePending = true;
    return;
}

// ---------------------------------------------------------------- brakes
void sendBrake(int value) {
    if (!g_sc) return;
    DWORD data = (DWORD)(int32_t)value;
    SimConnect_TransmitClientEvent(g_sc, SIMCONNECT_OBJECT_ID_USER, EVT_LEFT_BRAKE, data,
                                   SIMCONNECT_GROUP_PRIORITY_HIGHEST, SIMCONNECT_EVENT_FLAG_GROUPID_IS_PRIORITY);
    SimConnect_TransmitClientEvent(g_sc, SIMCONNECT_OBJECT_ID_USER, EVT_RIGHT_BRAKE, data,
                                   SIMCONNECT_GROUP_PRIORITY_HIGHEST, SIMCONNECT_EVENT_FLAG_GROUPID_IS_PRIORITY);
}

void OnFrame(const SimData& d) {
    // dt from sim time, same clamping as the old JS loop
    double dt = 0.016;
    if (g_haveTime) {
        dt = d.absTime - g_lastTime;
        if (!(dt > 0.0)) dt = 0.016;
        if (dt > 0.2) dt = 0.2;
    }
    g_haveTime = true;
    g_lastTime = d.absTime;

    // Cache sim telemetry for status messages (Task C: panel telemetry).
    g_lastStick     = d.yokeY;
    g_lastSimBrake  = d.brakeLeft > d.brakeRight ? d.brakeLeft : d.brakeRight;
    g_lastThrottlePct = d.throttle1 > d.throttle2 ? d.throttle1 : d.throttle2;
    g_lastAirspeedKt = d.airspeedKt;

    observeDefaultBrake(d);

    if (pollPanelSettings(g_ctl.s)) { g_saveDue = true; g_saveAt = d.absTime + 1.0; }
    double learnSeq = get_named_variable_value(v.learnSeq);
    if (learnSeq != 0.0 && learnSeq != g_lastLearnSeq) {
        g_lastLearnSeq = learnSeq;
        bool shouldLearn = get_named_variable_value(v.learnArm) > 0.5;
        g_capturePending = false;
        if (shouldLearn && !g_learning) {
            startLearn(d.absTime);
            set_named_variable_value(v.learnResult, (double)FS_EVENT_INVALID_ID);
#ifdef GS_USE_LEARN
            g_fanArmed = false;
            g_captNoIdent = false;
            if (g_sc) SimConnect_EnumerateControllers(g_sc);
#endif
        } else if (!shouldLearn && g_learning) {
            rollbackLearn();
        }
    }
#ifdef GS_USE_LEARN
    if (g_learning && g_actInput.result().captured) {
        const gslearn::LearnResult& r = g_actInput.result();
        unsigned capDev = 0;
        int capBtn = -1;
        if (parseSimInput(r.input.c_str(), &capDev, &capBtn)) {
            int slot = -1;
            for (int i = 0; i < g_nCtlList && slot < 0; ++i)
                if (g_ctlList[i].deviceId == capDev) slot = i;
            if (slot >= 0) {
                snprintf(g_candidateBinding.simInStr, sizeof(g_candidateBinding.simInStr), "%s", r.input.c_str());
                snprintf(g_candidateBinding.simDevName, sizeof(g_candidateBinding.simDevName), "%s", g_ctlList[slot].name);
                g_candidateBinding.simDevPid = g_ctlList[slot].productId;
                g_candidateBinding.simDevCid = g_ctlList[slot].compositeId;
                g_candidateBinding.simDevHw = g_ctlList[slot].hw;

                char persisted[48];
                if (g_actInput.confirm(persisted, sizeof(persisted), d.absTime)) {
                    // Commit candidate binding
                    memcpy(g_simInStr, g_candidateBinding.simInStr, sizeof(g_simInStr));
                    memcpy(g_simDevName, g_candidateBinding.simDevName, sizeof(g_simDevName));
                    g_simDevPid = g_candidateBinding.simDevPid;
                    g_simDevCid = g_candidateBinding.simDevCid;
                    g_simDevHw = g_candidateBinding.simDevHw;

                    g_activationMode = MODE_BUTTON_CTRL;
                    g_learning = false;
                    // Preserve pre-learn armed state
                    g_armed = g_learnBackup.armed;
                    g_gateState = g_armed;
                    g_bindingType = BINDING_JOYSTICK;   // joystick binding needs registration-tracking
                    g_ghostExpected = true;
                    g_ghostUntil = d.absTime + 0.5;
                    g_bindRestored = false;
                    g_bindRegistered = true;
                    g_matchCount = 1;
                    g_simState = 2; // registered

                    g_capturedEvent = ACTIVATION_SIMINPUT;
                    g_capturePending = true;
                    saveSettingsFile(g_ctl.s);
                    printf("GentleStop: learn committed -> '%s', mode=MODE_BUTTON_CTRL, armed=%d\n",
                           g_simInStr, g_armed ? 1 : 0);
                } else {
                    printf("GentleStop: capture confirm failed (budget?); rolling back\n");
                    rollbackLearn();
                }
            } else if (!g_captNoIdent) {
                g_captNoIdent = true;
                printf("GentleStop: captured device left the list; rolling back\n");
                rollbackLearn();
            }
        } else {
            printf("GentleStop: unexpected capture type; rolling back\n");
            rollbackLearn();
        }
    }
#endif
    if (g_capturePending) {
        g_activationEvent = g_capturedEvent;
        g_capturePending = false;
        g_learnResultSeq += 1.0;
        set_named_variable_value(v.learnResult, (double)g_capturedEvent);
        set_named_variable_value(v.learnResultSeq, g_learnResultSeq);
        g_saveDue = true;
        g_saveAt = d.absTime + 1.0;
#ifdef GS_USE_LEARN
        g_settingsSeq++;
        g_simState = (g_activationMode == MODE_BUTTON_CTRL) ? 2 : 0;
#endif
    }
#ifdef GS_USE_LEARN
    g_actInput.tick(d.absTime);
    if (g_learning && g_actInput.result().timedOut) {
        printf("GentleStop: learn window timed out\n");
        rollbackLearn();
    }
    if (g_reenumPending) {   // D3: re-enumerate on the panel-open proxy only
        g_reenumPending = false;
        if (g_sc) SimConnect_EnumerateControllers(g_sc);
    }
    // Bounded periodic controller re-enumeration: while the saved binding is
    // unavailable (g_simState == 4) and we are not learning, re-enumerate every
    // g_reenumInterval seconds so a reconnected previously-learned controller
    // is re-bound automatically. Exactly one matching device binds via
    // restoreFromList(); 0 matches stays unavailable, >= 2 stays ambiguous
    // (never silently rebound). Retries stop once the bind succeeds (state 2/3)
    // or learning starts.
    if (g_simState == 4 && g_sc && !g_learning && d.absTime - g_reenumLastAt >= g_reenumInterval) {
        // Task A diagnostic: log re-enum trigger and the SimConnect result code.
        printf("GentleStop: re-enum t=%.2f state=4 interval=%.1f\n",
               d.absTime, g_reenumInterval);
        g_reenumLastAt = d.absTime;
        HRESULT hr = SimConnect_EnumerateControllers(g_sc);
        printf("GentleStop: re-enum SimConnect_EnumerateControllers -> 0x%08X%s\n",
               (unsigned)hr, FAILED(hr) ? "  <-- FAILED" : "");
    }
#endif
    if (g_togglePending) {
        g_togglePending = false;
        if (g_activationMode == MODE_BUTTON_CTRL && bindingReady()) {
            g_armed = !g_armed;
            g_gateState = g_armed;
            saveSettingsFile(g_ctl.s);
            printf("GentleStop: activation armed -> %s\n", g_armed ? "on" : "off");
        } else if (g_activationEvent != FS_EVENT_INVALID_ID && g_activationEvent != ACTIVATION_SIMINPUT) {
            // Keyboard (fsEvent) binding: bindingReady() covers it once
            // g_simInStr is set; this branch is a safety net for any path that
            // sets g_activationEvent without populating g_simInStr.
            g_armed = !g_armed;
            g_gateState = g_armed;
            saveSettingsFile(g_ctl.s);
            printf("GentleStop: activation armed (key) -> %s\n", g_armed ? "on" : "off");
        }
    }
    if (g_saveDue && (d.absTime >= g_saveAt || d.absTime < g_saveAt - 5.0)) {   // debounce slider drags
        saveSettingsFile(g_ctl.s);
        g_saveDue = false;
    }

    bool allowed = activationAllowed();
#ifdef GS_USE_ELEV_SUPPRESS
    // Ground-braking pitch isolation (staging-only): suppress exactly while
    // armed (enabled + activation) on the ground, independent of the brake
    // gate. Any other state (airborne, disarmed, cleared, learning, module
    // teardown) hands the elevator straight back to normal control.
    bool elevEligible = g_ctl.s.enabled && allowed && d.onGround == 1.0;
    gs_elev::update(elevEligible, d.absTime);
    // While the axis is consumed by the mask, YOKE Y POSITION may stop
    // tracking the physical stick; feed the braking input from the RECEIVED
    // axis value instead so braking keeps working (no elevator write).
    double yokeIn = (gs_elev::active() && gs_elev::haveAxis())
                        ? gs_elev::axisToYoke(gs_elev::lastAxis())
                        : d.yokeY;
    gs::Inputs in = { yokeIn, d.onGround, d.airspeedKt, d.strobe, d.throttle1, d.throttle2, d.parking, allowed };
#else
    gs::Inputs in = { d.yokeY, d.onGround, d.airspeedKt, d.strobe, d.throttle1, d.throttle2, d.parking, allowed };
#endif
#ifdef GS_YAXIS_PROBE
    // Diagnostic-only periodic summary of the received axis events. No
    // effect whatsoever on the braking pipeline: gs::Inputs stays untouched.
    gs_yprobe::update(d.absTime);
#endif
    g_ctl.step(in, dt);
    // Task B / Stage 4.3 diagnostic: report when the braking controller blocks
    // because the throttle is at/above the configured limit, with the actual
    // limit in use. Also capture panel-visible snapshot.
    g_tBlockNow = (g_ctl.reason == gs::BLOCK_THROTTLE);
    if (g_tBlockNow && d.absTime - g_lastBlockPrint > 0.5) {
        g_lastBlockPrint = d.absTime;
        g_tLimitBlock = g_ctl.s.throttleLimit;
        g_tPctAtBlock = gs::mx(gs::nz(d.throttle1), gs::nz(d.throttle2));
        g_tBlockTime = d.absTime;
        printf("GentleStop: block throttle throttlePct=%.1f limit=%.1f\n",
               g_tPctAtBlock, g_tLimitBlock);
    }
#ifdef GS_USE_LEARN
    // Adaptive status cadence (stage-1 bar-display fix, staging only): the
    // panel's bars follow the already-smoothed brake value, and its 250 ms
    // poll alone hops visibly during a ~0.3 s transient. While the brake
    // value is moving, broadcast a status snapshot at most every 25 ms (the
    // transient can move at ~11/s, so the throttle is what bounds the gaps,
    // not the 2% threshold) and only when it moved >= 2% since the last one;
    // idle moments send nothing extra. sendPanelStatus is a single snprintf
    // + CommBus call - each slam amortizes to a few dozen calls.
    if (fabs(g_ctl.brake - g_statusSentBrake) >= 0.02 && d.absTime - g_statusSentAt >= 0.025) {
        g_statusSentAt = d.absTime;
        g_statusSentBrake = g_ctl.brake;
        sendPanelStatus();
    }
#endif

    // Same write policy as the old JS: only touch the sim's brakes while braking, plus one release.
    bool isBraking = g_ctl.brake > 0.005;
    if (isBraking || g_wasBraking) {
        g_wasBraking = isBraking;
        sendBrake(gs::Controller::axisValue(isBraking ? g_ctl.brake : 0.0));
    }
    // Expected pressure from our own writes: BRAKE *_POSITION bites our command fast but lingers above it
    // while it falls (and after the release the sim drains the brakes on its own), so the expectation is
    // the running maximum of the command and the previous expectation. It only decays (0.90 is slower than
    // the real drain) and it FREEZES while a press is being consumed, so a still-held user press cannot
    // look like a second press as the expectation decays underneath it.
    double cmd = isBraking ? g_ctl.brake : 0.0;
    double decayed = g_ownSent * 0.90;
    g_ownSent = cmd > decayed ? cmd : (g_toggleFired ? g_ownSent : decayed);

    g_frames += 1.0;
    if (g_frames > 1.0e9) g_frames = 1.0;
    set_named_variable_value(v.outBrake, g_ctl.brake);
    set_named_variable_value(v.outBlock, (double)g_ctl.reason);
    set_named_variable_value(v.outGate, g_ctl.gate ? 1.0 : 0.0);
    set_named_variable_value(v.outActivation, allowed ? 1.0 : 0.0);
    set_named_variable_value(v.outLearning, g_learning ? 1.0 : 0.0);
    set_named_variable_value(v.alive, g_frames);
}

void Shutdown() {
#ifdef GS_USE_LEARN
    g_actInput.cancel();   // reset learn/binding state so a restart re-establishes mappings
#endif
#ifdef GS_USE_ELEV_SUPPRESS
    gs_elev::shutdown();   // release the pitch mask before anything closes
#endif
#ifdef GS_YAXIS_PROBE
    gs_yprobe::shutdown(); // remove the diagnostic input mappings (own client)
#endif
    if (g_commBusRegistered) {
        fsCommBusUnregister("GentleStopCommand", panelCommand);
        g_commBusRegistered = false;
    }
    if (g_keyHandlerRegistered) {
        fsEventsUnregisterKeyEventHandler(activationKeyEvent, nullptr);
        g_keyHandlerRegistered = false;
    }
    if (g_sc) {
        if (g_wasBraking) { sendBrake(gs::Controller::axisValue(0.0)); g_wasBraking = false; }
        if (g_saveDue) { saveSettingsFile(g_ctl.s); g_saveDue = false; }
        SimConnect_Close(g_sc);
        g_sc = 0;
    }
}

// gsdbg helper (staging diagnostics): attribute the packet the sim just
// assigned to the call that follows, so a later SIMCONNECT_RECV_EXCEPTION
// (which carries only a numeric send id) can name its call.
#ifdef GS_USE_LEARN
static void g_sendAnnounce(const char* what) {
    DWORD sid = 0;
    if (SimConnect_GetLastSentPacketID(g_sc, &sid) == S_OK) {
        g_simApi.tagCall(what);
        printf("GentleStop: send %u: %.40s\n", (unsigned)sid, what);
    } else {
        printf("GentleStop: send ?: %.40s\n", what);
    }
}
#endif

void CALLBACK GentleStopDispatch(SIMCONNECT_RECV* pData, DWORD cbData, void* pContext) {
    (void)cbData; (void)pContext;
    g_msgs += 1.0;
    if (g_msgs > 1.0e9) g_msgs = 1.0;
    set_named_variable_value(v.dbgMsgs, g_msgs);
    if (g_msgs <= 5.0) printf("GentleStop: dispatch #%d, recv id %u\n", (int)g_msgs, (unsigned)pData->dwID);
    switch (pData->dwID) {
    case SIMCONNECT_RECV_ID_SIMOBJECT_DATA: {
        const SIMCONNECT_RECV_SIMOBJECT_DATA* o = (const SIMCONNECT_RECV_SIMOBJECT_DATA*)pData;
        if (o->dwRequestID == REQ_SIMDATA) {
            if (o->dwDefineID != DEF_SIMDATA) {
                if (g_frames < 1.0) printf("GentleStop: UNEXPECTED define ID %u (expected %u)\n", (unsigned)o->dwDefineID, DEF_SIMDATA);
            } else if (o->dwDefineCount != 10) {
                if (g_frames < 1.0) printf("GentleStop: UNEXPECTED define count %u (expected 10)\n", (unsigned)o->dwDefineCount);
            } else {
                if (g_frames < 1.0) printf("GentleStop: first data frame (define %u, %u values)\n", (unsigned)o->dwDefineID, (unsigned)o->dwDefineCount);
                OnFrame(*(const SimData*)&o->dwData);
            }
        }
        break;
    }
    case SIMCONNECT_RECV_ID_EXCEPTION: {
        const SIMCONNECT_RECV_EXCEPTION* e = (const SIMCONNECT_RECV_EXCEPTION*)pData;
        set_named_variable_value(v.dbgExc, (double)e->dwException);
        printf("GentleStop: SimConnect exception %u (send id %u, index %u)\n",
               (unsigned)e->dwException, (unsigned)e->dwSendID, (unsigned)e->dwIndex);
#ifdef GS_USE_LEARN
        if (e->dwException == 9)   // EVENT_ID_DUPLICATE: must never happen
            printf("GentleStop: duplicate event id! report this\n");
        {   // attribute the packet with the adapter's annotation ring
            char cause[48];
            if (g_simApi.lookup(e->dwSendID, cause, sizeof(cause)))
                printf("GentleStop: cause (send %u): %s\n",
                       (unsigned)e->dwSendID, cause);
            else {
                // An untagged helper send between two tagged calls is still
                // identified by its bracketing neighbours.
                const char* aText = 0; int aId = 0;
                const char* bText = 0; int bId = 0;
                if (g_simApi.lookupNear(e->dwSendID, &aText, &aId, &bText, &bId)) {
                    printf("GentleStop: cause?? send %u unannotated; sits between\n",
                           (unsigned)e->dwSendID);
                    if (aText) printf("GentleStop:   '%.46s' (send %d) and", aText, aId);
                    if (bText) printf("GentleStop:   '%.46s' (send %d)"
                            "; index %u\n", bText, bId, (unsigned)e->dwIndex);
                } else {
                    printf("GentleStop: cause?? PLEASE note lines above the exception here (send %u unannotated; index %u)\n",
                           (unsigned)e->dwSendID, (unsigned)e->dwIndex);
                }
            }
        }
#endif
        break;
    }
#ifdef GS_USE_LEARN
    case SIMCONNECT_RECV_ID_CONTROLLERS_LIST:
        noteControllersList((const SIMCONNECT_RECV_CONTROLLERS_LIST*)pData);
        break;
    case SIMCONNECT_RECV_ID_EVENT: {
        const SIMCONNECT_RECV_EVENT* ev = (const SIMCONNECT_RECV_EVENT*)pData;
        int design = g_simApi.simToDesign((int)ev->uEventID);
        if (design >= 0) {
            // Stage 4.3: count physical press down-events received in the
            // production (bound) phase, before the state machine consumes them.
            if (design == gslearn::EVT_ACT_DOWN &&
                g_actInput.phase() == gslearn::LEARN_PROD) {
                g_pressRx++;
            }
            gslearn::LearnResult r = g_actInput.onInputEvent(design, g_lastTime);
            // Track whether the last press was rejected (anomaly: down without
            // an intervening up). A successful toggle clears the rejection flag.
            if (r.anomaly && !r.toggled) g_pressRejected = true;
            else if (r.toggled) g_pressRejected = false;
            bool canVerify = (g_activationMode == MODE_BUTTON_CTRL) &&
                             (g_simInStr[0] != 0) &&
                             g_bindRegistered &&
                             (g_matchCount == 1);

            bool ghostOpen = g_ghostExpected && g_lastTime <= g_ghostUntil;
            if (ghostOpen) {
                g_ghostExpected = false;
                if (r.toggled && g_activationMode == MODE_BUTTON_CTRL) {
                    if (canVerify && g_simState < 3) {
                        g_simState = 3;
                        printf("GentleStop: sim input verified (delivered)\n");
                    }
                    printf("GentleStop: ghost press after rebind ignored\n");
                    break;
                }
            }

            if (r.toggled && g_activationMode == MODE_BUTTON_CTRL) {
                if (canVerify && g_simState < 3) {
                    g_simState = 3;
                    printf("GentleStop: sim input verified (delivered)\n");
                }
                g_togglePending = true;
            }
        }
        break;
    }
#endif
    case SIMCONNECT_RECV_ID_QUIT:
        Shutdown();
        break;
    default:
        break;
    }
}

} // namespace

namespace {
bool g_setupFailed = false;
// Logs the HRESULT of every SimConnect setup call (printf shows up in the sim's WASM console).
void step(const char* what, HRESULT hr) {
    printf("GentleStop: %s -> 0x%08X%s\n", what, (unsigned)hr, FAILED(hr) ? "  <-- FAILED" : "");
    if (FAILED(hr)) g_setupFailed = true;
}
}

// Forward declaration: resetModuleState is defined after module_deinit().
extern "C" void resetModuleState(void);

extern "C" MSFS_CALLBACK void module_init(void) {
#ifdef GS_HOST_TEST
    resetModuleState();   // clean runtime baseline before loading settings (host-test isolation only)
#endif
    registerVars();
    // FIRST observable effect: proves the sim loaded + instantiated + called module_init (panel: "initialised").
    set_named_variable_value(v.alive, -1.0);
    printf("GentleStop: module_init entered (build %s %s)\n", __DATE__, __TIME__);
    loadSettingsFile(g_ctl.s);
    recomputePressLevels();
#ifdef GS_USE_LEARN
    // Task A diagnostic: log initial simState after settings load.
    printf("GentleStop: init simState=%d mode=%d armed=%d simInStr='%.40s'\n",
           g_simState, g_activationMode, g_armed ? 1 : 0, g_simInStr);
#endif
    g_commBusRegistered = fsCommBusRegister("GentleStopCommand", panelCommand, nullptr);
    printf("GentleStop: CommBus register -> %s\n", g_commBusRegistered ? "ok" : "failed");
    fsEventsRegisterKeyEventHandler(activationKeyEvent, nullptr);
    g_keyHandlerRegistered = true;

    HRESULT hr = SimConnect_Open(&g_sc, "GentleStop", nullptr, 0, 0, 0);
    printf("GentleStop: SimConnect_Open -> 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) {
        printf("GentleStop: SimConnect_Open failed\n");
        set_named_variable_value(v.alive, -2.0);
        g_sc = 0;
        return;
    }
#ifdef GS_USE_LEARN
    g_simApi.setHandle(g_sc);   // annotations valid from here on
    // Stage-1 exception diagnosis: record which raw setup call each send
    // packet belongs to (the learn-fan/learn adapter already annotates its
    // own; fewer lines later than a full ring dump on every exception).
    #define GS_TAG(what) g_sendAnnounce(what)
    #define GS_ADD(name, unit) { step("AddToDataDefinition " name, SimConnect_AddToDataDefinition(g_sc, DEF_SIMDATA, name, unit, SIMCONNECT_DATATYPE_FLOAT64)); GS_TAG("AddToDataDefinition " name); }
#else
#define GS_ADD(name, unit) step("AddToDataDefinition " name, SimConnect_AddToDataDefinition(g_sc, DEF_SIMDATA, name, unit, SIMCONNECT_DATATYPE_FLOAT64))
#endif
    GS_ADD("YOKE Y POSITION", "position");
    GS_ADD("SIM ON GROUND", "bool");
    GS_ADD("AIRSPEED INDICATED", "knots");
    GS_ADD("LIGHT STROBE", "bool");
    GS_ADD("GENERAL ENG THROTTLE LEVER POSITION:1", "percent");
    GS_ADD("GENERAL ENG THROTTLE LEVER POSITION:2", "percent");
    GS_ADD("BRAKE PARKING POSITION", "bool");
    GS_ADD("BRAKE LEFT POSITION", "position");
    GS_ADD("BRAKE RIGHT POSITION", "position");
    GS_ADD("ABSOLUTE TIME", "seconds");
#undef GS_ADD

    step("MapClientEventToSimEvent AXIS_LEFT_BRAKE_SET",  SimConnect_MapClientEventToSimEvent(g_sc, EVT_LEFT_BRAKE,  "AXIS_LEFT_BRAKE_SET"));
#ifdef GS_USE_LEARN
    GS_TAG("MapClientEvent AXIS_LEFT_BRAKE_SET");
#endif
    step("MapClientEventToSimEvent AXIS_RIGHT_BRAKE_SET", SimConnect_MapClientEventToSimEvent(g_sc, EVT_RIGHT_BRAKE, "AXIS_RIGHT_BRAKE_SET"));
#ifdef GS_USE_LEARN
    GS_TAG("MapClientEvent AXIS_RIGHT_BRAKE_SET");
#endif
    step("RequestDataOnSimObject (SIM_FRAME)", SimConnect_RequestDataOnSimObject(g_sc, REQ_SIMDATA, DEF_SIMDATA,
                                      SIMCONNECT_OBJECT_ID_USER, SIMCONNECT_PERIOD_SIM_FRAME));
#ifdef GS_USE_LEARN
    GS_TAG("RequestDataOnSimObject");
#endif
    step("CallDispatch", SimConnect_CallDispatch(g_sc, GentleStopDispatch, nullptr));

#ifdef GS_USE_ELEV_SUPPRESS
    // Staging-only feature: ground braking without unwanted nose-down pitch.
    // The mask lives on ITS OWN SimConnect client: a failure here (or the
    // main setup above having failed) skips/disables the feature and the
    // stock elevator routing is untouched; the module keeps working.
    if (g_setupFailed)
        printf("GentleStop: elev: skipped (main SimConnect setup failed)\n");
    else
        gs_elev::init();
#endif

#ifdef GS_YAXIS_PROBE
    // Staging-only diagnostic: receive-only raw Y-axis capture. Failure here
    // (or in the setups above) only silences the probe; nothing else changes.
    if (g_setupFailed)
        printf("GentleStop: probe: skipped (main SimConnect setup failed)\n");
    else
        gs_yprobe::init();
#endif

    if (g_setupFailed) {
        set_named_variable_value(v.alive, -3.0);
        printf("GentleStop: a SimConnect setup call failed (see lines above)\n");
        return;
    }
#ifdef GS_USE_LEARN
    if (g_activationMode == MODE_BUTTON_CTRL) {
        if (g_simInStr[0]) {
            printf("GentleStop: saved input '%.40s' - enumerating\n", g_simInStr);
            HRESULT hr = SimConnect_EnumerateControllers(g_sc);
            printf("GentleStop: EnumerateControllers (restore) -> 0x%08X%s\n",
                   (unsigned)hr, FAILED(hr) ? "  <-- FAILED" : "");
            GS_TAG("EnumerateControllers (restore)");
        } else {
            printf("GentleStop: mode -3 without saved input -> automatic mode\n");
            g_activationMode = MODE_AUTOMATIC;
        }
    }
#endif
    printf("GentleStop module ready\n");   // alive stays -1 until the first data frame arrives
}

extern "C" MSFS_CALLBACK void module_deinit(void) {
    Shutdown();
}

// Reset all runtime module state to defaults. Called at the start of
// module_init() to provide a clean baseline before loadSettingsFile() restores
// saved values from the config file (persistence scenario) or applies defaults
// (independent scenario with config removed).
extern "C" void resetModuleState(void) {
    g_activationMode = MODE_AUTOMATIC;
    g_armed = true;
    g_gateState = true;
    g_activationEvent = FS_EVENT_INVALID_ID;
    g_simInStr[0] = 0;
    g_bindingType = BINDING_NONE;
    g_bindRegistered = false;
    g_bindRestored = false;
    g_bindingLoaded = false;
    g_simState = 0;
    g_learning = false;
    g_capturePending = false;
    g_capturedEvent = FS_EVENT_INVALID_ID;
    g_togglePending = false;
    g_pressRx = 0;
    g_pressRejected = false;
    g_fanArmed = false;
    g_reenumPending = false;
    g_ghostExpected = false;
    g_ghostUntil = 0.0;
    g_ctlCount = 0;
    g_nCtlList = 0;
    g_ctlTruncated = false;
    g_matchCount = 0;
    g_resolvedDevId = 0;
    g_regAttempted = false;
    g_regOk = false;
    g_regHr = 0;
    g_enumEntry = 0;
    g_enumOutOf = 1;
    g_enumComplete = false;
    g_enumReset = true;
    g_simDevName[0] = 0;
    g_simDevPid = 0;
    g_simDevCid = 0;
    g_simDevHw.Major = g_simDevHw.Minor = g_simDevHw.Revision = g_simDevHw.Build = 0;
    for (unsigned i = 0; i < GS_CTL_LIST_MAX; ++i) {
        g_ctlList[i].deviceId = 0;
        g_ctlList[i].productId = 0;
        g_ctlList[i].compositeId = 0;
        g_ctlList[i].name[0] = 0;
        g_ctlList[i].hw.Major = 0; g_ctlList[i].hw.Minor = 0;
        g_ctlList[i].hw.Revision = 0; g_ctlList[i].hw.Build = 0;
    }
    g_ctl = gs::Controller();
    g_brakeBase = 0.0;
    g_brakePeak = 1.0;
    g_pressRise = PRESS_RISE_MIN;
    g_toggleLow = 0.0;
    g_togglePeak = 0.0;
    g_toggleFired = false;
    g_learnBase = 0.0; g_learnPeak = 0.0; g_learnLastRise = -1.0;
    g_learnHaveBase = false;
    g_ownSent = 0.0;
    g_lastStick = 0.0;
    g_lastSimBrake = 0.0;
    g_lastThrottlePct = 0.0;
    g_lastAirspeedKt = 0.0;
    // g_actInput.cancel() is already called from Shutdown(); no need to repeat.
}
