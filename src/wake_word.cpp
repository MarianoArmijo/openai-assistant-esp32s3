#include <esp_log.h>
#include <esp_mn_iface.h>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>
#include <esp_timer.h>
#include <esp_vad.h>
#include <math.h>
#include <model_path.h>
#include <string.h>

#include <vector>

#include "main.h"

#define TAG "wake_word"
#define MULTINET_DURATION_MS 3000
#define MULTINET_THRESHOLD 0.15f  // lower = more sensitive, better far-field
#define WAKE_SAMPLE_RATE 16000
#define VAD_FRAME_MS 30
#define VAD_FRAME_SAMPLES (WAKE_SAMPLE_RATE * VAD_FRAME_MS / 1000)  // 480
// Minimum continuous speech-like audio before the VAD reports VAD_SPEECH.
// Kept high enough that brief acoustic events (speaker click, hiss tail,
// short reverberation of the assistant's own audio) do not trigger a turn.
#define VAD_MIN_SPEECH_MS 300
// Ignore VAD for this long after a wake-word transition: the wake-up "ding"
// otherwise echoes through the speaker into the mic and the VAD would count
// it as the user speaking, closing the turn before the user can begin.
#define VAD_WARMUP_MS 200

// Wake-up "ding" played as immediate user feedback so the user knows the
// device is now listening and can start speaking without hesitation.
#define BEEP_FREQ_HZ      1200
#define BEEP_DURATION_MS  80
#define BEEP_OUTPUT_RATE  24000
#define BEEP_SAMPLES      (BEEP_OUTPUT_RATE * BEEP_DURATION_MS / 1000)
#define BEEP_AMPLITUDE    6000

static AssistantState assistant_state = ASSISTANT_IDLE_WAITING_WAKE;
static srmodel_list_t *sr_models = nullptr;
static esp_mn_iface_t *multinet = nullptr;
static model_iface_data_t *multinet_data = nullptr;
static vad_handle_t vad_handle = nullptr;
static std::vector<int16_t> mn_input_buffer;
static std::vector<int16_t> vad_input_buffer;
static uint32_t silence_ms = 0;
static bool had_speech_in_turn = false;
static int64_t streaming_started_us = 0;
// True when STREAMING was entered automatically after the assistant reply (no
// wake word). If the user does not speak within FOLLOWUP_WINDOW_MS we abandon
// the turn and fall back to wake-word mode.
static bool followup_active = false;

static void play_wakeup_beep(void) {
  static int16_t tone[BEEP_SAMPLES];
  static bool initialized = false;
  if (!initialized) {
    const float two_pi_f =
        2.0f * 3.14159265358979f * BEEP_FREQ_HZ / BEEP_OUTPUT_RATE;
    const int ramp_in = BEEP_OUTPUT_RATE * 5 / 1000;
    const int ramp_out = BEEP_OUTPUT_RATE * 10 / 1000;
    for (int i = 0; i < BEEP_SAMPLES; i++) {
      float env = 1.0f;
      if (i < ramp_in) {
        env = (float)i / ramp_in;
      } else if (i > BEEP_SAMPLES - ramp_out) {
        env = (float)(BEEP_SAMPLES - i) / ramp_out;
      }
      tone[i] = (int16_t)(env * BEEP_AMPLITUDE * sinf(two_pi_f * i));
    }
    initialized = true;
  }
  oai_audio_output(tone, BEEP_SAMPLES);
}

static void transition_to(AssistantState new_state) {
  if (assistant_state == new_state) {
    return;
  }
  ESP_LOGI(TAG, "State: %d -> %d", assistant_state, new_state);
  assistant_state = new_state;
  if (new_state == ASSISTANT_STREAMING_TO_OPENAI) {
    silence_ms = 0;
    had_speech_in_turn = false;
    streaming_started_us = esp_timer_get_time();
    if (vad_handle != nullptr) {
      vad_reset_trigger(vad_handle);
    }
    vad_input_buffer.clear();
  } else {
    // Any non-STREAMING state cancels a pending follow-up window.
    followup_active = false;
  }
}

