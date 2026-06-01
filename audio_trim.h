#pragma once

#include <stdint.h>

struct AudioTrimWindow {
    uint32_t offsetSamples;
    uint32_t sampleCount;
};

AudioTrimWindow audio_trim_voice_window(const int16_t *samples,
                                        uint32_t sampleCount,
                                        uint32_t sampleRate);
