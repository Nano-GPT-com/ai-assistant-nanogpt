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
#include "app_common.h"
#include "audio_wav.h"
#include "audio_engine.h"
#include "nanogpt_protocol.h"
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
#include "TouchDrvFT6X36.hpp"
#include "Fonts/FreeMonoBold12pt7b.h"

extern USBCDC USBSerial;
extern Arduino_Canvas *g_canvas;
extern TouchDrvFT6X36  touch;

// ── Constants ────────────────────────────────────────────────────────────────
#define BOOT_BTN        0
#define PWR_POLL_MS     50
#define SAMPLE_RATE     16000
#define MAX_REC_S       30
#define MAX_REC_SAMPLES (SAMPLE_RATE * MAX_REC_S)   // 480000
#define MAX_REC_BYTES   (MAX_REC_SAMPLES * 2)        // 960000
#define PULL_CHUNK      4096
#define MAX_HISTORY     6
#define MAX_HIST_BYTES  4096   // prune when total history text exceeds this
#define HTTP_TIMEOUT_MS 30000
#define BOUNDARY        "----ESP32Bnd9a7f3c"
#define SWIPE_THRESH    8

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
static uint32_t s_lastPwr        = 0;
static uint32_t s_lastDraw       = 0;
static uint32_t s_recStartMs     = 0;

// Config
static AssistantConfig s_config;
static float    s_locLat         = 0.0f;
static float    s_locLon         = 0.0f;
static bool     s_locGeocoded    = false;

// Tools / agent state
static Arduino_SH8601 *s_gfx     = nullptr; // for set_brightness
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

// Touch scroll
static int      s_scrollY        = 0;
static bool     s_touchWas       = false;
static int16_t  s_touchLastY     = 0;
static int16_t  s_touchDownX     = 0;   // touch-down position for tap detection
static int16_t  s_touchDownY     = 0;
static uint32_t s_touchDownMs    = 0;
static uint32_t s_touchEndedAt   = 0;   // ms of last touch release

// Tap thresholds
#define TAP_MAX_MS    600
#define TAP_MAX_DIST  25

// ── Page-advance button geometry (bottom of screen, only when scrollable) ───
#define PAGE_BTN_W    ((LCD_WIDTH  * 80) / 100)   // 80 % wide
#define PAGE_BTN_H    ((LCD_HEIGHT *  9) / 100)   //  9 % tall
#define PAGE_BTN_X    ((LCD_WIDTH - PAGE_BTN_W) / 2)
#define PAGE_BTN_Y    (LCD_HEIGHT - PAGE_BTN_H - 6)
#define PAGE_VIEW_H   (LCD_HEIGHT - 130)          // visible text height

static int      s_contentH       = 0;
static bool     s_scrollDone     = false;  // user pressed BOOT at end → scroll
                                           // is dismissed; next press starts talk

// "scroll" mode is active when the latest answer doesn't fit on one screen
// AND the user hasn't yet pressed BOOT past the bottom (which dismisses
// scroll-mode and brings the pill back to "talk").
static bool scrollModeActive() {
    return s_contentH > PAGE_VIEW_H && !s_scrollDone;
}


// Conversation history (alternating user / assistant)
static String   s_history[MAX_HISTORY * 2];
static int      s_histCount      = 0;

// ── WiFi ────────────────────────────────────────────────────────────────────
static bool wifiConnect() {
    WifiCred list[3] = {
        { s_config.ssid[0], s_config.password[0] },
        { s_config.ssid[1], s_config.password[1] },
        { s_config.ssid[2], s_config.password[2] },
    };
    return wifi_try_connect(list, 3) >= 0;
}

