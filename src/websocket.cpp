#include <esp_websocket_client.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "main.h"
#include "memory.h"

// OpenAI Realtime API GA — WebSocket endpoint
#define OPENAI_WS_URL "wss://api.openai.com/v1/realtime?model=" OPENAI_MODEL

// Audio input: 240 samples × 24 kHz = 10 ms chunks
#define AUDIO_IN_SAMPLES   240
#define AUDIO_IN_BYTES     (AUDIO_IN_SAMPLES * sizeof(int16_t))  // 480 bytes
// Base64 of 480 bytes = ceil(480/3)*4 = 640 bytes + null
#define BASE64_OUT_SIZE    644
// JSON wrapper + 640 base64 = ~700 bytes
#define AUDIO_MSG_MAX      768

// Accumulation buffer for fragmented WebSocket messages (in PSRAM)
#define ACCUM_BUF_SIZE     (64 * 1024)

static esp_websocket_client_handle_t s_client = NULL;
static volatile bool s_connected = false;     // WebSocket connected
static volatile bool s_session_ready = false; // session.updated received from server
static char *s_accum_buf = NULL;

// Build session.update JSON with Vitalservit prompt + SPIFFS context + input transcription.
static char *build_session_update_json(void) {
  char *instructions =
      (char *)heap_caps_malloc(OAI_INSTRUCTIONS_MAX, MALLOC_CAP_SPIRAM);
  if (!instructions) {
    ESP_LOGE(LOG_TAG, "OOM for instructions buffer");
    return nullptr;
  }
  oai_memory_build_instructions(instructions, OAI_INSTRUCTIONS_MAX);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "type", "session.update");
  cJSON *session = cJSON_CreateObject();
  cJSON_AddItemToObject(root, "session", session);
  cJSON_AddStringToObject(session, "type", "realtime");
  cJSON_AddStringToObject(session, "instructions", instructions);
  free(instructions);

  cJSON *audio = cJSON_CreateObject();
  cJSON_AddItemToObject(session, "audio", audio);

  cJSON *input = cJSON_CreateObject();
  cJSON_AddItemToObject(audio, "input", input);
  cJSON *in_fmt = cJSON_CreateObject();
  cJSON_AddItemToObject(input, "format", in_fmt);
  cJSON_AddStringToObject(in_fmt, "type", "audio/pcm");
  cJSON_AddNumberToObject(in_fmt, "rate", 24000);
  cJSON_AddNullToObject(input, "turn_detection");
  cJSON *transcription = cJSON_CreateObject();
  cJSON_AddItemToObject(input, "transcription", transcription);
  cJSON_AddStringToObject(transcription, "model", "gpt-4o-mini-transcribe");
  cJSON_AddStringToObject(transcription, "language", "es");

  cJSON *output = cJSON_CreateObject();
  cJSON_AddItemToObject(audio, "output", output);
  cJSON *out_fmt = cJSON_CreateObject();
  cJSON_AddItemToObject(output, "format", out_fmt);
  cJSON_AddStringToObject(out_fmt, "type", "audio/pcm");
  cJSON_AddNumberToObject(out_fmt, "rate", 24000);
  cJSON_AddStringToObject(output, "voice", "alloy");

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return json;
}

static void send_session_update(void) {
  char *json = build_session_update_json();
  if (!json) {
    return;
  }
  esp_websocket_client_send_text(s_client, json, strlen(json),
                                 pdMS_TO_TICKS(5000));
  ESP_LOGI(LOG_TAG, "session.update sent (%u bytes)", (unsigned)strlen(json));
  free(json);
}

static void persist_transcript_events(const char *msg) {
  cJSON *root = cJSON_Parse(msg);
  if (!root) {
    return;
  }

  cJSON *type_item = cJSON_GetObjectItem(root, "type");
  if (!cJSON_IsString(type_item)) {
    cJSON_Delete(root);
    return;
  }

  const char *type = type_item->valuestring;
  if (strcmp(type, "conversation.item.input_audio_transcription.completed") ==
      0) {
    cJSON *transcript = cJSON_GetObjectItem(root, "transcript");
    if (cJSON_IsString(transcript) && transcript->valuestring[0] != '\0') {
      oai_memory_session_append("user", transcript->valuestring);
    }
  } else if (strcmp(type, "response.output_audio_transcript.done") == 0) {
    cJSON *transcript = cJSON_GetObjectItem(root, "transcript");
    if (cJSON_IsString(transcript) && transcript->valuestring[0] != '\0') {
      oai_memory_session_append("assistant", transcript->valuestring);
    }
  }

  cJSON_Delete(root);
}