static void on_wake_word_detected(void) {
  ESP_LOGI(TAG, "Wake word detected: %s", WAKE_WORD_PHRASE);
  if (assistant_state == ASSISTANT_TTS_PLAYING) {
    ESP_LOGI(TAG, "Barge-in: interrupting TTS");
  }
  transition_to(ASSISTANT_STREAMING_TO_OPENAI);
  if (multinet != nullptr && multinet_data != nullptr) {
    multinet->clean(multinet_data);
  }
  mn_input_buffer.clear();
  // Audible confirmation so the user can speak immediately.
  play_wakeup_beep();
}

// Convert N samples @24kHz to (N*2/3) samples @16kHz via simple linear interp.
// 240 @24k -> 160 @16k (10 ms in both representations).
static void resample_24k_to_16k(const int16_t *in, int in_samples,
                                std::vector<int16_t> &out) {
  int out_samples = in_samples * 2 / 3;
  out.reserve(out.size() + out_samples);
  for (int i = 0; i < out_samples; i++) {
    float pos = i * 1.5f;
    int idx = (int)pos;
    float frac = pos - idx;
    int next = idx + 1;
    if (next >= in_samples) next = in_samples - 1;
    int32_t a = in[idx];
    int32_t b = in[next];
    int32_t v = a + (int32_t)((b - a) * frac);
    if (v > INT16_MAX) v = INT16_MAX;
    else if (v < INT16_MIN) v = INT16_MIN;
    out.push_back((int16_t)v);
  }
}

static void process_multinet(void) {
  if (multinet == nullptr || multinet_data == nullptr) {
    return;
  }
  const int chunk_size = multinet->get_samp_chunksize(multinet_data);
  while ((int)mn_input_buffer.size() >= chunk_size) {
    std::vector<int16_t> chunk(mn_input_buffer.begin(),
                               mn_input_buffer.begin() + chunk_size);
    esp_mn_state_t mn_state = multinet->detect(multinet_data, chunk.data());

    if (mn_state == ESP_MN_STATE_DETECTED) {
      esp_mn_results_t *result = multinet->get_results(multinet_data);
      if (result != nullptr && result->num > 0) {
        on_wake_word_detected();
        return;
      }
      multinet->clean(multinet_data);
    } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
      multinet->clean(multinet_data);
    }

    mn_input_buffer.erase(mn_input_buffer.begin(),
                          mn_input_buffer.begin() + chunk_size);
  }
}

static void process_vad(void) {
  if (vad_handle == nullptr) {
    return;
  }

  // Skip VAD while the wake-up beep / DAC tail is still being captured by
  // the mic. Audio is still streamed to OpenAI; we just don't let the VAD
  // mistake the beep echo for user speech.
  int64_t since_us = esp_timer_get_time() - streaming_started_us;
  if (since_us < VAD_WARMUP_MS * 1000LL) {
    vad_input_buffer.clear();
    return;
  }

  // Follow-up window: if the user does not start speaking within
  // FOLLOWUP_WINDOW_MS after the previous reply, revert to wake-word mode.
  if (followup_active && !had_speech_in_turn &&
      since_us > (int64_t)FOLLOWUP_WINDOW_MS * 1000LL) {
    ESP_LOGI(TAG, "Follow-up window expired — say \"%s\" to activate",
             WAKE_WORD_PHRASE);
    transition_to(ASSISTANT_IDLE_WAITING_WAKE);
    vad_input_buffer.clear();
    return;
  }

  while ((int)vad_input_buffer.size() >= VAD_FRAME_SAMPLES) {
    vad_state_t vad_state =
        vad_process_with_trigger(vad_handle, vad_input_buffer.data());

    if (vad_state == VAD_SPEECH) {
      if (!had_speech_in_turn) {
        ESP_LOGI(TAG, "Speech detected, listening...");
      }
      had_speech_in_turn = true;
      silence_ms = 0;
    } else if (had_speech_in_turn) {
      // Only count silence once the user has actually started speaking.
      // This gives them unlimited time to begin after the wake word "ding".
      silence_ms += VAD_FRAME_MS;
      if (silence_ms >= VAD_SILENCE_END_MS) {
        ESP_LOGI(TAG, "End of speech (silence %u ms)",
                 (unsigned)silence_ms);
        transition_to(ASSISTANT_TTS_PLAYING);
        silence_ms = 0;
        vad_input_buffer.clear();
        return;
      }
    }

    vad_input_buffer.erase(vad_input_buffer.begin(),
                           vad_input_buffer.begin() + VAD_FRAME_SAMPLES);
  }
}