// ── HTTP response reader (robust) ───────────────────────────────────────────
// Reads until connection closes or timeout, returns JSON body
static String readHttpResponse(WiFiClientSecure &client) {
    // Read status line + headers + body into a buffer
    // Use a PSRAM-backed String for large responses
    String raw;
    raw.reserve(4096);

    uint32_t start = millis();
    uint32_t lastData = start;
    uint8_t buf[1024];

    while (millis() - start < HTTP_TIMEOUT_MS) {
        int avail = client.available();
        if (avail > 0) {
            int toRead = avail;
            if (toRead > (int)sizeof(buf)) toRead = sizeof(buf);
            int got = client.read(buf, toRead);
            if (got > 0) {
                raw.concat((char *)buf, got);
                lastData = millis();
            }
        } else if (!client.connected()) {
            break;  // server closed connection
        } else if (millis() - lastData > 10000) {
            USBSerial.println("[http] read timeout (no data for 10s)");
            break;  // no data for too long
        } else {
            delay(10);
        }
    }
    client.stop();

    USBSerial.printf("[http] raw response: %d bytes\n", raw.length());

    // Find body after headers
    int bodyStart = raw.indexOf("\r\n\r\n");
    if (bodyStart < 0) {
        USBSerial.println("[http] no header/body separator found");
        // Log first 200 chars for debug
        USBSerial.printf("[http] raw: %.200s\n", raw.c_str());
        return "";
    }

    // Log HTTP status line
    int statusEnd = raw.indexOf("\r\n");
    if (statusEnd > 0) {
        USBSerial.printf("[http] status: %s\n", raw.substring(0, statusEnd).c_str());
    }

    String body = raw.substring(bodyStart + 4);
    raw = String();  // free

    // Handle chunked encoding or find JSON
    int jsonStart = body.indexOf('{');
    int jsonEnd   = body.lastIndexOf('}');
    if (jsonStart < 0 || jsonEnd < 0) {
        USBSerial.printf("[http] no JSON in body (%d bytes): %.200s\n",
                         body.length(), body.c_str());
        return "";
    }
    return body.substring(jsonStart, jsonEnd + 1);
}

