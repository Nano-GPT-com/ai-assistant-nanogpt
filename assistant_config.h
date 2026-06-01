#pragma once

#include <stddef.h>

#define WIFI_SLOT_COUNT 3
#define WIFI_SSID_LEN 64
#define WIFI_PASSWORD_LEN 64
#define NANOGPT_KEY_LEN 128
#define NANOGPT_MODEL_LEN 96
#define NANOGPT_STT_MODEL_LEN 64
#define LANGUAGE_LEN 8
#define TIMEZONE_LEN 64
#define LOCATION_LEN 64

#define DEFAULT_NANOGPT_MODEL "openai/gpt-chat-latest"
#define DEFAULT_NANOGPT_STT_MODEL "Whisper-Large-V3"
#define DEFAULT_TIMEZONE "UTC0"

struct AssistantConfig {
    char ssid[WIFI_SLOT_COUNT][WIFI_SSID_LEN];
    char password[WIFI_SLOT_COUNT][WIFI_PASSWORD_LEN];
    char nanogptKey[NANOGPT_KEY_LEN];
    char nanogptModel[NANOGPT_MODEL_LEN];
    char nanogptSttModel[NANOGPT_STT_MODEL_LEN];
    char language[LANGUAGE_LEN];
    char timezone[TIMEZONE_LEN];
    char location[LOCATION_LEN];
    bool webSearch;
};

void assistant_config_init(AssistantConfig &config);
bool assistant_config_extract_value(const char *line, const char *key, char *out, size_t cap);
bool assistant_config_is_false(const char *value);

#ifndef UNIT_TEST
bool assistant_config_load(AssistantConfig &config);
#endif
