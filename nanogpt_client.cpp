#include "nanogpt_client.h"

#include "assistant_config.h"
#include "audio_wav.h"
#include "nanogpt_protocol.h"

#include <WiFiClientSecure.h>
#include "HWCDC.h"

#define HTTP_TIMEOUT_MS 30000
#define STT_TIMEOUT_MS 120000
#define BOUNDARY "----ESP32Bnd9a7f3c"

extern USBCDC USBSerial;

class ChunkedBodyStream : public Stream {
public:
    explicit ChunkedBodyStream(WiFiClientSecure &client)
        : m_client(client) {}

    int available() override {
        return m_done ? 0 : 1;
    }

    int read() override {
        if (m_peeked >= 0) {
            int c = m_peeked;
            m_peeked = -1;
            return c;
        }
        return readByte();
    }

    int peek() override {
        if (m_peeked < 0) m_peeked = readByte();
        return m_peeked;
    }

    void flush() override {}

    size_t write(uint8_t) override {
        return 0;
    }

private:
    WiFiClientSecure &m_client;
    size_t m_chunkRemaining = 0;
    bool m_done = false;
    int m_peeked = -1;

    int readByte() {
        if (m_done) return -1;
        if (!ensureChunk()) return -1;

        int c = m_client.read();
        if (c < 0) return -1;
        m_chunkRemaining--;
        if (m_chunkRemaining == 0) {
            consumeCrlf();
        }
        return c;
    }

    bool ensureChunk() {
        while (m_chunkRemaining == 0 && !m_done) {
            String line = m_client.readStringUntil('\n');
            line.trim();
            int semicolon = line.indexOf(';');
            if (semicolon >= 0) line.remove(semicolon);
            m_chunkRemaining = strtoul(line.c_str(), nullptr, 16);
            if (m_chunkRemaining == 0) {
                m_done = true;
                consumeTrailers();
                return false;
            }
        }
        return m_chunkRemaining > 0;
    }

    void consumeCrlf() {
        if (m_client.peek() == '\r') m_client.read();
        if (m_client.peek() == '\n') m_client.read();
    }

    void consumeTrailers() {
        for (;;) {
            String line = m_client.readStringUntil('\n');
            line.trim();
            if (line.length() == 0) break;
        }
    }
};

static bool readHttpHeaders(WiFiClientSecure &client,
                            bool &chunked,
                            int &statusCode,
                            uint32_t timeoutMs,
                            String &errorOut) {
    uint32_t start = millis();
    while (!client.available() && client.connected() && millis() - start < timeoutMs) {
        delay(10);
    }

    String status = client.readStringUntil('\n');
    status.trim();
    if (status.length() == 0) {
        USBSerial.println("[http] empty status line");
        errorOut = "No response from NanoGPT";
        return false;
    }
    USBSerial.printf("[http] status: %s\n", status.c_str());
    statusCode = 0;
    if (status.startsWith("HTTP/")) {
        int firstSpace = status.indexOf(' ');
        if (firstSpace >= 0) {
            statusCode = status.substring(firstSpace + 1, firstSpace + 4).toInt();
        }
    }

    chunked = false;
    for (;;) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) break;
        String lower = line;
        lower.toLowerCase();
        if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") >= 0) {
            chunked = true;
        }
    }
    return true;
}

static bool deserializeJsonHttpResponse(WiFiClientSecure &client,
                                        JsonDocument &doc,
                                        uint32_t timeoutMs,
                                        String &errorOut) {
    bool chunked = false;
    int statusCode = 0;
    if (!readHttpHeaders(client, chunked, statusCode, timeoutMs, errorOut)) {
        client.stop();
        return false;
    }

    DeserializationError err;
    if (chunked) {
        ChunkedBodyStream body(client);
        err = deserializeJson(doc, body);
    } else {
        err = deserializeJson(doc, client);
    }
    client.stop();

    if (err) {
        USBSerial.printf("[http] JSON parse failed: %s\n", err.c_str());
        errorOut = String("Bad NanoGPT response: ") + err.c_str();
        return false;
    }
    if (statusCode < 200 || statusCode >= 300) {
        const char *errMsg = doc["error"]["message"] | doc["message"] | (const char *)nullptr;
        if (errMsg && errMsg[0]) {
            errorOut = errMsg;
        } else if (statusCode > 0) {
            errorOut = String("NanoGPT HTTP ") + statusCode;
        } else {
            errorOut = "NanoGPT HTTP error";
        }
        USBSerial.printf("[http] non-2xx response: %s\n", errorOut.c_str());
        return false;
    }
    return true;
}

static bool writeClientBytes(WiFiClientSecure &client,
                             const uint8_t *data,
                             uint32_t length,
                             const char *label,
                             String &errorOut) {
    uint32_t sent = 0;
    uint32_t stalledSince = 0;

    while (sent < length) {
        if (!client.connected()) {
            errorOut = String(label) + " socket closed";
            USBSerial.printf("[nanogpt] %s socket closed at %u/%u\n", label, sent, length);
            return false;
        }

        uint32_t chunk = length - sent;
        if (chunk > 1024) chunk = 1024;

        size_t written = client.write(data + sent, chunk);
        if (written == 0) {
            if (stalledSince == 0) {
                stalledSince = millis();
                USBSerial.printf("[nanogpt] %s write stall at %u/%u\n", label, sent, length);
            }
            if (millis() - stalledSince > 8000) {
                errorOut = String(label) + " stalled";
                USBSerial.printf("[nanogpt] %s write stalled too long at %u/%u\n", label, sent, length);
                return false;
            }
            delay(25);
            yield();
            continue;
        }

        stalledSince = 0;
        sent += (uint32_t)written;
        if ((sent % 32768) == 0 || sent == length) {
            USBSerial.printf("[nanogpt] %s upload %u/%u\n", label, sent, length);
            delay(1);
            yield();
        }
    }
    return true;
}

