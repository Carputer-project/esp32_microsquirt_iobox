# Operation & how to use

This document describes how the I/O box behaves and how to configure and drive
it. For wiring and parts see `BUILD.md` / `PARTS.md`; for the raw CAN byte maps
see `PROTOCOL.md`.

## 1. What the box does

The box sits on the MicroSquirt CAN bus and:

1. **Reads** the MS2/Extra realtime broadcast (`0x5F0`+groups) for rpm, MAP,
   MAT, CLT, TPS, battery, AFR and (optionally) the ECU's own idle step.
2. **Computes** its own output states: fan from CLT hysteresis, IAC from a
   warm-up table + RPM trim (or mirrors the ECU's `iacstep`), O1–O6 from
   manual/temp/rpm triggers, plus input-forced overrides.
3. **Acts** through low-side drivers (ULN2003A for fan + O1–O6, IRLZ44N for the
   ISCV).
4. **Reports** on the round GC9A01A display and via a serial console.

MS2/Extra has no I/O-box slave protocol (MS3-only), so the box is an autonomous
"smart slave" — its logic lives in firmware/config, not in TunerStudio. It also
implements the 29-bit Megasquirt expansion protocol as a responder so the ECU
*master* can drive it directly when configured for remote ports (see §7).

## 2. Power-up / boot

- On boot the box draws the gauge frame, loads its NVS config, zeroes all
  outputs (fail-safe start) and starts CAN.
- Serial prints `MS2/Extra I/O box ready. Type ? for status.` at 115200 baud.
- Until the first CAN frame arrives the box holds all outputs off and IAC at
  `iacFailDuty` (default 0).

## 3. CAN data the box needs

TunerStudio → **CAN Realtime Data Broadcasting** (MicroSquirt):

| Setting | Value |
|---------|-------|
| Enable realtime broadcasting over CAN | On (`can_outpc_gp00_master`) |
| Base message identifier | 1520 |
| Broadcasting rate | 20 Hz |
| Groups | **00** (rpm), **02** (baro/map/mat/clt), **03** (tps/batt/afr) |
| Optional | **06** (totalcor/ve/iacstep) for Follow mode |

Group N is broadcast at ID `1520 + N` whether or not it is enabled; enabling a
group just turns its transmission on. IDs are fixed — the box needs no
reconfiguration if you add groups later.

### Group gating

- Missing group 00 → RPM reads 0 and RPM triggers/display RPM stay dead.
- Missing group 02 → CLT is invalid: fan stays off (unless manual/forced) and
  IAC auto falls back to the 120 °F interpolated duty.
- Missing group 06 → Follow mode falls back to the auto CLT table (never to a
  closed valve).
- No frame at all for **500 ms** → full fail-safe: fan off, O1–O6 off, IAC =
  `iacFailDuty`. The display shows **CAN LOST** and the LED blinks fast.

## 4. Output logic

### 4.1 Fan

Priority (first true wins): **input-forced** → **29-bit remote port bit** →
**auto CLT hysteresis** → **manual**.

- Auto: on when CLT ≥ `fanOnTemp` (default 200 °F), off when CLT ≤ `fanOffTemp`
  (default 190 °F). Hysteresis prevents relay chatter.
- Manual: `F1` / `F0` pins it on/off.
- Channel: the fan rides a ULN2003A output selected by `fanOut` (default **6** =
  O6/GPIO23). `Y<k>` moves it to channel k (1–6) or off (0). The chosen channel
  is skipped by the O1–O6 loop.
- ULN2003A channels are rated ~500 mA — drive a big radiator fan through a relay
  powered by a ULN channel instead of directly.

### 4.2 IAC (idle control)

Three modes, selected with `I`:

| Mode | Command | Behavior |
|------|---------|----------|
| **Auto** (default) | `I` or `I A` | Open-loop duty from a CLT table (60% @50 °F → 28% @200 °F, interpolated) plus an RPM trim: target `iacTargetRpm` (default 900), +8%/−5% duty max. |
| **Follow** | `I F` | Mirrors the ECU's own idle output: MS2/Extra broadcasts its valve duty as `iacstep` (group 06, offset 54, ×0.392 = duty %). Enable MS2 native idle (`IdleCtl = PWM valve`, open-loop or closed-loop) in TS and the box becomes a pure amplifier for **fully TS-tunable idle** — target rpm, warm-up curve, PID, all from TunerStudio. No local RPM trim (the ECU already has feedback). Group 06 missing → falls back to auto. |
| **Manual** | `I35` | Fixed duty for testing. |

Output: 30 Hz PWM on GPIO32 → IRLZ44N low-side → 3-wire rotary ISCV. The
7A/4A rotary valve opens above ~50 % duty and closes below; if the response is
inverted, swap the two outer wires on the valve connector.

### 4.3 O1–O6

Each output has a mode: **off**, **manual** (`1`/`0`), **temp** (`T<°F>`, needs
valid CLT), or **rpm** (`R<rpm>`, needs group 00). Defaults: O1 = shift light
@7000 rpm (single threshold with natural hysteresis), O2–O6 off. `S<rpm>`
reconfigures O1 to the given rpm.

Input-forced overrides apply on top of any mode (see §5).

### 4.4 Fail-safe

- ECU CAN silent for 500 ms → fan + O1–O6 off, IAC = `iacFailDuty` (default 0),
  display status goes red **CAN LOST**, LED blinks at 120 ms.
- The engine keeps running on the mechanical base idle — set the throttle stop
  ~700 rpm so the car idles even with the ISCV fully closed.
- The rotary ISCV unpowered rest is ~1000-1200 rpm — a natural safety net.

## 5. Inputs drive outputs

### 5.1 Switch inputs D1–D3 (active-low, internal pull-up)

| Command | Meaning |
|---------|---------|
| `D1 O2` | D1 grounded → force O2 on |
| `D2 F` | D2 grounded → force fan on |
| `D3 0` | clear D3 assignment |

### 5.2 Analog inputs A1–A4 (0-5 V, 0.15 V hysteresis)

| Command | Meaning |
|---------|---------|
| `A1 O2 3.5` | A1 above 3.5 V → force O2 on |
| `A2 F 2.0` | A2 above 2.0 V → force fan on |
| `A3 4.2` | set A3 threshold 4.2 V, keep current target |
| `A4 0` | disable A4 trigger |

Hysteresis: input must fall 150 mV below the threshold to release, preventing
chatter. **On the iobox2 PCB these four inputs are wired to 12 V→3.3 V dividers**
for the dash indicator signals — set the thresholds from what the diag app reads
(see `PROTOCOL.md` §4 for the 0x710 frame and divider values).

## 6. Display (GC9A01A round 240×240)

Idle-control focused, refreshed at 10 Hz:

- **Big center**: current IAC duty %.
- **Quadrants**: RPM (white), target RPM (yellow), CLT °F (yellow), idle MODE
  (AUTO / FOLLOW / MAN / RMT). CLT and RPM show `--` while a group is missing.
- **Bottom line**: `FAN ON/OFF  CAN OK` (green/light-grey) or `CAN LOST` (red).
- RMT mode (29-bit remote driving the ports) shows magenta.

Pin map: SCL→15, MOSI/SDA→21, CS→22, DC→19, RST→3V3, VCC→5V. (D4's old pin
GPIO19 became the display DC line — switch inputs are D1–D3.)

## 7. 29-bit CAN responder (MS2/Extra remote ports)

The box answers the Megasquirt expansion protocol (29-bit extended frames) so an
MS2/Extra master can directly drive its outputs and read its inputs:

| Serial | Effect |
|--------|--------|
| `R1` | enable responder |
| `R0` | disable (default) |
| `RB5` | set box ID (1–14; default 5 — matches TS `can_poll_id0-3`) |

When a fresh remote **port frame** is present the remote bytes drive the box:
byte0 bits 0-5 = O1–O6, bit 0x40 = fan, byte1 = ISCV duty (×100/255). Input
forces still OR in. If the port frames go stale (>500 ms) the box falls back to
autonomous logic; if the ECU broadcast is gone entirely it goes fail-safe.

Full wire format, reply echo rules and TS config for remote ports:
`docs/PROTOCOL.md` §3.

## 8. Windows diagnostic app (`diag/iobox_diag.py`)

Single-file Python 3 + Tkinter tool that talks the same serial command language
over USB (WiFi/TCP transport is in the code but the box firmware has no WiFi
server yet — use USB).

```sh
pip install pyserial      # only needed for the USB path
python iobox_diag.py      # or python3
```

- **Connection bar**: pick the USB COM port (Refresh scans), Connect.
- **Fan tab**: auto/manual, on/off thresholds, output channel (`Y`).
- **IAC tab**: auto / follow / manual duty, target rpm.
- **Outputs tab**: per-output manual on/off, temp and rpm triggers.
- **Inputs tab**: D1–D3 and A1–A4 → target output mappings.
- **Advanced tab**: 29-bit responder enable/disable/ID, shift-light rpm.
- **Console**: raw command entry, `?` status button, scrolling RX log — every
  command shown here maps 1:1 to the serial reference below.

## 9. Serial command reference (115200 baud)

| Command | Effect |
|---------|--------|
| `?` | status dump: can fresh, rpm, map, mat, clt, tps, batt, afr, fan, iac mode/duty, responder state/ports, output states, input voltages/states |
| `F` | fan auto, default thresholds |
| `F205` | fan auto, on ≥ 205 °F |
| `F1` / `F0` | fan manual on / off |
| `E190` | fan off (hysteresis) ≤ 190 °F |
| `I35` | IAC manual 35 % |
| `I` / `I A` | IAC auto (CLT table + RPM trim) |
| `I F` | IAC follow-ECU (`iacstep`; needs group 06 + TS `IdleCtl = PWM`) |
| `T900` | IAC target idle 900 rpm (auto mode) |
| `S7000` | shift light threshold 7000 rpm (O1) |
| `Y6` | fan on ULN channel 6 (1–6, 0 = off) |
| `O2 1` / `O2 0` | O2 manual on / off |
| `O3 T210` | O3 on when CLT ≥ 210 °F |
| `O4 R3500` | O4 on when rpm ≥ 3500 |
| `O5 A` | O5 auto/off (clear mode) |
| `D1 O2` | switch D1 drives O2 (closed = on) |
| `D2 F` | switch D2 forces fan |
| `D3 0` / `D3 N` | clear D3 assignment |
| `A1 O2 3.5` | A1 above 3.5 V forces O2 |
| `A2 F 2.0` | A2 above 2.0 V forces fan |
| `A3 4.2` | set A3 threshold only |
| `A4 0` | disable A4 |
| `R1` / `R0` | 29-bit responder on / off |
| `RB5` | responder box ID = 5 |
| `S` | print the full command list |

All settings are written to NVS (stored with a magic/version word; a stored
config of the wrong size or without the magic is discarded and defaults are
rewritten).

## 10. Bench-testing checklist

Before installing in the car:

1. Flash + serial: `?` works, status shows `can_fresh=0`.
2. Second ESP32 + SN65HVD230 broadcasting synthetic outpc (groups 00/02/03, 20
   Hz): `can_fresh=1`, rpm/CLT/AFR appear on `?` and the display.
3. Fan: `F1` → fan channel goes low-side on; `F0` off; `F200`/`E190` with a fake
   CLT → hysteresis toggles.
4. IAC: `I60` → ISCV PWM; `I F` + synthetic group 06 → duty tracks `iacstep`;
   `T<rpm>` changes the target shown on the display.
5. Inputs: ground a D pin → forced output; feed a voltage above/below an A
   threshold → latch + release (0.15 V hysteresis).
6. Responder: send a 29-bit MSG_CMD port frame (box ID 5) → O1–O6/fan/IAC follow
   the port bytes; stop sending → falls back to autonomous within 500 ms.
7. `0x710`: sniff the bus — 8-byte frame every 100 ms, byte0/byte1 track the A/D
   input states.
8. Cut CAN entirely → all outputs off, display `CAN LOST`, LED fast-blink.

## 11. Integration notes (7A-GTE)

- The I/O box frees **FIDLE** — the boost solenoid can move to TACHOUT; the box
  never touches the ECU's boost output.
- On this car the ISCV was blocked off and idle ran at 1600 rpm via the manual
  screw. Sequence: unblock the ISCV → wire it to the box IAC → set the manual
  screw base idle ~700 rpm → let the box close-loop the rest (do this in the shop
  with the wideband live).
- **Before the first start after any tune change**: the box's injector-PWM fix
  (`injctl2_1=On`, `injPwmP=30`) must be loaded and burned on the ECU side —
  see the AGENTS.md project notes for the 7A-GTE specifics.
