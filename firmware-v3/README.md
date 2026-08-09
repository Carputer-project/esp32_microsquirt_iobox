# ESP32 MicroSquirt I/O box v3 (iobox3)

Third-generation I/O box for MicroSquirt/MS2-extra. Same firmware core as iobox2
(CAN broadcast decode, IAC, fan, outputs, 29-bit responder, EngineProfile warnings,
0x710 dash broadcast) but **output + IAC pins are runtime-configurable** and stored
in NVS, so one firmware serves any WROOM-32 board.

## Pin map (default = iobox3 board)

Configurable at runtime (persisted in NVS) via the `P` command or the diag app's
**Pins** tab:

| Function | Default GPIO | Configurable |
|----------|--------------|--------------|
| IAC PWM (LEDC 30 Hz) | 19 | yes (`P IAC <pin>`) |
| O1-O7 (ULN2003A low-side) | 13, 12, 14, 27, 26, 25, 33 | yes (`P O<n> <pin>`) |
| CAN TX / RX | 5 / 4 | no (compile-time) |
| Analog A1-A4 | 36, 39, 34, 35 | no (compile-time) |
| Status LED | 2 | no (compile-time) |

Board presets are built into the diag app:
- **iobox2**: IAC 32, O1-O6 25/26/27/14/13/23 (+ O7 spare 33)
- **iobox3**: IAC 19, O1-O7 13/12/14/27/26/25/33

Changing a pin takes effect immediately (`applyPinConfig()` reconfigures GPIO modes
and re-attaches LEDC) and survives reboot via NVS.

## Rejected pins

`P` refuses to map these GPIOs (boot straps, console UART, LED, CAN, dead, or
input-only): 0, 1, 2, 3, 4, 5, 6-11, 20, 24, 28-31, 34, 35, 36, 39.

## Serial commands

Everything from iobox2 plus:

| Command | Effect |
|---------|--------|
| `P` | report current pin map |
| `P IAC 19` | set IAC pin |
| `P O3 27` | set O3 pin |
| `P RESET` | restore iobox3 defaults (persisted) |

Full iobox2 command set (F/E/I/T/Y/S/O/A/D/R/W) is unchanged — see
`../esp32_ms_iobox/README.md`.

## Build & flash

```
pio run                         # build
pio run --target upload         # flash over USB
pio device monitor -b 115200    # serial console
```

## Diag app

`~/Desktop/carputer/iobox_diag/iobox_diag.py` gained a **Pins** tab: pick a board
preset, edit the GPIO fields, Apply to box. Read-from-box (`P`) parses the reply
into the fields automatically.
