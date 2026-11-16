// test_elevator_suppress.cpp - offline validation of the STAGING-ONLY
// ground-braking pitch isolation (GS_USE_LEARN + GS_USE_ELEV_SUPPRESS build of
// GentleStop.cpp + elevator_suppress.h) against mocks of the SimConnect
// event/masking services.
//
// Scope:
//   mask-client setup (OWN handle, documented HIGHEST_MASKABLE priority,
//     maskable registration of AXIS_ELEVATOR_SET, its own CallDispatch);
//   suppression policy: engaged only when enabled + activation allowed +
//     on ground + mask client healthy - INDEPENDENT of the brake gate/hysteresis;
//   failsafe direction: airborne / disarmed / learning / teardown all release
//     with ONE retransmit of the latest physical axis (normal pitch returns),
//     and no repeated competing elevator writes in any state;
//   braking input: while suppressed, braking follows the RECEIVED axis value
//     (documented -16383..16384 range, forward = negative), curve unchanged;
//   the healthy path WITHOUT suppression still reads the YOKE simvar.
//
// In-sim masking exactness (whether the engine really stops applying the
// physical axis when our maskable client consumes it) CANNOT be proven
// offline - the staged sim run on real hardware decides that. Nothing here
// writes elevator values to fake the feature.
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
#include <cstring>
#include <cstdlib>
#include <cmath>

extern "C" void module_init(void);
extern "C" void module_deinit(void);

// crash tracer: prints the exception code + faulting address without pulling in
// the real windows headers (they clash with the test stubs).
struct ExcRec { unsigned code; void* flags; void* rec; void* addr; unsigned nParms; unsigned info[15]; };
struct ExcCtx { void* dummy[2]; };
struct ExcPointers { ExcRec* rec; ExcCtx* ctx; };
#pragma pack(push,8)
struct WinCtx { unsigned char pad[0x98]; void* rsp; unsigned char pad2[0x58]; void* rip; };
#pragma pack(pop)
static long CALLBACK gsTestCrashHandler(ExcPointers* ep) {
    if (ep && ep->rec) {
        WinCtx* c = (WinCtx*)(void*)ep->ctx;
        printf("\n!! EXCEPTION 0x%08X at %p (access addr likely info[1]=%p) RIP=%p moduleBase~%p\n",
               ep->rec->code, ep->rec->addr, ep->rec->info[1], c->rip, (void*)&gsTestCrashHandler);
    }
    fflush(stdout);
    return 1 /*EXCEPTION_CONTINUE_SEARCH*/;
}
extern "C" __declspec(dllimport) void* __stdcall SetUnhandledExceptionFilter(void*);
extern "C" __declspec(dllimport) void* __stdcall SetUnhandledExceptionFilter(void*);

#ifndef GS_CFG_PATH
#define GS_CFG_PATH "unused"
#endif

