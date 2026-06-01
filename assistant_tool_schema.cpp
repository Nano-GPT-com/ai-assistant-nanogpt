#include "assistant_tool_schema.h"

static const AssistantToolParamSchema kSetBrightnessParams[] = {
    { "percent", ASSISTANT_TOOL_PARAM_INTEGER, "Brightness percent 0-100", true },
};

static const AssistantToolParamSchema kPlayBeepParams[] = {
    { "frequency_hz", ASSISTANT_TOOL_PARAM_INTEGER, "Tone frequency in Hz (default 1000, 100-8000).", false },
    { "duration_ms", ASSISTANT_TOOL_PARAM_INTEGER, "Duration in milliseconds (default 200, max 2000).", false },
};

static const AssistantToolParamSchema kSaveNoteParams[] = {
    { "text", ASSISTANT_TOOL_PARAM_STRING, "The note text to save (single line).", true },
};

static const AssistantToolParamSchema kListRecentNotesParams[] = {
    { "count", ASSISTANT_TOOL_PARAM_INTEGER, "How many recent notes to return (default 5, max 20).", false },
};

static const AssistantToolSchema kToolSchemas[] = {
    { "get_time", "Returns the current local date and time on the device.", nullptr, 0 },
    { "get_battery_status", "Returns battery percentage and charging state.", nullptr, 0 },
    { "get_uptime", "Returns how long the device has been running since the last boot.", nullptr, 0 },
    { "get_wifi_info", "Returns the WiFi network name and signal strength.", nullptr, 0 },
    { "get_orientation", "Returns the device tilt (pitch and roll in degrees) and a friendly description such as 'face up' or 'on its side'.", nullptr, 0 },
    { "set_brightness", "Sets the screen brightness. Percent 0-100 (0=off, 100=max).", kSetBrightnessParams, sizeof(kSetBrightnessParams) / sizeof(kSetBrightnessParams[0]) },
    { "play_beep", "Plays a short beep through the speaker.", kPlayBeepParams, sizeof(kPlayBeepParams) / sizeof(kPlayBeepParams[0]) },
    { "restart_device", "Reboots the device. Only call when the user explicitly asks to restart.", nullptr, 0 },
    { "power_off", "Powers the device off. Only call when the user explicitly asks to shut down.", nullptr, 0 },
    { "get_weather", "Returns the current weather (temperature, conditions, wind, humidity) for the device's configured location.", nullptr, 0 },
    { "save_note", "Saves a short text note to today's note file on the SD card.", kSaveNoteParams, sizeof(kSaveNoteParams) / sizeof(kSaveNoteParams[0]) },
    { "list_recent_notes", "Lists the most recent notes from today's note file.", kListRecentNotesParams, sizeof(kListRecentNotesParams) / sizeof(kListRecentNotesParams[0]) },
};

const AssistantToolSchema *assistant_tool_schemas(size_t *count) {
    if (count) *count = sizeof(kToolSchemas) / sizeof(kToolSchemas[0]);
    return kToolSchemas;
}

#ifndef UNIT_TEST
static const char *paramTypeName(AssistantToolParamType type) {
    switch (type) {
        case ASSISTANT_TOOL_PARAM_INTEGER: return "integer";
        case ASSISTANT_TOOL_PARAM_STRING:  return "string";
    }
    return "string";
}

void assistant_tool_schema_add_to(JsonArray tools) {
    size_t count = 0;
    const AssistantToolSchema *schemas = assistant_tool_schemas(&count);

    for (size_t i = 0; i < count; i++) {
        const AssistantToolSchema &schema = schemas[i];
        JsonObject tool = tools.add<JsonObject>();
        tool["type"] = "function";

        JsonObject fn = tool["function"].to<JsonObject>();
        fn["name"] = schema.name;
        fn["description"] = schema.description;

        JsonObject params = fn["parameters"].to<JsonObject>();
        params["type"] = "object";
        JsonObject props = params["properties"].to<JsonObject>();
        JsonArray required = params["required"].to<JsonArray>();

        for (size_t p = 0; p < schema.paramCount; p++) {
            const AssistantToolParamSchema &param = schema.params[p];
            JsonObject prop = props[param.name].to<JsonObject>();
            prop["type"] = paramTypeName(param.type);
            prop["description"] = param.description;
            if (param.required) required.add(param.name);
        }
    }
}
#endif
