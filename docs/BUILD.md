# ESP32 MicroSquirt I/O box — Point-to-point build

DIY clone of the MS I/O-box: ESP32 + SN65HVD230 reads the MicroSquirt CAN broadcast
(base ID 0x5F0, 500k) and drives fan, IAC, and 6 generic low-side outputs. Fail-safe:
all outputs off after 500 ms no CAN.

Build style: **module-to-module point-to-point wiring** — no unified PCB. ESP32 comes
with a pinout board / screw terminals; every module (CAN transceiver, MOSFETs, ULN2003A,
buck) wires directly to it.

---

## 1. Block diagram

```
                        ┌──────────────────────────────┐
                        │         +12V switched         │
                        └──────┬───────────────────────┘
                               │
                          [ 2-5A fuse ]
                               │
                       ┌───────┴────────┐
                       │  MP1584/LM2596 │  buck 12V→5V
                       │  buck module   │
                       └───┬────────┬───┘
                           │ 5V     │ GND
                           │        │
        ┌──────────────────┼────────┼──────────────────────────┐
        │                  │        │                          │
   ┌────┴─────┐      ┌─────┴────┐   │                    ┌─────┴─────┐
   │  ESP32   │      │SN65HVD230│   │                    │  MicroSq. │
   │ DevKit   │      │ CAN trx  │   │                    │  V3 (uS)  │
   │ 30-pin   │      │          │   │                    │           │
   └─┬──┬──┬──┘      └──┬────┬──┘   │                    └─────┬─────┘
     │  │  │            │    │      │                          │
     │  │  │   GPIO4 RX─┘    └─GPIO5 TX                       │
     │  │  │            │          │                          │
     │  │  │       CANH └──────────┼────────── uS pin 2 (CANH, Blue/Yellow)
     │  │  │       CANL ───────────┼────────── uS pin 3 (CANL, Blue/Red)
     │  │  │                       │
     │  │  │   (120Ω termination on SN65HVD230 jumper — uS has its own)
     │  │  │
     │  │  └── GPIO32 → IRLZ44N → ISCV (IAC, 30 Hz PWM)
     │  │
      ├── GPIO25,26,27,14,13,23 → ULN2003A IN1-IN6 → O1-O6 (relays/loads)
      │  │     fan = ULN channel (default O6/GPIO23, Y<k> command to change)
      ├── GPIO2  → status LED (onboard)
      ├── GPIO36,39,34,35 → A1-A4 (0-5V via 10k/20k divider)
      ├── GPIO16,17,18 → D1-D3 switches (to GND, INPUT_PULLUP)
      └── GPIO15,21,22,19 → GC9A01 round display (SCLK, MOSI, CS, DC)
```

---

## 2. Wiring diagram (point-to-point)

### 2.1 Power

```
+12V (switched) ──[fuse 2-5A]──┬─ buck IN+
                               └─ fan/ISCV/relay +12V rail
buck OUT 5V ── ESP32 "5V" pin
buck GND ───── ESP32 GND + SN65HVD230 GND + uS signal GND (all common)
```

### 2.2 CAN (MicroSquirt → SN65HVD230 → ESP32)

```
uS pin 2 (CANH, Blue/Yellow) ── SN65HVD230 CANH
uS pin 3 (CANL, Blue/Red)    ── SN65HVD230 CANL
ESP32 GPIO5 (TX)             ── SN65HVD230 CTX
ESP32 GPIO4 (RX)             ── SN65HVD230 CRX
SN65HVD230 120Ω jumper: ON
```

### 2.3 IAC stage — confirmed: 7A rotary-solenoid ISCV (3-wire)

Customer's valve is the **7A-FE/4A-FE rotary solenoid ISCV** (NOT a stepper). 3-wire
connector: **center = +12V (switched)**; one outer pin = **ground**; one outer pin =
**ECU low-side PWM** (IRLZ44N on GPIO32). Duty ratio: **>50% opens, <50% closes**
(swap the two outer pins to invert if it responds backwards). Unpowered, the valve
rests at ~1000-1200 rpm — a natural fail-safe (CAN loss → valve stays near default).

```
+12V rail ── ISCV center pin (+B)
ISCV outer pin (E1/ground side) ── GND
ESP32 GPIO32 ──[100Ω]── gate ──[10kΩ]── GND   (IRLZ44N)
               source ── GND
               drain  ── ISCV remaining outer pin (PWM control)
               [1N5404 across ISCV, cathode→+12V, anode→drain]
```

Firmware: existing PWM mode on GPIO32 already correct (30 Hz low-side duty).
Only tuning: map duty so >50% = open (or rewire outer pins to invert).

**Option B — 4-wire stepper ISCV (22270-16010/16020): H-bridge/stepper driver**

