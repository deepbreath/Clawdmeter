#include "audio_stream.h"
#include "sound.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <opus.h>

// 16 kHz mono, 20 ms frames (matches daemon encoder settings)
#define AS_SAMPLE_RATE    16000
#define AS_FRAME_SAMPLES  320        // 16000 * 0.020

// PSRAM ring buffer: 300 frames = 6 s  (holds a full fish utterance before EOS)
#define AS_RING_SAMPLES   (AS_FRAME_SAMPLES * 300)

// Pre-buffer: don't start playback until this many samples are queued.
// Absorbs BLE delivery jitter without real-time pacing constraints.
#define AS_PREBUF_SAMPLES (AS_FRAME_SAMPLES * 15)   // 300 ms

// Frame type bytes (first byte of each BLE write to AUDIO_CHAR)
#define FRAME_EOS   0x00  // end-of-stream — no payload
#define FRAME_OPUS  0x01  // Opus-encoded frame
#define FRAME_ADPCM 0x02  // IMA-ADPCM: [pred_hi][pred_lo][step_idx][nibbles…]

// ---------------------------------------------------------------------------
// IMA-ADPCM tables (decoder only)
// ---------------------------------------------------------------------------
static const int s_step_tab[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28,
    31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337,
    371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
    1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
    3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int s_idx_tab[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

// ---------------------------------------------------------------------------
// Ring buffer (PSRAM)
// ---------------------------------------------------------------------------
static int16_t*     s_ring  = nullptr;
static volatile int s_head  = 0;
static volatile int s_tail  = 0;

static int ring_readable(void) {
    int h = s_head, t = s_tail;
    return (h >= t) ? (h - t) : (AS_RING_SAMPLES - t + h);
}
static int ring_writable(void) {
    return AS_RING_SAMPLES - ring_readable() - 1;
}
static void ring_push(const int16_t* src, int n) {
    for (int i = 0; i < n; i++) {
        s_ring[s_head] = src[i];
        s_head = (s_head + 1) % AS_RING_SAMPLES;
    }
}
static int ring_pop(int16_t* dst, int n) {
    int avail = ring_readable();
    if (n > avail) n = avail;
    for (int i = 0; i < n; i++) {
        dst[i] = s_ring[s_tail];
        s_tail = (s_tail + 1) % AS_RING_SAMPLES;
    }
    return n;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static OpusDecoder* s_dec     = nullptr;
static volatile bool s_active = false;
static volatile bool s_eos    = false;

// ---------------------------------------------------------------------------
// Decoders
// ---------------------------------------------------------------------------

static void decode_opus(const uint8_t* payload, size_t plen) {
    int16_t pcm[AS_FRAME_SAMPLES];
    int n = opus_decode(s_dec, payload, (opus_int32)plen, pcm, AS_FRAME_SAMPLES, 0);
    if (n > 0 && ring_writable() >= n) ring_push(pcm, n);
    else if (n < 0) Serial.printf("audio_stream: opus_decode %d\n", n);
}

static void decode_adpcm(const uint8_t* payload, size_t plen) {
    // payload: [pred_hi][pred_lo][step_idx][nibble bytes…]
    if (plen < 4) return;
    int16_t pred = (int16_t)((payload[0] << 8) | payload[1]);
    int     sidx = payload[2];
    if (sidx < 0) sidx = 0;
    if (sidx > 88) sidx = 88;

    int16_t pcm[AS_FRAME_SAMPLES];
    int n = 0;
    for (size_t i = 3; i < plen && n < AS_FRAME_SAMPLES; i++) {
        uint8_t byte = payload[i];
        for (int shift = 0; shift < 8 && n < AS_FRAME_SAMPLES; shift += 4) {
            uint8_t nib = (byte >> shift) & 0x0F;
            int step = s_step_tab[sidx];
            int diff = step >> 3;
            if (nib & 4) diff += step;
            if (nib & 2) diff += step >> 1;
            if (nib & 1) diff += step >> 2;
            if (nib & 8) pred -= diff; else pred += diff;
            if (pred >  32767) pred =  32767;
            if (pred < -32768) pred = -32768;
            sidx += s_idx_tab[nib & 0xF];
            if (sidx < 0)  sidx = 0;
            if (sidx > 88) sidx = 88;
            pcm[n++] = pred;
        }
    }
    if (n > 0 && ring_writable() >= n) ring_push(pcm, n);
}

// ---------------------------------------------------------------------------
// Playback task
// ---------------------------------------------------------------------------

static void playback_task(void*) {
    // Wait for pre-buffer to fill (absorbs BLE jitter).
    // Skip wait if EOS already received (very short utterance).
    while (ring_readable() < AS_PREBUF_SAMPLES && !s_eos)
        vTaskDelay(pdMS_TO_TICKS(5));

    int16_t buf[AS_FRAME_SAMPLES];
    while (!s_eos || ring_readable() > 0) {
        int n = ring_pop(buf, AS_FRAME_SAMPLES);
        if (n > 0) sound_push_pcm(buf, n);
        else       vTaskDelay(pdMS_TO_TICKS(2));
    }
    sound_set_stream_active(false);
    s_active = false;
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void audio_stream_init(void) {
    int err;
    s_dec = opus_decoder_create(AS_SAMPLE_RATE, 1, &err);
    if (!s_dec || err != OPUS_OK) {
        Serial.printf("audio_stream: opus_decoder_create failed (%d)\n", err);
        return;
    }
    s_ring = (int16_t*)heap_caps_malloc(AS_RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_ring) { Serial.println("audio_stream: PSRAM alloc failed"); return; }
    Serial.println("audio_stream: ready (Opus + ADPCM)");
}

void audio_stream_push(const uint8_t* data, size_t len) {
    if (!s_ring || len == 0) return;

    uint8_t ftype = data[0];

    if (ftype == FRAME_EOS) { s_eos = true; return; }
    if (len < 2) return;

    // Start playback task on first frame
    if (!s_active) {
        if (!sound_try_start_stream()) return;
        s_head = s_tail = 0;
        s_eos = false;
        s_active = true;
        xTaskCreate(playback_task, "opus_play", 6144, nullptr, 5, nullptr);
    }

    const uint8_t* payload = data + 1;
    size_t plen = len - 1;

    if (ftype == FRAME_OPUS && s_dec) decode_opus(payload, plen);
    else if (ftype == FRAME_ADPCM)   decode_adpcm(payload, plen);
}

bool audio_stream_is_active(void) { return s_active; }
