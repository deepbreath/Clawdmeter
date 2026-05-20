#include <Arduino.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include "display_cfg.h"
#include "data.h"
#include "ui.h"
#include "ble.h"
#include "power.h"
#include "imu.h"
#include "splash.h"
#include "usage_rate.h"
#include "sound.h"
#include "audio_stream.h"
#include "sd_card.h"
#include "sd_phrases.h"
#include "usb_msc.h"
#include "recorder.h"
#include "wifi_manager.h"
#include "wifi_backup.h"
#include <esp_heap_caps.h>

// Physical buttons (global, screen-independent):
//   KEY3_IO18  (GPIO 18) — double-click to start/stop recorder; hold at boot → USB Drive Mode (MSC)
//   AXP PKEY   (Key1/PWR) — short-press cycles screens/animations; long-press is power
// GPIO9/GPIO10 are reserved for I2S audio and must not be configured as buttons.
#define KEY3_IO18  18

// ---- Hardware objects ----
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0 /* rotation */,
    LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
TouchDrvCST92xx touch;
XPowersPMU pmu;
SensorQMI8658 imu;

static UsageData usage = {};
static CodexData codex = {};

// ---- Touch interrupt + shared state ----
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;
static volatile bool     touch_data_ready = false;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

static void touch_read() {
    if (!touch_data_ready) return;
    touch_data_ready = false;

    int16_t tx[5], ty[5];
    uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
    if (n > 0) {
        touch_pressed = true;
        touch_x = (uint16_t)tx[0];
        touch_y = (uint16_t)ty[0];
    } else {
        touch_pressed = false;
    }
}

// ---- LVGL draw buffers (PSRAM-backed, partial render) ----
#define BUF_LINES 40
static uint16_t *buf1 = nullptr;
static uint16_t *buf2 = nullptr;
// rot_buf for strip rotation — max size is 480×480 (full invalidation case)
// but typical partial strips are much smaller
static uint16_t *rot_buf = nullptr;

// LVGL tick callback
static uint32_t my_tick(void) {
    return millis();
}

// Rotate a w×h strip and compute destination coordinates on the 480×480 display.
// src pixels are in row-major order for the rectangle (sx, sy, w, h).
// Output goes to rot_buf in row-major order for the destination rectangle.
static void rotate_strip(const uint16_t *src, int32_t w, int32_t h,
                         int32_t sx, int32_t sy, uint8_t r,
                         int32_t *dx, int32_t *dy, int32_t *dw, int32_t *dh) {
    const int S = LCD_WIDTH;  // 480

    switch (r) {
    case 1: { // 90° CW: (x,y) -> (S-1-y, x)
        *dw = h; *dh = w;
        *dx = S - sy - h;
        *dy = sx;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                // src(x,y) -> dst(h-1-y, x)
                rot_buf[x * h + (h - 1 - y)] = src[y * w + x];
            }
        }
        break;
    }
    case 2: { // 180°: (x,y) -> (S-1-x, S-1-y)
        *dw = w; *dh = h;
        *dx = S - sx - w;
        *dy = S - sy - h;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                rot_buf[(h - 1 - y) * w + (w - 1 - x)] = src[y * w + x];
            }
        }
        break;
    }
    case 3: { // 270° CW: (x,y) -> (y, S-1-x)
        *dw = h; *dh = w;
        *dx = sy;
        *dy = S - sx - w;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                // src(x,y) -> dst(y, w-1-x)
                rot_buf[(w - 1 - x) * h + y] = src[y * w + x];
            }
        }
        break;
    }
    default:
        *dx = sx; *dy = sy; *dw = w; *dh = h;
        break;
    }
}

// LVGL flush callback — rotates partial strips and writes to display
static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    uint16_t *src = (uint16_t*)px_map;
    uint8_t r = imu_get_rotation();

    if (r == 0) {
        gfx->draw16bitRGBBitmap(area->x1, area->y1, src, w, h);
    } else {
        int32_t dx, dy, dw, dh;
        rotate_strip(src, w, h, area->x1, area->y1, r, &dx, &dy, &dw, &dh);
        gfx->draw16bitRGBBitmap(dx, dy, rot_buf, dw, dh);
    }
    lv_display_flush_ready(disp);
}

