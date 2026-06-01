#include "../assistant_config.h"
#include "../assistant_chat_protocol.h"
#include "../assistant_recording.h"
#include "../assistant_scroll.h"
#include "../assistant_text_layout.h"
#include "../assistant_tool_schema.h"
#include "../assistant_tool_utils.h"
#include "../assistant_weather.h"
#include "../audio_trim.h"
#include "../audio_wav.h"
#include "../conversation_history.h"
#include "../nanogpt_protocol.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint16_t readLe16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t readLe32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void testConfigDefaults() {
    AssistantConfig config;
    assistant_config_init(config);

    assert(strcmp(config.nanogptModel, DEFAULT_NANOGPT_MODEL) == 0);
    assert(strcmp(config.nanogptSttModel, DEFAULT_NANOGPT_STT_MODEL) == 0);
    assert(strcmp(config.timezone, DEFAULT_TIMEZONE) == 0);
    assert(config.webSearch);
    assert(config.ssid[0][0] == '\0');
    assert(config.nanogptKey[0] == '\0');
}

static void testConfigValueExtraction() {
    char out[16] = {};

    assert(assistant_config_extract_value("SSID = HomeNet", "SSID", out, sizeof(out)));
    assert(strcmp(out, "HomeNet") == 0);

    memset(out, 0, sizeof(out));
    assert(assistant_config_extract_value("  NANOGPT_KEY = \"secret-key\"\r\n", "NANOGPT_KEY", out, sizeof(out)));
    assert(strcmp(out, "secret-key") == 0);

    memset(out, 0, sizeof(out));
    assert(!assistant_config_extract_value("AP_SSID = wrong", "SSID", out, sizeof(out)));
    assert(out[0] == '\0');

    memset(out, 0, sizeof(out));
    assert(assistant_config_extract_value("NANOGPT_MODEL = abcdefghijklmnopqrstuvwxyz", "NANOGPT_MODEL", out, sizeof(out)));
    assert(strcmp(out, "abcdefghijklmno") == 0);
}

static void testConfigFalseValues() {
    assert(assistant_config_is_false("0"));
    assert(assistant_config_is_false("off"));
    assert(assistant_config_is_false("NO"));
    assert(assistant_config_is_false("False"));
    assert(!assistant_config_is_false("1"));
    assert(!assistant_config_is_false("on"));
    assert(!assistant_config_is_false(""));
    assert(!assistant_config_is_false(nullptr));
}

static void testWavHeader() {
    uint8_t header[WAV_HEADER_SIZE] = {};
    wav_build_pcm16_mono_header(header, 32000, 16000);

    assert(memcmp(header, "RIFF", 4) == 0);
    assert(readLe32(header + 4) == 32036);
    assert(memcmp(header + 8, "WAVE", 4) == 0);
    assert(memcmp(header + 12, "fmt ", 4) == 0);
    assert(readLe32(header + 16) == 16);
    assert(readLe16(header + 20) == 1);
    assert(readLe16(header + 22) == 1);
    assert(readLe32(header + 24) == 16000);
    assert(readLe32(header + 28) == 32000);
    assert(readLe16(header + 32) == 2);
    assert(readLe16(header + 34) == 16);
    assert(memcmp(header + 36, "data", 4) == 0);
    assert(readLe32(header + 40) == 32000);
}

static void testAudioTrimKeepsSpeechPadding() {
    int16_t samples[16000] = {};
    for (int i = 4000; i < 8000; i++) {
        samples[i] = (i & 1) ? 600 : -600;
    }

    AudioTrimWindow trim = audio_trim_voice_window(samples, 16000, 16000);
    assert(trim.offsetSamples == 640);
    assert(trim.sampleCount == 10560);
}

static void testAudioTrimKeepsAllSilentAudio() {
    int16_t samples[16000] = {};
    AudioTrimWindow trim = audio_trim_voice_window(samples, 16000, 16000);
    assert(trim.offsetSamples == 0);
    assert(trim.sampleCount == 16000);
}

static void testRecordingCaptureMath() {
    assert(assistant_recording_samples_to_pull(0, 100, 30) == 30);
    assert(assistant_recording_samples_to_pull(90, 100, 30) == 10);
    assert(assistant_recording_samples_to_pull(100, 100, 30) == 0);

    int16_t samples[] = { 3, 4, -3, -4 };
    assert(assistant_recording_rms(samples, 4) == 3);
    assert(assistant_recording_rms(nullptr, 4) == 0);
    assert(assistant_recording_rms(samples, 0) == 0);

    assert(assistant_recording_is_too_short(3999, 16000));
    assert(!assistant_recording_is_too_short(4000, 16000));
    assert(assistant_recording_is_too_short(1, 0));
}

