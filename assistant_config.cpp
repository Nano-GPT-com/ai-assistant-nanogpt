#include "assistant_config.h"

#include <ctype.h>
#include <string.h>

#ifndef UNIT_TEST
#include <Arduino.h>
#include <SD_MMC.h>
#include <FS.h>
#include "pin_config.h"
#endif

static bool equalsIgnoreCase(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

void assistant_config_init(AssistantConfig &config) {
    memset(&config, 0, sizeof(config));
    strncpy(config.nanogptModel, DEFAULT_NANOGPT_MODEL, sizeof(config.nanogptModel) - 1);
    strncpy(config.nanogptSttModel, DEFAULT_NANOGPT_STT_MODEL, sizeof(config.nanogptSttModel) - 1);
    strncpy(config.timezone, DEFAULT_TIMEZONE, sizeof(config.timezone) - 1);
    config.webSearch = true;
}

bool assistant_config_extract_value(const char *line, const char *key, char *out, size_t cap) {
    if (!line || !key || !out || cap == 0) return false;

    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    size_t keyLen = strlen(key);
    if (strncmp(p, key, keyLen) != 0) return false;

    char after = p[keyLen];
    if (isalnum((unsigned char)after) || after == '_') return false;
    p += keyLen;
    while (*p == ' ' || *p == '=') p++;
    if (*p == '"') p++;

    size_t n = 0;
    while (*p && *p != '"' && *p != '\n' && *p != '\r' && n < cap - 1) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    return n > 0;
}

bool assistant_config_is_false(const char *value) {
    return value &&
        (!strcmp(value, "0") ||
         equalsIgnoreCase(value, "off") ||
         equalsIgnoreCase(value, "no") ||
         equalsIgnoreCase(value, "false"));
}

#ifndef UNIT_TEST
bool assistant_config_load(AssistantConfig &config) {
    assistant_config_init(config);

    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    if (!SD_MMC.begin("/sdcard", true)) return false;

    File file = SD_MMC.open("/setup/setup.txt");
    if (!file) {
        SD_MMC.end();
        return false;
    }

    char line[160];
    while (file.available()) {
        int len = file.readBytesUntil('\n', line, sizeof(line) - 1);
        line[len] = '\0';

        assistant_config_extract_value(line, "SSID", config.ssid[0], WIFI_SSID_LEN);
        assistant_config_extract_value(line, "PASSWORD", config.password[0], WIFI_PASSWORD_LEN);
        assistant_config_extract_value(line, "SSID2", config.ssid[1], WIFI_SSID_LEN);
        assistant_config_extract_value(line, "PASSWORD2", config.password[1], WIFI_PASSWORD_LEN);
        assistant_config_extract_value(line, "SSID3", config.ssid[2], WIFI_SSID_LEN);
        assistant_config_extract_value(line, "PASSWORD3", config.password[2], WIFI_PASSWORD_LEN);
        assistant_config_extract_value(line, "NANOGPT_KEY", config.nanogptKey, NANOGPT_KEY_LEN);
        assistant_config_extract_value(line, "NANOGPT_MODEL", config.nanogptModel, NANOGPT_MODEL_LEN);
        assistant_config_extract_value(line, "NANOGPT_STT_MODEL", config.nanogptSttModel, NANOGPT_STT_MODEL_LEN);
        assistant_config_extract_value(line, "LANGUAGE", config.language, LANGUAGE_LEN);
        assistant_config_extract_value(line, "TIMEZONE", config.timezone, TIMEZONE_LEN);
        assistant_config_extract_value(line, "LOCATION_1", config.location, LOCATION_LEN);

        char webSearch[8] = {};
        if (assistant_config_extract_value(line, "NANOGPT_WEBSEARCH", webSearch, sizeof(webSearch))) {
            config.webSearch = !assistant_config_is_false(webSearch);
        }
    }

    file.close();
    SD_MMC.end();
    return config.ssid[0][0] && config.nanogptKey[0];
}
#endif
