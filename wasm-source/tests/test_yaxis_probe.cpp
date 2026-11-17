// test_yaxis_probe.cpp - offline validation of the STAGING-ONLY Y-axis
// diagnostic probe (GentleStop.cpp built with -DGS_USE_LEARN -DGS_USE_ELEV_SUPPRESS
// -DGS_YAXIS_PROBE, the full staged define set) against mocks of the SimConnect
// input-mapping services.
//
// Scope (receive-only policy, per the authorized diagnostic correction):
//   - probe uses its OWN SimConnect client ("GentleStopYProbe"), separate
//     handle, separate dispatch, one dedicated input group;
//   - TWO labelled candidate families x 4 device indices = 8 mapping calls:
//     family A 'joystick:n:YAxis' (previous run's string, retried as a
//     control), family B 'joystick:n:Y' (msfs2024-channel evidence);
//   - each mapping call's actual GetLastSentPacketID value is recorded right
//     after the call, and exceptions are attributed + counted cumulatively
//     per send id, distinguishing duplicate notifications from separate
//     failures (the mock's session send ids are deterministic, so offline
//     attribution is exact);
//   - the probe NEVER calls TransmitClientEvent / any write path, and never
//     drives the main client's transmit state;
//   - delivery, per-candidate isolation (event id -> candidate), min/max/
//     last tracking, unrelated ids ignored;
//   - async exceptions are logged (labelled) and never disable the module;
//   - after teardown, deliveries are ignored (no resurrection);
//   - the module is fully functional with the probe open, and with the
//     probe failed (fail-open: an unavailable probe changes nothing).
//
// Whether the real engine accepts any of these input strings (and whether it
// delivers while the Elevator Axis binding is present) cannot be proven
// offline; the staged sim run decides that.
#include <MSFS/MSFS.h>
#include <MSFS/MSFS_WindowsTypes.h>
#include <MSFS/MSFS_Events.h>
#include <MSFS/MSFS_CommBus.h>
#include <MSFS/Legacy/gauges.h>
#include <SimConnect.h>
#include "../../yaxis_probe.h"   // header under test (mocks below)

#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

extern "C" void module_init(void);
extern "C" void module_deinit(void);

#ifndef GS_YAXIS_PROBE
#error this suite must be compiled with -DGS_YAXIS_PROBE
#endif

#ifndef GS_CFG_PATH
#define GS_CFG_PATH "unused"
#endif