// CO5300 requires even-aligned flush regions
static void rounder_cb(lv_event_t* e) {
    lv_area_t *area = (lv_area_t*)lv_event_get_param(e);
    area->x1 = area->x1 & ~1;
    area->y1 = area->y1 & ~1;
    area->x2 = area->x2 | 1;
    area->y2 = area->y2 | 1;
}

// LVGL touch callback
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    if (touch_pressed) {
        data->point.x = touch_x;
        data->point.y = touch_y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Parse a JSON line into UsageData (and optionally CodexData and sound event)
static bool parse_json(const char* json, UsageData* out, CodexData* cx_out, sound_event_t* evt_out = nullptr) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->ok = doc["ok"] | false;
    out->valid = true;

    if (cx_out) {
        float cx_ts = doc["cx_ts"] | -1.0f;
        cx_out->token_pct    = (cx_ts >= 0.0f) ? cx_ts : 0.0f;
        cx_out->token_reset_s = doc["cx_tr"] | -1;
        cx_out->req_pct      = doc["cx_rs"] | 0.0f;
        cx_out->ok           = doc["cx_ok"] | false;
        cx_out->valid        = cx_out->ok;
    }

    if (evt_out) *evt_out = (sound_event_t)(doc["evt"] | 0);

    return true;
}

// Serial command buffer
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
    const uint32_t w = LCD_WIDTH, h = LCD_HEIGHT;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n", (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");

    heap_caps_free(sbuf);
}

