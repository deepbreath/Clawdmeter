#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Claude Controller" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).
"""

import asyncio
import getpass
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

try:
    from fish_voice import FishVoice
    _fish = FishVoice()
    _fish.try_load()
except ImportError:
    _fish = None  # type: ignore

DEVICE_NAME = "Claude Controller"
SERVICE_UUID    = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID    = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID   = "4c41555a-4465-7669-6365-000000000004"
AUDIO_CHAR_UUID = "4c41555a-4465-7669-6365-000000000005"

POLL_INTERVAL = 10
TICK = 1
SCAN_TIMEOUT = 8.0

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"

API_URL = "https://api.anthropic.com/v1/messages"
API_HEADERS_TEMPLATE = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}

OPENAI_API_URL = "https://api.openai.com/v1/chat/completions"
OPENAI_API_BODY = {
    "model": "gpt-4o-mini",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}
CODEX_USAGE_URL = "https://chatgpt.com/backend-api/wham/usage"
CODEX_CONFIG_PATH = Path.home() / ".codex" / "config.json"
CODEX_AUTH_PATH = Path.home() / ".codex" / "auth.json"
EVENT_FILE = Path.home() / ".config" / "claude-usage-monitor" / "event.json"

# Maps Claude Code hook event type strings to firmware sound_event_t values
_EVENT_TYPE_MAP = {
    "start":    1,  # EVT_START
    "complete": 2,  # EVT_COMPLETE
    "error":    3,  # EVT_ERROR
    "input":    4,  # EVT_INPUT
}


def read_pending_event() -> int:
    """Read and clear the event file. Returns EVT_* int (0 = none)."""
    if not EVENT_FILE.exists():
        return 0
    try:
        data = json.loads(EVENT_FILE.read_text())
        EVENT_FILE.unlink(missing_ok=True)
        return _EVENT_TYPE_MAP.get(data.get("type", ""), 0)
    except (json.JSONDecodeError, OSError):
        return 0


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "..."}}). Fall back to a
    regex match so unexpected shapes still work, and finally treat the
    blob as a raw token if nothing else matches.
    """
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "..."}
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _read_token_keychain() -> str | None:
    try:
        out = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
                "-a",
                getpass.getuser(),
                "-w",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except subprocess.CalledProcessError as e:
        log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return _extract_access_token(out.stdout)


def _read_token_file() -> str | None:
    try:
        raw = CREDENTIALS_PATH.read_text()
    except OSError as e:
        log(f"Error reading credentials: {e}")
        return None
    return _extract_access_token(raw)


def read_token() -> str | None:
    if sys.platform == "darwin":
        return _read_token_keychain()
    return _read_token_file()


def read_codex_credentials() -> tuple[str, str, str | None] | None:
    env_key = os.environ.get("OPENAI_API_KEY")
    if env_key:
        return "api_key", env_key, None
    if CODEX_CONFIG_PATH.exists():
        try:
            data = json.loads(CODEX_CONFIG_PATH.read_text())
            key = data.get("apiKey")
            if isinstance(key, str) and key:
                return "api_key", key, None
        except (json.JSONDecodeError, OSError):
            pass
    if CODEX_AUTH_PATH.exists():
        try:
            data = json.loads(CODEX_AUTH_PATH.read_text())
            key = data.get("OPENAI_API_KEY")
            if isinstance(key, str) and key:
                return "api_key", key, None
            tokens = data.get("tokens", {})
            token = tokens.get("access_token")
            if isinstance(token, str) and token:
                account_id = tokens.get("account_id")
                return "chatgpt", token, account_id if isinstance(account_id, str) else None
        except (json.JSONDecodeError, OSError):
            pass
    return None

def _parse_reset_seconds(value: str) -> int:
    """Parse OpenAI reset time string like '1m30.5s' or '30s' to integer seconds."""
    mins = re.search(r"(\d+)m", value)
    secs = re.search(r"(\d+(?:\.\d+)?)s", value)
    total = (int(mins.group(1)) * 60 if mins else 0) + (float(secs.group(1)) if secs else 0)
    return max(0, int(round(total)))


