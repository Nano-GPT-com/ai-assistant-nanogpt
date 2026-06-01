#pragma once

#include <stdint.h>

#define WAV_HEADER_SIZE 44

void wav_build_pcm16_mono_header(uint8_t header[WAV_HEADER_SIZE], uint32_t dataBytes, uint32_t sampleRate);