// ── NanoGPT Speech-to-Text ──────────────────────────────────────────────────
static String nanogptTranscribe(const int16_t *pcm, uint32_t numSamples) {
    uint32_t pcmBytes = numSamples * 2;

    // Build multipart parts (small strings)
    String part1hdr = String("--") + BOUNDARY + "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    String part1end = "\r\n";
    String part2 = String("--") + BOUNDARY + "\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        + (s_config.nanogptSttModel[0] ? s_config.nanogptSttModel : DEFAULT_NANOGPT_STT_MODEL) + "\r\n";
    // Optional language hint: only sent when LANGUAGE is set in setup.txt.
    // Empty → Whisper auto-detects.
    String part3;
    if (s_config.language[0]) {
        part3 = String("--") + BOUNDARY + "\r\n"
            "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
            + s_config.language + "\r\n";
    }
    String closing = String("--") + BOUNDARY + "--\r\n";

    uint32_t contentLen = nanogpt_stt_content_length(
        BOUNDARY,
        s_config.nanogptSttModel[0] ? s_config.nanogptSttModel : DEFAULT_NANOGPT_STT_MODEL,
        s_config.language,
        WAV_HEADER_SIZE,
        pcmBytes);

    USBSerial.printf("[nanogpt] STT free heap: %u, largest: %u\n",
                     ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    USBSerial.printf("[nanogpt] STT: %u samples (%us), %u PCM bytes, content-length=%u\n",
                     numSamples, numSamples / SAMPLE_RATE, pcmBytes, contentLen);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(HTTP_TIMEOUT_MS / 1000);

    USBSerial.println("[nanogpt] STT connecting...");
    if (!client.connect(NANOGPT_HOST, NANOGPT_PORT)) {
        USBSerial.println("[nanogpt] STT connect failed");
        return "";
    }
    USBSerial.println("[nanogpt] STT connected, sending request...");

    // HTTP headers
    String headers = String("POST /api/v1/audio/transcriptions HTTP/1.1\r\n"
        "Host: ") + NANOGPT_HOST + "\r\n"
        "Authorization: Bearer " + s_config.nanogptKey + "\r\n"
        "Content-Type: multipart/form-data; boundary=" + BOUNDARY + "\r\n"
        "Content-Length: " + String(contentLen) + "\r\n"
        "Connection: close\r\n"
        "\r\n";
    client.print(headers);

    // Part 1: WAV file
    client.print(part1hdr);

    // WAV header
    uint8_t wavHdr[WAV_HEADER_SIZE];
    wav_build_pcm16_mono_header(wavHdr, pcmBytes, SAMPLE_RATE);
    client.write(wavHdr, WAV_HEADER_SIZE);

    // Stream PCM data in chunks
    uint32_t sent = 0;
    while (sent < pcmBytes) {
        uint32_t chunk = pcmBytes - sent;
        if (chunk > 4096) chunk = 4096;
        size_t written = client.write(((const uint8_t *)pcm) + sent, chunk);
        if (written == 0) {
            USBSerial.printf("[nanogpt] STT write stall at %u/%u\n", sent, pcmBytes);
            delay(50);
            // Retry once
            written = client.write(((const uint8_t *)pcm) + sent, chunk);
            if (written == 0) {
                USBSerial.println("[nanogpt] STT write failed");
                client.stop();
                return "";
            }
        }
        sent += written;
        if ((sent % 32768) == 0) {
            delay(1);  // yield every 32KB
            USBSerial.printf("[nanogpt] STT upload %u/%u\n", sent, pcmBytes);
        }
    }
    USBSerial.printf("[nanogpt] STT upload done: %u/%u bytes\n", sent, pcmBytes);

    client.print(part1end);
    client.print(part2);
    client.print(part3);
    client.print(closing);
    USBSerial.println("[nanogpt] STT request sent, waiting for response...");

    // Read response
    String body = readHttpResponse(client);
    if (body.length() == 0) {
        USBSerial.println("[nanogpt] STT empty response");
        return "";
    }
    USBSerial.printf("[nanogpt] STT body: %.300s\n", body.c_str());

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        USBSerial.println("[nanogpt] STT JSON parse failed");
        return "";
    }

    // Check for error
    const char *errMsg = doc["error"]["message"] | (const char *)nullptr;
    if (errMsg) {
        USBSerial.printf("[nanogpt] STT error: %s\n", errMsg);
        return String("STT error: ") + errMsg;
    }

    String text = doc["text"] | "";
    USBSerial.printf("[nanogpt] STT result: '%s'\n", text.c_str());
    return text;
}

// ── Custom tool declarations ────────────────────────────────────────────────
// Each call adds one tool entry to the API request's tools[] array.
static JsonObject toolDecl(JsonArray tools, const char *name, const char *desc) {
    JsonObject t = tools.add<JsonObject>();
    t["type"] = "function";
    JsonObject fn = t["function"].to<JsonObject>();
    fn["name"]        = name;
    fn["description"] = desc;
    JsonObject schema = fn["parameters"].to<JsonObject>();
    schema["type"] = "object";
    schema["properties"].to<JsonObject>();   // empty by default
    return t;
}

