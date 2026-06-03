/*
 * app_nanogpt_assistant.cpp — Voice-to-text AI assistant (NanoGPT STT + chat)
 *
 * Push-to-talk: hold BOOT to speak, release to get a text answer.
 * Uses one NanoGPT call for speech-to-text and one NanoGPT call for the reply:
 *   1. Whisper STT  (audio → text) via nano-gpt.com
 *   2. NanoGPT Chat  (text → text)  via nano-gpt.com
 *
 * Config from SD /setup/setup.txt:
 *   SSID / PASSWORD   — WiFi credentials
 *   NANOGPT_KEY       — NanoGPT API key (nano-gpt.com/api)
 *   NANOGPT_MODEL     — optional model id. Defaults to openai/gpt-chat-latest.
 *   NANOGPT_STT_MODEL — optional STT model id. Defaults to Whisper-Large-V3.
 *   NANOGPT_WEBSEARCH — "0" / "off" / "no" / "false" disables NanoGPT web
 *                       search. Anything else (incl. missing key) leaves it ON.
 *   LANGUAGE          — optional ISO-639-1 hint for Whisper (e.g. de, en).
 *                       Omit or leave empty for auto-detection.
 */

#include "app_nanogpt_assistant.h"
#include "assistant_config.h"
#include "assistant_chat_protocol.h"
#include "assistant_tool_schema.h"
#include "assistant_tool_utils.h"
#include "assistant_recording.h"
#include "assistant_notes.h"
#include "assistant_scroll.h"
#include "assistant_text_layout.h"
#include "assistant_weather.h"
#include "assistant_weather_client.h"
#include "app_common.h"
#include "audio_engine.h"
#include "audio_trim.h"
#include "conversation_history.h"
#include "nanogpt_client.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <FS.h>
#include <time.h>
#include <math.h>
#include "SensorQMI8658.hpp"
#include "canvas/Arduino_Canvas.h"
#include "pin_config.h"
#include "HWCDC.h"
#include "Fonts/FreeMonoBold12pt7b.h"

extern USBCDC USBSerial;
extern Arduino_Canvas *g_canvas;

// ── Constants ────────────────────────────────────────────────────────────────
#define BOOT_BTN        0
#define SAMPLE_RATE     16000
#define MAX_REC_S       30
#define MAX_REC_SAMPLES (SAMPLE_RATE * MAX_REC_S)   // 480000
#define MAX_REC_BYTES   (MAX_REC_SAMPLES * 2)        // 960000
#define MAX_HISTORY     6
#define MAX_HIST_BYTES  4096   // prune when total history text exceeds this

// ── State ────────────────────────────────────────────────────────────────────
enum GState {
    GS_INIT,
    GS_IDLE,
    GS_LISTENING,
    GS_TRANSCRIBING,
    GS_THINKING,
    GS_ERROR
};

static Arduino_Canvas *canvas    = nullptr;
static GState   s_state          = GS_INIT;
static bool     s_bootWas        = false;
static uint32_t s_lastDraw       = 0;
static uint32_t s_recStartMs     = 0;
static uint32_t s_lastDrawRecSec = UINT32_MAX;
static int      s_lastDrawRmsBucket = -1;

// Config
static AssistantConfig s_config;
static float    s_locLat         = 0.0f;
static float    s_locLon         = 0.0f;
static bool     s_locGeocoded    = false;

// Tools / agent state
static Arduino_OLED *s_gfx       = nullptr; // for set_brightness
static SensorQMI8658 s_imu;
static bool     s_imuOk          = false;
static bool     s_pendingRestart = false;
static bool     s_pendingPowerOff = false;

// Recording buffer (PSRAM)
static int16_t *s_recBuf         = nullptr;
static uint32_t s_recCount       = 0;
static int      s_recRms         = 0;      // current RMS level for VU meter

// Display text
static String   s_userText;
static String   s_agentText;
static String   s_errorMsg;

static AssistantTextLayout s_userLayout = {};
static AssistantTextLayout s_agentLayout = {};
static AssistantTextLayout s_errorLayout = {};

// Scroll state: BOOT advances pages when the latest reply overflows the viewport.
static int      s_scrollY        = 0;

#define PAGE_VIEW_H   (LCD_HEIGHT - 130)          // visible text height

static int      s_contentH       = 0;
static bool     s_scrollDone     = false;  // user pressed BOOT at end → scroll
                                           // is dismissed; next press starts talk

// "scroll" mode is active when the latest answer doesn't fit on one screen
// AND the user hasn't yet pressed BOOT past the bottom (which dismisses
// scroll-mode and brings the pill back to "talk").
static bool scrollModeActive() {
    return assistant_scroll_active(s_contentH, PAGE_VIEW_H, s_scrollDone);
}

static int16_t logoScaleX(int16_t originX, int16_t width, int16_t x) {
    return originX + (int32_t)x * width / 756;
}

static int16_t logoScaleY(int16_t originY, int16_t height, int16_t y) {
    return originY + (int32_t)y * height / 779;
}

static void fillLogoTriangle(int16_t originX, int16_t originY,
                             int16_t width, int16_t height,
                             int16_t x0, int16_t y0,
                             int16_t x1, int16_t y1,
                             int16_t x2, int16_t y2,
                             uint16_t color) {
    canvas->fillTriangle(
        logoScaleX(originX, width, x0), logoScaleY(originY, height, y0),
        logoScaleX(originX, width, x1), logoScaleY(originY, height, y1),
        logoScaleX(originX, width, x2), logoScaleY(originY, height, y2),
        color);
}

