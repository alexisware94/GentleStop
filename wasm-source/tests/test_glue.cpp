// Host-side smoke test of GentleStop.cpp against MOCK SimConnect / named-variable implementations.
#include <MSFS/MSFS.h>
#include <MSFS/MSFS_WindowsTypes.h>
#include <MSFS/MSFS_Events.h>
#include <MSFS/MSFS_CommBus.h>
#include <MSFS/Legacy/gauges.h>
#include <SimConnect.h>
#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void module_init(void);
extern "C" void module_deinit(void);

static std::map<int,double> g_vals; static std::map<std::string,int> g_ids; static std::vector<std::string> g_names;
ID register_named_variable(const char* n) { auto it = g_ids.find(n); if (it != g_ids.end()) return it->second; int id = (int)g_names.size(); g_names.push_back(n); g_ids[n] = id; return id; }
double get_named_variable_value(ID id) { return g_vals[id]; }
void set_named_variable_value(ID id, double v) { g_vals[id] = v; }
static double var(const char* n) { return g_vals[register_named_variable(n)]; }
static void setvar(const char* n, double x) { g_vals[register_named_variable(n)] = x; }

static DispatchProc g_proc; static int g_defs = 0, g_maps = 0; static std::vector<std::pair<int,int>> g_events; static bool g_closed = false;
static bool g_failOpen = false;
static FsEventsKeyEventHandler g_keyHandler = nullptr; static void* g_keyUserData = nullptr;
static fsCommBusWasmCallback g_busHandler = nullptr; static void* g_busContext = nullptr; static std::string g_busEvent;
HRESULT SimConnect_Open(HANDLE* h, const char*, void*, DWORD, void*, DWORD) { if (g_failOpen) return -1; *h = (HANDLE)1; return 0; }
HRESULT SimConnect_Close(HANDLE) { g_closed = true; return 0; }
HRESULT SimConnect_AddToDataDefinition(HANDLE, DWORD, const char*, const char*, SIMCONNECT_DATATYPE, float, DWORD) { g_defs++; return 0; }
HRESULT SimConnect_MapClientEventToSimEvent(HANDLE, DWORD, const char*) { g_maps++; return 0; }
HRESULT SimConnect_RequestDataOnSimObject(HANDLE, DWORD, DWORD, DWORD, SIMCONNECT_PERIOD) { return 0; }
HRESULT SimConnect_TransmitClientEvent(HANDLE, DWORD, DWORD evt, DWORD data, DWORD, DWORD) { g_events.push_back({(int)evt, (int32_t)data}); return 0; }
HRESULT SimConnect_CallDispatch(HANDLE, DispatchProc p, void*) { g_proc = p; return 0; }
// Mock input-API functions needed by input_event_api.h (GS_USE_LEARN build).
HRESULT SimConnect_SetInputGroupState(HANDLE, DWORD, DWORD) { return 0; }
HRESULT SimConnect_ClearInputGroup(HANDLE, DWORD) { return 0; }
HRESULT SimConnect_SetInputGroupPriority(HANDLE, DWORD, DWORD) { return 0; }
HRESULT SimConnect_AddClientEventToNotificationGroup(HANDLE, DWORD, DWORD, BOOL) { return 0; }
HRESULT SimConnect_SetNotificationGroupPriority(HANDLE, DWORD, DWORD) { return 0; }
HRESULT SimConnect_GetLastSentPacketID(HANDLE, DWORD*) { return 0; }
HRESULT SimConnect_EnumerateControllers(HANDLE) { return 0; }
HRESULT SimConnect_MapInputEventToClientEvent_EX1(HANDLE, DWORD, const char*, DWORD, DWORD, DWORD, DWORD, BOOL) { return 0; }
extern "C" void fsEventsRegisterKeyEventHandler(FsEventsKeyEventHandler handler, void* userData) { g_keyHandler = handler; g_keyUserData = userData; }
extern "C" void fsEventsUnregisterKeyEventHandler(FsEventsKeyEventHandler handler, void* userData) { if (g_keyHandler == handler && g_keyUserData == userData) { g_keyHandler = nullptr; g_keyUserData = nullptr; } }
extern "C" bool fsCommBusRegister(const char* eventName, fsCommBusWasmCallback callback, void* context) { g_busEvent = eventName; g_busHandler = callback; g_busContext = context; return true; }
extern "C" int fsCommBusUnregister(const char* eventName, fsCommBusWasmCallback callback) { if (g_busEvent == eventName && (!callback || g_busHandler == callback)) { g_busHandler = nullptr; g_busContext = nullptr; return 1; } return 0; }
static std::string g_lastStatus;
extern "C" bool fsCommBusCall(const char* eventName, const char* buf, unsigned int size, FsCommBusBroadcastFlags) {
    if (eventName && !strcmp(eventName, "GentleStopStatus")) g_lastStatus.assign(buf, size);
    return true;
}
static void key(FsEventId eventId) { FsVarParamArray p = {0, nullptr}; g_keyHandler(eventId, &p, g_keyUserData); }
static void command(const char* json) { g_busHandler(json, (unsigned int)strlen(json), g_busContext); }
static double T = 100.0;
static void frame(double yoke, double ground, double kt, double thr, double parking, double brake = 0.0) {
    alignas(8) char buf[sizeof(SIMCONNECT_RECV_SIMOBJECT_DATA) + 96] = {0};
    auto* o = (SIMCONNECT_RECV_SIMOBJECT_DATA*)buf; o->dwID = SIMCONNECT_RECV_ID_SIMOBJECT_DATA; o->dwRequestID = 1; o->dwDefineID = 1; o->dwDefineCount = 10;
    double* d = (double*)&o->dwData; d[0]=yoke; d[1]=ground; d[2]=kt; d[3]=0; d[4]=thr; d[5]=0; d[6]=parking; d[7]=brake; d[8]=brake; d[9]=T; T += 0.02;
    g_proc((SIMCONNECT_RECV*)o, sizeof buf, nullptr);
}
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// Models the sim feeding our own AXIS_*_BRAKE_SET writes back into BRAKE LEFT/RIGHT POSITION: the pressure
// bites our command fast but lingers above it while it falls, and drains on its own after the last write.
// The user's button adds its own pressure on top (the simvar shows the strongest source): instant with the
// press, decaying after the release. The parking brake pins the pressure regardless of both. The ledger
// decay here (0.85) is deliberately faster than the module's expectation decay (0.90), so the expectation
// always stays above the real linger: our own output can never produce an upward residual in these tests.
static double g_ownP = 0.0, g_userP = 0.0;
static void frameFB(double yoke, double ground, double kt, double thr, double parking, double user = 0.0) {
    double applied = var("GS_OUT_BRAKE");
    double decayed = g_ownP * 0.85;
    g_ownP = applied > decayed ? applied : decayed;
    g_userP = user >= g_userP ? user : g_userP * 0.85;
    double pressure = parking > 0.5 ? 1.0 : (g_ownP > g_userP ? g_ownP : g_userP);
    frame(yoke, ground, kt, thr, parking, pressure);
}
int main() {
    int fails = 0;
#ifdef GS_TEST_CFG
    remove(GS_CFG_PATH);   // defines redirected to a temp file by tools/build_wasm.ps1's test build
#endif
    module_init();
    command("{\"cmd\":\"setArmed\",\"armed\":1}");   // ensure armed for the test sequence
    CHECK(g_defs == 10); CHECK(g_maps == 2); CHECK(g_proc != nullptr); CHECK(g_keyHandler != nullptr); CHECK(g_busHandler != nullptr);
    CHECK(var("GS_ALIVE") == -1.0);   // init marker: "initialised, waiting for frames"

    // panel opens and pushes settings through the Communication Bus.
    command("{\"cmd\":\"settings\",\"enabled\":1,\"invert\":0,\"strobeLock\":0,\"speedLimitOn\":0,\"speedLimitKt\":60,\"throttleLimitOn\":1,\"throttleLimit\":50,\"deadzone\":0.1,\"curve\":0,\"smoothing\":0,\"jumpResponse\":0,\"activationEvent\":-1}");

    for (int i = 0; i < 20; i++) frame(0.0, 1, 5, 0, 0);          // idle: must NOT touch brakes
    CHECK(g_events.empty());
    for (int i = 0; i < 40; i++) frame(-0.6, 1, 5, 0, 0);         // stick forward 60% (MSFS: forward = negative)
    CHECK(!g_events.empty());
    CHECK(g_events.size() % 2 == 0 && g_events.back().second > 0);
    printf("last brake axis value: %d (expected ~ %d for 60%%)\n", g_events.back().second, (int)(-16383 + 2*16383*0.6));
    CHECK(var("GS_OUT_BRAKE") > 0.59 && var("GS_OUT_BRAKE") < 0.61);
    CHECK(var("GS_OUT_GATE") == 1.0 && var("GS_OUT_BLOCK") == 0.0 && var("GS_ALIVE") > 0);

    CHECK(var("GS_DBG_MSGS") > 0);

    // "panel closed": nothing in the module depends on it -- keep flying, brakes keep following the stick
    size_t before = g_events.size(); frame(-0.3, 1, 5, 0, 0); CHECK(g_events.size() > before);

    // Learn an activation key: the press that teaches it also arms it, so one press is enough.
    command("{\"cmd\":\"learn\",\"arm\":1}"); frame(0, 1, 5, 0, 0);
    CHECK(var("GS_OUT_LEARNING") == 1.0);
    key(4242); frame(0, 1, 5, 0, 0);
    CHECK(var("GS_LEARN_RESULT") == 4242.0 && var("GS_LEARN_RESULT_SEQ") == 1.0);
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);
    size_t learnedBefore = g_events.size();
    for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 0, 0);   // brake ramps over a few frames (8/s, 0.02 s)
    CHECK(var("GS_OUT_ACTIVATION") == 1.0 && var("GS_OUT_BRAKE") > 0.59 && g_events.size() > learnedBefore);
    key(4242);                                            // press again -> disarmed
    for (int i = 0; i < 10; i++) frame(-0.6, 1, 5, 0, 0);
    CHECK(var("GS_OUT_ACTIVATION") == 0.0 && var("GS_OUT_BLOCK") == 7.0 && var("GS_OUT_BRAKE") < 0.01);
    key(4242);                                            // and armed again
    for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 0, 0);
    CHECK(var("GS_OUT_ACTIVATION") == 1.0 && var("GS_OUT_BRAKE") > 0.59);

    // The default BRAKES action is observable as brake pressure even if its button does not raise a Key Event.
    command("{\"cmd\":\"learn\",\"arm\":1}");
    frame(0, 1, 5, 0, 0, 0.0);                                  // stick released: our own braking stops, gate drops
    CHECK(var("GS_OUT_LEARNING") == 1.0);
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);                     // learn mode never brakes with the stick
    for (int i = 0; i < 15; i++) frame(0, 1, 5, 0, 0, 1.0);     // hold the brake button
    // Under GS_USE_LEARN, brake-pressure capture is disabled by design
    // (observeDefaultBrake() only records, does not capture). The key capture
    // at line 110-111 is cleared when the learn window restarts, so the result
    // reverts to the invalid-id sentinel until a new capture completes.
    CHECK(var("GS_LEARN_RESULT") == -1.0); CHECK(var("GS_LEARN_RESULT_SEQ") == 1.0);
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);                     // learn mode blocks activation
    frame(0, 1, 5, 0, 0, 0.0);                                  // release
    frame(0, 1, 5, 0, 0, 1.0);                                  // press again: learn still active, no toggle
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);                     // still learn-blocked
    command("{\"cmd\":\"learn\",\"arm\":0}");                      // cancel learn
    frame(0, 1, 5, 0, 0, 0.0);
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);                     // armed from key capture
    frame(0, 1, 5, 0, 0, 0.0);

    // Regression: our own stick braking also drives BRAKE LEFT/RIGHT POSITION. That feedback must never
    // be mistaken for the user's button (it used to toggle the activation gate back off at once).
    for (int i = 0; i < 30; i++) frameFB(-1.0, 1, 5, 0, 0);     // full forward stick -> 100% brake
    CHECK(var("GS_OUT_ACTIVATION") == 1.0 && var("GS_OUT_BRAKE") > 0.9);
    for (int i = 0; i < 50; i++) frameFB(0.0, 1, 5, 0, 0);      // release: pressure lingers then drops
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);
    for (int i = 0; i < 30; i++) frameFB(-0.6, 1, 5, 0, 0);     // brake again: still armed
    CHECK(var("GS_OUT_ACTIVATION") == 1.0 && var("GS_OUT_BRAKE") > 0.59);
    for (int i = 0; i < 50; i++) frameFB(0.0, 1, 5, 0, 0);
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);
    // the real button (learned key), with no stick braking: toggles the gate off
    key(4242); frame(0, 1, 5, 0, 0, 0.0);
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);
    frame(0, 1, 5, 0, 0, 0.0);
    for (int i = 0; i < 20; i++) frame(-0.6, 1, 5, 0, 0, 0.0);  // disarmed: forward stick must not brake
    CHECK(var("GS_OUT_ACTIVATION") == 0.0 && var("GS_OUT_BLOCK") == 7.0 && var("GS_OUT_BRAKE") < 0.01);

    // press the learned key once more -> armed again for the remaining checks
    key(4242); frame(0, 1, 5, 0, 0, 0.0);
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);

    // blocked: throttle high -> brake released, reason = Throttle high (5)
    for (int i = 0; i < 30; i++) frame(-0.6, 1, 5, 80, 0, 0.0);
    CHECK(var("GS_OUT_BRAKE") == 0.0 && var("GS_OUT_BLOCK") == 5.0 && g_events.back().second == -16383);

    // ---- Task B regression: throttle-limit config propagation (panel -> module -> braking controller) ----
    // For each limit (50/70/30): test throttle just below, at, and above the configured limit.
    // Respects existing comparison semantics: throttlePct >= limit blocks.
    {
        // 70% limit: 69 allowed, 70 blocked, 80 blocked
        command("{\"cmd\":\"settings\",\"enabled\":1,\"invert\":0,\"strobeLock\":0,\"speedLimitOn\":0,\"speedLimitKt\":60,\"throttleLimitOn\":1,\"throttleLimit\":70,\"deadzone\":0.1,\"curve\":0,\"smoothing\":0,\"jumpResponse\":0,\"activationEvent\":-1}");
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 69, 0, 0.0);
        CHECK(var("GS_OUT_BLOCK") == 0.0 && var("GS_OUT_BRAKE") > 0.0);   // 69 < 70 : allowed
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 70, 0, 0.0);
        CHECK(var("GS_OUT_BLOCK") == 5.0 && var("GS_OUT_BRAKE") == 0.0);  // 70 >= 70 : blocked
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 80, 0, 0.0);
        CHECK(var("GS_OUT_BLOCK") == 5.0 && var("GS_OUT_BRAKE") == 0.0);  // 80 >= 70 : blocked
    }
    {
        // 50% limit: 49 allowed, 50 blocked, 60 blocked
        command("{\"cmd\":\"settings\",\"enabled\":1,\"invert\":0,\"strobeLock\":0,\"speedLimitOn\":0,\"speedLimitKt\":60,\"throttleLimitOn\":1,\"throttleLimit\":50,\"deadzone\":0.1,\"curve\":0,\"smoothing\":0,\"jumpResponse\":0,\"activationEvent\":-1}");
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 49, 0, 0.0);
        CHECK(var("GS_OUT_BLOCK") == 0.0 && var("GS_OUT_BRAKE") > 0.0);   // 49 < 50 : allowed
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 50, 0, 0.0);
        CHECK(var("GS_OUT_BLOCK") == 5.0 && var("GS_OUT_BRAKE") == 0.0);  // 50 >= 50 : blocked
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 60, 0, 0.0);
        CHECK(var("GS_OUT_BLOCK") == 5.0 && var("GS_OUT_BRAKE") == 0.0);  // 60 >= 50 : blocked
    }
    {
        // 30% limit: 29 allowed, 30 blocked, 40 blocked
        command("{\"cmd\":\"settings\",\"enabled\":1,\"invert\":0,\"strobeLock\":0,\"speedLimitOn\":0,\"speedLimitKt\":60,\"throttleLimitOn\":1,\"throttleLimit\":30,\"deadzone\":0.1,\"curve\":0,\"smoothing\":0,\"jumpResponse\":0,\"activationEvent\":-1}");
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 29, 0, 0.0);
        CHECK(var("GS_OUT_BLOCK") == 0.0 && var("GS_OUT_BRAKE") > 0.0);   // 29 < 30 : allowed
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 30, 0, 0.0);
        CHECK(var("GS_OUT_BLOCK") == 5.0 && var("GS_OUT_BRAKE") == 0.0);  // 30 >= 30 : blocked
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 40, 0, 0.0);
        CHECK(var("GS_OUT_BLOCK") == 5.0 && var("GS_OUT_BRAKE") == 0.0);  // 40 >= 30 : blocked
    }
    {
        // throttle limit off: 90% throttle not blocked
        command("{\"cmd\":\"settings\",\"enabled\":1,\"invert\":0,\"strobeLock\":0,\"speedLimitOn\":0,\"speedLimitKt\":60,\"throttleLimitOn\":0,\"throttleLimit\":50,\"deadzone\":0.1,\"curve\":0,\"smoothing\":0,\"jumpResponse\":0,\"activationEvent\":-1}");
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 90, 0, 0.0);
        CHECK(var("GS_OUT_BLOCK") == 0.0 && var("GS_OUT_BRAKE") > 0.0);    // limit off : not blocked
    }

    // ---- Task B: settings reload/persistence - throttle limit survives module reload ----
    {
        // Set a non-default limit, save to cfg, re-init, verify the limit is used after reload.
        command("{\"cmd\":\"settings\",\"enabled\":1,\"invert\":0,\"strobeLock\":0,\"speedLimitOn\":0,\"speedLimitKt\":60,\"throttleLimitOn\":1,\"throttleLimit\":70,\"deadzone\":0.1,\"curve\":0,\"smoothing\":0,\"jumpResponse\":0,\"activationEvent\":-1}");
        for (int i = 0; i < 80; i++) frame(-0.6, 1, 5, 0, 0, 0.0);   // debounce the cfg save (>1 s)
        // 70% limit still active before reload
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 60, 0, 0.0);
        CHECK(var("GS_OUT_BLOCK") == 0.0 && var("GS_OUT_BRAKE") > 0.0);   // 60 < 70 : allowed
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 80, 0, 0.0);
        CHECK(var("GS_OUT_BLOCK") == 5.0 && var("GS_OUT_BRAKE") == 0.0);  // 80 >= 70 : blocked

        // Reload the module (sim restart)
        module_deinit();
        CHECK(g_closed);
        g_vals.clear();
        module_init();
        // After reload, limit=70 should be loaded from cfg and used by braking
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 60, 0, 0.0);
        CHECK(var("GS_OUT_BLOCK") == 0.0 && var("GS_OUT_BRAKE") > 0.0);   // 60 < 70 : allowed after reload
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 80, 0, 0.0);
        CHECK(var("GS_OUT_BLOCK") == 5.0 && var("GS_OUT_BRAKE") == 0.0);  // 80 >= 70 : blocked after reload
    }

    // ---- Stage 4.4: NaN L: variable rejection in pollPanelSettings ----
    {
        // throttleLimit=70 is active via CommBus
        command("{\"cmd\":\"settings\",\"enabled\":1,\"invert\":0,\"strobeLock\":0,\"speedLimitOn\":0,\"speedLimitKt\":60,\"throttleLimitOn\":1,\"throttleLimit\":70,\"deadzone\":0.1,\"curve\":0,\"smoothing\":0,\"jumpResponse\":0,\"activationEvent\":-1}");
        for (int i = 0; i < 5; i++) frame(0, 1, 5, 0, 0, 0.0);
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 60, 0, 0.0);
        CHECK(var("GS_OUT_BLOCK") == 0.0 && var("GS_OUT_BRAKE") > 0.0);   // 60 < 70 : allowed

        // Corrupt GS_THROTTLELIMIT L: var with NaN, then force pollPanelSettings via new seq
        {
            double zero = 0.0;
            setvar("GS_THROTTLELIMIT", zero / zero);   // NaN (simulates unset L: var in real MSFS)
            setvar("GS_SETTINGS_SEQ", 99.0);           // different from g_lastSeq -> pollPanelSettings runs
        }
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 60, 0, 0.0);
        // NaN throttleLimit must be rejected: 70% limit still in force
        CHECK(var("GS_OUT_BLOCK") == 0.0 && var("GS_OUT_BRAKE") > 0.0);   // 60 < 70 : still allowed

        // Corrupt GS_SETTINGS_SEQ with NaN -> pollPanelSettings must return false entirely
        {
            double zero = 0.0;
            setvar("GS_SETTINGS_SEQ", zero / zero);   // NaN seq
            setvar("GS_THROTTLELIMIT", 30.0);        // valid but must NOT be applied (seq NaN -> skip all)
        }
        for (int i = 0; i < 5; i++) frame(-0.6, 1, 5, 80, 0, 0.0);
        // 70% limit still in force (NaN seq rejected everything)
        CHECK(var("GS_OUT_BLOCK") == 5.0 && var("GS_OUT_BRAKE") == 0.0);   // 80 >= 70 : blocked
    }
    // leave the brakes released so the following airborne check sees no events
    for (int i = 0; i < 10; i++) frame(0.0, 1, 5, 0, 0, 0.0);
    CHECK(var("GS_OUT_BRAKE") == 0.0);
    // airborne -> reason 2, and no more events once released
    size_t n = g_events.size(); for (int i = 0; i < 10; i++) frame(-0.6, 0, 120, 0, 0); CHECK(g_events.size() == n && var("GS_OUT_BLOCK") == 2.0);

