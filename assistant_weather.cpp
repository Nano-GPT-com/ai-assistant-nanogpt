#include "assistant_weather.h"

const char *weather_wmo_description(int code) {
    if (code == 0) return "clear";
    if (code == 1 || code == 2) return "partly cloudy";
    if (code == 3) return "overcast";
    if (code >= 45 && code <= 48) return "foggy";
    if (code >= 51 && code <= 57) return "drizzle";
    if (code >= 61 && code <= 67) return "rain";
    if (code >= 71 && code <= 77) return "snow";
    if (code >= 80 && code <= 82) return "rain showers";
    if (code >= 85 && code <= 86) return "snow showers";
    if (code >= 95 && code <= 99) return "thunderstorm";
    return "unknown conditions";
}