static void fillLogoQuad(int16_t originX, int16_t originY,
                         int16_t width, int16_t height,
                         int16_t x0, int16_t y0,
                         int16_t x1, int16_t y1,
                         int16_t x2, int16_t y2,
                         int16_t x3, int16_t y3,
                         uint16_t color) {
    fillLogoTriangle(originX, originY, width, height, x0, y0, x1, y1, x2, y2, color);
    fillLogoTriangle(originX, originY, width, height, x0, y0, x2, y2, x3, y3, color);
}

static void drawNanoGptDiamond(int16_t cx, int16_t cy, int16_t size) {
    const int16_t width = size;
    const int16_t height = (int32_t)size * 779 / 756;
    const int16_t x = cx - width / 2;
    const int16_t y = cy - height / 2;

    fillLogoQuad(x, y, width, height, 0, 327, 125, 178, 224, 224, 152, 405, 0x04B5);
    fillLogoTriangle(x, y, width, height, 50, 382, 150, 436, 276, 641, 0x0BB5);
    fillLogoQuad(x, y, width, height, 183, 404, 294, 116, 443, 11, 611, 267, 0x16D7);
    fillLogoTriangle(x, y, width, height, 183, 404, 611, 267, 294, 116, 0x15F7);
    fillLogoTriangle(x, y, width, height, 181, 432, 608, 300, 390, 768, 0x04B5);
    fillLogoTriangle(x, y, width, height, 181, 432, 390, 768, 280, 606, 0x0C36);
    fillLogoQuad(x, y, width, height, 455, 697, 641, 296, 746, 340, 545, 600, 0x15F7);
    fillLogoQuad(x, y, width, height, 539, 104, 561, 88, 725, 300, 643, 265, 0x16D7);
}

static bool recordingMeterNeedsRedraw(uint32_t now) {
    uint32_t recSec = (now - s_recStartMs) / 1000;
    int rmsBucket = s_recRms / 250;
    if (recSec != s_lastDrawRecSec || rmsBucket != s_lastDrawRmsBucket) {
        s_lastDrawRecSec = recSec;
        s_lastDrawRmsBucket = rmsBucket;
        return true;
    }
    return false;
}

static int drainRecordingSamples() {
    int lastPulled = 0;
    for (;;) {
        uint32_t toPull = assistant_recording_samples_to_pull(
            s_recCount,
            MAX_REC_SAMPLES,
            (uint32_t)audio_engine_rx_avail());
        if (toPull == 0) break;

        int got = audio_engine_pull(s_recBuf + s_recCount, (int)toPull);
        if (got <= 0) break;

        s_recRms = assistant_recording_rms(s_recBuf + s_recCount, (uint32_t)got);
        s_recCount += (uint32_t)got;
        lastPulled = got;
        if (s_recCount >= MAX_REC_SAMPLES) break;
    }
    return lastPulled;
}

// Conversation history (alternating user / assistant)
static ConversationHistory s_history(MAX_HISTORY, MAX_HIST_BYTES);

// ── WiFi ────────────────────────────────────────────────────────────────────
static bool wifiConnect() {
    WifiCred list[3] = {
        { s_config.ssid[0], s_config.password[0] },
        { s_config.ssid[1], s_config.password[1] },
        { s_config.ssid[2], s_config.password[2] },
    };
    return wifi_try_connect(list, 3) >= 0;
}

// ── Tool implementations ────────────────────────────────────────────────────
static String tool_get_time(JsonVariant input) {
    (void)input;
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 100)) return "time not yet synced";
    char buf[80];
    strftime(buf, sizeof(buf), "%A, %d %B %Y, %H:%M:%S", &timeinfo);
    return String(buf);
}

static String tool_get_battery_status(JsonVariant input) {
    (void)input;
    if (!power.isBatteryConnect()) return "no battery";
    int pct = (int)power.getBatteryPercent();
    bool charging = power.isCharging();
    char buf[64];
    snprintf(buf, sizeof(buf), "%d%%%s", pct, charging ? ", charging" : "");
    return String(buf);
}

static String tool_get_uptime(JsonVariant input) {
    (void)input;
    uint32_t s = millis() / 1000;
    uint32_t h = s / 3600;
    uint32_t m = (s % 3600) / 60;
    uint32_t sec = s % 60;
    char buf[64];
    snprintf(buf, sizeof(buf), "%uh %um %us", (unsigned)h, (unsigned)m, (unsigned)sec);
    return String(buf);
}

static String tool_get_wifi_info(JsonVariant input) {
    (void)input;
    if (WiFi.status() != WL_CONNECTED) return "WiFi not connected";
    char buf[128];
    snprintf(buf, sizeof(buf), "%s (%d dBm)", WiFi.SSID().c_str(), WiFi.RSSI());
    return String(buf);
}

static String tool_get_orientation(JsonVariant input) {
    (void)input;
    if (!s_imuOk) return "IMU unavailable";
    float ax = 0, ay = 0, az = 0;
    int got = 0;
    for (int i = 0; i < 24 && got < 8; i++) {
        if (s_imu.getDataReady()) {
            float x, y, z;
            s_imu.getAccelerometer(x, y, z);
            ax += x; ay += y; az += z;
            got++;
        }
        delay(5);
    }
    if (got == 0) return "IMU read failed";
    ax /= got; ay /= got; az /= got;

    float pitch = atan2f(ax, sqrtf(ay*ay + az*az)) * 180.0f / (float)M_PI;
    float roll  = atan2f(ay, az) * 180.0f / (float)M_PI;

    const char *desc = assistant_orientation_description(ax, ay, az);

    char buf[128];
    snprintf(buf, sizeof(buf), "pitch %.1f deg, roll %.1f deg (%s)",
             pitch, roll, desc);
    return String(buf);
}

