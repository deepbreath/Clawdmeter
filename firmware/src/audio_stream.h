#pragma once
#include <stdint.h>
#include <stddef.h>

// Initialize Opus decoder and PSRAM ring buffer. Call once from setup().
void audio_stream_init(void);

// Called from BLE write callback with raw Opus frame bytes.
// One write = one 20ms Opus frame, except a single 0x00 byte = end-of-stream.
void audio_stream_push(const uint8_t* data, size_t len);

bool audio_stream_is_active(void);
