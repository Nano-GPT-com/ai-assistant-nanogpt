#include "audio_trim.h"

#include <stdlib.h>

static uint32_t avgAbs(const int16_t *samples, uint32_t start, uint32_t end) {
    uint64_t sum = 0;
    for (uint32_t i = start; i < end; i++) {
        sum += (uint32_t)abs((int)samples[i]);
    }
    uint32_t n = end - start;
    return n ? (uint32_t)(sum / n) : 0;
}

AudioTrimWindow audio_trim_voice_window(const int16_t *samples,
                                        uint32_t sampleCount,
                                        uint32_t sampleRate) {
    AudioTrimWindow all = { 0, sampleCount };
    if (!samples || sampleCount == 0 || sampleRate == 0) return all;

    const uint32_t frame = sampleRate / 50;       // 20 ms
    const uint32_t pad = sampleRate / 5;          // 200 ms
    const uint32_t threshold = 120;               // average absolute PCM level
    if (frame == 0 || sampleCount < sampleRate / 2) return all;

    uint32_t firstVoice = sampleCount;
    for (uint32_t start = 0; start < sampleCount; start += frame) {
        uint32_t end = start + frame;
        if (end > sampleCount) end = sampleCount;
        if (avgAbs(samples, start, end) >= threshold) {
            firstVoice = start;
            break;
        }
    }
    if (firstVoice == sampleCount) return all;

    uint32_t lastVoiceEnd = 0;
    for (uint32_t end = sampleCount; end > 0;) {
        uint32_t start = (end > frame) ? end - frame : 0;
        if (avgAbs(samples, start, end) >= threshold) {
            lastVoiceEnd = end;
            break;
        }
        end = start;
    }
    if (lastVoiceEnd <= firstVoice) return all;

    uint32_t trimStart = (firstVoice > pad) ? firstVoice - pad : 0;
    uint32_t trimEnd = lastVoiceEnd + pad;
    if (trimEnd > sampleCount) trimEnd = sampleCount;
    if (trimEnd <= trimStart) return all;

    AudioTrimWindow trimmed = { trimStart, trimEnd - trimStart };
    return trimmed;
}
