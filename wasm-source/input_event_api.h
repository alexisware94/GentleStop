// input_event_api.h - production SimConnect adapter for the learned discrete
// input toggle (compiled only under GS_USE_LEARN).
//
// Implements gslearn::IInputApi (tests/input_learn.h, PINNED) over real
// SimConnect using the probe v13-v18 validated call sequence:
//
//   MapClientEventToSimEvent(down [, up] private)
//   MapInputEventToClientEvent_EX1(GROUP, bareString, down, 0, up, 0, FALSE)
//   AddClientEventToNotificationGroup(NGROUP, id, FALSE)   [per mapped id]
//   SetNotificationGroupPriority(NGROUP, DEFAULT)          [mandatory, once]
//   SetInputGroupPriority(GROUP, DEFAULT)                  [via IInputApi]
//   SetInputGroupState(GROUP, ON)                          [via IInputApi]
//
// Production-only duties on top of the pinned interface (approved decisions):
//
//   D1 translation: the pinned design emits "keys:<K>" strings; only the bare
//      grammar is accepted by the sim, so "keys:<K>" is translated to bare
//      "<K>". In THIS phase the keyboard fan is policy-DISABLED (keyboard
//      learning keeps using the existing fsEvents path, which cannot race the
//      fan): setKeysPolicy defaults to false and keyboard strings are refused
//      before any SimConnect call. The translation itself is kept and
//      unit-tested for the phase 2 keyboard fan.
//
//   Notification wiring: every mapped id joins the notification group with
//      bMaskable = FALSE everywhere; the group priority is set once.
//
//   Session-unique ids: mappings cannot be unmapped and reusing a client event
//      id raises EVENT_ID_DUPLICATE (=9), so EVERY (re)mapping allocates FRESH
//      ids from a per-session monotonically increasing space. Design ids
//      (EVT_ACT_DOWN/UP 500/501 and the EVT_FAN_BASE 1000+ fan) are translated
//      on the way out and translated BACK on delivery, so the pinned state
//      machine always sees its own stable design ids.
//
//   Budget: 512 client event ids per session. Learning stops (HR_FAIL) with a
//      bounded console line when the budget is reached; a small reserve keeps
//      the runtime re-bind (restored binding) working after learning exhausts
//      the fan space.
//
// Send-id annotation ring (probe pattern): MapInputEventToClientEvent_EX1 and
// friends may return S_OK while the server later delivers an exception naming
// the dwSendID of the rejected packet - the ring attributes those packets.
//
// No STL: the wasm toolchain links only the wasi C library.
//
// Freestanding C++ ABI note: SimApi derives from gslearn::IInputApi, whose
// virtual destructor and pure virtuals make the compiler-reference two
// Itanium-ABI symbols (operator delete from the deleting-destructor vtable
// slot, __cxa_pure_virtual from the abstract base sub-object vtable). The
// sim host exports neither. Both stubs below are unreachable in normal
// operation - the wasm links no C++ heap (no operator new reference), and
// SimApi overrides every pure virtual - they only keep the module from
// importing symbols the sim cannot resolve.
#ifndef GS_INPUT_EVENT_API_H
#define GS_INPUT_EVENT_API_H

#include <MSFS/MSFS_WindowsTypes.h>   // DWORD etc., before SimConnect.h
#include <SimConnect.h>
#include "tests/input_learn.h"        // gslearn state machine + IInputApi

// Freestanding ABI stubs (see note above): the wasm toolchain only, guarded
// because only __wasi__ lacks the C++ runtime. BOTH are unreachable in normal
// operation (init/bind/learn/capture/dispatch/rebind/deinit). They exist so
// the vtable machinery can reference them without importing symbols the sim
// does not export. If any of them is ever EXECUTED, that is a bug (see each
// comment), so both fail fast: a bounded console line + wasm "unreachable"
// (module stops immediately; the fault shows up in the sim log/console).
#ifdef __wasi__
// Deleting-destructor vtable slot: calling this means a `delete` expression
// ran on a non-heap object - freeing a global/stack pointer (heap corruption).
// The module never heap-allocates (no operator new reference exists), so this
// can only mean a bug; trap rather than corrupt memory (worse than no-op).
inline void operator delete(void*) noexcept {
    printf("GentleStop: operator delete reached - module stopped\n");
    __builtin_trap();
}
// Pure-virtual vtable slot: calling this means a virtual was invoked on an
// object whose derived constructor has not finished (or a corrupt vtable) -
// e.g. calling virtuals from IInputApi's ctor/dtor. No such call exists in
// the module; trap like libc++abi's "pure virtual method called" abort.
extern "C" void __cxa_pure_virtual() {
    printf("GentleStop: __cxa_pure_virtual reached - module stopped\n");
    __builtin_trap();
}
#endif

