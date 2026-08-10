# CAN protocol reference

Three independent streams live on the bus:

1. **ECU → box**: 11-bit broadcast `0x5F0`+ (MS2/Extra outpc groups).
2. **Box → dash**: 11-bit `0x710` (input states for the in-dash HMI).
3. **Master → box**: 29-bit extended `MSG_REQ` / `MSG_CMD` (Megasquirt expansion
   protocol; MS2/Extra remote ports).

All frames are **500 kbps**. The MicroSquirt V3 has an internal CAN terminator;
the SN65HVD230 module's 120 Ω jumper is the other end of a 2-node bus.

---

## 1. ECU broadcast — `0x5F0` … `0x5F8` (11-bit, 20 Hz, big-endian)

MS2/Extra MicroSquirt broadcasts its `outpc` structure 8 bytes per group, group N
at ID `1520 + N`. Offsets are the `outpc` offsets from `microsquirt-module.ini`
(`ochBlockSize = 212`). Enabled groups: 00, 02, 03 (+ 06 for Follow mode).

| ID | Grp | Byte | Channels (offset, type, scale) |
|----|-----|------|-------------------------------|
| 0x5F0 | 00 | 0-7 | seconds u16@0, pw1 u16@2, pw2 u16@4, **rpm u16@6** |
| 0x5F1 | 01 | 8-15 | advance s16@8 (×0.1°), flags, afrtgt1/2, wbo2 |
| 0x5F2 | 02 | 16-23 | **baro s16@16**, **map s16@18**, **mat s16@20**, **clt s16@22** (all ×0.1) |
| 0x5F3 | 03 | 24-31 | **tps s16@24**, **batt s16@26**, **afr1 s16@28**, afr2 s16@30 (×0.1) |
| 0x5F4 | 04 | 32-39 | knock u16@32, egocor1 s16@34, egocor2 s16@36, aircor s16@38 |
| 0x5F5 | 05 | 40-47 | warmcor s16@40, tpsaccel s16@42, tpsfuelcut s16@44, barocor s16@46 |
| 0x5F6 | 06 | 48-55 | totalcor s16@48, ve1 s16@50, ve2 s16@52, **iacstep s16@54** |
| 0x5F7 | 07 | 56-63 | coldadv s16@56, TPSdot s16@58, MAPdot s16@60, dwell u16@62 |

Notes:

- All values big-endian; temperatures in °F (×10), pressures in kPa (×10).
- `iacstep` (offset 54) is the ECU's idle valve duty raw 0-255; **duty % =
  `iacstep × 0.392`**. Used by Follow mode (TS `IdleCtl = PWM valve`).
- Group 08 (0x5F8, outpc 64-71) carries portStatus/knockRetard — reserved.

The box's TWAI filter is accept-all (`0xFFFFFFFF`); software ignores IDs outside
`0x5F0`–`0x5F8`, plus `0x600` (commands) and extended frames.

---

## 2. Box → dash — `0x710` (11-bit, 8 bytes, 10 Hz, always on)

Broadcast by the box itself (not the ECU), so it keeps running even if the ECU
bus drops. Carried the input states for the in-dash HMI.

| Byte | Bits | Meaning |
|------|------|---------|
| 0 | 0-3 | A1–A4 threshold latches (1 = analog input above its `A<n>` trigger) |
| 1 | — | reserved (0) |
| 2 | all | frame counter (rollover) |
| 3-7 | — | reserved (0) |

The dash decodes this alongside `0x5F0`–`0x5F8` to render indicator / high-beam /
brake icons. The ECU ignores `0x710` (not a group it broadcasts or listens to).

**iobox2 PCB analog dividers** (indicator L, indicator R, high beam, brake
lights): 12 V signals → divider → A1–A4. Ratio must be ≈ 1/4.8 so the GPIO sees
≤ 3.3 V at 14.4 V running voltage. Recommended resistor pairs (series / to-GND):
**33 kΩ / 8.2 kΩ**, 10 kΩ / 2.7 kΩ, 30 kΩ / 7.5 kΩ, or 47 kΩ / 12 kΩ. Avoid
10 kΩ + 3.9 kΩ (≈4.03 V at 14.4 V). Add a 0.1 µF cap GPIO→GND per input; the
firmware's 0.15 V hysteresis is built in. Thresholds are set from what the diag
app reports (board reports GPIO volts × `ADC_DIVIDER` = 1.5).

