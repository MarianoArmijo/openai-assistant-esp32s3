#pragma once
#include <stdint.h>

#define LOG_TAG "realtimeapi-sdk"

void oai_wifi(void);
void oai_init_audio_capture(void);
void oai_audio_input(int16_t *buf, int samples);
void oai_audio_output(const int16_t *buf, int samples);
void oai_websocket(void);