static void testTextLayoutWrapping() {
    const char *text = "hello world\nabc def";
    uint32_t hash = assistant_text_layout_hash(text, strlen(text));

    AssistantTextLayout layout = {};
    assert(!assistant_text_layout_matches(layout, strlen(text), hash, 6));
    assistant_text_layout_prepare(layout, text, strlen(text), hash, 6);

    assert(assistant_text_layout_matches(layout, strlen(text), hash, 6));
    assert(layout.lineCount == 4);
    assert(layout.start[0] == 0);
    assert(layout.len[0] == 5);
    assert(strncmp(layout.text + layout.start[0], "hello", layout.len[0]) == 0);
    assert(strncmp(layout.text + layout.start[1], "world", layout.len[1]) == 0);
    assert(strncmp(layout.text + layout.start[2], "abc", layout.len[2]) == 0);
    assert(strncmp(layout.text + layout.start[3], "def", layout.len[3]) == 0);
}

static void testTextLayoutHardWrapsLongWords() {
    const char *text = "abcdefgh";
    AssistantTextLayout layout = {};
    assistant_text_layout_prepare(
        layout,
        text,
        strlen(text),
        assistant_text_layout_hash(text, strlen(text)),
        3);

    assert(layout.lineCount == 3);
    assert(layout.len[0] == 3);
    assert(layout.len[1] == 3);
    assert(layout.len[2] == 2);
}

static void testScrollAdvance() {
    assert(!assistant_scroll_active(100, 100, false));
    assert(assistant_scroll_active(101, 100, false));
    assert(!assistant_scroll_active(101, 100, true));
    assert(assistant_scroll_max_offset(240, 100) == 140);
    assert(assistant_scroll_max_offset(80, 100) == 0);

    AssistantScrollState state = { 0, false };
    assistant_scroll_advance(state, 240, 100);
    assert(state.offset == 50);
    assert(!state.done);
    assistant_scroll_advance(state, 240, 100);
    assert(state.offset == 100);
    assistant_scroll_advance(state, 240, 100);
    assert(state.offset == 140);
    assistant_scroll_advance(state, 240, 100);
    assert(state.offset == 0);
    assert(state.done);
}

static void testNanoGptMultipartLengths() {
    const char *boundary = "----ESP32Bnd9a7f3c";
    const char *model = "Whisper-Large-V3";

    size_t withoutLanguage = nanogpt_stt_content_length(boundary, model, "", WAV_HEADER_SIZE, 32000);
    size_t expectedWithoutLanguage =
        strlen("------ESP32Bnd9a7f3c\r\n"
               "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
               "Content-Type: audio/wav\r\n\r\n") +
        WAV_HEADER_SIZE +
        32000 +
        strlen("\r\n") +
        strlen("------ESP32Bnd9a7f3c\r\n"
               "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
               "Whisper-Large-V3\r\n") +
        strlen("------ESP32Bnd9a7f3c--\r\n");
    assert(withoutLanguage == expectedWithoutLanguage);
    assert(nanogpt_stt_language_part_length(boundary, "") == 0);

    size_t languagePart = nanogpt_stt_language_part_length(boundary, "en");
    assert(languagePart == strlen("------ESP32Bnd9a7f3c\r\n"
                                  "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
                                  "en\r\n"));
    assert(nanogpt_stt_content_length(boundary, model, "en", WAV_HEADER_SIZE, 32000) ==
           expectedWithoutLanguage + languagePart);
}

static void testConversationHistory() {
    ConversationHistory history(2, 100);

    history.add("u1", "a1");
    history.add("u2", "a2");
    assert(history.count() == 4);
    assert(history.at(0) == "u1");
    assert(history.at(3) == "a2");

    history.add("u3", "a3");
    assert(history.count() == 4);
    assert(history.at(0) == "u2");
    assert(history.at(1) == "a2");
    assert(history.at(2) == "u3");
    assert(history.at(3) == "a3");

    history.clear();
    assert(history.count() == 0);
    assert(history.bytes() == 0);
}

static void testConversationHistoryByteBudget() {
    ConversationHistory history(4, 10);

    history.add("1234", "56");
    assert(history.count() == 2);
    assert(history.bytes() == 6);

    history.add("abcd", "ef");
    assert(history.count() == 2);
    assert(history.at(0) == "abcd");
    assert(history.at(1) == "ef");
    assert(history.bytes() == 6);
}

static void testWeatherDescriptions() {
    assert(strcmp(weather_wmo_description(0), "clear") == 0);
    assert(strcmp(weather_wmo_description(2), "partly cloudy") == 0);
    assert(strcmp(weather_wmo_description(3), "overcast") == 0);
    assert(strcmp(weather_wmo_description(45), "foggy") == 0);
    assert(strcmp(weather_wmo_description(57), "drizzle") == 0);
    assert(strcmp(weather_wmo_description(67), "rain") == 0);
    assert(strcmp(weather_wmo_description(77), "snow") == 0);
    assert(strcmp(weather_wmo_description(82), "rain showers") == 0);
    assert(strcmp(weather_wmo_description(86), "snow showers") == 0);
    assert(strcmp(weather_wmo_description(99), "thunderstorm") == 0);
    assert(strcmp(weather_wmo_description(1234), "unknown conditions") == 0);
}

