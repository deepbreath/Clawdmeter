#pragma once
#include <stdint.h>
#include "data.h"

void sound_init(void);
void sound_play(sound_event_t evt);
void sound_play_async(sound_event_t evt);  // non-blocking; skips if already playing
bool sound_ready(void);
void sound_debug_tone(void);
bool sound_audio_busy(void);
bool sound_try_claim_audio(void);
bool sound_try_start_stream(void);
bool sound_try_start_sd_stream(void);
void sound_resume_output(void);

// Low-level I2S access for audio_stream (BLE Opus streaming)
void sound_push_pcm(const int16_t* pcm, int n);   // write n mono samples to I2S (blocking ~n/16kHz s)
void sound_set_stream_active(bool active);          // claim/release I2S for streaming
