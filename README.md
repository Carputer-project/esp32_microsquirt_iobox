# ESP32 MicroSquirt I/O box

DIY clone of the official MicroSquirt "I/O-box" — an **ESP32 + SN65HVD230** that
listens to the MS2/Extra MicroSquirt CAN realtime broadcast (base ID `0x5F0`,
500 kbps) and drives **fan**, **idle control (IAC)** and **6 generic low-side
outputs**, with the same fail-safe the real I/O-box uses (outputs off when CAN
goes silent).

Because MS2/Extra has no I/O-box slave protocol (that's an MS3-only feature), this
box is an **autonomous "smart slave"**: it computes its own output logic from the
broadcast data. It also implements the full **Megasquirt 29-bit expansion
protocol** as a responder (so an MS2/Extra master *can* drive its outputs/ADC
directly), and broadcasts a small **`0x710`** frame for the in-dash HMI.

Works with **any MS2/Extra 3.4.x MicroSquirt V3** — no fuel/spark tune changes
needed, only CAN broadcast settings.

![status](https://img.shields.io/badge/status-in%20development-yellow)

## Features

- **Fan** — CLT hysteresis (on ≥ 200 °F / off ≤ 190 °F default, adjustable) or
  manual override, driven through a ULN2003A channel (default O6/GPIO23).
- **IAC** — 30 Hz PWM on GPIO32 → IRLZ44N → 3-wire rotary ISCV. Three modes:
  **Auto** (CLT warm-up table + RPM trim), **Follow** (mirror the ECU's own
  `iacstep` → fully TunerStudio-tunable closed-loop idle), or **Manual**.
- **O1–O6** — generic low-side outputs (ULN2003A): manual, CLT-temp or RPM
  trigger. O1 defaults to a 7000 rpm shift light.
- **Inputs drive outputs** — A1–A4 analog (0-5 V, 0.15 V hysteresis) can force any
  output or the fan (I/O-box parity).
- **29-bit CAN responder** — MS2/Extra remote-port protocol: MSG_REQ/MSG_CMD on
  table 7; remote idle PWM, remote on/off outputs, remote boost, ADC readback.
- **`0x710` dash broadcast** — A1–A4 threshold latches for
  the in-dash HMI (indicator/high-beam/brake icons). 10 Hz, always on.
- **GC9A01A round gauge** — 1.28" 240×240 idle-control display: big IAC duty %,
  RPM / target RPM / CLT / idle mode, fan + CAN status line.
- **Fail-safe** — all outputs off after 500 ms without CAN (matches the real
  I/O-box); IAC falls back to the ~1000-1200 rpm valve default.
- **Persistent config** — all settings stored in NVS; loadable/settable over
  serial or via the included Windows diagnostic app.

## Repo layout

```
esp32_microsquirt_iobox/
├── firmware/            PlatformIO ESP32 firmware — iobox2 (round display)
├── firmware-v3/         PlatformIO ESP32 firmware — iobox3 (configurable pins, display opt-in)
├── pcb/                 KiCad 8 PCB design (iobox2: schematic, board, project)
├── diag/                iobox_diag.py — Windows test/configure app (Python + Tkinter)
└── docs/
    ├── OPERATION.md     How the box works + how to use it (modes, commands, display)
    ├── PROTOCOL.md      CAN protocol reference (0x5F0 broadcast, 0x710, 29-bit responder)
    ├── BUILD.md         Full point-to-point build/wiring guide
    └── PARTS.md         Parts list with rough prices
```

## Two firmware variants

| | `firmware/` (iobox2) | `firmware-v3/` (iobox3) |
|---|---|---|
| Board | ESP32 DevKit + breakout, GC9A01A round display | Bare ESP32-WROOM-32 module board, 7" dash on CAN is the HMI |
| IAC pin | 32 (compile-time) | **runtime-configurable** (`P` command / diag app Pins tab) |
| Outputs | O1-O6 = 25,26,27,14,13,23 (compile-time) | O1-O7 = 13,12,14,27,26,25,33 default, **runtime-configurable** |
| Round display | yes | opt-in (`P TFT 1`; GPIO19 conflicts with IAC) |
| Switches | none (removed) | none |
| Use `P`? | no (fixed pins) | yes |

Both share the same core (CAN broadcast decode, fan, IAC, outputs, engine
profile warnings, 29-bit responder, `0x710` dash broadcast). iobox2 = one build
for that board; iobox3 = one firmware, pin map set once per board via the
diag app.

## Quick start

1. **Build hardware** — see `docs/BUILD.md` (wiring) and `docs/PARTS.md` (parts).
   The KiCad design in `pcb/` is the iobox2 board; the point-to-point
   build guide is an alternative for module wiring without a PCB. iobox3 is the
   bare-module 3×3 board — see `firmware-v3/README.md`.
2. **Flash firmware** (pick the folder matching your board):

   ```sh
   cd firmware        # iobox2  — or:  cd firmware-v3  for iobox3
   pio run                 # build
   pio run --target upload # flash over USB
   pio device monitor -b 115200
   ```

   Requires PlatformIO (ESP32 Arduino framework). At the serial prompt type `?`
   for a live status dump. On iobox3, type `P` to see the pin map (set it once
   from the diag app's Pins tab if your board is wired differently).
3. **Configure the MicroSquirt (one time)** in TunerStudio → CAN Realtime Data
   Broadcasting: **On**, base ID **1520**, rate **20 Hz**, enable groups
   **00** (rpm), **02** (baro/map/mat/clt), **03** (tps/batt/afr), and optionally
   **06** (iacstep) for Follow mode. Details in `docs/OPERATION.md`.
4. **Use the Windows app** (`diag/iobox_diag.py`) to set fan thresholds, IAC mode,
   output triggers, input mappings, the 29-bit responder and (iobox3) the pin map
   from a GUI.

## Hardware snapshot

| GPIO | Function | Drives |
|------|----------|--------|
| 5 / 4 | CAN TX / RX | SN65HVD230 (MicroSquirt pins 2/3, CANH/CANL) |
| 32 | IAC PWM (30 Hz) | ISCV via IRLZ44N |
| 25,26,27,14,13,23 | O1–O6 | ULN2003A (O6 = fan by default, `Y<k>` to change) |
| 2 | Status LED | onboard (CAN heartbeat / fail-safe blink) |
| 36,39,34,35 | A1–A4 analog 0-5 V | 10k/20k divider (or PCB 12 V→3.3 V dividers) |
| 15,21,22,19 | GC9A01A round display | SCLK / MOSI / CS / DC |

All outputs are **low-side (ground-switching)**, like the real MS I/O-box. Never
feed 12 V into the ESP32 or SN65HVD230 — a 12 V→5 V buck is mandatory.

## Serial command cheat sheet

| Command | Effect |
|---------|--------|
| `?` | status dump |
| `F205` / `F1` / `F0` | fan auto @205 °F / manual on / off |
| `E190` | fan off hysteresis 190 °F |
| `I35` / `I` / `I F` | IAC manual 35% / auto / follow-ECU |
| `T900` | idle target 900 rpm (auto mode) |
| `S7000` | shift light @7000 rpm |
| `O3 R3500` / `O4 1` | O3 above 3500 rpm / O4 on |
| `A1 O2 3.5` / `A2 F 2.0` | analog threshold → output/fan |
| `Y6` | fan on ULN channel 6 |
| `R1` / `R0` / `RB5` | 29-bit responder on/off / box ID |
| `W1` / `W0` | engine profile monitoring on/off |
| `W idle 650 1000` | idle RPM band (warning below/above, <20% TPS) |
| `W clt 230` / `W mat 160` | max coolant / intake °F |
| `W batt 11.0 16.0` / `W map 280` | battery band / max MAP kPa |
| `W afr 10.0 16.5` | AFR window (below=RICH, above=LEAN) |
| `W hold 3000` / `W warnout 3` | warn latch ms / blink O3 as check-engine lamp |
| `P IAC 19` / `P O3 27` | **iobox3 only** — set IAC / output pin |
| `P RESET` | iobox3 only — restore default pin map |
| `S` | full command list |

## CAN at a glance

- **ECU → box**: 11-bit IDs `0x5F0`–`0x5F8` (group N at ID 1520+N), big-endian,
  8 bytes/group, 20 Hz. rpm@6, baro/map/mat/clt@16-22 (×0.1), tps/batt/afr@24-28
  (×0.1), iacstep@54 (×0.392 = duty %).
- **Box → dash**: `0x710`, byte0 = A1–A4 latches, byte1 = reserved, byte2 = counter.
- **Master → box**: 29-bit extended `MSG_REQ`/`MSG_CMD` (MS2/Extra remote ports).

Full byte maps and the 29-bit responder spec: `docs/PROTOCOL.md`.

## Project context

Built for a 7A-GTE (7A block + 4A-GE head) MicroSquirt V3 car: the I/O box frees
the ECU's FIDLE (moving boost control off it), adds proper PWM idle via the ISCV,
handles the radiator fan, and feeds warning-lamp states (indicators, high beam,
brake) to a 7" ESP32-S3 in-dash HMI. This box is one of several open-source
modules in the [Carputer-project](https://github.com/Carputer-project)
ecosystem.

## License

Firmware, PCB and tooling in this repo are released for personal/DIY use.
See each file header for notes.
