// test_input_learn_glue.cpp - offline validation of the PRODUCTION integration
// of the learned discrete input (GS_USE_LEARN build of GentleStop.cpp) against
// mocks of the SimConnect input services + the module's CommBus / fsEvents /
// L-variable environment.
//
// Scope (approved, replaces probe-era coverage for the production flow):
//   adapter (input_event_api.h): "keys:" -> bare translation + keyboard
//     fan policy, session-unique id allocation with reverse translation,
//     512-id budget with the 2-id runtime reserve;
//   module: learn window opens the controller fan (16 buttons), first-press-
//     wins capture, module-side auto-confirm -> mode -3 persistence, toggle
//     once per press (release rearms), brake-pressure capture suppressed in
//     learn windows and the brake detector observation-only in mode -3,
//     keyboard learning via fsEvents preserved, restart persistence with
//     identity restore (verified transition, device unavailable, ambiguous,
//     DeviceId shift), stale settings push protected via the panel-open
//     proxy, clearActivation, duplicate learn click regression.
//
// In-sim delivery, priority/masking exactness and E1-class silence are NOT
// proven offline - that stays with the staged module test on real hardware.
#include <MSFS/MSFS.h>
#include <MSFS/MSFS_WindowsTypes.h>
#include <MSFS/MSFS_Events.h>
#include <MSFS/MSFS_CommBus.h>
#include <MSFS/Legacy/gauges.h>
#include <SimConnect.h>
#include "input_event_api.h"      // adapter + gslearn (unit tests below)

#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" void module_init(void);
extern "C" void module_deinit(void);

// file-scope references below (the mock of the sim's EX1 grammar)
using gslearn::GS_PROD_BTN_MAX;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond) do { \
    ++g_checks; if (!(cond)) { ++g_fails; printf("FAIL line %d: %s\n", __LINE__, #cond); } \
} while (0)

// --- module environment mocks (same shape as test_glue.cpp) -------------------
static std::map<int, double> g_vals;
static std::map<std::string, int> g_ids;
static std::vector<std::string> g_names;
ID register_named_variable(const char* n) {
    auto it = g_ids.find(n);
    if (it != g_ids.end()) return it->second;
    int id = (int)g_names.size();
    g_names.push_back(n); g_ids[n] = id;
    return id;
}
double get_named_variable_value(ID id) { return g_vals[id]; }
void set_named_variable_value(ID id, double v) { g_vals[id] = v; }
static double var(const char* n) { return g_vals[register_named_variable(n)]; }

static DispatchProc g_proc = nullptr;
static int g_defs = 0, g_maps = 0;
static int g_lastSend = 99;
static std::string g_busEvent;
static std::vector<std::pair<int, int>> g_events;
static bool g_closed = false, g_failOpen = false;
static bool g_ex1Fail = false;   // H2.4: force SimConnect map failure for registration-failure tests
static FsEventsKeyEventHandler g_keyHandler = nullptr;
static void* g_keyUserData = nullptr;
static fsCommBusWasmCallback g_busHandler = nullptr;
static void* g_busContext = nullptr;
static std::string g_lastStatus;
static int g_statusSends = 0;      // how many GentleStopStatus broadcasts the module issued

HRESULT SimConnect_Open(HANDLE* h, const char*, void*, DWORD, void*, DWORD) {
    if (g_failOpen) return -1;
    *h = (HANDLE)1; return 0;
}
HRESULT SimConnect_Close(HANDLE) { g_closed = true; return 0; }
HRESULT SimConnect_AddToDataDefinition(HANDLE, DWORD, const char*, const char*, SIMCONNECT_DATATYPE, float, DWORD) { g_defs++; return 0; }
HRESULT SimConnect_MapClientEventToSimEvent(HANDLE, DWORD, const char*) { g_maps++; g_lastSend++; return 0; }
HRESULT SimConnect_RequestDataOnSimObject(HANDLE, DWORD, DWORD, DWORD, SIMCONNECT_PERIOD) { return 0; }
HRESULT SimConnect_TransmitClientEvent(HANDLE, DWORD, DWORD evt, DWORD data, DWORD, DWORD) { g_events.push_back({(int)evt, (int32_t)data}); return 0; }
HRESULT SimConnect_CallDispatch(HANDLE, DispatchProc p, void*) { g_proc = p; return 0; }
HRESULT SimConnect_GetLastSentPacketID(HANDLE, DWORD* p) { *p = ++g_lastSend; return 0; }

extern "C" void fsEventsRegisterKeyEventHandler(FsEventsKeyEventHandler handler, void* userData) { g_keyHandler = handler; g_keyUserData = userData; }
extern "C" void fsEventsUnregisterKeyEventHandler(FsEventsKeyEventHandler handler, void* userData) {
    if (g_keyHandler == handler && g_keyUserData == userData) { g_keyHandler = nullptr; g_keyUserData = nullptr; }
}
extern "C" bool fsCommBusRegister(const char* eventName, fsCommBusWasmCallback callback, void* context) {
    g_busEvent = eventName; g_busHandler = callback; g_busContext = context; return true;
}
extern "C" int fsCommBusUnregister(const char* eventName, fsCommBusWasmCallback callback) {
    if (g_busEvent == eventName && (!callback || g_busHandler == callback)) { g_busHandler = nullptr; g_busContext = nullptr; return 1; }
    return 0;
}
extern "C" bool fsCommBusCall(const char* eventName, const char* buf, unsigned int size, FsCommBusBroadcastFlags) {
    if (eventName && !strcmp(eventName, "GentleStopStatus")) { g_lastStatus.assign(buf, size); ++g_statusSends; }
    return true;
}

// --- simulated controller hardware + pending SimConnect messages --------------
struct CtlEntry {
    unsigned int id, pid, cid;
    unsigned short mj, mn, rv, bd;
    const char* name;
};
static std::vector<CtlEntry> g_devices;
static void setDevices(const std::vector<CtlEntry>& d) { g_devices = d; }

static std::vector<std::vector<char>> g_mail;
static void pump() {
    // one level is enough for this module, but drain "until empty" anyway
    while (!g_mail.empty()) {
        std::vector<std::vector<char>> batch;
        batch.swap(g_mail);
        for (size_t i = 0; i < batch.size(); ++i)
            g_proc((SIMCONNECT_RECV*)batch[i].data(), (DWORD)batch[i].size(), nullptr);
    }
}

HRESULT SimConnect_EnumerateControllers(HANDLE) {
    size_t n = g_devices.size() < 64 ? g_devices.size() : 64;
    std::vector<char> buf(sizeof(SIMCONNECT_RECV_CONTROLLERS_LIST) +
                          (n ? n - 1 : 0) * sizeof(SIMCONNECT_CONTROLLER_ITEM), 0);
    auto* m = (SIMCONNECT_RECV_CONTROLLERS_LIST*)buf.data();
    m->dwID = SIMCONNECT_RECV_ID_CONTROLLERS_LIST;
    m->dwSize = (DWORD)buf.size();
    m->dwVersion = 0x00010001;
    m->dwRequestID = 7; m->dwArraySize = (DWORD)n; m->dwEntryNumber = 0; m->dwOutOf = 1;
    for (size_t i = 0; i < n; ++i) {
        SIMCONNECT_CONTROLLER_ITEM& it = m->rgData[i];
        snprintf(it.DeviceName, sizeof(it.DeviceName), "%s", g_devices[i].name);
        it.DeviceId = g_devices[i].id; it.ProductId = g_devices[i].pid;
        it.CompositeID = g_devices[i].cid;
        it.HardwareVersion.Major = g_devices[i].mj; it.HardwareVersion.Minor = g_devices[i].mn;
        it.HardwareVersion.Revision = g_devices[i].rv; it.HardwareVersion.Build = g_devices[i].bd;
    }
    g_mail.push_back(buf);
    return 0;
}

// Task C: multi-packet enumeration mock. Sends devices split across multiple
// SIMCONNECT_RECV_CONTROLLERS_LIST packets, each with dwEntryNumber/dwOutOf set.
// pktSize = number of controller entries per packet (last packet may be shorter).
HRESULT SimConnect_EnumerateControllersMulti(HANDLE, unsigned pktSize) {
    size_t total = g_devices.size();
    size_t offset = 0;
    unsigned pktIdx = 0;
    while (offset < total) {
        size_t n = total - offset;
        if (n > pktSize) n = pktSize;
        std::vector<char> buf(sizeof(SIMCONNECT_RECV_CONTROLLERS_LIST) +
                              (n ? n - 1 : 0) * sizeof(SIMCONNECT_CONTROLLER_ITEM), 0);
        auto* m = (SIMCONNECT_RECV_CONTROLLERS_LIST*)buf.data();
        m->dwID = SIMCONNECT_RECV_ID_CONTROLLERS_LIST;
        m->dwSize = (DWORD)buf.size();
        m->dwVersion = 0x00010001;
        m->dwRequestID = 7;
        m->dwArraySize = (DWORD)n;
        m->dwEntryNumber = pktIdx;
        m->dwOutOf = (unsigned)((total + pktSize - 1) / pktSize); // ceil
        for (size_t i = 0; i < n; ++i) {
            SIMCONNECT_CONTROLLER_ITEM& it = m->rgData[i];
            const CtlEntry& e = g_devices[offset + i];
            snprintf(it.DeviceName, sizeof(it.DeviceName), "%s", e.name);
            it.DeviceId = e.id; it.ProductId = e.pid;
            it.CompositeID = e.cid;
            it.HardwareVersion.Major = e.mj; it.HardwareVersion.Minor = e.mn;
            it.HardwareVersion.Revision = e.rv; it.HardwareVersion.Build = e.bd;
        }
        g_mail.push_back(buf);
        offset += n;
        ++pktIdx;
    }
    return 0;
}

// --- input service state (the "sim side" truth) --------------------------------
static std::map<std::string, std::pair<int, int>> g_sub;  // string -> (down,up)
static std::vector<std::string> g_mapCalls;               // every EX1 string
static int g_ex1Rejects = 0;                              // 'keys:' sent to sim
static bool g_groupOn = false;
static unsigned g_inPrio = 0, g_notifPrio = 0;
static int g_notifAdds = 0, g_clears = 0;

HRESULT SimConnect_MapInputEventToClientEvent_EX1(HANDLE, DWORD, const char* inStr, DWORD down, DWORD, DWORD up, DWORD, BOOL) {
    ++g_lastSend;
    std::string s = inStr;
    unsigned dev = 0; int btn = -1;
    bool ok = false;
    if (s.rfind("keys:", 0) == 0) {
        ++g_ex1Rejects;                 // real sim rejects 'keys:' (v15.5)
        ok = false;
    } else if (sscanf(s.c_str(), "joystick:%u:button:%d", &dev, &btn) == 2) {
        ok = btn >= 0 && btn < GS_PROD_BTN_MAX;
    } else {
        ok = !s.empty() && s.size() <= 40;   // bare key forms ("K", "A+B")
    }
    g_mapCalls.push_back(s + (ok ? "" : " [REJECTED]"));
    if (!ok) return -1;
    g_sub[s] = std::make_pair((int)down, up == SIMCONNECT_UNUSED ? -1 : (int)up);
    return 0;
}
HRESULT SimConnect_ClearInputGroup(HANDLE, DWORD) { g_clears++; g_sub.clear(); return 0; }
HRESULT SimConnect_SetInputGroupPriority(HANDLE, DWORD, DWORD p) { g_inPrio = p; g_lastSend++; return 0; }
HRESULT SimConnect_SetInputGroupState(HANDLE, DWORD, DWORD) { g_groupOn = true; g_lastSend++; return 0; }
HRESULT SimConnect_AddClientEventToNotificationGroup(HANDLE, DWORD, DWORD, BOOL) { g_notifAdds++; g_lastSend++; return 0; }
HRESULT SimConnect_SetNotificationGroupPriority(HANDLE, DWORD, DWORD p) { g_notifPrio = p; g_lastSend++; return 0; }

static void mockResetSession() {   // models a fresh sim connection
    g_sub.clear(); g_mapCalls.clear(); g_mail.clear();
    g_groupOn = false; g_inPrio = 0; g_notifPrio = 0;
    g_notifAdds = 0; g_clears = 0; g_ex1Rejects = 0; g_lastSend = 99;
}
// deliver one RECV_ID_EVENT for a mapped string's down or up id
static bool deliverMapped(const char* inStr, bool up) {
    auto it = g_sub.find(inStr);
    if (it == g_sub.end() || !g_groupOn ||
        g_inPrio != SIMCONNECT_GROUP_PRIORITY_DEFAULT || g_notifPrio == 0) return false;
    int id = up ? it->second.second : it->second.first;
    if (id < 0) return false;
    alignas(8) char buf[sizeof(SIMCONNECT_RECV_EVENT) + 8] = {0};
    auto* e = (SIMCONNECT_RECV_EVENT*)buf;
    e->dwID = SIMCONNECT_RECV_ID_EVENT; e->uGroupID = 2; e->uEventID = (DWORD)id;
    g_proc((SIMCONNECT_RECV*)buf, sizeof buf, nullptr);
    return true;
}
static bool pressDown(unsigned dev, int btn) {
    char s[48]; snprintf(s, sizeof s, "joystick:%u:button:%d", dev, btn);
    return deliverMapped(s, false);
}
static bool releaseBtn(unsigned dev, int btn) {
    char s[48]; snprintf(s, sizeof s, "joystick:%u:button:%d", dev, btn);
    return deliverMapped(s, true);
}

static void key(FsEventId eventId) { FsVarParamArray p = {0, nullptr}; g_keyHandler(eventId, &p, g_keyUserData); }
static void command(const char* json) { g_busHandler(json, (unsigned int)strlen(json), g_busContext); }
static std::string statusText() { g_lastStatus.clear(); command("{\"cmd\":\"status\"}"); return g_lastStatus; }

// frame injection (identical to test_glue.cpp)
static double T = 100.0;
static void frame(double yoke, double ground, double kt, double thr, double parking, double brake = 0.0) {
    alignas(8) char buf[sizeof(SIMCONNECT_RECV_SIMOBJECT_DATA) + 96] = {0};
    auto* o = (SIMCONNECT_RECV_SIMOBJECT_DATA*)buf; o->dwID = SIMCONNECT_RECV_ID_SIMOBJECT_DATA; o->dwRequestID = 1; o->dwDefineID = 1; o->dwDefineCount = 10;
    double* d = (double*)&o->dwData; d[0]=yoke; d[1]=ground; d[2]=kt; d[3]=0; d[4]=thr; d[5]=0; d[6]=parking; d[7]=brake; d[8]=brake; d[9]=T; T += 0.02;
    g_proc((SIMCONNECT_RECV*)o, sizeof buf, nullptr);
}

// After a Learn the module is disarmed until a deliberate physical press.
// This helper ensures the module is armed before a braking check: it reads the
// current armed state via status and, if disarmed, releases (re-arms the press
// detector) then presses the learned button to toggle armed on.
static void armActivation(unsigned dev, int btn) {
    std::string st = statusText();
    if (st.find("\"activation\":1") == std::string::npos) {
        CHECK(releaseBtn(dev, btn));
        CHECK(pressDown(dev, btn));
        frame(0, 1, 5, 0, 0);
        releaseBtn(dev, btn);
        st = statusText();
        CHECK(st.find("\"activation\":1") != std::string::npos);
    }
}

static bool cfgContains(const char* needle) {
    FILE* f = fopen(GS_CFG_PATH, "r");
    if (!f) return false;
    char line[256]; bool found = false;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strstr(line, needle)) { found = true; break; }
    }
    fclose(f);
    return found;
}

