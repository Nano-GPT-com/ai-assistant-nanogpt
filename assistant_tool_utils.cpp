#include "assistant_tool_utils.h"

#include <math.h>

int assistant_clamp_brightness_percent(int percent) {
    if (percent < 0) return -1;
    if (percent > 100) return 100;
    return percent;
}

int assistant_clamp_beep_frequency_hz(int frequencyHz) {
    if (frequencyHz < 100) return 100;
    if (frequencyHz > 8000) return 8000;
    return frequencyHz;
}

int assistant_clamp_beep_duration_ms(int durationMs) {
    if (durationMs < 20) return 20;
    if (durationMs > 2000) return 2000;
    return durationMs;
}

int assistant_clamp_note_count(int count) {
    if (count < 1) return 1;
    if (count > 20) return 20;
    return count;
}

const char *assistant_orientation_description(float ax, float ay, float az) {
    if (az > 0.85f) return "face up";
    if (az < -0.85f) return "face down";
    if (ax > 0.85f) return "upright, top up";
    if (ax < -0.85f) return "upside down";
    if (fabsf(ay) > 0.7f) return "on its side";
    return "tilted";
}