static void addCustomTools(JsonArray tools) {
    toolDecl(tools, "get_time",
        "Returns the current local date and time on the device.");
    toolDecl(tools, "get_battery_status",
        "Returns battery percentage and charging state.");
    toolDecl(tools, "get_uptime",
        "Returns how long the device has been running since the last boot.");
    toolDecl(tools, "get_wifi_info",
        "Returns the WiFi network name and signal strength.");
    toolDecl(tools, "get_orientation",
        "Returns the device tilt (pitch and roll in degrees) and a friendly description such as 'face up' or 'on its side'.");

    {
        JsonObject t = toolDecl(tools, "set_brightness",
            "Sets the screen brightness. Percent 0-100 (0=off, 100=max).");
        JsonObject props = t["function"]["parameters"]["properties"].as<JsonObject>();
        JsonObject p = props["percent"].to<JsonObject>();
        p["type"]        = "integer";
        p["description"] = "Brightness percent 0-100";
        JsonArray req = t["function"]["parameters"]["required"].to<JsonArray>();
        req.add("percent");
    }
    {
        JsonObject t = toolDecl(tools, "play_beep",
            "Plays a short beep through the speaker.");
        JsonObject props = t["function"]["parameters"]["properties"].as<JsonObject>();
        JsonObject f = props["frequency_hz"].to<JsonObject>();
        f["type"]        = "integer";
        f["description"] = "Tone frequency in Hz (default 1000, 100-8000).";
        JsonObject d = props["duration_ms"].to<JsonObject>();
        d["type"]        = "integer";
        d["description"] = "Duration in milliseconds (default 200, max 2000).";
    }
    toolDecl(tools, "restart_device",
        "Reboots the device. Only call when the user explicitly asks to restart.");
    toolDecl(tools, "power_off",
        "Powers the device off. Only call when the user explicitly asks to shut down.");
    toolDecl(tools, "get_weather",
        "Returns the current weather (temperature, conditions, wind, humidity) for the device's configured location.");
    {
        JsonObject t = toolDecl(tools, "save_note",
            "Saves a short text note to today's note file on the SD card.");
        JsonObject props = t["function"]["parameters"]["properties"].as<JsonObject>();
        JsonObject txt = props["text"].to<JsonObject>();
        txt["type"]        = "string";
        txt["description"] = "The note text to save (single line).";
        JsonArray req = t["function"]["parameters"]["required"].to<JsonArray>();
        req.add("text");
    }
    {
        JsonObject t = toolDecl(tools, "list_recent_notes",
            "Lists the most recent notes from today's note file.");
        JsonObject props = t["function"]["parameters"]["properties"].as<JsonObject>();
        JsonObject c = props["count"].to<JsonObject>();
        c["type"]        = "integer";
        c["description"] = "How many recent notes to return (default 5, max 20).";
    }
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

    const char *desc;
    if (az >  0.85f)                   desc = "face up";
    else if (az < -0.85f)              desc = "face down";
    else if (ax >  0.85f)              desc = "upright, top up";
    else if (ax < -0.85f)              desc = "upside down";
    else if (fabsf(ay) > 0.7f)         desc = "on its side";
    else                               desc = "tilted";

    char buf[128];
    snprintf(buf, sizeof(buf), "pitch %.1f deg, roll %.1f deg (%s)",
             pitch, roll, desc);
    return String(buf);
}

static String tool_set_brightness(JsonVariant input) {
    int pct = input["percent"] | -1;
    if (pct < 0) return "missing 'percent' (0-100)";
    if (pct > 100) pct = 100;
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
    if (freq < 100)  freq = 100;
    if (freq > 8000) freq = 8000;
    if (durMs < 20)   durMs = 20;
    if (durMs > 2000) durMs = 2000;

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

    char enc[160] = {};
    int j = 0;
    for (int i = 0; s_config.location[i] && j < 150; i++) {
        char c = s_config.location[i];
        if (c == ' ') { enc[j++]='%'; enc[j++]='2'; enc[j++]='0'; }
        else enc[j++] = c;
    }
    char url[256];
    snprintf(url, sizeof(url),
        "https://geocoding-api.open-meteo.com/v1/search"
        "?name=%s&count=1&language=en&format=json", enc);

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

static const char *wmoDescription(int code) {
    if (code == 0) return "clear";
    if (code == 1 || code == 2) return "partly cloudy";
    if (code == 3) return "overcast";
    if (code >= 45 && code <= 48) return "foggy";
    if (code >= 51 && code <= 57) return "drizzle";
    if (code >= 61 && code <= 67) return "rain";
    if (code >= 71 && code <= 77) return "snow";
    if (code >= 80 && code <= 82) return "rain showers";
    if (code >= 85 && code <= 86) return "snow showers";
    if (code >= 95 && code <= 99) return "thunderstorm";
    return "unknown conditions";
}

static String tool_get_weather(JsonVariant input) {
    (void)input;
    if (s_config.location[0] == '\0')
        return "no LOCATION_1 configured in /setup/setup.txt";
    if (!geocodeLocation())
        return "could not geocode location";

    char url[400];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,relative_humidity_2m,wind_speed_10m,"
        "precipitation,weather_code"
        "&timezone=auto&wind_speed_unit=kmh",
        s_locLat, s_locLon);

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
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) return "weather JSON parse failed";
    JsonObject cur = doc["current"];
    if (cur.isNull()) return "weather: no 'current' data";

    float temp   = cur["temperature_2m"].as<float>();
    int   hum    = cur["relative_humidity_2m"].as<int>();
    float wind   = cur["wind_speed_10m"].as<float>();
    float precip = cur["precipitation"].as<float>();
    int   wmo    = cur["weather_code"].as<int>();
    const char *desc = wmoDescription(wmo);

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
    if (count < 1)  count = 1;
    if (count > 20) count = 20;

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

    int len = all.length();
    while (len > 0 && (all[len-1] == '\n' || all[len-1] == '\r')) len--;
    int found = 0;
    int start = 0;
    for (int i = len - 1; i >= 0; i--) {
        if (all[i] == '\n') {
            found++;
            if (found >= count) { start = i + 1; break; }
        }
    }
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