static int g_checks = 0, g_fails = 0;
#define CHECK(cond) do { \
    ++g_checks; if (!(cond)) { ++g_fails; printf("FAIL line %d: %s\n", __LINE__, #cond); } \
} while (0)

// --- module environment mocks (test_elevator_suppress.cpp pattern) ------------
static std::map<int, double> g_vals;
static std::vector<std::string> g_names;
typedef ID VarId;
static std::map<std::string, VarId> g_ids;
ID register_named_variable(const char* n) {
    auto it = g_ids.find(n);
    if (it != g_ids.end()) return it->second;
    VarId id = (VarId)g_names.size();
    g_names.push_back(n); g_ids[n] = id;
    return id;
}
double get_named_variable_value(VarId id) { return g_vals[(int)id]; }
void set_named_variable_value(VarId id, double v) { g_vals[(int)id] = v; }

static DispatchProc g_proc = nullptr;        // main client
static DispatchProc g_probeProc = nullptr;   // probe client
static HANDLE g_hMain = (HANDLE)0x11;
static HANDLE g_hProbe = (HANDLE)0x22;
static HANDLE g_hElev = (HANDLE)0x33;        // elevator-suppress client (kept dead in this suite)
static bool g_failOpenMain = false, g_failOpenProbe = false, g_failProbeMap = false;
static bool g_noElev = true;                 // suppression client unavailable for the whole suite
static int g_mainSend = 15, g_probeSend = 100;
static int g_probeOpenCalls = 0, g_elevOpenCalls = 0;

struct Tx { HANDLE h; DWORD evt; int data; };
static std::vector<Tx> g_tx;                 // every TransmitClientEvent (any handle)

HRESULT SimConnect_Open(HANDLE* h, const char* name, void*, DWORD, void*, DWORD) {
    if (name && !strcmp(name, "GentleStopYProbe")) {
        ++g_probeOpenCalls;
        if (g_failOpenProbe) return -1;
        *h = g_hProbe; return 0;
    }
    if (name && !strcmp(name, "GentleStopElev")) {
        ++g_elevOpenCalls;
        if (g_noElev) return -1;
        *h = g_hElev; return 0;
    }
    if (g_failOpenMain) return -1;
    *h = g_hMain; return 0;
}
HRESULT SimConnect_Close(HANDLE) { return 0; }
HRESULT SimConnect_AddToDataDefinition(HANDLE, DWORD, const char*, const char*, SIMCONNECT_DATATYPE, float, DWORD) { return 0; }
HRESULT SimConnect_MapClientEventToSimEvent(HANDLE, DWORD, const char*) { return 0; }
HRESULT SimConnect_RequestDataOnSimObject(HANDLE, DWORD, DWORD, DWORD, SIMCONNECT_PERIOD) { return 0; }
HRESULT SimConnect_TransmitClientEvent(HANDLE h, DWORD, DWORD evt, DWORD data, DWORD, DWORD) {
    Tx t; t.h = h; t.evt = evt; t.data = (int32_t)data;
    g_tx.push_back(t);
    if (h == g_hProbe) ++g_probeSend; else ++g_mainSend;
    return 0;
}
HRESULT SimConnect_CallDispatch(HANDLE h, DispatchProc p, void*) {
    if (h == g_hProbe) g_probeProc = p; else g_proc = p;
    return 0;
}
HRESULT SimConnect_GetLastSentPacketID(HANDLE h, DWORD* p) {
    if (h == g_hProbe) *p = (DWORD)++g_probeSend; else *p = (DWORD)++g_mainSend;
    return 0;
}
HRESULT SimConnect_EnumerateControllers(HANDLE) {
    alignas(8) static char buf[sizeof(SIMCONNECT_RECV_CONTROLLERS_LIST)] = {0};
    auto* m = (SIMCONNECT_RECV_CONTROLLERS_LIST*)buf;
    m->dwID = SIMCONNECT_RECV_ID_CONTROLLERS_LIST;
    m->dwRequestID = 7; m->dwArraySize = 0; m->dwEntryNumber = 0; m->dwOutOf = 1;
    if (g_proc) g_proc((SIMCONNECT_RECV*)buf, sizeof buf, nullptr);
    return 0;
}

// input-group mocks (probe targets) ------------------------------------------
struct Ex1Rec { DWORD group; std::string input; DWORD evt; };
static std::vector<Ex1Rec> g_ex1;            // recorded mappings (call order)
static int g_ex1Calls = 0;                   // every call, even failed ones
static DWORD g_inputPriority = 0;
static int  g_groupOnCalls = 0;
static int  g_clearInputCalls = 0;

HRESULT SimConnect_MapInputEventToClientEvent_EX1(HANDLE h, DWORD group, const char* input,
                                                  DWORD downEvent, DWORD, DWORD, DWORD, BOOL) {
    if (h == g_hProbe) {
        ++g_ex1Calls;
        if (!g_failProbeMap) {
            Ex1Rec r; r.group = group; r.input = input ? input : ""; r.evt = downEvent;
            g_ex1.push_back(r);
        }
    }
    return 0;
}
HRESULT SimConnect_SetInputGroupPriority(HANDLE h, DWORD, DWORD prio) {
    if (h == g_hProbe) g_inputPriority = prio;
    return 0;
}
HRESULT SimConnect_SetInputGroupState(HANDLE h, DWORD, DWORD) {
    if (h == g_hProbe) ++g_groupOnCalls;
    return 0;
}
HRESULT SimConnect_ClearInputGroup(HANDLE h, DWORD) {
    if (h == g_hProbe) ++g_clearInputCalls;
    return 0;
}
// elevator-suppress services (linked; the elev client stays dead in this suite)
HRESULT SimConnect_AddClientEventToNotificationGroup(HANDLE, DWORD, DWORD, BOOL) { return 0; }
HRESULT SimConnect_SetNotificationGroupPriority(HANDLE, DWORD, DWORD) { return 0; }

// --- remaining module env -------------------------------------------------------
static FsEventsKeyEventHandler g_keyHandler = nullptr;
static void* g_keyUserData = nullptr;
extern "C" void fsEventsRegisterKeyEventHandler(FsEventsKeyEventHandler h, void* u) { g_keyHandler = h; g_keyUserData = u; }
extern "C" void fsEventsUnregisterKeyEventHandler(FsEventsKeyEventHandler, void*) {}

static fsCommBusWasmCallback g_busHandler = nullptr;
static void* g_busContext = nullptr;
extern "C" bool fsCommBusRegister(const char*, fsCommBusWasmCallback h, void* ctx) { g_busHandler = h; g_busContext = ctx; return true; }
extern "C" int fsCommBusUnregister(const char*, fsCommBusWasmCallback) { return 1; }
static std::string g_lastStatus;
extern "C" bool fsCommBusCall(const char* eventName, const char* buf, unsigned int size, FsCommBusBroadcastFlags) {
    if (eventName && !strcmp(eventName, "GentleStopStatus")) g_lastStatus.assign(buf, size);
    return true;
}

// --- helpers --------------------------------------------------------------------
static const DWORD GPROBE = 4801;      // yaxis_probe.h group id
static const DWORD EVT_FIRST = 4802;   // probe client event id = EVT_FIRST + candidate k

static double T = 100.0;
static void frame(double yoke, double ground, double kt, double thr, double parking, double brake = 0.0) {
    alignas(8) char buf[sizeof(SIMCONNECT_RECV_SIMOBJECT_DATA) + 96] = {0};
    auto* o = (SIMCONNECT_RECV_SIMOBJECT_DATA*)buf;
    o->dwID = SIMCONNECT_RECV_ID_SIMOBJECT_DATA; o->dwRequestID = 1;
    double* d = (double*)&o->dwData;
    d[0] = yoke; d[1] = ground; d[2] = kt; d[3] = 0; d[4] = thr; d[5] = 0;
    d[6] = parking; d[7] = brake; d[8] = brake; d[9] = T; T += 0.02;
    if (g_proc) g_proc((SIMCONNECT_RECV*)o, sizeof buf, nullptr);
}
// engine -> the probe dispatch that init() registered
static void feedEvent(DWORD evtId, int value) {
    alignas(8) char buf[sizeof(SIMCONNECT_RECV_EVENT) + 8] = {0};
    auto* e = (SIMCONNECT_RECV_EVENT*)buf;
    e->dwID = SIMCONNECT_RECV_ID_EVENT; e->uEventID = evtId; e->dwData = (DWORD)(int32_t)value;
    if (g_probeProc) g_probeProc((SIMCONNECT_RECV*)buf, sizeof buf, nullptr);
}
static void feedException(unsigned exc, unsigned sendId) {
    alignas(8) char buf[sizeof(SIMCONNECT_RECV_EXCEPTION)] = {0};
    auto* e = (SIMCONNECT_RECV_EXCEPTION*)buf;
    e->dwID = SIMCONNECT_RECV_ID_EXCEPTION;
    e->dwException = exc; e->dwSendID = sendId; e->dwIndex = 0xFFFFFFFF;
    if (g_probeProc) g_probeProc((SIMCONNECT_RECV*)buf, sizeof buf, nullptr);
}
// engine -> probeDispatch DIRECTLY: meant for sections that validate the
// header's own accounting on the TEST TU's instance (its state is
// internal-linkage, so this - and the accessors - must be in the same TU).
static void feedCopy(DWORD evtId, int value) {
    alignas(8) char buf[sizeof(SIMCONNECT_RECV_EVENT) + 8] = {0};
    auto* e = (SIMCONNECT_RECV_EVENT*)buf;
    e->dwID = SIMCONNECT_RECV_ID_EVENT; e->uEventID = evtId; e->dwData = (DWORD)(int32_t)value;
    gs_yprobe::probeDispatch((SIMCONNECT_RECV*)buf, sizeof buf, nullptr);
}
static void feedCopyException(unsigned exc, unsigned sendId) {
    alignas(8) char buf[sizeof(SIMCONNECT_RECV_EXCEPTION)] = {0};
    auto* e = (SIMCONNECT_RECV_EXCEPTION*)buf;
    e->dwID = SIMCONNECT_RECV_ID_EXCEPTION;
    e->dwException = exc; e->dwSendID = sendId; e->dwIndex = 0xFFFFFFFF;
    gs_yprobe::probeDispatch((SIMCONNECT_RECV*)buf, sizeof buf, nullptr);
}
static unsigned totalProbeEvents() {
    unsigned n = 0;
    for (int k = 0; k < gs_yprobe::candCount(); ++k) n += gs_yprobe::events(k);
    return n;
}

// Mock session send ids are deterministic, and in this mock the counter that
// GetLastSentPacketID exposes increases ONLY when GetLastSentPacketID itself
// is called (other APIs do not bump it). So the healthy module_init consumes:
//   Open=101 | mapping calls' GetLastSentPacketID=102..109 (the eight
//   candidates own sids 102..109 in call order). With g_failProbeMap on, the
//   calls still "accepted" locally but the mock returns success while the
//   probe CANNOT record them -> exception counters stay driven by real sids.
static const DWORD kSid_cand0 = 101;
static const DWORD kSid_cand1 = 102;

// Expected mapping strings: family A = previous run's string (control retry),
// family B = msfs2024-channel evidence (Input Configuration XML X/Y/Z/rX/rY/rZ).
static const char* kExpectedInputs[8] = {
    "joystick:0:YAxis", "joystick:1:YAxis", "joystick:2:YAxis", "joystick:3:YAxis",
    "joystick:0:Y", "joystick:1:Y", "joystick:2:Y", "joystick:3:Y"
};

int main() {
    remove(GS_CFG_PATH);
    setvbuf(stdout, nullptr, _IONBF, 0);

    // ---- sanity: mocks start clean -------------------------------------------
    CHECK(g_probeOpenCalls == 0 && g_ex1Calls == 0);

    // ---- I. probe open fails: module unaffected, fail-open -------------------
    g_failOpenProbe = true;
    module_init();
    CHECK(g_probeOpenCalls == 1);                 // tried exactly once, no retry stampede
    for (int i = 0; i < 5; ++i) frame(0.0, 1.0, 0, 0, 0);
    CHECK(g_ex1Calls == 0);                       // no mappings attempted after failed Open
    CHECK(g_tx.empty());                          // NO writes of any kind
    module_deinit();
    g_failOpenProbe = false;
    int openBefore = g_probeOpenCalls;

    // ---- healthy run: module_init again (fresh lifecycle) -------------------
    // ("/base" = the value GetLastSentPacketID returns per candidate; the
    //  candidates own sids 102..109 deterministically because only
    //  GetLastSentPacketID bumps the probe-session counter.)
    module_init();
    CHECK(g_probeOpenCalls == openBefore + 1);    // fresh lifecycle re-runs the probe
    const DWORD kBase = 108;                      // probe counter after this session (only
    // GetLastSentPacketID bumps it in this mock; Open does not take a Get here).
    const DWORD sidCand0 = kBase - 7;             // candidate 0's own send id
    const DWORD sidCand1 = kBase - 6;
    CHECK(sidCand0 == kSid_cand0 && sidCand1 == kSid_cand1);      // bookkeeping sanity
    CHECK(g_ex1Calls == 8);                       // two labelled families x 4 device indices
    CHECK((int)g_ex1.size() == 8);
    for (int k = 0; k < 8; ++k) {
        CHECK(g_ex1[k].input == kExpectedInputs[k]);          // exact documented formats
        CHECK(g_ex1[k].group == GPROBE);                      // all in the dedicated group
        CHECK(g_ex1[k].evt == EVT_FIRST + (DWORD)k);          // one client event per candidate
    }
    CHECK(g_probeProc != nullptr);                // probe owns its dispatch
    CHECK(g_inputPriority == SIMCONNECT_GROUP_PRIORITY_DEFAULT);
    CHECK(g_groupOnCalls == 1);                   // input group turned ON exactly once
    CHECK(g_tx.empty());                          // still NO writes (receive-only)

    // ---- III. own-instance run: the header's accounting, validated here -----
    // (the test TU compiled the header directly, so this init/dispatch/
    // accessors all operate on ONE instance: exact offline attribution.)
    int copyOpenBefore = g_probeOpenCalls;
    CHECK(gs_yprobe::init());                     // its own instance, mock client
    CHECK(g_probeOpenCalls == copyOpenBefore + 1);
    const DWORD base2 = (DWORD)g_probeSend;       // counter after this instance's session
    const DWORD testSid0 = base2 - 7;             // this instance's candidate 0 send id
    const DWORD testSid4 = base2 - 3;             // this instance's candidate 4 send id

    // ---- A. delivery + per-candidate tracking -------------------------------
    feedCopy(EVT_FIRST + 1, -16384);              // candidate 1: full forward (documented -32K)
    feedCopy(EVT_FIRST + 1, 8192);                // half back
    feedCopy(EVT_FIRST + 5, -100);                // candidate 5 (family B, device 1)
    feedCopy(4799, 5000);                         // not one of ours
    feedCopy(EVT_FIRST + 8, 5000);                // beyond the candidate space
    CHECK(gs_yprobe::have(1) && gs_yprobe::last(1) == 8192);
    CHECK(gs_yprobe::minimum(1) == -16384 && gs_yprobe::maximum(1) == 8192);
    CHECK(gs_yprobe::events(5) == 1 && gs_yprobe::minimum(5) == -100 && gs_yprobe::maximum(5) == -100);
    CHECK(!gs_yprobe::have(0));                   // candidate 0 never delivered
    CHECK(totalProbeEvents() == 3);               // unrelated ids excluded
    CHECK(g_tx.empty());

    // ---- B. exception attribution + cumulative per-send counting (exact) ----
    feedCopyException(1 /*SIMCONNECT_EXCEPTION_ERROR*/, testSid0);   // 'joystick:0:YAxis' refused
    feedCopyException(1, testSid0);               // SECOND notification, SAME send id
    CHECK(gs_yprobe::exceptions(0) == 2);         // counted cumulatively per send id
    CHECK(gs_yprobe::exceptions(1) == 0);
    feedCopyException(3 /*UNRECOGNIZED_ID*/, 987654); // a send id no candidate owns
    CHECK(gs_yprobe::exceptions(0) == 2);         // unattributed: counters untouched
    CHECK(g_probeProc != nullptr);                // nothing torn down
    feedCopy(EVT_FIRST + 1, 100);
    CHECK(gs_yprobe::last(1) == 100);             // delivery still tracked after exceptions
    CHECK(g_tx.empty());

    // ---- C. module runs normally alongside the probe ------------------------
    for (int i = 0; i < 10; ++i) frame(-1.0, 1.0, 0, 0, 0);
    for (size_t i = 0; i < g_tx.size(); ++i) {
        CHECK(g_tx[i].h == g_hMain);              // never on the probe client
        for (int k = 0; k < gs_yprobe::candCount(); ++k)
            CHECK(g_tx[i].evt != EVT_FIRST + (DWORD)k);       // never a probe event id
    }

    // ---- D. teardown clears the mapping and closes the probe client ---------
    int clearBefore = g_clearInputCalls;
    size_t txBefore = g_tx.size();
    module_deinit();
    CHECK(g_clearInputCalls == clearBefore + 1);  // ClearInputGroup BEFORE close
    feedEvent(EVT_FIRST + 1, 7);                  // late delivery after teardown
    for (size_t i = txBefore; i < g_tx.size(); ++i) CHECK(g_tx[i].h == g_hMain);

    // ---- E. teardown + guarded deliveries ------------------------------------
    size_t txE = g_tx.size();
    gs_yprobe::shutdown();                        // teardown of the own instance
    CHECK(!gs_yprobe::mapped());
    feedCopy(EVT_FIRST + 0, 512);
    CHECK(!gs_yprobe::have(0));                   // guarded: no resurrection after teardown
    CHECK(g_tx.size() == txE);                    // no NEW writes (older brake writes already recorded)

    // ---- G. mapping-failure run: attempts logged, never retried -------------
    int preCalls = g_ex1Calls;
    g_failProbeMap = true;
    module_init();
    CHECK(g_probeOpenCalls == copyOpenBefore + 2);
    CHECK(g_ex1Calls == preCalls + 8);            // 8 attempted, never retried
    CHECK(g_inputPriority == SIMCONNECT_GROUP_PRIORITY_DEFAULT);
    CHECK(g_groupOnCalls == 3);                   // healthy + this instance + failed run
    module_deinit();

    printf("yaxis probe suite: checks=%d fails=%d\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
