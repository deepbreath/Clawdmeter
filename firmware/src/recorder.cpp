#include "recorder.h"
#include "display_cfg.h"
#include "sd_card.h"
#include "sound.h"
#include "ui.h"
#include "wifi_backup.h"
#include "wifi_manager.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <driver/i2s_std.h>
#include <driver/i2s_tdm.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

#define REC_SAMPLE_RATE 16000
#define REC_BITS        16
#define REC_CHANNELS    1
#define REC_RAW_BYTES   512
#define REC_FILE_BUF_BYTES 512
#define REC_INPUT_SLOTS 4
#define REC_ACTIVE_SLOT 0
#define REC_MIN_DURATION_MS 1000
#define REC_GAIN        12
#define REC_START_DMA_WAIT_MS 6000
#define REC_WARMUP_MS 300
#define REC_PROBE_MS 500
#define REC_PROBE_BYTES ((REC_SAMPLE_RATE * REC_PROBE_MS / 1000) * REC_INPUT_SLOTS * (REC_BITS / 8))
#define REC_PROBE_MODES 9
#define REC_SIGNAL_MIN_AC_RMS 4.0
#define REC_SIGNAL_MIN_PEAK 32
#define REC_WRITE_DEBUG_VARIANTS 0

enum recorder_capture_mode_t {
    REC_MODE_AUTO,
    REC_MODE_SLOT0,
    REC_MODE_SLOT1,
    REC_MODE_SLOT2,
    REC_MODE_SLOT3,
    REC_MODE_SWAP0,
    REC_MODE_SWAP1,
    REC_MODE_SWAP2,
    REC_MODE_SWAP3,
    REC_MODE_MIX,
    REC_MODE_TDMRAW,
    REC_MODE_ES8311,
};

struct recorder_probe_stats_t {
    uint64_t count;
    uint64_t sum_abs;
    uint64_t sum_squares;
    int64_t sum;
    uint32_t peak_abs;
    uint32_t zero_crossings;
    uint32_t clip_count;
    uint32_t zero_count;
    int16_t min_sample;
    int16_t max_sample;
    int16_t prev_sample;
    bool have_prev_sample;
    double mean;
    double avg_abs;
    double rms;
    double ac_rms;
    double zero_crossing_rate;
    double clip_rate;
    double zero_rate;
};

static i2s_chan_handle_t s_i2s_rx = nullptr;
static TaskHandle_t s_task = nullptr;
static volatile bool s_recording = false;
static volatile bool s_stop_requested = false;
static volatile bool s_stopping = false;
static char s_status[48] = "Idle";
static char s_wav_path[96];
static char s_meta_path[96];
static char s_last_wav_path[96];
static char s_slot0_wav_path[96];
static char s_slot1_wav_path[96];
static char s_swap_wav_path[96];
static char s_mix_wav_path[96];
static uint32_t s_start_ms = 0;
static uint32_t s_data_bytes = 0;
static uint32_t s_peak_abs = 0;
static uint32_t s_slot_peak[REC_INPUT_SLOTS] = {};
static uint64_t s_slot_sum_abs[REC_INPUT_SLOTS] = {};
static uint64_t s_sum_abs = 0;
static int64_t s_sum_samples = 0;
static uint64_t s_sum_squares = 0;
static uint32_t s_sample_count = 0;
static int16_t s_min_sample = 32767;
static int16_t s_max_sample = -32768;
static uint32_t s_zero_crossings = 0;
static uint32_t s_clip_count = 0;
static int16_t s_prev_sample = 0;
static bool s_have_prev_sample = false;
static volatile bool s_status_dirty = false;
static int32_t s_dc_estimate = 0;
static bool s_logged_first_samples = false;
static recorder_capture_mode_t s_capture_mode = REC_MODE_AUTO;
static recorder_capture_mode_t s_selected_capture_mode = REC_MODE_AUTO;
static uint8_t s_wav_channels = REC_CHANNELS;
static uint32_t s_sample_rate = REC_SAMPLE_RATE;
static bool s_probe_signal_ok = false;
static uint32_t s_probe_peak_abs = 0;
static double s_probe_rms = 0.0;
static double s_probe_ac_rms = 0.0;
static double s_probe_avg_abs = 0.0;
static recorder_probe_stats_t s_probe_stats[REC_PROBE_MODES];

static const char* capture_mode_name(recorder_capture_mode_t mode) {
    switch (mode) {
        case REC_MODE_AUTO:  return "auto";
        case REC_MODE_SLOT0: return "slot0";
        case REC_MODE_SLOT1: return "slot1";
        case REC_MODE_SLOT2: return "slot2";
        case REC_MODE_SLOT3: return "slot3";
        case REC_MODE_SWAP0: return "swap0";
        case REC_MODE_SWAP1: return "swap1";
        case REC_MODE_SWAP2: return "swap2";
        case REC_MODE_SWAP3: return "swap3";
        case REC_MODE_MIX:   return "mix";
        case REC_MODE_TDMRAW:return "tdmraw";
        case REC_MODE_ES8311:return "es8311";
        default:             return "auto";
    }
}

static void set_status(const char* status) {
    strlcpy(s_status, status, sizeof(s_status));
    s_status_dirty = true;
}

