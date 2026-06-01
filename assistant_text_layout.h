#pragma once

#include <stddef.h>
#include <stdint.h>

struct AssistantTextLayout {
    static const int TEXT_CAP = 2048;
    static const int MAX_LINES = 128;

    char text[TEXT_CAP];
    uint16_t start[MAX_LINES];
    uint8_t len[MAX_LINES];
    int lineCount;
    int maxChars;
    uint32_t hash;
    size_t sourceLen;
    bool valid;
};

uint32_t assistant_text_layout_hash(const char *text, size_t len);

bool assistant_text_layout_matches(const AssistantTextLayout &layout,
                                   size_t sourceLen,
                                   uint32_t hash,
                                   int maxChars);

void assistant_text_layout_prepare(AssistantTextLayout &layout,
                                   const char *convertedText,
                                   size_t sourceLen,
                                   uint32_t hash,
                                   int maxChars);
