#pragma once

#include <stddef.h>

enum AssistantToolParamType {
    ASSISTANT_TOOL_PARAM_INTEGER,
    ASSISTANT_TOOL_PARAM_STRING,
};

struct AssistantToolParamSchema {
    const char *name;
    AssistantToolParamType type;
    const char *description;
    bool required;
};

struct AssistantToolSchema {
    const char *name;
    const char *description;
    const AssistantToolParamSchema *params;
    size_t paramCount;
};

const AssistantToolSchema *assistant_tool_schemas(size_t *count);

#ifndef UNIT_TEST
#include <ArduinoJson.h>
void assistant_tool_schema_add_to(JsonArray tools);
#endif
