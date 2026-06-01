#include "assistant_recording.h"

#include <math.h>

uint32_t assistant_recording_samples_to_pull(uint32_t currentSamples,
                                             uint32_t maxSamples,
                                             uint32_t availableSamples) {
    if (currentSamples >= maxSamples) return 0;
    uint32_t remaining = maxSamples - currentSamples;
    return availableSamples < remaining ? availableSamples : remaining;
}

int assistant_recording_rms(const int16_t *samples, uint32_t sampleCount) {
    if (!samples || sampleCount == 0) return 0;

    int64_t sum = 0;
    for (uint32_t i = 0; i < sampleCount; i++) {
        int32_t sample = samples[i];
        sum += sample * sample;
    }
    return (int)sqrt((double)sum / sampleCount);
}

bool assistant_recording_is_too_short(uint32_t sampleCount, uint32_t sampleRate) {
    return sampleRate == 0 || sampleCount < sampleRate / 4;
}
