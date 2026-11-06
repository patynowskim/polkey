#!/usr/bin/env python3
"""Tkinter front-end for the PolKey BL616 tools (flash, boot info, eFuse, CTAP tests).

Run from the repo dev shell (tkinter is in shell.nix):
    nix-shell --run 'python3 hw/bl616/tools/gui.py'
"""
import glob
import os
import queue
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import ttk

HERE = os.path.dirname(os.path.abspath(__file__))
PORTS = sorted(set(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))) or ["/dev/ttyACM0"]

TOOLS = [
    ("Flash FIDO2 app", ["bash", "flash_fidokey.sh"], True),
    ("Read info (boot + eFuse)", [sys.executable, "readinfo.py"], True),
    ("Read eFuse", [sys.executable, "readefuse.py"], True),
    ("CTAPHID smoke test", [sys.executable, "ctaphid_test.py"], False),
    ("CTAP2 end-to-end", [sys.executable, "ctap2_test.py"], False),
]

ERR_HINTS = ("Traceback", "Error", "error", "FAIL", "not found", "refused")


class ToolGui:
    POLL_MS = 100

    def __init__(self):
        self.q = queue.Queue()
        self.proc = None
        self.root = root = tk.Tk()
        root.title("PolKey BL616 toolbench")
        root.minsize(680, 580)
        self._build_banner(root)
        self._build_controls(root)
        self._build_console(root)
        root.protocol("WM_DELETE_WINDOW", self._quit)

    def _build_banner(self, root):
        frame = ttk.Frame(root, padding=8)
        frame.pack(fill="x")
        ttk.Label(frame, text="PolKey BL616 toolbench", font="sans-serif 14 bold").pack(pady=(6, 0))
        ttk.Label(frame, text="UPDATE mode: hold the UPDATE button (behind HDMI) while plugging USB in.",
                  foreground="#666").pack()

    def _build_controls(self, root):
        frame = ttk.Frame(root, padding=8)
        frame.pack(fill="x")

        ttk.Label(frame, text="Port").grid(row=0, column=0, sticky="w", pady=2)
        self.port = tk.StringVar(value="/dev/ttyACM0")
        cb = ttk.Combobox(frame, textvariable=self.port, width=26, values=PORTS)
        cb.grid(row=0, column=1, columnspan=2, sticky="w", padx=6)
        ttk.Button(frame, text="Refresh", command=lambda: cb.configure(
            values=sorted(set(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*")))
        )).grid(row=0, column=3, padx=6, sticky="w")

        for i, (label, cmd, needs_port) in enumerate(TOOLS, start=1):
            ttk.Button(frame, text=label, width=26, command=lambda c=cmd, p=needs_port: self.run(
                c + ([self.port.get()] if p else [])
            )).grid(row=i, column=0, columnspan=2, sticky="w", pady=3)

        ttk.Separator(frame, orient="vertical").grid(row=0, column=4, rowspan=len(TOOLS) + 1, sticky="ns", padx=12)
        ttk.Button(frame, text="Stop", command=self.stop).grid(row=0, column=5, pady=2, sticky="ew")
        ttk.Button(frame, text="Clear", command=self.clear).grid(row=1, column=5, pady=2, sticky="ew")

    def _build_console(self, root):
        frame = ttk.Frame(root, padding=8)
        frame.pack(fill="both", expand=True)
        text = tk.Text(frame, wrap="word", height=18, background="#101418", foreground="#d7dde3",
                       insertbackground="#d7dde3", font="monospace 9")
        text.pack(fill="both", expand=True)
        text.tag_configure("err", foreground="#ff8080")
        text.tag_configure("ok", foreground="#7ee787")
        self.text = text

    def _append(self, line):
        text = self.text
        text.insert("end", line + "\n")
        tag = "err" if any(s in line for s in ERR_HINTS) else ("ok" if "PASS" in line or "OK" in line else None)
        if tag:
            text.tag_add(tag, "end-2lines", "end-1lines")
        text.see("end")

    def run(self, cmd):
        if self.proc:
            self._append("another tool is still running (pid %d)" % self.proc.pid)
            return
        self._append("$ " + " ".join(cmd))
        try:
            self.proc = proc = subprocess.Popen(
                cmd, cwd=HERE, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, bufsize=1)
        except OSError as e:
            self.proc = None
            self._append("cannot start %s: %s" % (cmd[0], e))
            return
        threading.Thread(target=self._reader, args=(proc,), daemon=True).start()
        self.root.after(self.POLL_MS, self._pump)

    def _reader(self, proc):
        for line in proc.stdout:
            self.q.put(line)
        self.q.put(None)

    def _pump(self):
        while True:
            try:
                line = self.q.get_nowait()
            except queue.Empty:
                break
            if line is not None:
                self._append(line.rstrip("\n"))
        if self.proc:
            code = self.proc.poll()
            if code is not None and self.q.empty():
                self.proc = None
                self._append("exit code %d" % code)
                return
            self.root.after(self.POLL_MS, self._pump)

    def stop(self):
        if self.proc:
            self.proc.terminate()
            self._append("terminated pid %d" % self.proc.pid)

    def clear(self):
        self.text.delete("1.0", "end")

    def _quit(self):
        self.stop()
        self.root.destroy()

    def mainloop(self):
        self.root.mainloop()


def main():
    try:
        ToolGui().mainloop()
    except tk.TclError as e:
        sys.exit("cannot open display: %s" % e)


if __name__ == "__main__":
    main()
