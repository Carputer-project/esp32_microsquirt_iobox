#!/usr/bin/env python3
"""iobox_diag - ESP32 MicroSquirt I/O box testing & configuration tool.

Connects to the ESP32 I/O box over USB serial or WiFi (TCP) and drives the
same text command interface the box exposes on its serial console.

Usage:
    python3 iobox_diag.py            # or python iobox_diag.py on Windows

Deps:
    pyserial (pip install pyserial)  # only needed for the USB path
"""

import queue
import socket
import threading
import tkinter as tk
from tkinter import ttk, messagebox, scrolledtext

try:
    import serial
    import serial.tools.list_ports as list_ports
except ImportError:
    serial = None

BAUD = 115200
DEFAULT_TCP_HOST = "192.168.4.1"
DEFAULT_TCP_PORT = 23


# ── Transport ──────────────────────────────────────────────────────────────
class IoBox:
    """Serial or TCP link to the box. Reader thread -> queue -> UI poll."""

    def __init__(self, on_line):
        self._on_line = on_line
        self._ser = None
        self._sock = None
        self._rx = queue.Queue()
        self._tx_lock = threading.Lock()
        self._stop = threading.Event()

    @property
    def connected(self):
        return self._ser is not None or self._sock is not None

    def _reader(self, readline):
        while not self._stop.is_set():
            try:
                line = readline()
            except Exception:
                break
            if not line:
                break
            self._rx.put(line.decode("latin-1", "replace").rstrip("\r\n"))
        self._rx.put(None)

    def connect_serial(self, port):
        if serial is None:
            raise RuntimeError("pyserial not installed (pip install pyserial)")
        if self.connected:
            self.disconnect()
        self._stop.clear()
        self._ser = serial.Serial(port, BAUD, timeout=0.5)
        threading.Thread(target=self._reader,
                         args=(self._ser.readline,), daemon=True).start()

    def connect_tcp(self, host, port):
        if self.connected:
            self.disconnect()
        self._stop.clear()
        self._sock = socket.create_connection((host, port), timeout=5)
        self._sock.settimeout(0.5)
        f = self._sock.makefile("rb")
        threading.Thread(target=self._reader, args=(f.readline,),
                         daemon=True).start()

    def disconnect(self):
        self._stop.set()
        if self._ser:
            try:
                self._ser.close()
            except Exception:
                pass
            self._ser = None
        if self._sock:
            try:
                self._sock.close()
            except Exception:
                pass
            self._sock = None

    def send(self, cmd):
        if not self.connected:
            return False
        data = cmd.encode("latin-1") + b"\n"
        try:
            with self._tx_lock:
                if self._ser:
                    self._ser.write(data)
                else:
                    self._sock.sendall(data)
        except Exception:
            self._rx.put(None)
            return False
        return True

    def poll(self):
        """Drain queued lines; returns a list of lines + disconnect sentinel."""
        lines = []
        done = False
        while True:
            try:
                item = self._rx.get_nowait()
            except queue.Empty:
                break
            if item is None:
                done = True
            else:
                lines.append(item)
        return lines, done


