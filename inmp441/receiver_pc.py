#!/usr/bin/env python3
# ============================================================
#  Cliente con interfaz grafica para la captura del ESP32 INMP441
#  - Se conecta al servidor TCP del ESP32 y queda a la espera.
#  - Cada vez que pulsas el boton fisico, el ESP32 envia una
#    grabacion; la GUI la detecta, la guarda como WAV y queda
#    lista para la siguiente.
#  - Numeracion automatica de archivos (captura_001.wav, ...).
# ============================================================

import os
import time
import wave
import queue
import socket
import threading
import tkinter as tk
from tkinter import ttk, filedialog, scrolledtext


class CapturaGUI:
    def __init__(self, root):
        self.root = root
        root.title("Captura INMP441 - ESP32")
        root.minsize(520, 460)

        self.q = queue.Queue()
        self.sock = None
        self.worker = None
        self.stop_evt = threading.Event()
        self.connected = False
        self.count = 0

        self._build_ui()
        self.root.after(100, self._poll_queue)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # ---------------- UI ----------------
    def _build_ui(self):
        pad = {"padx": 6, "pady": 4}

        params = ttk.LabelFrame(self.root, text="Conexion")
        params.pack(fill="x", padx=10, pady=(10, 6))

        self.ip_var       = tk.StringVar(value="10.109.146.120")
        self.port_var     = tk.StringVar(value="8000")
        self.sr_var       = tk.StringVar(value="8000")
        self.idle_var     = tk.StringVar(value="0.8")
        self.maxdur_var   = tk.StringVar(value="30")
        self.folder_var   = tk.StringVar(value=os.getcwd())
        self.base_var     = tk.StringVar(value="captura")

        def row(r, label, var, width=18):
            ttk.Label(params, text=label).grid(row=r, column=0, sticky="e", **pad)
            ttk.Entry(params, textvariable=var, width=width).grid(row=r, column=1, sticky="w", **pad)

        row(0, "IP del ESP32:", self.ip_var)
        row(1, "Puerto:", self.port_var, 10)
        row(2, "Sample rate (Hz):", self.sr_var, 10)
        row(3, "Silencio fin (s):", self.idle_var, 10)
        row(4, "Duracion max (s):", self.maxdur_var, 10)
        row(5, "Nombre base:", self.base_var)

        ttk.Label(params, text="Carpeta:").grid(row=6, column=0, sticky="e", **pad)
        fframe = ttk.Frame(params)
        fframe.grid(row=6, column=1, sticky="w", **pad)
        ttk.Entry(fframe, textvariable=self.folder_var, width=30).pack(side="left")
        ttk.Button(fframe, text="...", width=3, command=self._pick_folder).pack(side="left", padx=4)

        # Botones conectar / desconectar
        btns = ttk.Frame(self.root)
        btns.pack(fill="x", padx=10, pady=4)
        self.btn_conn = ttk.Button(btns, text="Conectar", command=self._connect)
        self.btn_conn.pack(side="left")
        self.btn_disc = ttk.Button(btns, text="Desconectar", command=self._disconnect, state="disabled")
        self.btn_disc.pack(side="left", padx=6)

        # Estado
        estado = ttk.Frame(self.root)
        estado.pack(fill="x", padx=10, pady=4)
        self.status_var = tk.StringVar(value="Desconectado")
        self.count_var  = tk.StringVar(value="Grabaciones: 0")
        ttk.Label(estado, text="Estado:").pack(side="left")
        self.status_lbl = ttk.Label(estado, textvariable=self.status_var,
                                     font=("TkDefaultFont", 11, "bold"))
        self.status_lbl.pack(side="left", padx=6)
        ttk.Label(estado, textvariable=self.count_var).pack(side="right")

        ttk.Label(self.root,
                  text="Ganancia y duracion de grabacion se ajustan en el firmware "
                       "(GAIN_SHIFT, REC_SECONDS).",
                  foreground="gray").pack(anchor="w", padx=12)

        # Registro
        logf = ttk.LabelFrame(self.root, text="Registro")
        logf.pack(fill="both", expand=True, padx=10, pady=(6, 10))
        self.log = scrolledtext.ScrolledText(logf, height=10, state="disabled", wrap="word")
        self.log.pack(fill="both", expand=True, padx=4, pady=4)

    def _pick_folder(self):
        d = filedialog.askdirectory(initialdir=self.folder_var.get() or os.getcwd())
        if d:
            self.folder_var.set(d)

    # ---------------- Conexion ----------------
    def _connect(self):
        if self.connected:
            return
        try:
            ip      = self.ip_var.get().strip()
            port    = int(self.port_var.get())
            self._sr     = int(self.sr_var.get())
            self._idle   = float(self.idle_var.get())
            self._maxdur = float(self.maxdur_var.get())
            self._folder = self.folder_var.get().strip() or os.getcwd()
            self._base   = self.base_var.get().strip() or "captura"
        except ValueError:
            self._log("Parametros invalidos: revisa puerto / sample rate / tiempos.")
            return

        os.makedirs(self._folder, exist_ok=True)
        self.stop_evt.clear()
        self.connected = True
        self.btn_conn.config(state="disabled")
        self.btn_disc.config(state="normal")
        self.status_var.set("Conectando...")
        self.worker = threading.Thread(target=self._worker, args=(ip, port), daemon=True)
        self.worker.start()

    def _disconnect(self):
        self.stop_evt.set()
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass

    # ---------------- Hilo de recepcion ----------------
    def _worker(self, ip, port):
        try:
            self.sock = socket.create_connection((ip, port), timeout=10)
        except OSError as e:
            self.q.put(("status", "Error de conexion"))
            self.q.put(("log", f"No se pudo conectar a {ip}:{port} -> {e}"))
            self.q.put(("disconnected", None))
            return

        self.q.put(("status", "Conectado - esperando boton"))
        self.q.put(("log", f"Conectado a {ip}:{port}. Pulsa el boton del ESP32 para grabar."))
        self.sock.settimeout(self._idle)

        buf = bytearray()
        recording = False

        while not self.stop_evt.is_set():
            try:
                chunk = self.sock.recv(65536)
                if not chunk:
                    self.q.put(("log", "El ESP32 cerro la conexion."))
                    break
                if not recording:
                    recording = True
                    buf = bytearray()
                    self.q.put(("status", "Grabando..."))
                buf.extend(chunk)
                if len(buf) / (self._sr * 2) >= self._maxdur:
                    self._save(buf)
                    buf, recording = bytearray(), False
                    self.q.put(("status", "Conectado - esperando boton"))
            except socket.timeout:
                if recording and buf:
                    self._save(buf)
                    buf, recording = bytearray(), False
                    self.q.put(("status", "Conectado - esperando boton"))
            except OSError:
                break

        if recording and buf:
            self._save(buf)
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass
        self.q.put(("disconnected", None))

    # ---------------- Guardado ----------------
    def _next_path(self):
        i = 1
        while True:
            p = os.path.join(self._folder, f"{self._base}_{i:03d}.wav")
            if not os.path.exists(p):
                return p
            i += 1

    def _save(self, data):
        if len(data) % 2:
            data = data[:-1]
        path = self._next_path()
        with wave.open(path, "wb") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(self._sr)
            wf.writeframes(data)
        n = len(data) // 2
        self.q.put(("log", f"Guardado: {os.path.basename(path)}  "
                           f"({n} muestras, {n / self._sr:.2f} s)"))
        self.q.put(("count", None))

    # ---------------- GUI <- hilo ----------------
    def _poll_queue(self):
        try:
            while True:
                kind, val = self.q.get_nowait()
                if kind == "status":
                    self.status_var.set(val)
                elif kind == "log":
                    self._log(val)
                elif kind == "count":
                    self.count += 1
                    self.count_var.set(f"Grabaciones: {self.count}")
                elif kind == "disconnected":
                    self._on_disconnected()
        except queue.Empty:
            pass
        self.root.after(100, self._poll_queue)

    def _on_disconnected(self):
        self.connected = False
        self.status_var.set("Desconectado")
        self.btn_conn.config(state="normal")
        self.btn_disc.config(state="disabled")

    def _log(self, msg):
        ts = time.strftime("%H:%M:%S")
        self.log.config(state="normal")
        self.log.insert("end", f"[{ts}] {msg}\n")
        self.log.see("end")
        self.log.config(state="disabled")

    def _on_close(self):
        self._disconnect()
        self.root.after(150, self.root.destroy)


def main():
    root = tk.Tk()
    CapturaGUI(root)
    root.mainloop()


if __name__ == "__main__":
    main()
