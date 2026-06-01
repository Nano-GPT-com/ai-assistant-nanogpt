#pragma once

#ifndef UNIT_TEST
#include <Arduino.h>
#include <ArduinoJson.h>
#endif

#define ASSISTANT_CHAT_MAX_TOKENS 1024
#define ASSISTANT_CHAT_TEMPERATURE 0.4
#define ASSISTANT_CHAT_MAX_TOOL_ROUNDS 6

const char *assistant_chat_system_prompt();
const char *assistant_chat_tool_arguments_or_empty(const char *arguments);

#ifndef UNIT_TEST
void assistant_chat_init_request(JsonDocument &doc,
                                 const char *model,
                                 bool webSearch);
#endif
