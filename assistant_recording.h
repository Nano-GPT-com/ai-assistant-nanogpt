#pragma once

#include <stdint.h>

uint32_t assistant_recording_samples_to_pull(uint32_t currentSamples,
                                             uint32_t maxSamples,
                                             uint32_t availableSamples);

int assistant_recording_rms(const int16_t *samples, uint32_t sampleCount);

bool assistant_recording_is_too_short(uint32_t sampleCount, uint32_t sampleRate);
