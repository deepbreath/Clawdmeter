#include "wifi_backup.h"
#include "recorder.h"
#include "sd_card.h"
#include "ui.h"
#include "wifi_manager.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <WiFiClient.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define BACKUP_CONFIG_PATH "/config/recorder.json"
#define UPLOAD_RETRY_MS   30000
#define UPLOAD_AFTER_WIFI_MS 5000
#define UPLOAD_CHUNK_BYTES 512

struct BackupConfig {
    char upload_url[160];
    char auth_token[160];
};

struct ParsedUrl {
    char host[96];
    char path[160];
    uint16_t port;
};

static BackupConfig s_cfg = {};
static bool s_configured = false;
static bool s_upload_requested = false;
static uint32_t s_last_upload_ms = 0;
static uint32_t s_wifi_ready_ms = 0;
static volatile bool s_uploading = false;
static volatile bool s_status_dirty = false;
static char s_status[48] = "";

static void set_status(const char* status) {
    strlcpy(s_status, status, sizeof(s_status));
    s_status_dirty = true;
}

static bool ends_with(const char* s, const char* suffix) {
    size_t sl = strlen(s);
    size_t pl = strlen(suffix);
    return sl >= pl && strcmp(s + sl - pl, suffix) == 0;
}

static void dirname_sent(const char* src, char* dst, size_t len) {
    const char* name = strrchr(src, '/');
    name = name ? name + 1 : src;
    snprintf(dst, len, "/recordings/sent/%s", name);
}

static void meta_path_for_wav(const char* wav, char* meta, size_t len) {
    strlcpy(meta, wav, len);
    char* dot = strrchr(meta, '.');
    if (dot) strlcpy(dot, ".json", len - (dot - meta));
}

static bool parse_url(const char* url, ParsedUrl* out) {
    const char* p = url;
    if (strncmp(p, "http://", 7) != 0) {
        Serial.println("wifi_backup: only http:// upload_url is supported");
        return false;
    }
    p += 7;
    const char* slash = strchr(p, '/');
    if (!slash) return false;
    const char* colon = strchr(p, ':');
    size_t host_len = 0;
    out->port = 80;
    if (colon && colon < slash) {
        host_len = (size_t)(colon - p);
        out->port = (uint16_t)atoi(colon + 1);
    } else {
        host_len = (size_t)(slash - p);
    }
    if (host_len == 0 || host_len >= sizeof(out->host)) return false;
    memcpy(out->host, p, host_len);
    out->host[host_len] = '\0';
    strlcpy(out->path, slash, sizeof(out->path));
    return true;
}

static bool load_config(void) {
    s_configured = false;
    if (!sd_ready()) {
        Serial.println("wifi_backup: SD not ready");
        return false;
    }
    File f = SD_MMC.open(BACKUP_CONFIG_PATH, FILE_READ);
    if (!f) {
        Serial.printf("wifi_backup: missing %s\n", BACKUP_CONFIG_PATH);
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("wifi_backup: config parse failed: %s\n", err.c_str());
        return false;
    }

    strlcpy(s_cfg.upload_url, doc["upload_url"] | "", sizeof(s_cfg.upload_url));
    strlcpy(s_cfg.auth_token, doc["auth_token"] | "", sizeof(s_cfg.auth_token));
    s_configured = s_cfg.upload_url[0];
    Serial.printf("wifi_backup: config %s (device=%s)\n",
                  s_configured ? "ready" : "incomplete",
                  wifi_manager_device_id());
    return s_configured;
}

static void ensure_dirs(void) {
    if (!SD_MMC.exists("/recordings")) SD_MMC.mkdir("/recordings");
    if (!SD_MMC.exists("/recordings/pending")) SD_MMC.mkdir("/recordings/pending");
    if (!SD_MMC.exists("/recordings/sent")) SD_MMC.mkdir("/recordings/sent");
}

static bool send_all(WiFiClient& client, const uint8_t* data, size_t len) {
    size_t written = 0;
    while (written < len) {
        size_t n = client.write(data + written, len - written);
        if (n == 0) return false;
        written += n;
    }
    return true;
}

