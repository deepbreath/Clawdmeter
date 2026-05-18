#include "sd_phrases.h"
#include "sd_card.h"
#include <Arduino.h>
#include <SD_MMC.h>

#define GROUP_COUNT  4
#define MAX_PHRASES  99

static int  s_count[GROUP_COUNT] = {0};
static int  s_last[GROUP_COUNT]  = {-1, -1, -1, -1};
static bool s_any = false;

// Count WAV files inside /phrases/group_<g>/ by probing filenames 001…MAX.
static int _count_group(int g) {
    char path[48];
    int n = 0;
    for (int i = 1; i <= MAX_PHRASES; i++) {
        snprintf(path, sizeof(path), "/phrases/group_%d/%03d.wav", g, i);
        if (!SD_MMC.exists(path)) break;
        n++;
    }
    return n;
}

void sd_phrases_init(void) {
    if (!sd_ready()) return;
    for (int g = 0; g < GROUP_COUNT; g++) {
        s_count[g] = _count_group(g);
        if (s_count[g] > 0) s_any = true;
        Serial.printf("sd_phrases: group %d — %d phrases\n", g, s_count[g]);
    }
}

bool sd_phrases_available(void) { return s_any; }

bool sd_phrase_play(int group) {
    if (group < 0 || group >= GROUP_COUNT) return false;
    if (s_count[group] == 0) return false;

    int n = s_count[group];
    int idx;

    if (n == 1) {
        idx = 0;
    } else {
        // Pick randomly, avoid repeating the last phrase
        do {
            idx = (int)(esp_random() % (uint32_t)n);
        } while (idx == s_last[group]);
    }
    s_last[group] = idx;

    char path[48];
    snprintf(path, sizeof(path), "/phrases/group_%d/%03d.wav", group, idx + 1);
    return sd_play_wav(path);
}