async def poll_openai_ratelimit_api(key: str) -> dict | None:
    headers = {"Authorization": f"Bearer {key}", "Content-Type": "application/json"}
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(OPENAI_API_URL, headers=headers, json=OPENAI_API_BODY)
    except httpx.HTTPError as e:
        log(f"Codex API call failed: {e}")
        return None

    def hdr(name: str, default: str = "0") -> str:
        return resp.headers.get(name, default)

    if resp.status_code not in (200, 429):
        log(f"Codex API unexpected status {resp.status_code}, skipping")
        return {"cx_ok": False}

    limit_tok = int(hdr("x-ratelimit-limit-tokens", "0") or "0")
    remaining_tok = int(hdr("x-ratelimit-remaining-tokens", "0") or "0")
    limit_req = int(hdr("x-ratelimit-limit-requests", "0") or "0")
    remaining_req = int(hdr("x-ratelimit-remaining-requests", "0") or "0")
    reset_tok = hdr("x-ratelimit-reset-tokens", "0s")
    reset_req = hdr("x-ratelimit-reset-requests", reset_tok)
    if limit_tok == 0 and limit_req == 0:
        log("Codex rate-limit headers absent (no quota info), skipping")
        return {"cx_ok": False}

    # Send remaining % (not used %) — display shows how much is left
    token_pct = round(remaining_tok * 100 / max(limit_tok, 1))
    req_pct = round(remaining_req * 100 / max(limit_req, 1))
    token_reset_s = _parse_reset_seconds(reset_tok)
    req_reset_s = _parse_reset_seconds(reset_req)

    return {
        "cx_ts": token_pct,
        "cx_tr": token_reset_s,
        "cx_rs": req_pct,
        "cx_rr": req_reset_s,
        "cx_ok": True,
    }



async def poll_codex_usage_api(token: str, account_id: str | None) -> dict | None:
    headers = {
        "Authorization": f"Bearer {token}",
        "Accept": "application/json",
        "User-Agent": "Mozilla/5.0",
    }
    if account_id:
        headers["chatgpt-account-id"] = account_id
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.get(CODEX_USAGE_URL, headers=headers)
    except httpx.HTTPError as e:
        log(f"Codex usage call failed: {e}")
        return None

    if resp.status_code != 200:
        log(f"Codex usage unexpected status {resp.status_code}, skipping")
        return {"cx_ok": False}

    try:
        data = resp.json()
    except json.JSONDecodeError:
        log("Codex usage response was not JSON, skipping")
        return {"cx_ok": False}

    rate_limit = data.get("rate_limit", {})
    primary = rate_limit.get("primary_window", {})
    secondary = rate_limit.get("secondary_window", {})
    if not primary and not secondary:
        log("Codex usage windows absent, skipping")
        return {"cx_ok": False}

    def remaining_pct(window: dict) -> int:
        try:
            return max(0, min(100, int(round(100 - float(window.get("used_percent", 0))))))
        except (TypeError, ValueError):
            return 0

    def reset_seconds(window: dict) -> int:
        reset = window.get("reset_after_seconds")
        if reset is not None:
            try:
                return max(0, int(round(float(reset))))
            except (TypeError, ValueError):
                pass
        try:
            return max(0, int(round(float(window.get("reset_at")) - time.time())))
        except (TypeError, ValueError):
            return -1

    return {
        "cx_ts": remaining_pct(primary),
        "cx_tr": reset_seconds(primary),
        "cx_rs": remaining_pct(secondary),
        "cx_rr": reset_seconds(secondary),
        "cx_ok": True,
    }


async def poll_codex_api(credentials: tuple[str, str, str | None]) -> dict | None:
    mode, secret, account_id = credentials
    if mode == "api_key":
        return await poll_openai_ratelimit_api(secret)
    return await poll_codex_usage_api(secret, account_id)

def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


def save_address(addr: str) -> None:
    SAVED_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
    SAVED_ADDR_FILE.write_text(addr)


async def scan_for_device():
    """Returns (address_str, BLEDevice) or None. Caller should pass BLEDevice to
    BleakClient directly — on Windows the address string alone loses context after scan."""
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name == DEVICE_NAME:
            log(f"Found: {d.address}")
            return d.address, d
    return None


