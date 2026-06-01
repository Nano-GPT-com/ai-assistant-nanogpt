#include "assistant_chat_protocol.h"

const char *assistant_chat_system_prompt() {
    return "Answer in as few words as possible: a single word or short phrase when you can. Reply in the same language the user uses. Use tools when they give accurate device or external data (battery, time, weather, notes, etc.); do not guess. Only call restart_device or power_off when the user explicitly asks for it. Use web search only when current information is required.";
}

const char *assistant_chat_tool_arguments_or_empty(const char *arguments) {
    return (arguments && arguments[0]) ? arguments : "{}";
}

#ifndef UNIT_TEST
void assistant_chat_init_request(JsonDocument &doc,
                                 const char *model,
                                 bool webSearch) {
    doc["model"] = model;
    doc["max_tokens"] = ASSISTANT_CHAT_MAX_TOKENS;
    doc["temperature"] = ASSISTANT_CHAT_TEMPERATURE;
    doc["tool_choice"] = "auto";
    doc["parallel_tool_calls"] = true;

    if (webSearch) {
        JsonObject search = doc["webSearch"].to<JsonObject>();
        search["enabled"] = true;
    }
}
#endif
