#include "sound.h"
#include "sd_card.h"
#include "sound_data.h"
#include "display_cfg.h"
#include <Arduino.h>
#include <ESP_I2S.h>
#include <Wire.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define SAMPLE_RATE  16000
#define TONE_AMP     0.45f   // 0..1, avoids clipping
#define PCM_GAIN_NUM 2
#define PCM_GAIN_DEN 3

static I2SClass s_i2s;
static bool s_ready = false;
static bool s_output_active = false;
static volatile bool s_playing = false;
static volatile bool s_stream_active = false;

static inline int16_t attenuate_sample(int16_t sample) {
    return (int16_t)(((int32_t)sample * PCM_GAIN_NUM) / PCM_GAIN_DEN);
}

// ---- ES8311 I2C helpers ----

static void es_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static uint8_t es_read(uint8_t reg) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)ES8311_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0xFF;
}

// Init ES8311 for 16kHz / 16-bit / mono playback.
// MCLK = 256 × 16000 = 4,096,000 Hz (provided on I2S_MCLK_ES8311 = GPIO42).
static bool es8311_init(void) {
    // Reset → clear → power-on
    es_write(0x00, 0x1F);
    delay(20);
    es_write(0x00, 0x00);
    delay(5);
    es_write(0x00, 0x80);

    uint8_t id = es_read(0xFD);
    Serial.printf("ES8311 chip ID: 0x%02X (expect 0x83)\n", id);

    // Clock: MCLK=4.096MHz from GPIO42, BCLK=MCLK/8=512kHz (16kHz*2slots*16bit)
    es_write(0x01, 0x3F);  // enable all internal clocks, MCLK from external pin
    es_write(0x02, 0x00);  // pre-divider = 1 (no pre-divide)
    es_write(0x03, 0x10);  // ADC OSR = 16
    es_write(0x04, 0x10);  // DAC OSR = 16
    es_write(0x05, 0x00);  // ADC/DAC div = 1
    es_write(0x06, 0x03);  // BCLK divider = 4 (MCLK/8, value = div-1)
    es_write(0x07, 0x00);  // LRCK high byte
    es_write(0x08, 0xFF);  // LRCK low byte (256 BCLK per frame)

    // I2S interface: Philips/standard, 16-bit, slave mode
    es_write(0x09, 0x0C);

    // System / power
    es_write(0x0D, 0x01);  // power up reference
    es_write(0x0E, 0x02);  // power up analog
    es_write(0x10, 0x03);  // vmid
    es_write(0x11, 0x7C);  // bias
    es_write(0x12, 0x00);  // power up DAC
    es_write(0x13, 0x10);  // anaref
    es_write(0x1C, 0x6A);  // ADC EQ bypass, cancel DC offset

    // DAC path
    es_write(0x31, 0x00);  // DAC equalizer bypass
    es_write(0x32, 0xB2);  // DAC volume: ~70% (0xFF = 0dB, 0x00 = mute)
    es_write(0x33, 0x00);  // DAC offset = 0
    es_write(0x37, 0x08);  // DAC analog block on
    es_write(0x45, 0x00);  // output stage on

    Serial.println("ES8311 init OK");
    return true;
}

// ---- I2S setup ----

static bool i2s_init(void) {
    if (s_output_active) return true;
    s_i2s.setPins(I2S_BCK, I2S_WS, I2S_DO, I2S_GPIO_UNUSED, I2S_MCLK_ES8311);
    if (!s_i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                     I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        Serial.printf("I2S init error: %d\n", s_i2s.lastError());
        return false;
    }

    Serial.println("I2S init OK");
    s_output_active = true;
    return true;
}

static bool output_start(void) {
    if (!s_ready) return false;
    if (s_output_active) return true;
    digitalWrite(PA_EN, LOW);
    if (!i2s_init()) return false;
    if (!es8311_init()) {
        s_i2s.end();
        s_output_active = false;
        return false;
    }
    delay(20);
    digitalWrite(PA_EN, HIGH);
    return true;
}

static void output_stop(void) {
    digitalWrite(PA_EN, LOW);
    if (s_output_active) {
        s_i2s.end();
        s_output_active = false;
    }
}

// ---- Tone generator ----

// FM synthesis tone. mod_idx=0 → pure sine (bell); 0.28 → digital shimmer.
// Envelope: 4ms attack → exponential decay (tau = duration * tau_ratio).
static void play_tone(float freq_hz, int duration_ms,
                      float mod_idx = 0.28f, float tau_ratio = 0.38f) {
    if (!s_ready) return;
    const int total  = SAMPLE_RATE * duration_ms / 1000;
    const int chunk  = 256;
    const int attack = SAMPLE_RATE * 4 / 1000;
    const float tau  = (duration_ms * tau_ratio) / 1000.0f;
    int16_t buf[chunk];

    for (int s = 0; s < total; s += chunk) {
        int n = (s + chunk <= total) ? chunk : (total - s);
        for (int i = 0; i < n; i++) {
            int idx = s + i;
            float t = (float)idx / SAMPLE_RATE;
            float env = (idx < attack)
                ? (float)idx / attack
                : expf(-(float)(idx - attack) / (tau * SAMPLE_RATE));
            float mod    = mod_idx * sinf(4.0f * (float)M_PI * freq_hz * t);
            float sample = sinf(2.0f * (float)M_PI * freq_hz * t + mod);
            buf[i] = (int16_t)(sample * TONE_AMP * 32767.0f * env);
        }
        // stereo: duplicate mono sample to L+R
        int16_t stereo[chunk * 2];
        for (int i = 0; i < n; i++) { stereo[i*2] = stereo[i*2+1] = buf[i]; }
        s_i2s.write((const uint8_t*)stereo, n * 4);
    }
}

