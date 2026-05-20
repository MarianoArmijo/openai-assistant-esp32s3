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
// After the assistant finishes its reply we keep the mic open for this long so
// the user can ask a follow-up without having to say the wake word again.
// If no speech is detected within the window, we fall back to wake-word mode.
#define FOLLOWUP_WINDOW_MS 2000

void oai_wakeword_init(void);
AssistantState oai_assistant_state(void);
void oai_wakeword_set_state(AssistantState state);
// Open a follow-up listening window: state becomes STREAMING_TO_OPENAI without
// the wake word, and reverts to IDLE_WAITING_WAKE if no speech is detected.
void oai_wakeword_start_followup_window(void);
// Feed mic PCM16 sampled at 24 kHz (10 ms = 240 samples). Wake-word module
// internally resamples to 16 kHz for Multinet/VAD.
void oai_wakeword_feed_24khz(const int16_t *pcm_24k, int samples_24k);