static bool upload_one(const char* wav_path) {
    if (!sd_dma_ready()) {
        Serial.printf("wifi_backup: defer upload, low dma=%u\n", (unsigned)sd_dma_free());
        return false;
    }
    uint8_t* io_buf = (uint8_t*)heap_caps_malloc(
        UPLOAD_CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!io_buf) {
        Serial.println("wifi_backup: DMA buffer alloc failed");
        return false;
    }

    char meta_path[96];
    meta_path_for_wav(wav_path, meta_path, sizeof(meta_path));
    if (!SD_MMC.exists(meta_path)) {
        Serial.printf("wifi_backup: skip %s (missing metadata)\n", wav_path);
        heap_caps_free(io_buf);
        return false;
    }

    File audio = SD_MMC.open(wav_path, FILE_READ);
    File meta_file = SD_MMC.open(meta_path, FILE_READ);
    if (!audio || !meta_file) {
        if (audio) audio.close();
        if (meta_file) meta_file.close();
        heap_caps_free(io_buf);
        return false;
    }

    String metadata = meta_file.readString();
    meta_file.close();

    ParsedUrl url = {};
    if (!parse_url(s_cfg.upload_url, &url)) {
        audio.close();
        heap_caps_free(io_buf);
        return false;
    }

    const char* boundary = "----ClawdmeterRecorderBoundary";
    String pre_meta = String("--") + boundary +
        "\r\nContent-Disposition: form-data; name=\"metadata\"\r\n"
        "Content-Type: application/json\r\n\r\n";
    String between = String("\r\n--") + boundary +
        "\r\nContent-Disposition: form-data; name=\"audio\"; filename=\"" +
        String(strrchr(wav_path, '/') ? strrchr(wav_path, '/') + 1 : wav_path) +
        "\"\r\nContent-Type: audio/wav\r\n\r\n";
    String closing = String("\r\n--") + boundary + "--\r\n";
    uint32_t content_length = pre_meta.length() + metadata.length() +
        between.length() + audio.size() + closing.length();

    WiFiClient client;
    client.setTimeout(12000);
    if (!client.connect(url.host, url.port)) {
        Serial.printf("wifi_backup: connect failed %s:%u\n", url.host, url.port);
        audio.close();
        heap_caps_free(io_buf);
        return false;
    }

    client.printf("POST %s HTTP/1.1\r\n", url.path);
    client.printf("Host: %s\r\n", url.host);
    client.print("Connection: close\r\n");
    client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
    client.printf("Content-Length: %lu\r\n", (unsigned long)content_length);
    if (s_cfg.auth_token[0]) {
        client.printf("Authorization: Bearer %s\r\n", s_cfg.auth_token);
    }
    client.print("\r\n");
    client.print(pre_meta);
    client.print(metadata);
    client.print(between);

    while (audio.available()) {
        int n = audio.read(io_buf, UPLOAD_CHUNK_BYTES);
        if (n <= 0 || !send_all(client, io_buf, (size_t)n)) {
            audio.close();
            client.stop();
            heap_caps_free(io_buf);
            return false;
        }
    }
    audio.close();
    heap_caps_free(io_buf);
    client.print(closing);

    String status = client.readStringUntil('\n');
    client.stop();
    bool ok = status.indexOf(" 200 ") > 0 || status.indexOf(" 201 ") > 0 ||
              status.indexOf(" 202 ") > 0 || status.indexOf(" 204 ") > 0;
    Serial.printf("wifi_backup: upload %s -> %s", wav_path, status.c_str());
    return ok;
}

static bool move_to_sent(const char* wav_path) {
    if (!sd_dma_ready()) return false;
    char meta_path[96], wav_dst[96], meta_dst[96];
    meta_path_for_wav(wav_path, meta_path, sizeof(meta_path));
    dirname_sent(wav_path, wav_dst, sizeof(wav_dst));
    dirname_sent(meta_path, meta_dst, sizeof(meta_dst));
    if (SD_MMC.exists(wav_dst)) SD_MMC.remove(wav_dst);
    if (SD_MMC.exists(meta_dst)) SD_MMC.remove(meta_dst);
    bool ok = SD_MMC.rename(wav_path, wav_dst);
    if (SD_MMC.exists(meta_path)) ok = SD_MMC.rename(meta_path, meta_dst) && ok;
    return ok;
}

static void scan_and_upload(void*) {
    if (recorder_is_recording() || !s_configured ||
        !wifi_manager_connected() || !sd_ready()) {
        s_uploading = false;
        vTaskDelete(nullptr);
        return;
    }
    if (!sd_dma_ready()) {
        Serial.printf("wifi_backup: defer scan, low dma=%u\n", (unsigned)sd_dma_free());
        set_status("Low memory");
        s_upload_requested = false;
        s_uploading = false;
        vTaskDelete(nullptr);
        return;
    }
    ensure_dirs();

    File dir = SD_MMC.open("/recordings/pending");
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        s_upload_requested = false;
        s_uploading = false;
        vTaskDelete(nullptr);
        return;
    }

    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            char path[96];
            const char* name = entry.name();
            if (name[0] == '/') strlcpy(path, name, sizeof(path));
            else snprintf(path, sizeof(path), "/recordings/pending/%s", name);
            entry.close();

            if (ends_with(path, ".wav")) {
                set_status("Uploading");
                if (upload_one(path)) {
                    move_to_sent(path);
                    set_status("Uploaded");
                } else {
                    set_status("Upload failed");
                    break;
                }
            }
        } else {
            entry.close();
        }
        entry = dir.openNextFile();
    }
    dir.close();
    s_upload_requested = false;
    s_uploading = false;
    vTaskDelete(nullptr);
}

void wifi_backup_init(void) {
    load_config();
}

void wifi_backup_tick(void) {
    if (!s_configured) return;
    if (s_status_dirty) {
        s_status_dirty = false;
        ui_update_recorder_status(s_status);
    }

    if (!wifi_manager_connected()) {
        s_wifi_ready_ms = 0;
        return;
    }

    uint32_t now = millis();
    if (s_wifi_ready_ms == 0) {
        s_wifi_ready_ms = now;
        return;
    }
    if (now - s_wifi_ready_ms < UPLOAD_AFTER_WIFI_MS) return;

    if (s_upload_requested || now - s_last_upload_ms >= UPLOAD_RETRY_MS) {
        s_last_upload_ms = now;
        if (!s_uploading) {
            if (!sd_dma_ready()) {
                set_status("Low memory");
                s_upload_requested = false;
                return;
            }
            s_uploading = true;
            if (xTaskCreate(scan_and_upload, "wifi_upload", 6144, nullptr, 3, nullptr) != pdPASS) {
                s_uploading = false;
                set_status("Upload failed");
            }
        }
    }
}

void wifi_backup_request_upload(void) {
    s_upload_requested = true;
}

void wifi_backup_print_status(void) {
    Serial.printf("wifi_backup: configured=%d wifi=%d device=%s url=%s\n",
                  s_configured ? 1 : 0,
                  wifi_manager_connected() ? 1 : 0,
                  wifi_manager_device_id(),
                  s_cfg.upload_url[0] ? s_cfg.upload_url : "---");
}

const char* wifi_backup_device_id(void) {
    return wifi_manager_device_id();
}

bool wifi_backup_configured(void) {
    return s_configured;
}

bool wifi_backup_is_uploading(void) {
    return s_uploading;
}
