#include "assistant_text_layout.h"

#include <string.h>

uint32_t assistant_text_layout_hash(const char *text, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)text[i];
        hash *= 16777619u;
    }
    return hash;
}

bool assistant_text_layout_matches(const AssistantTextLayout &layout,
                                   size_t sourceLen,
                                   uint32_t hash,
                                   int maxChars) {
    return layout.valid &&
           layout.sourceLen == sourceLen &&
           layout.hash == hash &&
           layout.maxChars == maxChars;
}

void assistant_text_layout_prepare(AssistantTextLayout &layout,
                                   const char *convertedText,
                                   size_t sourceLen,
                                   uint32_t hash,
                                   int maxChars) {
    if (!convertedText) convertedText = "";
    if (maxChars < 1) maxChars = 1;
    if (maxChars > 127) maxChars = 127;

    strncpy(layout.text, convertedText, sizeof(layout.text) - 1);
    layout.text[sizeof(layout.text) - 1] = '\0';
    layout.sourceLen = sourceLen;
    layout.hash = hash;
    layout.maxChars = maxChars;
    layout.valid = true;
    layout.lineCount = 0;

    const int textLen = (int)strlen(layout.text);
    int pos = 0;
    while (pos < textLen && layout.lineCount < AssistantTextLayout::MAX_LINES) {
        int end = pos + maxChars;
        if (end > textLen) end = textLen;

        int nl = -1;
        for (int i = pos; i < end; i++) {
            if (layout.text[i] == '\n') {
                nl = i;
                break;
            }
        }
        if (nl >= 0) {
            end = nl;
        } else if (end < textLen) {
            int lastSpace = -1;
            for (int i = end; i > pos; i--) {
                if (layout.text[i] == ' ') {
                    lastSpace = i;
                    break;
                }
            }
            if (lastSpace > pos) end = lastSpace + 1;
        }

        int lineLen = end - pos;
        if (lineLen > 127) lineLen = 127;
        while (lineLen > 0 &&
               (layout.text[pos + lineLen - 1] == ' ' ||
                layout.text[pos + lineLen - 1] == '\t')) {
            lineLen--;
        }

        layout.start[layout.lineCount] = (uint16_t)pos;
        layout.len[layout.lineCount] = (uint8_t)lineLen;
        layout.lineCount++;

        pos = end;
        if (pos < textLen && layout.text[pos] == '\n') pos++;
    }
}
