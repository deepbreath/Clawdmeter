#pragma once
#include <stdbool.h>
#include <stdint.h>

// Initialize onboard TF card (SDMMC 1-bit: CLK=2, CMD=1, D0=3).
// Call once from setup() after sound_init().
bool sd_init(void);

bool sd_ready(void);
void sd_end(void);
void sd_print_status(void);
void sd_set_recording_busy(bool busy);
bool sd_dma_ready(void);
bool sd_dma_ready_for_recording(void);
uint32_t sd_dma_free(void);

// Asynchronously stream a 16 kHz / 16-bit WAV from SD through the speaker.
// Path must start with '/' (e.g. "/fish_idle.wav").
// Returns false if SD not ready, file not found, or another WAV is playing.
bool sd_play_wav(const char* path);

// True while a WAV task is active (guards against double-launch).
bool sd_wav_busy(void);

// Append one CSV row to /log/usage.csv (creates /log dir if needed).
void sd_log_usage(const char* row);