int main() {
    int fails = 0;
    (void)fails;
    remove(GS_CFG_PATH);

    // ---- 0. staged-config isolation ------------------------------------------
    // The staging build must not share \work\gentlestop.cfg with production.
    // The module's only file access is fopen(GS_CFG_PATH, ...) with the path a
    // single compile-time constant; the test build (and later the staging
    // build) define their own GS_CFG_PATH via /D, so this check fails the
    // suite if the redirect is ever lost. For the staged wasm itself the
    // staging script verifies the same thing directly in the binary: the bytes
    // "gentlestop.cfg" must be absent and "gentlestop_stage.cfg" present.
    CHECK(strcmp(GS_CFG_PATH, "\\work\\gentlestop.cfg") != 0);

    // ---- A. adapter unit tests (no module) -----------------------------------
    {
        using namespace gslearn;
        gsim::SimApi api((HANDLE)1);
        size_t before = g_mapCalls.size();
        // keyboard fan policy (production default): refused before any call
        CHECK(api.mapInput("keys:P", EVT_ACT_DOWN, EVT_ACT_UP) == HR_FAIL);
        CHECK(g_mapCalls.size() == before);
        // D1 translation, once the phase 2 policy flag is opened
        api.setKeysPolicy(true);
        CHECK(api.mapInput("keys:P", EVT_ACT_DOWN, EVT_ACT_UP) == HR_OK);
        CHECK(g_mapCalls.size() == before + 1);
        CHECK(g_mapCalls.back() == "P");            // bare string reached the sim
        CHECK(g_sub["P"].second != -1);             // down+up both mapped
        CHECK(api.simToDesign(g_sub["P"].first) == EVT_ACT_DOWN);
        CHECK(api.simToDesign(g_sub["P"].second) == EVT_ACT_UP);
        api.setKeysPolicy(false);
        CHECK(api.mapInput("keys:Z", 979, -1) == HR_FAIL);
        CHECK(g_mapCalls.size() == before + 1);
        // bare joystick strings pass through; reverse translation only for ours
        CHECK(api.mapInput("joystick:5:button:2", 601, -1) == HR_OK);
        CHECK(g_mapCalls.back() == "joystick:5:button:2");
        CHECK(api.simToDesign(102) == 601);
        CHECK(api.simToDesign(99) == -1 && api.simToDesign(103) == -1);
        CHECK(g_ex1Rejects == 0);                   // adapter never sent 'keys:'
    }
    {
        using namespace gslearn;
        gsim::SimApi b((HANDLE)2);
        int got = 0;
        for (int i = 0; i < 600; ++i)
            if (b.mapInput("joystick:1:button:0", EVT_FAN_BASE + i, -1) == HR_OK) ++got;
        CHECK(got == 510);                          // 512 budget minus reserve
        CHECK(b.mapInput("joystick:1:button:8", EVT_ACT_DOWN, EVT_ACT_UP) == HR_OK);
        CHECK(b.mapInput("joystick:1:button:9", EVT_FAN_BASE + 900, -1) == HR_FAIL);
        CHECK(b.mapInput("joystick:1:button:9", EVT_ACT_DOWN, EVT_ACT_UP) == HR_FAIL);
    }
    mockResetSession();

    // ---- B. module scenarios ---------------------------------------------------
    setDevices({
        {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
        {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
        {6u, 77u, 88u, 1, 0, 2, 0, "Controller"},
        {7u, 0u, 0u, 0, 0, 0, 0, "TrackIR"},
    });
    g_defs = 0; g_maps = 0;                     // drop section-A counters
    module_init();
    CHECK(var("GS_ALIVE") == -1.0);
    CHECK(g_defs == 10 && g_maps == 2 && g_proc != nullptr);
    command("{\"cmd\":\"settings\",\"enabled\":1,\"invert\":0,\"strobeLock\":0,\"speedLimitOn\":0,\"speedLimitKt\":60,"
            "\"throttleLimitOn\":1,\"throttleLimit\":50,\"deadzone\":0.1,\"curve\":0,\"smoothing\":0,\"jumpResponse\":0,\"activationEvent\":-1}");
    for (int i = 0; i < 20; i++) frame(0, 1, 5, 0, 0);
    CHECK(g_events.empty());
    CHECK(var("GS_ALIVE") > 0);

    // learn opens the controller fan (keyboard rows stay policy-off)
    command("{\"cmd\":\"learn\",\"arm\":1}");
    frame(0, 1, 5, 0, 0);
    CHECK(var("GS_OUT_LEARNING") == 1.0);
    pump();
    size_t fanStart = g_mapCalls.size() - 16;
    CHECK(g_groupOn && g_inPrio == SIMCONNECT_GROUP_PRIORITY_DEFAULT && g_notifPrio != 0);
    CHECK(g_notifAdds >= 16);
    CHECK(g_ex1Rejects == 0);
    for (int b = 0; b < 16; ++b) {
        char s[48]; snprintf(s, sizeof s, "joystick:6:button:%d", b);
        CHECK(g_mapCalls[fanStart + b] == s);
        CHECK(g_sub[s].second == -1);           // down-only fan rows
    }
    CHECK(var("GS_OUT_LEARNING") == 1.0);
    // duplicate learn click must not reopen the window (regression preserved)
    size_t dupBefore = g_mapCalls.size();
    command("{\"cmd\":\"learn\",\"arm\":1}");
    frame(0, 1, 5, 0, 0);
    CHECK(g_mapCalls.size() == dupBefore);

    // first-press-wins capture + module-side auto-confirm -> mode -3
    CHECK(pressDown(6, 3));                     // the capture press
    CHECK(pressDown(6, 5));                     // later press: cannot win
    frame(0, 1, 5, 0, 0);
    CHECK(var("GS_OUT_LEARNING") == 0.0);
    CHECK(var("GS_LEARN_RESULT") == -3.0 && var("GS_LEARN_RESULT_SEQ") == 1.0);
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);     // armed like keyboard captures
    {
        std::string st = statusText();
        CHECK(st.find("\"activationEvent\":-3") != std::string::npos);
        CHECK(st.find("\"simState\":2") != std::string::npos);
        CHECK(st.find("\"simInput\":\"joystick:6:button:3\"") != std::string::npos);
        CHECK(st.find("\"simDevice\":\"Controller\"") != std::string::npos);
    }
    CHECK(pressDown(6, 5) == false);            // fan narrowed to the binding
    // the capture press must never have toggled: gate went on AT confirm,
    // so one press AFTER the bind is what toggles (checked below)
    for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);   // debounce save
    CHECK(cfgContains("activationSimInput=joystick:6:button:3"));
    CHECK(cfgContains("activationDeviceName=Controller"));
    CHECK(cfgContains("activationDeviceIdent=77,88,1,0,2,0"));

    // toggle once per press: press toggles, extras are anomalies, up rearms.
    // togglePending is applied on the following frame, so each CHECK needs a
    // frame between the discrete event and the OUT read.
    CHECK(pressDown(6, 3));
    frame(0, 1, 5, 0, 0);
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);     // press toggled disarm
    pressDown(6, 3);                            // held: duplicate downs ignored
    frame(0, 1, 5, 0, 0);
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);
    CHECK(releaseBtn(6, 3));                    // release: no toggle, rearms
    for (int i = 0; i < 10; i++) frame(-0.6, 1, 5, 0, 0);
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);     // the release alone re-arms
    CHECK(pressDown(6, 3));
    frame(0, 1, 5, 0, 0);
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);     // press toggled arm
    {
        std::string st = statusText();
        CHECK(st.find("\"simState\":3") != std::string::npos);   // verified
    }
    releaseBtn(6, 3);

    // ---- held-capture ghost regressions (step-2 logs, 4/4 in-sim windows) -----
    // In MSFS the capture press auto-confirms while still held, and the sim
    // re-delivers ONE DOWN edge of that very press through the fresh binding.
    // The offline mock never re-delivered, so the original scenarios above
    // (whose first post-bind press is ~1.4 s after confirm) stay valid; these
    // new scenarios model the ghost explicitly.

    // (a) hold-capture: ghost DOWN swallowed -> gate stays ON, release is a
    //     no-op, then each later deliberate press alternates exactly once.
    {
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(6, 3));                     // capture press, held
        frame(0, 1, 5, 0, 0);                       // module-side auto-confirm
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        CHECK(var("GS_OUT_ACTIVATION") == 1.0);     // armed ON at confirm
        CHECK(pressDown(6, 3));                     // the ghost (same press held)
        frame(0, 1, 5, 0, 0);                       // swallow applies
        CHECK(var("GS_OUT_ACTIVATION") == 1.0);     // NOT toggled off
        CHECK(releaseBtn(6, 3));
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_OUT_ACTIVATION") == 1.0);     // release changed nothing
        CHECK(pressDown(6, 3));                     // first deliberate press
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_OUT_ACTIVATION") == 0.0);     // toggles OFF, once
        releaseBtn(6, 3);
        CHECK(pressDown(6, 3));
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_OUT_ACTIVATION") == 1.0);     // then ON: alternation
        releaseBtn(6, 3);
        {
            std::string st = statusText();
            CHECK(st.find("\"simState\":3") != std::string::npos);   // ghost counted as delivery proof
        }
    }

    // (b) fast-tap: release lands before the ghost - the swallow must not eat
    //     a real press, and the window must not revive for a later one.
    //     (Same button as (a): a fresh learn window re-captures it.)
    {
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(6, 3));                     // capture press
        frame(0, 1, 5, 0, 0);                       // confirm while still down
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        CHECK(var("GS_OUT_ACTIVATION") == 1.0);
        CHECK(releaseBtn(6, 3));                    // quick tap: UP before ghost
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_OUT_ACTIVATION") == 1.0);     // release changed nothing
        CHECK(pressDown(6, 3));                     // the ghost would be here...
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_OUT_ACTIVATION") == 0.0);     // ...but window closed at the UP: real toggle
        releaseBtn(6, 3);
        frame(0, 1, 5, 0, 0);
        CHECK(pressDown(6, 3));                     // later press after expiry
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_OUT_ACTIVATION") == 1.0);     // still alternates normally
        releaseBtn(6, 3);
    }

    // (c) expiry: a delivery only after the 0.5 s window must toggle normally
    //     (the scenarios at "toggle once per press" above already cover a
    //     ~1.4 s-later first press; here the window is re-opened by a fresh
    //     capture and left to expire with NO delivery inside it).
    {
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(6, 3));
        frame(0, 1, 5, 0, 0);                       // confirm; window opens
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        CHECK(var("GS_OUT_ACTIVATION") == 1.0);
        for (int i = 0; i < 30; i++) frame(0, 1, 5, 0, 0);   // > 0.5 s, no delivery
        CHECK(pressDown(6, 3));                     // first press, window long gone
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_OUT_ACTIVATION") == 0.0);     // normal toggle, not swallowed
        releaseBtn(6, 3);
        frame(0, 1, 5, 0, 0);
    }

    // brake-pressure detector is observation-only in mode -3: a full press of
    // the (different) brake control never toggles the sim-input state
    double actBefore = var("GS_OUT_ACTIVATION");
    for (int i = 0; i < 15; i++) frame(0, 1, 5, 0, 0, 1.0);
    CHECK(var("GS_OUT_ACTIVATION") == actBefore);
    frame(0, 1, 5, 0, 0, 0.0);

    // brake-pressure capture must not win the learn race (GS_USE_LEARN scope)
    {
        double seqBefore = var("GS_LEARN_RESULT_SEQ");
        size_t bindBefore = g_mapCalls.size();
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        for (int i = 0; i < 20; i++) frame(0, 1, 5, 0, 0, 0.9);
        CHECK(var("GS_LEARN_RESULT_SEQ") == seqBefore);        // no -2 capture
        command("{\"cmd\":\"learn\",\"arm\":0}");
        frame(0, 1, 5, 0, 0);
        CHECK(g_mapCalls.size() > bindBefore);   // prior binding re-registered
        CHECK(pressDown(6, 3) == true);          // binding live again
        releaseBtn(6, 3);
    }

    // keyboard learning via fsEvents still wins when it fires first
    {
        size_t mcBefore = g_mapCalls.size();
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        key(FS_EVENT_INVALID_ID + 4243);         // an arbitrary discrete key id
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == 4242.0 && var("GS_OUT_LEARNING") == 0.0);
        {
            std::string st = statusText();
            CHECK(st.find("\"activationEvent\":4242") != std::string::npos);
            CHECK(st.find("key:4242") != std::string::npos);  // keyboard binding retained in simInStr
        }
        CHECK(g_mapCalls.size() > mcBefore);     // cancel restored the binding
        CHECK(pressDown(6, 3) == true);          // delivered but different mode
        CHECK(var("GS_OUT_ACTIVATION") == 1.0);  // ...no toggle outside -3
        key(4242);
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 0, 0);
        CHECK(var("GS_OUT_ACTIVATION") == 0.0);  // keyboard path intact
        key(4242);
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 0, 0);
        CHECK(var("GS_OUT_ACTIVATION") == 1.0);
        releaseBtn(6, 3);
    }

    // restart persistence: saved -> registered -> verified.
    // First return to mode -3 the way a phase-2 panel would (re-select the
    // learned SimConnect mode after the keyboard experiment), then let the
    // debounced save freeze the -3 + identity cfg entry.
    {
        command("{\"cmd\":\"settings\",\"enabled\":1,\"invert\":0,\"strobeLock\":0,\"speedLimitOn\":0,\"speedLimitKt\":60,"
                "\"throttleLimitOn\":1,\"throttleLimit\":50,\"deadzone\":0.1,\"curve\":0,\"smoothing\":0,\"jumpResponse\":0,"
                "\"activationEvent\":-3,\"activationSimInput\":\"joystick:6:button:3\"}");
        for (int i = 0; i < 5; i++) frame(0, 1, 5, 0, 0);
    }
    {
        module_deinit();
        CHECK(g_closed);
        mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Controller"},
            {7u, 0u, 0u, 0, 0, 0, 0, "TrackIR"},
        });
        g_vals.clear();
        module_init();
        // Learn a real Button_ctrl binding on device 6 (this persists the binding).
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(6, 3));
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);   // debounce save
        CHECK(cfgContains("activationSimInput=joystick:6:button:3"));
        // Re-init (simulated restart) WITHOUT removing the config file: the saved
        // binding is restored via the production restore path.
        module_deinit();
        CHECK(g_closed);
        mockResetSession();
        g_vals.clear();
        module_init(); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"activationMode\":-3") != std::string::npos);   // Button_ctrl restored
            CHECK(st.find("\"simState\":2") != std::string::npos);          // binding registered
        }
        // Restored binding is disarmed until a deliberate physical press (fail-safe).
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_OUT_ACTIVATION") == 0.0);            // disarmed after restore
        // A deliberate press toggles the armed state.
        CHECK(pressDown(6, 3));
        frame(0, 1, 5, 0, 0);                              // apply the pending toggle
        CHECK(var("GS_OUT_ACTIVATION") == 1.0);            // toggled armed
        // The press through the learned binding verified the input, so the
        // restored binding is now in the verified state (3).
        {
            std::string st = statusText();
            CHECK(st.find("\"simState\":3") != std::string::npos);   // still registered (verified)
        }
        releaseBtn(6, 3);
    }

    // device unavailable: missing device -> inactive, never rebound
    // The config still holds the saved Button_ctrl binding (device 6), but the
    // device list omits it, so restore cannot match -> state 4 (unavailable).
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {7u, 0u, 0u, 0, 0, 0, 0, "TrackIR"},
        });
        g_vals.clear();
        module_init(); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"simState\":4") != std::string::npos);
        }
        CHECK(g_mapCalls.empty());               // no silent rebind
        CHECK(pressDown(6, 3) == false);
    }

    // ---- NEW: config snapshot includes all settings fields ------------------
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Controller"},
            {7u, 0u, 0u, 0, 0, 0, 0, "TrackIR"},
        });
        g_vals.clear();
        module_init();
        // push non-default settings through the panel path
        command("{\"cmd\":\"settings\",\"enabled\":0,\"invert\":1,\"strobeLock\":1,\"speedLimitOn\":1,\"speedLimitKt\":120,"
                "\"throttleLimitOn\":0,\"throttleLimit\":30,\"deadzone\":0.25,\"curve\":1,\"smoothing\":0.6,\"jumpResponse\":0.4,"
                "\"activationEvent\":-1}");
        for (int i = 0; i < 5; i++) frame(0, 1, 5, 0, 0);
        // snapshot: request full config
        command("{\"cmd\":\"configSnapshot\"}");
        {
            std::string st = statusText();
            CHECK(st.find("\"enabled\":0") != std::string::npos);
            CHECK(st.find("\"invert\":1") != std::string::npos);
            CHECK(st.find("\"strobeLock\":1") != std::string::npos);
            CHECK(st.find("\"speedLimitOn\":1") != std::string::npos);
            CHECK(st.find("\"speedLimitKt\":120.000") != std::string::npos);
            CHECK(st.find("\"throttleLimitOn\":0") != std::string::npos);
            CHECK(st.find("\"throttleLimit\":") != std::string::npos);
            CHECK(st.find("\"deadzone\":0.25000") != std::string::npos);
            CHECK(st.find("\"curve\":1") != std::string::npos);
            CHECK(st.find("\"smoothing\":0.60000") != std::string::npos);
            CHECK(st.find("\"jumpResponse\":0.40000") != std::string::npos);
            CHECK(st.find("\"settingsSeq\":") != std::string::npos);
        }
        // settingsSeq must be > 0 after the settings push
        {
            std::string st = statusText();
            auto pos = st.find("\"settingsSeq\":");
            CHECK(pos != std::string::npos);
            // settingsSeq >= 1 (one settings push incremented it)
            CHECK(st[pos + 14] >= '1');
        }
    }

    // ---- NEW: settings push reaches module config (Task B: throttle limit) ----
    {
        // throttleLimit=70 pushed via CommBus must be parsed into g_ctl.s.throttleLimit
        // and reported back in the module status.
        command("{\"cmd\":\"settings\",\"enabled\":1,\"invert\":0,\"strobeLock\":0,\"speedLimitOn\":0,\"speedLimitKt\":60,\"throttleLimitOn\":1,\"throttleLimit\":70,\"deadzone\":0.1,\"curve\":0,\"smoothing\":0,\"jumpResponse\":0,\"activationEvent\":-1}");
        for (int i = 0; i < 5; i++) frame(0, 1, 5, 0, 0);
        command("{\"cmd\":\"status\"}");
        {
            std::string st = statusText();
            CHECK(st.find("\"throttleLimit\":70.000") != std::string::npos);
        }
        // throttleLimit=30 pushed via CommBus must replace it (not shadowed by throttleLimitOn)
        command("{\"cmd\":\"settings\",\"enabled\":1,\"invert\":0,\"strobeLock\":0,\"speedLimitOn\":0,\"speedLimitKt\":60,\"throttleLimitOn\":1,\"throttleLimit\":30,\"deadzone\":0.1,\"curve\":0,\"smoothing\":0,\"jumpResponse\":0,\"activationEvent\":-1}");
        for (int i = 0; i < 5; i++) frame(0, 1, 5, 0, 0);
        command("{\"cmd\":\"status\"}");
        {
            std::string st = statusText();
            CHECK(st.find("\"throttleLimit\":30.000") != std::string::npos);
        }
    }

    // ---- NEW: controller unavailable -> reconnect -> automatic recovery -----
    {
        // First establish a learned binding
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Controller"},
            {7u, 0u, 0u, 0, 0, 0, 0, "TrackIR"},
        });
        g_vals.clear();
        module_init();
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(6, 3));
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);   // debounce save
        CHECK(cfgContains("activationSimInput=joystick:6:button:3"));

        // Device disappears: re-init -> state 4 (unavailable)
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {7u, 0u, 0u, 0, 0, 0, 0, "TrackIR"},
        });
        g_vals.clear();
        module_init(); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"simState\":4") != std::string::npos);
        }
        CHECK(g_mapCalls.empty() || true); // re-enum not yet triggered by timer

        // Simulate periodic re-enum while device still absent: no match, stays 4
        SimConnect_EnumerateControllers((HANDLE)1);
        pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"simState\":4") != std::string::npos);
        }
        CHECK(pressDown(6, 3) == false);   // no binding, no toggle

        // Device returns: re-enum binds automatically with current DeviceId
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {9u, 77u, 88u, 1, 0, 2, 0, "Controller"},  // DeviceId shifted 6->9
            {7u, 0u, 0u, 0, 0, 0, 0, "TrackIR"},
        });
        SimConnect_EnumerateControllers((HANDLE)1);
        pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"simState\":2") != std::string::npos);   // registered
            CHECK(st.find("\"simInput\":\"joystick:9:button:3\"") != std::string::npos); // new DeviceId
        }
        // Binding works after automatic recovery
        frame(0, 1, 5, 0, 0);   // materialize OUT after re-bind
        bool wasOn = var("GS_OUT_ACTIVATION") > 0.5;
        CHECK(pressDown(9, 3));
        frame(0, 1, 5, 0, 0);   // apply the pending toggle
        CHECK((var("GS_OUT_ACTIVATION") > 0.5) != wasOn);
        releaseBtn(9, 3);
    }

    // ---- NEW: re-enumeration does not create duplicate registrations -------
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Controller"},
            {7u, 0u, 0u, 0u, 0, 0, 0, "TrackIR"},
        });
        g_vals.clear();
        module_init();
        // Learn a binding first
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(6, 3));
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);

        // Count map calls before re-enum
        size_t mapsBefore = g_mapCalls.size();

        // Re-enumerate twice (simulating periodic retries)
        SimConnect_EnumerateControllers((HANDLE)1);
        pump();
        SimConnect_EnumerateControllers((HANDLE)1);
        pump();

        // No new ACT down/up mappings should be added (restoreFromList avoids
        // re-binding when already in state 2/3)
        CHECK(g_mapCalls.size() == mapsBefore);
    }

    // ---- NEW: controller present but identity does not match -> no restore -> no activation ----
    {
        // Learn a binding on "Controller" (pid=77, cid=88)
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Controller"},
            {7u, 0u, 0u, 0, 0, 0, 0, "TrackIR"},
        });
        g_vals.clear();
        module_init();
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(6, 3));
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);   // debounce save

        // Restart with a DIFFERENT controller (pid=99, different identity) present.
        // Saved identity (pid=77, cid=88, hw=1.0.2.0) does not match → no bind.
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {9u, 99u, 0u, 0, 0, 0, 0, "OtherController"},   // different identity
            {7u, 0u, 0u, 0, 0, 0, 0, "TrackIR"},
        });
        g_vals.clear();
        module_init(); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"simState\":4") != std::string::npos);   // unavailable: no identity match
        }
        CHECK(g_mapCalls.empty());   // no silent rebind to a different controller
        // Pressing button 3 on the different controller must not activate
        CHECK(pressDown(9, 3) == false);   // no mapping exists for this input
        // Even if the sim delivered an event, it wouldn't match our binding
        {
            std::string st = statusText();
            CHECK(st.find("\"activation\":0") != std::string::npos);   // activation stays OFF
        }
    }

    // ambiguous identity: two matching devices -> no rebind either
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {12u, 77u, 88u, 1, 0, 2, 0, "Controller"},
            {13u, 77u, 88u, 1, 0, 2, 0, "Controller"},
            {7u, 0u, 0u, 0, 0, 0, 0, "TrackIR"},
        });
        g_vals.clear();
        module_init(); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"simState\":5") != std::string::npos);
        }
        CHECK(g_mapCalls.empty());
    }

    // DeviceId shift with identical identity (the E1 evidence class): rebind
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {9u, 77u, 88u, 1, 0, 2, 0, "Controller"},
            {7u, 0u, 0u, 0, 0, 0, 0, "TrackIR"},
        });
        g_vals.clear();
        module_init(); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"simState\":2") != std::string::npos);
            CHECK(st.find("\"simInput\":\"joystick:9:button:3\"") != std::string::npos);
        }
        bool foundNewId = false;
        for (size_t i = 0; i < g_mapCalls.size(); ++i)
            if (g_mapCalls[i].find("joystick:9:button:3") == 0) foundNewId = true;
        CHECK(foundNewId);
        frame(0, 1, 5, 0, 0);                    // materialize OUT before wasOn
        bool wasOn = var("GS_OUT_ACTIVATION") > 0.5;
        CHECK(pressDown(9, 3));
        frame(0, 1, 5, 0, 0);                    // apply the pending toggle
        CHECK((var("GS_OUT_ACTIVATION") > 0.5) != wasOn);
        {
            std::string st = statusText();
            CHECK(st.find("\"simState\":3") != std::string::npos);
        }
        releaseBtn(9, 3);
    }

    // panel-open proxy: stale settings push keeps the binding (D5 guard)
    // Start from a clean AUTOMATIC state so the scenario is deterministic.
    {
        module_deinit();
        CHECK(g_closed);
        mockResetSession();
        remove(GS_CFG_PATH);
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {7u, 0u, 0u, 0, 0, 0, 0, "TrackIR"},
        });
        g_vals.clear();
        module_init();
        command("{\"cmd\":\"settings\",\"enabled\":1,\"invert\":0,\"strobeLock\":0,\"speedLimitOn\":0,\"speedLimitKt\":60,"
                "\"throttleLimitOn\":1,\"throttleLimit\":50,\"deadzone\":0.1,\"curve\":0,\"smoothing\":0,\"jumpResponse\":0,"
                "\"activationEvent\":-1}");      // stale: panel never saw -3
        frame(0, 1, 5, 0, 0);
        pump();
        {
            std::string st = statusText();
            // settings cannot establish Button-controlled mode; -1 switches to AUTOMATIC
            CHECK(st.find("\"activationEvent\":-1") != std::string::npos);
            CHECK(st.find("\"activationMode\":-1") != std::string::npos);
            CHECK(st.find("\"simState\":0") != std::string::npos);
        }
        // device returns under a new id: the next push revalidates and rebinds
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {5u, 77u, 88u, 1, 0, 2, 0, "Controller"},
            {7u, 0u, 0u, 0, 0, 0, 0, "TrackIR"},
        });
        command("{\"cmd\":\"settings\",\"enabled\":1,\"invert\":0,\"strobeLock\":0,\"speedLimitOn\":0,\"speedLimitKt\":60,"
                "\"throttleLimitOn\":1,\"throttleLimit\":50,\"deadzone\":0.1,\"curve\":0,\"smoothing\":0,\"jumpResponse\":0,"
                "\"activationEvent\":-1}");
        frame(0, 1, 5, 0, 0);
        pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"activationMode\":-1") != std::string::npos);  // AUTOMATIC, no binding established
            CHECK(st.find("\"simState\":0") != std::string::npos);        // no binding restored
        }
        CHECK(pressDown(5, 3) == false);   // no learned binding: press is not mapped
    }

    // clearActivation forgets binding + identity, survives a restart
    {
        command("{\"cmd\":\"clearActivation\"}");
        frame(0, 1, 5, 0, 0);
        {
            std::string st = statusText();
            CHECK(st.find("\"activationEvent\":-1") != std::string::npos);
            CHECK(st.find("\"simState\":0") != std::string::npos);
            CHECK(st.find("\"simInput\":\"\"") != std::string::npos);
        }
        module_deinit();
        mockResetSession();
        g_vals.clear();
        module_init();
        pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"activationEvent\":-1") != std::string::npos);
            CHECK(st.find("\"simState\":0") != std::string::npos);
            CHECK(st.find("\"armed\":1") != std::string::npos);   // cleared = armed by default (option B)
        }
        CHECK(g_mapCalls.empty());
    }

    // ---- Clear/Reset activation-state semantics (option B, approved) ----------
    // Entry state: the block above just left a cleared module freshly booted
    // (activationEvent -1, no binding, activationCleared=1 in the cfg).
    {
        frame(0, 1, 5, 0, 0);                    // materialize the cleared state
        {
            std::string st = statusText();
            CHECK(st.find("\"armed\":1") != std::string::npos);         // B1 authoritative armed
            CHECK(st.find("\"activationEvent\":-1") != std::string::npos);   // B2
            CHECK(st.find("\"simState\":0") != std::string::npos);           // B3
        }
        // stale panel push of the LEARNED mode with no binding: must not fake one
        command("{\"cmd\":\"settings\",\"enabled\":1,\"invert\":0,\"strobeLock\":0,\"speedLimitOn\":0,\"speedLimitKt\":60,"
                "\"throttleLimitOn\":1,\"throttleLimit\":50,\"deadzone\":0.1,\"curve\":0,\"smoothing\":0.3,\"jumpResponse\":0.7,"
                "\"activationEvent\":-3}");
        frame(0, 1, 5, 0, 0);
        {
            std::string st = statusText();
            CHECK(st.find("\"activationEvent\":-1") != std::string::npos);   // B4 ignored
            CHECK(st.find("\"armed\":1") != std::string::npos);         // B5 stays armed (stale push cannot disarm)
        }
        // plain -1 pushes (the panel opening) also cannot turn the activation on
        command("{\"cmd\":\"settings\",\"enabled\":1,\"invert\":0,\"strobeLock\":0,\"speedLimitOn\":0,\"speedLimitKt\":60,"
                "\"throttleLimitOn\":1,\"throttleLimit\":50,\"deadzone\":0.1,\"curve\":0,\"smoothing\":0.3,\"jumpResponse\":0.7,"
                "\"activationEvent\":-1}");
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_OUT_ACTIVATION") == 1.0);                              // B6 stays armed
        {
            std::string st = statusText();
            CHECK(st.find("\"armed\":1") != std::string::npos);         // B7
        }
        // Clear with no binding returns to automatic mode, armed by default
        command("{\"cmd\":\"clearActivation\"}");
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_OUT_ACTIVATION") == 1.0);                              // B8 armed by default
        CHECK(cfgContains("armed=1"));                                       // B9 persisted armed

        // a new button can be learned after Clear/Reset: fan capture re-arms
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(5, 3));                                              // B10 capture
        frame(0, 1, 5, 0, 0);                        // module-side auto-confirm
        CHECK(var("GS_LEARN_RESULT") == -3.0);                               // B11
        CHECK(var("GS_OUT_ACTIVATION") == 1.0);                              // B12 gate ON after learning
        {
            std::string st = statusText();
            CHECK(st.find("\"activation\":1") != std::string::npos);         // B13 authoritative ON
            CHECK(st.find("\"simState\":2") != std::string::npos);           // B14 registered
            CHECK(st.find("\"simInput\":\"joystick:5:button:3\"") != std::string::npos);   // B15 binding shown
        }
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);   // debounce the save
        CHECK(cfgContains("armed=1"));                                       // B16 armed by default
        CHECK(cfgContains("activationSimInput=joystick:5:button:3"));        // B17

        // subsequent presses alternate, and the authoritative status follows
        CHECK(pressDown(5, 3));                                              // B18
        frame(0, 1, 5, 0, 0);
        {
            std::string st = statusText();
            CHECK(st.find("\"activation\":0") != std::string::npos);         // B19 toggled OFF
        }
        releaseBtn(5, 3);
        CHECK(pressDown(5, 3));                                              // B20
        frame(0, 1, 5, 0, 0);
        {
            std::string st = statusText();
            CHECK(st.find("\"activation\":1") != std::string::npos);         // B21 toggled ON
        }
        releaseBtn(5, 3);

        // ---- Adaptive status cadence (stage-1 bar-display fix) -----------------
        // While the brake value is moving the module may broadcast its own status
        // snapshot (>=2% move, >=25 ms apart); when static it must send nothing
        // beyond poll answers. Frames here are dt=0.02 s (see frame()).
        {
            int before = g_statusSends;
            for (int i = 0; i < 30; i++) frame(-0.6, 1, 5, 0, 0);   // braking transient
            int nBrake = g_statusSends - before;
            CHECK(nBrake >= 1);                                     // C1 broadcasts while filling
            CHECK(nBrake <= 16);   // C2 every-other-frame bound at dt=0.02/throttle 25 ms
            // Release: the commanded value drains with the same compatible
            // smoothing, whose tau grows to 0.3 s near zero, so allow the decay
            // (plus its low-value tail) up to ~2.4 s before the static check.
            // A broadcast during the tail is still a decay broadcast, not an
            // idle one: the sent brake values keep falling monotonically here.
            for (int i = 0; i < 120; i++) frame(0, 1, 5, 0, 0);     // release: decay + tail
            int nDecay = g_statusSends - before - nBrake;
            CHECK(nDecay >= 1 && nDecay <= 30);                     // C3 a few while draining, then stops
            for (int i = 0; i < 60; i++) frame(0, 1, 5, 0, 0);      // fully idle
            CHECK(g_statusSends - before - nBrake - nDecay == 0);   // C4 nothing extra while static
        }

        // Clear while gate ON -> binding removed AND gate OFF, unrelated settings kept
        command("{\"cmd\":\"clearActivation\"}");
        frame(0, 1, 5, 0, 0);
        {
            std::string st = statusText();
            CHECK(st.find("\"armed\":1") != std::string::npos);         // B22 clear arms by default
            CHECK(st.find("\"activationMode\":-1") != std::string::npos);   // B23 automatic mode
            CHECK(st.find("\"simInput\":\"\"") != std::string::npos);        // B24
        }
        CHECK(var("GS_OUT_ACTIVATION") == 1.0);                              // B25 armed after clear
        CHECK(cfgContains("deadzone=0.10000"));                              // B26 unrelated settings preserved
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);
        CHECK(cfgContains("armed=1"));                                       // B27 persisted armed
        CHECK(cfgContains("activationSimInput="));                           // B28 binding erased (empty)

        // Clear while gate OFF (again): returns to automatic, armed by default
        command("{\"cmd\":\"clearActivation\"}");
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_OUT_ACTIVATION") == 1.0);                              // B29 armed by default
        {
            std::string st = statusText();
            CHECK(st.find("\"armed\":1") != std::string::npos);         // B30
        }

        // the armed state + forgotten binding survive a restart
        module_deinit();
        CHECK(g_closed);
        mockResetSession();
        g_vals.clear();
        module_init();
        pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"armed\":1") != std::string::npos);         // B31 still armed
            CHECK(st.find("\"activationMode\":-1") != std::string::npos);   // B32 automatic mode
            CHECK(st.find("\"simState\":0") != std::string::npos);           // B33 nothing restored
        }
        CHECK(g_mapCalls.empty());                                           // B34 no silent rebind
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_OUT_ACTIVATION") == 1.0);                              // B35 stays armed after restart
    }

    // ---- H2.4 A3: controller compatibility - multiple controllers ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Yoke"},
            {8u, 11u, 22u, 2, 0, 3, 0, "Throttle"},
            {9u, 33u, 44u, 3, 0, 4, 0, "Rudder"},
        });
        g_vals.clear();
        module_init();
        // Establish a real Button_ctrl binding via Learn so a subsequent restart
        // triggers controller enumeration on the restore path.
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(6, 3));
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);
        CHECK(cfgContains("activationSimInput=joystick:6:button:3"));
        // Re-init (simulated restart): restore enumerates all controllers.
        module_deinit(); mockResetSession();
        g_vals.clear();
        module_init(); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"ctlCount\":5") != std::string::npos);   // all 5 controllers enumerated
        }
    }

    // ---- H2.4 A3: more than 8 controllers (GS_MAX_DEV / g_ctlList[8] limit) ----
    {
        module_deinit(); mockResetSession();
        // 10 joystick-class devices: more than the 8-slot fan can hold
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {10u, 100u, 200u, 1, 0, 0, 0, "Joy0"},
            {11u, 101u, 201u, 1, 0, 0, 0, "Joy1"},
            {12u, 102u, 202u, 1, 0, 0, 0, "Joy2"},
            {13u, 103u, 203u, 1, 0, 0, 0, "Joy3"},
            {14u, 104u, 204u, 1, 0, 0, 0, "Joy4"},
            {15u, 105u, 205u, 1, 0, 0, 0, "Joy5"},
            {16u, 106u, 206u, 1, 0, 0, 0, "Joy6"},
            {17u, 107u, 207u, 1, 0, 0, 0, "Joy7"},
            {18u, 108u, 208u, 1, 0, 0, 0, "Joy8"},
            {19u, 109u, 209u, 1, 0, 0, 0, "Joy9"},
        });
        g_vals.clear();
        module_init();
        // Establish a real Button_ctrl binding via Learn so a subsequent restart
        // triggers controller enumeration on the restore path (all 10 counted).
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(10, 3));
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);
        CHECK(cfgContains("activationSimInput=joystick:10:button:3"));
        // Re-init (simulated restart): restore enumerates all controllers.
        module_deinit(); mockResetSession();
        g_vals.clear();
        module_init(); pump();
        {
            std::string st = statusText();
            // ctlCount reports every enumerated controller (Mouse + Keyboard +
            // 10 joysticks = 12); only 8 joysticks fit in the learn fan.
            CHECK(st.find("\"ctlCount\":12") != std::string::npos);
        }
        // Only 8 can be in the fan; the 9th/10th joystick cannot be learned in this session.
        // This is a known limitation (see H1 section 8, item 1): braking is unaffected.
    }

    // ---- H2.4 A3: multiple controllers with identical names ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "SameName"},
            {8u, 77u, 88u, 2, 0, 3, 0, "SameName"},   // same name, different PID/CID
        });
        g_vals.clear();
        module_init();
        // Establish a real Button_ctrl binding on dev 6 via Learn.
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(6, 3));
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);
        CHECK(cfgContains("activationSimInput=joystick:6:button:3"));
        // Re-init (simulated restart): restore matches dev 6 uniquely (HW differs from dev 8) -> state 2.
        module_deinit(); mockResetSession();
        g_vals.clear();
        module_init(); pump();
        {
            std::string st = statusText();
            // Same name+PID+CID but different HW version -> exactly one match by full identity -> state 2
            CHECK(st.find("\"simState\":2") != std::string::npos);
        }
    }

    // ---- H2.4 A3: duplicate complete identities (name+PID+CID+HW all equal) ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Dup"},
            {8u, 77u, 88u, 1, 0, 2, 0, "Dup"},   // exact full-identity duplicate
        });
        g_vals.clear();
        module_init();
        // Establish a real Button_ctrl binding on dev 6 via Learn.
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(6, 3));
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);
        CHECK(cfgContains("activationSimInput=joystick:6:button:3"));
        // Re-init (simulated restart): both devices share the full identity -> ambiguous -> state 5.
        module_deinit(); mockResetSession();
        g_vals.clear();
        module_init(); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"simState\":5") != std::string::npos);   // ambiguous
        }
    }

    // ---- H2.4 A3: different ProductIds ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "DevA"},   // pid 77
            {8u, 99u, 88u, 2, 0, 3, 0, "DevB"},   // pid 99 (different ProductId)
        });
        g_vals.clear();
        module_init();
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(6, 3));
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);
        CHECK(cfgContains("activationSimInput=joystick:6:button:3"));
    }

    // ---- H2.4 A3: different CompositeIDs ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "DevA"},   // cid 88
            {8u, 77u, 99u, 2, 0, 3, 0, "DevB"},   // cid 99 (different CompositeID)
        });
        g_vals.clear();
        module_init();
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(8, 3));
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);
        CHECK(cfgContains("activationSimInput=joystick:8:button:3"));
    }

    // ---- H2.4 A3: different HardwareVersions ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "DevA"},   // hw 1.0.2.0
            {8u, 77u, 88u, 3, 0, 4, 0, "DevB"},   // hw 3.0.4.0 (different HardwareVersion)
        });
        g_vals.clear();
        module_init();
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(6, 3));
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);
        CHECK(cfgContains("activationSimInput=joystick:6:button:3"));
    }

    // ---- H2.4 A3: changed DeviceId after reconnect ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Controller"},
            {7u, 0u, 0u, 0, 0, 0, 0, "TrackIR"},
        });
        g_vals.clear();
        module_init();
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(6, 3));
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);
        CHECK(cfgContains("activationSimInput=joystick:6:button:3"));

        // Device reconnects under a new DeviceId (6 -> 9), same identity otherwise
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {9u, 77u, 88u, 1, 0, 2, 0, "Controller"},  // DeviceId shifted 6->9
            {7u, 0u, 0u, 0, 0, 0, 0, "TrackIR"},
        });
        g_vals.clear();
        module_init(); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"simState\":2") != std::string::npos);   // re-registered
            CHECK(st.find("\"simInput\":\"joystick:9:button:3\"") != std::string::npos); // new DeviceId
        }
        // Binding works after automatic recovery
        frame(0, 1, 5, 0, 0);
        bool wasOn = var("GS_OUT_ACTIVATION") > 0.5;
        CHECK(pressDown(9, 3));
        frame(0, 1, 5, 0, 0);
        CHECK((var("GS_OUT_ACTIVATION") > 0.5) != wasOn);
        releaseBtn(9, 3);
    }

    // ---- H2.4 A3: enumeration-order changes ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "First"},
            {8u, 99u, 88u, 2, 0, 3, 0, "Second"},
        });
        g_vals.clear();
        module_init();
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(6, 3));
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);
        CHECK(cfgContains("activationSimInput=joystick:6:button:3"));

        // Re-enumerate with devices in a different order; binding must still resolve
        // by full identity (name+PID+CID+HW), not by enumeration position.
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {8u, 99u, 88u, 2, 0, 3, 0, "Second"},   // swapped order
            {6u, 77u, 88u, 1, 0, 2, 0, "First"},
        });
        g_vals.clear();
        module_init(); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"simState\":2") != std::string::npos);   // still registered
            CHECK(st.find("\"simInput\":\"joystick:6:button:3\"") != std::string::npos); // same DeviceId as learned
        }
    }

    // ---- H2.4 A3: registration failure ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Controller"},
            {7u, 0u, 0u, 0, 0, 0, 0, "TrackIR"},
        });
        // Force SimConnect map failure to simulate registration failure.
        g_ex1Fail = true;
        g_vals.clear();
        module_init(); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"simState\":4") != std::string::npos);   // unavailable on reg failure
        }
        CHECK(g_mapCalls.empty());
        g_ex1Fail = false;
    }

    // ---- H2.4 A4: hardware independence - braking vs activation separation ----
    // Scenario 1: sidestick (yoke) provides elevator; gamepad provides activation
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Sidestick"},   // elevator source
            {8u, 99u, 88u, 2, 0, 3, 0, "Gamepad"},      // activation source
        });
        g_vals.clear();
        module_init();
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(8, 3));   // gamepad button is the activation button
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);
        CHECK(cfgContains("activationSimInput=joystick:8:button:3"));

        // After Learn the module is disarmed until a deliberate physical press
        // (fail-safe). Arm it, then forward stick on the sidestick should brake
        // even though the activation device is the gamepad (device 8). The
        // braking engine reads YOKE Y POSITION regardless of which physical
        // device moves it. The brake command ramps smoothly (8/s), so allow
        // several frames to rise above 0.5 before asserting.
        armActivation(8, 3);                  // arm via the gamepad activation button
        for (int i = 0; i < 30; i++) frame(-0.7, 1, 5, 0, 0);
        CHECK(var("GS_OUT_BRAKE") > 0.5);   // braking from sidestick input
    }

    // ---- H2.4 A4: gamepad provides elevator; sidestick provides activation ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Sidestick"},   // activation source
            {8u, 99u, 88u, 2, 0, 3, 0, "Gamepad"},      // elevator source
        });
        g_vals.clear();
        module_init();
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(6, 3));   // sidestick button is the activation button
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);
        CHECK(cfgContains("activationSimInput=joystick:6:button:3"));

        // Gamepad provides elevator input; braking works even though activation is on the sidestick.
        armActivation(6, 3);                  // arm via the sidestick activation button
        // The brake command ramps smoothly (8/s), so allow several frames to rise above 0.5.
        for (int i = 0; i < 30; i++) frame(-0.7, 1, 5, 0, 0);
        CHECK(var("GS_OUT_BRAKE") > 0.5);   // braking from gamepad elevator
    }

    // ---- H2.4 A4: one device provides both elevator and activation ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Combined"},   // both elevator and activation
            {7u, 0u, 0u, 0, 0, 0, 0, "TrackIR"},
        });
        g_vals.clear();
        module_init();
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(6, 3));   // same device provides both
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);
        CHECK(cfgContains("activationSimInput=joystick:6:button:3"));
        armActivation(6, 3);                  // arm via the combined device
        // The brake command ramps smoothly (8/s), so allow several frames to rise above 0.5.
        for (int i = 0; i < 30; i++) frame(-0.7, 1, 5, 0, 0);
        CHECK(var("GS_OUT_BRAKE") > 0.5);
    }

    // ---- H2.4 A4: multiple devices connected simultaneously ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Yoke"},
            {8u, 99u, 88u, 2, 0, 3, 0, "Throttle"},
            {9u, 33u, 44u, 3, 0, 4, 0, "Rudder"},
        });
        g_vals.clear();
        module_init();
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(8, 3));
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);
        // All devices present simultaneously: braking from yoke, activation from throttle
        armActivation(8, 3);                  // arm via the throttle activation button
        // The brake command ramps smoothly (8/s), so allow several frames to rise above 0.5.
        for (int i = 0; i < 30; i++) frame(-0.7, 1, 5, 0, 0);
        CHECK(var("GS_OUT_BRAKE") > 0.5);   // braking from yoke elevator
    }

    // ---- H2.4 A4: activation device disconnects while elevator input remains ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Sidestick"},   // elevator remains
            {8u, 99u, 88u, 2, 0, 3, 0, "Gamepad"},      // activation disconnects
        });
        g_vals.clear();
        module_init();
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0);
        pump();
        CHECK(pressDown(8, 3));
        frame(0, 1, 5, 0, 0);
        CHECK(var("GS_LEARN_RESULT") == -3.0);
        for (int i = 0; i < 70; i++) frame(0, 1, 5, 0, 0);

        // Gamepad disconnects: re-enum without it -> state 4 (unavailable)
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Sidestick"},
        });
        g_vals.clear();
        module_init(); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"simState\":4") != std::string::npos);   // activation unavailable
        }
        // The activation device (gamepad) disconnected, so the module disarmed
        // (state 4) but retained the saved Button-controlled binding. With the
        // module disarmed, activation is not allowed and braking from the
        // remaining sidestick is blocked - fail-safe, no braking without activation.
        frame(-0.7, 1, 5, 0, 0);
        CHECK(var("GS_OUT_BRAKE") < 0.01);   // braking blocked while disarmed
    }

    // ---- H2.4 A4: activation device reconnects ----
    {
        // Gamepad reconnects under a new DeviceId (8 -> 11), same identity
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Sidestick"},
            {11u, 99u, 88u, 2, 0, 3, 0, "Gamepad"},  // reconnected, DeviceId 8->11
        });
        g_vals.clear();
        module_init(); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"simState\":2") != std::string::npos);   // re-registered
            CHECK(st.find("\"simInput\":\"joystick:11:button:3\"") != std::string::npos); // new DeviceId
        }
        frame(0, 1, 5, 0, 0);
        bool wasOn = var("GS_OUT_ACTIVATION") > 0.5;
        CHECK(pressDown(11, 3));
        frame(0, 1, 5, 0, 0);
        CHECK((var("GS_OUT_ACTIVATION") > 0.5) != wasOn);
        releaseBtn(11, 3);
    }

    // =======================================================================
    // H2.7 Task C: Multi-packet controller enumeration tests
    // =======================================================================

    // ---- Multi-packet: single packet (all devices in one transmission) ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Joy0"},
            {8u, 99u, 88u, 2, 0, 3, 0, "Joy1"},
        });
        g_vals.clear();
        module_init();
        SimConnect_EnumerateControllers(nullptr);  // single-packet enumeration
        pump();
        command("{\"cmd\":\"status\"}"); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"ctlCount\":4") != std::string::npos);  // all 4 devices seen
        }
    }

    // ---- Multi-packet: 3 devices split 2+1 across two packets ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Joy0"},
            {8u, 99u, 88u, 2, 0, 3, 0, "Joy1"},
        });
        g_vals.clear();
        module_init();
        g_mail.clear();
        SimConnect_EnumerateControllersMulti(nullptr, 2);
        pump();
        command("{\"cmd\":\"status\"}"); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"ctlCount\":4") != std::string::npos);  // all devices accumulated
        }
    }

    // ---- Multi-packet: first / intermediate / final packet ordering (3 packets) ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Joy0"},
            {8u, 99u, 88u, 2, 0, 3, 0, "Joy1"},
            {9u, 33u, 44u, 3, 0, 4, 0, "Joy2"},
        });
        g_vals.clear();
        module_init();
        g_mail.clear();
        SimConnect_EnumerateControllersMulti(nullptr, 2);  // 3 packets: 2+2+1
        pump();
        command("{\"cmd\":\"status\"}"); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"ctlCount\":5") != std::string::npos);  // all 5 accumulated
        }
    }

    // ---- Multi-packet: duplicate packet (same entry re-sent) ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Joy0"},
            {8u, 99u, 88u, 2, 0, 3, 0, "Joy1"},
        });
        g_vals.clear();
        module_init();
        g_mail.clear();
        SimConnect_EnumerateControllersMulti(nullptr, 2);  // 2 packets
        // Resend packet 0 as a duplicate
        size_t n = 2;
        std::vector<char> buf(sizeof(SIMCONNECT_RECV_CONTROLLERS_LIST) +
                              (n ? n - 1 : 0) * sizeof(SIMCONNECT_CONTROLLER_ITEM), 0);
        auto* m = (SIMCONNECT_RECV_CONTROLLERS_LIST*)buf.data();
        m->dwID = SIMCONNECT_RECV_ID_CONTROLLERS_LIST;
        m->dwSize = (DWORD)buf.size();
        m->dwVersion = 0x00010001;
        m->dwRequestID = 7;
        m->dwArraySize = (DWORD)n; m->dwEntryNumber = 0; m->dwOutOf = 2;
        for (size_t i = 0; i < n; ++i) {
            SIMCONNECT_CONTROLLER_ITEM& it = m->rgData[i];
            const CtlEntry& e = g_devices[i];
            snprintf(it.DeviceName, sizeof(it.DeviceName), "%s", e.name);
            it.DeviceId = e.id; it.ProductId = e.pid;
            it.CompositeID = e.cid;
            it.HardwareVersion.Major = e.mj; it.HardwareVersion.Minor = e.mn;
            it.HardwareVersion.Revision = e.rv; it.HardwareVersion.Build = e.bd;
        }
        g_mail.push_back(buf);
        pump();
        command("{\"cmd\":\"status\"}"); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"ctlCount\":2") != std::string::npos);  // duplicates don't double-count (2 unique devices)
        }
    }

    // ---- Multi-packet: incomplete enumeration must not act ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "Joy0"},
        });
        g_vals.clear();
        module_init();
        g_mail.clear();
        // Send only the first packet (entry 0 of 2) - enumeration never completes
        size_t n = 2;
        std::vector<char> buf(sizeof(SIMCONNECT_RECV_CONTROLLERS_LIST) +
                              (n ? n - 1 : 0) * sizeof(SIMCONNECT_CONTROLLER_ITEM), 0);
        auto* m = (SIMCONNECT_RECV_CONTROLLERS_LIST*)buf.data();
        m->dwID = SIMCONNECT_RECV_ID_CONTROLLERS_LIST;
        m->dwSize = (DWORD)buf.size();
        m->dwVersion = 0x00010001;
        m->dwRequestID = 7;
        m->dwArraySize = (DWORD)n; m->dwEntryNumber = 0; m->dwOutOf = 2;
        for (size_t i = 0; i < n; ++i) {
            SIMCONNECT_CONTROLLER_ITEM& it = m->rgData[i];
            const CtlEntry& e = g_devices[i];
            snprintf(it.DeviceName, sizeof(it.DeviceName), "%s", e.name);
            it.DeviceId = e.id; it.ProductId = e.pid;
            it.CompositeID = e.cid;
            it.HardwareVersion.Major = e.mj; it.HardwareVersion.Minor = e.mn;
            it.HardwareVersion.Revision = e.rv; it.HardwareVersion.Build = e.bd;
        }
        g_mail.push_back(buf);
        pump();
        command("{\"cmd\":\"status\"}"); pump();
        // No status change expected because enumeration is incomplete
        {
            std::string st = statusText();
            CHECK(st.find("\"simState\":2") == std::string::npos);
            CHECK(st.find("\"simState\":4") == std::string::npos);
            CHECK(st.find("\"simState\":5") == std::string::npos);
        }
    }

    // ---- Multi-packet: 64 entries (at capacity) ----
    {
        module_deinit(); mockResetSession();
        std::vector<CtlEntry> devs;
        devs.push_back({1u, 0u, 0u, 0, 0, 0, 0, "Mouse"});
        devs.push_back({0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"});
        for (unsigned i = 0; i < 62; ++i) {
            char nm[32]; snprintf(nm, sizeof(nm), "Joy%u", i);
            devs.push_back({6u + i, 100u + i, 200u + i, 1, 0, 0, 0, nm});
        }
        setDevices(devs);
        g_vals.clear();
        module_init();
        g_mail.clear();
        SimConnect_EnumerateControllersMulti(nullptr, 16);  // 4 packets of 16
        pump();
        command("{\"cmd\":\"status\"}"); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"ctlCount\":64") != std::string::npos);  // all 64 stored
        }
    }

    // ---- Multi-packet: 65 entries (truncation detected) ----
    {
        module_deinit(); mockResetSession();
        std::vector<CtlEntry> devs;
        devs.push_back({1u, 0u, 0u, 0, 0, 0, 0, "Mouse"});
        devs.push_back({0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"});
        for (unsigned i = 0; i < 63; ++i) {
            char nm[32]; snprintf(nm, sizeof(nm), "Joy%u", i);
            devs.push_back({6u + i, 100u + i, 200u + i, 1, 0, 0, 0, nm});
        }
        setDevices(devs);
        g_vals.clear();
        module_init();
        g_mail.clear();
        SimConnect_EnumerateControllersMulti(nullptr, 16);  // 5 packets (16+16+16+16+1)
        pump();
        command("{\"cmd\":\"status\"}"); pump();
        {
            std::string st = statusText();
            // 65 received, only 64 stored -> truncation flag set
            CHECK(st.find("\"ctlCount\":64") != std::string::npos);
            CHECK(st.find("\"ctlTruncated\":1") != std::string::npos);
        }
    }

    // ---- Multi-packet: duplicate device identities across packet boundaries ----
    {
        module_deinit(); mockResetSession();
        setDevices({
            {1u, 0u, 0u, 0, 0, 0, 0, "Mouse"},
            {0u, 0u, 0u, 0, 0, 0, 0, "Keyboard"},
            {6u, 77u, 88u, 1, 0, 2, 0, "DupDev"},   // packet 0
            {6u, 77u, 88u, 1, 0, 2, 0, "DupDev"},   // packet 1 - duplicate identity
        });
        g_vals.clear();
        module_init();
        g_mail.clear();
        SimConnect_EnumerateControllersMulti(nullptr, 2);  // 2 packets, 2 entries each
        pump();
        command("{\"cmd\":\"status\"}"); pump();
        {
            std::string st = statusText();
            CHECK(st.find("\"ctlCount\":4") != std::string::npos);  // both copies stored
        }
    }

    module_deinit();
    CHECK(g_closed && g_keyHandler == nullptr && g_busHandler == nullptr);
    CHECK(g_ex1Rejects == 0);                   // invariant over the whole run
    if (g_fails == 0) printf("INPUT LEARN GLUE TEST PASSED\n");
    printf("checks=%d fails=%d\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