#ifndef GS_USE_LEARN
    // A brake control that only produces partial pressure must still be learnable (the level is measured).
    command("{\"cmd\":\"learn\",\"arm\":1}");
    frame(0, 1, 5, 0, 0, 0.0);                                  // base line: brake released when Learn is clicked
    for (int i = 0; i < 15; i++) frame(0, 1, 5, 0, 0, 0.55);
    CHECK(var("GS_LEARN_RESULT") == -2.0 && var("GS_LEARN_RESULT_SEQ") == 3.0);
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);                     // learned and armed by that one press
    frame(0, 1, 5, 0, 0, 0.0);
    frame(0, 1, 5, 0, 0, 0.55);                                  // half a press is enough now -> disarms
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);
    frame(0, 1, 5, 0, 0, 0.0);
    frame(0, 1, 5, 0, 0, 0.55);                                  // and arms again
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);
    frame(0, 1, 5, 0, 0, 0.0);
    frame(0, 1, 5, 0, 0, 1.0);                                   // a full press still works
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);
    frame(0, 1, 5, 0, 0, 0.0);
    frame(0, 1, 5, 0, 0, 1.0);
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);
    frame(0, 1, 5, 0, 0, 0.0);

    // A quick second tap while the pressure is still decaying must count: the detector re-arms once the
    // pressure has fallen one pressRise below the peak (0.55 - 0.06 = 0.49 for these learned levels),
    // not at "almost released".
    frame(0, 1, 5, 0, 0, 0.0);                                  // fully released
    frame(0, 1, 5, 0, 0, 0.55);                                  // tap -> disarms
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);
    frame(0, 1, 5, 0, 0, 0.20);                                  // only part-way back down -> re-armed detector
    frame(0, 1, 5, 0, 0, 0.55);                                  // immediate second tap -> arms
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);
    // A tap too short to be sampled at full pressure still counts (the peak is latched).
    frame(0, 1, 5, 0, 0, 0.0);
    frame(0, 1, 5, 0, 0, 1.0); frame(0, 1, 5, 0, 0, 0.0);        // one-frame tap -> disarms
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);
    // A blip below the minimum rise (0.06 for these learned levels: 0.05 * 0.60 = 0.03 is floored to
    // the 0.06 minimum) is not a press. Detector stays disarmed (the one-frame tap at line 187
    // disarmed it, and below-threshold noise does not re-arm).
    frame(0, 1, 5, 0, 0, 0.04); frame(0, 1, 5, 0, 0, 0.0);       // noise -> nothing
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);
    // Threshold boundary: a rise exactly at the floor (0.06) IS a press.
    frame(0, 1, 5, 0, 0, 0.06); frame(0, 1, 5, 0, 0, 0.0);       // at threshold -> fires
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);
    frame(0, 1, 5, 0, 0, 0.0);                                   // release -> detector re-arms
    // Threshold boundary: a rise just below the floor (0.05) is NOT a press.
    frame(0, 1, 5, 0, 0, 0.05); frame(0, 1, 5, 0, 0, 0.0);       // just below threshold -> nothing
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);   // unchanged
    // Noise hovering around the threshold must not produce spurious toggles.
    frame(0, 1, 5, 0, 0, 0.03); frame(0, 1, 5, 0, 0, 0.05); frame(0, 1, 5, 0, 0, 0.04); frame(0, 1, 5, 0, 0, 0.0);
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);   // noise ignored

    // A SHORT tap counts: the rise is what matters, not the absolute level the brake reaches.
    frame(0, 1, 5, 0, 0, 0.30); frame(0, 1, 5, 0, 0, 0.0);       // tap to 30% -> arms
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);
    frame(0, 1, 5, 0, 0, 0.30); frame(0, 1, 5, 0, 0, 0.0);       // same tap again -> disarms
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);
    frame(0, 1, 5, 0, 0, 1.0); frame(0, 1, 5, 0, 0, 0.0);        // full press -> armed again
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);

    // Learned levels here are 0..0.60 → pressRise = max(0.05 * 0.60, 0.06) = 0.06 (floor dominates).
    // A slow ramp peaking at only ~0.09 must still count as a press: the rise matters, not how fast
    // the pressure climbs. This is what makes a genuinely short tap register quickly.
    frame(0, 1, 5, 0, 0, 0.02);
    frame(0, 1, 5, 0, 0, 0.05);
    frame(0, 1, 5, 0, 0, 0.09);          // crosses the 0.06 floor -> must fire
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);   // slow-ramp tap disarms
    frame(0, 1, 5, 0, 0, 0.0);           // release -> detector re-arms

    // A held press produces exactly one toggle: pressure stays high, so the residual never reaches
    // peak - pressRise and the detector never re-arms mid-hold.
    frame(0, 1, 5, 0, 0, 0.55);          // press -> arms
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);
    for (int i = 0; i < 30; i++) frame(0, 1, 5, 0, 0, 0.55);  // held for ~0.6 s
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);   // still exactly one toggle, no double
    frame(0, 1, 5, 0, 0, 0.0);           // release -> re-arms
    frame(0, 1, 5, 0, 0, 0.55);          // second distinct tap -> disarms again
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);

    // Noise below and around the floor.
    frame(0, 1, 5, 0, 0, 0.00);          // baseline trough
    frame(0, 1, 5, 0, 0, 0.04);          // below the 0.06 floor -> not a press
    frame(0, 1, 5, 0, 0, 0.00);
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);
    frame(0, 1, 5, 0, 0, 0.07); frame(0, 1, 5, 0, 0, 0.0);   // just above the floor -> IS a press
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);

    // Pressing the learned button while GentleStop itself is stick-braking must disarm it: the pressure
    // the sim shows above our own commanded level (the residual) is the only thing that rises with the
    // user's button, so the press separates cleanly from our own braking.
    for (int i = 0; i < 30; i++) frameFB(-0.6, 1, 5, 0, 0);       // stick braking at ~60%
    CHECK(var("GS_OUT_BRAKE") > 0.59 && var("GS_OUT_GATE") == 1.0);
    frameFB(-0.6, 1, 5, 0, 0, 1.0);                              // the user presses while we are braking
    for (int i = 0; i < 4; i++) frameFB(0.0, 1, 5, 0, 0);         // ...and releases everything
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);                      // one press disarmed GentleStop
    CHECK(var("GS_OUT_BRAKE") < 0.01);                           // stick braking stopped at once
    for (int i = 0; i < 20; i++) frameFB(0.0, 1, 5, 0, 0);        // lingering pressure decays
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);                      // decay of anything is not a press
    frameFB(0.0, 1, 5, 0, 0, 1.0); frameFB(0.0, 1, 5, 0, 0, 0.0); // short tap -> armed again
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);
    for (int i = 0; i < 10; i++) frameFB(-0.6, 1, 5, 0, 0);       // stick braking resumes
    CHECK(var("GS_OUT_BRAKE") > 0.59);
    for (int i = 0; i < 60; i++) frameFB(0.0, 1, 5, 0, 0);        // release: our own output/decay never toggles
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);
    // rapid consecutive taps, with the pressure still partly up between them
    frameFB(0.0, 1, 5, 0, 0, 1.0); frameFB(0.0, 1, 5, 0, 0, 0.0); // tap #1 -> off
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);
    frameFB(0.0, 1, 5, 0, 0, 0.35);                              // shallow in between, high pressure lingers
    frameFB(0.0, 1, 5, 0, 0, 1.0); frameFB(0.0, 1, 5, 0, 0, 0.0); // immediate second tap -> on
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);

    // The parking brake drives the same pressure without a button: it must never toggle anything.
    for (int i = 0; i < 10; i++) frameFB(0.0, 1, 5, 0, 1);        // parked, full pressure, no button
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);                       // no spurious toggle
    for (int i = 0; i < 10; i++) frameFB(0.0, 1, 5, 0, 0);        // released
    frameFB(0.0, 1, 5, 0, 0, 1.0); frameFB(0.0, 1, 5, 0, 0, 0.0); // a normal tap still toggles
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);
    frameFB(0.0, 1, 5, 0, 0, 1.0); frameFB(0.0, 1, 5, 0, 0, 0.0); // and back on
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);

    // Known limit (deliberate: full braking strength is preserved): with the stick fully forward our own
    // output commands the full brake range, so a full user press adds nothing above it and cannot be
    // separated. Partial stick braking (the normal roll/taxi case) leaves residual headroom.
    for (int i = 0; i < 30; i++) frameFB(-1.0, 1, 5, 0, 0);       // full stick: brake output 100%
    CHECK(var("GS_OUT_BRAKE") > 0.9 && var("GS_OUT_GATE") == 1.0);
    frameFB(-1.0, 1, 5, 0, 0, 1.0);
    for (int i = 0; i < 3; i++) frameFB(-1.0, 1, 5, 0, 0);
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);                       // not separable at the pinned pressure
    for (int i = 0; i < 8; i++) frameFB(-0.5, 1, 5, 0, 0);        // relax to ~50% braking
    frameFB(-0.5, 1, 5, 0, 0, 1.0);                               // now the press shows up (1.0 - 0.5)
    for (int i = 0; i < 3; i++) frameFB(-0.5, 1, 5, 0, 0);
    CHECK(var("GS_OUT_ACTIVATION") == 0.0);
    frameFB(0.0, 1, 5, 0, 0, 1.0); frameFB(0.0, 1, 5, 0, 0, 0.0); // re-arm for the remaining checks
    CHECK(var("GS_OUT_ACTIVATION") == 1.0);

    // A learn click with the parking brake set must not capture the parking pressure as the button.
    {
        double seqBefore = var("GS_LEARN_RESULT_SEQ");
        command("{\"cmd\":\"learn\",\"arm\":1}");
        frame(0, 1, 5, 0, 0, 0.0);
        for (int i = 0; i < 30; i++) frame(0, 1, 5, 0, 1, 1.0);   // parked, pressure pinned high
        CHECK(var("GS_LEARN_RESULT_SEQ") == seqBefore);            // nothing captured while parked
        command("{\"cmd\":\"learn\",\"arm\":0}");
        frame(0, 1, 5, 0, 0, 0.0);
    }

    // A duplicate learn command (one click reaching the button twice) must not wipe a window that already

    // has a reading, otherwise the press that follows would be swallowed as a fresh capture.
    command("{\"cmd\":\"learn\",\"arm\":1}");
    frame(0, 1, 5, 0, 0, 0.0);                                  // baseline
    frame(0, 1, 5, 0, 0, 0.6);                                  // press starts
    command("{\"cmd\":\"learn\",\"arm\":1}");               // duplicate click while already listening
    for (int i = 0; i < 15; i++) frame(0, 1, 5, 0, 0, 0.6);
    CHECK(var("GS_LEARN_RESULT_SEQ") == 4.0);                   // still captured
    frame(0, 1, 5, 0, 0, 0.0);
    // Opening a learn window while the brake is already down shows only a decay: that is not a press.
    command("{\"cmd\":\"learn\",\"arm\":1}");
    frame(0, 1, 5, 0, 0, 1.0);
    for (int i = 0; i < 20; i++) frame(0, 1, 5, 0, 0, 0.0);
    CHECK(var("GS_LEARN_RESULT_SEQ") == 4.0);                   // no capture from the decay
    command("{\"cmd\":\"learn\",\"arm\":0}");
    frame(0, 1, 5, 0, 0, 0.0);
