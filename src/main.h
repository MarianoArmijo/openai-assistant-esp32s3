#pragma once
#include <stdint.h>

#define LOG_TAG "realtimeapi-sdk"

void oai_wifi(void);
void oai_init_audio_capture(void);
void oai_audio_input(int16_t *buf, int samples);
void oai_audio_output(const int16_t *buf, int samples);
void oai_websocket(void);

// Wake word + local VAD
typedef enum {
  ASSISTANT_IDLE_WAITING_WAKE,
  ASSISTANT_STREAMING_TO_OPENAI,
  ASSISTANT_TTS_PLAYING,
} AssistantState;

#define WAKE_WORD_PHRASE "suku"
#define VAD_SILENCE_END_MS 600

void oai_wakeword_init(void);
AssistantState oai_assistant_state(void);
void oai_wakeword_set_state(AssistantState state);
// Feed mic PCM16 sampled at 24 kHz (10 ms = 240 samples). Wake-word module
// internally resamples to 16 kHz for Multinet/VAD.
void oai_wakeword_feed_24khz(const int16_t *pcm_24k, int samples_24k);
