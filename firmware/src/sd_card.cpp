#include "sd_card.h"
#include "sound.h"
#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

// Waveshare ESP32-S3-Touch-AMOLED-2.16 onboard TF card, SDMMC 1-bit mode.
#define SD_CLK 2
#define SD_CMD 1
#define SD_D0  3
#define SD_MIN_DMA_FREE 24576
#define SD_RECORD_MIN_DMA_FREE 4096

static bool     s_ready    = false;
static volatile bool s_wav_busy = false;
static volatile bool s_recording_busy = false;
static char     s_wav_path[64];
static SemaphoreHandle_t s_sd_mutex = nullptr;
static bool s_log_dir_ready = false;
static uint32_t s_last_log_skip_ms = 0;

static bool sd_try_lock(uint32_t timeout_ms) {
    return s_sd_mutex && xSemaphoreTake(s_sd_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void sd_unlock(void) {
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
}

static bool sd_dma_has_margin(void) {
    return sd_dma_ready();
}

static void log_skip_once(const char* reason) {
    uint32_t now = millis();
    if (now - s_last_log_skip_ms < 10000) return;
    s_last_log_skip_ms = now;
    Serial.printf("sd: skip usage log (%s, dma=%u)\n",
                  reason,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
}

// ---------------------------------------------------------------------------
// WAV header scanner - handles any chunk order, optional extended fmt
// ---------------------------------------------------------------------------

struct _WavInfo {
    uint32_t data_offset;
    uint32_t data_bytes;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits;
};

static bool _wav_parse(File& f, _WavInfo& out) {
    char     tag[4];
    uint32_t sz;

    f.seek(0);
    if (f.read((uint8_t*)tag, 4) < 4 || memcmp(tag, "RIFF", 4)) return false;
    f.read((uint8_t*)&sz, 4);
    if (f.read((uint8_t*)tag, 4) < 4 || memcmp(tag, "WAVE", 4)) return false;

    bool got_fmt = false, got_data = false;
    while (!got_data && f.available() >= 8) {
        if (f.read((uint8_t*)tag, 4) < 4) break;
        uint32_t csz = 0;
        f.read((uint8_t*)&csz, 4);

        if (!memcmp(tag, "fmt ", 4)) {
            uint16_t fmt = 0;
            f.read((uint8_t*)&fmt, 2);
            f.read((uint8_t*)&out.channels, 2);
            f.read((uint8_t*)&out.sample_rate, 4);
            uint32_t br = 0; f.read((uint8_t*)&br, 4);
            uint16_t ba = 0; f.read((uint8_t*)&ba, 2);
            f.read((uint8_t*)&out.bits, 2);
            // skip any extra fmt bytes
            int extra = (int)csz - 16;
            if (extra > 0) f.seek(f.position() + extra);
            got_fmt = true;

        } else if (!memcmp(tag, "data", 4)) {
            out.data_offset = f.position();
            out.data_bytes  = csz;
            got_data = true;

        } else {
            // skip unknown chunk (word-aligned)
            f.seek(f.position() + csz + (csz & 1));
        }
    }
    return got_fmt && got_data;
}

// ---------------------------------------------------------------------------
// WAV streaming task
// ---------------------------------------------------------------------------

static void _wav_task(void*) {
    File f = SD_MMC.open(s_wav_path, FILE_READ);
    if (!f) {
        Serial.printf("sd: not found: %s\n", s_wav_path);
        s_wav_busy = false;
        vTaskDelete(nullptr);
        return;
    }

    _WavInfo info = {};
    if (!_wav_parse(f, info) || info.bits != 16) {
        Serial.printf("sd: unsupported WAV: %s\n", s_wav_path);
        f.close();
        s_wav_busy = false;
        vTaskDelete(nullptr);
        return;
    }

    f.seek(info.data_offset);
    Serial.printf("sd: play %s (%u Hz %u-ch, %.1fs)\n",
        s_wav_path, info.sample_rate, info.channels,
        (float)info.data_bytes / (info.sample_rate * info.channels * 2));

    if (!sound_try_start_sd_stream()) {
        Serial.println("sd: audio busy");
        f.close();
        s_wav_busy = false;
        vTaskDelete(nullptr);
        return;
    }

    const int CHUNK = 256;
    int16_t buf[CHUNK];
    uint32_t rem = info.data_bytes;

    while (rem > 0 && f.available()) {
        uint32_t want = (rem < (uint32_t)(CHUNK * 2)) ? rem : (uint32_t)(CHUNK * 2);
        int got = f.read((uint8_t*)buf, want);
        if (got <= 0) break;
        int n = got / 2;  // samples per channel
        if (info.channels == 2) {
            // mix stereo to mono
            for (int i = 0; i < n / 2; i++)
                buf[i] = ((int32_t)buf[i * 2] + buf[i * 2 + 1]) / 2;
            n /= 2;
        }
        sound_push_pcm(buf, n);
        rem -= (uint32_t)got;
    }

    f.close();
    sound_set_stream_active(false);
    s_wav_busy = false;
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool sd_init(void) {
    if (!s_sd_mutex) s_sd_mutex = xSemaphoreCreateMutex();

    if (!SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0)) {
        Serial.println("SD: setPins failed");
        return false;
    }

    if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5)) {
        Serial.println("SD: init failed (check pins / card)");
        return false;
    }

    uint64_t mb = SD_MMC.cardSize() / (1024ULL * 1024ULL);
    Serial.printf("SD: %llu MB ready\n", mb);
    s_ready = true;
    if (sd_try_lock(250)) {
        if (!SD_MMC.exists("/log")) SD_MMC.mkdir("/log");
        s_log_dir_ready = SD_MMC.exists("/log");
        sd_unlock();
    }
    return true;
}

bool sd_ready(void) { return s_ready; }

void sd_end(void) {
    if (!s_ready) return;
    uint32_t start = millis();
    while ((s_wav_busy || s_recording_busy) && millis() - start < 1000) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (sd_try_lock(1000)) {
        Serial.println("SD: unmount");
        SD_MMC.end();
        s_ready = false;
        s_log_dir_ready = false;
        sd_unlock();
    } else {
        Serial.println("SD: unmount skipped (busy)");
    }
}

void sd_print_status(void) {
    Serial.printf("SD: ready=%d dma=%u card_mb=%llu\n",
                  s_ready ? 1 : 0,
                  (unsigned)sd_dma_free(),
                  s_ready ? (SD_MMC.cardSize() / (1024ULL * 1024ULL)) : 0ULL);
}

void sd_set_recording_busy(bool busy) {
    s_recording_busy = busy;
}

uint32_t sd_dma_free(void) {
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
}

bool sd_dma_ready(void) {
    return sd_dma_free() >= SD_MIN_DMA_FREE;
}

bool sd_dma_ready_for_recording(void) {
    return sd_dma_free() >= SD_RECORD_MIN_DMA_FREE;
}

bool sd_play_wav(const char* path) {
    if (!s_ready || s_wav_busy) return false;
    if (!sd_dma_ready()) {
        log_skip_once("low dma memory");
        return false;
    }
    if (!SD_MMC.exists(path))   return false;
    strncpy(s_wav_path, path, sizeof(s_wav_path) - 1);
    s_wav_path[sizeof(s_wav_path) - 1] = '\0';
    s_wav_busy = true;
    xTaskCreate(_wav_task, "sd_wav", 4096, nullptr, 4, nullptr);
    return true;
}

bool sd_wav_busy(void) { return s_wav_busy; }

void sd_log_usage(const char* row) {
    if (!s_ready || !row) return;
    if (s_recording_busy) return;
    if (!s_log_dir_ready) {
        log_skip_once("log dir unavailable");
        return;
    }
    if (!sd_dma_has_margin()) {
        log_skip_once("low dma memory");
        return;
    }
    if (!sd_try_lock(0)) {
        log_skip_once("sd busy");
        return;
    }
    File f = SD_MMC.open("/log/usage.csv", FILE_APPEND);
    if (!f) {
        sd_unlock();
        return;
    }
    f.println(row);
    f.close();
    sd_unlock();
}
