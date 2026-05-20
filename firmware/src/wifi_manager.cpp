#include "wifi_manager.h"
#include "sd_card.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_err.h>

#define WIFI_CONFIG_PATH       "/config/wifi.json"
#define RECORDER_CONFIG_PATH   "/config/recorder.json"
#define WIFI_RETRY_MS          10000
#define WIFI_CONNECT_TIMEOUT_MS 20000

struct WifiConfig {
    char ssid[64];
    char password[96];
    char device_id[64];
};

static WifiConfig s_cfg = {};
static bool s_configured = false;
static uint32_t s_last_wifi_ms = 0;
static uint32_t s_connect_started_ms = 0;
static bool s_connecting = false;
static bool s_paused = false;
static bool s_wifi_stopped_for_pause = false;
static wl_status_t s_last_status = WL_IDLE_STATUS;

static bool load_config_file(const char* path) {
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("wifi: config parse failed %s: %s\n", path, err.c_str());
        return false;
    }

    strlcpy(s_cfg.ssid, doc["ssid"] | "", sizeof(s_cfg.ssid));
    strlcpy(s_cfg.password, doc["password"] | "", sizeof(s_cfg.password));
    strlcpy(s_cfg.device_id, doc["device_id"] | "clawdmeter", sizeof(s_cfg.device_id));
    s_configured = s_cfg.ssid[0] && s_cfg.device_id[0];
    Serial.printf("wifi: config %s from %s (ssid=%s, device=%s)\n",
                  s_configured ? "ready" : "incomplete",
                  path,
                  s_cfg.ssid[0] ? s_cfg.ssid : "---",
                  s_cfg.device_id);
    return s_configured;
}

static bool load_config(void) {
    s_configured = false;
    if (!sd_ready()) {
        Serial.println("wifi: SD not ready");
        return false;
    }

    if (load_config_file(WIFI_CONFIG_PATH)) return true;
    if (load_config_file(RECORDER_CONFIG_PATH)) {
        Serial.println("wifi: using legacy recorder.json network fields");
        return true;
    }

    Serial.printf("wifi: missing %s (or legacy %s)\n",
                  WIFI_CONFIG_PATH, RECORDER_CONFIG_PATH);
    return false;
}

void wifi_manager_init(void) {
    load_config();
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);
}

void wifi_manager_tick(void) {
    uint32_t now = millis();
    if (!s_configured) return;
    if (s_paused) return;

    wl_status_t status = WiFi.status();
    if (status != s_last_status) {
        s_last_status = status;
        Serial.printf("wifi: status=%d\n", (int)status);
    }

    if (status == WL_CONNECTED) {
        if (s_connecting) {
            s_connecting = false;
            Serial.printf("wifi: connected ip=%s\n", WiFi.localIP().toString().c_str());
        }
        return;
    }

    if (s_connecting) {
        if (now - s_connect_started_ms < WIFI_CONNECT_TIMEOUT_MS) return;
        Serial.println("wifi: connect timeout, retry later");
        WiFi.disconnect(false, false);
        s_connecting = false;
        s_last_wifi_ms = now;
        return;
    }

    if (status == WL_SCAN_COMPLETED) return;

    if (now - s_last_wifi_ms < WIFI_RETRY_MS) return;
    s_last_wifi_ms = now;
    s_connect_started_ms = now;
    s_connecting = true;

    WiFi.mode(WIFI_STA);
    WiFi.begin(s_cfg.ssid, s_cfg.password);
    Serial.printf("wifi: connecting to %s\n", s_cfg.ssid);
}

bool wifi_manager_configured(void) {
    return s_configured;
}

bool wifi_manager_connected(void) {
    return WiFi.status() == WL_CONNECTED;
}

int wifi_manager_rssi(void) {
    return wifi_manager_connected() ? WiFi.RSSI() : 0;
}

void wifi_manager_pause(bool paused) {
    if (s_paused == paused) return;
    s_paused = paused;
    s_connecting = false;
    if (paused) {
        Serial.println("wifi: paused");
        if (WiFi.getMode() != WIFI_MODE_NULL) {
            WiFi.disconnect(false, false);
            WiFi.mode(WIFI_OFF);
            s_wifi_stopped_for_pause = true;
        } else {
            s_wifi_stopped_for_pause = false;
        }
        s_last_status = WL_IDLE_STATUS;
    } else {
        Serial.println("wifi: resumed");
        if (s_wifi_stopped_for_pause) {
            WiFi.mode(WIFI_STA);
            s_wifi_stopped_for_pause = false;
        }
        s_connecting = false;
        s_last_wifi_ms = 0;
    }
}

const char* wifi_manager_ssid(void) {
    return s_cfg.ssid[0] ? s_cfg.ssid : "---";
}

const char* wifi_manager_device_id(void) {
    return s_cfg.device_id[0] ? s_cfg.device_id : "clawdmeter";
}

void wifi_manager_print_status(void) {
    Serial.printf("wifi: configured=%d connected=%d ssid=%s device=%s ip=%s\n",
                  s_configured ? 1 : 0,
                  wifi_manager_connected() ? 1 : 0,
                  wifi_manager_ssid(),
                  wifi_manager_device_id(),
                  wifi_manager_connected() ? WiFi.localIP().toString().c_str() : "---");
}
