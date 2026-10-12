# GentleStop 🛬

**GentleStop** is a lightweight, zero-stutter native toolbar panel for **Microsoft Flight Simulator (MSFS 2020 / MSFS 2024)** designed to eliminate abrupt, hard landing rollouts and jerky taxi braking—especially for pilot setups without dedicated physical toe-brake pedals.

It unifies **Pitch-to-Brake modulation** (using forward elevator stick/yoke movement) and **Digital Button Time-Ramping** (gradually scaling key/button pressure over time) into a single, seamless in-sim HTML/JS interface.

---

## ✨ Key Features

* 🕹️ **Pitch-to-Brake Mode:** Map forward elevator pitch to progressive left/right toe-brake axis pressure while on the ground.
* ⏱️ **Digital Button Ramping:** Smoothly ramp digital trigger or keyboard inputs from 0% to 100% over configurable curves.
* 🛡️ **Airborne Safety Lockout:** Automatically disarms braking when landing gear is off the ground (`IS GEAR TOUCHING GROUND == False`) to prevent unwanted brake application on touchdown.
* 🎛️ **In-Sim Toolbar Control:** Clean, native dark-themed HTML/JS overlay—no need to launch external `.exe` files or alt-tab mid-flight.
* ⚡ **Zero Frame Overhead:** Highly optimized SimConnect execution with minimal CPU/RAM footprint.

---

## 🛠️ Tech Stack & Architecture

* **Frontend:** MSFS Native HTML/CSS/JavaScript (Coherent GT)
* **Backend:** WASM / C++ Gauge & SimConnect Event Interception
* **Compatibility:** MSFS 2020 & MSFS 2024 Community Folder Standard

---

## 👨‍💻 Developer & Community

Created by **Alexisware**.

* **License:** MIT License
* **Support / Feedback:** [Flightsim.to Profile](https://flightsim.to) | Open an Issue on GitHub
