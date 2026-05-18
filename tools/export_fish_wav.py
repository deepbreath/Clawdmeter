#!/usr/bin/env python3
"""
Export PCM arrays from firmware/src/fish_sounds.h → WAV files.
Copy the output WAV files to the root of your SD card.

Usage:
    python tools/export_fish_wav.py
"""

import re
import struct
import wave
import pathlib
import sys

HEADER = pathlib.Path(__file__).parent.parent / "firmware/src/fish_sounds.h"
OUT_DIR = pathlib.Path(__file__).parent.parent / "sd_audio"

SAMPLE_RATE = 16000
CHANNELS    = 1
BITS        = 16


def parse_array(text: str, name: str) -> list[int]:
    # Match:  fish_<name>[] = { ... };
    pat = rf'fish_{name}\s*\[\s*\d*\s*\]\s*=\s*\{{([^}}]+)\}}'
    m = re.search(pat, text, re.DOTALL)
    if not m:
        raise ValueError(f"Array fish_{name} not found in {HEADER}")
    raw = m.group(1)
    # Strip C-style comments
    raw = re.sub(r'//[^\n]*', '', raw)
    raw = re.sub(r'/\*.*?\*/', '', raw, flags=re.DOTALL)
    return [int(x.strip()) for x in raw.split(',') if x.strip()]


def write_wav(samples: list[int], path: pathlib.Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), 'w') as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(BITS // 8)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(struct.pack(f'<{len(samples)}h', *samples))
    dur = len(samples) / SAMPLE_RATE
    print(f"  {path.name}  {len(samples):>7} samples  {dur:.1f}s")


def main() -> None:
    if not HEADER.exists():
        print(f"ERROR: {HEADER} not found", file=sys.stderr)
        sys.exit(1)

    print(f"Reading {HEADER}")
    text = HEADER.read_text(encoding='utf-8', errors='replace')

    print(f"Writing WAV files to {OUT_DIR}/")
    for name in ('idle', 'norm', 'active', 'heavy'):
        samples = parse_array(text, name)
        write_wav(samples, OUT_DIR / f"fish_{name}.wav")

    print(f"\nDone.  Copy {OUT_DIR}/*.wav to the root of your SD card.")


if __name__ == '__main__':
    main()
