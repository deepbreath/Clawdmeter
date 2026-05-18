#!/usr/bin/env python3
"""
Generate fish-personality phrase WAV library for the SD card.

Runs GuppyLM ONNX offline → Windows SAPI TTS → saves 16 kHz mono WAV files.
Fully offline — no internet connection required.

Usage:
    pip install onnxruntime tokenizers pyttsx3 miniaudio
    python tools/gen_phrase_library.py
    python tools/gen_phrase_library.py --count 30 --out sd_phrases

SD layout produced:
    sd_phrases/phrases/
      group_0/001.wav … 020.wav   (idle)
      group_1/001.wav … 020.wav   (normal)
      group_2/001.wav … 020.wav   (active)
      group_3/001.wav … 020.wav   (heavy)
"""

import argparse
import os
import pathlib
import struct
import sys
import tempfile
import wave

# ---------------------------------------------------------------------------
# GuppyLM model paths
# ---------------------------------------------------------------------------

_REPO_SIBLING = pathlib.Path(__file__).parent.parent.parent / "guppylm" / "docs"
_HOME_CACHE   = pathlib.Path.home() / ".local" / "share" / "guppylm"

GUPPYLM_MODEL = pathlib.Path(
    os.environ.get("GUPPYLM_MODEL", str(_REPO_SIBLING / "model.onnx")))
GUPPYLM_TOKENIZER = pathlib.Path(
    os.environ.get("GUPPYLM_TOKENIZER", str(_REPO_SIBLING / "tokenizer.json")))
if not GUPPYLM_MODEL.exists():
    GUPPYLM_MODEL     = _HOME_CACHE / "model.onnx"
    GUPPYLM_TOKENIZER = _HOME_CACHE / "tokenizer.json"

_GROUP_PROMPTS = [
    "how are you feeling today",
    "what do you notice in the water around you",
    "there are so many bubbles right now, how does that feel",
    "everything is vibrating and moving so fast, what is happening",
]

# ---------------------------------------------------------------------------
# GuppyLM inference
# ---------------------------------------------------------------------------

class _Infer:
    def __init__(self):
        import numpy as np
        import onnxruntime as ort
        from tokenizers import Tokenizer
        self._np = np
        opts = ort.SessionOptions()
        opts.log_severity_level = 3
        self._sess = ort.InferenceSession(str(GUPPYLM_MODEL), sess_options=opts)
        tok = Tokenizer.from_file(str(GUPPYLM_TOKENIZER))
        tok.no_padding(); tok.no_truncation()
        self._tok = tok
        self._im_end = tok.token_to_id("<|im_end|>")

    def generate(self, prompt: str, max_new: int = 48,
                 temperature: float = 0.85, top_k: int = 50) -> str:
        np = self._np
        text = f"<|im_start|>user\n{prompt}<|im_end|>\n<|im_start|>assistant\n"
        ids  = list(self._tok.encode(text).ids)
        gen  = []
        for _ in range(max_new):
            arr    = np.array([ids], dtype=np.int64)
            logits = self._sess.run(None, {"input_ids": arr})[0][0, -1]
            if temperature > 0:
                logits = logits / temperature
            if top_k > 0:
                thr    = np.sort(logits)[-top_k]
                logits = np.where(logits >= thr, logits, -1e9)
            exp_l  = np.exp(logits - logits.max())
            probs  = exp_l / exp_l.sum()
            nid    = int(np.random.choice(len(probs), p=probs))
            if nid == self._im_end:
                break
            ids.append(nid); gen.append(nid)
        return self._tok.decode(gen).strip()


# ---------------------------------------------------------------------------
# TTS  (Windows SAPI via pyttsx3 — fully offline)
# ---------------------------------------------------------------------------