static bool es_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES7210_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static uint8_t es_read(uint8_t reg) {
    Wire.beginTransmission(ES7210_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0xFF;
    Wire.requestFrom((uint8_t)ES7210_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0xFF;
}

static bool es_probe(void) {
    Wire.beginTransmission(ES7210_ADDR);
    return Wire.endTransmission() == 0;
}

static bool es8311_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static uint8_t es8311_read(uint8_t reg) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0xFF;
    Wire.requestFrom((uint8_t)ES8311_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0xFF;
}

static bool es8311_probe(void) {
    Wire.beginTransmission(ES8311_ADDR);
    return Wire.endTransmission() == 0;
}

static bool es8311_adc_init(void) {
    if (!es8311_probe()) {
        Serial.println("recorder: ES8311 not responding on I2C");
        return false;
    }

    es8311_write(0x00, 0x1F);
    delay(20);
    es8311_write(0x00, 0x00);
    delay(5);
    es8311_write(0x00, 0x80);

    uint8_t id = es8311_read(0xFD);
    Serial.printf("ES8311 chip ID: 0x%02X (expect 0x83)\n", id);

    bool ok = true;
    ok &= es8311_write(0x44, 0x08);
    ok &= es8311_write(0x44, 0x08);

    // 16 kHz, 16-bit, I2S/Philips, slave mode. MCLK is 256 * Fs = 4.096 MHz.
    ok &= es8311_write(0x01, 0x3F);
    ok &= es8311_write(0x02, 0x00);
    ok &= es8311_write(0x03, 0x10);
    ok &= es8311_write(0x04, 0x10);
    ok &= es8311_write(0x05, 0x00);
    ok &= es8311_write(0x06, 0x03);
    ok &= es8311_write(0x07, 0x00);
    ok &= es8311_write(0x08, 0xFF);
    ok &= es8311_write(0x09, 0x0C);
    ok &= es8311_write(0x0A, 0x0C);

    // Espressif ES8311 start sequence for analog microphone ADC.
    ok &= es8311_write(0x0D, 0x01);
    ok &= es8311_write(0x0E, 0x02);
    ok &= es8311_write(0x10, 0x1F);
    ok &= es8311_write(0x11, 0x7F);
    ok &= es8311_write(0x12, 0x00);
    ok &= es8311_write(0x13, 0x10);
    ok &= es8311_write(0x14, 0x1A);
    ok &= es8311_write(0x15, 0x40);
    ok &= es8311_write(0x16, 0x04); // microphone gain scale, 24 dB
    ok &= es8311_write(0x17, 0xC8); // ADC digital volume, slight boost
    ok &= es8311_write(0x1B, 0x0A);
    ok &= es8311_write(0x1C, 0x6A);
    ok &= es8311_write(0x37, 0x08);
    ok &= es8311_write(0x45, 0x00);
    ok &= es8311_write(0x44, 0x58);

    if (!ok) Serial.println("recorder: ES8311 ADC init failed");
    Serial.printf("ES8311 regs: 00=%02X 01=%02X 09=%02X 0A=%02X 0E=%02X 14=%02X 15=%02X 16=%02X 17=%02X 44=%02X\n",
                  es8311_read(0x00), es8311_read(0x01), es8311_read(0x09),
                  es8311_read(0x0A), es8311_read(0x0E), es8311_read(0x14),
                  es8311_read(0x15), es8311_read(0x16), es8311_read(0x17),
                  es8311_read(0x44));
    return ok;
}

static bool es7210_init(void) {
    if (!es_probe()) {
        Serial.println("recorder: ES7210 not responding on I2C");
        return false;
    }
    uint8_t id = es_read(0xFD);
    Serial.printf("ES7210 chip ID: 0x%02X\n", id);
    if (id == 0xFF || id == 0x00) {
        Serial.println("recorder: ES7210 ID register unavailable, continuing after address ACK");
    }

    bool ok = true;
    ok &= es_write(0x00, 0xFF);
    ok &= es_write(0x00, 0x32);
    ok &= es_write(0x09, 0x30);
    ok &= es_write(0x0A, 0x30);
    ok &= es_write(0x23, 0x2A);
    ok &= es_write(0x22, 0x0A);
    ok &= es_write(0x21, 0x2A);
    ok &= es_write(0x20, 0x0A);
    ok &= es_write(0x11, 0x60); // Official ES7210: I2S, 16-bit
    ok &= es_write(0x12, 0x02); // Official ES7210: enable 1xFS TDM
    ok &= es_write(0x40, 0xC3);
    ok &= es_write(0x41, 0x70); // MIC1/2 bias 2.87V
    ok &= es_write(0x42, 0x70); // MIC3/4 bias 2.87V
    ok &= es_write(0x43, 0x18); // 24dB gain + PGA enable
    ok &= es_write(0x44, 0x18);
    ok &= es_write(0x45, 0x18);
    ok &= es_write(0x46, 0x18);
    ok &= es_write(0x47, 0x08);
    ok &= es_write(0x48, 0x08);
    ok &= es_write(0x49, 0x08);
    ok &= es_write(0x4A, 0x08);

    // 16 kHz with 4.096 MHz MCLK (256 * Fs), from Espressif ES7210 table.
    ok &= es_write(0x07, 0x20);
    ok &= es_write(0x02, 0xC1);
    ok &= es_write(0x04, 0x01);
    ok &= es_write(0x05, 0x00);
    ok &= es_write(0x06, 0x04);
    ok &= es_write(0x4B, 0x0F);
    ok &= es_write(0x4C, 0x0F);
    ok &= es_write(0x1B, 0xBF);
    ok &= es_write(0x1C, 0xBF);
    ok &= es_write(0x1D, 0xBF);
    ok &= es_write(0x1E, 0xBF);
    ok &= es_write(0x00, 0x71);
    ok &= es_write(0x00, 0x41);

    if (!ok) Serial.println("recorder: ES7210 init failed");
    Serial.printf("ES7210 regs: 00=%02X 02=%02X 06=%02X 11=%02X 12=%02X 1B=%02X 1C=%02X 40=%02X 4B=%02X 4C=%02X\n",
                  es_read(0x00), es_read(0x02), es_read(0x06), es_read(0x11),
                  es_read(0x12), es_read(0x1B), es_read(0x1C),
                  es_read(0x40), es_read(0x4B), es_read(0x4C));
    return ok;
}

static void recorder_i2s_stop(void) {
    if (!s_i2s_rx) return;
    i2s_channel_disable(s_i2s_rx);
    i2s_del_channel(s_i2s_rx);
    s_i2s_rx = nullptr;
}

static bool recorder_i2s_start_tdm(void) {
    recorder_i2s_stop();

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 2;
    chan_cfg.dma_frame_num = 16;
    esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &s_i2s_rx);
    if (err != ESP_OK) {
        Serial.printf("recorder: I2S channel failed: %s\n", esp_err_to_name(err));
        s_i2s_rx = nullptr;
        return false;
    }

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(s_sample_rate),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO,
            (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3)),
        .gpio_cfg = {
            .mclk = (gpio_num_t)I2S_MCLK_ES7210,
            .bclk = (gpio_num_t)I2S_BCK,
            .ws = (gpio_num_t)I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = (gpio_num_t)I2S_DI,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    err = i2s_channel_init_tdm_mode(s_i2s_rx, &tdm_cfg);
    if (err != ESP_OK) {
        Serial.printf("recorder: I2S TDM init failed: %s\n", esp_err_to_name(err));
        recorder_i2s_stop();
        return false;
    }
    err = i2s_channel_enable(s_i2s_rx);
    if (err != ESP_OK) {
        Serial.printf("recorder: I2S enable failed: %s\n", esp_err_to_name(err));
        recorder_i2s_stop();
        return false;
    }
    Serial.println("recorder: I2S RX TDM ready");
    return true;
}

static bool recorder_i2s_start_std(void) {
    recorder_i2s_stop();

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 2;
    chan_cfg.dma_frame_num = 16;
    esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &s_i2s_rx);
    if (err != ESP_OK) {
        Serial.printf("recorder: I2S channel failed: %s\n", esp_err_to_name(err));
        s_i2s_rx = nullptr;
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)I2S_MCLK_ES8311,
            .bclk = (gpio_num_t)I2S_BCK,
            .ws = (gpio_num_t)I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = (gpio_num_t)I2S_DI,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    err = i2s_channel_init_std_mode(s_i2s_rx, &std_cfg);
    if (err != ESP_OK) {
        Serial.printf("recorder: I2S STD init failed: %s\n", esp_err_to_name(err));
        recorder_i2s_stop();
        return false;
    }
    err = i2s_channel_enable(s_i2s_rx);
    if (err != ESP_OK) {
        Serial.printf("recorder: I2S enable failed: %s\n", esp_err_to_name(err));
        recorder_i2s_stop();
        return false;
    }
    Serial.println("recorder: I2S RX ES8311 STD ready");
    return true;
}

static void write_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void write_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void make_wav_header(uint8_t* h, uint32_t data_bytes, uint16_t channels) {
    memcpy(h + 0, "RIFF", 4);
    write_u32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    write_u32(h + 16, 16);
    write_u16(h + 20, 1);
    write_u16(h + 22, channels);
    write_u32(h + 24, s_sample_rate);
    write_u32(h + 28, s_sample_rate * channels * (REC_BITS / 8));
    write_u16(h + 32, channels * (REC_BITS / 8));
    write_u16(h + 34, REC_BITS);
    memcpy(h + 36, "data", 4);
    write_u32(h + 40, data_bytes);
}

static void make_variant_path(char* out, size_t out_size, const char* suffix) {
    strlcpy(out, s_wav_path, out_size);
    char* dot = strrchr(out, '.');
    if (dot) *dot = '\0';
    strlcat(out, suffix, out_size);
    strlcat(out, ".wav", out_size);
}

static void ensure_dirs(void) {
    if (!SD_MMC.exists("/recordings")) SD_MMC.mkdir("/recordings");
    if (!SD_MMC.exists("/recordings/pending")) SD_MMC.mkdir("/recordings/pending");
    if (!SD_MMC.exists("/recordings/sent")) SD_MMC.mkdir("/recordings/sent");
}