static String tool_set_brightness(JsonVariant input) {
    int pct = input["percent"] | -1;
    pct = assistant_clamp_brightness_percent(pct);
    if (pct < 0) return "missing 'percent' (0-100)";
    if (s_gfx) {
        int b255 = (pct * 255 + 50) / 100;
        s_gfx->setBrightness(b255);
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "brightness set to %d%%", pct);
    return String(buf);
}

static String tool_play_beep(JsonVariant input) {
    int freq  = input["frequency_hz"] | 1000;
    int durMs = input["duration_ms"]  | 200;
    freq = assistant_clamp_beep_frequency_hz(freq);
    durMs = assistant_clamp_beep_duration_ms(durMs);

    int totalSamples = (int)((int64_t)durMs * SAMPLE_RATE / 1000);
    int halfPeriod = SAMPLE_RATE / freq / 2;
    if (halfPeriod < 1) halfPeriod = 1;

    int16_t beep[256];
    audio_engine_play();
    for (int s = 0; s < totalSamples; s += 256) {
        int chunk = (s + 256 <= totalSamples) ? 256 : (totalSamples - s);
        for (int i = 0; i < chunk; i++) {
            beep[i] = (((s + i) / halfPeriod) & 1) ? 12000 : -12000;
        }
        // Wait until ring has room
        int waits = 0;
        while (audio_engine_tx_space() < chunk && waits++ < 100) delay(10);
        audio_engine_push(beep, chunk);
    }
    // Let the buffer drain before turning TX off
    delay(durMs + 100);
    audio_engine_stop();

    char buf[64];
    snprintf(buf, sizeof(buf), "beep: %d Hz for %d ms", freq, durMs);
    return String(buf);
}

static String tool_restart_device(JsonVariant input) {
    (void)input;
    s_pendingRestart = true;
    return "restarting after this reply";
}

static String tool_power_off(JsonVariant input) {
    (void)input;
    s_pendingPowerOff = true;
    return "powering off after this reply";
}

// ── Weather (open-meteo) ────────────────────────────────────────────────────
static bool geocodeLocation() {
    if (s_locGeocoded) return true;
    if (s_config.location[0] == '\0') return false;

    char enc[160];
    if (!assistant_weather_encode_location(s_config.location, enc, sizeof(enc))) return false;

    char url[256];
    assistant_weather_geocode_url(enc, url, sizeof(url));

    WiFiClientSecure cli; cli.setInsecure();
    HTTPClient http;
    http.begin(cli, url);
    http.setTimeout(10000);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }

    JsonDocument doc;
    deserializeJson(doc, http.getStream());
    http.end();
    JsonArray r = doc["results"];
    if (r.isNull() || r.size() == 0) return false;
    s_locLat = r[0]["latitude"].as<float>();
    s_locLon = r[0]["longitude"].as<float>();
    s_locGeocoded = true;
    USBSerial.printf("[nanogpt] geocoded '%s' → %.4f, %.4f\n",
                     s_config.location, s_locLat, s_locLon);
    return true;
}

static String tool_get_weather(JsonVariant input) {
    (void)input;
    if (s_config.location[0] == '\0')
        return "no LOCATION_1 configured in /setup/setup.txt";
    if (!geocodeLocation())
        return "could not geocode location";

    char url[400];
    assistant_weather_forecast_url(s_locLat, s_locLon, url, sizeof(url));

    WiFiClientSecure cli; cli.setInsecure();
    HTTPClient http;
    http.begin(cli, url);
    http.setTimeout(15000);
    int code = http.GET();
    if (code != 200) {
        http.end();
        char b[40];
        snprintf(b, sizeof(b), "weather HTTP %d", code);
        return String(b);
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) return "weather JSON parse failed";
    JsonObject cur = doc["current"];
    if (cur.isNull()) return "weather: no 'current' data";

    float temp   = cur["temperature_2m"].as<float>();
    int   hum    = cur["relative_humidity_2m"].as<int>();
    float wind   = cur["wind_speed_10m"].as<float>();
    float precip = cur["precipitation"].as<float>();
    int   wmo    = cur["weather_code"].as<int>();
    const char *desc = weather_wmo_description(wmo);

    char buf[220];
    snprintf(buf, sizeof(buf),
        "%s in %s: %.0f C, humidity %d%%, wind %.0f km/h, precip %.1f mm",
        desc, s_config.location, temp, hum, wind, precip);
    return String(buf);
}

// ── Notes on SD card ────────────────────────────────────────────────────────
static String tool_save_note(JsonVariant input) {
    const char *text = input["text"] | "";
    if (!*text) return "empty note ignored";

    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    if (!SD_MMC.begin("/sdcard", true)) return "SD card unavailable";
    SD_MMC.mkdir("/notes");

    char filename[64];
    char timeBuf[16] = "??:??";
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 100)) {
        strftime(filename, sizeof(filename), "/notes/%Y-%m-%d.txt", &timeinfo);
        strftime(timeBuf,  sizeof(timeBuf),  "%H:%M",                &timeinfo);
    } else {
        strcpy(filename, "/notes/unsynced.txt");
    }

    File f = SD_MMC.open(filename, FILE_APPEND);
    if (!f) { SD_MMC.end(); return "could not open notes file"; }
    f.print(timeBuf);
    f.print(" - ");
    f.println(text);
    f.close();
    SD_MMC.end();

    return String("saved to ") + filename;
}

