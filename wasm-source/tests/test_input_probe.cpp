// test_input_probe.cpp - offline validation of the GentleStop direct-input
// activation design (input_learn.h) against a mock of the SimConnect input
// services. This is the diagnostic prototype required before any production
// wiring: it proves the call sequence, the learn lifecycle and the held-press
// semantics the wasm module would implement. It does NOT prove in-sim delivery
// - that is what the separate wasm probe build is for (NOT part of production,
// NOT deployed here).
//
// Mock behavior mirrors the documented SimConnect server rules:
//   * an input group with no priority set or state OFF delivers NO events
//     (documented: "a priority setting must be made for all input groups,
//      otherwise event notifications will not be sent"; default group state
//      is inactive);
//   * MapInputEventToClientEvent fails on strings outside the documented
//     grammar ("joystick:n:button:i", "keys:<KEY>"); axis/slider/POV/mouse
//     names are NOT part of the learn fan and can never be captured;
//   * the sim itself is a client at DEFAULT priority that never masks: at
//     DEFAULT priority this client receives events without ever taking the
//     input away from another client (no suppression; a duplicate fire for an
//     input that also has a sim binding is the compatible behavior).

#include "input_learn.h"

#include <map>
#include <string>
#include <vector>
#include <stdio.h>

using namespace gslearn;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond) do { \
    ++g_checks; if (!(cond)) { ++g_fails; printf("FAIL line %d: %s\n", __LINE__, #cond); } \
} while (0)

// --- mock SimConnect input services ------------------------------------------
class MockApi : public IInputApi {
public:
    std::vector<std::string> mapLog, clearLog;
    std::map<std::string, std::pair<int, int>> mapping;   // input -> (down, up)
    unsigned priority = 0;
    bool groupOn = false;

    // Documented grammar check, shared by every mock map call.
    static bool validJoystick(const std::string& s, unsigned* dev, int* btn) {
        if (s.compare(0, 9, "joystick:") != 0) return false;
        unsigned d = 0; int b = -1;
        if (sscanf_s(s.c_str(), "joystick:%u:button:%d", &d, &b) != 2) return false;
        if (b < 0 || b >= GS_PROD_BTN_MAX) return false;
        if (dev) *dev = d;
        if (btn) *btn = b;
        return true;
    }
    static bool validKey(const std::string& s) {
        for (int k = 0; k < GS_KEY_FAN_N; ++k) {
            char buf[48];
            makeKeyString(GS_KEY_FAN[k], buf, sizeof(buf));
            if (s == buf) return true;
        }
        return false;
    }

    long mapInput(const char* input, int downEvent, int upEvent) override {
        std::string s = input;
        unsigned dev; int btn;
        bool ok = (validJoystick(s, &dev, &btn) || validKey(s))
                  && downEvent != EVT_NONE
                  && (int)mapping.size() < 1000;   // documented per-group cap
        mapLog.push_back(s + (ok ? "" : " [REJECTED]"));
        if (!ok) return HR_FAIL;
        mapping[s] = std::make_pair(downEvent, upEvent < 0 ? EVT_NONE : upEvent);
        return HR_OK;
    }
    long clearGroup() override { clearLog.push_back("clear"); mapping.clear(); return HR_OK; }
    long setPriority(unsigned p) override { priority = p; return HR_OK; }
    long setGroupOn() override { groupOn = true; return HR_OK; }

    // The server only delivers when priority is set AND the group is switched on.
    bool delivers() const { return priority != 0 && groupOn; }

    int findDown(const std::string& input) const {
        std::map<std::string, std::pair<int, int>>::const_iterator it = mapping.find(input);
        return it == mapping.end() ? EVT_NONE : it->second.first;
    }
    int findUp(const std::string& input) const {
        std::map<std::string, std::pair<int, int>>::const_iterator it = mapping.find(input);
        if (it == mapping.end() || it->second.second == EVT_NONE) return EVT_NONE;
        return it->second.second;
    }
};

static ControllerInfo* twoControllers(int* count) {
    static ControllerInfo v[2];
    v[0].deviceId = 3;
    v[0].name.assign("Logitech Extreme 3D");
    v[1].deviceId = 11;
    v[1].name.assign("Test Yoke");
    *count = 2;
    return v;
}

int main() {
    int nCtl = 0;
    twoControllers(&nCtl);
    char joyB7[64], joyB2[64], keyP[64], joyB2b[64];
    makeJoystickString(11u, 7, joyB7, sizeof(joyB7));    // the capture target
    makeJoystickString(3u, 2, joyB2, sizeof(joyB2));
    makeKeyString("P", keyP, sizeof(keyP));
    makeJoystickString(3u, 2, joyB2b, sizeof(joyB2b));   // same as joyB2

    // ---- 1. binding restore (module init path) ------------------------------
    {
        MockApi api;
        ActivationInput act(&api);
        CHECK(act.bind(joyB2));
        CHECK(act.phase() == LEARN_PROD);
        CHECK(api.priority == GROUP_PRIORITY && api.groupOn);   // mandatory setup done
        // a press toggles immediately: no relearn, no pressure detector involved
        LearnResult r = act.onInputEvent(api.findDown(joyB2), 1.0);
        CHECK(r.toggled && !r.anomaly && !act.armed());
        r = act.onInputEvent(api.findUp(joyB2), 2.0);
        CHECK(!r.toggled && act.armed());                       // up re-arms
        // restore refuses empty and malformed strings
        ActivationInput act2(&api);
        CHECK(!act2.bind(""));
        CHECK(!act2.bind("joystick:3"));                        // grammar violation
        CHECK(!act2.bind("joystick:3:button:999"));             // out of the button range
        CHECK(!act2.bind("joystick:3:XAxis"));                  // axes are not supported
        CHECK(!act2.bind("mouse:none"));                        // mouse not supported
        CHECK(!act2.bind("keys:F15"));                          // not in the key fan
    }
    // ---- 2. restore from persisted string (config-file simulation) ---------
    {
        printf("simulating persistence: activationInput=%s\n", joyB2);
        MockApi api;
        ActivationInput act(&api);
        CHECK(api.mapInput(joyB2, EVT_ACT_DOWN, EVT_ACT_UP) == HR_OK);
        CHECK(act.bind(joyB2));
        CHECK(api.findDown(joyB2) == EVT_ACT_DOWN && api.findUp(joyB2) == EVT_ACT_UP);
        LearnResult r = act.onInputEvent(EVT_ACT_DOWN, 3.0);
        CHECK(r.toggled);                                       // works after "restart"
        CHECK(act.onInputEvent(EVT_ACT_UP, 4.0).toggled == false);
        CHECK(act.armed());
    }
    // ---- 3. learn lifecycle: fan, single-press capture, confirm -------------
    {
        MockApi api;
        ActivationInput act(&api);
        CHECK(act.beginLearn(twoControllers(&nCtl), nCtl, 0.0));
        CHECK(act.phase() == LEARN_FAN_ARMED);
        CHECK(api.mapLog.size() == (size_t)(2 * GS_LEARN_BTN_MAX + GS_KEY_FAN_N) && api.delivers());
        // fan: every candidate mapped to a DISTINCT id; buttons have no up events
        for (int d = 0; d < 2; ++d) {
            unsigned dev = (d == 0) ? 3u : 11u;
            for (int b = 0; b < GS_LEARN_BTN_MAX; ++b) {
                char s[64];
                makeJoystickString(dev, b, s, sizeof(s));
                CHECK(api.findDown(s) == EVT_FAN_BASE + d * GS_LEARN_BTN_MAX + b);
                CHECK(api.findUp(s) == EVT_NONE);
            }
        }
        for (int k = 0; k < GS_KEY_FAN_N; ++k) {
            char s[48];
            makeKeyString(GS_KEY_FAN[k], s, sizeof(s));
            CHECK(api.findDown(s) == EVT_FAN_BASE + 2 * GS_LEARN_BTN_MAX + k);
        }
        // the user presses 'Test Yoke' button 7 exactly once
        LearnResult r = act.onInputEvent(api.findDown(joyB7), 10.0);
        CHECK(r.captured && r.input.equals(joyB7) && r.deviceName.equals("Test Yoke"));
        CHECK(act.phase() == LEARN_CAPTURED);
        // a later press cannot steal the capture (first-wins)
        LearnResult r2 = act.onInputEvent(api.findDown(joyB2), 11.0);
        CHECK(!r2.captured && r2.input.empty() && act.result().input.equals(joyB7));
        // confirm: fan cleared, production down+up map replaces it, string returned
        char persisted[64];
        CHECK(act.confirm(persisted, sizeof(persisted), 12.0));
        CHECK(strcmp(persisted, joyB7) == 0);
        CHECK(act.phase() == LEARN_PROD && api.mapping.size() == 1);
        CHECK(api.clearLog.size() >= 1);                        // fan removed
        CHECK(api.findUp(joyB7) == EVT_ACT_UP);                 // down+up for toggle
        // captured input is immediately usable
        CHECK(act.onInputEvent(api.findDown(joyB7), 13.0).toggled);
        CHECK(act.onInputEvent(api.findUp(joyB7), 14.0).toggled == false);
        // confirm is one-shot: a second confirm fails, state unchanged
        char again[64];
        CHECK(!act.confirm(again, sizeof(again), 15.0));
        CHECK(act.phase() == LEARN_PROD && again[0] == 0);
    }
    // ---- 4. learn captures a KEYBOARD key the same way ----------------------
    {
        MockApi api;
        ActivationInput act(&api);
        CHECK(act.beginLearn(twoControllers(&nCtl), nCtl, 0.0));
        LearnResult r = act.onInputEvent(api.findDown(keyP), 10.0);
        CHECK(r.captured && r.input.equals(keyP) && r.deviceName.empty());  // no device name
        char persisted[64];
        CHECK(act.confirm(persisted, sizeof(persisted), 12.0));
        CHECK(strcmp(persisted, keyP) == 0);
        CHECK(act.onInputEvent(api.findDown(keyP), 13.0).toggled);
        CHECK(act.onInputEvent(api.findUp(keyP), 14.0).toggled == false);
    }
    // ---- 5. capture timeout --------------------------------------------------
    {
        MockApi api;
        ActivationInput act(&api);
        CHECK(act.beginLearn(twoControllers(&nCtl), nCtl, 0.0));
        // No press arrives; caller must drive timeouts (the dispatch loop does).
        act.onInputEvent(EVT_FAN_BASE - 1, 1.0);   // out-of-range id ignored
        CHECK(act.phase() == LEARN_FAN_ARMED);
        act.tick(9.0);                             // before deadline: still armed
        CHECK(act.phase() == LEARN_FAN_ARMED);
        CHECK(!act.result().timedOut && act.result().input.empty());
        act.tick(25.0);                            // past the 20 s window: timed out
        CHECK(act.phase() == LEARN_OFF && api.mapping.empty());  // fan cleared
        unsigned dev; int btn;
        CHECK(MockApi::validJoystick(joyB7, &dev, &btn) && btn == 7 && dev == 11u);
    }
    // ---- 5b. timeout restores the previous binding --------------------------
    {
        MockApi api;
        ActivationInput act(&api);
        CHECK(act.bind(joyB2));
        CHECK(act.beginLearn(twoControllers(&nCtl), nCtl, 0.0));   // binding dropped
        CHECK(api.findUp(joyB2) == EVT_NONE);                      // no up+down pair: fan only
        act.tick(25.0);                                            // timeout
        CHECK(act.phase() == LEARN_PROD);                          // old binding restored
        CHECK(strcmp(act.binding(), joyB2b) == 0);
        CHECK(api.mapping.size() == 1);
        CHECK(api.findUp(joyB2b) == EVT_ACT_UP);                   // down+up ready again
    }
    // ---- 6. unsupported inputs can never be captured ------------------------
    {
        MockApi api;
        // the fan only contains button and key strings: axes/sliders/POV and
        // mouse inputs are rejected at map time, so no event id exists for them
        CHECK(api.mapInput("joystick:3:XAxis",  8000, -1) == HR_FAIL);
        CHECK(api.mapInput("joystick:3:Slider", 8000, -1) == HR_FAIL);
        CHECK(api.mapInput("joystick:3:POV",    8000, -1) == HR_FAIL);
        CHECK(api.mapInput("mouse:none",        8000, -1) == HR_FAIL);
        CHECK(api.mapInput("joystick:3:button", 8000, -1) == HR_FAIL);
        CHECK(api.mapping.empty());
        // a key outside the documented whitelist is rejected too
        CHECK(api.mapInput("keys:F13", 8000, -1) == HR_FAIL);
    }
    // ---- 7. held press: exactly one toggle; up is the only re-arm -----------
    {
        MockApi api;
        ActivationInput act(&api);
        std::string in = joyB2;
        CHECK(api.mapInput(in.c_str(), EVT_ACT_DOWN, EVT_ACT_UP) == HR_OK);
        CHECK(act.bind(joyB2));
        // the user holds the button: one down ...
        unsigned dev; int btn;
        CHECK(MockApi::validJoystick(in, &dev, &btn) && btn == 2);
        LearnResult r = act.onInputEvent(EVT_ACT_DOWN, 10.0);
        CHECK(r.toggled && !r.anomaly && !act.armed());
        // ... simconnect sends NO additional down during the hold; if a duplicate
        // down somehow arrives (replay/other client), it must NOT re-toggle
        for (double t = 11.0; t < 20.0; t += 1.0) {
            r = act.onInputEvent(EVT_ACT_DOWN, t);       // hostile duplicates
            CHECK(r.anomaly && !r.toggled);
        }
        CHECK(strcmp(act.binding(), in.c_str()) == 0);
        // release is the re-arm evidence - not a timer
        CHECK(act.onInputEvent(EVT_ACT_UP, 25.0).toggled == false);
        CHECK(act.armed());
        // a distinct second tap (down+up) toggles again
        CHECK(act.onInputEvent(EVT_ACT_DOWN, 30.0).toggled);
        CHECK(act.onInputEvent(EVT_ACT_UP, 31.0).toggled == false && act.armed());
        CHECK(!act.result().captured && !act.result().timedOut);
    }
    // ---- 8. events that are not ours must be ignored ------------------------
    {
        MockApi api;
        ActivationInput act(&api);
        CHECK(act.bind(joyB2));
        LearnResult r = act.onInputEvent(1, 1.0);       // 1 = some other event
        CHECK(!r.toggled && act.armed());
        r = act.onInputEvent(EVT_ACT_UP, 2.0);          // stray up: harmless
        CHECK(!r.toggled && act.armed());
    }
    // ---- 9. delivery gate: no priority / not switched on = no events --------
    {
        MockApi api;                    // beginLearn performs the mandatory setup
        ActivationInput act(&api);
        CHECK(act.beginLearn(twoControllers(&nCtl), nCtl, 0.0));
        CHECK(api.priority == GROUP_PRIORITY && api.groupOn && api.delivers());
        // simulate a client that forgot the priority call
        MockApi dead;
        dead.groupOn = true;
        CHECK(!dead.delivers());
    }

    printf("checks=%d fails=%d\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
