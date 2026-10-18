# 🛬 GentleStop

Smooth, controllable braking for Microsoft Flight Simulator, without brake pedals.

GentleStop turns your stick or yoke into a progressive brake. Push the elevator forward while taxiing and the brakes build up gradually, ease off and they release. No more tapping a key or button and getting an abrupt stop.

## ✨ Features

- 🕹️ **Stick-forward braking:** your elevator input sets the brake pressure on both sides, like a brake pedal.
- 🔄 **Invert option:** the stick direction is inverted by default to match how MSFS reports the axis. You can flip it in the panel.
- ⏱️ **Optional pressure ramp:** let brake pressure rise smoothly over a time you choose (0.2 to 5 seconds).
- 📈 **Response curves:** linear, or exponential for a softer start.
- 🎚️ **Presets and tuning:** Taxi and Landing presets, plus adjustable stick deadzone.
- 🛡️ **Built-in safety:**
  - Never brakes while airborne.
  - Never brakes above a speed you set (default 60 kt), so normal touchdowns are unaffected.
  - Never brakes above a throttle level you set (default 50%), so takeoff rolls are unaffected.
  - Respects the parking brake.
  - Optional: no braking while the strobe lights are on (off by default).
- 📊 **Live feedback:** left and right brake bars and a status line (Armed, Braking, Airborne, and so on).
- 🧩 **Lives in the MSFS toolbar:** nothing to run outside the sim.

## 📦 Installation

1. Copy the `alexisware94-alexisware94-gentlestop-toolbar-control` folder into your MSFS `Community` folder.
2. Start MSFS and open the **GentleStop** panel from the toolbar.
3. On the ground, push your stick forward and watch the brake bars.

## ⚙️ Settings

Stick deadzone, ramp duration, response curve, cutoff speed, cutoff throttle, invert and the strobe lock are all adjustable in the panel.

## ❗ Notes

- This addon sends brake commands only while you are braking. When you let go, it releases the brakes once and leaves them alone.
- The elevator and yoke will still move when you push forward, as they normally do.
