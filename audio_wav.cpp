#include "audio_wav.h"

#include <string.h>

void wav_build_pcm16_mono_header(uint8_t header[WAV_HEADER_SIZE], uint32_t dataBytes, uint32_t sampleRate) {
    const uint16_t channels = 1;
    const uint16_t bitsPerSample = 16;
    const uint16_t blockAlign = channels * bitsPerSample / 8;
    const uint32_t byteRate = sampleRate * blockAlign;
    const uint32_t chunkSize = 36 + dataBytes;
    const uint32_t subchunk1Size = 16;
    const uint16_t audioFormat = 1;

    memcpy(header, "RIFF", 4);
    memcpy(header + 4, &chunkSize, 4);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    memcpy(header + 16, &subchunk1Size, 4);
    memcpy(header + 20, &audioFormat, 2);
    memcpy(header + 22, &channels, 2);
    memcpy(header + 24, &sampleRate, 4);
    memcpy(header + 28, &byteRate, 4);
    memcpy(header + 32, &blockAlign, 2);
    memcpy(header + 34, &bitsPerSample, 2);
    memcpy(header + 36, "data", 4);
    memcpy(header + 40, &dataBytes, 4);
}