namespace gsim {

using gslearn::HR_OK;
using gslearn::HR_FAIL;
using gslearn::EVT_ACT_DOWN;
using gslearn::EVT_ACT_UP;
using gslearn::GROUP_PRIORITY;

// Routing group (input side) and receiver group (notification side) are
// separate ids on purpose, exactly as validated by probes v13-v18.
static const SIMCONNECT_INPUT_GROUP_ID GROUP = (SIMCONNECT_INPUT_GROUP_ID)1;
static const SIMCONNECT_NOTIFICATION_GROUP_ID NGROUP =
    (SIMCONNECT_NOTIFICATION_GROUP_ID)2;

// Session-unique client event id space. The production module already uses ids
// 1 and 2 for AXIS_*_BRAKE_SET; the adapter allocates above them.
static const int SIMID_FIRST     = 100;
static const int GS_EVENT_BUDGET = 512;   // approved session budget (ids)
static const int GS_BIND_RESERVE = 2;     // ids preserved for re-bind

// Pinned-header constants (namespaced in tests/input_learn.h). The
// using-declarations below keep the call sites readable without leaking the
// namespace to includers; a namespace-scope using works on every compiler,
// unlike a class-scope one for namespace-scope const objects (MSVC C2886).
using gslearn::HR_OK;
using gslearn::HR_FAIL;
using gslearn::EVT_ACT_DOWN;
using gslearn::EVT_ACT_UP;
using gslearn::GROUP_PRIORITY;

class SimApi : public gslearn::IInputApi {
public:
    explicit SimApi(HANDLE h = 0)
        : m_h(h), m_next(SIMID_FIRST), m_notifyPrio(false),
          m_failures(0), m_budgetWarned(false), m_policyWarned(false),
          m_allowKeys(false), m_lastSendId(-1), m_lastHr(HR_OK) {
        for (int i = 0; i < GS_EVENT_BUDGET; ++i) m_simToDesign[i] = 0;
        for (int i = 0; i < ANNOT_MAX; ++i) { m_annotId[i] = -1; m_annotText[i][0] = 0; }
    }

    // A fresh SimConnect handle is a fresh session: the notification-group
    // priority, the id space and the annotation ring are per-SESSION server
    // state (a reconnect without a wasm reload must re-establish them).
    void setHandle(HANDLE h) {
        m_h = h;
        if (!h) return;
        m_next = SIMID_FIRST;
        for (int i = 0; i < GS_EVENT_BUDGET; ++i) m_simToDesign[i] = 0;
        m_notifyPrio = false;
        m_budgetWarned = false;
        m_lastSendId = -1;
        m_lastHr = HR_OK;
        for (int i = 0; i < ANNOT_MAX; ++i) { m_annotId[i] = -1; m_annotText[i][0] = 0; }
    }
    // D1 keyboard policy: false (production default) refuses "keys:..." strings
    // so the learn fan never races the fsEvents keyboard capture.
    void setKeysPolicy(bool allow) { m_allowKeys = allow; }

