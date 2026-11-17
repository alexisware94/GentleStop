// input_learn.h - GentleStop direct-input activation: learn/persist/toggle state machine.
//
// Design under investigation (NOT production code yet). It turns the user's chosen
// joystick button or keyboard key into exactly-one-toggle-per-press, using ONLY the
// down and up delivery that SimConnect input groups provide - no brake-pressure
// detector involved anywhere in this path.
//
// How it maps onto the MSFS 2024 SimConnect SDK (verified against SimConnect.h of the
// installed SDK and the online MSFS API reference, Oct 2026):
//
//   SimConnect_EnumerateControllers(h)
//       -> SIMCONNECT_RECV_ID_CONTROLLERS_LIST : SIMCONNECT_CONTROLLER_ITEM[]
//          { DeviceName[256], DeviceId, ProductId, CompositeID, HardwareVersion }
//   SimConnect_MapInputEventToClientEvent_EX1(h, GROUP, inputString,
//          downEventId, 0, upEventId, 0, /*bMaskable*/ FALSE)
//       input string  "joystick:<DeviceId>:button:<n>"  or  "keys:<KEY>"
//       EX1 exists because plain MapInputEventToClientEvent mishandles DeviceId
//       re-enumeration when a device is unplugged/replugged (documented bug).
//   SimConnect_SetInputGroupPriority(h, GROUP, SIMCONNECT_GROUP_PRIORITY_DEFAULT)
//       - documented as MANDATORY: without it the server sends no events at all.
//   SimConnect_SetInputGroupState(h, GROUP, SIMCONNECT_STATE_ON)
//       - documented default state of a group is INACTIVE; must be switched on.
//
// Compatibility with a user's existing MSFS assignments (from the priority rules:
// the sim engine itself is a SimConnect client at DEFAULT priority and never masks;
// a group at DEFAULT priority numerically above HIGHEST_MASKABLE cannot mask):
//   bMaskable = FALSE  => our event cannot be taken away by other clients.
//   priority  DEFAULT => we cannot steal the input from the sim or other clients;
//                        an input that already has a binding fires BOTH actions.
//   => our listener receives events non-suppressibly and suppresses nothing,
//      including nothing from the simulator's own binding engine.
//
// Learning "next supported input" without knowing the button up front:
//   A single-press fan - during Learn the client maps the whole candidate space in
//   one group, every candidate to its OWN distinct client event id (the group cap is
//   1000 events, the fan fits), and the first down event arriving identifies the
//   physical input directly through its event id. Axes/sliders/POV are deliberately
//   not in the fan: they cannot be captured and are reported unsupported. Keyboard
//   keys do not come from EnumerateControllers; they are fanned from a static
//   whitelist of the documented valid key strings.
//
// Held press = exactly one toggle: the down event toggles (when armed), the UP event
// is the sole re-arm evidence. Time is never re-arm evidence.
//
// The header is shared by tests/test_input_probe.cpp (mock Api, MSVC) and the wasm
// probe (real Api) - so it uses NO STL (the wasm toolchain links only the wasi C
// library). Strings travel in a fixed-size InputStr.
//
#ifndef GS_INPUT_LEARN_H
#define GS_INPUT_LEARN_H

#include <stdio.h>
#include <string.h>