static String tool_list_recent_notes(JsonVariant input) {
    int count = input["count"] | 5;
    count = assistant_clamp_note_count(count);

    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    if (!SD_MMC.begin("/sdcard", true)) return "SD card unavailable";

    char filename[64];
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 100)) {
        SD_MMC.end();
        return "time not synced — cannot determine today's note file";
    }
    strftime(filename, sizeof(filename), "/notes/%Y-%m-%d.txt", &timeinfo);

    File f = SD_MMC.open(filename);
    if (!f) { SD_MMC.end(); return "no notes today"; }

    String all;
    all.reserve(f.size() + 1);
    while (f.available()) all += (char)f.read();
    f.close();
    SD_MMC.end();

    size_t len = assistant_notes_trim_end(all.c_str(), all.length());
    size_t start = assistant_notes_recent_slice_start(all.c_str(), len, count);
    String result = all.substring(start, len);
    result.trim();
    return result.length() ? result : "no notes today";
}

// ── Tool dispatcher ─────────────────────────────────────────────────────────
static String executeTool(const char *name, JsonVariant input) {
    USBSerial.printf("[nanogpt] exec tool: %s\n", name);
    if (!strcmp(name, "get_time"))           return tool_get_time(input);
    if (!strcmp(name, "get_battery_status")) return tool_get_battery_status(input);
    if (!strcmp(name, "get_uptime"))         return tool_get_uptime(input);
    if (!strcmp(name, "get_wifi_info"))      return tool_get_wifi_info(input);
    if (!strcmp(name, "get_orientation"))    return tool_get_orientation(input);
    if (!strcmp(name, "set_brightness"))     return tool_set_brightness(input);
    if (!strcmp(name, "play_beep"))          return tool_play_beep(input);
    if (!strcmp(name, "restart_device"))     return tool_restart_device(input);
    if (!strcmp(name, "power_off"))          return tool_power_off(input);
    if (!strcmp(name, "get_weather"))        return tool_get_weather(input);
    if (!strcmp(name, "save_note"))          return tool_save_note(input);
    if (!strcmp(name, "list_recent_notes"))  return tool_list_recent_notes(input);
    return String("error: unknown tool ") + name;
}