    long mapInput(const char* input, int downEvent, int upEvent) override {
        if (!m_h) return HR_FAIL;
        const char* bare = policy(input);
        if (!bare) return HR_FAIL;
        // Budget: learn-class calls stop before the reserve; bind-class calls
        // (the runtime re-bind, design ids EVT_ACT_DOWN/UP) may use it.
        bool bindClass = (downEvent == EVT_ACT_DOWN) || (upEvent == EVT_ACT_UP);
        int wanted = 1 + (upEvent >= 0 ? 1 : 0);
        int used = m_next - SIMID_FIRST;
        if (used + wanted > GS_EVENT_BUDGET - (bindClass ? 0 : GS_BIND_RESERVE)) {
            if (!m_budgetWarned) {
                m_budgetWarned = true;
                printf("GentleStop: input event budget exhausted (%d ids per\n"
                       "GentleStop:   session); restart the flight to reset\n",
                       GS_EVENT_BUDGET);
            }
            return HR_FAIL;
        }
        // Allocate FRESH sim ids (relearn cycles must never reuse an id).
        int simDown = m_next++;
        char tag[60];
        snprintf(tag, sizeof(tag), "MapClientEvent %.22s down=%d %s",
                 bare, simDown, simEventName ? "named" : "private");
        HRESULT hr = SimConnect_MapClientEventToSimEvent(
                m_h, (SIMCONNECT_CLIENT_EVENT_ID)simDown, simEventName);
        m_lastHr = (long)hr;
        tagCall(tag);                       // every packet gets labelled: an
        if (FAILED(hr)) return HR_FAIL;     // exception names its send id, and
        int simUp = -1;                     // only per-call tags can point at
        if (upEvent >= 0) {                 // the exact failing MapClientEvent.
            simUp = m_next++;
            snprintf(tag, sizeof(tag), "MapClientEvent %.22s up=%d private",
                     bare, simUp);
            hr = SimConnect_MapClientEventToSimEvent(
                    m_h, (SIMCONNECT_CLIENT_EVENT_ID)simUp, simEventName);
            m_lastHr = (long)hr;
            tagCall(tag);
            if (FAILED(hr)) return HR_FAIL;
        }
        snprintf(tag, sizeof(tag), "MapInput %.26s down=%d up=%d",
                 bare, simDown, simUp);
        hr = SimConnect_MapInputEventToClientEvent_EX1(
            m_h, GROUP, bare, (SIMCONNECT_CLIENT_EVENT_ID)simDown, 0,
            upEvent >= 0 ? (SIMCONNECT_CLIENT_EVENT_ID)simUp
                         : (SIMCONNECT_CLIENT_EVENT_ID)SIMCONNECT_UNUSED,
            0, /*bMaskable*/ FALSE);
        m_lastHr = (long)hr;
        tagCall(tag);
        if (FAILED(hr)) return (long)hr;
        // Record the reverse translation. On a relearn the design id is reused
        // with a FRESH sim id, so every allocation always writes its entry;
        // stale old entries are harmless (ClearInputGroup removed their input
        // side, so those ids can never deliver again).
        m_simToDesign[simDown - SIMID_FIRST] = downEvent;
        if (simUp >= 0) m_simToDesign[simUp - SIMID_FIRST] = upEvent;
        if (addNotify(simDown) != HR_OK) return HR_FAIL;
        if (simUp >= 0 && addNotify(simUp) != HR_OK) return HR_FAIL;
        printf("GentleStop: map '%.24s' ids %d,%d\n", bare, simDown, simUp);
        return HR_OK;
    }

    long clearGroup() override {
        tagCall("ClearInputGroup");
        return (long)SimConnect_ClearInputGroup(m_h, GROUP);
    }
    long setPriority(unsigned priority) override {
        tagCall("SetInputGroupPriority");
        return (long)SimConnect_SetInputGroupPriority(m_h, GROUP, (DWORD)priority);
    }
    long setGroupOn() override {
        tagCall("SetInputGroupState(ON)");
        return (long)SimConnect_SetInputGroupState(m_h, GROUP, SIMCONNECT_STATE_ON);
    }

    // Reverse translation: delivered SimConnect id -> pinned design id.
    int simToDesign(int simId) const {
        if (simId < SIMID_FIRST || simId >= m_next) return -1;
        return m_simToDesign[simId - SIMID_FIRST];
    }
    int usedIds() const { return m_next - SIMID_FIRST; }

