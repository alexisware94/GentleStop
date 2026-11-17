# Building the GentleStop WASM module

**Not compiled against the real SDK yet.** I had no MSFS SDK available, so this source has only been
checked on a normal PC compiler against stand-in headers (see the host regression-test section below).
Expect to possibly fix a small API detail on the first real build. Send me the compiler errors and I'll
fix them.

## What you need
- MSFS 2024 SDK (enable Developer Mode in the sim to download it)
- Visual Studio 2022 with the "Desktop development with C++" workload (the SDK installs its VS extension)

## Host regression tests (no MSFS SDK required)

The retained regression tests run on a normal Windows PC against the MSFS/SimConnect stand-in
headers in `tests/stubs/`. They exercise the braking core, the module glue against simulated
SimConnect / Communication Bus / L-variable services, the learned activation input and the two
optional probe headers. **They do not build the production WASM module and do not need the MSFS
SDK.** Building the production module remains the separate workflow described under "Build".

### Prerequisites
- Windows PowerShell 5.1 or PowerShell 7+.
- MSVC `cl.exe` in the environment: Visual Studio 2022 or newer with the "Desktop development
  with C++" workload.
- Node.js on `PATH` for the JavaScript tests.

`cl.exe` is normally not on the default `PATH`. Open an **x64 Native Tools Command Prompt for VS**
(or a **Developer PowerShell for Visual Studio**) and run the commands from there, or call
`vcvars64.bat` from a Visual Studio installation in a console before starting PowerShell. You can
also pass the full path explicitly with `-CompilerPath`. The script never hardcodes a Visual Studio
installation path.

