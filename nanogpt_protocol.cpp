#include "nanogpt_protocol.h"

#include <string.h>

static size_t multipartTextPartLength(const char *boundary, const char *name, const char *value) {
    if (!value || !value[0]) return 0;
    return 2 + strlen(boundary) + 2 +
           strlen("Content-Disposition: form-data; name=\"") + strlen(name) + strlen("\"\r\n\r\n") +
           strlen(value) + 2;
}

size_t nanogpt_stt_language_part_length(const char *boundary, const char *language) {
    return multipartTextPartLength(boundary, "language", language);
}

size_t nanogpt_stt_content_length(const char *boundary,
                                  const char *model,
                                  const char *language,
                                  uint32_t wavHeaderBytes,
                                  uint32_t pcmBytes) {
    const size_t fileHeader =
        2 + strlen(boundary) + 2 +
        strlen("Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n") +
        strlen("Content-Type: audio/wav\r\n\r\n");
    const size_t fileEnd = 2;
    const size_t modelPart = multipartTextPartLength(boundary, "model", model);
    const size_t languagePart = nanogpt_stt_language_part_length(boundary, language);
    const size_t closing = 2 + strlen(boundary) + strlen("--\r\n");

    return fileHeader + wavHeaderBytes + pcmBytes + fileEnd + modelPart + languagePart + closing;
}
