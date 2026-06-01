#include "nanogpt_client.h"

#include "assistant_config.h"
#include "audio_wav.h"
#include "nanogpt_protocol.h"

#include <WiFiClientSecure.h>
#include "HWCDC.h"

#define HTTP_TIMEOUT_MS 30000
#define BOUNDARY "----ESP32Bnd9a7f3c"

extern USBCDC USBSerial;

static String readJsonHttpResponse(WiFiClientSecure &client) {
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
            break;
        } else if (millis() - lastData > 10000) {
            USBSerial.println("[http] read timeout (no data for 10s)");
            break;
        } else {
            delay(10);
        }
    }
    client.stop();

    USBSerial.printf("[http] raw response: %d bytes\n", raw.length());

    int bodyStart = raw.indexOf("\r\n\r\n");
    if (bodyStart < 0) {
        USBSerial.println("[http] no header/body separator found");
        USBSerial.printf("[http] raw: %.200s\n", raw.c_str());
        return "";
    }

    int statusEnd = raw.indexOf("\r\n");
    if (statusEnd > 0) {
        USBSerial.printf("[http] status: %s\n", raw.substring(0, statusEnd).c_str());
    }

    String body = raw.substring(bodyStart + 4);
    raw = String();

    int jsonStart = body.indexOf('{');
    int jsonEnd = body.lastIndexOf('}');
    if (jsonStart < 0 || jsonEnd < 0) {
        USBSerial.printf("[http] no JSON in body (%d bytes): %.200s\n",
                         body.length(), body.c_str());
        return "";
    }
    return body.substring(jsonStart, jsonEnd + 1);
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
    client.setTimeout(HTTP_TIMEOUT_MS / 1000);

    USBSerial.println("[nanogpt] STT connecting...");
    if (!client.connect(NANOGPT_HOST, NANOGPT_PORT)) {
        USBSerial.println("[nanogpt] STT connect failed");
        return "";
    }
    USBSerial.println("[nanogpt] STT connected, sending request...");

    String headers = String("POST ") + NANOGPT_TRANSCRIPTIONS_PATH + " HTTP/1.1\r\n"
        "Host: " + NANOGPT_HOST + "\r\n"
        "Authorization: Bearer " + config.apiKey + "\r\n"
        "Content-Type: multipart/form-data; boundary=" + BOUNDARY + "\r\n"
        "Content-Length: " + String(contentLen) + "\r\n"
        "Connection: close\r\n\r\n";
    client.print(headers);

    client.print(part1hdr);

    uint8_t wavHdr[WAV_HEADER_SIZE];
    wav_build_pcm16_mono_header(wavHdr, pcmBytes, sampleRate);
    client.write(wavHdr, WAV_HEADER_SIZE);

    uint32_t sent = 0;
    while (sent < pcmBytes) {
        uint32_t chunk = pcmBytes - sent;
        if (chunk > 4096) chunk = 4096;
        size_t written = client.write(((const uint8_t *)pcm) + sent, chunk);
        if (written == 0) {
            USBSerial.printf("[nanogpt] STT write stall at %u/%u\n", sent, pcmBytes);
            delay(50);
            written = client.write(((const uint8_t *)pcm) + sent, chunk);
            if (written == 0) {
                USBSerial.println("[nanogpt] STT write failed");
                client.stop();
                return "";
            }
        }
        sent += written;
        if ((sent % 32768) == 0) {
            delay(1);
            USBSerial.printf("[nanogpt] STT upload %u/%u\n", sent, pcmBytes);
        }
    }
    USBSerial.printf("[nanogpt] STT upload done: %u/%u bytes\n", sent, pcmBytes);

    client.print(part1end);
    client.print(part2);
    client.print(part3);
    client.print(closing);
    USBSerial.println("[nanogpt] STT request sent, waiting for response...");

    String body = readJsonHttpResponse(client);
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

    const char *errMsg = doc["error"]["message"] | (const char *)nullptr;
    if (errMsg) {
        USBSerial.printf("[nanogpt] STT error: %s\n", errMsg);
        return String("STT error: ") + errMsg;
    }

    String text = doc["text"] | "";
    USBSerial.printf("[nanogpt] STT result: '%s'\n", text.c_str());
    return text;
}

bool nanogpt_client_post_chat(const char *apiKey, JsonDocument &request, JsonDocument &response) {
    String reqBody;
    serializeJson(request, reqBody);
    USBSerial.printf("[nanogpt] req %d bytes\n", reqBody.length());

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
        "Content-Length: " + String(reqBody.length()) + "\r\n"
        "Connection: close\r\n\r\n");
    client.print(reqBody);
    reqBody = String();

    String body = readJsonHttpResponse(client);
    if (body.length() == 0) {
        USBSerial.println("[nanogpt] empty response");
        return false;
    }
    USBSerial.printf("[nanogpt] body: %.300s\n", body.c_str());

    if (deserializeJson(response, body)) {
        USBSerial.println("[nanogpt] JSON parse failed");
        return false;
    }
    return true;
}
