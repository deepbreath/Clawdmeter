"""fish_voice.py — GuppyLM-driven autonomous speech for the Clawdmeter splash pet.

Pipeline:
  usage state → GuppyLM ONNX → fish text → edge-tts → PCM → Opus → BLE stream

Dependencies (install once):
  pip install onnxruntime tokenizers edge-tts miniaudio opuslib
  macOS: brew install opus
  Linux: sudo apt install libopus-dev

Model files (default path: siblings repo ../guppylm/docs/):
  model.onnx      (~10 MB, quantized uint8)
  tokenizer.json  (~169 KB)
"""

import asyncio
import logging
import os
import struct
import tempfile
import threading
import time
from pathlib import Path

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Configurable paths
# ---------------------------------------------------------------------------
_REPO_SIBLING = Path(__file__).parent.parent.parent / "guppylm" / "docs"
_HOME_CACHE   = Path.home() / ".local" / "share" / "guppylm"

GUPPYLM_MODEL     = Path(os.environ.get("GUPPYLM_MODEL",     str(_REPO_SIBLING / "model.onnx")))
GUPPYLM_TOKENIZER = Path(os.environ.get("GUPPYLM_TOKENIZER", str(_REPO_SIBLING / "tokenizer.json")))

# Fallback to home-cache if sibling path absent
if not GUPPYLM_MODEL.exists():
    GUPPYLM_MODEL     = _HOME_CACHE / "model.onnx"
    GUPPYLM_TOKENIZER = _HOME_CACHE / "tokenizer.json"

# BLE characteristic UUIDs
AUDIO_CHAR_UUID = "4c41555a-4465-7669-6365-000000000005"  # Opus/ADPCM audio frames
TEXT_CHAR_UUID  = "4c41555a-4465-7669-6365-000000000006"  # fish text for display

# Opus parameters — must match audio_stream.cpp on firmware side
OPUS_SAMPLE_RATE   = 16000
OPUS_FRAME_SAMPLES = 320      # 20 ms at 16 kHz
OPUS_BITRATE       = 8000     # bps — speech-quality, fits in BLE MTU

# Per-group prompts for GuppyLM — phrased as user questions Guppy responds to
_GROUP_PROMPTS = [
    "how are you feeling today",                             # idle: calm, restful
    "what do you notice in the water around you",            # normal: gentle activity
    "there are so many bubbles right now, how does that feel",  # active: excited
    "everything is vibrating and moving so fast, what is happening",  # heavy: intense
]

# ---------------------------------------------------------------------------
# GuppyLM ONNX inference
# ---------------------------------------------------------------------------

class _GuppyInfer:
    """Minimal autoregressive wrapper around the GuppyLM ONNX model."""

    def __init__(self):
        import numpy as np
        import onnxruntime as ort
        from tokenizers import Tokenizer

        self._np = np
        opts = ort.SessionOptions()
        opts.log_severity_level = 3  # suppress verbose
        self._sess = ort.InferenceSession(str(GUPPYLM_MODEL), sess_options=opts)
        tok = Tokenizer.from_file(str(GUPPYLM_TOKENIZER))
        tok.no_padding()
        tok.no_truncation()
        self._tok = tok
        self._im_end = tok.token_to_id("<|im_end|>")
        log.info("GuppyLM ONNX loaded from %s", GUPPYLM_MODEL)

    def generate(self, prompt: str, max_new_tokens: int = 48,
                 temperature: float = 0.75, top_k: int = 40) -> str:
        np = self._np
        text = f"<|im_start|>user\n{prompt}<|im_end|>\n<|im_start|>assistant\n"
        ids = list(self._tok.encode(text).ids)
        generated = []

        for _ in range(max_new_tokens):
            ids_arr = np.array([ids], dtype=np.int64)
            logits = self._sess.run(None, {"input_ids": ids_arr})[0][0, -1]

            if temperature > 0:
                logits = logits / temperature
            if top_k > 0:
                threshold = np.sort(logits)[-top_k]
                logits = np.where(logits >= threshold, logits, -1e9)

            exp_l = np.exp(logits - logits.max())
            probs = exp_l / exp_l.sum()
            next_id = int(np.random.choice(len(probs), p=probs))

            if next_id == self._im_end:
                break
            ids.append(next_id)
            generated.append(next_id)

        return self._tok.decode(generated).strip()


# ---------------------------------------------------------------------------
# TTS  (Windows SAPI via pyttsx3 — offline, no network required)
# ---------------------------------------------------------------------------

_sapi_lock   = threading.Lock()
_sapi_engine = None   # lazily initialised