### Run all regression tests
From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_tests.ps1
```

Optional parameters:

| Parameter | Meaning |
|---|---|
| `-BuildDirectory <path>` | Use a specific build directory (must be outside the repository). |
| `-CompilerPath <cl.exe>` | Use a specific MSVC compiler executable. |
| `-TestTimeoutSeconds <n>` | Per-process timeout in seconds (default `300`). |
| `-KeepBuildArtifacts` | Keep the generated binaries, object files and logs for debugging. |

Compiled test executables, object files, generated configuration headers and logs are written to a
unique directory under the system temporary folder (for example
`%TEMP%\gentlestop-tests-<timestamp>-<pid>`). The runner never writes them into `wasm-source/`,
`wasm-source/tests/`, `tools/`, the panel package or the installed Community package. The directory
is removed at the end unless `-KeepBuildArtifacts` is given, and its path is always printed.

### What the runner builds and executes

Each C++ executable gets exactly the sources and defines it needs; the runner does not apply one
common set of definitions to every test. The include path order is `tests/stubs`, `tests`,
`wasm-source`; the tests that compile `GentleStop.cpp` also receive a generated force-include header
that redirects `GS_CFG_PATH` into the temporary build directory, so no test touches
`\work\gentlestop.cfg`.

| Executable | Sources | Definitions | Covers |
|---|---|---|---|
| `test_core` | `test_core.cpp` | none | Core stepping harness; stdin-driven, used by the parity test |
| `test_core_assert` | `test_core_assert.cpp` | none | Deterministic core braking assertions |
| `test_brake_input` | `test_brake_input.cpp` | none | Braking input range and telemetry guards |
| `test_input_probe` | `test_input_probe.cpp` | none | Input-learn state machine against a mock API |
| `test_glue` | `test_glue.cpp`, `GentleStop.cpp` | `GS_USE_LEARN`, `GS_TEST_CFG` | Module glue against SimConnect/CommBus mocks |
| `test_input_learn_glue` | `test_input_learn_glue.cpp`, `GentleStop.cpp` | `GS_USE_LEARN` | Production learned-input integration |
| `test_elevator_suppress` | `test_elevator_suppress.cpp`, `GentleStop.cpp` | `GS_USE_LEARN`, `GS_USE_ELEV_SUPPRESS` | Elevator suppression policy |
| `test_yaxis_probe` | `test_yaxis_probe.cpp`, `GentleStop.cpp` | `GS_USE_LEARN`, `GS_USE_ELEV_SUPPRESS`, `GS_YAXIS_PROBE` | Y-axis diagnostic probe |

`test_core` is not a self-checking assertion suite. It reads one settings line and then input rows
from standard input and prints one line per step; the runner smoke-runs it with one deterministic
step, and `tests/run_parity.js` drives it for the actual comparison.

The two JavaScript tests are then executed with the discovered `node` executable:

- `wasm-source/tests/run_parity.js` - randomized comparison of the C++ core against a reference
  JavaScript braking engine. The runner passes the canonical panel source as the first argument and
  the freshly built `test_core` executable as the second argument. You can also run it manually:
  `node wasm-source/tests/run_parity.js [panel-js] [test-core-executable]`; the executable path may
  instead be supplied through the `GS_TEST_CORE_EXE` environment variable.
- `wasm-source/tests/test_panel.js` - panel smoke test over the canonical
  `gentlestop-stage-panel/` package, covering the legacy, legacy-unbound and 2024 routes. It
  resolves every file from its own location and needs no private `staging/` directory.

Both scripts work from any current directory and exit non-zero on failure.

### Results and exit codes

The runner prints a per-test status (`PASS`, `FAIL` or `NOT RUN`), the individual exit codes, a
per-area summary and an overall result. It continues through the whole matrix so every available
result is reported, then:

| Exit code | Meaning |
|---|---|
| `0` | Every required C++ and JavaScript test passed. |
| `1` | At least one test was executed and failed. |
| `2` | At least one required test could not run because a prerequisite was missing; the overall result is `PARTIAL`, never `PASS`. |

A missing `cl.exe` or `node.exe` is reported explicitly and never counted as a pass.

### Known limitation of the parity harness

`run_parity.js` was written to compare the C++ core with the reference braking engine that used to
live in the panel JavaScript. The canonical public panel package
(`gentlestop-stage-panel/html_ui/InGamePanels/GentleStopToolbarControl/GentleStop.js`) does not
contain that reference engine, so with the default arguments the harness reports that the reference
markers are missing and exits `1`; `run_tests.ps1` reports this as `FAIL` rather than hiding it.
Passing a panel source that does contain the reference engine as the first argument restores the
original comparison. Keeping or retiring this harness is an open owner decision.

### Host tests versus the production WASM

`tools/run_tests.ps1` is the regression-test entry point only. `tools/build_wasm.ps1` is the
production module build: it needs the MSFS 2024 SDK, the SDK's clang toolchain and a real link step,
and it is not invoked by the regression runner. A passing host regression run does not rebuild,
replace or validate the packaged `gentlestop.wasm`.

## Build
1. In Visual Studio create a new project from the SDK's WASM module template
   (search "MSFS" in "Create a new project"), or start from a standalone-module sample in the SDK.
   Choose the MSFS 2024 platform/configuration if the template offers one.
2. Replace the generated source with `GentleStop.cpp` and add `gentlestop_core.h` next to it.
3. Build in Release. Name the output `gentlestop.wasm`.
4. Put it in the add-on at `alexisware94-gentlestop/modules/gentlestop.wasm`.
5. Run `python tools/update_layout.py` (MSFS only loads files listed in layout.json).
6. Copy the whole `alexisware94-gentlestop` folder into your Community folder, replacing the old one.
7. Start the sim. Modules are loaded when the sim starts / a flight loads.

## Checking it works
Open the panel: the bottom line should say `Module: running`. If it says `not loaded`, the sim did not
load the .wasm (check the modules folder + layout.json). In Developer Mode the console prints
"GentleStop module ready" at startup. Then close the panel and taxi: brakes should keep following the stick.

## How it fits together
- `gentlestop_core.h` the braking logic, a line-by-line port of the original JS (tested: identical output
  to the original JS over 180,000 randomized steps, see `tests/run_parity.js`).
- `GentleStop.cpp` SimConnect glue: reads yoke/on-ground/speed/strobe/throttle/parking every sim frame
  and sends AXIS_LEFT/RIGHT_BRAKE_SET, exactly like the JS did.
- Settings: panel -> L:GS_* variables -> module. The module also saves them to `\work\gentlestop.cfg`,
  so your last profile is active after a restart even if you never open the panel.
- Safety: the module releases the brakes when it unloads and never touches the brakes while idle.

## Update (v0.3): building with the SDK's standalone clang, as the first real build did
The first real build linked no C library, so printf/fopen/exp/... stayed as unresolved imports and the sim
cannot load such a module. Fixed in v0.3:
- `gentlestop_core.h` no longer needs C++ std headers (uses math.h).
- `GentleStop.cpp` avoids the stack protector (`__stack_chk_fail`).
- `tools/build_wasm.ps1` compiles AND links the wasi libc statically, then runs
  `tools/check_wasm_imports.py`, which must say "Looks loadable" before you start the sim.
Run:  powershell -ExecutionPolicy Bypass -File tools\build_wasm.ps1
(`-Sdk "C:\MSFS 2024 SDK"` if your SDK is elsewhere.) If it reports "conflicting declaration" for the
L-variable functions, remove `-DGS_DECLARE_L_API` from the script.

## Findings (29 Oct 2026): braking input path, pitch side effect, `position` units

Documentation only -- this section changes no code, build, or package. Findings come from
the post-observation architecture review, backed by in-sim observation runs with the
standalone probe packages (`alexisware94-gs-yaxis-probe`, `alexisware94-gs-elevobs-probe`;
both receive-only and separate from gentlestop.wasm).

1. **Braking input source.** The module reads the `YOKE Y POSITION` SimVar, not the raw
   `joystick:<DeviceId>:YAxis` event: added first in the data definition
   (`GentleStop.cpp:1091`), parsed into `SimData.yokeY` (`GentleStop.cpp:63-65`), consumed
   every sim frame in `OnFrame` (`GentleStop.cpp:869`, then `g_ctl.step` at `:876`). The
   sim has already normalized it to -1..1, forward = negative
   (`gentlestop_core.h:43,67-68`).
2. **Control path.** The module writes brake events only: `AXIS_LEFT_BRAKE_SET` /
   `AXIS_RIGHT_BRAKE_SET` via `SimConnect_TransmitClientEvent` while braking plus one
   release frame (`GentleStop.cpp:893-898` policy, `:727-734` transmit). It never writes
   elevator or pitch in the active path. Guards: airborne/parking/strobe/throttle/speed
   (`gentlestop_core.h:88-97`); gate + hysteresis `:99-104`; curve + smoothing `:111-131`;
   brake-axis conversion `:77`.
3. **Pitch side effect -- strongly supported explanation, not an experimentally isolated
   result.** While armed on the ground, pulling the same physical stick back as the
   braking gesture can also raise the nose. The supported explanation: the stick remains
   bound to the simulator's own elevator control, so the sim's native input path pitches
   the aircraft independently of anything GentleStop reads or writes -- the module's
   SimVar read is a passive copy and its only writes are brake events.
4. **`ELEVATOR POSITION`.** In the recorded observation test (29 Oct 2026) it tracked
   physical stick movement in both directions. Switching GentleStop's input source to it
   has **not** been shown to prevent pitch changes -- it would only change how the same
   input is observed. `AXIS_ELEVATOR_SET` was not observed during the test (it is
   documented as an input-only setter, so its silence is expected).
5. **`position` units: validate the scale empirically.** The SDK documents
   `ELEVATOR POSITION` as units `Position (-16K to 0)` (-16K = full down), yet the probe
   observed values in the range **-1.0 .. +1.0**. The unit name does not pin the scale:
   `normalizedBrake()` (`GentleStop.cpp:624-630`) already rescales
   `BRAKE LEFT/RIGHT POSITION` by /16383 when above 1.5, because some aircraft report the
   raw axis range under the same `position` units. No universal scale is asserted here:
   calibrate any `position`-unit SimVar against observed values.
6. **Separate brake binding (user configuration, not a GentleStop feature).** Binding the
   brakes to a different control -- a button, lever, or a second axis -- in the
   simulator's controls UI may avoid the stick/elevator conflict entirely. That is a
   user-configuration alternative, not a verified GentleStop feature.

### Input-suppression research (29 Oct 2026, documentation only)

Results of a read-only review of the installed MSFS 2024 SDK headers and the official
SimConnect / Input Events documentation. This note records findings only; nothing was
built, run, or changed:

7. **No documented mechanism was found in the examined sources** that reliably consumes a
   continuous physical elevator axis before the simulator applies it to elevator control.
   Examined: the complete installed `SimConnect.h` function inventory (`SimConnect.h:1020-1136`),
   the official SimConnect Priorities and masking pages, the MSFS 2024 Input Events API
   (`SimConnect_GetInputEvent` / `SetInputEvent` / `SubscribeInputEvent` are documented as
   read / inject / observe only), and the WASM event headers (`MSFS_Events.h`,
   `Legacy/gauges.h`). This is a statement about the examined sources, not proof that no
   such mechanism can exist.
8. **Masking documentation ambiguity.** The official priority table marks
   `SIMCONNECT_GROUP_PRIORITY_HIGHEST_MASKABLE` as "No longer used in Microsoft Flight
   Simulator 2020" while the adjacent paragraph still defines masking via that threshold;
   the installed header (`SimConnect.h:36-40`) carries no such note. Separately: event
   *reception* (a client notification copy), event *injection* (`TransmitClientEvent` /
   `SetInputEvent`), and *suppression* of the sim's native continuous-axis processing are
   three different things; only the first two are documented, and they must not be
   treated as equivalent.
9. **The elevator-suppression experiment did not demonstrate successful suppression.**
   `elevator_suppress.h` (staging-only; its define is never set, so it stays compiled out
   of the installed build) implemented the documented maskable-notification pattern on
   `AXIS_ELEVATOR_SET`. In the recorded in-sim test it produced no elevator effect and
   zero delivered events, and normal elevator control remained stock
   (`BASELINE_LEARNONLY_20261028.md` section 6).
10. **ModelBehaviors input inhibition is aircraft content, not a runtime API.** The
    documented `<InputEvent>` / `<Binding>` "react and inhibit any Event IDs" mechanism
    lives in aircraft package XML (ModelBehaviors) -- per-aircraft content work, not an
    established runtime capability for this generic add-on.
11. **Recommended pitch-free user configuration:** bind a separate control (button, lever,
    or spare axis) to the brake controls in the sim's Controls Options, leaving the normal
    elevator axis dedicated to flight control. Simulator-native, documented, and
    aircraft-independent at the input level.
12. **The learned activation button only arms/disarms GentleStop** (it toggles the braking
    gate); it does not itself provide the progressive braking input, which still comes
    from the forward-stick gesture (`gentlestop_core.h:88-104` gate, `:111-131` braking).

### Native modifier / axis-binding investigation (29 Oct 2026, documentation only)

Read-only review of the official Microsoft Flight Simulator 2024 controller configuration
documentation (Controller Settings FAQ, article 16459737949980; Peripheral Control Settings
Overview and FAQ, article 360016018600; SimConnect MapInputEventToClientEvent_EX1 page).
This note records findings only; nothing was built, run, or changed:

13. **Static assignment model.** The documented controller configuration assigns each physical
    input (button, key, or axis) to one or more commands permanently. Mapping the same Y-axis
    to both "Elevator axis" and a brake-axis command produces a conflict badge and
    **simultaneous activation** — per the official FAQ: *"if the conflicting assignments are
    in the same context, for example, aileron & elevator commands, both will be active at the
    same time. This can lead to weird or unwanted behavior in the sim."* This is not
    conditional switching.

14. **No conditional-axis mechanism found.** No documented modifier button, alternate
    binding layer, or runtime profile-switch mechanism for conditionally routing a continuous
    axis was found in the examined official documentation. The SimConnect
    MapInputEventToClientEvent_EX1 grammar allows modifier combinations (VK_LSHIFT etc.)
    **only for keyboard definitions**; joystick definitions are single inputs of the form
    `joystick:n:input[:i]` with no modifier syntax. Profile switching exists only as a manual
    UI action (pause menu / toolbar), not as a key event or API trigger.

15. **Cross-device key combinations: not yet supported.** The official Peripheral Control
    Settings FAQ states "Can I map cross devices key combination (e.g. mouse + keyboard
    shortcuts)? — Not yet." This is a statement about the documented UI capability; it does
    not generalize into proof that every possible UI workaround is impossible.

16. **The learned activation button arms/disarms GentleStop only.** It toggles the braking
    gate; it does not redirect the signal used as the progressive-braking input, which is
    fixed at the SimConnect data-definition level (`YOKE Y POSITION`,
    `GentleStop.cpp:1091`).

17. **Supported pitch-free configuration remains a separate physical axis or lever** assigned
    to the brake controls in the sim's Controls Options, while the elevator axis stays
    dedicated to flight control. Simulator-native, documented, aircraft-independent at the
    input level.

18. **Conclusion:** same-stick conditional switching is **unsupported in the documented UI**,
    not "technically impossible." The evidence reflects the examined official sources only.