static const AssistantToolSchema *findTool(const char *name) {
    size_t count = 0;
    const AssistantToolSchema *tools = assistant_tool_schemas(&count);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(tools[i].name, name) == 0) return &tools[i];
    }
    return nullptr;
}

static void testToolSchemaRegistry() {
    size_t count = 0;
    const AssistantToolSchema *tools = assistant_tool_schemas(&count);
    assert(tools != nullptr);
    assert(count == 12);
    assert(strcmp(tools[0].name, "get_time") == 0);
    assert(strcmp(tools[count - 1].name, "list_recent_notes") == 0);

    const AssistantToolSchema *brightness = findTool("set_brightness");
    assert(brightness != nullptr);
    assert(brightness->paramCount == 1);
    assert(strcmp(brightness->params[0].name, "percent") == 0);
    assert(brightness->params[0].type == ASSISTANT_TOOL_PARAM_INTEGER);
    assert(brightness->params[0].required);

    const AssistantToolSchema *beep = findTool("play_beep");
    assert(beep != nullptr);
    assert(beep->paramCount == 2);
    assert(strcmp(beep->params[0].name, "frequency_hz") == 0);
    assert(!beep->params[0].required);
    assert(strcmp(beep->params[1].name, "duration_ms") == 0);
    assert(!beep->params[1].required);

    const AssistantToolSchema *saveNote = findTool("save_note");
    assert(saveNote != nullptr);
    assert(saveNote->paramCount == 1);
    assert(saveNote->params[0].type == ASSISTANT_TOOL_PARAM_STRING);
    assert(saveNote->params[0].required);
}

static void testChatProtocolConstants() {
    assert(ASSISTANT_CHAT_MAX_TOKENS == 1024);
    assert(ASSISTANT_CHAT_MAX_TOOL_ROUNDS == 6);
    assert(ASSISTANT_CHAT_TEMPERATURE > 0.39);
    assert(ASSISTANT_CHAT_TEMPERATURE < 0.41);
    assert(strstr(assistant_chat_system_prompt(), "Only call restart_device or power_off") != nullptr);

    assert(strcmp(assistant_chat_tool_arguments_or_empty("{\"percent\":50}"), "{\"percent\":50}") == 0);
    assert(strcmp(assistant_chat_tool_arguments_or_empty(""), "{}") == 0);
    assert(strcmp(assistant_chat_tool_arguments_or_empty(nullptr), "{}") == 0);
}

static void testToolUtilityClamps() {
    assert(assistant_clamp_brightness_percent(-1) == -1);
    assert(assistant_clamp_brightness_percent(0) == 0);
    assert(assistant_clamp_brightness_percent(101) == 100);

    assert(assistant_clamp_beep_frequency_hz(20) == 100);
    assert(assistant_clamp_beep_frequency_hz(440) == 440);
    assert(assistant_clamp_beep_frequency_hz(9000) == 8000);

    assert(assistant_clamp_beep_duration_ms(1) == 20);
    assert(assistant_clamp_beep_duration_ms(200) == 200);
    assert(assistant_clamp_beep_duration_ms(5000) == 2000);

    assert(assistant_clamp_note_count(0) == 1);
    assert(assistant_clamp_note_count(5) == 5);
    assert(assistant_clamp_note_count(99) == 20);
}

static void testOrientationDescriptions() {
    assert(strcmp(assistant_orientation_description(0.0f, 0.0f, 0.9f), "face up") == 0);
    assert(strcmp(assistant_orientation_description(0.0f, 0.0f, -0.9f), "face down") == 0);
    assert(strcmp(assistant_orientation_description(0.9f, 0.0f, 0.0f), "upright, top up") == 0);
    assert(strcmp(assistant_orientation_description(-0.9f, 0.0f, 0.0f), "upside down") == 0);
    assert(strcmp(assistant_orientation_description(0.0f, 0.8f, 0.0f), "on its side") == 0);
    assert(strcmp(assistant_orientation_description(0.1f, 0.1f, 0.1f), "tilted") == 0);
}

int main() {
    testConfigDefaults();
    testConfigValueExtraction();
    testConfigFalseValues();
    testWavHeader();
    testAudioTrimKeepsSpeechPadding();
    testAudioTrimKeepsAllSilentAudio();
    testRecordingCaptureMath();
    testTextLayoutWrapping();
    testTextLayoutHardWrapsLongWords();
    testScrollAdvance();
    testNanoGptMultipartLengths();
    testConversationHistory();
    testConversationHistoryByteBudget();
    testWeatherDescriptions();
    testToolSchemaRegistry();
    testChatProtocolConstants();
    testToolUtilityClamps();
    testOrientationDescriptions();
    puts("pure tests passed");
    return 0;
}