def _sapi_init():
    global _sapi_engine
    if _sapi_engine is not None:
        return _sapi_engine
    import pyttsx3
    eng = pyttsx3.init()
    eng.setProperty('rate', 175)
    eng.setProperty('volume', 0.95)
    # Prefer Zira (en-US female); fall back to whatever is available
    for v in eng.getProperty('voices'):
        if 'zira' in v.name.lower():
            eng.setProperty('voice', v.id)
            break
    _sapi_engine = eng
    return eng


def _tts_to_pcm(text: str) -> bytes:
    """Windows SAPI → 16 kHz mono int16 PCM bytes (fully offline)."""
    import miniaudio
    tmp = tempfile.mktemp(suffix='.wav')
    try:
        with _sapi_lock:
            eng = _sapi_init()
            eng.save_to_file(text, tmp)
            eng.runAndWait()
        decoded = miniaudio.decode_file(
            tmp,
            output_format=miniaudio.SampleFormat.SIGNED16,
            nchannels=1,
            sample_rate=OPUS_SAMPLE_RATE)
        return bytes(decoded.samples)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


# ---------------------------------------------------------------------------
# IMA-ADPCM encoder  (pure Python, no external DLLs needed)
# ---------------------------------------------------------------------------

_STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28,
    31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337,
    371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
    1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
    3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767,
]
_IDX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def _encode_nibble(sample: int, pred: int, sidx: int) -> tuple[int, int, int]:
    step = _STEP_TABLE[sidx]
    diff = sample - pred
    sign = 8 if diff < 0 else 0
    if diff < 0:
        diff = -diff
    code = 0
    vpdiff = step >> 3
    if diff >= step:     code |= 4; diff -= step;      vpdiff += step
    if diff >= step >> 1: code |= 2; diff -= step >> 1; vpdiff += step >> 1
    if diff >= step >> 2: code |= 1;                    vpdiff += step >> 2
    code |= sign
    pred = pred - vpdiff if sign else pred + vpdiff
    pred = max(-32768, min(32767, pred))
    sidx = max(0, min(88, sidx + _IDX_TABLE[code & 0xF]))
    return code, pred, sidx


def _pcm_to_adpcm_frames(pcm_bytes: bytes) -> list[bytes]:
    """Encode 16-bit LE mono PCM → list of ADPCM BLE payload bytes.

    Each frame: [pred_hi][pred_lo][step_idx][nibble_bytes…] (163 bytes per 20 ms)
    The firmware prefixes 0x02 before sending to BLE; we return raw payload.
    """
    import struct
    samples = struct.unpack(f'<{len(pcm_bytes)//2}h', pcm_bytes)
    chunk = OPUS_FRAME_SAMPLES  # 320 samples per frame

    frames = []
    pred, sidx = 0, 0
    for start in range(0, len(samples), chunk):
        block = samples[start:start + chunk]
        if len(block) < chunk:
            block = block + (0,) * (chunk - len(block))

        frame_pred = pred
        frame_sidx = sidx
        nibbles = []
        for s in block:
            code, pred, sidx = _encode_nibble(s, pred, sidx)
            nibbles.append(code)

        # Pack nibbles low-nibble-first (matches decoder shift=0,4)
        data = bytearray()
        data.append((frame_pred >> 8) & 0xFF)
        data.append(frame_pred & 0xFF)
        data.append(frame_sidx & 0xFF)
        for i in range(0, len(nibbles), 2):
            lo = nibbles[i] & 0x0F
            hi = (nibbles[i + 1] & 0x0F) if i + 1 < len(nibbles) else 0
            data.append(lo | (hi << 4))
        frames.append(bytes(data))
    return frames


# ---------------------------------------------------------------------------
# Opus encoder (preferred when opuslib + libopus.so/.dylib/.dll available)
# ---------------------------------------------------------------------------

def _pcm_to_opus_frames(pcm_bytes: bytes) -> list[bytes]:
    import opuslib
    enc = opuslib.Encoder(OPUS_SAMPLE_RATE, 1, opuslib.APPLICATION_VOIP)
    enc.bitrate = OPUS_BITRATE
    chunk_bytes = OPUS_FRAME_SAMPLES * 2
    frames = []
    for offset in range(0, len(pcm_bytes), chunk_bytes):
        chunk = pcm_bytes[offset:offset + chunk_bytes]
        if len(chunk) < chunk_bytes:
            chunk = chunk + b"\x00" * (chunk_bytes - len(chunk))
        frames.append(enc.encode(chunk, OPUS_FRAME_SAMPLES))
    return frames