static void scan_i2c_bus() {
    Serial.println("i2c: scanning");
    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        uint8_t err = Wire.endTransmission();
        if (err == 0) {
            Serial.printf("i2c: found 0x%02X", addr);
            if (addr == AXP2101_ADDR) Serial.print(" AXP2101");
            if (addr == CST9220_ADDR) Serial.print(" CST9220");
            if (addr == ES8311_ADDR) Serial.print(" ES8311");
            if (addr == ES7210_ADDR) Serial.print(" ES7210");
            Serial.println();
            found++;
        }
    }
    Serial.printf("i2c: %d device(s)\n", found);
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) {
                send_screenshot();
            } else if (strcmp(cmd_buf, "msc") == 0) {
                Serial.println("[MSC] serial command received - rebooting into USB drive mode");
                sd_end();
                Serial.flush();
                delay(200);
                usb_msc_request();
            } else if (strcmp(cmd_buf, "tone") == 0) {
                sound_debug_tone();
            } else if (strncmp(cmd_buf, "sound", 5) == 0) {
                int n = (cmd_pos > 5) ? atoi(cmd_buf + 6) : 2;
                if (n < 1 || n > 4) n = 2;
                Serial.printf("sound_play(%d)\n", n);
                sound_play((sound_event_t)n);
            } else if (strcmp(cmd_buf, "rec") == 0) {
                recorder_toggle();
            } else if (strncmp(cmd_buf, "recmode", 7) == 0) {
                const char* mode = (cmd_pos > 8) ? (cmd_buf + 8) : "";
                if (!recorder_set_capture_mode(mode)) {
                    Serial.printf("recorder: capture mode=%s; use: recmode auto|slot0|slot1|slot2|slot3|swap0|swap1|swap2|swap3|mix|tdmraw|es8311\n",
                                  recorder_capture_mode());
                }
            } else if (strncmp(cmd_buf, "recrate", 7) == 0) {
                uint32_t rate = (cmd_pos > 8) ? (uint32_t)atoi(cmd_buf + 8) : 0;
                if (!recorder_set_sample_rate(rate)) {
                    Serial.printf("recorder: sample rate=%lu; use: recrate 16000|48000\n",
                                  (unsigned long)recorder_sample_rate());
                }
            } else if (strcmp(cmd_buf, "recplay") == 0) {
                if (recorder_is_recording()) {
                    Serial.println("recorder: play blocked while recording");
                    cmd_pos = 0;
                    continue;
                }
                const char* path = recorder_last_wav_path();
                if (path) {
                    Serial.printf("recorder: play %s\n", path);
                    if (!sd_play_wav(path)) Serial.printf("recorder: play failed (dma=%u)\n", (unsigned)sd_dma_free());
                } else {
                    Serial.println("recorder: no recording yet");
                }
            } else if (strcmp(cmd_buf, "recanalyze") == 0) {
                recorder_analyze_last_wav();
            } else if (strcmp(cmd_buf, "upload") == 0) {
                wifi_backup_request_upload();
            } else if (strcmp(cmd_buf, "wifi") == 0) {
                wifi_manager_print_status();
                wifi_backup_print_status();
            } else if (strcmp(cmd_buf, "mem") == 0) {
                Serial.printf("mem: free=%u internal=%u dma=%u psram=%u min_free=%u\n",
                              (unsigned)ESP.getFreeHeap(),
                              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                              (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                              (unsigned)ESP.getMinFreeHeap());
            } else if (strcmp(cmd_buf, "sd") == 0) {
                sd_print_status();
            } else if (strcmp(cmd_buf, "i2c") == 0) {
                scan_i2c_bus();
            }
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

void setup() {
    // Sample Key3 (GPIO18) at boot for USB MSC mode.
    // GPIO0 is an ESP32-S3 strapping pin (causes ROM download mode if held at
    // reset), so we use GPIO18, the accessible side button.
    pinMode(KEY3_IO18, INPUT_PULLUP);
    delay(10);
    bool msc_boot_held = (digitalRead(KEY3_IO18) == LOW);

    Serial.begin(115200);

    // No delay(300) yet — MSC boot must reach sd_msc_init() before Windows
    // completes USB enumeration; otherwise it caches "no media" and never
    // retries. The delay is added below for the normal-boot path only.

    // I2C bus (shared by PMU, touch, IMU)
    Wire.begin(IIC_SDA, IIC_SCL);

    // Display must init before PMU: PMU reconfigures power rails that the
    // AMOLED needs in their default (POR) state.
    gfx->begin();
    gfx->fillScreen(0x0000);
    gfx->setBrightness(200);

    // PMU init after display; SD card power also comes from AXP2101.
    power_init();

    // USB MSC mode: hold Key3 (GPIO18) at boot.
    // Activated BEFORE delay(300) so SD is ready before Windows reads
    // sector 0 during USB enumeration.
    if (usb_msc_try_activate(msc_boot_held)) {
        Serial.println("{\"mode\":\"usb_msc\"}");
        // Green screen = USB drive mode active. No LVGL here, avoid raw GFX
        // text which garbles on CO5300 without full font init.
        gfx->fillScreen(0x07E0);
        Serial.println("[MSC] looping — SD owned by USB host");
        while (true) { delay(1000); }
    }

    delay(300);
    Serial.println("{\"ready\":true}");

    // Init audio codec + Opus decoder + SD card + phrase library
    sound_init();
    audio_stream_init();
    sd_init();
    sd_phrases_init();
    wifi_manager_init();
    wifi_backup_init();

    // Init IMU (accelerometer for auto-rotation)
    imu_init();

    // Init touch
    touch.setPins(TP_RST, TP_INT);
    if (!touch.begin(Wire, CST9220_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("Touch init failed");
    } else {
        touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
        touch.setSwapXY(true);
        touch.setMirrorXY(true, false);
        attachInterrupt(TP_INT, touch_isr, FALLING);
        Serial.println("Touch init OK");
    }

    // Init LVGL
    lv_init();
    lv_tick_set_cb(my_tick);

    // Allocate PSRAM-backed partial render buffers
    buf1 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    buf2 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    // rot_buf needs to hold the largest possible strip after rotation
    // A 480×40 strip rotated 90° becomes 40×480, same pixel count
    rot_buf = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);

    lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, LCD_WIDTH * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    // CO5300 even-alignment rounder
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    // Init BLE data channel
    ble_init();

    // Physical button. Leave GPIO9/GPIO10 alone; they are I2S audio pins.
    pinMode(KEY3_IO18, INPUT_PULLUP);   // Key3 / IO18

    // Build dashboard
    ui_init();
    recorder_init();

    // Show initial BLE status on Bluetooth screen
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());

    // Show initial battery status
    ui_update_battery(power_battery_pct(), power_is_charging());
    ui_update_wifi_status(wifi_manager_connected(), wifi_manager_rssi());

    ui_show_screen(SCREEN_SPLASH);

    Serial.println("Dashboard ready, waiting for data on BLE...");
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Brightness ramp state for rotation transition
// On rotation change we blank the panel, force a full LVGL redraw at the
// new orientation, then ramp brightness back up over ~125ms so the
// transition reads as deliberate instead of as a glitch.
static void handle_rotation_change(void) {
    static uint8_t last_rotation = 0;
    static uint8_t  ramp_step = 0;  // 0=idle, 1-4=ramping
    static uint32_t ramp_last = 0;

    uint8_t rot = imu_get_rotation();
    if (rot != last_rotation) {
        gfx->setBrightness(0);
        last_rotation = rot;
        lv_obj_invalidate(lv_screen_active());
        ramp_step = 1;
        return;
    }

    if (ramp_step == 0) return;
    uint32_t now = millis();
    if (now - ramp_last < 25) return;
    ramp_last = now;

    static const uint8_t levels[] = {60, 120, 170, 200};
    gfx->setBrightness(levels[ramp_step - 1]);
    if (ramp_step >= 4) ramp_step = 0;
    else                ramp_step++;
}

void loop() {
    touch_read();
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    power_tick();
    imu_tick();
    splash_tick();
    recorder_tick();
    wifi_manager_tick();
    wifi_backup_tick();

    // Button input:
    //   KEY3 (GPIO18) double-click -> start/stop recorder; hold at boot -> USB Drive Mode
    //   PWR short-press -> cycle screens; on splash -> cycle animations
    // GPIO9/GPIO10 are I2S BCLK/DIN and are not safe as buttons.
    {
        static bool key3_was = false;
        static uint32_t key3_last_release = 0;
        static uint32_t key3_last_toggle = 0;
        bool key3_now = (digitalRead(KEY3_IO18) == LOW);
        if (!key3_now && key3_was) {
            uint32_t now = millis();
            if (now - key3_last_release <= 450) {
                if (now - key3_last_toggle > 800) {
                    recorder_toggle();
                    key3_last_toggle = now;
                }
                key3_last_release = 0;
            } else {
                key3_last_release = now;
            }
        }
        key3_was = key3_now;

        if (power_pwr_pressed()) {
            if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
            else                                          ui_cycle_screen();
        }
    }

    handle_rotation_change();

    // Update BLE status on screen when state changes
    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    // Update battery indicator
    static int last_pct = -2;
    static bool last_charging = false;
    static bool last_wifi_connected = false;
    static int last_wifi_rssi_bucket = 99;
    int pct = power_battery_pct();
    bool charging = power_is_charging();
    if (pct != last_pct || charging != last_charging) {
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }
    bool wifi_connected = wifi_manager_connected();
    int wifi_rssi = wifi_manager_rssi();
    int wifi_rssi_bucket = wifi_connected ? (wifi_rssi / 5) : 99;
    if (wifi_connected != last_wifi_connected || wifi_rssi_bucket != last_wifi_rssi_bucket) {
        last_wifi_connected = wifi_connected;
        last_wifi_rssi_bucket = wifi_rssi_bucket;
        ui_update_wifi_status(wifi_connected, wifi_rssi);
    }

    // Check for serial commands (screenshot, etc.)
    check_serial_cmd();

    // Process incoming BLE data
    if (ble_has_data()) {
        sound_event_t evt = EVT_NONE;
        if (parse_json(ble_get_data(), &usage, &codex, &evt)) {
            int g_before = usage_rate_group();
            usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(&usage);
            ui_update_codex(&codex);
            if (evt != EVT_NONE) sound_play(evt);
            ble_send_ack();

            // Log to SD card: uptime_ms,session_pct,session_reset_mins,weekly_pct,status
            if (sd_ready()) {
                char log_row[96];
                snprintf(log_row, sizeof(log_row), "%lu,%.1f,%d,%.1f,%s",
                    millis(), usage.session_pct, usage.session_reset_mins,
                    usage.weekly_pct, usage.status);
                sd_log_usage(log_row);
            }
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