# ── App ────────────────────────────────────────────────────────────────────
class App:
    def __init__(self, root):
        self.root = root
        self.root.title("I/O Box Diagnostic - ESP32 MicroSquirt")
        self.root.geometry("860x680")
        self.box = IoBox(self.on_line)

        self._build_ui()
        self._poll()

    # ---- UI --------------------------------------------------------------
    def _build_ui(self):
        pad = {"padx": 6, "pady": 4}

        # Connection bar
        conn = ttk.LabelFrame(self.root, text=" Connection ")
        conn.pack(fill="x", padx=8, pady=6)

        self.mode = tk.StringVar(value="serial")
        self.wifi_radio = ttk.Radiobutton(conn, text="WiFi", variable=self.mode,
                        value="wifi", command=self._mode_changed, state="disabled")
        self.wifi_radio.grid(
            row=0, column=0, sticky="w", **pad)
        ttk.Label(conn, text="WiFi pending (firmware)").grid(row=0, column=5, sticky="w")

        ttk.Label(conn, text="Host:").grid(row=0, column=1, sticky="e")
        self.tcp_host = ttk.Entry(conn, width=16)
        self.tcp_host.insert(0, DEFAULT_TCP_HOST)
        self.tcp_host.grid(row=0, column=2, sticky="w")

        ttk.Label(conn, text="Port:").grid(row=0, column=3, sticky="e")
        self.tcp_port = ttk.Entry(conn, width=6)
        self.tcp_port.insert(0, str(DEFAULT_TCP_PORT))
        self.tcp_port.grid(row=0, column=4, sticky="w")

        ttk.Radiobutton(conn, text="USB", variable=self.mode,
                        value="serial", command=self._mode_changed).grid(
            row=1, column=0, sticky="w", **pad)

        self.port_cb = ttk.Combobox(conn, width=24, state="readonly")
        self.port_cb.grid(row=1, column=1, columnspan=3, sticky="w")
        ttk.Button(conn, text="Refresh", command=self._refresh_ports).grid(
            row=1, column=4, **pad)

        self.connect_btn = ttk.Button(conn, text="Connect",
                                      command=self._toggle_connect)
        self.connect_btn.grid(row=0, column=6, rowspan=2, padx=12)

        self.status_lbl = ttk.Label(conn, text="Not connected", foreground="#888")
        self.status_lbl.grid(row=2, column=0, columnspan=7, sticky="w", padx=6)

        # Tabs
        nb = ttk.Notebook(self.root)
        nb.pack(fill="both", expand=True, padx=8)
        self._tab_fan = self._build_fan(nb)
        self._tab_iac = self._build_iac(nb)
        self._tab_out = self._build_outputs(nb)
        self._tab_in = self._build_inputs(nb)
        self._tab_eng = self._build_engine(nb)
        self._tab_pins = self._build_pins(nb)
        self._tab_adv = self._build_advanced(nb)
        nb.add(self._tab_fan, text=" Fan ")
        nb.add(self._tab_iac, text=" IAC ")
        nb.add(self._tab_out, text=" Outputs ")
        nb.add(self._tab_in, text=" Inputs ")
        nb.add(self._tab_eng, text=" Engine ")
        nb.add(self._tab_pins, text=" Pins ")
        nb.add(self._tab_adv, text=" Advanced ")

        # Console
        log = ttk.LabelFrame(self.root, text=" Console / Status ")
        log.pack(fill="both", expand=False, padx=8, pady=6)
        self.console = scrolledtext.ScrolledText(log, height=12, state="disabled",
                                                 font=("Consolas", 9))
        self.console.pack(fill="x", padx=6, pady=4)

        sendrow = ttk.Frame(log)
        sendrow.pack(fill="x", padx=6, pady=(0, 6))
        ttk.Label(sendrow, text="Command:").pack(side="left")
        self.cmd_entry = ttk.Entry(sendrow)
        self.cmd_entry.pack(side="left", fill="x", expand=True, padx=6)
        self.cmd_entry.bind("<Return>", lambda _e: self.send_cmd())
        ttk.Button(sendrow, text="Send", command=self.send_cmd).pack(side="left")
        ttk.Button(sendrow, text="Status (?)", command=lambda: self.send("?")).pack(side="left", padx=6)

        self._refresh_ports()
        self._mode_changed()

    def _build_fan(self, parent):
        f = ttk.Frame(parent, padding=10)
        g = ttk.LabelFrame(f, text=" Fan mode ")
        g.pack(fill="x")
        ttk.Button(g, text="Auto", command=lambda: self.send("F")).grid(row=0, column=0, **self._p())
        ttk.Button(g, text="Manual ON", command=lambda: self.send("F1")).grid(row=0, column=1, **self._p())
        ttk.Button(g, text="Manual OFF", command=lambda: self.send("F0")).grid(row=0, column=2, **self._p())

        g2 = ttk.LabelFrame(f, text=" Thresholds (auto mode) ")
        g2.pack(fill="x", pady=6)
        ttk.Label(g2, text="On at °F:").grid(row=0, column=0, sticky="e")
        self.fan_on = ttk.Entry(g2, width=6)
        self.fan_on.insert(0, "205")
        self.fan_on.grid(row=0, column=1)
        ttk.Button(g2, text="Set", command=lambda: self.send(f"F{self._v(self.fan_on)}")).grid(row=0, column=2, **self._p())
        ttk.Label(g2, text="Off at °F:").grid(row=0, column=3, sticky="e")
        self.fan_off = ttk.Entry(g2, width=6)
        self.fan_off.insert(0, "190")
        self.fan_off.grid(row=0, column=4)
        ttk.Button(g2, text="Set", command=lambda: self.send(f"E{self._v(self.fan_off)}")).grid(row=0, column=5, **self._p())

        g3 = ttk.LabelFrame(f, text=" Output channel ")
        g3.pack(fill="x")
        ttk.Label(g3, text="Fan drives ULN OUT (1-7, 0=off):").grid(row=0, column=0, sticky="e")
        self.fan_out = ttk.Combobox(g3, width=4, values=["0", "1", "2", "3", "4", "5", "6", "7"], state="readonly")
        self.fan_out.set("6")
        self.fan_out.grid(row=0, column=1)
        ttk.Button(g3, text="Set (Y)", command=lambda: self.send(f"Y{self.fan_out.get()}")).grid(row=0, column=2, **self._p())
        return f

    def _build_iac(self, parent):
        f = ttk.Frame(parent, padding=10)
        g = ttk.LabelFrame(f, text=" IAC mode ")
        g.pack(fill="x")
        ttk.Button(g, text="Auto", command=lambda: self.send("I")).grid(row=0, column=0, **self._p())
        ttk.Button(g, text="Follow ECU (iacstep)", command=lambda: self.send("I F")).grid(row=0, column=1, **self._p())

        g2 = ttk.LabelFrame(f, text=" Manual / target ")
        g2.pack(fill="x", pady=6)
        ttk.Label(g2, text="Manual duty %:").grid(row=0, column=0, sticky="e")
        self.iac_duty = ttk.Spinbox(g2, from_=0, to=100, width=5)
        self.iac_duty.set(30)
        self.iac_duty.grid(row=0, column=1)
        ttk.Button(g2, text="Set (I)", command=lambda: self.send(f"I{self.iac_duty.get()}")).grid(row=0, column=2, **self._p())
        ttk.Label(g2, text="Target idle RPM:").grid(row=0, column=3, sticky="e")
        self.iac_tgt = ttk.Entry(g2, width=7)
        self.iac_tgt.insert(0, "900")
        self.iac_tgt.grid(row=0, column=4)
        ttk.Button(g2, text="Set (T)", command=lambda: self.send(f"T{self._v(self.iac_tgt)}")).grid(row=0, column=5, **self._p())
        return f

    def _build_outputs(self, parent):
        f = ttk.Frame(parent, padding=10)
        ttk.Label(f, text="Each output: manual ON/OFF, temp trigger, or RPM trigger.").pack(anchor="w")
        hdr = ttk.Frame(f)
        hdr.pack(fill="x", pady=2)
        for i, h in enumerate(["OUT", "On", "Off", "Temp °F", "Set", "RPM", "Set"]):
            ttk.Label(hdr, text=h, width=8).grid(row=0, column=i, padx=2)
        self.out_rows = []
        for n in range(1, 8):
            row = ttk.Frame(f)
            row.pack(fill="x", pady=1)
            ttk.Label(row, text=f"O{n}", width=8).grid(row=0, column=0, padx=2)
            ttk.Button(row, text="ON", width=5,
                       command=lambda n=n: self.send(f"O{n} 1")).grid(row=0, column=1, padx=2)
            ttk.Button(row, text="OFF", width=5,
                       command=lambda n=n: self.send(f"O{n} 0")).grid(row=0, column=2, padx=2)
            te = ttk.Entry(row, width=6)
            te.insert(0, "205")
            te.grid(row=0, column=3, padx=2)
            ttk.Button(row, text="Set", width=5,
                       command=lambda n=n, e=te: self.send(f"O{n} T{self._v(e)}")).grid(row=0, column=4, padx=2)
            re_ = ttk.Entry(row, width=6)
            re_.insert(0, "3500")
            re_.grid(row=0, column=5, padx=2)
            ttk.Button(row, text="Set", width=5,
                       command=lambda n=n, e=re_: self.send(f"O{n} R{self._v(e)}")).grid(row=0, column=6, padx=2)
            self.out_rows.append((te, re_))
        ttk.Label(f, text="O1 defaults to shift light; 'S' command reconfigures it.").pack(anchor="w", pady=6)
        return f

    def _build_inputs(self, parent):
        f = ttk.Frame(parent, padding=10)
        targets = ["0"] + [f"O{i}" for i in range(1, 8)] + ["F"]

        g = ttk.LabelFrame(f, text=" Switches D1-D3 (ground, active-low) ")
        g.pack(fill="x")
        self.sw_combos = []
        for n in range(1, 4):
            ttk.Label(g, text=f"D{n} drives:").grid(row=n - 1, column=0, sticky="e", padx=6, pady=2)
            cb = ttk.Combobox(g, width=4, values=targets, state="readonly")
            cb.set("0")
            cb.grid(row=n - 1, column=1)
            ttk.Button(g, text="Set", width=5,
                       command=lambda n=n, c=cb: self.send(f"D{n} {c.get()}")).grid(row=n - 1, column=2, padx=6)
            self.sw_combos.append(cb)

        g2 = ttk.LabelFrame(f, text=" Analog A1-A4 (0-5V threshold) ")
        g2.pack(fill="x", pady=8)
        self.an_rows = []
        for n in range(1, 5):
            row = ttk.Frame(g2)
            row.pack(fill="x", pady=1)
            ttk.Label(row, text=f"A{n}", width=4).grid(row=0, column=0, padx=4)
            ttk.Label(row, text="drives:").grid(row=0, column=1)
            cb = ttk.Combobox(row, width=4, values=targets, state="readonly")
            cb.set("0")
            cb.grid(row=0, column=2, padx=4)
            ttk.Label(row, text="above V:").grid(row=0, column=3)
            tv = ttk.Entry(row, width=6)
            tv.insert(0, "3.5")
            tv.grid(row=0, column=4, padx=4)
            ttk.Button(row, text="Set", width=5,
                       command=lambda n=n, c=cb, v=tv: self._set_analog(n, c.get(), v.get())).grid(row=0, column=5, padx=4)
            self.an_rows.append((cb, tv))
        return f

    def _build_engine(self, parent):
        f = ttk.Frame(parent, padding=10)

        g = ttk.LabelFrame(f, text=" Engine profile monitoring ")
        g.pack(fill="x")
        ttk.Label(g, text="Warns when live CAN values go outside these limits; "
                          "top warning blinks on the screen + optional output.").grid(
            row=0, column=0, columnspan=6, sticky="w", padx=6, pady=2)
        ttk.Button(g, text="Enable (W1)", command=lambda: self.send("W1")).grid(row=1, column=0, **self._p())
        ttk.Button(g, text="Disable (W0)", command=lambda: self.send("W0")).grid(row=1, column=1, **self._p())

        rows = [
            ("Idle RPM min", "idle", ["700", "1100"], "idle <min> <max>"),
            ("Max RPM", "maxrpm", ["8000"], "maxrpm <rpm>"),
            ("Coolant max °F", "clt", ["230"], "clt <F>"),
            ("Intake max °F", "mat", ["160"], "mat <F>"),
            ("Battery min/max V", "batt", ["11.0", "16.0"], "batt <min> <max>"),
            ("Boost max kPa", "map", ["280"], "map <kPa>"),
            ("AFR min/max", "afr", ["10.0", "16.5"], "afr <min> <max>"),
            ("Warning hold ms", "hold", ["3000"], "hold <ms>"),
            ("Warn output (0-7)", "warnout", ["0"], "warnout <0-7>"),
        ]
        g2 = ttk.LabelFrame(f, text=" Limits ")
        g2.pack(fill="x", pady=6)
        self.eng_rows = []
        for r, (label, key, defs, _hint) in enumerate(rows):
            ttk.Label(g2, text=f"{label}:").grid(row=r, column=0, sticky="e", padx=6, pady=2)
            entry = ttk.Entry(g2, width=8)
            entry.insert(0, defs[0])
            entry.grid(row=r, column=1, padx=2)
            second = None
            if len(defs) == 2:
                ttk.Label(g2, text="to").grid(row=r, column=2)
                second = ttk.Entry(g2, width=8)
                second.insert(0, defs[1])
                second.grid(row=r, column=3, padx=2)
            ttk.Button(g2, text="Set", width=5,
                       command=lambda key=key, e=entry, s=second: self._set_engine(key, e, s)).grid(
                row=r, column=4, padx=6)
            self.eng_rows.append((entry, second))

        g3 = ttk.LabelFrame(f, text=" Warning output behavior ")
        g3.pack(fill="x")
        ttk.Label(g3, text="The warn output blinks (2 Hz) while a warning is active; "
                           "clears after the hold time once conditions return to spec.").pack(anchor="w", padx=6, pady=2)
        return f

    def _set_engine(self, key, e, second):
        if key in ("idle", "batt", "afr"):
            if second is None:
                return
            self.send(f"W {key} {self._v(e)} {self._v(second)}")
        else:
            self.send(f"W {key} {self._v(e)}")

    # Board presets: name -> (IAC pin, [O1..O7 pins])
    PRESETS = {
        "iobox2": (32, [25, 26, 27, 14, 13, 23, 33]),
        "iobox3": (19, [13, 12, 14, 27, 26, 25, 33]),
    }

    def _build_pins(self, parent):
        f = ttk.Frame(parent, padding=10)
        ttk.Label(f, text="Output + IAC GPIO map (stored in box NVS; CAN/analog/LED are "
                          "compile-time). Set per board once, then forget.").pack(anchor="w")

        g = ttk.LabelFrame(f, text=" Board preset ")
        g.pack(fill="x", pady=6)
        ttk.Label(g, text="Load preset:").grid(row=0, column=0, sticky="e", padx=6)
        self.pin_preset = ttk.Combobox(g, width=12, values=list(self.PRESETS),
                                       state="readonly")
        self.pin_preset.set("iobox3")
        self.pin_preset.grid(row=0, column=1, padx=6)
        ttk.Button(g, text="Fill fields", command=self._pin_preset_fill).grid(
            row=0, column=2, padx=6)
        ttk.Label(g, text="Then edit the pin fields below and press Apply.").grid(
            row=1, column=0, columnspan=4, sticky="w", padx=6, pady=2)

        g2 = ttk.LabelFrame(f, text=" Pin map ")
        g2.pack(fill="x")
        self.pin_iac = ttk.Entry(g2, width=5)
        self.pin_out = []
        for n in range(1, 8):
            ttk.Label(g2, text=f"O{n} GPIO:").grid(row=n, column=0, sticky="e", padx=6, pady=2)
            e = ttk.Entry(g2, width=5)
            e.grid(row=n, column=1, padx=2)
            self.pin_out.append(e)
        ttk.Label(g2, text="IAC GPIO:").grid(row=0, column=0, sticky="e", padx=6, pady=2)
        self.pin_iac.grid(row=0, column=1, padx=2)

        g3 = ttk.LabelFrame(f, text=" Actions ")
        g3.pack(fill="x", pady=6)
        ttk.Button(g3, text="Apply to box (P)", command=self._pin_apply).grid(
            row=0, column=0, **self._p())
        ttk.Button(g3, text="Read from box (P)", command=lambda: self.send("P")).grid(
            row=0, column=1, **self._p())
        ttk.Button(g3, text="Reset to iobox3 defaults (P RESET)",
                   command=lambda: self.send("P RESET")).grid(row=0, column=2, **self._p())
        ttk.Label(g3, text="Pins the box rejects: 0,1,2,3,4,5,6-11,20,24,28-31,34,35,36,39 "
                           "(boot/console/LED/CAN/dead/input-only).").grid(
            row=1, column=0, columnspan=4, sticky="w", padx=6, pady=2)

        self._pin_preset_fill()
        return f

    def _parse_pins(self, line):
        # matches: pins iac=19 o1=13 o2=12 ... o7=33
        if not line.startswith("pins iac="):
            return
        try:
            parts = line.split()
            iac = int(parts[1].split("=")[1])
            outs = [int(p.split("=")[1]) for p in parts[2:9]]
        except (IndexError, ValueError):
            return
        if len(outs) != 7:
            return
        self.pin_iac.delete(0, tk.END)
        self.pin_iac.insert(0, str(iac))
        for n in range(7):
            self.pin_out[n].delete(0, tk.END)
            self.pin_out[n].insert(0, str(outs[n]))

    def _pin_preset_fill(self):
        name = self.pin_preset.get()
        iac, outs = self.PRESETS[name]
        self.pin_iac.delete(0, tk.END)
        self.pin_iac.insert(0, str(iac))
        for n in range(7):
            self.pin_out[n].delete(0, tk.END)
            self.pin_out[n].insert(0, str(outs[n]))

    def _pin_apply(self):
        try:
            iac = int(self.pin_iac.get().strip())
            outs = [int(e.get().strip()) for e in self.pin_out]
        except ValueError:
            messagebox.showwarning("Pin map", "All pin fields must be integers.")
            return
        self.send(f"P IAC {iac}")
        for n, pin in enumerate(outs):
            self.send(f"P O{n + 1} {pin}")

    def _build_advanced(self, parent):
        f = ttk.Frame(parent, padding=10)

        g = ttk.LabelFrame(f, text=" 29-bit CAN responder ")
        g.pack(fill="x")
        ttk.Button(g, text="Enable (R1)", command=lambda: self.send("R1")).grid(row=0, column=0, **self._p())
        ttk.Button(g, text="Disable (R0)", command=lambda: self.send("R0")).grid(row=0, column=1, **self._p())
        ttk.Label(g, text="Box ID:").grid(row=0, column=2, sticky="e")
        self.resp_id = ttk.Spinbox(g, from_=1, to=14, width=4)
        self.resp_id.set(5)
        self.resp_id.grid(row=0, column=3)
        ttk.Button(g, text="Set (RB)", command=lambda: self.send(f"RB{self.resp_id.get()}")).grid(row=0, column=4, **self._p())
        ttk.Label(g, text="(matches TS can_poll_id0-3 = 5)").grid(row=1, column=0, columnspan=5, sticky="w", padx=6)

        g2 = ttk.LabelFrame(f, text=" Shift light ")
        g2.pack(fill="x", pady=8)
        ttk.Label(g2, text="Threshold RPM:").grid(row=0, column=0, sticky="e")
        self.shift_rpm = ttk.Entry(g2, width=7)
        self.shift_rpm.insert(0, "7000")
        self.shift_rpm.grid(row=0, column=1)
        ttk.Button(g2, text="Set (S)", command=lambda: self.send(f"S{self._v(self.shift_rpm)}")).grid(row=0, column=2, **self._p())

        ttk.Label(f, text="Hint: full command reference at the prompt below "
                          "(e.g. '?', 'I F', 'O3 T210').").pack(anchor="w", pady=8)
        return f

    # ---- helpers ----------------------------------------------------------
    @staticmethod
    def _p():
        return {"padx": 6, "pady": 3}

    @staticmethod
    def _v(entry):
        return entry.get().strip()

    def _set_analog(self, n, target, volts):
        if target == "0":
            self.send(f"A{n} 0")
        elif target == "F":
            self.send(f"A{n} F {volts}")
        else:
            self.send(f"A{n} {target} {volts}")

    def _mode_changed(self):
        wifi = self.mode.get() == "wifi"
        state = "normal" if wifi else "disabled"
        self.tcp_host.configure(state=state)
        self.tcp_port.configure(state=state)
        state = "disabled" if wifi else "readonly"
        self.port_cb.configure(state=state)

    def _refresh_ports(self):
        if serial is None:
            self.port_cb["values"] = []
            return
        ports = []
        for p in list_ports.comports():
            ports.append(f"{p.device}  {p.description}" if p.description else p.device)
        self.port_cb["values"] = ports
        if ports and not self.port_cb.get():
            self.port_cb.current(0)

    def _toggle_connect(self):
        if self.box.connected:
            self.box.disconnect()
            self._set_connected(False)
            return
        try:
            if self.mode.get() == "serial":
                sel = self.port_cb.get()
                if not sel:
                    raise RuntimeError("No USB port selected")
                port = sel.split()[0]
                self.box.connect_serial(port)
                self._log(f"[local] connected to {port} @ {BAUD}")
            else:
                host = self.tcp_host.get().strip() or DEFAULT_TCP_HOST
                port = int(self.tcp_port.get().strip() or DEFAULT_TCP_PORT)
                self.box.connect_tcp(host, port)
                self._log(f"[net] connected to {host}:{port}")
            self._set_connected(True)
        except Exception as e:
            messagebox.showerror("Connection failed", str(e))
            self._set_connected(False)

    def _set_connected(self, on):
        self.connect_btn.config(text="Disconnect" if on else "Connect")
        self.status_lbl.config(text=("Connected" if on else "Not connected"),
                               foreground=("#0a0" if on else "#888"))

    def send(self, cmd):
        cmd = cmd.strip()
        if not cmd:
            return
        if not self.box.connected:
            messagebox.showwarning("Not connected", "Connect to the box first.")
            return
        if self.box.send(cmd):
            self._log(f"> {cmd}")
            self.cmd_entry.delete(0, tk.END)

    def send_cmd(self):
        self.send(self.cmd_entry.get())

    def on_line(self, line):
        self._log(f"< {line}")
        self._parse_pins(line)

    def _log(self, text):
        self.console.configure(state="normal")
        self.console.insert(tk.END, text + "\n")
        self.console.see(tk.END)
        self.console.configure(state="disabled")

    def _poll(self):
        try:
            lines, done = self.box.poll()
            for ln in lines:
                self._log(f"< {ln}")
            if done:
                self.box.disconnect()
                self._set_connected(False)
                self._log("[!] link lost")
        except Exception:
            pass
        self.root.after(50, self._poll)


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