static void process_message(const char *msg) {
  persist_transcript_events(msg);

  if (strstr(msg, "\"session.updated\"")) {
    s_session_ready = true;
    ESP_LOGI(LOG_TAG, "Session ready — waiting for wake word \"%s\"",
             WAKE_WORD_PHRASE);
    return;
  }

  if (strstr(msg, "\"response.done\"")) {
    // Barge-in: the user already started a new turn (wake word) while we were
    // still finishing the previous reply. Don't clear their buffered audio
    // and don't open a follow-up window — they're already streaming.
    if (oai_assistant_state() == ASSISTANT_STREAMING_TO_OPENAI) {
      ESP_LOGI(LOG_TAG, "Response done during barge-in — keeping user turn");
      return;
    }

    // Cool-down lets the DAC + room reverberation fully die before we open
    // the mic for follow-up. Too short and the speaker tail re-triggers the
    // VAD into a feedback loop. 1500 ms = 683 ms DMA buffer + safety margin.
    vTaskDelay(pdMS_TO_TICKS(1500));

    static const char clear_buf[] =
        "{\"type\":\"input_audio_buffer.clear\"}";
    esp_websocket_client_send_text(s_client, clear_buf, strlen(clear_buf),
                                   pdMS_TO_TICKS(500));

    ESP_LOGI(LOG_TAG, "Response done — opening follow-up window (%d ms)",
             FOLLOWUP_WINDOW_MS);
    oai_wakeword_start_followup_window();
  }

  // Log non-audio-delta events (keep audio path quiet)
  if (!strstr(msg, "response.output_audio.delta")) {
    if (!strstr(msg, "response.output_audio_transcript.delta")) {
      ESP_LOGI(LOG_TAG, "Server event: %.200s", msg);
    }
    return;
  }

  const char *tag = "\"delta\":\"";
  const char *p = strstr(msg, tag);
  if (!p) return;
  p += strlen(tag);

  const char *end = strchr(p, '"');
  if (!end || end == p) return;

  size_t b64_len = (size_t)(end - p);
  size_t pcm_max = (b64_len * 3) / 4 + 4;
  int16_t *pcm = (int16_t *)malloc(pcm_max);
  if (!pcm) {
    ESP_LOGE(LOG_TAG, "OOM for audio output buffer (%d bytes)", (int)pcm_max);
    return;
  }

  size_t decoded = 0;
  int rc = mbedtls_base64_decode((uint8_t *)pcm, pcm_max, &decoded,
                                 (const unsigned char *)p, b64_len);

  static int s_chunk_count = 0;
  if (rc == 0 && decoded > 0) {
    s_chunk_count++;
    if (s_chunk_count <= 3 || s_chunk_count % 20 == 0) {
      ESP_LOGI(LOG_TAG, "Audio chunk #%d: b64=%d -> pcm=%d bytes",
               s_chunk_count, (int)b64_len, (int)decoded);
    }
    oai_audio_output(pcm, (int)(decoded / sizeof(int16_t)));
  } else {
    ESP_LOGW(LOG_TAG, "Base64 decode failed (rc=%d, b64_len=%d)", rc, (int)b64_len);
  }
  free(pcm);
}

static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data) {
  esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;

  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(LOG_TAG, "WebSocket connected to OpenAI Realtime API");
      s_connected = true;
      send_session_update();
      break;

    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGW(LOG_TAG, "WebSocket disconnected");
      s_connected = false;
      s_session_ready = false;
      vTaskDelay(pdMS_TO_TICKS(300));
      esp_restart();
      break;

    case WEBSOCKET_EVENT_DATA:
      if (d->op_code != 1 || !s_accum_buf) break;

      if ((int)d->payload_offset + d->data_len <= ACCUM_BUF_SIZE) {
        memcpy(s_accum_buf + d->payload_offset, d->data_ptr, d->data_len);

        bool complete = (d->payload_len > 0) &&
                        ((int)d->payload_offset + d->data_len >= d->payload_len);
        if (complete) {
          s_accum_buf[d->payload_offset + d->data_len] = '\0';
          process_message(s_accum_buf);
        }
      } else {
        ESP_LOGW(LOG_TAG, "WS message too large for accumulation buffer (%d bytes)",
                 (int)d->payload_offset + d->data_len);
      }
      break;

    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGE(LOG_TAG, "WebSocket error");
      break;

    default:
      break;
  }
}