// ── Single HTTP round-trip to NanoGPT Chat Completions API ────────────────
// Sends the current request doc, returns the response body as a parsed JSON
// document via `out`. Returns false on transport/parse error.
static bool postNanoGPT(JsonDocument &reqDoc, JsonDocument &out) {
    String reqBody;
    serializeJson(reqDoc, reqBody);
    USBSerial.printf("[nanogpt] req %d bytes\n", reqBody.length());

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(HTTP_TIMEOUT_MS / 1000);
    if (!client.connect(NANOGPT_HOST, NANOGPT_PORT)) {
        USBSerial.println("[nanogpt] connect failed");
        return false;
    }
    client.print(String("POST /api/v1/chat/completions HTTP/1.1\r\n"
        "Host: ") + NANOGPT_HOST + "\r\n"
        "Authorization: Bearer " + s_config.nanogptKey + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + String(reqBody.length()) + "\r\n"
        "Connection: close\r\n\r\n");
    client.print(reqBody);
    reqBody = String();

    String body = readHttpResponse(client);
    if (body.length() == 0) { USBSerial.println("[nanogpt] empty response"); return false; }
    USBSerial.printf("[nanogpt] body: %.300s\n", body.c_str());

    if (deserializeJson(out, body)) {
        USBSerial.println("[nanogpt] JSON parse failed");
        return false;
    }
    return true;
}