namespace gslearn {

// ---------------------------------------------------------------------------
// Minimal fixed string (no STL: this header is shared with the wasm toolchain).
// ---------------------------------------------------------------------------
struct InputStr {
    char s[48];
    InputStr() : s{ 0 } {}
    void assign(const char* str) {
        size_t i = 0;
        if (str) for (; str[i] && i + 1 < sizeof(s); ++i) s[i] = str[i];
        s[i] = 0;
    }
    bool equals(const char* o) const { return strcmp(s, o ? o : "") == 0; }
    bool empty() const { return s[0] == 0; }
    const char* c_str() const { return s; }
};

// ---------------------------------------------------------------------------
// Host API the state machine drives. The wasm production binding implements this
// with real SimConnect calls; tests implement it with a recording mock.
// Return type is `long` = HRESULT as declared by SimConnect.h (S_OK==0).
// ---------------------------------------------------------------------------
struct IInputApi {
    virtual ~IInputApi() {}
    // Map one input string. upEventId < 0 means "no up event". Returns S_OK(0) or a failure.
    virtual long mapInput(const char* input, int downEvent, int upEvent) = 0;
    virtual long clearGroup()                                           = 0;
    virtual long setPriority(unsigned priority)                         = 0;
    virtual long setGroupOn()                                           = 0;
};

// S_OK as plain constants (no windows types in this header).
static const long HR_OK   = 0;
static const long HR_FAIL = 1;

// ---------------------------------------------------------------------------
// Candidates: the fan. Strings follow the documented input-string grammar.
// ---------------------------------------------------------------------------
struct ControllerInfo {
    unsigned    deviceId;
    InputStr    name;
};

// Buttons are embedded device-specific; joystick buttons start at 0 (documented).
static const int GS_LEARN_BTN_MAX = 16;     // probe fan: buttons 0..15 per device
static const int GS_PROD_BTN_MAX  = 128;    // production fan bound by the group cap

// Keyboard fan: first slice of the documented "Valid Input Strings".
static const char* const GS_KEY_FAN[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
};
static const int GS_KEY_FAN_N = (int)(sizeof(GS_KEY_FAN) / sizeof(GS_KEY_FAN[0]));

// ---------------------------------------------------------------------------
// Event-id layout (client-defined; layout is part of the design)
// ---------------------------------------------------------------------------
// One input group for everything; ids are laid out so a sense check is trivial.
static const unsigned GROUP_PRIORITY = 2000000000u; // = SIMCONNECT_GROUP_PRIORITY_DEFAULT
static const int      EVT_NONE       = -1;
static const int      EVT_ACT_DOWN   = 500;       // production down (single input)
static const int      EVT_ACT_UP     = 501;       // production up   (single input)
static const int      EVT_FAN_BASE   = 1000;      // learn fan ids start here

static bool isFanEvent(int eventId) { return eventId >= EVT_FAN_BASE; }

// --- string builders ---------------------------------------------------------
static void makeJoystickString(unsigned deviceId, int button, char* out, unsigned n) {
    snprintf(out, n, "joystick:%u:button:%d", deviceId, button);
}
static void makeKeyString(const char* key, char* out, unsigned n) {
    snprintf(out, n, "keys:%s", key);
}

// ---------------------------------------------------------------------------
// The learner + activation state machine.
// ---------------------------------------------------------------------------

enum LearnPhase {
    LEARN_OFF = 0,    // not learning (a binding may still be active)
    LEARN_FAN_ARMED,  // fan mapped, first down event captures
    LEARN_CAPTURED,   // input captured, waiting for confirm / timeout
    LEARN_PROD,       // production mapping (down+up) active
};

struct LearnResult {
    bool     captured = false;  // a supported input was captured
    bool     toggled = false;   // a production-press toggle fired this event
    bool     anomaly = false;   // un-armed down (duplicate/anomaly down) ignored
    InputStr input;             // e.g. "joystick:11:button:7" or "keys:P"
    InputStr deviceName;        // empty for keyboard
    bool     timedOut = false;
};

class ActivationInput {
public:
    explicit ActivationInput(IInputApi* api) : m_api(api) {}

    // --- binding restore (module init or settings import) --------------------
    bool bind(const char* input) {
        if (!input || !input[0]) return false;
        if (m_api->clearGroup() != HR_OK) return false;
        if (m_api->mapInput(input, EVT_ACT_DOWN, EVT_ACT_UP) != HR_OK) return false;
        m_api->setPriority(GROUP_PRIORITY);
        m_api->setGroupOn();
        m_phase = LEARN_PROD;
        m_binding.assign(input);
        return true;
    }

    // --- learn lifecycle ------------------------------------------------------
    // Maps the fan. Only enter while a flight is loaded: input events are
    // documented to be delivered only with a flight loaded. absTime starts the
    // capture-timewait window (the dispatch loop calls tick() each frame).
    bool beginLearn(const ControllerInfo* controllers, int count, double absTime,
                    double timeoutSec = 20.0) {
        if (m_phase == LEARN_FAN_ARMED || m_phase == LEARN_CAPTURED) cancel();
        if (m_phase == LEARN_PROD) { m_api->clearGroup(); m_phase = LEARN_OFF; }
        for (int i = 0; i < count && i < GS_MAX_DEV; ++i) m_devices[(size_t)i] = controllers[i];
        m_nDevices = count < GS_MAX_DEV ? count : GS_MAX_DEV;
        if (m_nDevices == 0) return false;
        int fan = EVT_FAN_BASE;
        bool anyMapped = false;
        char buf[64];
        for (int d = 0; d < m_nDevices; ++d)
            for (int b = 0; b < GS_LEARN_BTN_MAX; ++b, ++fan) {
                makeJoystickString(m_devices[(size_t)d].deviceId, b, buf, sizeof(buf));
                if (m_api->mapInput(buf, fan, EVT_NONE) == HR_OK) anyMapped = true;
            }
        for (int k = 0; k < GS_KEY_FAN_N; ++k, ++fan) {
            makeKeyString(GS_KEY_FAN[k], buf, sizeof(buf));
            if (m_api->mapInput(buf, fan, EVT_NONE) == HR_OK) anyMapped = true;
        }
        if (!anyMapped) return false;
        m_api->setPriority(GROUP_PRIORITY);
        m_api->setGroupOn();
        m_phase = LEARN_FAN_ARMED;
        m_deadline = absTime + timeoutSec;   // capture window (tick() enforces)
        resetResult();
        return true;
    }

    void cancel() { cancelInternal(true); }