static bool writeClientString(WiFiClientSecure &client,
                              const String &value,
                              const char *label,
                              String &errorOut) {
    return writeClientBytes(client,
                            (const uint8_t *)value.c_str(),
                            (uint32_t)value.length(),
                            label,
                            errorOut);
}

String nanogpt_client_transcribe(const NanoGptClientConfig &config,
                                 const int16_t *pcm,
                                 uint32_t numSamples,
                                 uint32_t sampleRate) {
    uint32_t pcmBytes = numSamples * 2;
    const char *sttModel = (config.sttModel && config.sttModel[0])
        ? config.sttModel
        : DEFAULT_NANOGPT_STT_MODEL;
    const char *language = config.language ? config.language : "";

    String part1hdr = String("--") + BOUNDARY + "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    String part1end = "\r\n";
    String part2 = String("--") + BOUNDARY + "\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        + sttModel + "\r\n";
    String part3;
    if (language[0]) {
        part3 = String("--") + BOUNDARY + "\r\n"
            "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
            + language + "\r\n";
    }
    String closing = String("--") + BOUNDARY + "--\r\n";

    uint32_t contentLen = nanogpt_stt_content_length(
        BOUNDARY,
        sttModel,
        language,
        WAV_HEADER_SIZE,
        pcmBytes);

    USBSerial.printf("[nanogpt] STT free heap: %u, largest: %u\n",
                     ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    USBSerial.printf("[nanogpt] STT: %u samples (%us), %u PCM bytes, content-length=%u\n",
                     numSamples, numSamples / sampleRate, pcmBytes, contentLen);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(STT_TIMEOUT_MS / 1000);

    USBSerial.println("[nanogpt] STT connecting...");
    if (!client.connect(NANOGPT_HOST, NANOGPT_PORT)) {
        USBSerial.println("[nanogpt] STT connect failed");
        return "STT error: connect failed";
    }
    USBSerial.println("[nanogpt] STT connected, sending request...");

    String headers = String("POST ") + NANOGPT_TRANSCRIPTIONS_PATH + " HTTP/1.1\r\n"
        "Host: " + NANOGPT_HOST + "\r\n"
        "Authorization: Bearer " + config.apiKey + "\r\n"
        "Content-Type: multipart/form-data; boundary=" + BOUNDARY + "\r\n"
        "Content-Length: " + String(contentLen) + "\r\n"
        "Connection: close\r\n\r\n";
    String uploadError;
    if (!writeClientString(client, headers, "STT headers", uploadError) ||
        !writeClientString(client, part1hdr, "STT file header", uploadError)) {
        client.stop();
        return String("STT error: ") + uploadError;
    }

    uint8_t wavHdr[WAV_HEADER_SIZE];
    wav_build_pcm16_mono_header(wavHdr, pcmBytes, sampleRate);
    if (!writeClientBytes(client, wavHdr, WAV_HEADER_SIZE, "STT wav header", uploadError)) {
        client.stop();
        return String("STT error: ") + uploadError;
    }

    if (!writeClientBytes(client, (const uint8_t *)pcm, pcmBytes, "STT audio", uploadError)) {
        client.stop();
        return String("STT error: ") + uploadError;
    }
    USBSerial.printf("[nanogpt] STT audio upload done: %u bytes\n", pcmBytes);

    if (!writeClientString(client, part1end, "STT file end", uploadError) ||
        !writeClientString(client, part2, "STT model", uploadError) ||
        !writeClientString(client, part3, "STT language", uploadError) ||
        !writeClientString(client, closing, "STT closing", uploadError)) {
        client.stop();
        return String("STT error: ") + uploadError;
    }
    USBSerial.println("[nanogpt] STT request sent, waiting for response...");

    JsonDocument doc;
    String httpError;
    if (!deserializeJsonHttpResponse(client, doc, STT_TIMEOUT_MS, httpError)) {
        return String("STT error: ") + httpError;
    }

    const char *errMsg = doc["error"]["message"] | (const char *)nullptr;
    if (errMsg) {
        USBSerial.printf("[nanogpt] STT error: %s\n", errMsg);
        return String("STT error: ") + errMsg;
    }

    String text = doc["text"] | "";
    USBSerial.printf("[nanogpt] STT result: '%s'\n", text.c_str());
    if (text.length() == 0) {
        return "STT error: empty transcript";
    }
    return text;
}

bool nanogpt_client_post_chat(const char *apiKey, JsonDocument &request, JsonDocument &response) {
    size_t contentLength = measureJson(request);
    USBSerial.printf("[nanogpt] req %u bytes\n", (unsigned)contentLength);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(HTTP_TIMEOUT_MS / 1000);
    if (!client.connect(NANOGPT_HOST, NANOGPT_PORT)) {
        USBSerial.println("[nanogpt] connect failed");
        return false;
    }
    client.print(String("POST ") + NANOGPT_CHAT_PATH + " HTTP/1.1\r\n"
        "Host: " + NANOGPT_HOST + "\r\n"
        "Authorization: Bearer " + apiKey + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + String(contentLength) + "\r\n"
        "Connection: close\r\n\r\n");
    serializeJson(request, client);

    String httpError;
    if (!deserializeJsonHttpResponse(client, response, HTTP_TIMEOUT_MS, httpError)) {
        USBSerial.printf("[nanogpt] chat response failed: %s\n", httpError.c_str());
        return false;
    }
    return true;
}