# pyttsx3 hangs on repeated runAndWait() calls in a loop (COM apartment issue).
# Workaround: run each TTS synthesis in a fresh subprocess.
_TTS_WORKER = r"""
import sys, os, pyttsx3, miniaudio, wave, io
text, out_path = sys.argv[1], sys.argv[2]
eng = pyttsx3.init()
eng.setProperty('rate', 175)
eng.setProperty('volume', 0.95)
for v in eng.getProperty('voices'):
    if 'zira' in v.name.lower():
        eng.setProperty('voice', v.id)
        break
tmp = out_path + '.raw.wav'
eng.save_to_file(text, tmp)
eng.runAndWait()
eng.stop()
dec = miniaudio.decode_file(tmp, output_format=miniaudio.SampleFormat.SIGNED16,
                             nchannels=1, sample_rate=16000)
pcm = bytes(dec.samples)
os.unlink(tmp)
buf = io.BytesIO()
with wave.open(buf, 'w') as wf:
    wf.setnchannels(1); wf.setsampwidth(2); wf.setframerate(16000)
    wf.writeframes(pcm)
open(out_path, 'wb').write(buf.getvalue())
"""

def _tts_to_wav_bytes(text: str) -> bytes:
    """Synthesise text → 16 kHz mono WAV bytes via subprocess (avoids pyttsx3 hang)."""
    import subprocess
    tmp_wav = tempfile.mktemp(suffix='.wav')
    tmp_py  = tempfile.mktemp(suffix='.py')
    try:
        pathlib.Path(tmp_py).write_text(_TTS_WORKER, encoding='utf-8')
        result = subprocess.run(
            [sys.executable, tmp_py, text, tmp_wav],
            capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            raise RuntimeError(result.stderr.strip())
        return pathlib.Path(tmp_wav).read_bytes()
    finally:
        for f in (tmp_py, tmp_wav):
            if os.path.exists(f):
                os.unlink(f)


# ---------------------------------------------------------------------------
# Generation
# ---------------------------------------------------------------------------

def generate_group(infer: _Infer, group: int, count: int,
                   out_dir: pathlib.Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    names  = ["idle", "normal", "active", "heavy"]
    prompt = _GROUP_PROMPTS[group]
    print(f"\nGroup {group} ({names[group]})  prompt: \"{prompt}\"")

    seen: set[str] = set()
    saved   = 0
    attempts = 0

    while saved < count and attempts < count * 5:
        attempts += 1
        text = infer.generate(prompt)
        if not text or text in seen:
            continue
        seen.add(text)

        try:
            wav = _tts_to_wav_bytes(text)
        except Exception as e:
            print(f"  TTS error: {e}")
            continue

        path = out_dir / f"{saved + 1:03d}.wav"
        path.write_bytes(wav)
        dur = len(wav) / (16000 * 2)
        print(f"  [{saved+1:02d}/{count}] {text!r}  ({len(wav)//1024} KB, {dur:.1f}s)")
        saved += 1

    if saved < count:
        print(f"  Warning: only generated {saved}/{count} unique phrases")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--count",  type=int, default=20,
                    help="Phrases per group (default 20)")
    ap.add_argument("--out",    default="sd_phrases",
                    help="Output base directory (default sd_phrases)")
    ap.add_argument("--groups", default="0,1,2,3",
                    help="Groups to generate, e.g. 0,1,2,3")
    args = ap.parse_args()

    if not GUPPYLM_MODEL.exists():
        print(f"ERROR: model not found at {GUPPYLM_MODEL}", file=sys.stderr)
        print("  Set GUPPYLM_MODEL env var or copy model.onnx there.")
        sys.exit(1)

    print(f"Loading GuppyLM from {GUPPYLM_MODEL} …")
    infer = _Infer()
    print("Model loaded.")

    base   = pathlib.Path(args.out) / "phrases"
    groups = [int(g) for g in args.groups.split(",")]

    for g in groups:
        generate_group(infer, g, args.count, base / f"group_{g}")

    print(f"\nDone.  Copy '{args.out}/phrases/' to the root of your SD card.")
    print("Expected layout: /phrases/group_0/001.wav … /phrases/group_3/020.wav")


if __name__ == "__main__":
    main()
