#include "sound.h"
#include "sound_data.h"
#include "display_cfg.h"
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <driver/i2s_std.h>

#define SAMPLE_RATE  16000
#define TONE_AMP     0.65f   // 0..1, avoids clipping

static i2s_chan_handle_t s_tx = nullptr;
static bool s_ready = false;

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
    es_write(0x32, 0xB2);  // DAC volume: 70% (0xFF = 0dB, 0x00 = mute)
    es_write(0x33, 0x00);  // DAC offset = 0
    es_write(0x37, 0x08);  // DAC analog block on
    es_write(0x45, 0x00);  // output stage on

    Serial.println("ES8311 init OK");
    return true;
}

// ---- I2S setup ----

static bool i2s_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx, nullptr);
    if (ret != ESP_OK) {
        Serial.printf("I2S new channel error: %d\n", ret);
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = SAMPLE_RATE,
            .clk_src        = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)I2S_MCLK_ES8311,
            .bclk = (gpio_num_t)I2S_BCK,
            .ws   = (gpio_num_t)I2S_WS,
            .dout = (gpio_num_t)I2S_DO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (ret != ESP_OK) {
        Serial.printf("I2S init std mode error: %d\n", ret);
        return false;
    }

    ret = i2s_channel_enable(s_tx);
    if (ret != ESP_OK) {
        Serial.printf("I2S enable error: %d\n", ret);
        return false;
    }

    Serial.println("I2S init OK");
    return true;
}

// ---- Tone generator ----

// FM synthesis tone. mod_idx=0 → pure sine (bell); 0.28 → digital shimmer.
// Envelope: 4ms attack → exponential decay (tau = duration * tau_ratio).
static void play_tone(float freq_hz, int duration_ms,
                      float mod_idx = 0.28f, float tau_ratio = 0.38f) {
    if (!s_tx) return;
    const int total  = SAMPLE_RATE * duration_ms / 1000;
    const int chunk  = 256;
    const int attack = SAMPLE_RATE * 4 / 1000;
    const float tau  = (duration_ms * tau_ratio) / 1000.0f;
    int16_t buf[chunk];
    size_t written;

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
        i2s_channel_write(s_tx, stereo, n * 4, &written, pdMS_TO_TICKS(200));
    }
}

static void silence(int duration_ms) {
    if (!s_tx) return;
    const int total = SAMPLE_RATE * duration_ms / 1000;
    const int chunk = 256;
    int16_t buf[chunk] = {};
    size_t written;
    for (int s = 0; s < total; s += chunk) {
        int n = (s + chunk <= total) ? chunk : (total - s);
        int16_t stereo[chunk * 2] = {};
        i2s_channel_write(s_tx, stereo, n * 4, &written, pdMS_TO_TICKS(100));
    }
}

static void play_pcm(const int16_t* data, int len) {
    if (!s_tx) return;
    const int chunk = 128;
    int16_t buf[chunk * 2];
    size_t written;
    for (int s = 0; s < len; s += chunk) {
        int n = (s + chunk <= len) ? chunk : (len - s);
        for (int i = 0; i < n; i++) {
            buf[i * 2]     = data[s + i];  // L
            buf[i * 2 + 1] = data[s + i];  // R
        }
        i2s_channel_write(s_tx, buf, n * 4, &written, pdMS_TO_TICKS(500));
    }
}

// ---- Public API ----

void sound_init(void) {
    pinMode(PA_EN, OUTPUT);
    digitalWrite(PA_EN, LOW);  // PA off during init to avoid pop

    if (!i2s_init())   return;
    if (!es8311_init()) return;

    delay(50);
    digitalWrite(PA_EN, HIGH);  // enable PA after codec is ready
    s_ready = true;
}

bool sound_ready(void) { return s_ready; }

void sound_play(sound_event_t evt) {
    if (!s_ready) return;
    switch (evt) {
        case EVT_COMPLETE: play_pcm(snd_finish, snd_finish_len); break;
        case EVT_ERROR:    play_pcm(snd_error,  snd_error_len);  break;
        case EVT_INPUT:    play_pcm(snd_input,  snd_input_len);  break;
        case EVT_START:    play_pcm(snd_start,  snd_start_len);  break;
        default:
            break;
    }
}