---

## 3. 29-bit expansion responder — MSG_REQ / MSG_CMD

The box implements the responder side of the Megasquirt 29-bit MS-CAN protocol,
so an MS2/Extra master can directly drive remote outputs and read remote ADC/ports
(MS2/Extra has full remote-port support in 3.4.4 — only the *official* MS2
I/O-box firmware is broadcast-only).

### 3.1 Header (29-bit extended identifier)

```
bit 28 .. 19 : offset      (11 bits, 0x000-0x7FF)
bit 18 .. 16 : type        (3 bits)
bit 15 .. 12 : from_id     (4 bits)
bit 11 ..  8 : to_id       (4 bits)
bit  7 ..  4 : table       (4 bits)
bit  3 ..  0 : table[bit4] (1 bit rides wire bit 2)
```

Equivalently `id = (offset<<18) | (type<<15) | (from<<11) | (to<<7) | (table<<3)`
with the table bit-4 folded into bit 2 (see `respMakeId()` in `main.cpp`).

Types: 0 = MSG_CMD, 1 = MSG_REQ, 2 = MSG_RSP, 4 = MSG_BURN, 5 = OUTMSG_REQ,
6 = OUTMSG_RSP, 12 = MSG_REQX, 0x80 = MSG_PROT, 0x82 = MSG_SPND.

IDs: 0 = ECU, 1-14 = boxes, 15 = broadcast. The ECU accepts `to_id` 0 or 15; the
box accepts `to_id` = its box ID (`respId`, default 5) or 15.

### 3.2 MSG_REQ (read box table) — DLC 3

| DSR | Meaning |
|-----|---------|
| 0 | `rem_table` (box echoes it in the reply header) |
| 1 | `rem_off >> 3` (offset bits 10-3) |
| 2 | `((rem_off & 7) << 5) | rem_by` (offset bits 2-0, count 1-8) |

Reply = **MSG_RSP**: header offset = `rem_off` (echo), table = `rem_table` (echo),
from = box, to = requester; DLC = `rem_by`, data = `rem_by` bytes from
`box.tables[rem_table][rem_off]`. The ECU stores the reply into `outpc` at the
echoed offset — **echo `rem_off` and exactly `rem_by` bytes** or the ECU
overwrites unrelated outpc.

### 3.3 MSG_CMD (write box table) — DLC 1-8

Data bytes are stored at `tables[header.table][header.offset]`. For the port
table (default offset 75, auto-detected from the first 1-3 byte write):

| Byte | Meaning |
|------|---------|
| 0 | bits 0-5 = O1–O6 on; bit 0x40 = fan on |
| 1 | ISCV duty raw (×100/255 = %) |
| 2 | reserved |

Remote drive is active while port frames stay fresh (< 500 ms); otherwise the
box falls back to autonomous logic, and with no ECU broadcast at all it goes
fail-safe. Input forces (D/A triggers) always OR in.

### 3.4 Box table 7 (256-byte response table)

| Offset | Contents |
|--------|----------|
| 0-7 | gpioadc0-3 (A1–A4, big-endian mV) |
| 8-15 | gpioadc4-7 (unused, 0) |
| 16-23 | gpiopwmin (unused) |
| 24-74 | free |
| 75-77 | remote ports (see §3.3) |

MSG_REQ reads any offset back; MSG_CMD writes any offset.

### 3.5 TunerStudio setup for remote-port control

```
CAN Parameters        → Master Enable: Enable
Enable poll ports     → enable_pollports: Enable
can_poll_id0-3        → 5 (box ID)
poll_tableports       → 7
poll_offsetports      → 75
ports_dir             → "3 Outputs"
port_generic          → "Remote Port 1"
pwmidle_port2         → "Remote Port 2"
IdleCtl               → PWM valve (open or closed loop)
Group 06              → enable (so Follow-mode fallback + display work)
```

The 0x5F0 broadcast + Follow mode remains available as a parallel, ECU-agnostic
path; the responder and broadcast can be used together or independently.

---

## 4. Box command channel — `0x600` (11-bit, optional)

Any 11-bit frame to ID `0x600` is treated as a serial command string (max 8
bytes, null-terminated): e.g. `F1`, `I F`, `?`. Same parser as the serial port.
Useful for remote reconfiguration over the bus.