    // ----- send-id annotation (probe pattern) --------------------------------
    void tagCall(const char* text) {
        DWORD sid = 0;
        if (SimConnect_GetLastSentPacketID(m_h, &sid) != HR_OK)
            return;                     // API unavailable: annotation only
        if ((int)sid == m_lastSendId) return;
        m_lastSendId = (int)sid;
        for (int i = 0; i < ANNOT_MAX; ++i) {
            if (m_annotId[i] == (int)sid) return;   // already labelled
        }
        for (int i = 0; i < ANNOT_MAX; ++i) {
            if (m_annotId[i] < 0) {
                m_annotId[i] = (int)sid;
                snprintf(m_annotText[i], sizeof(m_annotText[i]), "%.40s", text);
                return;
            }
        }
    }
    const char* lookup(DWORD sid, char* out, unsigned nOut) const {
        for (int i = 0; i < ANNOT_MAX; ++i)
            if (m_annotId[i] == (int)sid) {
                snprintf(out, nOut, "%s", m_annotText[i]);
                return m_annotText[i];
            }
        return 0;
    }
    // Exception diagnostics: an unannotated send id sits BETWEEN annotated
    // neighbours (each real call sends exactly one packet, tags are added
    // after every send, and the ring keeps every id of the session). Naming
    // the bracketing calls pins the failing operation even when the ring
    // missed the exact packet (e.g. a helper send inside the tagged window).
    bool lookupNear(DWORD sid, const char** beforeText, int* beforeId,
                    const char** afterText, int* afterId) const {
        *beforeId = -1; *afterId = -1; *beforeText = 0; *afterText = 0;
        for (int i = 0; i < ANNOT_MAX; ++i) {
            int id = m_annotId[i];
            if (id < 0) continue;
            if (id < (int)sid && (*beforeId == -1 || id > *beforeId)) {
                *beforeId = id; *beforeText = m_annotText[i];
            }
            if (id > (int)sid && (*afterId == -1 || id < *afterId)) {
                *afterId = id; *afterText = m_annotText[i];
            }
        }
        return *beforeText != 0 || *afterText != 0;
    }

    // Diagnostic override for MapClientEventToSimEvent (0 = private token).
    const char* simEventName;

private:
    static const int PAIR_MAX   = GS_EVENT_BUDGET;
    // Every send gets a tag now (MapClientEvent, MapInput, group ops, notify…),
    // so the ring must cover a whole session of mappings: ~3 packets per
    // mapInput = 2x the id budget, plus slack for the init sets.
    static const int ANNOT_MAX  = GS_EVENT_BUDGET * 2 + 64;

    // "keys:<K>" -> bare "<K>" (D1). Bare strings pass through untouched.
    const char* policy(const char* input) {
        static char buf[56];
        buf[0] = 0;
        if (!input || !input[0]) { policyWarn(input); return 0; }
        if (strncmp(input, "keys:", 5) == 0) {
            if (!m_allowKeys) { policyWarn(input); return 0; }
            snprintf(buf, sizeof(buf), "%.40s", input + 5);
            if (!buf[0]) { policyWarn(input); return 0; }
            return buf;
        }
        return input;   // "joystick:d:button:b" / "K" / "VK_A+VK_B" grammar
    }
    void policyWarn(const char* input) {
        if (!m_policyWarned) {
            m_policyWarned = true;
            printf("GentleStop: learn policy off for keyboard strings\n"
                   "GentleStop:   ('%.20s')\n", input ? input : "");
        }
    }
    long addNotify(int simId) {
        long hr = (long)SimConnect_AddClientEventToNotificationGroup(
            m_h, NGROUP, (SIMCONNECT_CLIENT_EVENT_ID)simId, FALSE);
        m_lastHr = hr;
        tagCall("AddNotify");
        if (hr != HR_OK) return failLog("AddClientEventToNotificationGroup", hr);
        if (!m_notifyPrio) {
            m_notifyPrio = true;
            long hr2 = (long)SimConnect_SetNotificationGroupPriority(
                m_h, NGROUP, (DWORD)GROUP_PRIORITY_LONG());
            tagCall("SetNotificationGroupPriority");
            if (hr2 != HR_OK) return failLog("SetNotificationGroupPriority", hr2);
        }
        return HR_OK;
    }
    static long GROUP_PRIORITY_LONG() { return (long)GROUP_PRIORITY; }
    long failLog(const char* what, long hr) {
        ++m_failures;
        if (m_failures <= 4)
            printf("GentleStop: %s failed hr=0x%08lX\n", what, (unsigned long)hr);
        return hr;
    }

    HANDLE m_h;
    int    m_next;                    // next session-unique client event id
    int    m_simToDesign[GS_EVENT_BUDGET];
    bool   m_notifyPrio;
    int    m_failures;
    bool   m_budgetWarned;
    bool   m_policyWarned;
    bool   m_allowKeys;
    int    m_annotId[ANNOT_MAX];
    char   m_annotText[ANNOT_MAX][48];
    int    m_lastSendId;
    long   m_lastHr;                  // Stage 4.5: last SimConnect HRESULT for diagnostics

public:
    // Stage 4.5: expose last HRESULT for panel diagnostics.
    long lastHr() const { return m_lastHr; }
};

} // namespace gsim

#endif // GS_INPUT_EVENT_API_H
