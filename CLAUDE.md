# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Current hardware bring-up memory

Before changing SD, USB MSC, ES8311/I2S audio, or button GPIOs, read `docs/bringup-notes.md`.

Known-good values from the successful debug session:

- Display reset is GPIO39; touch reset is GPIO40.
- Onboard TF card is SDMMC 1-bit: `CLK=2`, `CMD=1`, `D0=3`; it is not SPI.
- USB MSC mode is most reliably entered with the serial command `msc`.
- USB MSC requires `RTC_NOINIT_ATTR` magic across soft reboot and an explicit `USB.begin()` after `USBMSC.begin(...)`.
- Audio is 16 kHz / 16-bit PCM. The ES8311 MCLK is 4.096 MHz (`16000 * 256`), not a 4 kHz sample rate.
- ES8311/I2S pins: `MCLK=42`, `BCLK=9`, `WS=45`, `DOUT=8`, `PA_EN=46`.
- Do not configure GPIO9 or GPIO10 as buttons; GPIO9 is I2S BCLK and GPIO10 is mic DIN. Reconfiguring them caused silent audio.
- Current anti-clipping values: ES8311 DAC volume register `0x32=0xB2`, PCM gain `2/3`, test tone gain `0.45`.

# Project context

ESP32-S3 firmware for a desk-side Claude Code usage monitor on a **Waveshare ESP32-S3-Touch-AMOLED-2.16** board (480×480 square AMOLED). Connects to a host daemon over BLE; daemon polls Anthropic API for usage data.

## Hardware (critical pins)

- Display: **CO5300** AMOLED via QSPI (CS=12, SCLK=38, SDIO0..3=4..7, RST=2)
- Touch: **CST9220** via I2C (SDA=15, SCL=14, INT=11, addr=0x5A)
- PMU: **AXP2101** on same I2C bus (addr=0x34) — battery, USB VBUS, PWR button IRQ
- IMU: **QMI8658** on same I2C bus (addr=0x6B) — accelerometer for auto-rotation
- Buttons: GPIO 0 (left → Space/voice-mode), GPIO 18 (right → Shift+Tab/mode-toggle), AXP PKEY (middle → cycle screens; on splash → cycle animations)

## Architecture

```text
main.cpp          — setup(), loop(), button polling (left→Space, right→Shift+Tab, mid→cycle), rotation flash
display_cfg.h     — pin defines, extern object decls
ui.{h,cpp}        — 3-screen UI (splash, usage, bluetooth); splash is touch-toggled, usage↔bluetooth via mid button
splash.{h,cpp}    — 20×20 pixel-art animation engine, 24× upscale to 480×480
imu.{h,cpp}       — accelerometer-driven rotation tracker (returns 0..3)
power.{h,cpp}     — AXP2101 wrapper (battery %, charging, VBUS, PWR button)
touch.{h,cpp}     — minimal tap detector → ui_toggle_splash() (Usage/Splash) or ble_clear_bonds() (BT reset zone)
ble.{h,cpp}       — NimBLE peripheral: custom data service + HID keyboard
usage_rate.{h,cpp}— tracks rate-of-change of session_pct over a rolling window; returns group 0..3 (idle→heavy)
                    used by splash.cpp to select animation mood tier
theme.h           — design tokens (single source of truth for all UI colors — Anthropic-inspired dark/AMOLED palette)
data.h            — UsageData struct
icons.h           — icon arrays. Battery (5×) are RGB565A8 with alpha; rest are raw RGB565.
logo.h            — 80×80 RGB565 logo
font_*.c          — pre-compiled LVGL 9 bitmap fonts (Tiempos 34/56px, Styrene 12/14/16/20/24/28/48px, Mono 18/32px)
splash_animations.h — generated, do not hand-edit
```

## Build / flash

```bash
pio run -d firmware                   # build only
./flash.sh                            # build + flash (Linux, auto-detects /dev/ttyACM0)
./flash-mac.sh                        # build + flash (macOS, auto-detects /dev/cu.usbmodem*)
./flash-mac.sh /dev/cu.usbmodem1101   # explicit port
```

Direct PlatformIO: `pio run -d firmware -t upload --upload-port /dev/ttyACM0`

Use `/home/hermann/.platformio/penv/bin/pio` if `pio` isn't on PATH. Device shows up as `/dev/ttyACM0` (Espressif USB JTAG); no boot-mode gymnastics needed.

## QA your own UI changes — don't ask the user

The firmware ships a `screenshot` serial command that dumps the LVGL framebuffer over `/dev/ttyACM0`. `./screenshot.sh out.png /dev/ttyACM0` captures a 480×480 PNG. **Use this on every UI iteration** — Read the PNG with the Read tool, verify the change visually, iterate.

The boot screen is `SCREEN_SPLASH` and only advances on a physical button press, so a fresh flash will sit on the splash. To screenshot the screen you're actually editing without asking the user to press a button, **temporarily change the default boot screen** in `main.cpp` (search for `ui_show_screen(SCREEN_SPLASH);`) to `SCREEN_USAGE` or `SCREEN_BLUETOOTH`, do your iteration, then revert before committing.

## Critical gotchas