async def poll_api(token: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None

    def hdr(name: str, default: str = "0") -> str:
        return resp.headers.get(name, default)

    now = time.time()

    def reset_minutes(reset_ts: str) -> int:
        try:
            r = float(reset_ts)
        except ValueError:
            return 0
        mins = (r - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def pct(util: str) -> int:
        try:
            return int(round(float(util) * 100))
        except ValueError:
            return 0

    payload: dict = {
        "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
        "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
        "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
        "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
        "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
        "ok": True,
    }

    codex_credentials = read_codex_credentials()
    if codex_credentials:
        codex = await poll_codex_api(codex_credentials)
        payload.update(codex if codex is not None else {"cx_ok": False})
    else:
        payload["cx_ok"] = False

    return payload


def _usage_group_from_payload(payload: dict) -> int:
    """Map session utilisation % to the same 4-group scale as the firmware."""
    s = payload.get("s", 0)
    if s < 10:   return 0  # idle
    if s < 30:   return 1  # normal
    if s < 60:   return 2  # active
    return 3               # heavy


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()
        self._fish_task: asyncio.Task | None = None
        self._connect_time = time.time()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        # Windows WinRT BLE initiates a security handshake when subscribing to
        # notifications, which ESP32 rejects (no bonding), causing an immediate
        # disconnect. Skip on Windows; 60s polling still delivers data.
        if sys.platform == "win32":
            log("Refresh subscription skipped on Windows (poll-only mode)")
            return
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
        except BleakError as e:
            log(f"Write failed: {e}")
            return False

        # Trigger autonomous fish speech after connect warm-up, if due
        if (_fish and _fish.available
                and time.time() - self._connect_time >= 5
                and (self._fish_task is None or self._fish_task.done())):
            group = _usage_group_from_payload(payload)
            if _fish.should_speak(group):
                self._fish_task = asyncio.create_task(
                    _fish.speak(self.client, group))

        return True


async def connect_and_run(address_or_device, stop_event: asyncio.Event) -> bool:
    """Connect to a device (str address or BLEDevice) and poll until disconnected or stopped.

    Returns True if the connection was used successfully (so the caller
    keeps the cached address), False if the connection failed and the
    cache should be invalidated.
    """
    label = address_or_device if isinstance(address_or_device, str) else address_or_device.address
    log(f"Connecting to {label}...")
    client = BleakClient(address_or_device)
    try:
        await client.connect(timeout=15.0)
    except (BleakError, asyncio.TimeoutError, asyncio.CancelledError) as e:
        log(f"Connection failed: {e}")
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    await asyncio.sleep(3.0)  # wait for Windows GATT discovery to complete

    # Verify our RX characteristic is present — Windows GATT cache can be stale
    # after a firmware reflash. Bail out so the caller reconnects with a fresh scan.
    rx_char = client.services.get_characteristic(RX_CHAR_UUID)
    if rx_char is None:
        log("RX characteristic not found (stale Windows GATT cache). "
            "Remove the device from Windows Bluetooth settings and reconnect.")
        try:
            await client.disconnect()
        except BleakError:
            pass
        return False

    session = Session(client)
    await session.setup_refresh_subscription()

    last_poll = 0.0
    last_payload: dict | None = None
    used_successfully = False
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            elapsed = now - last_poll

            # Fast path: pending event + cached payload → send immediately without API poll
            evt = read_pending_event()
            if evt and last_payload is not None:
                payload = dict(last_payload)
                payload["evt"] = evt
                log(f"Event fast-send: evt={evt}")
                if await session.write_payload(payload):
                    used_successfully = True
            elif session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL or evt:
                session.refresh_requested.clear()
                token = read_token()
                if not token:
                    log("No token; skipping poll")
                else:
                    payload = await poll_api(token)
                    if payload is not None:
                        if evt:
                            payload["evt"] = evt
                            log(f"Event injected: evt={evt}")
                        last_payload = payload
                        if await session.write_payload(payload):
                            last_poll = time.time()
                            used_successfully = True

            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
    finally:
        try:
            await client.disconnect()
        except (BleakError, asyncio.CancelledError):
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


async def main() -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    backoff = 1
    while not stop_event.is_set():
        address = load_cached_address()
        # On Windows, WinRT BLE locks a device after a failed direct connect,
        # causing subsequent scans to miss it. Always scan on Windows to get
        # a fresh BLEDevice object instead of connecting by raw MAC string.
        if address and sys.platform != "win32":
            connect_target = address
        else:
            result = await scan_for_device()
            if result:
                address, connect_target = result  # BLEDevice object
                save_address(address)
            else:
                log(f"Device not found, retrying in {backoff}s...")
                try:
                    await asyncio.wait_for(stop_event.wait(), timeout=backoff)
                except asyncio.TimeoutError:
                    pass
                backoff = min(backoff * 2, 60)
                continue

        ok = await connect_and_run(connect_target, stop_event)
        if not ok:
            log("Invalidating cached address")
            SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
        else:
            backoff = 1


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
