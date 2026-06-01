#pragma once

#include <stddef.h>
#include <stdint.h>

#define NANOGPT_HOST "nano-gpt.com"
#define NANOGPT_PORT 443
#define NANOGPT_CHAT_PATH "/api/v1/chat/completions"
#define NANOGPT_TRANSCRIPTIONS_PATH "/api/v1/audio/transcriptions"

size_t nanogpt_stt_language_part_length(const char *boundary, const char *language);
size_t nanogpt_stt_content_length(const char *boundary,
                                  const char *model,
                                  const char *language,
                                  uint32_t wavHeaderBytes,
                                  uint32_t pcmBytes);