1. **CO5300 cannot rotate.** Its MADCTL only supports axis flips, not column/row exchange. Rotation is done by **CPU pixel remapping in `my_flush_cb`** in main.cpp. We use **PARTIAL render mode with strip rotation** (small 480×40 strips, fast). On rotation change → AMOLED brightness flash → force redraw.
2. **OPI PSRAM** required: `board_build.arduino.memory_type = qio_opi` in platformio.ini. Without this, `MALLOC_CAP_SPIRAM` returns NULL and the screen is black.
3. **pioarduino platform required.** GFX Library for Arduino needs Arduino Core 3.x (`esp32-hal-periman.h`), not the 2.x that standard `espressif32` ships. We pin `pioarduino/platform-espressif32` 55.03.38-1.
4. **LVGL 9 font patching.** `lv_font_conv` outputs LVGL 8 format. Must remove `#if LVGL_VERSION_MAJOR >= 8` guards, drop `.cache` field, add `.release_glyph`, `.kerning`, `.static_bitmap`, `.fallback`, `.user_data`. Without patching, fonts render invisible.
5. **Touch reading must be centralized.** CST9220's `getPoint()` does a full I2C transaction. Calling it from multiple places consumed each other's data and broke input. `touch_read()` is called once per loop in main.cpp; both LVGL `my_touch_cb` and `touch.cpp` read from shared `touch_pressed/touch_x/touch_y` state.
6. **CO5300 needs even-aligned flush regions.** `rounder_cb` enforces this.
7. **Touch `setSwapXY(true)` and `setMirrorXY(true, false)`** are the empirically-correct values for default rotation 0. IMU rotation logic doesn't change touch mapping (it does CPU-side rotation of the rendered pixels, so LVGL still thinks the display is portrait at 0°).
8. **LVGL RGB565A8 is planar.** `w*h` RGB565 pixels followed by `w*h` alpha bytes; `data_size = w*h*3`, `stride = w*2`. Use `init_icon_dsc_rgb565a8()` for icons that overlap non-uniform backgrounds (e.g. battery over splash). Lucide source PNGs are black-on-transparent — converter must tint to white or icons render invisible. See `tools/png_to_lvgl.js`.

## Icons

`tools/png_to_lvgl.js <input.png> <symbol> [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]` converts an alpha PNG to RGB565A8. Default tint is white (`0xFFFFFF`) — necessary for Lucide PNGs. Splice output into `firmware/src/icons.h` and use `init_icon_dsc_rgb565a8()` in ui.cpp. Currently only the 5 battery icons use this format; the rest are still raw RGB565 baked over the panel background, fine because they live inside opaque zones.

## Splash animations

13 × 20×20 pixel-art creature animations sourced from
[claudepix.vercel.app](https://claudepix.vercel.app). Pipeline:

```bash
node tools/scrape_claudepix.js  # → tools/claudepix_data/*.json
node tools/convert_to_c.js      # → firmware/src/splash_animations.h
```

Each animation has a per-animation 10-color RGB565 palette. Cell values 0..9 index it. Default boot screen. Animations are grouped by `usage_rate_group()` (0=idle, 1=normal, 2=active, 3=heavy) so the splash mood tracks how hard Claude is working.

## User profile / preferences

See `~/.claude/projects/.../memory/` files for persistent context (user is an embedded-beginner senior dev, brand-conscious, prefers iterative UI refinement, dislikes me authoring my own art when third-party assets are intended). Always read those memory files at session start.

## Daemon / host side

Two daemon variants — Linux (bash) and macOS (Python):

| Platform | Script | Service |
|---|---|---|
| Linux | `daemon/claude-usage-daemon.sh` | systemd user service via `./install.sh` |
| macOS | `daemon/claude_usage_daemon.py` | LaunchAgent via `./install-mac.sh` |

Linux reads OAuth token from `~/.claude/.credentials.json`. macOS reads from the Keychain (`Claude Code-credentials`).

Run Linux: `systemctl --user start claude-usage-daemon`. The unit file's `ExecStart` is the absolute path — repoint it when switching between worktrees.

**Discovery & resilience:**

- Connects by name (`"Claude Controller"`) on first run, caches resolved MAC at `~/.config/claude-usage-monitor/ble-address`. ESP32 BLE addresses are factory-burned per-chip, so swapping any board invalidates the cache.
- On connect failure: cache is dropped AND device is removed from bluez (`bluetoothctl remove`) so the next scan won't re-pick a dead MAC.
- `POLL_INTERVAL=60`, `TICK=5`. Inner loop wakes every 5s to detect disconnects fast; polls Anthropic when 60s elapsed OR when ESP fires a refresh request.

**GATT characteristics on service `4c41555a-...0001`:**

- `...0002` RX — daemon writes JSON usage payload here.
- `...0003` TX — firmware notifies ack/nack (daemon doesn't subscribe).
- `...0004` REQ — firmware fires `0x01` notify in `onSubscribe` if `has_received_data` is false. Daemon subscribes via `setsid bash -c "stdbuf -oL dbus-monitor … | awk …"`; awk drops a flag file the inner loop picks up. See the `feedback_dbus_monitor_pipe` memory for the three subtle gotchas (pipe buffering, busctl-exits race, `wait` blocking on pipeline jobs).
