#!/usr/bin/env python3
"""Send a command to ESP32 and print the response. Usage: python serial_cmd.py sound 2"""
import sys, serial, time

PORT = "COM4"
BAUD = 115200

cmd = " ".join(sys.argv[1:]) if len(sys.argv) > 1 else "sound 2"

with serial.Serial(PORT, BAUD, timeout=3) as s:
    time.sleep(0.3)
    s.reset_input_buffer()
    s.write((cmd + "\n").encode())
    print(f"Sent: {cmd!r}")
    deadline = time.time() + 2.0
    lines = []
    while time.time() < deadline:
        line = s.readline()
        if line:
            lines.append(line.decode(errors="ignore").strip())
    if lines:
        for l in lines:
            print(f"  << {l}")
    else:
        print("  (no response — firmware may not be flashed yet)")