A stepper has 2 coils (4 wires) and CANNOT be driven by one PWM channel. Use a
stepper driver (DRV8825/A4988) or dual H-bridge (L298N).

**NOTE**: the old design reserved GPIO21+GPIO22 for the stepper. Those are now used by
the round display (MOSI, CS). The stepper option is demoted — if a 4-wire valve ever
shows up, rewire the display elsewhere (or run it on a second ESP32). The confirmed
3-wire rotary ISCV (Option A above) is the only supported IAC path. Stepper firmware
is also still pending — current `main.cpp` outputs PWM on GPIO32 only (Option A).


### 2.4 Fan stage (via ULN2003A channel, default O6/GPIO23)

**There is NO dedicated fan MOSFET.** The firmware drives the fan through one of the
ULN2003A channels — `Cfg.fanOut` (default **6** = GPIO23/O6, set with the `Y<k>`
serial command, `Y0` = off). `setFan()` in `main.cpp:127` just pokes that channel and
the output loop skips it (main.cpp:329).

```
fan + wire ── +12V rail
fan − wire ── ULN OUT<fanOut>   (e.g. OUT6)
fan control wire ── (ULN COM → +12V already provides internal flyback)
```

- **Inductive kickback**: handled by the ULN2003A built-in flyback diodes (COM must be
  tied to +12V). No external 1N5404 needed on the fan.
- **Current**: ULN2003A channels are ~500 mA continuous / 350 mA per-channel worst-case
  daisy-chained. If the radiator fan draws more, drive a **relay** from a ULN channel
  instead (ULN OUT → relay coil → +12V, relay contacts switch the fan).
- Default output is O6; if you want the fan on a different channel, set `Y<k>` and wire
  that OUT pin to the fan.

### 2.5 Generic outputs (ULN2003A)

```
ESP32 GPIO25 ── ULN IN1 │ OUT1 ── O1 (shift light 7000 rpm)
ESP32 GPIO26 ── ULN IN2 │ OUT2 ── O2
ESP32 GPIO27 ── ULN IN3 │ OUT3 ── O3
ESP32 GPIO14 ── ULN IN4 │ OUT4 ── O4
ESP32 GPIO13 ── ULN IN5 │ OUT5 ── O5
ESP32 GPIO23 ── ULN IN6 │ OUT6 ── O6 (fan by default — see §2.4)
ULN COM pin ── +12V rail (internal flyback diodes need this)
Each OUT n ── load low side; load + side ── +12V
```

### 2.6 Analog inputs (×4) — 0-5V → 3.3V

```
sensor sig (0-5V) ──[10kΩ]── GPIO36 (A1)
                 └──[20kΩ]── GND
optional 0.1µF cap: GPIO ── GND
same divider for A2/A3/A4 → GPIO39/34/35
```

### 2.7 Switch inputs (×3) — active low

```
GPIO16 ── D1 switch ── GND   (INPUT_PULLUP internal)
GPIO17 ── D2 switch ── GND
GPIO18 ── D3 switch ── GND
```

### 2.8 Round display (GC9A01A 240×240) — 4-wire SPI

```
display VCC ── 5V      display GND ── GND
display SCL ── GPIO15   display SDA ── GPIO21
display CS  ── GPIO22   display DC  ── GPIO19
display RST ── 3V3 (tied high; reset handled at power-up)
```

Software SPI via the Adafruit GC9A01A library (hardware SPI pins 15/21/22/19 are in
use by other channels). Renders an idle-control gauge: big IAC duty %, actual RPM /
target RPM / CLT / idle mode (AUTO·FOLLOW·MAN), and a bottom fan + CAN status line
(red "CAN LOST" on fail-safe). D4's old pin (GPIO19) became the display DC line —
switch inputs are now D1-D3.

### 2.9 Status LED

```
GPIO2 ── onboard LED (CAN heartbeat)   [no wiring needed]
```

---

## 3. ESP32 pin map (summary)

| GPIO | Function | Drives |
|------|----------|--------|
| 5 | CAN TX | SN65HVD230 CTX |
| 4 | CAN RX | SN65HVD230 CRX |
| 32 | IAC PWM | ISCV via IRLZ44N (30 Hz) |
| 25 | O1 shift light | ULN2003A |
| 26 | O2 | ULN2003A |
| 27 | O3 | ULN2003A |
| 14 | O4 | ULN2003A |
| 13 | O5 | ULN2003A |
| 23 | O6 — fan (default) | ULN2003A |
| 2 | Status LED | onboard |
| 15 | Display SCLK | GC9A01 round display |
| 21 | Display MOSI | GC9A01 round display |
| 22 | Display CS | GC9A01 round display |
| 19 | Display DC | GC9A01 round display |
| 36,39,34,35 | A1-A4 analog 0-5V | via 10k/20k divider to 3.3V |
| 16,17,18 | D1-D3 switches | INPUT_PULLUP, active-low |

