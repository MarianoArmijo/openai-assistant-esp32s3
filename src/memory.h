#pragma once

#include <stddef.h>
#include "esp_err.h"

#define OAI_MEM_BASE           "/mem"
#define OAI_MEM_SESSION_FILE   OAI_MEM_BASE "/session.jsonl"
#define OAI_MEM_LONGTERM_FILE  OAI_MEM_BASE "/MEMORY.md"

#define OAI_SESSION_MAX_MSGS   20
#define OAI_SESSION_MAX_BYTES  (32 * 1024)
#define OAI_INSTRUCTIONS_MAX (12 * 1024)

esp_err_t oai_memory_init(void);

/** Append a transcript line to session.jsonl (role: "user" or "assistant"). */
esp_err_t oai_memory_session_append(const char *role, const char *content);

/**
 * Build full session instructions: Vitalservit prompt + MEMORY.md + recent history.
 * Caller provides buffer (recommend OAI_INSTRUCTIONS_MAX).
 */
esp_err_t oai_memory_build_instructions(char *buf, size_t size);