static void silence(int duration_ms) {
    if (!s_ready) return;
    const int total = SAMPLE_RATE * duration_ms / 1000;
    const int chunk = 256;
    int16_t buf[chunk] = {};
    for (int s = 0; s < total; s += chunk) {
        int n = (s + chunk <= total) ? chunk : (total - s);
        int16_t stereo[chunk * 2] = {};
        s_i2s.write((const uint8_t*)stereo, n * 4);
    }
}

static void play_pcm(const int16_t* data, int len) {
    if (!s_ready) return;
    const int chunk = 128;
    int16_t buf[chunk * 2];
    for (int s = 0; s < len; s += chunk) {
        int n = (s + chunk <= len) ? chunk : (len - s);
        for (int i = 0; i < n; i++) {
            int16_t sample = attenuate_sample(data[s + i]);
            buf[i * 2]     = sample;  // L
            buf[i * 2 + 1] = sample;  // R
        }
        s_i2s.write((const uint8_t*)buf, n * 4);
    }
}

// ---- Public API ----

void sound_init(void) {
    pinMode(PA_EN, OUTPUT);
    digitalWrite(PA_EN, LOW);  // PA off during init to avoid pop
    s_ready = true;
    Serial.println("sound: ready (lazy I2S)");
}

bool sound_ready(void) { return s_ready; }

bool sound_audio_busy(void) {
    return s_playing || s_stream_active || sd_wav_busy();
}

bool sound_try_claim_audio(void) {
    if (!s_ready || sound_audio_busy()) return false;
    s_stream_active = true;
    return true;
}

bool sound_try_start_stream(void) {
    if (!s_ready || sound_audio_busy()) return false;
    if (!output_start()) return false;
    s_stream_active = true;
    return true;
}

bool sound_try_start_sd_stream(void) {
    if (!s_ready || s_playing || s_stream_active) return false;
    if (!output_start()) return false;
    s_stream_active = true;
    return true;
}

void sound_resume_output(void) {
    s_stream_active = false;
    output_stop();
    s_ready = true;
}

void sound_debug_tone(void) {
    if (!s_ready || sound_audio_busy()) {
        Serial.println("sound: not ready or busy");
        return;
    }
    if (!output_start()) {
        Serial.println("sound: output init failed");
        return;
    }
    s_playing = true;
    Serial.printf("sound: PA_EN=%d, tone test\n", digitalRead(PA_EN));
    play_tone(880.0f, 700, 0.0f, 1.5f);
    silence(80);
    play_tone(1320.0f, 700, 0.0f, 1.5f);
    s_playing = false;
    output_stop();
}

static void play_event_pcm(sound_event_t evt) {
    switch (evt) {
        case EVT_COMPLETE:    play_pcm(snd_finish,    snd_finish_len);    break;
        case EVT_ERROR:       play_pcm(snd_error,     snd_error_len);     break;
        case EVT_INPUT:       play_pcm(snd_input,     snd_input_len);     break;
        case EVT_START:       play_pcm(snd_start,     snd_start_len);     break;
        default: break;
    }
}

void sound_play(sound_event_t evt) {
    if (!s_ready || sound_audio_busy()) return;
    if (!output_start()) return;
    s_playing = true;
    play_event_pcm(evt);
    s_playing = false;
    output_stop();
}

static void async_sound_task(void *arg) {
    sound_event_t evt = (sound_event_t)(intptr_t)arg;
    play_event_pcm(evt);
    s_playing = false;
    output_stop();
    vTaskDelete(nullptr);
}

void sound_play_async(sound_event_t evt) {
    if (!s_ready || sound_audio_busy()) return;

    // Fish events are SD-only to keep the firmware image small.
    if (evt >= EVT_FISH_IDLE && evt <= EVT_FISH_HEAVY) {
        static const char* const fish_paths[] = {
            "/fish_idle.wav", "/fish_norm.wav",
            "/fish_active.wav", "/fish_heavy.wav"
        };
        const char* path = fish_paths[evt - EVT_FISH_IDLE];
        if (!sd_play_wav(path)) {
            Serial.printf("sound: fish SD wav unavailable: %s\n", path);
        }
        return;
    }

    s_playing = true;
    if (!output_start()) {
        s_playing = false;
        return;
    }
    if (xTaskCreate(async_sound_task, "fish_snd", 4096,
                    (void*)(intptr_t)evt, 5, nullptr) != pdPASS) {
        s_playing = false;
        output_stop();
    }
}

void sound_push_pcm(const int16_t* pcm, int n) {
    if (!s_ready || !s_stream_active) return;
    const int chunk = 128;
    int16_t buf[chunk * 2];
    for (int s = 0; s < n; s += chunk) {
        int c = (n - s < chunk) ? (n - s) : chunk;
        for (int i = 0; i < c; i++) {
            int16_t sample = attenuate_sample(pcm[s + i]);
            buf[i * 2]     = sample;
            buf[i * 2 + 1] = sample;
        }
        s_i2s.write((const uint8_t*)buf, c * 4);
    }
}

void sound_set_stream_active(bool active) {
    s_stream_active = active;
    if (!active) output_stop();
}
