// yaxis_probe.h -- STAGING-ONLY DIAGNOSTIC (compiled only with -DGS_YAXIS_PROBE,
// which neither the production build script nor any pinned suite defines).
//
// Purpose (authorized diagnostic): determine, on the real simulator, whether
// the physical joystick Y axis can be received continuously through
// SimConnect_MapInputEventToClientEvent_EX1, and with WHICH input-definition
// string, while the user's Elevator Axis binding stays intact.
//
// Runtime evidence driving this revision (28 Oct 2026 run):
//   - all four 'joystick:n:YAxis' mappings returned S_OK locally, then each
//     was rejected asynchronous-y by the engine as
//     SIMCONNECT_EXCEPTION_ERROR (=1) with dwIndex = 0xFFFFFFFF (the
//     documented "no parameter/unknown" special case value, per the
//     SIMCONNECT_RECV_EXCEPTION docs + the devsupport documentation-bug
//     report; it does NOT identify a bad argument);
//   - two exception packets per mapping send -> tracked cumulatively below
//     (per-send-id counters) so duplicate delivery vs separate failures can
//     be distinguished by the NEXT run;
//   - the probe's send-id annotation ring was silently inactive in that run
//     (ring entries init to 0 by static storage, never set to -1, so
//     tagCall's "< 0" slot search never found a slot): FIXED here, and each
//     setup call's actual GetLastSentPacketID value is now logged right
//     after the call (the officially supported correlation method).
//
// Candidate input strings (explicitly labelled; evidence-based, no
// speculative sweeps):
//   family A "yaxis-controls" (retry control): 'joystick:n:YAxis'
//     -- the previous run's string, retried to confirm the failure pattern
//     is reproducible and to verify the per-send exception count.
//   family B "msfs2024-channel": 'joystick:n:Y'
//     -- justified by SDK-era documentation: the Input Configuration
//     (SDK-era Input Configuration XML page) identifies the physical axis
//     channels inside the sim's own input system as X/Y/Z/rX/rY/rZ, and the
//     Input Profile Editor presents them as "Joystick L AXIS Y" style names;
//     the accepted 'joystick:n:button:i' evidence shows the 'joystick:n:NAME'
//     grammar itself is intact. The exact axis NAME for the Y channel is
//     therefore a measured candidate, not an asserted one.
//
// STRICT receive-only policy (unchanged):
//   - NO elevator events, NO TransmitClientEvent, NO brake writes,
//     NO braking-source change, NO settings/persistence of any kind.
//   - The probe holds its OWN SimConnect client (name "GentleStopYProbe") so
//     its mappings are completely separate from the main client, from the
//     learn capture (GS_USE_LEARN) and from the elevator-suppression
//     feature (GS_USE_ELEV_SUPPRESS). It never touches their state.
//   - Per-mapping setup failures are LOGGED (with input string, event id and
//     the actual send id) and tolerated; only a failed Open or CallDispatch
//     makes the probe report unavailable. The module is unaffected either way.
//   - Shutdown clears the whole input group before closing: the mapping
//     disappears with the client session, nothing persists.
//
// Measurement: first arrival per candidate, event counts, min/max/last raw
// value, cumulative exception counts per candidate (by send id), and a
// periodic summary. Documented range (MSFS SDK): Y axis +32K pulled back ->
// -32K pushed forward; the run compares reality against that, and against
// this project's observed inverted response. NOTHING is inferred from the
// separately-running elevator client beyond a same-instant cross-check.
#pragma once

#ifdef GS_YAXIS_PROBE

#include <stdio.h>
#include <string.h>