def _encode_audio(pcm_bytes: bytes) -> tuple[int, list[bytes]]:
    """Returns (frame_type, frames). Tries Opus first, falls back to ADPCM."""
    try:
        return 0x01, _pcm_to_opus_frames(pcm_bytes)
    except Exception:
        pass
    return 0x02, _pcm_to_adpcm_frames(pcm_bytes)


# ---------------------------------------------------------------------------
# BLE streaming
# ---------------------------------------------------------------------------

async def _stream_to_ble(client, frame_type: int, frames: list[bytes]) -> None:
    """Burst-stream encoded audio frames to ESP32, then EOS.

    No real-time pacing — the firmware's 300-frame ring buffer (6 s) absorbs all
    frames before playback starts.  Sending as fast as BLE allows avoids the
    Windows asyncio timer resolution (≥15 ms) that caused jitter with the old
    wall-clock pacing approach.
    """
    prefix = bytes([frame_type])
    for frame in frames:
        await client.write_gatt_char(AUDIO_CHAR_UUID, prefix + frame, response=False)
        await asyncio.sleep(0)   # yield to event loop; BLE stack self-paces

    await client.write_gatt_char(AUDIO_CHAR_UUID, bytes([0x00]), response=False)
    codec = "Opus" if frame_type == 0x01 else "ADPCM"
    log.info("audio stream: %d %s frames sent", len(frames), codec)


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

class FishVoice:
    """Manages autonomous fish-personality speech for the splash pet."""

    SPEAK_INTERVAL = 180   # seconds between unprompted utterances
    SPEAK_ON_CONNECT_DELAY = 5  # seconds after BLE connect before first speech

    def __init__(self):
        self._infer: _GuppyInfer | None = None
        self._available = False
        self._last_group = -1
        self._last_spoke = 0.0

    def try_load(self) -> bool:
        """Load GuppyLM model. Returns False gracefully if deps missing.

        opuslib is optional — falls back to pure-Python ADPCM if libopus DLL
        is not found (common on Windows without brew/apt).
        """
        if not GUPPYLM_MODEL.exists():
            log.warning("GuppyLM model not found at %s — fish voice disabled\n"
                        "  Copy model.onnx + tokenizer.json from guppylm/docs/ to that path",
                        GUPPYLM_MODEL)
            return False
        try:
            self._infer = _GuppyInfer()
            import pyttsx3   # noqa
            import miniaudio  # noqa
        except ImportError as e:
            log.warning("FishVoice disabled (missing dep: %s). "
                        "pip install onnxruntime tokenizers pyttsx3 miniaudio", e)
            return False

        try:
            import opuslib  # noqa
            log.info("FishVoice ready (Opus codec)")
        except Exception:
            log.info("FishVoice ready (ADPCM fallback — install libopus for better quality)")

        self._available = True
        return True

    @property
    def available(self) -> bool:
        return self._available

    def should_speak(self, usage_group: int) -> bool:
        """Return True when it's time for an autonomous utterance."""
        if not self._available:
            return False
        now = time.time()
        group_changed = usage_group != self._last_group
        interval_elapsed = (now - self._last_spoke) >= self.SPEAK_INTERVAL
        return group_changed or interval_elapsed

    async def speak(self, client, usage_group: int) -> None:
        """Generate and stream a fish utterance matching usage_group (0-3)."""
        if not self._available or self._infer is None:
            return

        # Check the audio char is present (Windows GATT cache can be stale)
        if client.services.get_characteristic(AUDIO_CHAR_UUID) is None:
            log.warning("audio char not found — fish voice skipped")
            return

        group = max(0, min(3, usage_group))
        prompt = _GROUP_PROMPTS[group]

        try:
            # 1. Generate fish text
            text = await asyncio.get_event_loop().run_in_executor(
                None, self._infer.generate, prompt)
            if not text:
                return
            log.info("fish [group=%d]: %s", group, text)

            # 2. TTS → 16 kHz mono PCM (offline SAPI, blocking → executor)
            pcm = await asyncio.get_event_loop().run_in_executor(
                None, _tts_to_pcm, text)

            # 3. Encode PCM → Opus (preferred) or ADPCM (fallback)
            frame_type, frames = await asyncio.get_event_loop().run_in_executor(
                None, _encode_audio, pcm)

            # 5. Display text on splash screen (before audio so it's ready)
            if client.services.get_characteristic(TEXT_CHAR_UUID) is not None:
                await client.write_gatt_char(
                    TEXT_CHAR_UUID, text.encode("utf-8", errors="replace"), response=False)

            # 6. Stream frames to ESP32 via BLE
            await _stream_to_ble(client, frame_type, frames)

            self._last_group = group
            self._last_spoke = time.time()

        except Exception as e:
            log.warning("fish_voice.speak error: %s", e)