---

## 4. Parts list

### Required

| Qty | Part | Purpose | Est. cost |
|-----|------|---------|-----------|
| 1 | ESP32 DevKit (WROOM-32) 30-pin with pinout board + screw terminals | main controller | $4-8 |
| 1 | SN65HVD230 CAN transceiver module | CAN interface (3.3V), 120Ω jumper | $2-3 |
| 1 | IRLZ44N logic-level N-MOSFET (TO-220) | IAC low-side | $1-2 |
| 1 | ULN2003A driver IC (DIP-16 or breakout) | O1-O6 low-side + fan channel | $1-2 |
| 1 | 12V→5V buck module (MP1584 / LM2596) | logic power — never feed 12V to ESP32 | $2-3 |
| 1 | ISCV (7A-FE/4A-FE rotary solenoid, **3-wire**) | IAC valve | on car |
| 1 | 1.28" round GC9A01A TFT 240×240 (e.g. Teyleten B0B7TFRNN1) | info gauge, SPI | $3-5 |
| 0-1 | **DRV8825/A4988 or L298N** (only if a 4-wire stepper ISCV is ever used) | stepper driver — NOTE: GPIO21/22 now used by display | $2-4 |

### Resistors

| Value | Qty | Where |
|-------|-----|-------|
| 10kΩ | 4 | A1-A4 dividers (sensor → GPIO) |
| 20kΩ | 4 | A1-A4 dividers (sensor → GND) |
| 10kΩ | 1 | gate pull-down on IRLZ44N (keeps MOSFET off during ESP32 boot) |
| 100Ω | 1 | optional gate series (noise) |
| 100µF electrolytic | 1 | bulk cap across DRV8825 VMOT/GND (only for stepper option) |

### Protection

| Part | Qty | Where |
|------|-----|-------|
| 1N5404 flyback diode | 1 | across ISCV (cathode → +12V). Fan kickback is handled by ULN2003A internal diodes |
| 0.1µF ceramic cap | 4 | 1 per analog input, GPIO → GND |
| 2-5A fast-blow inline fuse | 1 | 12V feed into buck |
| 120Ω resistor | 1 | only if SN65HVD230 module lacks termination jumper |

### Wire / hardware

| Item | Qty | Purpose |
|------|-----|---------|
| 18-22 AWG wire (various colors) | roll | power + loads |
| 24 AWG hookup wire | roll | signals (CAN, GPIO) |
| Heat shrink / solder / flux | — | joints |
| Small enclosure or mounting plate | 1 | mount modules |
| Ring/spade terminals + M3 bolts | — | IRLZ44N TO-220 mounting |

---

## 5. Build order (suggested)

1. Buck module: fuse → 12V in, set 5V out, verify with multimeter before connecting ESP32
2. CAN: SN65HVD230 → ESP32 GPIO4/5, then MicroSquirt pins 2/3 (only after power check)
3. MOSFET stage: IRLZ44N + gate pull-down + flyback diode (IAC only)
4. ULN2003A: COM to +12V, wire O1-O6 (+ fan on its channel)
5. Analog dividers A1-A4 + caps
6. Switch inputs D1-D3
7. Round display: SCL→15, SDA→21, CS→22, DC→19, RST→3V3
8. Flash firmware, bench-test with CAN sniffer, then install

## 6. TunerStudio setup (one-time, on the MicroSquirt)

1. `CAN Parameters` → Master Enable `Enable` (already set in 7A-GTE tune)
2. `CAN Realtime Data Broadcasting`:
   - Enable realtime data broadcasting over CAN = **On**
   - Base message identifier = **1520**
   - Broadcasting rate = **20 Hz**
   - Enable groups **00** (rpm), **02** (baro/map/mat/clt), **03** (tps/batt/afr)

## 7. Cautions

- **Never** feed 12V into ESP32 or SN65HVD230 — buck is mandatory.
- **Fan rides a ULN2003A channel (default O6/GPIO23, `Y<k>` to change)** — no dedicated
  MOSFET. ULN channels are ~500 mA: use a relay for a big radiator fan.
- **ISCV = 7A rotary solenoid (3-wire)**: center +12V, one outer to GND, one outer to
  IRLZ44N drain. Duty >50% opens, <50% closes. Swap the two outer pins if inverted.
  Do NOT use a stepper driver with this valve.
- All outputs are low-side (ground-switch), like the real MS I/O-box.
- CAN loss leaves the rotary ISCV at its ~1000-1200 rpm default (natural fail-safe);
  set the base idle screw below that so the box controls idle.
