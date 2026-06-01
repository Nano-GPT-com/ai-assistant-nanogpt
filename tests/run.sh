#!/usr/bin/env sh
set -eu

cd "$(dirname "$0")/.."

mkdir -p build/tests
g++ -std=c++17 -Wall -Wextra -Werror -DUNIT_TEST \
  tests/test_pure.cpp \
  assistant_chat_protocol.cpp \
  assistant_config.cpp \
  assistant_tool_schema.cpp \
  assistant_tool_utils.cpp \
  assistant_weather.cpp \
  audio_trim.cpp \
  audio_wav.cpp \
  conversation_history.cpp \
  nanogpt_protocol.cpp \
  -o build/tests/test_pure

./build/tests/test_pure
