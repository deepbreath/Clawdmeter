#!/usr/bin/env python3
"""Reset ESP32 and capture 6 seconds of boot log."""
import serial, time

PORT = "COM4"
BAUD = 115200

with serial.Serial(PORT, BAUD, timeout=0.1) as s:
    # Toggle DTR to reset the ESP32
    s.dtr = False
    time.sleep(0.1)
    s.dtr = True
    print("=== Reset, capturing boot log ===")
    deadline = time.time() + 6.0
    while time.time() < deadline:
        line = s.readline()
        if line:
            print(line.decode(errors="ignore").rstrip())
    print("=== Done ===")