#endif

    // brake again, then unload mid-brake: module must release the brakes
    for (int i = 0; i < 10; i++) frame(-0.5, 1, 5, 0, 0);
    module_deinit(); CHECK(g_closed && g_keyHandler == nullptr && g_busHandler == nullptr && g_events.back().second == -16383);

#ifdef GS_TEST_CFG
    // Persistence: the learned -2 binding must come back after a restart (\\work\\gentlestop.cfg).
    module_init();
    command("{\"cmd\":\"status\"}");
    CHECK(g_lastStatus.find("\"activationMode\":-3") != std::string::npos);
    {   // the measured press levels are persisted too
        FILE* f = fopen(GS_CFG_PATH, "r"); CHECK(f != nullptr);
        if (f) {
            char line[256]; bool hi = false, lo = false;
            while (fgets(line, sizeof line, f)) {
                if (strstr(line, "activationBrakeBase=")) hi = true;
                if (strstr(line, "activationBrakePeak=")) lo = true;
            }
            fclose(f);
            CHECK(hi && lo);
        }
    }
    // ("starts disarmed" cannot be asserted here: this harness re-inits in the same process, so the
    //  previous run's controller/gate statics are still around. In the sim module_init is a fresh load
    //  and g_activationGate is initialised false.)
    printf("status after reload: %s\n", g_lastStatus.c_str());
    module_deinit();
#endif
    // SimConnect_Open failure must be visible as marker -2 (init ran, SimConnect failed)
    g_failOpen = true; g_vals.clear(); module_init(); CHECK(var("GS_ALIVE") == -2.0);
    if (fails == 0) printf("GLUE TEST PASSED\n");
    return fails;
}
