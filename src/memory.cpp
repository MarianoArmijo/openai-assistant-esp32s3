#include "memory.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include <esp_log.h>
#include <esp_spiffs.h>
#include <cJSON.h>

#define TAG "memory"

#define VITALSERVIT_INSTRUCTIONS_TEXT \
    "You are a helpful voice assistant. Respond in Spanish. Be concise."

static size_t append_file_to_buf(char *buf, size_t size, size_t off,
                                 const char *path, const char *header) {
  FILE *f = fopen(path, "r");
  if (!f || off >= size - 1) {
    return off;
  }
  if (header && off < size - 1) {
    off += (size_t)snprintf(buf + off, size - off, "\n\n%s\n", header);
  }
  size_t n = fread(buf + off, 1, size - off - 1, f);
  off += n;
  buf[off] = '\0';
  fclose(f);
  return off;
}

static void session_compact(void) {
  FILE *f = fopen(OAI_MEM_SESSION_FILE, "r");
  if (!f) {
    return;
  }

  cJSON *messages[OAI_SESSION_MAX_MSGS];
  int count = 0;
  int write_idx = 0;

  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
      line[len - 1] = '\0';
    }
    if (line[0] == '\0') {
      continue;
    }

    cJSON *obj = cJSON_Parse(line);
    if (!obj) {
      continue;
    }

    if (count >= OAI_SESSION_MAX_MSGS) {
      cJSON_Delete(messages[write_idx]);
    }
    messages[write_idx] = obj;
    write_idx = (write_idx + 1) % OAI_SESSION_MAX_MSGS;
    if (count < OAI_SESSION_MAX_MSGS) {
      count++;
    }
  }
  fclose(f);

  if (count == 0) {
    return;
  }

  f = fopen(OAI_MEM_SESSION_FILE, "w");
  if (!f) {
    int start = (count < OAI_SESSION_MAX_MSGS) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
      cJSON_Delete(messages[(start + i) % OAI_SESSION_MAX_MSGS]);
    }
    return;
  }

  int start = (count < OAI_SESSION_MAX_MSGS) ? 0 : write_idx;
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % OAI_SESSION_MAX_MSGS;
    char *line_out = cJSON_PrintUnformatted(messages[idx]);
    if (line_out) {
      fprintf(f, "%s\n", line_out);
      free(line_out);
    }
    cJSON_Delete(messages[idx]);
  }
  fclose(f);
}

esp_err_t oai_memory_init(void) {
  esp_vfs_spiffs_conf_t conf = {
      .base_path = OAI_MEM_BASE,
      .partition_label = "storage",
      .max_files = 5,
      .format_if_mount_failed = true,
  };

  esp_err_t ret = esp_vfs_spiffs_register(&conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
    return ret;
  }

  size_t total = 0, used = 0;
  ret = esp_spiffs_info(conf.partition_label, &total, &used);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "SPIFFS mounted at %s (%u / %u bytes used)",
             OAI_MEM_BASE, (unsigned)used, (unsigned)total);
  }
  return ESP_OK;
}

esp_err_t oai_memory_session_append(const char *role, const char *content) {
  if (!role || !content || content[0] == '\0') {
    return ESP_ERR_INVALID_ARG;
  }

  FILE *f = fopen(OAI_MEM_SESSION_FILE, "a");
  if (!f) {
    ESP_LOGE(TAG, "Cannot open %s", OAI_MEM_SESSION_FILE);
    return ESP_FAIL;
  }

  cJSON *obj = cJSON_CreateObject();
  cJSON_AddStringToObject(obj, "role", role);
  cJSON_AddStringToObject(obj, "content", content);
  cJSON_AddNumberToObject(obj, "ts", (double)time(NULL));

  char *line = cJSON_PrintUnformatted(obj);
  cJSON_Delete(obj);

  if (line) {
    fprintf(f, "%s\n", line);
    free(line);
  }
  fclose(f);

  struct stat st;
  if (stat(OAI_MEM_SESSION_FILE, &st) == 0 &&
      st.st_size > (off_t)OAI_SESSION_MAX_BYTES) {
    session_compact();
  } else {
    /* Compact when message count may exceed limit (cheap: always ring-trim). */
    session_compact();
  }

  ESP_LOGI(TAG, "Session saved (%s): %.80s%s", role, content,
           strlen(content) > 80 ? "..." : "");
  return ESP_OK;
}

esp_err_t oai_memory_build_instructions(char *buf, size_t size) {
  if (!buf || size == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  size_t off = (size_t)snprintf(buf, size, "%s", VITALSERVIT_INSTRUCTIONS_TEXT);

  off = append_file_to_buf(buf, size, off, OAI_MEM_LONGTERM_FILE,
                           "MEMORIA PERSISTENTE:");

  FILE *f = fopen(OAI_MEM_SESSION_FILE, "r");
  if (f) {
    cJSON *messages[OAI_SESSION_MAX_MSGS];
    int count = 0;
    int write_idx = 0;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
      size_t len = strlen(line);
      if (len > 0 && line[len - 1] == '\n') {
        line[len - 1] = '\0';
      }
      if (line[0] == '\0') {
        continue;
      }

      cJSON *obj = cJSON_Parse(line);
      if (!obj) {
        continue;
      }

      if (count >= OAI_SESSION_MAX_MSGS) {
        cJSON_Delete(messages[write_idx]);
      }
      messages[write_idx] = obj;
      write_idx = (write_idx + 1) % OAI_SESSION_MAX_MSGS;
      if (count < OAI_SESSION_MAX_MSGS) {
        count++;
      }
    }
    fclose(f);

    if (count > 0 && off < size - 32) {
      off += (size_t)snprintf(buf + off, size - off,
                              "\n\nHISTORIAL RECIENTE:\n");
      int start = (count < OAI_SESSION_MAX_MSGS) ? 0 : write_idx;
      for (int i = 0; i < count && off < size - 64; i++) {
        int idx = (start + i) % OAI_SESSION_MAX_MSGS;
        cJSON *obj = messages[idx];
        cJSON *role = cJSON_GetObjectItem(obj, "role");
        cJSON *content = cJSON_GetObjectItem(obj, "content");
        if (!cJSON_IsString(role) || !cJSON_IsString(content)) {
          continue;
        }
        const char *label =
            (strcmp(role->valuestring, "user") == 0) ? "Usuario" : "Asistente";
        off += (size_t)snprintf(buf + off, size - off, "%s: %s\n", label,
                                content->valuestring);
      }
      for (int i = 0; i < count; i++) {
        int idx = (start + i) % OAI_SESSION_MAX_MSGS;
        cJSON_Delete(messages[idx]);
      }
    }
  }

  ESP_LOGI(TAG, "Instructions built (%u bytes)", (unsigned)strlen(buf));
  return ESP_OK;
}