static void make_unique_recording_paths(void) {
    for (uint8_t attempt = 0; attempt < 100; attempt++) {
        if (attempt == 0) {
            snprintf(s_wav_path, sizeof(s_wav_path), "/recordings/pending/rec_%lu.wav",
                     (unsigned long)s_start_ms);
            snprintf(s_meta_path, sizeof(s_meta_path), "/recordings/pending/rec_%lu.json",
                     (unsigned long)s_start_ms);
        } else {
            snprintf(s_wav_path, sizeof(s_wav_path), "/recordings/pending/rec_%lu_%u.wav",
                     (unsigned long)s_start_ms,
                     (unsigned)attempt);
            snprintf(s_meta_path, sizeof(s_meta_path), "/recordings/pending/rec_%lu_%u.json",
                     (unsigned long)s_start_ms,
                     (unsigned)attempt);
        }
        if (!SD_MMC.exists(s_wav_path) && !SD_MMC.exists(s_meta_path)) return;
    }

    snprintf(s_wav_path, sizeof(s_wav_path), "/recordings/pending/rec_%lu_%lu.wav",
             (unsigned long)s_start_ms,
             (unsigned long)esp_random());
    snprintf(s_meta_path, sizeof(s_meta_path), "/recordings/pending/rec_%lu_%lu.json",
             (unsigned long)s_start_ms,
             (unsigned long)esp_random());
}

