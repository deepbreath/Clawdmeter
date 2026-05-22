#include "wifi_audio.h"
#include "audio_stream.h"
#include "wifi_manager.h"
#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiServer.h>

#define WIFI_AUDIO_PORT 8788
#define WIFI_AUDIO_MAX_FRAME 512
#define WIFI_AUDIO_IDLE_TIMEOUT_MS 10000

static WiFiServer s_server(WIFI_AUDIO_PORT);
static WiFiClient s_client;
static bool s_started = false;
static uint8_t s_len_buf[2];
static uint8_t s_frame[WIFI_AUDIO_MAX_FRAME];
static size_t s_len_read = 0;
static size_t s_frame_len = 0;
static size_t s_frame_read = 0;
static uint32_t s_last_rx_ms = 0;

static void reset_frame_state(void) {
    s_len_read = 0;
    s_frame_len = 0;
    s_frame_read = 0;
}

static void close_client(const char* reason) {
    if (s_client) {
        Serial.printf("wifi_audio: client closed (%s)\n", reason);
        s_client.stop();
    }
    reset_frame_state();
}

void wifi_audio_init(void) {
    reset_frame_state();
}

void wifi_audio_tick(void) {
    if (!wifi_manager_connected()) {
        if (s_client) close_client("wifi down");
        return;
    }

    if (!s_started) {
        s_server.begin();
        s_started = true;
        Serial.printf("wifi_audio: listening on port %d\n", WIFI_AUDIO_PORT);
    }

    if (!s_client || !s_client.connected()) {
        if (s_client) close_client("disconnected");
        WiFiClient incoming = s_server.accept();
        if (incoming) {
            s_client = incoming;
            s_client.setNoDelay(true);
            s_last_rx_ms = millis();
            reset_frame_state();
            Serial.println("wifi_audio: client connected");
        }
        return;
    }

    if (millis() - s_last_rx_ms > WIFI_AUDIO_IDLE_TIMEOUT_MS) {
        close_client("idle timeout");
        return;
    }

    while (s_client && s_client.connected() && s_client.available() > 0) {
        s_last_rx_ms = millis();

        if (s_len_read < sizeof(s_len_buf)) {
            int b = s_client.read();
            if (b < 0) return;
            s_len_buf[s_len_read++] = (uint8_t)b;
            if (s_len_read < sizeof(s_len_buf)) continue;

            s_frame_len = ((size_t)s_len_buf[0] << 8) | s_len_buf[1];
            s_frame_read = 0;
            if (s_frame_len == 0 || s_frame_len > WIFI_AUDIO_MAX_FRAME) {
                close_client("bad frame length");
                return;
            }
        }

        while (s_frame_read < s_frame_len && s_client.available() > 0) {
            int b = s_client.read();
            if (b < 0) return;
            s_frame[s_frame_read++] = (uint8_t)b;
        }

        if (s_frame_read == s_frame_len) {
            audio_stream_push(s_frame, s_frame_len);
            reset_frame_state();
        }
    }
}
