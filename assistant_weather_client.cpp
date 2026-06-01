#include "assistant_weather_client.h"

#include <stdio.h>

bool assistant_weather_encode_location(const char *location,
                                       char *out,
                                       size_t outSize) {
    if (!location || !out || outSize == 0) return false;

    size_t j = 0;
    for (size_t i = 0; location[i] && j + 1 < outSize; i++) {
        char c = location[i];
        if (c == ' ') {
            if (j + 3 >= outSize) break;
            out[j++] = '%';
            out[j++] = '2';
            out[j++] = '0';
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
    return j > 0;
}

void assistant_weather_geocode_url(const char *encodedLocation,
                                   char *out,
                                   size_t outSize) {
    if (!out || outSize == 0) return;
    snprintf(out, outSize,
        "https://geocoding-api.open-meteo.com/v1/search"
        "?name=%s&count=1&language=en&format=json",
        encodedLocation ? encodedLocation : "");
}

void assistant_weather_forecast_url(float latitude,
                                    float longitude,
                                    char *out,
                                    size_t outSize) {
    if (!out || outSize == 0) return;
    snprintf(out, outSize,
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,relative_humidity_2m,wind_speed_10m,"
        "precipitation,weather_code"
        "&timezone=auto&wind_speed_unit=kmh",
        latitude, longitude);
}