void oai_wakeword_init(void) {
  sr_models = esp_srmodel_init("model");
  if (sr_models == nullptr || sr_models->num == -1) {
    ESP_LOGE(TAG, "Failed to load SR models from 'model' partition");
    return;
  }

  char *mn_name = esp_srmodel_filter(sr_models, ESP_MN_PREFIX, "en");
  if (mn_name == nullptr) {
    mn_name = esp_srmodel_filter(sr_models, ESP_MN_PREFIX, NULL);
  }
  if (mn_name == nullptr) {
    ESP_LOGE(TAG, "No Multinet model found (enable SR_MN_EN_MULTINET7_QUANT)");
    return;
  }

  ESP_LOGI(TAG, "Using Multinet model: %s", mn_name);
  multinet = esp_mn_handle_from_name(mn_name);
  multinet_data = multinet->create(mn_name, MULTINET_DURATION_MS);
  multinet->set_det_threshold(multinet_data, MULTINET_THRESHOLD);

  esp_mn_commands_clear();
  esp_mn_commands_add(1, (char *)WAKE_WORD_PHRASE);
  esp_mn_commands_update();
  multinet->print_active_speech_commands(multinet_data);

  // VAD_MODE_1 (mildly aggressive) detects quieter / far-field speech better
  // than the very aggressive modes. Bump higher if the VAD keeps the turn
  // open due to background noise, or lower if it misses your voice.
  vad_handle = vad_create_with_param(VAD_MODE_1, WAKE_SAMPLE_RATE, VAD_FRAME_MS,
                                     VAD_MIN_SPEECH_MS, VAD_SILENCE_END_MS);
  if (vad_handle == nullptr) {
    ESP_LOGE(TAG, "Failed to create VAD");
  }

  assistant_state = ASSISTANT_IDLE_WAITING_WAKE;
  ESP_LOGI(TAG, "Wake word ready. Say \"%s\" to activate.", WAKE_WORD_PHRASE);
}

AssistantState oai_assistant_state(void) { return assistant_state; }

void oai_wakeword_set_state(AssistantState state) { transition_to(state); }

void oai_wakeword_start_followup_window(void) {
  ESP_LOGI(TAG, "Follow-up window open: speak within %d ms (no \"%s\" needed)",
           FOLLOWUP_WINDOW_MS, WAKE_WORD_PHRASE);
  transition_to(ASSISTANT_STREAMING_TO_OPENAI);
  followup_active = true;
}

void oai_wakeword_feed_24khz(const int16_t *pcm_24k, int samples_24k) {
  if (pcm_24k == nullptr || samples_24k <= 0) {
    return;
  }

  switch (assistant_state) {
    case ASSISTANT_IDLE_WAITING_WAKE:
      resample_24k_to_16k(pcm_24k, samples_24k, mn_input_buffer);
      process_multinet();
      break;
    case ASSISTANT_TTS_PLAYING:
      // Ignore mic input while the speaker is playing: without echo
      // cancellation the assistant's own audio loops back and the wake-word
      // detector mistakes it for the user saying "suku". Buffers are kept
      // clean so no stale audio leaks into the next state.
      mn_input_buffer.clear();
      vad_input_buffer.clear();
      break;
    case ASSISTANT_STREAMING_TO_OPENAI:
      resample_24k_to_16k(pcm_24k, samples_24k, vad_input_buffer);
      process_vad();
      break;
  }
}
