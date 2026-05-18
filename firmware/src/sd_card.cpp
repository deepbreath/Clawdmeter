#include "sd_card.h"
#include "sound.h"
#include <Arduino.h>
#include <SD_MMC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

// Waveshare ESP32-S3-Touch-AMOLED-2.16 onboard TF card, SDMMC 1-bit mode.
#define SD_CLK 2
#define SD_CMD 1
#define SD_D0  3

static bool     s_ready    = false;
static volatile bool s_wav_busy = false;
static char     s_wav_path[64];

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

    sound_set_stream_active(true);

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
    return true;
}

bool sd_ready(void) { return s_ready; }

bool sd_play_wav(const char* path) {
    if (!s_ready || s_wav_busy) return false;
    if (!SD_MMC.exists(path))   return false;
    strncpy(s_wav_path, path, sizeof(s_wav_path) - 1);
    s_wav_path[sizeof(s_wav_path) - 1] = '\0';
    s_wav_busy = true;
    xTaskCreate(_wav_task, "sd_wav", 8192, nullptr, 4, nullptr);
    return true;
}

bool sd_wav_busy(void) { return s_wav_busy; }

void sd_log_usage(const char* row) {
    if (!s_ready || !row) return;
    if (!SD_MMC.exists("/log")) SD_MMC.mkdir("/log");
    File f = SD_MMC.open("/log/usage.csv", FILE_APPEND);
    if (!f) return;
    f.println(row);
    f.close();
}