static StaticTask_t s_audio_task_buf;

static void commit_and_request_response(void) {
  static const char commit_msg[] =
      "{\"type\":\"input_audio_buffer.commit\"}";
  static const char response_create[] =
      "{\"type\":\"response.create\"}";
  esp_websocket_client_send_text(s_client, commit_msg, strlen(commit_msg),
                                 pdMS_TO_TICKS(500));
  esp_websocket_client_send_text(s_client, response_create,
                                 strlen(response_create), pdMS_TO_TICKS(500));
  ESP_LOGI(LOG_TAG, "Turn closed locally: commit + response.create sent");
}

static void audio_send_task(void *arg) {
  static int16_t  pcm[AUDIO_IN_SAMPLES];
  static uint8_t  b64[BASE64_OUT_SIZE + 1];
  static char     msg[AUDIO_MSG_MAX];

  AssistantState last_state = oai_assistant_state();

  while (1) {
    if (!s_connected || !s_session_ready) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // Always read mic so the I2S DMA never overflows.
    oai_audio_input(pcm, AUDIO_IN_SAMPLES);

    // Feed wake-word / local VAD (may transition the state below).
    oai_wakeword_feed_24khz(pcm, AUDIO_IN_SAMPLES);

    AssistantState state = oai_assistant_state();

    // STREAMING -> TTS_PLAYING transition closes the turn for the server.
    if (last_state == ASSISTANT_STREAMING_TO_OPENAI &&
        state == ASSISTANT_TTS_PLAYING) {
      commit_and_request_response();
    }
    // STREAMING -> IDLE without commit means the follow-up window expired
    // without speech. Discard buffered audio so it does not pollute the next
    // turn or get committed accidentally.
    if (last_state == ASSISTANT_STREAMING_TO_OPENAI &&
        state == ASSISTANT_IDLE_WAITING_WAKE) {
      static const char clear_buf[] =
          "{\"type\":\"input_audio_buffer.clear\"}";
      esp_websocket_client_send_text(s_client, clear_buf, strlen(clear_buf),
                                     pdMS_TO_TICKS(500));
    }
    last_state = state;

    if (state != ASSISTANT_STREAMING_TO_OPENAI) {
      // Either waiting for "suku" or assistant is replying — don't burn tokens.
      continue;
    }

    // Base64-encode 10 ms of PCM and send as input_audio_buffer.append
    size_t b64_len = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &b64_len,
                          (const uint8_t *)pcm, AUDIO_IN_BYTES);
    b64[b64_len] = '\0';

    int msg_len = snprintf(msg, sizeof(msg),
                           "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}",
                           (char *)b64);
    if (msg_len > 0 && msg_len < (int)sizeof(msg)) {
      esp_websocket_client_send_text(s_client, msg, msg_len, pdMS_TO_TICKS(1000));
    }
  }
}

void oai_websocket(void) {
  s_accum_buf = (char *)heap_caps_malloc(ACCUM_BUF_SIZE + 1, MALLOC_CAP_SPIRAM);
  if (!s_accum_buf) {
    ESP_LOGE(LOG_TAG, "Failed to allocate WS accumulation buffer");
    esp_restart();
  }

  static char headers[256];
  snprintf(headers, sizeof(headers),
           "Authorization: Bearer %s\r\n",
           OPENAI_API_KEY);

  esp_websocket_client_config_t cfg = {};
  cfg.uri         = OPENAI_WS_URL;
  cfg.headers     = headers;
  cfg.buffer_size = 32768;  // 32KB receive buffer — handles audio delta messages
  cfg.task_stack  = 8192;
  cfg.reconnect_timeout_ms = 5000;
  cfg.network_timeout_ms   = 10000;

  s_client = esp_websocket_client_init(&cfg);
  esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
  esp_websocket_client_start(s_client);

  StackType_t *stack = (StackType_t *)heap_caps_malloc(
      8192 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
  xTaskCreateStaticPinnedToCore(audio_send_task, "audio_send", 8192,
                                NULL, 5, stack, &s_audio_task_buf, 0);

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    if (!esp_websocket_client_is_connected(s_client)) {
      ESP_LOGW(LOG_TAG, "WebSocket not connected, restarting");
      vTaskDelay(pdMS_TO_TICKS(300));
      esp_restart();
    }
  }
}