// ── NanoGPT Chat Completion (OpenAI-compatible API) ───────────────────────
// Multi-turn agent loop: lets NanoGPT call device tools with OpenAI-style
// tool_calls, then returns the final text answer.
static String nanogptChat(const String &userMessage) {
    USBSerial.printf("[nanogpt] free heap: %u, largest block: %u\n",
                     ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    JsonDocument doc;
    assistant_chat_init_request(
        doc,
        s_config.nanogptModel[0] ? s_config.nanogptModel : DEFAULT_NANOGPT_MODEL,
        s_config.webSearch);

    JsonArray tools = doc["tools"].to<JsonArray>();
    assistant_tool_schema_add_to(tools);

    JsonArray msgs = doc["messages"].to<JsonArray>();
    JsonObject sys = msgs.add<JsonObject>();
    sys["role"] = "system";
    sys["content"] = assistant_chat_system_prompt();
    for (int i = 0; i < s_history.count(); i++) {
        JsonObject m = msgs.add<JsonObject>();
        m["role"]    = (i & 1) ? "assistant" : "user";
        m["content"] = s_history.at(i);
    }
    JsonObject cur = msgs.add<JsonObject>();
    cur["role"]    = "user";
    cur["content"] = userMessage;

    String finalText;
    for (int round = 0; round < ASSISTANT_CHAT_MAX_TOOL_ROUNDS; round++) {
        JsonDocument rdoc;
        if (!nanogpt_client_post_chat(s_config.nanogptKey, doc, rdoc)) return "";

        const char *errMsg = rdoc["error"]["message"] | (const char *)nullptr;
        if (errMsg) {
            USBSerial.printf("[nanogpt] error: %s\n", errMsg);
            return String("LLM error: ") + errMsg;
        }

        JsonObject message = rdoc["choices"][0]["message"];
        const char *finishReason = rdoc["choices"][0]["finish_reason"] | "";
        USBSerial.printf("[nanogpt] round %d finish_reason=%s\n", round, finishReason);

        JsonArray toolCalls = message["tool_calls"].as<JsonArray>();
        if (toolCalls.isNull() || toolCalls.size() == 0) {
            finalText = String((const char *)(message["content"] | ""));
            return finalText;
        }

        // Mirror the assistant tool_calls into the next request.
        // Use String() wrappers to force ArduinoJson to duplicate the strings
        // (otherwise it would store pointers into rdoc, which dies next round).
        JsonObject assistantTurn = msgs.add<JsonObject>();
        assistantTurn["role"] = "assistant";
        if (!message["content"].isNull()) {
            assistantTurn["content"] = String((const char *)(message["content"] | ""));
        } else {
            assistantTurn["content"] = nullptr;
        }
        JsonArray copiedCalls = assistantTurn["tool_calls"].to<JsonArray>();
        for (JsonObject call : toolCalls) {
            JsonObject c = copiedCalls.add<JsonObject>();
            c["id"] = String((const char *)(call["id"] | ""));
            c["type"] = "function";
            JsonObject fn = c["function"].to<JsonObject>();
            fn["name"] = String((const char *)(call["function"]["name"] | ""));
            const char *args = call["function"]["arguments"] | nullptr;
            fn["arguments"] = String(assistant_chat_tool_arguments_or_empty(args));
        }

        for (JsonObject call : toolCalls) {
            const char *id = call["id"] | "";
            const char *name = call["function"]["name"] | "";
            const char *args = assistant_chat_tool_arguments_or_empty(call["function"]["arguments"] | nullptr);

            JsonDocument inputDoc;
            if (deserializeJson(inputDoc, args)) {
                inputDoc.to<JsonObject>();
            }
            String result = executeTool(name, inputDoc.as<JsonVariant>());
            USBSerial.printf("[nanogpt] tool %s → %.120s\n", name, result.c_str());

            JsonObject tr = msgs.add<JsonObject>();
            tr["role"]         = "tool";
            tr["tool_call_id"] = String(id);
            tr["content"]      = result;
        }
        // Loop to the next round with the augmented doc.
    }

    USBSerial.println("[nanogpt] max rounds reached without final text");
    return finalText.length() ? finalText : String("");
}

static void addToHistory(const String &user, const String &assistant) {
    int beforeBytes = s_history.bytes();
    int beforeCount = s_history.count();
    s_history.add(user, assistant);
    if (s_history.count() < beforeCount + 2 || s_history.bytes() < beforeBytes) {
        USBSerial.println("[nanogpt] history pruned oldest pair");
    }
    USBSerial.printf("[nanogpt] history: %d msgs, %d bytes\n", s_history.count(), s_history.bytes());
}

// ── Drawing ─────────────────────────────────────────────────────────────────
static void prepareWrappedText(AssistantTextLayout &cache, const String &source, int16_t maxW) {
    const char *raw = source.c_str();
    size_t rawLen = source.length();
    uint32_t hash = assistant_text_layout_hash(raw, rawLen);
    const int charW = 14;
    int maxChars = maxW / charW;
    if (assistant_text_layout_matches(cache, rawLen, hash, maxChars)) return;

    char converted[AssistantTextLayout::TEXT_CAP];
    utf8_to_cp437(converted, sizeof(converted), raw);
    assistant_text_layout_prepare(cache, converted, rawLen, hash, maxChars);
}

static int drawWrappedCached(AssistantTextLayout &cache, const String &text,
                             int16_t x, int16_t y, int16_t maxW, uint16_t col)
{
    prepareWrappedText(cache, text, maxW);

    canvas->setFont(&FreeMonoBold12pt7b);
    canvas->setTextSize(1);
    canvas->setTextColor(col);

    const int lineH = 22;
    const int bottomClip = LCD_HEIGHT - 50;
    char buf[128];
    int lineY = y;

    for (int i = 0; i < cache.lineCount; i++) {
        int n = cache.len[i];
        // Skip lines whose top edge would be above the canvas. Arduino_GFX's
        // drawChar for custom fonts only clips glyphs whose ENTIRE bounding
        // box is off-screen; partial-top glyphs invoke writePixelPreclipped()
        // with negative coordinates and that helper does no bounds check —
        // it writes outside the framebuffer, corrupting PSRAM. Manifested as
        // silent hard-resets whenever we scrolled text upward.
        if (n > 0 && lineY >= 0 && lineY + lineH <= bottomClip) {
            memcpy(buf, cache.text + cache.start[i], n);
            buf[n] = '\0';
            canvas->setCursor(x, lineY + lineH - 4);
            canvas->print(buf);
        }
        lineY += lineH;
    }
    canvas->setFont(nullptr);
    return lineY;
}

static void draw() {
    canvas->fillScreen(0x0000);
    int16_t cx = LCD_WIDTH / 2;

    // ── Status pill ─────────────────────────────────────────────────
    const char *stStr;
    uint16_t stCol, stBg;
    switch (s_state) {
        case GS_INIT:         stStr = "INIT";   stCol = 0x2945; stBg = 0x1082; break;
        case GS_IDLE:         stStr = "READY";  stCol = 0x07E0; stBg = 0x0200; break;
        case GS_LISTENING:    stStr = "LISTEN"; stCol = 0xF800; stBg = 0x4000; break;
        case GS_TRANSCRIBING: stStr = "STT";    stCol = 0xFFE0; stBg = 0x4200; break;
        case GS_THINKING:     stStr = "THINK";  stCol = 0xFFE0; stBg = 0x4200; break;
        case GS_ERROR:        stStr = "ERROR";  stCol = 0xF800; stBg = 0x4000; break;
        default:              stStr = "?";      stCol = 0xFFFF; stBg = 0x0000; break;
    }

    int16_t stw   = (int16_t)(strlen(stStr) * 18);
    int16_t pillW = stw + 28;
    canvas->fillRoundRect(cx - pillW / 2, 40, pillW, 38, 19, stBg);
    canvas->setTextSize(3);
    canvas->setTextColor(stCol);
    canvas->setCursor(cx - stw / 2, 48);
    canvas->print(stStr);

    // Recording indicator: time + VU meter
    if (s_state == GS_LISTENING) {
        canvas->fillCircle(cx - stw / 2 - 16, 60, 6, 0xF800);

        // Elapsed time
        uint32_t elapsed = (millis() - s_recStartMs) / 1000;
        char timeBuf[8];
        snprintf(timeBuf, sizeof(timeBuf), "%u:%02u", elapsed / 60, elapsed % 60);
        canvas->setTextSize(2);
        canvas->setTextColor(0xF800);
        int16_t tw = (int16_t)(strlen(timeBuf) * 12);
        canvas->setCursor(cx - tw / 2, 82);
        canvas->print(timeBuf);

        // VU meter bar — horizontal bar showing audio level
        int16_t barX = 32;
        int16_t barY = 104;
        int16_t barMaxW = LCD_WIDTH - 64;
        int16_t barH = 12;
        // Map RMS (0..~8000) to bar width, clamp
        int level = s_recRms;
        if (level > 6000) level = 6000;
        int16_t barW = (int16_t)((long)level * barMaxW / 6000);
        if (barW < 2) barW = 2;

        // Background
        canvas->drawRoundRect(barX, barY, barMaxW, barH, 3, 0x2945);
        // Level bar — green for low, yellow for mid, red for high
        uint16_t barCol;
        if (level < 1500)      barCol = 0x07E0;  // green
        else if (level < 3500) barCol = 0xFFE0;  // yellow
        else                   barCol = 0xF800;  // red
        canvas->fillRoundRect(barX, barY, barW, barH, 3, barCol);
    }

    // ── NanoGPT diamond (idle + no conversation yet) ─────────────────
    if (s_state == GS_IDLE && s_userText.length() == 0 && s_agentText.length() == 0) {
        drawNanoGptDiamond(cx, LCD_HEIGHT / 2 + 4, 132);

        // Tool-status label, vertically aligned with the PWR pill on the right
        // edge (PWR is what the user uses to toggle). Reflects current state.
        const char *wsLabel = s_config.webSearch ? "web search: ON" : "web search: OFF";
        uint16_t wsColor   = s_config.webSearch ? 0x6E0B   // muted teal
                                                 : 0x52AA;  // muted grey
        canvas->setTextSize(2);
        canvas->setTextColor(wsColor);
        int16_t wsW = (int16_t)(strlen(wsLabel) * 12);
        canvas->setCursor((LCD_WIDTH - wsW) / 2 - 18, PWR_BTN_Y_P - 8);
        canvas->print(wsLabel);
    }

    // ── Scrollable text area ────────────────────────────────────────
    int16_t textTop = (s_state == GS_LISTENING) ? 124 : 90;
    int16_t textY = textTop - s_scrollY;

    if (s_userText.length() > 0) {
        canvas->setTextSize(2);
        canvas->setTextColor(0x8410);
        if (textY >= -16 && textY < LCD_HEIGHT)
            { canvas->setCursor(16, textY); canvas->print("You:"); }
        textY += 22;
        textY = drawWrappedCached(s_userLayout, s_userText, 16, textY, LCD_WIDTH - 32, 0xFFFF);
        textY += 14;
    }

    if (s_agentText.length() > 0) {
        canvas->setTextSize(2);
        canvas->setTextColor(0x8410);
        if (textY >= -16 && textY < LCD_HEIGHT)
            { canvas->setCursor(16, textY); canvas->print("AI:"); }
        textY += 22;
        textY = drawWrappedCached(s_agentLayout, s_agentText, 16, textY, LCD_WIDTH - 32, 0x07E0);
        textY += 14;
    }

    s_contentH = textY + s_scrollY - textTop;

    // ── Error ───────────────────────────────────────────────────────
    if (s_state == GS_ERROR && s_errorMsg.length() > 0) {
        drawWrappedCached(s_errorLayout, s_errorMsg, 16, 180, LCD_WIDTH - 32, 0xF800);
        canvas->setTextSize(2);
        canvas->setTextColor(0x8410);
        const char *hint = "Press PWR to retry";
        int16_t hw = (int16_t)(strlen(hint) * 12);
        canvas->setCursor(cx - hw / 2, 230);
        canvas->print(hint);
    }

    // ── Pill labels ─────────────────────────────────────────────────
    if (s_state == GS_IDLE || s_state == GS_LISTENING) {
        // Multi-page answers: BOOT label is "scroll" until the user has
        // walked through to the end and pressed once more to dismiss; then
        // it goes back to "talk" for the next question.
        const char *bootLabel = (s_state == GS_IDLE && scrollModeActive())
            ? "scroll" : "talk";
        draw_pill_label(canvas, 0, 0, bootLabel);
    }
    if (s_state == GS_ERROR) {
        draw_pill_label(canvas, 0, 1, "retry");
    } else if (s_state == GS_IDLE) {
        // On the splash (no conversation yet) PWR toggles web search; with a
        // conversation on screen PWR clears it and starts a new one.
        bool onSplash = (s_userText.length() == 0 && s_agentText.length() == 0);
        if (onSplash) {
            draw_pill_label(canvas, 0, 1, s_config.webSearch ? "on" : "off");
        } else {
            draw_pill_label(canvas, 0, 1, "new");
        }
    }

    draw_battery_g(canvas, LCD_WIDTH, LCD_HEIGHT);

    // Standard footer (touch input is disabled — no page button anymore;
    // BOOT does paging via its pill label switching to "page").
    {
        draw_watermark_g(canvas, LCD_WIDTH, LCD_HEIGHT);
    }
    canvas->flush();
    s_lastDraw = millis();
}

// ── Setup ───────────────────────────────────────────────────────────────────
void app_nanogpt_assistant_setup(Arduino_OLED *gfx) {
    USBSerial.println("[trace] setup() entered — device booted/rebooted");
    canvas       = g_canvas;
    s_gfx        = gfx;
    s_state      = GS_INIT;
    s_bootWas    = false;
    s_lastDraw   = 0;
    s_lastDrawRecSec = UINT32_MAX;
    s_lastDrawRmsBucket = -1;
    s_recCount   = 0;
    s_recRms     = 0;
    s_userText   = "";
    s_agentText  = "";
    s_errorMsg   = "";
    s_scrollY    = 0;
    s_scrollDone = false;
    s_contentH   = 0;
    s_history.clear();

    if (!s_recBuf) s_recBuf = (int16_t *)ps_malloc(MAX_REC_BYTES);
    if (!s_recBuf) {
        s_errorMsg = "PSRAM alloc failed";
        s_state = GS_ERROR;
        draw();
        return;
    }

    audio_engine_init();
    pinMode(BOOT_BTN, INPUT_PULLUP);
    // Touch is not used in this app (see note in loop body).
    draw();

    if (!assistant_config_load(s_config)) {
        s_errorMsg = "Missing NANOGPT_KEY in setup.txt";
        s_state = GS_ERROR;
        draw();
        return;
    }
    USBSerial.printf("[nanogpt] config: ssid='%s' nanogpt='%.8s...' model='%s' stt='%s' websearch=%s\n",
                     s_config.ssid[0], s_config.nanogptKey, s_config.nanogptModel,
                     s_config.nanogptSttModel, s_config.webSearch ? "on" : "off");

    s_state = GS_INIT;
    draw();
    if (!wifiConnect()) {
        s_errorMsg = "WiFi connect failed";
        s_state = GS_ERROR;
        draw();
        return;
    }

    // NTP sync — TIMEZONE from setup.txt is a POSIX TZ string (e.g.
    // "CET-1CEST,M3.5.0,M10.5.0/3"). If missing we default to UTC0.
    configTzTime(s_config.timezone, "pool.ntp.org", "time.nist.gov");
    USBSerial.printf("[nanogpt] NTP sync requested, tz='%s'\n", s_config.timezone);

    // IMU init — used by get_orientation tool. If absent, tool returns error.
    s_imuOk = s_imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
    if (s_imuOk) {
        s_imu.configAccelerometer(
            SensorQMI8658::ACC_RANGE_2G,
            SensorQMI8658::ACC_ODR_125Hz,
            SensorQMI8658::LPF_MODE_2);
        s_imu.enableAccelerometer();
        USBSerial.println("[nanogpt] IMU ready");
    } else {
        USBSerial.println("[nanogpt] IMU not found");
    }

    s_state = GS_IDLE;
    USBSerial.println("[nanogpt] ready");
    draw();
}

// ── Loop ────────────────────────────────────────────────────────────────────
void app_nanogpt_assistant_loop() {
    common_tick();
    uint32_t now = millis();

    // ── Recording: drain audio engine ring fully into PSRAM buffer ──
    // The ring is 32k samples (~2 s); if we leave anything behind and the
    // loop later stalls (TLS handshake, screen redraw, heap alloc), the I2S
    // push silently drops new samples once the ring is full. Drain to empty
    // every iteration instead of capping per-iteration to PULL_CHUNK.
    if (s_state == GS_LISTENING) {
        drainRecordingSamples();
        if (s_recCount >= MAX_REC_SAMPLES) {
            audio_engine_record_stop();
            audio_engine_unmute();
            USBSerial.printf("[nanogpt] auto-stop, %u samples\n", s_recCount);
            s_state = GS_TRANSCRIBING;
            draw();
        }
    }

    // ── STT processing (blocking) ───────────────────────────────────
    if (s_state == GS_TRANSCRIBING) {
        draw();  // show "STT" status before blocking
        NanoGptClientConfig clientConfig = {
            s_config.nanogptKey,
            s_config.nanogptSttModel,
            s_config.language,
        };
        AudioTrimWindow trim = audio_trim_voice_window(s_recBuf, s_recCount, SAMPLE_RATE);
        if (trim.offsetSamples > 0 || trim.sampleCount < s_recCount) {
            USBSerial.printf("[nanogpt] trimmed recording: %u -> %u samples (offset %u)\n",
                             s_recCount, trim.sampleCount, trim.offsetSamples);
        }
        String text = nanogpt_client_transcribe(
            clientConfig,
            s_recBuf + trim.offsetSamples,
            trim.sampleCount,
            SAMPLE_RATE);
        // Resync button state after blocking call
        s_bootWas = (digitalRead(BOOT_BTN) == LOW);
        if (text.length() == 0) {
            s_errorMsg = "Transcription failed";
            s_state = GS_ERROR;
            draw();
        } else if (text.startsWith("STT error:")) {
            s_errorMsg = text;
            s_state = GS_ERROR;
            draw();
        } else {
            s_userText  = text;
            s_agentText = "";    // clear the previous answer the moment the
                                 // new question appears — avoids the stale
                                 // "old reply + new question + spinner" UI.
            s_scrollY   = 0;
            s_state     = GS_THINKING;
            draw();
        }
    }

    // ── LLM processing (blocking) ───────────────────────────────────
    if (s_state == GS_THINKING) {
        draw();  // show "THINK" status before blocking
        String reply = nanogptChat(s_userText);
        // Resync button state + drain any PWR press that latched while we
        // were blocked in nanogptChat().
        s_bootWas = (digitalRead(BOOT_BTN) == LOW);
        common_drain_pwr();
        if (reply.length() == 0) {
            s_errorMsg = "Chat failed";
            s_state = GS_ERROR;
            draw();
        } else if (reply.startsWith("LLM error:")) {
            s_errorMsg = reply;
            s_state = GS_ERROR;
            draw();
        } else {
            s_agentText = reply;
            addToHistory(s_userText, s_agentText);
            s_scrollY = 0;
            s_scrollDone = false;     // new answer → scroll mode is fresh
            s_state = GS_IDLE;
            USBSerial.println("[nanogpt] -> IDLE, ready for next question");
            draw();
        }
        // Deferred device-control tools — applied AFTER the reply renders so
        // the user can read what NanoGPT said before the chip resets / shuts.
        if (s_pendingRestart) {
            delay(800);
            ESP.restart();
        }
        if (s_pendingPowerOff) {
            delay(800);
            power.shutdown();
        }
    }

    // ── BOOT button: scroll half a page if scroll-mode is active, otherwise
    //    push-to-talk for a new question. The very last press in scroll-mode
    //    (when scrollY is already at the bottom) resets back to the top AND
    //    dismisses scroll-mode, so the pill flips to "talk" for the next press.
    bool boot = (digitalRead(BOOT_BTN) == LOW);
    if (boot && !s_bootWas) {
        common_activity();
        if (s_state == GS_IDLE && scrollModeActive()) {
            AssistantScrollState scroll = { s_scrollY, s_scrollDone };
            assistant_scroll_advance(scroll, s_contentH, PAGE_VIEW_H);
            s_scrollY = scroll.offset;
            s_scrollDone = scroll.done;
            draw();
        } else if (s_state == GS_IDLE) {
            s_state      = GS_LISTENING;
            s_recCount   = 0;
            s_recRms     = 0;
            s_recStartMs = now;
            s_lastDrawRecSec = UINT32_MAX;
            s_lastDrawRmsBucket = -1;
            s_scrollY    = 0;
            // Ensure clean audio state before recording
            audio_engine_record_stop();
            audio_engine_stop();
            delay(50);
            audio_engine_mute();
            audio_engine_record();
            USBSerial.println("[nanogpt] BOOT -> LISTENING");
            draw();
        }
    }
    if (!boot && s_bootWas) {
        if (s_state == GS_LISTENING) {
            // Drain any remaining samples from audio engine
            delay(50);
            int avail = audio_engine_rx_avail();
            drainRecordingSamples();
            audio_engine_record_stop();
            audio_engine_unmute();
            USBSerial.printf("[nanogpt] BOOT released, %u samples (~%us), rx_avail was %d\n",
                             s_recCount, s_recCount / SAMPLE_RATE, avail);
            if (assistant_recording_is_too_short(s_recCount, SAMPLE_RATE)) {
                USBSerial.printf("[nanogpt] too short (%u < %d), ignoring\n",
                                 s_recCount, SAMPLE_RATE / 4);
                s_state = GS_IDLE;
                draw();
            } else {
                s_state = GS_TRANSCRIBING;
            }
        }
    }
    s_bootWas = boot;

    // ── PWR button ──────────────────────────────────────────────────
    if (common_consume_pwr_short()) {
        common_activity();
        if (s_state == GS_ERROR) {
            USBSerial.println("[nanogpt] PWR -> retry");
            s_errorMsg = "";
            s_state = GS_INIT;
            draw();
            if (WiFi.status() != WL_CONNECTED) {
                WiFi.mode(WIFI_STA);
                if (!wifiConnect()) {
                    s_errorMsg = "WiFi connect failed";
                    s_state = GS_ERROR;
                    draw();
                    return;
                }
            }
            s_state = GS_IDLE;
            draw();
        } else if (s_state == GS_IDLE) {
            bool onSplash = (s_userText.length() == 0 && s_agentText.length() == 0);
            if (onSplash) {
                // Splash: PWR toggles the web_search tool for the next chat.
                s_config.webSearch = !s_config.webSearch;
                USBSerial.printf("[nanogpt] PWR → web search %s\n",
                                 s_config.webSearch ? "ON" : "OFF");
                draw();
            } else {
                USBSerial.println("[nanogpt] PWR → new conversation");
                s_history.clear();
                s_userText  = "";
                s_agentText = "";
                s_errorMsg  = "";
                s_scrollY   = 0;
                s_recCount  = 0;
                s_recRms    = 0;
                // Ensure audio engine is in clean state
                audio_engine_record_stop();
                audio_engine_unmute();
                audio_engine_stop();
                draw();
            }
        }
    }

    // ── Touch input is fully disabled in this app ─────────────────────────
    // BOOT handles paging and talk control, so touch stays off intentionally.


    // ── Periodic redraw. Most states draw on explicit transitions only; the
    //    listening screen refreshes when the timer or VU meter visibly changes.
    {
        bool due = false;
        if (s_state == GS_LISTENING) {
            due = (now - s_lastDraw >= 250) && recordingMeterNeedsRedraw(now);
        } else {
            // Keep slow-changing HUD items, such as battery, from going stale
            // without paying for a full canvas flush five times per second.
            due = (now - s_lastDraw >= 30000);
        }
        if (due) {
            s_lastDraw = now;
            draw();
        }
    }

    if (s_state == GS_IDLE || s_state == GS_ERROR) delay(10);
}
