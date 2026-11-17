// elevator_suppress.h -- STAGING-ONLY feature (compiled only with
// -DGS_USE_ELEV_SUPPRESS, which the production build script never defines).
//
// Ground braking without unwanted nose-down pitch:
//   GentleStop's braking is driven by FORWARD stick/yoke input. The module
//   reads it as the YOKE Y POSITION simvar; it does NOT and cannot stop the
//   simulator from also applying that same physical axis to the elevator --
//   so pushing forward for extra braking pitches the nose down while armed
//   on the ground.
//
//   Officially supported interception mechanism (MSFS 2024 SimConnect docs,
//   "SimConnect Priorities"): "It is possible to mask Microsoft Flight
//   Simulator 2024 events, and therefore intercept them before they reach
//   the simulation engine, and perhaps send new events to the simulation
//   engine after appropriate processing has been done." A SimConnect client
//   that opens its own connection, joins a notification group at priority
//   SIMCONNECT_GROUP_PRIORITY_HIGHEST_MASKABLE and adds the event with
//   bMaskable=TRUE receives AND consumes AXIS_ELEVATOR_SET; the sim engine
//   (SIMCONNECT_GROUP_PRIORITY_DEFAULT ordinary client) then no longer gets
//   it. This client-local mask disappears automatically when the client
//   disconnects: module exit / flight unload / failure all restore the
//   stock routing, so the failsafe direction is "pitch back under normal
//   control", never a stuck elevator.
//
// Policy (defined BEFORE coding, per the approved Phase B conditions):
//   Suppress (consume the elevator axis, no pitch command) exactly while
//     enabled && activation allowed && on ground && data fresh && !learning
//     && the mask client is healthy.
//   The suppression is INDEPENDENT of the brake gate: it does not follow
//   brake pressure or the gate hysteresis, so guessing "is it braking?" can
//   never mis-route pitch. While airborne, disarmed, learning, or when the
//   ground-state data goes stale, the elevator is handed back with ONE
//   retransmit of the latest physical axis value (no jump, no spring of
//   competing writes: the mask stops consuming and ordinary MSFS routing
//   resumes).
//   Lost events while suppressed are the FEATURE: the axis is consumed.
//   On exit the one retransmit puts the elevator exactly where the stick is.
//
// Braking input: while the axis is consumed by the mask, YOKE Y POSITION may
// stop tracking the physical stick. The module DOES receive every axis value
// here, so the braking input is fed from the received axis (axis -> yoke
// range conversion) while suppression is active. Nothing writes elevator or
// brake values from here.
#pragma once

#ifdef GS_USE_ELEV_SUPPRESS

