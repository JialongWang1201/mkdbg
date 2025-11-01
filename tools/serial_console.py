#!/usr/bin/env python3
import threading
import queue
import os
import tkinter as tk
from tkinter import ttk, scrolledtext

try:
    import serial
    import serial.tools.list_ports
except Exception as exc:  # pragma: no cover
    raise SystemExit("pyserial not installed. Run: pip3 install pyserial") from exc


class SerialUI(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("VM32 Serial Console")
        self.geometry("780x520")
        self.resizable(True, True)

        self.ser = None
        self.reader_thread = None
        self.stop_reader = threading.Event()
        self.rx_queue = queue.Queue()
        self.connecting = False

        self._build_ui()
        self._refresh_ports()
        self.after(50, self._drain_rx)

    def _build_ui(self):
        top = ttk.Frame(self)
        top.pack(fill="x", padx=10, pady=8)

        ttk.Label(top, text="Port:").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=28, state="normal")
        self.port_combo.pack(side="left", padx=6)

        ttk.Label(top, text="Baud:").pack(side="left", padx=(12, 0))
        self.baud_var = tk.StringVar(value="115200")
        self.baud_combo = ttk.Combobox(top, textvariable=self.baud_var, width=10, state="readonly")
        self.baud_combo["values"] = ("9600", "19200", "38400", "57600", "115200", "230400")
        self.baud_combo.pack(side="left", padx=6)

        ttk.Button(top, text="Refresh", command=self._refresh_ports).pack(side="left", padx=6)
        self.connect_btn = ttk.Button(top, text="Connect", command=self._toggle_connect)
        self.connect_btn.pack(side="left", padx=6)

        self.text = scrolledtext.ScrolledText(self, wrap="word", height=22)
        self.text.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        self.text.configure(state="disabled")

        bottom = ttk.Frame(self)
        bottom.pack(fill="x", padx=10, pady=(0, 10))

        ttk.Label(bottom, text="Input:").pack(side="left")
        self.input_var = tk.StringVar()
        self.input_entry = ttk.Entry(bottom, textvariable=self.input_var)
        self.input_entry.pack(side="left", fill="x", expand=True, padx=6)
        self.input_entry.bind("<Return>", self._send_line)

        ttk.Button(bottom, text="Send", command=self._send_line).pack(side="left", padx=6)
        ttk.Button(bottom, text="Clear", command=self._clear_text).pack(side="left")

    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        expanded = []
        for p in ports:
            expanded.append(p)
            if p.startswith("/dev/cu."):
                tty = "/dev/tty." + p.split("/dev/cu.", 1)[1]
                if tty not in expanded:
                    expanded.append(tty)
            if p.startswith("/dev/tty."):
                cu = "/dev/cu." + p.split("/dev/tty.", 1)[1]
                if cu not in expanded:
                    expanded.append(cu)
        if not expanded:
            expanded = ports
        existing = [p for p in expanded if os.path.exists(p)]
        if existing:
            expanded = existing
        self.port_combo["values"] = expanded
        if expanded and not self.port_var.get():
            self.port_var.set(expanded[0])

    def _toggle_connect(self):
        if self.connecting:
            return
        if self.ser is None:
            self._connect_async()
        else:
            self._disconnect()

    def _connect_async(self):
        port = self.port_var.get().strip()
        if not port:
            self._append_text("No port selected.\n")
            return
        try:
            baud = int(self.baud_var.get())
        except ValueError:
            self._append_text("Invalid baud.\n")
            return

        self.connecting = True
        self.connect_btn.configure(text="Connecting...", state="disabled")

        def worker():
            try:
                ser = serial.Serial(
                    port,
                    baudrate=baud,
                    timeout=0.1,
                    write_timeout=0.2,
                    rtscts=False,
                    dsrdtr=False,
                )
            except Exception as exc:
                self.after(0, self._connect_failed, f"Connect failed: {exc}\n")
                return
            self.after(0, self._finish_connect, ser, port, baud)

        threading.Thread(target=worker, daemon=True).start()

    def _finish_connect(self, ser, port, baud):
        self.ser = ser
        self.stop_reader.clear()
        self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader_thread.start()
        self.connect_btn.configure(text="Disconnect", state="normal")
        self.connecting = False
        self._append_text(f"Connected to {port} @ {baud}.\n")

    def _connect_failed(self, msg):
        self.connecting = False
        self.connect_btn.configure(text="Connect", state="normal")
        self._append_text(msg)

    def _disconnect(self):
        self.stop_reader.set()
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self.connect_btn.configure(text="Connect", state="normal")
        self._append_text("Disconnected.\n")

    def _reader_loop(self):
        while not self.stop_reader.is_set():
            try:
                if self.ser is None:
                    break
                data = self.ser.read(256)
                if data:
                    text = data.decode(errors="replace")
                    self.rx_queue.put(text)
            except Exception:
                break

    def _drain_rx(self):
        try:
            while True:
                text = self.rx_queue.get_nowait()
                self._append_text(text)
        except queue.Empty:
            pass
        self.after(50, self._drain_rx)

    def _append_text(self, s: str):
        self.text.configure(state="normal")
        self.text.insert("end", s)
        self.text.see("end")
        self.text.configure(state="disabled")

    def _clear_text(self):
        self.text.configure(state="normal")
        self.text.delete("1.0", "end")
        self.text.configure(state="disabled")

    def _send_line(self, _event=None):
        if self.ser is None:
            self._append_text("Not connected.\n")
            return
        line = self.input_var.get()
        if line == "":
            return
        try:
            self.ser.write((line + "\r\n").encode())
        except Exception as exc:
            self._append_text(f"Send failed: {exc}\n")
        else:
            self._append_text(f">> {line}\n")
        self.input_var.set("")


if __name__ == "__main__":
    app = SerialUI()
    app.mainloop()
