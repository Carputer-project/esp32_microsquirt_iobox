# iobox_diag — Windows test/configure app

Single-file **Python 3 + Tkinter** GUI for the ESP32 MicroSquirt I/O box. It
talks the same text command language the box exposes on its serial console, so
every action maps 1:1 to the serial reference in `../docs/OPERATION.md` §9.

![dependency: python3 + tkinter + optional pyserial]

## Run

```sh
pip install pyserial      # only needed for the USB path
python iobox_diag.py      # python3 on Linux/macOS
```

No Qt needed. Works on Windows too (uses `python`, `pip`).

## What it does

| Tab | Purpose |
|-----|---------|
| Fan | auto / manual on-off, on-threshold (°F), off-threshold (°F), output channel (`Y1-7`) |
| IAC | auto / follow-ECU / manual duty, target idle rpm |
| Outputs | per-output manual on/off, temp trigger, rpm trigger (O1-O7) |
| Inputs | D1-D3 switch → target mapping, A1-A4 analog → target + threshold volts |
| Engine | engine-profile limits grid (idle/maxrpm/clt/mat/batt/map/afr/hold/warnout) |
| Pins | **IAC + output GPIO map** — board presets (iobox2 / iobox3), edit, apply (`P` cmds), read from box |
| Advanced | 29-bit CAN responder enable/disable/box-ID, shift-light rpm |
| Console | raw command line, `?` status button, scrolling RX log |

## Transport

- **USB (default, active)**: `pyserial`, 115200 baud. Pick the COM port and
  Connect.
- **WiFi/TCP**: the radio is currently **disabled** ("WiFi pending (firmware)") —
  the box firmware has no WiFi server yet. The TCP transport code
  (`IoBox.connect_tcp`, default `192.168.4.1:23`) is left in place for when the
  box grows a soft-AP command port.

## Design notes

- `IoBox` class: serial or TCP link, background reader thread → `queue.Queue` →
  `poll()` drains to the UI. `on_line` / `_poll` keep the console live at 50 ms.
- The box persists every setting in NVS; the app just sends commands.
- Status responses (e.g. `?`) come back as `can_fresh=... rpm=...` lines in the
  console — parse-friendly for future automation.
