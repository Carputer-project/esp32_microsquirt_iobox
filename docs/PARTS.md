# ESP32 MicroSquirt I/O box — Parts list & wiring

Everything needed to build the I/O box. Prices are rough street prices (AliExpress/
DigiKey/Mouser/local); totals assume AliExpress quantities.

## 1. Required parts

| Qty | Part | Purpose | Est. cost |
|-----|------|---------|-----------|
| 1 | ESP32 DevKit (WROOM-32), 30-pin | main controller | $4-8 |
| 1 | SN65HVD230 CAN transceiver module | CAN bus interface (3.3V) | $2-3 |
| 1 | IRLZ44N logic-level N-MOSFET (TO-220) | IAC low-side switch | $1-2 |
| 1 | ULN2003A driver IC (DIP-16 or breakout) | 6 generic low-side outputs O1-O6 (+ fan channel) | $1-2 |
| 1 | 12V→5V buck module (MP1584 / LM2596) | powers logic — do NOT feed 12V into the ESP32 | $2-3 |
| 1 | 4A-GE/7A-FE ISCV (3-wire rotary solenoid) | idle control valve (IAC function only) | spare/used |

Core total: **~$10-20**

## 2. Resistors

| Value | Qty | Where | Required? |
|-------|-----|-------|-----------|
| 10kΩ + 20kΩ | 4 pairs | A1-A4 input dividers: sensor → 10k → GPIO, sensor → 20k → GND (GPIO sees 2/3 of input; 0-5V → 0-3.3V) | yes |
| 10kΩ | 1 | gate pull-down on the IRLZ44N gate (keeps MOSFET OFF during ESP32 boot while GPIO floats) | recommended |
| 100Ω-1kΩ | 1 | gate series resistor (slow switching edge, reduce noise) | optional |
| 120Ω | 1 | bus termination — only if the SN65HVD230 module lacks its termination jumper (the MicroSquirt V3 already has its own terminator) | only then |

## 3. Protection components (recommended)

| Part | Qty | Where |
|------|-----|-------|
| 1N5404 or 1N4007 flyback diode | 1 | across the ISCV (cathode to +12V) — kills inductive kickback that would destroy the MOSFET. Fan kickback is handled by the ULN2003A internal flyback diodes |
| 0.1µF ceramic cap | 4 | 1 per analog input, GPIO → GND — filters sensor noise |
| Inline fuse 2-5A fast-blow | 1 | on the 12V feed into the buck module |

## 4. Wiring summary

### CAN
```
uS pin 2 (CANH, Blue/Yellow)  → SN65HVD230 CANH
uS pin 3 (CANL, Blue/Red)     → SN65HVD230 CANL
ESP32 GPIO5 (TX)              → SN65HVD230 CTX
ESP32 GPIO4 (RX)              → SN65HVD230 CRX
```
MicroSquirt V3 has an internal terminator; the SN65HVD230 module's 120Ω jumper is the
other end (2-node bus). Keep it ON.

### ESP32 pin map
| GPIO | Function |
|------|----------|
| 5 / 4 | CAN TX / RX → SN65HVD230 |
| 32 | IAC PWM → IRLZ44N gate → ISCV |
| 25,26,27,14,13,23 | O1-O6 → ULN2003A inputs (O6 = fan by default) |
| 2 | status LED (onboard) |
| 36,39,34,35 | A1-A4 analog (via 10k/20k divider) |
| 15,21,22,19 | GC9A01 display SCLK/MOSI/CS/DC |

### MOSFET stage (IAC — ×1)
```
GPIO → [optional 100Ω] → gate ── 10kΩ → GND
source → GND
drain  → load low side (load other end to +12V)
flyback diode across load (cathode → +12V)
```

### ULN2003A stage (O1-O6 + fan)
```
GPIO → ULN2003A IN pin directly (built-in 2.7kΩ base resistor)
ULN2003A OUT pin → relay coil / device to +12V (internal flyback diodes)
Fan (default = OUT6): fan − → OUT<fanOut>, fan + → +12V; ULN COM → +12V.
Use a relay for fans drawing > ~500 mA (ULN channel limit).
```

### Analog inputs (×4)
```
sensor signal (0-5V) → 10kΩ → GPIO
                    → 20kΩ → GND
optional 0.1µF cap: GPIO → GND
```

### Switches

None — D1-D3 switch inputs were removed from the firmware. GPIOs 16/17/18 are
unused on the board.

### Power
```
+12V → fuse → buck in → 5V out → ESP32 5V pin
ESP32 GND = buck GND = SN65HVD230 GND = uS signal ground (all common)
```

## 5. Notes / cautions

- **Never** feed 12V into the ESP32 or SN65HVD230 — the buck module is mandatory.
- ISCV is a **3-wire rotary solenoid** (center = +12V, one outer = GND, one outer =
  IRLZ44N drain). PWM duty >50% opens, <50% closes — swap the outer pins to invert.
  Do NOT use a stepper driver with this valve.
- If running the IAC output, leave the 3.3V/5V off the gate — these are low-side,
  ground-switching drivers.
- All outputs are low-side (ground-switch) exactly like the real MS I/O-box.
- Fan rides a ULN2003A channel (`fanOut`, default 6 = O6). ULN channels are ~500 mA —
  run a big radiator fan through a relay driven by a ULN channel instead.
