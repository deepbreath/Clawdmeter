#pragma once
#include <stdbool.h>
#include <stdint.h>

void recorder_init(void);
void recorder_tick(void);
bool recorder_toggle(void);
bool recorder_is_recording(void);
const char* recorder_status(void);
const char* recorder_last_wav_path(void);
void recorder_forget_last_wav_path(void);
void recorder_analyze_last_wav(void);
bool recorder_set_capture_mode(const char* mode);
const char* recorder_capture_mode(void);
bool recorder_set_sample_rate(uint32_t sample_rate);
uint32_t recorder_sample_rate(void);
