# Clawdmeter

[English](README.md) | [中文](README.zh.md)

A small ESP32 dashboard I made for my desk to keep an eye on Claude Code usage.

It runs on a [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786) and pairs with my laptop over Bluetooth, the splash screen plays pixel-art Clawd animations that get
busier when your usage rate climbs. The two side buttons send Space and
Shift+Tab over BLE HID for Claude Code's voice mode and mode-toggle shortcuts.

|              Usage meter              |              Clawd animation screen              |
| :-----------------------------------: | :----------------------------------------------: |
| ![Usage meter](assets/demo.jpeg) | ![Clawd animation screen](assets/demo.gif) |

The Clawd animations come from [claudepix](https://claudepix.vercel.app), [@amaanbuilds](https://x.com/amaanbuilds)'s library of pixel-art Clawd sprites, check it out, it's lovely.

## What's new

- **Language switch in the docs:** this README now links to a Chinese version at the top, and the Chinese README links back here.
- **Codex usage screen:** the daemon can read Codex/OpenAI credentials and send token/request remaining percentages to the device. The middle button now cycles through Splash, Claude usage, Codex, and Bluetooth screens.
- **Wi-Fi status:** the firmware reads Wi-Fi config from the SD card and shows connection quality on the dashboard.
- **Recorder and backup:** double-click the right side button (GPIO 18) or use the `rec` serial command to record WAV files to SD; pending recordings can upload automatically after Wi-Fi connects.
- **Sound notifications:** Claude Code hook events can trigger onboard sounds for start, completion, input-needed, and error states.
- **Host audio streaming:** optional fish voice/audio streaming can send Opus or ADPCM frames to the firmware over BLE, with Wi-Fi TCP as a faster path when configured.

## Screens

The device boots into the splash and stays there until you press the middle (PWR) button, which cycles between Usage, Codex, and Bluetooth. Tap the screen anywhere (except the Reset zone on the Bluetooth screen) to flip back to the splash; tap again to dismiss it.

|              Splash               |              Usage              |                Bluetooth                |
| :-------------------------------: | :-----------------------------: | :-------------------------------------: |
| ![Splash](screenshots/splash.png) | ![Usage](screenshots/usage.png) | ![Bluetooth](screenshots/bluetooth.png) |
|   Splash; touch-toggle anytime    | Session and weekly utilization  |    Connection status and bond reset     |

While the splash is up, the middle button cycles animations instead of screens. The firmware also auto-rotates every 20 s within the current usage-rate group, so a long stretch on the splash isn't just one Clawd on loop.

The Codex screen uses the same dashboard style and shows token/request remaining percentages when the daemon can find an OpenAI API key or Codex OAuth token.

## Hardware

- [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786) - ESP32-S3R8, 2.16" 480×480 AMOLED (CO5300 QSPI), CST9220 cap touch, AXP2101 PMU + Li-Po battery, QMI8658 IMU
- USB-C cable for flashing firmware and charging
- 3.7V Li-Po battery (MX1.25 2-pin connector, optional)
- microSD card (optional, required for recording, Wi-Fi config, and upload backup)

## Prerequisites

- Linux (tested on Ubuntu) or macOS
- [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation/index.html)
- Linux: `curl`, `bluetoothctl`, `busctl` (BlueZ Bluetooth stack)
- macOS: `python3` (the installer sets up a venv with `bleak` and `httpx`)
- Claude Code with an active subscription
- Optional Codex/OpenAI quota display: `OPENAI_API_KEY`, `~/.codex/config.json`, or `~/.codex/auth.json`

## macOS installation

The macOS host pieces — Python daemon, LaunchAgent, and flash helper — were ported by [Chris Davidson (@lorddavidson)](https://github.com/lorddavidson). Thanks Chris!

### Flash the firmware

```bash
./flash-mac.sh                       # auto-detects /dev/cu.usbmodem*
./flash-mac.sh /dev/cu.usbmodem1101  # or pass an explicit USB serial port
```

### Pair the device

After flashing, open **System Settings → Bluetooth** and click *Connect* next to "Clawdmeter". The daemon will discover it on its next scan (~30 s).

### Install the daemon

The daemon reads your Claude OAuth token from the macOS Keychain (service `Claude Code-credentials`), polls usage every 60 s, and pushes it to the display over BLE.

```bash
./install-mac.sh
```

The installer creates a Python venv in `daemon/.venv/`, installs `bleak` and `httpx`, renders a LaunchAgent into `~/Library/LaunchAgents/com.user.claude-usage-daemon.plist`, and loads it. The first run is launched interactively so macOS prompts for Bluetooth permission.

Useful commands:

```bash
launchctl list | grep claude-usage                                          # check it's running
tail -F ~/Library/Logs/claude-usage-daemon.out.log                          # live logs
launchctl unload ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist  # stop
launchctl load -w ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist # start
```

## Windows installation

### Flash the firmware

Install [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation/index.html), then:

```powershell
pio run -d firmware -t upload --upload-port COM4
```

Replace `COM4` with your device's port (check Device Manager under "Espressif USB JTAG/serial debug unit").

### Pair the device

Open **Settings → Bluetooth & devices**, find "Claude Controller" and click *Connect*. Once paired, remove it from Windows Bluetooth settings — the daemon connects directly via bleak and Windows auto-reconnecting will block the daemon from scanning.

### Install the daemon

The daemon reads your Claude OAuth token from `~/.claude/.credentials.json`, polls usage every 10 s, and pushes it to the display over BLE.

If `OPENAI_API_KEY`, `~/.codex/config.json`, or `~/.codex/auth.json` is available, the daemon also sends Codex/OpenAI rate-limit data to the Codex screen.

**Dependencies:**

```powershell
pip install bleak httpx
```

**Run manually:**

```powershell
$env:PYTHONUTF8=1; python daemon/claude_usage_daemon.py
```

**Build a standalone exe:**

```powershell
pip install pyinstaller
pyinstaller --onefile `
  --hidden-import=bleak.backends.winrt `
  --hidden-import=bleak.backends.winrt.scanner `
  --hidden-import=bleak.backends.winrt.client `
  --name claude-usage-daemon `
  daemon/claude_usage_daemon.py
```

Output: `dist\claude-usage-daemon.exe`. Re-run this command after any daemon update; the exe path stays the same.

**Auto-start at login:**

```powershell
$startup = [Environment]::GetFolderPath("Startup")
$wsh = New-Object -ComObject WScript.Shell
$sc = $wsh.CreateShortcut("$startup\ClaudeUsageDaemon.lnk")
$sc.TargetPath = "E:\workspace-github\Clawdmeter\dist\claude-usage-daemon.exe"
$sc.WorkingDirectory = "E:\workspace-github\Clawdmeter"
$sc.WindowStyle = 7
$sc.Save()
```

Update the path to match wherever you cloned the repo.

### Sound notifications

The device plays audio notifications via the onboard ES8311 codec when Claude Code events fire. Wire up the hooks in `~/.claude/settings.json`:

```json
"hooks": {
  "Stop": [{
    "hooks": [{"type": "command", "command": "python -c \"import json,pathlib; p=pathlib.Path.home()/'.config'/'claude-usage-monitor'/'event.json'; p.parent.mkdir(parents=True,exist_ok=True); p.write_text(json.dumps({'type':'complete'}))\""}]
  }],
  "Notification": [{
    "hooks": [{"type": "command", "command": "python -c \"import json,pathlib; p=pathlib.Path.home()/'.config'/'claude-usage-monitor'/'event.json'; p.parent.mkdir(parents=True,exist_ok=True); p.write_text(json.dumps({'type':'input'}))\""}]
  }]
}
```

| Event | Sound | Trigger |
|-------|-------|---------|
| `complete` | finish.wav | Claude finishes a response |
| `input` | waiInput.wav | Claude is waiting for input |
| `start` | start.wav | Claude starts working |
| `error` | error.wav | Error occurred |

Optional fish voice support lives in `daemon/fish_voice.py`. It can synthesize short spoken updates and stream compressed audio frames to the firmware over BLE, or over Wi-Fi TCP when `FISH_AUDIO_HOST` is configured.

To replace sounds, put WAV files in a folder and run the converter:

```powershell
python -c "
import wave, audioop, struct
SRC = 'path/to/your/wavs'
# ... see tools/convert_sounds.py
"
```

### Debug tools

```powershell
python serial_boot_log.py          # reset ESP32 and capture boot log
python serial_cmd.py sound 2       # trigger a sound directly (1=start 2=finish 3=error 4=input)
python serial_cmd.py screenshot    # capture framebuffer to PNG
```

## Linux installation

### Flash the firmware

```bash
cd firmware
pio run -t upload --upload-port /dev/ttyACM0
```

### Pair the device

After flashing, the device advertises as "Claudemeter". Pair it once:

```bash
# Scan for the device
bluetoothctl scan le

# When "Claude Controller" appears, pair and trust it
bluetoothctl pair F4:12:FA:C0:8F:E5    # use your device's MAC
bluetoothctl trust F4:12:FA:C0:8F:E5
```

The MAC address is shown on the Bluetooth screen — press the middle (PWR) button to cycle to it.

### Install the daemon

The daemon polls your Claude usage every 60 seconds and sends it to the display over BLE.

```bash
./install.sh
systemctl --user start claude-usage-daemon
```

Check status: `systemctl --user status claude-usage-daemon`

View logs: `journalctl --user -u claude-usage-daemon -f`

## How it works

1. The daemon reads your Claude Code OAuth token from `~/.claude/.credentials.json`.
2. It makes a minimal API call to `api.anthropic.com/v1/messages` — one token of Haiku, basically free.
3. The usage numbers come straight out of the response headers (`anthropic-ratelimit-unified-5h-utilization` and friends).
4. The daemon connects to the ESP32 over BLE and writes a JSON payload to the GATT RX characteristic.
5. The firmware parses it and updates the LVGL dashboard.
6. The firmware also tracks the rate of change of session % over a 5-minute window and picks splash animations from the matching mood group.
7. If Codex/OpenAI credentials are available, the daemon includes Codex token/request quota data in the same payload.
8. Claude Code hook events can be folded into the next payload to play onboard notification sounds.
9. The side buttons are independent of the dashboard data path: left sends Space as BLE HID, and the right button sends Shift+Tab on single press or toggles recording on double-click.

## Recording, Wi-Fi, and SD backup

Recordings are stored on the SD card as WAV files plus JSON metadata. Use `Key3 / GPIO18` double-click or the serial `rec` command to start/stop recording. The latest recording can be replayed with `recplay`, analyzed with `recanalyze`, and uploaded with `upload`.

Wi-Fi and recorder upload settings live on the SD card:

```json
// /config/wifi.json
{
  "ssid": "your-wifi",
  "password": "your-password",
  "device_id": "clawdmeter-001"
}
```

```json
// /config/recorder.json
{
  "upload_url": "http://raspberrypi.local:8080/upload",
  "auth_token": "optional-token"
}
```

Pending recordings live in `/recordings/pending/`; successfully uploaded files move to `/recordings/sent/`. See [docs/recording-backup.md](docs/recording-backup.md) for the SD layout and upload notes.

## Physical buttons

The board has three side buttons. Left and right do the same thing on every screen; the middle button is screen-aware.

| Button           | GPIO         | Function                                                       |
| ---------------- | ------------ | -------------------------------------------------------------- |
| **Left**         | GPIO 0       | Hold to send Space (Claude Code voice-mode push-to-talk)       |
| **Middle** (PWR) | AXP2101 PKEY | Cycle screens (Usage ↔ Bluetooth); on splash, cycle animations |
| **Right**        | GPIO 18      | Press to send Shift+Tab (Claude Code mode toggle)              |

Space and Shift+Tab go out as standard BLE HID keyboard reports, so they trigger in whatever window has focus on the paired host — not just Claude Code.

Current firmware note: the middle button cycles Usage > Codex > Bluetooth, and the right button starts/stops recording on double-click while keeping single-click Shift+Tab.

## BLE protocol

The device advertises a custom GATT service alongside the standard HID keyboard service:

|                            | UUID                                   |
| -------------------------- | -------------------------------------- |
| **Data Service**           | `4c41555a-4465-7669-6365-000000000001` |
| RX Characteristic (write)  | `4c41555a-4465-7669-6365-000000000002` |
| TX Characteristic (notify) | `4c41555a-4465-7669-6365-000000000003` |
| REQ Characteristic (notify) | `4c41555a-4465-7669-6365-000000000004` |
| AUDIO Characteristic (write) | `4c41555a-4465-7669-6365-000000000005` |
| **HID Service**            | `00001812-0000-1000-8000-00805f9b34fb` |

JSON payload format (written to RX):

```json
{ "s": 45, "sr": 120, "w": 28, "wr": 7200, "st": "allowed", "ok": true, "cx_ts": 80, "cx_tr": 30, "cx_rs": 95, "cx_ok": true, "evt": 2 }
```

Fields: `s` = Claude session %, `sr` = Claude session reset (minutes), `w` = Claude weekly %, `wr` = Claude weekly reset (minutes), `st` = Claude status, `ok` = success flag, `cx_ts` = Codex token remaining %, `cx_tr` = Codex token reset (seconds), `cx_rs` = Codex request remaining %, `cx_ok` = Codex data valid, `evt` = optional sound event.

## Recompiling fonts

The `firmware/src/font_*.c` files are pre-compiled LVGL bitmap fonts.

```bash
npm install -g lv_font_conv
```

Generate each one (one at a time — `lv_font_conv` doesn't like loop-driven invocations) with `--no-compress` (required for LVGL 9):

```bash
# Tiempos Text (titles, 56px)
lv_font_conv --font assets/TiemposText-400-Regular.otf -r 0x20-0x7E \
  --size 56 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_tiempos_56.c --lv-include "lvgl.h"

# Styrene B (large numbers 48, panel labels 28, small text 24, minimal 20)
for size in 48 28 24 20; do
  lv_font_conv --font assets/StyreneB-Regular.otf -r 0x20-0x7E \
    --size $size --format lvgl --bpp 4 --no-compress \
    -o firmware/src/font_styrene_${size}.c --lv-include "lvgl.h"
done

# DejaVu Sans Mono (32px, with spinner Unicode chars)
lv_font_conv --font assets/DejaVuSansMono.ttf \
  -r 0x20-0x7E,0xB7,0x2026,0x2722,0x2733,0x2736,0x273B,0x273D \
  --size 32 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_mono_32.c --lv-include "lvgl.h"
```

**Important:** `lv_font_conv` v1.5.3 outputs LVGL 8 format. Each generated file must be patched for LVGL 9 compatibility:

1. Remove `#if LVGL_VERSION_MAJOR >= 8` guards around `font_dsc` and the font struct
2. Remove the `.cache` field from `font_dsc`
3. Add `.release_glyph = NULL`, `.kerning = 0`, `.static_bitmap = 0` to the font struct
4. Add `.fallback = NULL`, `.user_data = NULL` to the font struct

Without these patches, fonts compile but render as invisible.

## Converting Lucide icons

The UI uses a small set of [Lucide](https://lucide.dev) icons (bluetooth + battery states) converted to RGB565 / RGB565A8 C arrays for LVGL.

```bash
node tools/png_to_lvgl.js assets/icon_bluetooth_48.png icon_bluetooth_data ICON_BLUETOOTH_WIDTH ICON_BLUETOOTH_HEIGHT
```

Default tint is white (`0xFFFFFF`); Lucide PNGs ship as black-on-transparent and would render invisible against the dark UI without it. Pass `--no-tint` for pre-coloured artwork like the logo. Battery icons use RGB565A8 (alpha plane) so they blend cleanly over the splash; the rest are baked RGB565 over the panel colour. Paste the converter output into `firmware/src/icons.h`.

## Splash animations

The animations come from [claudepix.vercel.app](https://claudepix.vercel.app),
a library of Clawd sprites. `tools/scrape_claudepix.js` evaluates the
site's JavaScript in a Node VM to pull out frame data and palettes, then
`tools/convert_to_c.js` turns everything into RGB565 C arrays and writes
`firmware/src/splash_animations.h`.

To re-pull (e.g. when the source library updates):

```bash
node tools/scrape_claudepix.js
node tools/convert_to_c.js
pio run -d firmware -t upload
```

See `tools/README.md` for details.

## Credits

- Pixel-art Clawd animation by [@amaanbuilds](https://x.com/amaanbuilds), sourced from [claudepix.vercel.app](https://claudepix.vercel.app). Frame data and palettes scraped + converted by the tooling in `tools/`.
- Lucide icon set ([lucide.dev](https://lucide.dev), MIT) for bluetooth and battery UI glyphs.
- Anthropic brand fonts (Tiempos Text, Styrene B) — see licensing warning below.

## Licensing gray area warning

The software in this repository uses and adheres to the Anthropic brand guidelines and uses the same proprietary fonts that Anthropic has a license for but this software uses without permission as well as using assets from Anthropic such as the copyrighted Clawd mascot so even though the code in this repo is non-proprietary I will not license it myself under a copyleft license since this repo includes proprietary fonts and copyrighted assets. Please be aware of this if you fork or copy the code from this repo. **You have been warned!**
