#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};

struct CodexData {
    float token_pct;      // current-minute token utilization (0-100)
    int   token_reset_s;  // seconds until token rate limit resets
    float req_pct;        // current-minute request utilization (0-100)
    bool  ok;
    bool  valid;          // false until first successful parse
};
