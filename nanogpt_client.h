#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdint.h>

struct NanoGptClientConfig {
    const char *apiKey;
    const char *sttModel;
    const char *language;
};

String nanogpt_client_transcribe(const NanoGptClientConfig &config,
                                 const int16_t *pcm,
                                 uint32_t numSamples,
                                 uint32_t sampleRate);

bool nanogpt_client_post_chat(const char *apiKey, JsonDocument &request, JsonDocument &response);
