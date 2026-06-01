#pragma once

#include <stddef.h>

bool assistant_weather_encode_location(const char *location,
                                       char *out,
                                       size_t outSize);

void assistant_weather_geocode_url(const char *encodedLocation,
                                   char *out,
                                   size_t outSize);

void assistant_weather_forecast_url(float latitude,
                                    float longitude,
                                    char *out,
                                    size_t outSize);