namespace gs_yprobe {

const DWORD GPROBE     = 4801;              // dedicated input group id
const DWORD EVT_FIRST  = 4802;              // client event id = EVT_FIRST + candidate k
const int   N_CAND     = 8;                 // 2 families x 4 candidate device indices
const double SUMMARY_S = 10.0;              // periodic stats interval

static HANDLE hProbe = 0;
static bool   m_ok = false;                 // client open, mappings sent, dispatch running
static bool   m_setupFailed = false;
static double m_lastSum = -1.0e9;

struct Cand { bool have; unsigned events; int last; int min; int max;
              unsigned exceptions; DWORD sid; char label[28]; char input[32]; };
static Cand  m_cand[N_CAND];
static DWORD m_evt[N_CAND];

static const char* FamilyLabel(int k) {
    // k 0..3: family A (yaxis-controls retry), k 4..7: family B (msfs2024-channel)
    return k < 4 ? "yaxis-controls" : "msfs2024-channel";
}

static bool   mapped() { return m_ok; }
static int    candCount() { return N_CAND; }
static bool   have(int k) { return (k >= 0 && k < N_CAND) ? m_cand[k].have : false; }
static int    last(int k) { return (k >= 0 && k < N_CAND) ? m_cand[k].last : 0; }
static int    minimum(int k) { return (k >= 0 && k < N_CAND) ? m_cand[k].min : 0; }
static int    maximum(int k) { return (k >= 0 && k < N_CAND) ? m_cand[k].max : 0; }
static unsigned events(int k) { return (k >= 0 && k < N_CAND) ? m_cand[k].events : 0u; }
static unsigned exceptions(int k) { return (k >= 0 && k < N_CAND) ? m_cand[k].exceptions : 0u; }

// One-slot exception packet counter + last-exception info for logging.
static void CALLBACK probeDispatch(SIMCONNECT_RECV* pData, DWORD cbData, void* pContext) {
    (void)cbData; (void)pContext;
    switch (pData->dwID) {
    case SIMCONNECT_RECV_ID_EVENT: {
        if (!m_ok) return;                        // after shutdown: mappings are gone
        const SIMCONNECT_RECV_EVENT* ev = (const SIMCONNECT_RECV_EVENT*)pData;
        for (int k = 0; k < N_CAND; ++k) {
            if (ev->uEventID != m_evt[k]) continue;
            int v = (int32_t)ev->dwData;
            Cand& c = m_cand[k];
            if (!c.have) { c.min = v; c.max = v; }
            else { if (v < c.min) c.min = v; if (v > c.max) c.max = v; }
            c.have = true;
            c.last = v;
            ++c.events;
            if (c.events == 1)
                printf("GentleStop: probe: [%s] '%s' (event %u, send %u) axis values arriving (first %d)\n",
                       c.label, c.input, (unsigned)ev->uEventID, c.sid, v);
            break;
        }
        break;
    }
    case SIMCONNECT_RECV_ID_EXCEPTION: {
        const SIMCONNECT_RECV_EXCEPTION* e = (const SIMCONNECT_RECV_EXCEPTION*)pData;
        int found = -1;
        for (int k = 0; k < N_CAND; ++k)
            if (m_cand[k].sid == (DWORD)e->dwSendID) { found = k; break; }
        if (found >= 0) {
            ++m_cand[found].exceptions;
            printf("GentleStop: probe: exception %u (send %u, index %u) [%s] '%s' event %u  count=%u\n",
                   (unsigned)e->dwException, (unsigned)e->dwSendID, (unsigned)e->dwIndex,
                   m_cand[found].label, m_cand[found].input,
                   (unsigned)m_evt[found], m_cand[found].exceptions);
        } else {
            printf("GentleStop: probe: exception %u (send %u, index %u) [unattributed: no candidate owns this send id]\n",
                   (unsigned)e->dwException, (unsigned)e->dwSendID, (unsigned)e->dwIndex);
        }
        break;
    }
    default:
        break;
    }
}

static bool init() {
    if (m_setupFailed) return false;   // never re-run after a failure in-session
    for (int k = 0; k < N_CAND; ++k) {
        m_evt[k] = EVT_FIRST + (DWORD)k;
        m_cand[k] = Cand();
        m_cand[k].label[0] = 0;
    }
    m_lastSum = -1.0e9;
    HRESULT hr = SimConnect_Open(&hProbe, "GentleStopYProbe", 0, 0, (HANDLE)0, 0);
    printf("GentleStop: probe: SimConnect_Open -> 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) { hProbe = 0; m_setupFailed = true; return false; }

    int failedMaps = 0;
    for (int k = 0; k < N_CAND; ++k) {
        char input[32];
        snprintf(input, sizeof(input), "joystick:%d:%s", k % 4,
                 k < 4 ? "YAxis" : "Y");
        snprintf(m_cand[k].label, sizeof(m_cand[k].label), "%.27s", FamilyLabel(k));
        snprintf(m_cand[k].input, sizeof(m_cand[k].input), "%.31s", input);
        hr = SimConnect_MapInputEventToClientEvent_EX1(hProbe, GPROBE, input, m_evt[k], 0,
                                                       SIMCONNECT_UNUSED, 0, FALSE);
        // Actual send id, logged right after the call: the officially
        // supported exception-correlation method (SIMCONNECT_RECV_EXCEPTION
        // Remarks).
        DWORD sid = 0;
        const char* hrTag = FAILED(hr) ? "  <-- FAILED" : "";
        if (SimConnect_GetLastSentPacketID(hProbe, &sid) == S_OK)
            printf("GentleStop: probe: send %u: [%s]: '%s' -> 0x%08X%s\n",
                   (unsigned)sid, m_cand[k].label, input, (unsigned)hr, hrTag);
        else
            printf("GentleStop: probe: send ?: [%s]: '%s' -> 0x%08X%s\n",
                   m_cand[k].label, input, (unsigned)hr, hrTag);
        m_cand[k].sid = sid;
        if (FAILED(hr)) ++failedMaps;
    }

    hr = SimConnect_SetInputGroupPriority(hProbe, GPROBE, SIMCONNECT_GROUP_PRIORITY_DEFAULT);
    printf("GentleStop: probe: SetInputGroupPriority DEFAULT -> 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) { m_setupFailed = true; return false; }

    hr = SimConnect_SetInputGroupState(hProbe, GPROBE, SIMCONNECT_STATE_ON);
    printf("GentleStop: probe: SetInputGroupState ON -> 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) { m_setupFailed = true; return false; }

    hr = SimConnect_CallDispatch(hProbe, probeDispatch, nullptr);
    printf("GentleStop: probe: CallDispatch -> 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) { m_setupFailed = true; return false; }

    m_ok = true;
    printf("GentleStop: probe: ready (%d mappings, receive-only)\n", N_CAND);
    return m_ok;
}

// Periodic summary, driven by the module's per-frame tick (no new hold loop).
static void update(double absTime) {
    if (!m_ok || absTime - m_lastSum < SUMMARY_S) return;
    m_lastSum = absTime;
    for (int k = 0; k < N_CAND; ++k) {
        if (m_cand[k].have)
            printf("GentleStop: probe: [%s] '%s': %u values  min %d  max %d  last %d  exceptions=%u\n",
                   m_cand[k].label, m_cand[k].input, m_cand[k].events,
                   m_cand[k].min, m_cand[k].max, m_cand[k].last,
                   m_cand[k].exceptions);
        else if (m_cand[k].exceptions)
            printf("GentleStop: probe: [%s] '%s': NO values  exceptions=%u\n",
                   m_cand[k].label, m_cand[k].input, m_cand[k].exceptions);
    }
}

static void shutdown() {
    if (hProbe) {
        if (m_ok)
            SimConnect_ClearInputGroup(hProbe, GPROBE);   // mappings die with the session
        SimConnect_Close(hProbe);
        hProbe = 0;
    }
    m_ok = false;
    m_setupFailed = false;    // a fresh module lifecycle retries the probe
    for (int k = 0; k < N_CAND; ++k) m_cand[k] = Cand();
    m_lastSum = -1.0e9;
}

} // namespace gs_yprobe

#endif // GS_YAXIS_PROBE
