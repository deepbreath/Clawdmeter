#pragma once
#include <stdbool.h>

// Scan SD for /phrases/group_0/ … /phrases/group_3/ at startup.
// Call once from setup() after sd_init().
void sd_phrases_init(void);

// Play a random (non-repeating) WAV for the given usage-rate group (0–3).
// Returns false if SD not ready, no phrases found, or audio already playing.
bool sd_phrase_play(int group);

// True when at least one group has phrases available on SD.
bool sd_phrases_available(void);