// ── NanoGPT Chat Completion (OpenAI-compatible API) ───────────────────────
// Multi-turn agent loop: lets NanoGPT call device tools with OpenAI-style
// tool_calls, then returns the final text answer.
static String nanogptChat(const String &userMessage) {
    USBSerial.printf("[nanogpt] free heap: %u, largest block: %u\n",
                     ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    JsonDocument doc;
    doc["model"]      = s_config.nanogptModel[0] ? s_config.nanogptModel : DEFAULT_NANOGPT_MODEL;
    doc["max_tokens"] = 1024;
    doc["temperature"] = 0.4;
    doc["tool_choice"] = "auto";
    doc["parallel_tool_calls"] = true;
    if (s_config.webSearch) {
        JsonObject webSearch = doc["webSearch"].to<JsonObject>();
        webSearch["enabled"] = true;
    }

    JsonArray tools = doc["tools"].to<JsonArray>();
    addCustomTools(tools);

    JsonArray msgs = doc["messages"].to<JsonArray>();
    JsonObject sys = msgs.add<JsonObject>();
    sys["role"] = "system";
    sys["content"] = "Answer in as few words as possible: a single word or short phrase when you can. Reply in the same language the user uses. Use tools when they give accurate device or external data (battery, time, weather, notes, etc.); do not guess. Only call restart_device or power_off when the user explicitly asks for it. Use web search only when current information is required.";
    for (int i = 0; i < s_histCount; i++) {
        JsonObject m = msgs.add<JsonObject>();
        m["role"]    = (i & 1) ? "assistant" : "user";
        m["content"] = s_history[i];
    }
    JsonObject cur = msgs.add<JsonObject>();
    cur["role"]    = "user";
    cur["content"] = userMessage;

    String finalText;
    const int MAX_ROUNDS = 6;
    for (int round = 0; round < MAX_ROUNDS; round++) {
        JsonDocument rdoc;
        if (!postNanoGPT(doc, rdoc)) return "";

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
            fn["arguments"] = String((const char *)(call["function"]["arguments"] | "{}"));
        }

        for (JsonObject call : toolCalls) {
            const char *id = call["id"] | "";
            const char *name = call["function"]["name"] | "";
            const char *args = call["function"]["arguments"] | "{}";

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

// ── Conversation history ────────────────────────────────────────────────────
static int historyBytes() {
    int total = 0;
    for (int i = 0; i < s_histCount; i++) total += s_history[i].length();
    return total;
}

static void addToHistory(const String &user, const String &assistant) {
    // Make room if at count limit
    if (s_histCount >= MAX_HISTORY * 2) {
        for (int i = 0; i < s_histCount - 2; i++)
            s_history[i] = s_history[i + 2];
        s_histCount -= 2;
    }
    s_history[s_histCount++] = user;
    s_history[s_histCount++] = assistant;

    // Prune oldest pairs while total text exceeds byte budget
    while (s_histCount > 2 && historyBytes() > MAX_HIST_BYTES) {
        USBSerial.printf("[nanogpt] history prune: %d bytes, dropping oldest pair\n",
                         historyBytes());
        for (int i = 0; i < s_histCount - 2; i++)
            s_history[i] = s_history[i + 2];
        s_histCount -= 2;
    }
    USBSerial.printf("[nanogpt] history: %d msgs, %d bytes\n", s_histCount, historyBytes());
}

static void clearHistory() {
    for (int i = 0; i < s_histCount; i++) s_history[i] = "";
    s_histCount = 0;
}

// ── Drawing ─────────────────────────────────────────────────────────────────
static int drawWrapped(const String &text, int16_t x, int16_t y, int16_t maxW,
                       uint16_t col)
{
    canvas->setFont(&FreeMonoBold12pt7b);
    canvas->setTextSize(1);
    canvas->setTextColor(col);

    const int charW = 14;
    const int lineH = 22;
    int maxChars = maxW / charW;
    if (maxChars < 1) maxChars = 1;
    if (maxChars > 127) maxChars = 127;

    // 1) Convert UTF-8 → CP437 once, up-front. The GFX 12pt monospace font
    //    has glyph slots for ä/ö/ü/ß/etc only at their CP437 code points,
    //    so without conversion German Umlauts render as garbage or get
    //    silently dropped.
    static char converted[2048];
    utf8_to_cp437(converted, sizeof(converted), text.c_str());
    const char *src = converted;
    int len = (int)strlen(src);

    // Stop drawing before we hit the MIC pill (anchored bottom-left). Lines
    // still advance lineY so the caller's textY return value is correct, but
    // we just don't paint over the MIC label.
    const int bottomClip = LCD_HEIGHT - 50;

    char buf[128];
    int pos = 0;
    int lineY = y;
    while (pos < len) {
        // 2) Honour explicit '\n' as a forced line break — LLM replies often
        //    contain bullet/list line breaks. Without this, canvas->print
        //    on a buffer that contains '\n' silently advances the cursor
        //    INTERNALLY, which then collides with the next iteration's
        //    lineY and causes lines to stack on top of each other.
        int end = pos + maxChars;
        if (end > len) end = len;

        int nl = -1;
        for (int i = pos; i < end; i++) {
            if (src[i] == '\n') { nl = i; break; }
        }
        if (nl >= 0) {
            end = nl;                       // wrap right before the \n
        } else if (end < len) {             // word-boundary wrap
            int lastSpace = -1;
            for (int i = end; i > pos; i--) {
                if (src[i] == ' ') { lastSpace = i; break; }
            }
            if (lastSpace > pos) end = lastSpace + 1;
        }

        int n = end - pos;
        if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
        // Skip lines whose top edge would be above the canvas. Arduino_GFX's
        // drawChar for custom fonts only clips glyphs whose ENTIRE bounding
        // box is off-screen; partial-top glyphs invoke writePixelPreclipped()
        // with negative coordinates and that helper does no bounds check —
        // it writes outside the framebuffer, corrupting PSRAM. Manifested as
        // silent hard-resets whenever we scrolled text upward.
        if (n > 0 && lineY >= 0 && lineY + lineH <= bottomClip) {
            memcpy(buf, src + pos, n);
            buf[n] = '\0';
            // strip trailing whitespace so the right margin looks clean
            while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) {
                buf[--n] = '\0';
            }
            canvas->setCursor(x, lineY + lineH - 4);
            canvas->print(buf);
        }
        pos = end;
        if (pos < len && src[pos] == '\n') pos++;   // consume the \n
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

    // ── NanoGPT spark (idle + no conversation yet) ───────────────────
    // Six fat lines radiating from a centre, rounded ends, lightly uneven
    // angles + lengths so it reads as a hand-drawn asterisk rather than a
    // perfect compass rose. Drawn by stamping filled circles along each arm.
    if (s_state == GS_IDLE && s_userText.length() == 0 && s_agentText.length() == 0) {
        const int16_t sx = cx;
        const int16_t sy = LCD_HEIGHT / 2 + 10;
        const uint16_t coral = 0xCBEC;   // ~ #D97757
        // Twelve arms — base 30° apart, jittered ±5°. Lengths AND thicknesses
        // vary so the mark reads as hand-drawn, not a perfect snowflake. Kept
        // thin so arms stay distinct instead of merging into a disc.
        struct Arm { float deg; int16_t len; int16_t thick; };
        static const Arm arms[12] = {
            {   5.0f, 60, 2 },
            {  32.0f, 50, 4 },
            {  63.0f, 56, 2 },
            {  92.0f, 46, 3 },
            { 122.0f, 60, 3 },
            { 154.0f, 48, 2 },
            { 183.0f, 58, 4 },
            { 213.0f, 50, 2 },
            { 245.0f, 62, 3 },
            { 274.0f, 46, 2 },
            { 305.0f, 56, 3 },
            { 333.0f, 50, 4 },
        };
        for (int a = 0; a < 12; a++) {
            const float rad = arms[a].deg * 0.01745329f;
            const float dx  = cosf(rad);
            const float dy  = sinf(rad);
            const int   len = arms[a].len;
            const int   t   = arms[a].thick;
            for (int k = 0; k <= len; k++) {
                int x = sx + (int)(k * dx);
                int y = sy + (int)(k * dy);
                canvas->fillCircle(x, y, t, coral);
            }
        }
        // Centre disc — a touch thicker than the arms.
        canvas->fillCircle(sx, sy, 6, coral);

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
        textY = drawWrapped(s_userText, 16, textY, LCD_WIDTH - 32, 0xFFFF);
        textY += 14;
    }

    if (s_agentText.length() > 0) {
        canvas->setTextSize(2);
        canvas->setTextColor(0x8410);
        if (textY >= -16 && textY < LCD_HEIGHT)
            { canvas->setCursor(16, textY); canvas->print("AI:"); }
        textY += 22;
        textY = drawWrapped(s_agentText, 16, textY, LCD_WIDTH - 32, 0x07E0);
        textY += 14;
    }

    s_contentH = textY + s_scrollY - textTop;

    // ── Error ───────────────────────────────────────────────────────
    if (s_state == GS_ERROR && s_errorMsg.length() > 0) {
        drawWrapped(s_errorMsg, 16, 180, LCD_WIDTH - 32, 0xF800);
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
        draw_mic_pill(canvas, LCD_WIDTH, LCD_HEIGHT);
    }
    canvas->flush();
}

// ── Setup ───────────────────────────────────────────────────────────────────
void app_nanogpt_assistant_setup(Arduino_SH8601 *gfx) {
    USBSerial.println("[trace] setup() entered — device booted/rebooted");
    canvas       = g_canvas;
    s_gfx        = gfx;
    s_state      = GS_INIT;
    s_bootWas    = false;
    s_lastPwr    = 0;
    s_lastDraw   = 0;
    s_recCount   = 0;
    s_recRms     = 0;
    s_userText   = "";
    s_agentText  = "";
    s_errorMsg   = "";
    s_scrollY    = 0;
    s_scrollDone = false;
    s_touchWas   = false;
    s_contentH   = 0;
    clearHistory();

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
        int totalPulledThisIter = 0;
        int got = 0;
        for (;;) {
            int avail = audio_engine_rx_avail();
            if (avail <= 0) break;
            int remaining = MAX_REC_SAMPLES - s_recCount;
            if (avail > remaining) avail = remaining;
            if (avail <= 0) break;
            got = audio_engine_pull(s_recBuf + s_recCount, avail);
            if (got <= 0) break;
            s_recCount         += got;
            totalPulledThisIter += got;
            if (s_recCount >= MAX_REC_SAMPLES) break;
        }
        // Only compute RMS for the most recent chunk — avoids scanning the
        // full backlog when we catch up after a stall.
        if (got > 0) {
            int64_t sum = 0;
            int start = s_recCount - got;
            for (int i = 0; i < got; i++) {
                int32_t s = s_recBuf[start + i];
                sum += s * s;
            }
            s_recRms = (int)sqrt((double)sum / got);
        }
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
        String text = nanogptTranscribe(s_recBuf, s_recCount);
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
            int step      = PAGE_VIEW_H / 2;
            int maxScroll = s_contentH - PAGE_VIEW_H;
            if (maxScroll < 0) maxScroll = 0;
            if (s_scrollY >= maxScroll) {
                // Already at the end → wrap to top and dismiss scroll-mode.
                s_scrollY    = 0;
                s_scrollDone = true;
            } else {
                s_scrollY += step;
                if (s_scrollY > maxScroll) s_scrollY = maxScroll;
            }
            draw();
        } else if (s_state == GS_IDLE) {
            s_state      = GS_LISTENING;
            s_recCount   = 0;
            s_recRms     = 0;
            s_recStartMs = now;
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
            if (avail > 0) {
                int remaining = MAX_REC_SAMPLES - s_recCount;
                if (avail > remaining) avail = remaining;
                if (avail > 0) {
                    int got = audio_engine_pull(s_recBuf + s_recCount, avail);
                    s_recCount += got;
                }
            }
            audio_engine_record_stop();
            audio_engine_unmute();
            USBSerial.printf("[nanogpt] BOOT released, %u samples (~%us), rx_avail was %d\n",
                             s_recCount, s_recCount / SAMPLE_RATE, avail);
            if (s_recCount < SAMPLE_RATE / 4) {
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
                clearHistory();
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
    // On this hardware (FT6X36 + SH8601 + AXP2101 on shared buses), any
    // canvas flush triggered shortly after a touch event reliably hard-resets
    // the chip. We tried gating, TP_INT, suspending the audio task, removing
    // printfs — none eliminated it. Pages are now turned with BOOT instead.


    // ── Periodic redraw (500 ms during LISTENING to leave headroom for the
    //    audio pull, 200 ms otherwise). No touch gating needed — touch is off.
    {
        uint32_t drawInterval = (s_state == GS_LISTENING) ? 500 : 200;
        if (now - s_lastDraw >= drawInterval) {
            s_lastDraw = now;
            draw();
        }
    }

    if (s_state == GS_IDLE || s_state == GS_ERROR) delay(10);
}