namespace gs_elev {

const DWORD GID_EL = 4701;   // notification-group id space of this client
const DWORD EVT_EL = 4702;   // client event id: AXIS_ELEVATOR_SET alias

HANDLE hElev = 0;
bool   m_ok = false;         // mask client registered and healthy
bool   m_active = false;     // pitch currently consumed by this module
bool   m_haveAxis = false;   // a physical axis value has been received
int    m_lastAxis = 0;       // latest physical axis value (-16383..16384, fwd = neg)
unsigned m_axisEvents = 0;
unsigned m_transitions = 0;
bool   m_setupFailed = false;

bool active() { return m_active; }
bool haveAxis() { return m_haveAxis; }
int  lastAxis() { return m_lastAxis; }

// AXIS_* 事件 values are normalised to -1..1 as documented (forward = negative).
double axisToYoke(int axis) {
    double y = axis / 16383.5;
    if (y < -1.0) y = -1.0; else if (y > 1.0) y = 1.0;
    return y;
}

void CALLBACK elevDispatch(SIMCONNECT_RECV* pData, DWORD cbData, void* pContext) {
    (void)cbData; (void)pContext;
    switch (pData->dwID) {
    case SIMCONNECT_RECV_ID_EVENT: {
        const SIMCONNECT_RECV_EVENT* ev = (const SIMCONNECT_RECV_EVENT*)pData;
        if (ev->uEventID == EVT_EL) {
            m_haveAxis = true;
            m_lastAxis = (int32_t)ev->dwData;
            if (++m_axisEvents == 1)
                printf("GentleStop: elev: axis events arriving (first value %d)\n", m_lastAxis);
        }
        break;
    }
    case SIMCONNECT_RECV_ID_EXCEPTION: {
        const SIMCONNECT_RECV_EXCEPTION* e = (const SIMCONNECT_RECV_EXCEPTION*)pData;
        printf("GentleStop: elev: SimConnect exception %u (send %u, index %u) - ignored, feature never suppresses on failure\n",
               (unsigned)e->dwException, (unsigned)e->dwSendID, (unsigned)e->dwIndex);
        break;
    }
    default:
        break;
    }
}

// One re-transmit hands pitch back the way the stick is right now.
void transmitAxis(int axis) {
    if (!m_ok) return;
    SimConnect_TransmitClientEvent(hElev, SIMCONNECT_OBJECT_ID_USER, EVT_EL, (DWORD)(int32_t)axis,
                                   SIMCONNECT_GROUP_PRIORITY_HIGHEST_MASKABLE,
                                   SIMCONNECT_EVENT_FLAG_GROUPID_IS_PRIORITY);
}

bool init() {
    if (m_setupFailed) return false;   // never re-run after a failure
    HRESULT hr = SimConnect_Open(&hElev, "GentleStopElev", (HWND)0, 0, (HANDLE)0, 0);
    printf("GentleStop: elev: SimConnect_Open -> 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) { hElev = 0; m_setupFailed = true; return false; }
    hr = SimConnect_MapClientEventToSimEvent(hElev, EVT_EL, "AXIS_ELEVATOR_SET");
    printf("GentleStop: elev: MapClientEventToSimEvent AXIS_ELEVATOR_SET -> 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) { m_setupFailed = true; return false; }
    hr = SimConnect_SetNotificationGroupPriority(hElev, GID_EL, SIMCONNECT_GROUP_PRIORITY_HIGHEST_MASKABLE);
    printf("GentleStop: elev: SetNotificationGroupPriority HIGHEST_MASKABLE -> 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) { m_setupFailed = true; return false; }
    hr = SimConnect_AddClientEventToNotificationGroup(hElev, GID_EL, EVT_EL, TRUE);
    printf("GentleStop: elev: AddClientEventToNotificationGroup maskable -> 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) { m_setupFailed = true; return false; }
    hr = SimConnect_CallDispatch(hElev, elevDispatch, nullptr);
    printf("GentleStop: elev: CallDispatch -> 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) { m_setupFailed = true; return false; }
    m_ok = true;
    if (m_ok) printf("GentleStop: elev: mask client ready (documented maskable priority)\n");
    return m_ok;
}

// Per-frame policy. eligible = the exact Phase B conditions; dataFresh as
// computed by the module (first data frame also granted once).
void update(bool eligible, double absTime) {
    (void)absTime;
    if (!m_ok) return;
    if (eligible && m_active) return;             // engaged, keep consuming
    if (!eligible && !m_active) return;           // normal control, no action
    m_transitions++;
    if (eligible) {
        // ENGAGE: one neutral write, then consume every axis event.
        m_active = true;
        transmitAxis(0);
        printf("GentleStop: elevator suppressed (#%u, was %d, stick -> braking)\n",
               m_transitions, m_haveAxis ? m_lastAxis : 0);
    } else {
        // RELEASE: forward the latest known physical axis, then stop.
        m_active = false;
        transmitAxis(m_haveAxis ? m_lastAxis : 0);
        printf("GentleStop: elevator released (#%u, axis %s%d)\n", m_transitions,
               m_haveAxis ? "" : "UNKNOWN->0 ", m_lastAxis);
    }
}

void shutdown() {
    if (m_active) {           // release on every exit path
        m_active = false;
        transmitAxis(m_haveAxis ? m_lastAxis : 0);
        printf("GentleStop: elevator released on shutdown\n");
    }
    if (hElev) { SimConnect_Close(hElev); hElev = 0; }
    m_ok = false;
    // a fresh module lifecycle (= DLL reload in the sim) always retries the
    // feature; the sticky flag only guards retries within one lifecycle.
    m_setupFailed = false;
    m_haveAxis = false;
    m_lastAxis = 0;
}

} // namespace gs_elev

#endif // GS_USE_ELEV_SUPPRESS