    void tick(double absTime) {
        if (m_phase == LEARN_FAN_ARMED && m_deadline >= 0.0 && absTime > m_deadline) {
            m_result.timedOut = true;
            cancelInternal(true);   // restore the previous binding, if there was one
        }
    }

    // Called from the dispatch loop for every SIMCONNECT_RECV_ID_EVENT that
    // belongs to our group. The returned LearnResult carries capture/toggle/anomaly.
    LearnResult onInputEvent(int eventId, double absTime) {
        if (m_phase == LEARN_FAN_ARMED && isFanEvent(eventId)) return capture(eventId, absTime);
        if (m_phase == LEARN_PROD) {
            if (eventId == EVT_ACT_DOWN) return prodDown(absTime);
            if (eventId == EVT_ACT_UP)   return prodUp();
        }
        return LearnResult();
    }

    // User confirmed the capture (panel click in production; auto in the probe).
    // Rebinds to the production single input (down+up), returns the string to persist.
    bool confirm(char* persisted, unsigned n, double absTime) {
        (void)absTime;
        if (m_phase != LEARN_CAPTURED) return false;
        if (!m_result.captured) return false;
        if (persisted) snprintf(persisted, n, "%s", m_result.input.c_str());
        char input[64];
        snprintf(input, sizeof(input), "%s", m_result.input.c_str());
        cancelInternal(false);   // clears the fan, sets LEARN_OFF
        if (!bind(input)) return false;
        m_phase = LEARN_PROD;
        return true;
    }

    const LearnResult& result() const { return m_result; }
    LearnPhase phase() const { return m_phase; }
    const char* binding() const { return m_binding.c_str(); }
    bool armed() const { return m_armed; }

private:
    // H2.5: the learn fan device cap was GentleStop-imposed (GS_MAX_DEV = 8),
    // not an SDK maximum. Raised to 64 to match the bounded controller snapshot
    // (GS_CTL_LIST_MAX) so more than 8 joysticks can be learn candidates.
    static const int GS_MAX_DEV = 64;

    void resetResult() {
        m_result = LearnResult();
    }

    void cancelInternal(bool restoreOldBinding) {
        // Always clear the binding so a fresh sim connection (or module restart)
        // re-establishes the input mapping via bind(). Without this, bind() would
        // skip remapping because m_binding still matches, leaving the sim side
        // without the mapping after a session reset.
        char old[64];
        snprintf(old, sizeof(old), "%s", m_binding.c_str());
        m_binding.s[0] = 0;
        m_phase = LEARN_OFF;
        if (m_api) m_api->clearGroup();
        resetResult();
        m_deadline = -1.0;
        if (restoreOldBinding && old[0]) bind(old);   // keep activation working
    }

    LearnResult capture(int eventId, double absTime) {
        int i = eventId - EVT_FAN_BASE;
        int buttonsTotal = m_nDevices * GS_LEARN_BTN_MAX;
        LearnResult r;
        if (i < 0) return r;
        if (i < buttonsTotal) {
            int d = i / GS_LEARN_BTN_MAX;
            int b = i % GS_LEARN_BTN_MAX;
            char buf[64];
            makeJoystickString(m_devices[(size_t)d].deviceId, b, buf, sizeof(buf));
            r.input.assign(buf);
            r.deviceName = m_devices[(size_t)d].name;
        } else if (i - buttonsTotal < GS_KEY_FAN_N) {
            char buf[64];
            makeKeyString(GS_KEY_FAN[i - buttonsTotal], buf, sizeof(buf));
            r.input.assign(buf);
            // keyboard: no device name
        } else {
            return r;   // out of fan range: not ours, ignore
        }
        r.captured = true;
        m_result = r;
        m_capturedAt = absTime;
        m_deadline = -1.0;     // capture window closed
        m_phase = LEARN_CAPTURED;  // fan stays live until confirm; a later press
        return r;              // first-wins overwrites (checked in tests)
    }

    LearnResult prodDown(double absTime) {
        (void)absTime;
        LearnResult r;
        if (m_armed) {
            m_armed = false;
            m_openDown = true;
            r.toggled = true;
        } else {
            r.anomaly = true;   // second down without an up: never re-toggle
        }
        return r;
    }

    LearnResult prodUp() {
        LearnResult r;
        m_armed = true;      // the release event is the only re-arm evidence
        m_openDown = false;  // (time alone never re-arms)
        return r;
    }

    IInputApi*   m_api;
    ControllerInfo m_devices[GS_MAX_DEV];
    int          m_nDevices   = 0;
    LearnPhase   m_phase      = LEARN_OFF;
    double       m_deadline   = -1.0;
    double       m_capturedAt = -1.0;
    bool         m_armed      = true;   // press detector starts armed
    bool         m_openDown   = false;
    LearnResult  m_result;
    InputStr     m_binding;
};

} // namespace gslearn

#endif // GS_INPUT_LEARN_H