static bool wait_for_sd_dma_margin(uint32_t timeout_ms) {
    uint32_t start = millis();
    while (!sd_dma_ready_for_recording()) {
        if (millis() - start >= timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

static recorder_capture_mode_t probe_mode_for_index(uint8_t index) {
    static const recorder_capture_mode_t modes[] = {
        REC_MODE_SLOT0,
        REC_MODE_SLOT1,
        REC_MODE_SLOT2,
        REC_MODE_SLOT3,
        REC_MODE_SWAP0,
        REC_MODE_SWAP1,
        REC_MODE_SWAP2,
        REC_MODE_SWAP3,
        REC_MODE_MIX,
    };
    return modes[index];
}

static uint8_t probe_index_for_mode(recorder_capture_mode_t mode) {
    switch (mode) {
        case REC_MODE_SLOT0: return 0;
        case REC_MODE_SLOT1: return 1;
        case REC_MODE_SLOT2: return 2;
        case REC_MODE_SLOT3: return 3;
        case REC_MODE_SWAP0: return 4;
        case REC_MODE_SWAP1: return 5;
        case REC_MODE_SWAP2: return 6;
        case REC_MODE_SWAP3: return 7;
        case REC_MODE_MIX:   return 8;
        default:             return 1;
    }
}

static int16_t byte_swap_sample(int16_t sample) {
    uint16_t u = (uint16_t)sample;
    return (int16_t)((u << 8) | (u >> 8));
}

static int16_t sample_for_mode(recorder_capture_mode_t mode,
                               int16_t slot0, int16_t slot1,
                               int16_t slot2, int16_t slot3) {
    switch (mode) {
        case REC_MODE_SLOT0:
            return slot0;
        case REC_MODE_SLOT1:
            return slot1;
        case REC_MODE_SLOT2:
            return slot2;
        case REC_MODE_SLOT3:
            return slot3;
        case REC_MODE_SWAP0:
            return byte_swap_sample(slot0);
        case REC_MODE_SWAP1:
            return byte_swap_sample(slot1);
        case REC_MODE_SWAP2:
            return byte_swap_sample(slot2);
        case REC_MODE_SWAP3:
            return byte_swap_sample(slot3);
        case REC_MODE_MIX:
            return (int16_t)(((int32_t)slot0 + (int32_t)slot2) / 2);
        case REC_MODE_TDMRAW:
        case REC_MODE_ES8311:
        case REC_MODE_AUTO:
        default:
            return slot0;
    }
}

static void probe_stats_reset(recorder_probe_stats_t& stats) {
    memset(&stats, 0, sizeof(stats));
    stats.min_sample = 32767;
    stats.max_sample = -32768;
}

static void probe_stats_add(recorder_probe_stats_t& stats, int16_t sample) {
    uint32_t abs_sample = (sample < 0) ? (uint32_t)(-(int32_t)sample) : (uint32_t)sample;
    if (abs_sample > stats.peak_abs) stats.peak_abs = abs_sample;
    if (sample < stats.min_sample) stats.min_sample = sample;
    if (sample > stats.max_sample) stats.max_sample = sample;
    if (sample == 32767 || sample == -32768) stats.clip_count++;
    if (sample == 0) stats.zero_count++;
    if (stats.have_prev_sample && ((sample < 0 && stats.prev_sample >= 0) ||
                                   (sample >= 0 && stats.prev_sample < 0))) {
        stats.zero_crossings++;
    }
    stats.prev_sample = sample;
    stats.have_prev_sample = true;
    stats.sum += sample;
    stats.sum_abs += abs_sample;
    stats.sum_squares += (int32_t)sample * (int32_t)sample;
    stats.count++;
}

static void probe_stats_finish(recorder_probe_stats_t& stats) {
    if (!stats.count) return;
    stats.mean = (double)stats.sum / (double)stats.count;
    stats.avg_abs = (double)stats.sum_abs / (double)stats.count;
    stats.rms = sqrt((double)stats.sum_squares / (double)stats.count);
    double variance = ((double)stats.sum_squares / (double)stats.count) -
                      (stats.mean * stats.mean);
    if (variance < 0.0) variance = 0.0;
    stats.ac_rms = sqrt(variance);
    stats.zero_crossing_rate = (double)stats.zero_crossings / (double)stats.count;
    stats.clip_rate = (double)stats.clip_count / (double)stats.count;
    stats.zero_rate = (double)stats.zero_count / (double)stats.count;
}

static void probe_stats_collect(const uint8_t* raw, size_t raw_bytes,
                                recorder_probe_stats_t* stats) {
    const int16_t* samples = (const int16_t*)raw;
    uint16_t frames = raw_bytes / (REC_INPUT_SLOTS * sizeof(int16_t));
    for (uint16_t i = 0; i < frames; i++) {
        int16_t slot0 = samples[i * REC_INPUT_SLOTS];
        int16_t slot1 = (REC_INPUT_SLOTS > 1) ? samples[i * REC_INPUT_SLOTS + 1] : slot0;
        int16_t slot2 = (REC_INPUT_SLOTS > 2) ? samples[i * REC_INPUT_SLOTS + 2] : slot0;
        int16_t slot3 = (REC_INPUT_SLOTS > 3) ? samples[i * REC_INPUT_SLOTS + 3] : slot0;
        for (uint8_t c = 0; c < REC_PROBE_MODES; c++) {
            probe_stats_add(stats[c], sample_for_mode(probe_mode_for_index(c), slot0, slot1, slot2, slot3));
        }
    }
}

static recorder_capture_mode_t select_capture_mode_from_probe(void) {
    for (uint8_t c = 0; c < REC_PROBE_MODES; c++) probe_stats_finish(s_probe_stats[c]);

    uint8_t selected = probe_index_for_mode(REC_MODE_SLOT1);
    double best_score = -1.0;
    static const uint8_t raw_candidates[] = {0, 1, 2, 3, 8};
    static const uint8_t swap_candidates[] = {4, 5, 6, 7};

    for (uint8_t i = 0; i < sizeof(raw_candidates); i++) {
        uint8_t c = raw_candidates[i];
        const recorder_probe_stats_t& stats = s_probe_stats[c];
        bool mostly_zero = stats.zero_rate > 0.98;
        bool mostly_clipped = stats.clip_rate > 0.60;
        double score = stats.ac_rms;
        bool signal_ok = score >= REC_SIGNAL_MIN_AC_RMS &&
                         stats.peak_abs >= REC_SIGNAL_MIN_PEAK;
        if (stats.count && signal_ok && !mostly_zero && !mostly_clipped && score > best_score) {
            best_score = score;
            selected = c;
        }
    }

    if (best_score < 0.0) {
        for (uint8_t i = 0; i < sizeof(swap_candidates); i++) {
            uint8_t c = swap_candidates[i];
            const recorder_probe_stats_t& stats = s_probe_stats[c];
            bool mostly_zero = stats.zero_rate > 0.98;
            bool mostly_clipped = stats.clip_rate > 0.60;
            double score = stats.ac_rms;
            if (stats.count && !mostly_zero && !mostly_clipped && score > best_score) {
                best_score = score;
                selected = c;
            }
        }
    }

    const recorder_probe_stats_t& chosen = s_probe_stats[selected];
    s_probe_signal_ok = chosen.ac_rms >= REC_SIGNAL_MIN_AC_RMS &&
                        chosen.peak_abs >= REC_SIGNAL_MIN_PEAK &&
                        chosen.zero_rate < 0.98 &&
                        chosen.clip_rate < 0.60;
    s_probe_peak_abs = chosen.peak_abs;
    s_probe_rms = chosen.rms;
    s_probe_ac_rms = chosen.ac_rms;
    s_probe_avg_abs = chosen.avg_abs;
    return probe_mode_for_index(selected);
}

static void write_metadata(void) {
    JsonDocument doc;
    doc["device_id"] = wifi_backup_device_id();
    doc["wav_path"] = s_wav_path;
#if REC_WRITE_DEBUG_VARIANTS
    doc["slot0_wav_path"] = s_slot0_wav_path;
    doc["slot1_wav_path"] = s_slot1_wav_path;
    doc["swap_wav_path"] = s_swap_wav_path;
    doc["mix_wav_path"] = s_mix_wav_path;
#endif
    doc["start_uptime_ms"] = s_start_ms;
    doc["end_uptime_ms"] = millis();
    doc["sample_rate"] = s_sample_rate;
    doc["bits"] = REC_BITS;
    doc["channels"] = s_wav_channels;
    doc["duration_ms"] = (uint32_t)((uint64_t)s_data_bytes * 1000ULL / (s_sample_rate * s_wav_channels * (REC_BITS / 8)));
    doc["file_bytes"] = s_data_bytes + 44;
    doc["peak_abs"] = s_peak_abs;
    JsonArray slot_peaks = doc["slot_peaks"].to<JsonArray>();
    for (uint8_t i = 0; i < REC_INPUT_SLOTS; i++) slot_peaks.add(s_slot_peak[i]);
    JsonArray slot_avg_abs = doc["slot_avg_abs"].to<JsonArray>();
    for (uint8_t i = 0; i < REC_INPUT_SLOTS; i++) {
        slot_avg_abs.add(s_sample_count ? (uint32_t)(s_slot_sum_abs[i] / s_sample_count) : 0);
    }
    doc["input_slots"] = REC_INPUT_SLOTS;
    doc["active_slot"] = REC_ACTIVE_SLOT;
    doc["capture_mode"] = capture_mode_name(s_capture_mode);
    doc["capture_mode_selected"] = capture_mode_name(s_selected_capture_mode);
    doc["raw_tdm"] = (s_selected_capture_mode == REC_MODE_TDMRAW);
    doc["codec"] = (s_selected_capture_mode == REC_MODE_ES8311) ? "ES8311" : "ES7210";
    doc["i2s_mode"] = (s_selected_capture_mode == REC_MODE_ES8311) ? "std" : "tdm";
    doc["signal_ok"] = s_probe_signal_ok;
    doc["probe_peak"] = s_probe_peak_abs;
    doc["probe_rms"] = s_probe_rms;
    doc["probe_ac_rms"] = s_probe_ac_rms;
    doc["probe_avg_abs"] = s_probe_avg_abs;
    doc["analog_gain_db"] = 24;
    doc["digital_gain"] = REC_GAIN;
    doc["avg_abs"] = (uint32_t)(s_sample_count ? (s_sum_abs / s_sample_count) : 0);
    JsonObject waveform = doc["waveform"].to<JsonObject>();
    waveform["min"] = s_min_sample;
    waveform["max"] = s_max_sample;
    waveform["mean"] = s_sample_count ? (double)s_sum_samples / (double)s_sample_count : 0.0;
    waveform["rms"] = s_sample_count ? sqrt((double)s_sum_squares / (double)s_sample_count) : 0.0;
    waveform["zero_crossings"] = s_zero_crossings;
    waveform["zero_crossing_rate"] = s_sample_count ? (double)s_zero_crossings / (double)s_sample_count : 0.0;
    waveform["clip_count"] = s_clip_count;
    waveform["clip_rate"] = s_sample_count ? (double)s_clip_count / (double)s_sample_count : 0.0;
    doc["status"] = "pending";

    SD_MMC.remove(s_meta_path);
    File f = SD_MMC.open(s_meta_path, FILE_WRITE);
    if (!f) {
        Serial.printf("recorder: metadata open failed: %s\n", s_meta_path);
        return;
    }
    size_t json_bytes = serializeJson(doc, f);
    f.println();
    f.flush();
    f.close();
    Serial.printf("recorder: metadata wrote %s (%u bytes json)\n",
                  s_meta_path,
                  (unsigned)json_bytes);
}

static void verify_saved_wav(void);

static bool finish_file(File& f) {
    uint8_t header[44] = {};
    make_wav_header(header, s_data_bytes, s_wav_channels);
    f.seek(0);
    size_t wrote = f.write(header, sizeof(header));
    f.flush();
    f.close();
    write_metadata();
    verify_saved_wav();
    return wrote == sizeof(header);
}

static void verify_saved_wav(void) {
    File f = SD_MMC.open(s_wav_path, FILE_READ);
    if (!f) {
        Serial.printf("recorder: verify open failed: %s\n", s_wav_path);
        return;
    }
    uint8_t header[44];
    if (f.read(header, sizeof(header)) != sizeof(header)) {
        Serial.println("recorder: verify header read failed");
        f.close();
        return;
    }
    int16_t buf[64];
    size_t got = f.read((uint8_t*)buf, sizeof(buf));
    uint32_t nonzero = 0;
    uint32_t peak = 0;
    uint64_t sum_abs = 0;
    uint16_t samples = got / sizeof(int16_t);
    for (uint16_t i = 0; i < samples; i++) {
        uint32_t av = (buf[i] < 0) ? (uint32_t)(-(int32_t)buf[i]) : (uint32_t)buf[i];
        if (buf[i] != 0) nonzero++;
        if (av > peak) peak = av;
        sum_abs += av;
    }
    f.close();
    Serial.printf("recorder: verify first %u samples nonzero=%lu peak=%lu avg_abs=%lu\n",
                  (unsigned)samples,
                  (unsigned long)nonzero,
                  (unsigned long)peak,
                  (unsigned long)(samples ? sum_abs / samples : 0));
}

static bool write_pcm_buffered(File& f, const uint8_t* data, size_t bytes,
                               uint8_t* file_buf, size_t& file_buf_used) {
    size_t offset = 0;
    while (offset < bytes) {
        size_t space = REC_FILE_BUF_BYTES - file_buf_used;
        size_t take = bytes - offset;
        if (take > space) take = space;
        memcpy(file_buf + file_buf_used, data + offset, take);
        file_buf_used += take;
        offset += take;

        if (file_buf_used == REC_FILE_BUF_BYTES) {
            size_t wrote = f.write(file_buf, REC_FILE_BUF_BYTES);
            if (wrote != REC_FILE_BUF_BYTES) return false;
            s_data_bytes += wrote;
            file_buf_used = 0;
        }
    }
    return true;
}

static bool flush_pcm_buffer(File& f, uint8_t* file_buf, size_t& file_buf_used) {
    if (!file_buf_used) return true;
    size_t wrote = f.write(file_buf, file_buf_used);
    if (wrote != file_buf_used) return false;
    s_data_bytes += wrote;
    file_buf_used = 0;
    return true;
}

static bool finish_variant_file(File& f) {
    if (!f) return true;
    uint8_t header[44] = {};
    make_wav_header(header, s_data_bytes, REC_CHANNELS);
    f.seek(0);
    size_t wrote = f.write(header, sizeof(header));
    f.flush();
    f.close();
    return wrote == sizeof(header);
}

static uint16_t fold_input_to_mono(const uint8_t* raw, size_t raw_bytes,
                                   int16_t* mono, int16_t* slot0_mono,
                                   int16_t* slot1_mono, int16_t* swap_mono,
                                   int16_t* mix_mono) {
    const int16_t* samples = (const int16_t*)raw;
    uint16_t frames = raw_bytes / (REC_INPUT_SLOTS * sizeof(int16_t));
    for (uint16_t i = 0; i < frames; i++) {
        int16_t slot0 = samples[i * REC_INPUT_SLOTS];
        int16_t slot1 = (REC_INPUT_SLOTS > 1) ? samples[i * REC_INPUT_SLOTS + 1] : slot0;
        int16_t slot2 = (REC_INPUT_SLOTS > 2) ? samples[i * REC_INPUT_SLOTS + 2] : slot0;
        int16_t slot3 = (REC_INPUT_SLOTS > 3) ? samples[i * REC_INPUT_SLOTS + 3] : slot0;
        int16_t sample = sample_for_mode(s_selected_capture_mode, slot0, slot1, slot2, slot3);
        if (slot0_mono) slot0_mono[i] = slot0;
        if (slot1_mono) slot1_mono[i] = slot1;
        if (swap_mono) swap_mono[i] = byte_swap_sample(slot1);
        if (mix_mono) mix_mono[i] = (int16_t)(((int32_t)slot0 + (int32_t)slot2) / 2);
        for (uint8_t slot = 0; slot < REC_INPUT_SLOTS; slot++) {
            int16_t candidate = samples[i * REC_INPUT_SLOTS + slot];
            uint32_t abs_candidate = (candidate < 0) ? (uint32_t)(-(int32_t)candidate) : (uint32_t)candidate;
            if (abs_candidate > s_slot_peak[slot]) s_slot_peak[slot] = abs_candidate;
            s_slot_sum_abs[slot] += abs_candidate;
        }
        if (!s_logged_first_samples && i == 0 && raw_bytes >= REC_INPUT_SLOTS * sizeof(int16_t)) {
            Serial.printf("recorder: first slots raw=[%d,%d,%d,%d], mode=%s\n",
                          (int)slot0,
                          (int)slot1,
                          (int)slot2,
                          (int)slot3,
                          capture_mode_name(s_selected_capture_mode));
            s_logged_first_samples = true;
        }
        s_dc_estimate += (((int32_t)sample << 8) - s_dc_estimate) >> 8;
        int32_t centered = (int32_t)sample - (s_dc_estimate >> 8);
        int32_t amplified = centered * REC_GAIN;
        if (amplified > 28000) amplified = 28000 + (amplified - 28000) / 8;
        if (amplified < -28000) amplified = -28000 + (amplified + 28000) / 8;
        if (amplified > 32767) amplified = 32767;
        if (amplified < -32768) amplified = -32768;
        sample = (int16_t)amplified;
        uint32_t abs_sample = (sample < 0) ? (uint32_t)(-(int32_t)sample) : (uint32_t)sample;
        if (abs_sample > s_peak_abs) s_peak_abs = abs_sample;
        s_sum_abs += abs_sample;
        s_sum_samples += sample;
        s_sum_squares += (int32_t)sample * (int32_t)sample;
        if (sample < s_min_sample) s_min_sample = sample;
        if (sample > s_max_sample) s_max_sample = sample;
        if (sample == 32767 || sample == -32768) s_clip_count++;
        if (s_have_prev_sample && ((sample < 0 && s_prev_sample >= 0) ||
                                   (sample >= 0 && s_prev_sample < 0))) {
            s_zero_crossings++;
        }
        s_prev_sample = sample;
        s_have_prev_sample = true;
        s_sample_count++;
        mono[i] = sample;
    }
    return frames;
}

static void collect_raw_tdm_stats(const uint8_t* raw, size_t raw_bytes) {
    const int16_t* samples = (const int16_t*)raw;
    uint16_t frames = raw_bytes / (REC_INPUT_SLOTS * sizeof(int16_t));
    for (uint16_t i = 0; i < frames; i++) {
        int16_t slot0 = samples[i * REC_INPUT_SLOTS];
        int16_t slot1 = (REC_INPUT_SLOTS > 1) ? samples[i * REC_INPUT_SLOTS + 1] : slot0;
        int16_t slot2 = (REC_INPUT_SLOTS > 2) ? samples[i * REC_INPUT_SLOTS + 2] : slot0;
        int16_t slot3 = (REC_INPUT_SLOTS > 3) ? samples[i * REC_INPUT_SLOTS + 3] : slot0;
        for (uint8_t slot = 0; slot < REC_INPUT_SLOTS; slot++) {
            int16_t candidate = samples[i * REC_INPUT_SLOTS + slot];
            uint32_t abs_candidate = (candidate < 0) ? (uint32_t)(-(int32_t)candidate) : (uint32_t)candidate;
            if (abs_candidate > s_slot_peak[slot]) s_slot_peak[slot] = abs_candidate;
            s_slot_sum_abs[slot] += abs_candidate;
        }
        if (!s_logged_first_samples && i == 0 && raw_bytes >= REC_INPUT_SLOTS * sizeof(int16_t)) {
            Serial.printf("recorder: first slots raw=[%d,%d,%d,%d], mode=%s\n",
                          (int)slot0,
                          (int)slot1,
                          (int)slot2,
                          (int)slot3,
                          capture_mode_name(s_selected_capture_mode));
            s_logged_first_samples = true;
        }

        int16_t sample = (int16_t)(((int32_t)slot0 + (int32_t)slot2) / 2);
        uint32_t abs_sample = (sample < 0) ? (uint32_t)(-(int32_t)sample) : (uint32_t)sample;
        if (abs_sample > s_peak_abs) s_peak_abs = abs_sample;
        s_sum_abs += abs_sample;
        s_sum_samples += sample;
        s_sum_squares += (int32_t)sample * (int32_t)sample;
        if (sample < s_min_sample) s_min_sample = sample;
        if (sample > s_max_sample) s_max_sample = sample;
        if (sample == 32767 || sample == -32768) s_clip_count++;
        if (s_have_prev_sample && ((sample < 0 && s_prev_sample >= 0) ||
                                   (sample >= 0 && s_prev_sample < 0))) {
            s_zero_crossings++;
        }
        s_prev_sample = sample;
        s_have_prev_sample = true;
        s_sample_count++;
    }
}

static uint16_t fold_es8311_std_to_mono(const uint8_t* raw, size_t raw_bytes, int16_t* mono) {
    const int16_t* samples = (const int16_t*)raw;
    uint16_t frames = raw_bytes / (2 * sizeof(int16_t));
    for (uint16_t i = 0; i < frames; i++) {
        int16_t left = samples[i * 2];
        int16_t right = samples[i * 2 + 1];

        uint32_t abs_left = (left < 0) ? (uint32_t)(-(int32_t)left) : (uint32_t)left;
        uint32_t abs_right = (right < 0) ? (uint32_t)(-(int32_t)right) : (uint32_t)right;
        if (abs_left > s_slot_peak[0]) s_slot_peak[0] = abs_left;
        if (abs_right > s_slot_peak[1]) s_slot_peak[1] = abs_right;
        s_slot_sum_abs[0] += abs_left;
        s_slot_sum_abs[1] += abs_right;

        if (!s_logged_first_samples && i == 0) {
            Serial.printf("recorder: first es8311 raw=[%d,%d], mode=%s\n",
                          (int)left,
                          (int)right,
                          capture_mode_name(s_selected_capture_mode));
            s_logged_first_samples = true;
        }

        int16_t sample = left;
        if (s_selected_capture_mode == REC_MODE_SLOT1) {
            sample = right;
        } else if (s_selected_capture_mode == REC_MODE_SWAP0) {
            sample = byte_swap_sample(left);
        } else if (s_selected_capture_mode == REC_MODE_SWAP1) {
            sample = byte_swap_sample(right);
        } else if (s_selected_capture_mode == REC_MODE_MIX ||
                   s_selected_capture_mode == REC_MODE_AUTO) {
            sample = (abs_right > abs_left) ? right : left;
        }

        s_dc_estimate += (((int32_t)sample << 8) - s_dc_estimate) >> 8;
        int32_t centered = (int32_t)sample - (s_dc_estimate >> 8);
        int32_t amplified = centered * REC_GAIN;
        if (amplified > 28000) amplified = 28000 + (amplified - 28000) / 8;
        if (amplified < -28000) amplified = -28000 + (amplified + 28000) / 8;
        if (amplified > 32767) amplified = 32767;
        if (amplified < -32768) amplified = -32768;
        sample = (int16_t)amplified;

        uint32_t abs_sample = (sample < 0) ? (uint32_t)(-(int32_t)sample) : (uint32_t)sample;
        if (abs_sample > s_peak_abs) s_peak_abs = abs_sample;
        s_sum_abs += abs_sample;
        s_sum_samples += sample;
        s_sum_squares += (int32_t)sample * (int32_t)sample;
        if (sample < s_min_sample) s_min_sample = sample;
        if (sample > s_max_sample) s_max_sample = sample;
        if (sample == 32767 || sample == -32768) s_clip_count++;
        if (s_have_prev_sample && ((sample < 0 && s_prev_sample >= 0) ||
                                   (sample >= 0 && s_prev_sample < 0))) {
            s_zero_crossings++;
        }
        s_prev_sample = sample;
        s_have_prev_sample = true;
        s_sample_count++;
        mono[i] = sample;
    }
    return frames;
}

static void recorder_task(void*) {
    uint8_t* raw_buf = nullptr;
    uint8_t* file_buf = nullptr;
    int16_t* mono_buf = nullptr;
    int16_t* slot0_mono_buf = nullptr;
    int16_t* slot1_mono_buf = nullptr;
    int16_t* swap_mono_buf = nullptr;
    int16_t* mix_mono_buf = nullptr;
    bool header_ok = false;
    bool slot0_header_ok = true;
    bool slot1_header_ok = true;
    bool swap_header_ok = true;
    bool mix_header_ok = true;
    double rms = 0.0;
    double zcr = 0.0;
    bool io_failed = false;
    bool raw_tdm = false;
    bool es8311_source = false;
    size_t file_buf_used = 0;
    File f;
#if REC_WRITE_DEBUG_VARIANTS
    File f_slot0;
    File f_slot1;
    File f_swap;
    File f_mix;
#endif

    s_selected_capture_mode = s_capture_mode;
    raw_tdm = (s_selected_capture_mode == REC_MODE_TDMRAW);
    es8311_source = (s_selected_capture_mode == REC_MODE_ES8311);
    s_wav_channels = raw_tdm ? REC_INPUT_SLOTS : REC_CHANNELS;

    raw_buf = (uint8_t*)heap_caps_malloc(
        REC_RAW_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    file_buf = (uint8_t*)heap_caps_malloc(
        REC_FILE_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_selected_capture_mode != REC_MODE_TDMRAW) {
        mono_buf = (int16_t*)heap_caps_malloc(
            REC_RAW_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
#if REC_WRITE_DEBUG_VARIANTS
    slot0_mono_buf = (int16_t*)heap_caps_malloc(REC_RAW_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    slot1_mono_buf = (int16_t*)heap_caps_malloc(REC_RAW_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    swap_mono_buf = (int16_t*)heap_caps_malloc(REC_RAW_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    mix_mono_buf = (int16_t*)heap_caps_malloc(REC_RAW_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
#endif
    if (!raw_buf || !file_buf || (s_selected_capture_mode != REC_MODE_TDMRAW && !mono_buf)
#if REC_WRITE_DEBUG_VARIANTS
        || !slot0_mono_buf || !slot1_mono_buf || !swap_mono_buf || !mix_mono_buf
#endif
    ) {
        Serial.println("recorder: DMA buffer alloc failed");
        set_status("No memory");
        goto done;
    }

    if (!(es8311_source ? recorder_i2s_start_std() : recorder_i2s_start_tdm())) {
        set_status("Audio init failed");
        goto done;
    }

    if (!(es8311_source ? es8311_adc_init() : es7210_init())) {
        set_status("Audio init failed");
        goto done;
    }

    s_probe_signal_ok = false;
    s_probe_peak_abs = 0;
    s_probe_rms = 0.0;
    s_probe_ac_rms = 0.0;
    s_probe_avg_abs = 0.0;
    for (uint8_t c = 0; c < REC_PROBE_MODES; c++) probe_stats_reset(s_probe_stats[c]);
    Serial.printf("recorder: capture configured=%s selected=%s\n",
                  capture_mode_name(s_capture_mode),
                  capture_mode_name(s_selected_capture_mode));

    {
        uint32_t warmup_start = millis();
        size_t probe_bytes = 0;
        while (!s_stop_requested && millis() - warmup_start < REC_WARMUP_MS) {
            size_t got = 0;
            esp_err_t read_err = i2s_channel_read(
                s_i2s_rx, raw_buf, REC_RAW_BYTES, &got, pdMS_TO_TICKS(100));
            if (got > 0 && !es8311_source && !raw_tdm && probe_bytes < REC_PROBE_BYTES) {
                probe_stats_collect(raw_buf, got, s_probe_stats);
                probe_bytes += got;
            }
            if (read_err != ESP_OK && read_err != ESP_ERR_TIMEOUT) {
                Serial.printf("recorder: warmup read failed: %s\n", esp_err_to_name(read_err));
                set_status("Audio read failed");
                break;
            }
        }
        if (s_capture_mode == REC_MODE_AUTO && !es8311_source && !raw_tdm && probe_bytes > 0) {
            s_selected_capture_mode = select_capture_mode_from_probe();
            Serial.printf("recorder: auto selected %s (peak=%lu rms=%.1f ac_rms=%.1f avg_abs=%.1f signal=%d)\n",
                          capture_mode_name(s_selected_capture_mode),
                          (unsigned long)s_probe_peak_abs,
                          s_probe_rms,
                          s_probe_ac_rms,
                          s_probe_avg_abs,
                          s_probe_signal_ok ? 1 : 0);
        } else if (s_capture_mode == REC_MODE_AUTO) {
            s_selected_capture_mode = REC_MODE_MIX;
        }
    }

    SD_MMC.remove(s_wav_path);
    if (!sd_dma_ready_for_recording()) {
        Serial.printf("recorder: low DMA before open, dma=%u\n", (unsigned)sd_dma_free());
        set_status("No memory");
        goto done;
    }
    f = SD_MMC.open(s_wav_path, FILE_WRITE);
    if (!f) {
        Serial.printf("recorder: open failed: %s (dma=%u)\n", s_wav_path, (unsigned)sd_dma_free());
        set_status("No SD");
        goto done;
    }

    {
        uint8_t header[44] = {};
        make_wav_header(header, 0, s_wav_channels);
        if (f.write(header, sizeof(header)) != sizeof(header)) {
            Serial.printf("recorder: header write failed (dma=%u)\n", (unsigned)sd_dma_free());
            set_status("No SD");
            io_failed = true;
        }
    }

    set_status("Recording");
    Serial.printf("recorder: recording to %s\n", s_wav_path);

    while (!s_stop_requested && !io_failed) {
        size_t got = 0;
        esp_err_t read_err = i2s_channel_read(
            s_i2s_rx, raw_buf, REC_RAW_BYTES, &got, pdMS_TO_TICKS(100));
        if (got > 0) {
            if (raw_tdm) {
                collect_raw_tdm_stats(raw_buf, got);
                if (!write_pcm_buffered(f, raw_buf, got, file_buf, file_buf_used)) {
                    Serial.println("recorder: SD write failed");
                    set_status("No SD");
                    io_failed = true;
                    break;
                }
                continue;
            }

            uint16_t mono_samples = es8311_source
                ? fold_es8311_std_to_mono(raw_buf, got, mono_buf)
                : fold_input_to_mono(raw_buf, got, mono_buf,
#if REC_WRITE_DEBUG_VARIANTS
                                     slot0_mono_buf, slot1_mono_buf,
                                     swap_mono_buf, mix_mono_buf
#else
                                     nullptr, nullptr, nullptr, nullptr
#endif
            );
            size_t mono_bytes = mono_samples * sizeof(int16_t);
            bool write_ok = write_pcm_buffered(
                f, (const uint8_t*)mono_buf, mono_bytes, file_buf, file_buf_used);
#if REC_WRITE_DEBUG_VARIANTS
            if (f_slot0) f_slot0.write((const uint8_t*)slot0_mono_buf, mono_bytes);
            if (f_slot1) f_slot1.write((const uint8_t*)slot1_mono_buf, mono_bytes);
            if (f_swap) f_swap.write((const uint8_t*)swap_mono_buf, mono_bytes);
            if (f_mix) f_mix.write((const uint8_t*)mix_mono_buf, mono_bytes);
#endif
            if (!write_ok) {
                Serial.println("recorder: SD write failed");
                set_status("No SD");
                io_failed = true;
                break;
            }
        } else if (read_err != ESP_OK && read_err != ESP_ERR_TIMEOUT) {
            Serial.printf("recorder: I2S read failed: %s\n", esp_err_to_name(read_err));
            set_status("Audio read failed");
            break;
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    recorder_i2s_stop();
    if (f && !flush_pcm_buffer(f, file_buf, file_buf_used)) {
        Serial.println("recorder: SD flush failed");
        header_ok = false;
        io_failed = true;
    }
    if (f) header_ok = finish_file(f);
#if REC_WRITE_DEBUG_VARIANTS
    slot0_header_ok = finish_variant_file(f_slot0);
    slot1_header_ok = finish_variant_file(f_slot1);
    swap_header_ok = finish_variant_file(f_swap);
    mix_header_ok = finish_variant_file(f_mix);
#endif
    strlcpy(s_last_wav_path, s_wav_path, sizeof(s_last_wav_path));
    rms = s_sample_count ? sqrt((double)s_sum_squares / (double)s_sample_count) : 0.0;
    zcr = s_sample_count ? (double)s_zero_crossings / (double)s_sample_count : 0.0;
    Serial.printf("recorder: saved %s (%lu bytes PCM, file=%lu, header=%s, gain=%dx, mode=%s, peak=%lu, avg_abs=%lu, rms=%.1f, zcr=%.4f, clips=%lu, min=%d, max=%d, slots=[%lu,%lu,%lu,%lu])\n",
                  s_wav_path,
                  (unsigned long)s_data_bytes,
                  (unsigned long)(s_data_bytes + 44),
                  header_ok ? "ok" : "failed",
                  REC_GAIN,
                  capture_mode_name(s_selected_capture_mode),
                  (unsigned long)s_peak_abs,
                  (unsigned long)(s_sample_count ? (s_sum_abs / s_sample_count) : 0),
                  rms,
                  zcr,
                  (unsigned long)s_clip_count,
                  (int)s_min_sample,
                  (int)s_max_sample,
                  (unsigned long)s_slot_peak[0],
                  (unsigned long)((REC_INPUT_SLOTS > 1) ? s_slot_peak[1] : 0),
                  (unsigned long)((REC_INPUT_SLOTS > 2) ? s_slot_peak[2] : 0),
                  (unsigned long)((REC_INPUT_SLOTS > 3) ? s_slot_peak[3] : 0));
#if REC_WRITE_DEBUG_VARIANTS
    Serial.printf("recorder: debug variants slot0=%s header=%s slot1=%s header=%s swap=%s header=%s mix=%s header=%s\n",
                  s_slot0_wav_path, slot0_header_ok ? "ok" : "failed",
                  s_slot1_wav_path, slot1_header_ok ? "ok" : "failed",
                  s_swap_wav_path, swap_header_ok ? "ok" : "failed",
                  s_mix_wav_path, mix_header_ok ? "ok" : "failed");
#endif
    set_status("Saved");

done:
    if (f) f.close();
    if (raw_buf) heap_caps_free(raw_buf);
    if (file_buf) heap_caps_free(file_buf);
    if (mono_buf) heap_caps_free(mono_buf);
    if (slot0_mono_buf) heap_caps_free(slot0_mono_buf);
    if (slot1_mono_buf) heap_caps_free(slot1_mono_buf);
    if (swap_mono_buf) heap_caps_free(swap_mono_buf);
    if (mix_mono_buf) heap_caps_free(mix_mono_buf);
    recorder_i2s_stop();
    sd_set_recording_busy(false);
    sound_set_stream_active(false);
    sound_resume_output();
    wifi_manager_pause(false);
    s_recording = false;
    s_stopping = false;
    s_stop_requested = false;
    s_task = nullptr;
    vTaskDelete(nullptr);
}

void recorder_init(void) {
    set_status("Idle");
    ui_update_recorder_status("");
    s_status_dirty = false;
}

void recorder_tick(void) {
    if (!s_status_dirty) return;
    s_status_dirty = false;
    ui_update_recorder_status(s_status);
}

bool recorder_toggle(void) {
    if (s_recording) {
        if (millis() - s_start_ms < REC_MIN_DURATION_MS) {
            set_status("Recording");
            return false;
        }
        s_stopping = true;
        s_stop_requested = true;
        set_status("Saving");
        return true;
    }

    if (s_stopping) return false;
    if (wifi_backup_is_uploading()) {
        set_status("Uploading");
        return false;
    }
    if (!sd_ready()) {
        Serial.printf("recorder: SD not ready (dma=%u)\n", (unsigned)sd_dma_free());
        set_status("No SD");
        return false;
    }
    wifi_manager_pause(true);
    wait_for_sd_dma_margin(REC_START_DMA_WAIT_MS);
    if (!sd_dma_ready_for_recording()) {
        wifi_manager_pause(false);
        Serial.printf("recorder: low DMA memory, dma=%u\n", (unsigned)sd_dma_free());
        set_status("No memory");
        return false;
    }
    if (!sound_try_claim_audio()) {
        wifi_manager_pause(false);
        set_status("Audio busy");
        return false;
    }

    ensure_dirs();
    sd_set_recording_busy(true);
    s_start_ms = millis();
    s_data_bytes = 0;
    s_peak_abs = 0;
    memset(s_slot_peak, 0, sizeof(s_slot_peak));
    memset(s_slot_sum_abs, 0, sizeof(s_slot_sum_abs));
    s_sum_abs = 0;
    s_sum_samples = 0;
    s_sum_squares = 0;
    s_sample_count = 0;
    s_min_sample = 32767;
    s_max_sample = -32768;
    s_zero_crossings = 0;
    s_clip_count = 0;
    s_prev_sample = 0;
    s_have_prev_sample = false;
    s_dc_estimate = 0;
    s_logged_first_samples = false;
    s_selected_capture_mode = REC_MODE_AUTO;
    s_wav_channels = REC_CHANNELS;
    s_probe_signal_ok = false;
    s_probe_peak_abs = 0;
    s_probe_rms = 0.0;
    s_probe_ac_rms = 0.0;
    s_probe_avg_abs = 0.0;
    memset(s_probe_stats, 0, sizeof(s_probe_stats));
    make_unique_recording_paths();
#if REC_WRITE_DEBUG_VARIANTS
    make_variant_path(s_slot0_wav_path, sizeof(s_slot0_wav_path), "_slot0");
    make_variant_path(s_slot1_wav_path, sizeof(s_slot1_wav_path), "_slot1");
    make_variant_path(s_swap_wav_path, sizeof(s_swap_wav_path), "_swap");
    make_variant_path(s_mix_wav_path, sizeof(s_mix_wav_path), "_mix");
#else
    s_slot0_wav_path[0] = '\0';
    s_slot1_wav_path[0] = '\0';
    s_swap_wav_path[0] = '\0';
    s_mix_wav_path[0] = '\0';
#endif
    s_stop_requested = false;
    s_recording = true;
    BaseType_t task_ok = xTaskCreate(recorder_task, "recorder", 8192, nullptr, 4, &s_task);
    if (task_ok != pdPASS) {
        s_recording = false;
        sound_set_stream_active(false);
        sd_set_recording_busy(false);
        wifi_manager_pause(false);
        set_status("Record failed");
        return false;
    }
    return true;
}

bool recorder_is_recording(void) {
    return s_recording;
}

const char* recorder_status(void) {
    return s_status;
}

const char* recorder_last_wav_path(void) {
    return s_last_wav_path[0] ? s_last_wav_path : nullptr;
}

void recorder_forget_last_wav_path(void) {
    s_last_wav_path[0] = '\0';
}

static bool analyze_wav_path(const char* label, const char* path) {
    if (!path || !path[0]) return false;
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) {
        Serial.printf("wave: %s open failed: %s\n", label, path);
        return false;
    }

    uint8_t header[44];
    if (f.read(header, sizeof(header)) != sizeof(header) ||
        memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4) ||
        memcmp(header + 12, "fmt ", 4) || memcmp(header + 36, "data", 4)) {
        Serial.printf("wave: %s invalid header: %s\n", label, path);
        f.close();
        return false;
    }
    uint16_t channels = header[22] | ((uint16_t)header[23] << 8);
    uint32_t rate = header[24] | ((uint32_t)header[25] << 8) |
                    ((uint32_t)header[26] << 16) | ((uint32_t)header[27] << 24);
    uint16_t bits = header[34] | ((uint16_t)header[35] << 8);
    uint32_t data_bytes = header[40] | ((uint32_t)header[41] << 8) |
                          ((uint32_t)header[42] << 16) | ((uint32_t)header[43] << 24);

    int16_t min_v = 32767;
    int16_t max_v = -32768;
    int16_t prev = 0;
    bool have_prev = false;
    uint64_t count = 0;
    uint64_t sum_abs = 0;
    int64_t sum = 0;
    uint64_t sum_sq = 0;
    uint32_t zero_cross = 0;
    uint32_t clips = 0;
    uint32_t zeros = 0;

    int16_t buf[128];
    while (f.available()) {
        int got = f.read((uint8_t*)buf, sizeof(buf));
        if (got <= 0) break;
        int n = got / sizeof(int16_t);
        for (int i = 0; i < n; i++) {
            int16_t v = buf[i];
            uint32_t av = v < 0 ? (uint32_t)(-(int32_t)v) : (uint32_t)v;
            if (v < min_v) min_v = v;
            if (v > max_v) max_v = v;
            if (v == 32767 || v == -32768) clips++;
            if (v == 0) zeros++;
            if (have_prev && ((v < 0 && prev >= 0) || (v >= 0 && prev < 0))) zero_cross++;
            prev = v;
            have_prev = true;
            sum += v;
            sum_abs += av;
            sum_sq += (int32_t)v * (int32_t)v;
            count++;
        }
    }
    f.close();

    double mean = count ? (double)sum / (double)count : 0.0;
    double avg_abs = count ? (double)sum_abs / (double)count : 0.0;
    double rms = count ? sqrt((double)sum_sq / (double)count) : 0.0;
    double zcr = count ? (double)zero_cross / (double)count : 0.0;
    double clip_rate = count ? (double)clips / (double)count : 0.0;
    double zero_rate = count ? (double)zeros / (double)count : 0.0;
    Serial.printf("wave: %s path=%s rate=%lu bits=%u ch=%u data=%lu samples=%llu min=%d max=%d mean=%.1f avg_abs=%.1f rms=%.1f zcr=%.4f clips=%lu(%.4f) zeros=%.4f\n",
                  label,
                  path,
                  (unsigned long)rate,
                  (unsigned)bits,
                  (unsigned)channels,
                  (unsigned long)data_bytes,
                  (unsigned long long)count,
                  (int)min_v,
                  (int)max_v,
                  mean,
                  avg_abs,
                  rms,
                  zcr,
                  (unsigned long)clips,
                  clip_rate,
                  zero_rate);
    return true;
}

void recorder_analyze_last_wav(void) {
    if (!s_last_wav_path[0]) {
        Serial.println("wave: no recording yet");
        return;
    }
    Serial.printf("wave: capture configured=%s selected=%s signal_ok=%d probe_peak=%lu probe_rms=%.1f probe_ac_rms=%.1f probe_avg_abs=%.1f\n",
                  capture_mode_name(s_capture_mode),
                  capture_mode_name(s_selected_capture_mode),
                  s_probe_signal_ok ? 1 : 0,
                  (unsigned long)s_probe_peak_abs,
                  s_probe_rms,
                  s_probe_ac_rms,
                  s_probe_avg_abs);
    analyze_wav_path("main", s_last_wav_path);
#if REC_WRITE_DEBUG_VARIANTS
    analyze_wav_path("slot0", s_slot0_wav_path);
    analyze_wav_path("slot1", s_slot1_wav_path);
    analyze_wav_path("swap", s_swap_wav_path);
    analyze_wav_path("mix", s_mix_wav_path);
#endif
}

bool recorder_set_capture_mode(const char* mode) {
    if (!mode) return false;
    if (strcmp(mode, "auto") == 0) {
        s_capture_mode = REC_MODE_AUTO;
    } else if (strcmp(mode, "slot0") == 0) {
        s_capture_mode = REC_MODE_SLOT0;
    } else if (strcmp(mode, "slot1") == 0) {
        s_capture_mode = REC_MODE_SLOT1;
    } else if (strcmp(mode, "slot2") == 0) {
        s_capture_mode = REC_MODE_SLOT2;
    } else if (strcmp(mode, "slot3") == 0) {
        s_capture_mode = REC_MODE_SLOT3;
    } else if (strcmp(mode, "swap0") == 0) {
        s_capture_mode = REC_MODE_SWAP0;
    } else if (strcmp(mode, "swap1") == 0 || strcmp(mode, "swap") == 0) {
        s_capture_mode = REC_MODE_SWAP1;
    } else if (strcmp(mode, "swap2") == 0) {
        s_capture_mode = REC_MODE_SWAP2;
    } else if (strcmp(mode, "swap3") == 0) {
        s_capture_mode = REC_MODE_SWAP3;
    } else if (strcmp(mode, "mix") == 0) {
        s_capture_mode = REC_MODE_MIX;
    } else if (strcmp(mode, "tdmraw") == 0 || strcmp(mode, "raw") == 0) {
        s_capture_mode = REC_MODE_TDMRAW;
    } else if (strcmp(mode, "es8311") == 0) {
        s_capture_mode = REC_MODE_ES8311;
    } else {
        return false;
    }
    Serial.printf("recorder: capture mode=%s\n", capture_mode_name(s_capture_mode));
    return true;
}

const char* recorder_capture_mode(void) {
    return capture_mode_name(s_capture_mode);
}

bool recorder_set_sample_rate(uint32_t sample_rate) {
    if (sample_rate != 16000 && sample_rate != 48000) return false;
    if (s_recording || s_stopping) return false;
    s_sample_rate = sample_rate;
    Serial.printf("recorder: sample rate=%lu\n", (unsigned long)s_sample_rate);
    return true;
}

uint32_t recorder_sample_rate(void) {
    return s_sample_rate;
}