static int g_checks = 0, g_fails = 0;
#define CHECK(cond) do { \
    ++g_checks; if (!(cond)) { ++g_fails; printf("FAIL line %d: %s\n", __LINE__, #cond); } \
} while (0)

// --- module environment mocks ------------------------------------------------
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
static double var(const char* n) { return g_vals[(int)g_ids.at(std::string(n))]; }
static void setVar(const char* n, double v) { g_vals[(int)g_ids.at(std::string(n))] = v; }

static DispatchProc g_proc = nullptr;        // main client
static DispatchProc g_elevProc = nullptr;    // suppress client
static HANDLE g_hMain = (HANDLE)0x11;
static HANDLE g_hElev = (HANDLE)0x22;
static bool  g_failOpenMain = false, g_failOpenElev = false, g_failElevSetup = false;
static std::string g_lastMapName;
static int g_lastSendMain = 15, g_lastSendElev = 500;
static int g_elevOpenCalls = 0;

struct Tx { HANDLE h; DWORD evt; int data; };
static std::vector<Tx> g_tx;                 // every TransmitClientEvent (any handle)

HRESULT SimConnect_Open(HANDLE* h, const char* name, void*, DWORD, void*, DWORD) {
    if (name && !strcmp(name, "GentleStopElev")) {   // suppress client by name
        ++g_elevOpenCalls;
        if (g_failOpenElev) return -1;
        *h = g_hElev; return 0;
    }
    if (g_failOpenMain) return -1;
    *h = g_hMain; return 0;
}
HRESULT SimConnect_Close(HANDLE) { return 0; }
HRESULT SimConnect_AddToDataDefinition(HANDLE, DWORD, const char*, const char*, SIMCONNECT_DATATYPE, float, DWORD) { return 0; }
HRESULT SimConnect_MapClientEventToSimEvent(HANDLE h, DWORD, const char* name) {
    if (h == g_hElev) {
        if (g_failElevSetup) return -1;
        ++g_lastSendElev;
    }
    g_lastMapName = name ? name : "";
    return 0;
}
HRESULT SimConnect_RequestDataOnSimObject(HANDLE, DWORD, DWORD, DWORD, SIMCONNECT_PERIOD) { return 0; }
HRESULT SimConnect_TransmitClientEvent(HANDLE h, DWORD, DWORD evt, DWORD data, DWORD, DWORD) {
    Tx t; t.h = h; t.evt = evt; t.data = (int32_t)data;
    g_tx.push_back(t);
    if (h == g_hElev) ++g_lastSendElev; else ++g_lastSendMain;
    return 0;
}
HRESULT SimConnect_CallDispatch(HANDLE h, DispatchProc p, void*) {
    if (h == g_hElev) g_elevProc = p; else g_proc = p;
    return 0;
}
HRESULT SimConnect_GetLastSentPacketID(HANDLE h, DWORD* p) {
    if (h == g_hElev) { *p = (DWORD)++g_lastSendElev; } else { *p = (DWORD)++g_lastSendMain; }
    return 0;
}
HRESULT SimConnect_EnumerateControllers(HANDLE) {
    // empty list: the suite exercises eligibility only, no capture
    alignas(8) static char buf[sizeof(SIMCONNECT_RECV_CONTROLLERS_LIST)] = {0};
    auto* m = (SIMCONNECT_RECV_CONTROLLERS_LIST*)buf;
    m->dwID = SIMCONNECT_RECV_ID_CONTROLLERS_LIST;
    m->dwRequestID = 7; m->dwArraySize = 0; m->dwEntryNumber = 0; m->dwOutOf = 1;
    if (g_proc) g_proc((SIMCONNECT_RECV*)buf, sizeof buf, nullptr);
    return 0;
}
// learn/input services (unused by this suite, linked via GentleStop.cpp)
HRESULT SimConnect_SetInputGroupState(HANDLE, DWORD, DWORD) { return 0; }
HRESULT SimConnect_ClearInputGroup(HANDLE, DWORD) { return 0; }
HRESULT SimConnect_SetInputGroupPriority(HANDLE, DWORD, DWORD) { return 0; }
HRESULT SimConnect_MapInputEventToClientEvent_EX1(HANDLE, DWORD, const char*, DWORD, DWORD, DWORD, DWORD, BOOL) { return -1; }

// --- suppress-client registration capture -------------------------------------
static DWORD g_prio = 0; static BOOL g_maskable = FALSE; static int g_regCalls = 0;
HRESULT SimConnect_SetNotificationGroupPriority(HANDLE h, DWORD, DWORD prio) {
    if (h == g_hElev) { g_prio = prio; g_regCalls++; }
    return 0;
}
HRESULT SimConnect_AddClientEventToNotificationGroup(HANDLE h, DWORD, DWORD, BOOL maskable) {
    if (h == g_hElev) { g_maskable = maskable; g_regCalls++; }
    return 0;
}

// --- remaining module env ------------------------------------------------------
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
static void command(const char* json) { g_busHandler(json, (unsigned int)strlen(json), g_busContext); }

// --- helpers --------------------------------------------------------------------
const DWORD EVT_EL = 4702;   // elevator_suppress.h's client event id

static double T = 100.0;
static long long g_callSeq = 0;
static void frame(double yoke, double ground, double kt, double thr,
                  double parking, double brake = 0.0) {
    ++g_callSeq;
    if (((unsigned long long)(void*)g_proc) < 0x1000 || ((unsigned long long)(void*)g_proc) > 0x7fffffffffffULL)
        printf("\n!! g_proc clobbered=0x%llX at call#%lld (guard check)\n", (unsigned long long)(void*)g_proc, g_callSeq);
    alignas(8) char buf[sizeof(SIMCONNECT_RECV_SIMOBJECT_DATA) + 96] = {0};
    auto* o = (SIMCONNECT_RECV_SIMOBJECT_DATA*)buf;
    o->dwID = SIMCONNECT_RECV_ID_SIMOBJECT_DATA; o->dwRequestID = 1; o->dwDefineID = 1;
    o->dwDefineCount = 10;
    double* d = (double*)&o->dwData;
    d[0] = yoke; d[1] = ground; d[2] = kt; d[3] = 0; d[4] = thr; d[5] = 0;
    d[6] = parking; d[7] = brake; d[8] = brake; d[9] = T; T += 0.02;
    if (g_proc) g_proc((SIMCONNECT_RECV*)o, sizeof buf, nullptr);
}
static void elevAxis(int value) {   // physical axis event reaches the masked client
    alignas(8) char buf[sizeof(SIMCONNECT_RECV_EVENT) + 8] = {0};
    auto* e = (SIMCONNECT_RECV_EVENT*)buf;
    e->dwID = SIMCONNECT_RECV_ID_EVENT; e->uEventID = EVT_EL; e->dwData = (DWORD)(int32_t)value;
    if (g_elevProc) g_elevProc((SIMCONNECT_RECV*)buf, sizeof buf, nullptr);
}
static int txCount(DWORD evt) {
    int n = 0;
    for (size_t i = 0; i < g_tx.size(); ++i) if (g_tx[i].evt == evt) n++;
    return n;
}
static const Tx* lastTx() {   // latest elevator-namespace transmission
    for (size_t i = g_tx.size(); i > 0; --i) if (g_tx[i - 1].evt == EVT_EL) return &g_tx[i - 1];
    return nullptr;
}
static double brakeOut() { return var("GS_OUT_BRAKE"); }
static double brakeAxis(int axisValue, int frames) {   // suppressed braking run
    elevAxis(axisValue);
    for (int i = 0; i < frames; ++i) frame(0.0, 1.0, 0, 0, 0);
    return brakeOut();
}
static void mockReset() {
    g_lastMapName.clear(); g_prio = 0; g_maskable = FALSE;
    g_regCalls = 0; g_failOpenElev = false; g_failElevSetup = false;
    g_failOpenMain = false;
}

int main() {
#ifndef GS_USE_ELEV_SUPPRESS
    CHECK(0);
#else
    remove(GS_CFG_PATH);
    setvbuf(stdout, nullptr, _IONBF, 0);   // exact stdout position on crash
    SetUnhandledExceptionFilter((void*)gsTestCrashHandler);
    CHECK(g_regCalls == 0);   // sanity: mocks start clean

    // ---- I. failure path FIRST (clean module state): no mask client -> never suppresses
    g_failOpenElev = true;   // the "elev" client is unavailable this session
    module_init();           // must turn the feature off, not break the module
    CHECK(g_elevOpenCalls == 1);        // tried exactly once, no retry stampede
    for (int i = 0; i < 5; ++i) frame(0.0, 1.0, 0, 0, 0);
    CHECK(txCount(EVT_EL) == 0);        // NO elevator writes at all: normal control
    // braking still works on the plain YOKE simvar path (no suppression):
    for (int i = 0; i < 180; ++i) frame(-1.0, 1.0, 0, 0, 0);
    CHECK(brakeOut() > 0.98);           // the standard stick braking curve intact
    module_deinit();
    g_tx.clear();
    g_failOpenElev = false;
    int openBeforeHealthy = g_elevOpenCalls;   // the failed session already tried once

    // ---- A. healthy setup: own client, documented maskable priority ----------
    module_init();
    CHECK(g_elevOpenCalls == openBeforeHealthy + 1);              // the RETRY of a fresh lifecycle succeeds
    CHECK(g_lastMapName == "AXIS_ELEVATOR_SET");                  // against the elevator axis event
    CHECK(g_prio == SIMCONNECT_GROUP_PRIORITY_HIGHEST_MASKABLE);  // documented maskable priority band
    CHECK(g_maskable == TRUE);                                    // maskable: the engine stops receiving it
    CHECK(g_elevProc != nullptr);                                 // and owns its dispatch

    // ---- B. engage exactly on the first armed-on-ground frame ----------------
    frame(0.0, 1.0, 0, 0, 0);
    CHECK(txCount(EVT_EL) == 1);        // ONE neutral write (elevator handed to neutral), no more
    CHECK(lastTx() && lastTx()->data == 0);
    for (int i = 0; i < 5; ++i) frame(0.0, 1.0, 0, 0, 0);
    CHECK(txCount(EVT_EL) == 1);        // engaged state: no competing elevator loop

    // ---- C. braking follows the RECEIVED axis while suppressed ---------------
    double v1 = brakeAxis(-8191, 60);   // half forward, 1.2 s of approach
    CHECK(v1 > 0.45 && v1 < 0.55);
    int nTx = txCount(EVT_EL);
    double v2 = brakeAxis(-8191, 120);  // well past the smoothing transient
    CHECK(fabs(v2 - 0.49996) < 0.004);
    for (int i = 0; i < 10; ++i) frame(0.0, 1.0, 0, 0, 0);
    CHECK(txCount(EVT_EL) == nTx);      // braking does not add elevator writes

    // ---- C2. teardown WHILE suppressed releases exactly once ------------------
    int preKill = txCount(EVT_EL);
    module_deinit();
    CHECK(txCount(EVT_EL) == preKill + 1);  // exactly one release retransmit on exit
    CHECK(lastTx() && lastTx()->data == -8191);

    // ---- reload: module restarts (cfg + learned state persist via file) -------
    g_tx.clear();
    module_init();
    g_tx.clear();
    for (int i = 0; i < 5; ++i) frame(-1.0, 1.0, 0, 0, 0);  // resume on the ground armed
    elevAxis(-8191);                          // physical stick pushed halfway forward
    for (int i = 0; i < 3; ++i) frame(-1.0, 1.0, 0, 0, 0);
    CHECK(lastTx() && lastTx()->data == 0);   // engaged again (one neutral write)

int nR = 0;
    g_tx.clear();
    // ---- D. airborne: instant release, one retransmit of the last axis --------
    frame(-1.0, 0.0, 120, 30, 0, 0.2);
    nR = txCount(EVT_EL);
    CHECK(lastTx() && lastTx()->data == -8191);     // control handed back where the stick is NOW
    CHECK(nR == 1);                     // (the airborne release retransmit)
    for (int i = 0; i < 10; ++i) frame(-1.0, 0.0, 120, 30, 0, 0.2);
    CHECK(txCount(EVT_EL) == nR);       // airborne: elevator fully untouched

    // ---- E. landing re-engages -----------------------------------------------
    frame(0.0, 1.0, 60, 20, 0);
    CHECK(lastTx() && lastTx()->data == 0);         // neutral again on the ground
    CHECK(txCount(EVT_EL) == nR + 1);

    // ---- F. learning opens -> released; ends -> re-engaged --------------------
    setVar("GS_LEARN_SEQ", 1); setVar("GS_LEARN_ARM", 1);
    frame(0.0, 1.0, 0, 0, 0);
    CHECK(lastTx() && lastTx()->data == -8191);     // released while learning
    int prev = txCount(EVT_EL);
    for (int i = 0; i < 5; ++i) frame(0.0, 1.0, 0, 0, 0);
    CHECK(txCount(EVT_EL) == prev);     // pitch normal through the learn window
    setVar("GS_LEARN_SEQ", 2); setVar("GS_LEARN_ARM", 0);
    frame(0.0, 1.0, 0, 0, 0);
    CHECK(lastTx() && lastTx()->data == 0);         // learning closed: suppression returns
    CHECK(txCount(EVT_EL) == prev + 1);

    // ---- G. settings disable -> released; re-enable -> engaged ---------------
    command("{\"cmd\":\"settings\",\"enabled\":0}");
    frame(0.0, 1.0, 0, 0, 0);
    CHECK(lastTx() && lastTx()->data == -8191);     // released: disabled = normal pitch
    prev = txCount(EVT_EL);
    for (int i = 0; i < 5; ++i) frame(0.0, 1.0, 0, 0, 0);
    CHECK(txCount(EVT_EL) == prev);
    command("{\"cmd\":\"settings\",\"enabled\":1}");
    frame(0.0, 1.0, 0, 0, 0);
    CHECK(lastTx() && lastTx()->data == 0);         // armed again on the ground

    // also: explicit clear (Clear / Reset Profile path): suppression stays ON
    // because clearActivation returns to Automatic/Armed mode (it does not
    // disable GentleStop). On the ground, enabled + armed => suppression engaged.
    int clearPre = txCount(EVT_EL);
    command("{\"cmd\":\"clearActivation\"}");
    frame(0.0, 1.0, 0, 0, 0);
    CHECK(lastTx() && lastTx()->data == 0);     // cleared state: suppression re-engaged (neutral write)
    prev = txCount(EVT_EL);
    for (int i = 0; i < 5; ++i) frame(0.0, 1.0, 0, 0, 0);
    CHECK(txCount(EVT_EL) == prev);     // stays engaged while cleared

    // ---- H. teardown in the CLEARED state adds one elevator write --------------
    int pre = txCount(EVT_EL);
    module_deinit();
    CHECK(txCount(EVT_EL) == pre + 1);      // engaged: shutdown releases the axis once

    // ---- final: everything above ended with a clean teardown
    printf("elevator suppress suite: checks=%d fails=%d\n", g_checks, g_fails);
#endif
    return g_fails == 0 ? 0 : 1;
}
