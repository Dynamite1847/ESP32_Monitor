#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_model/app_model.h"

bool desk_qweather_parse_current(const char *json, size_t length, desk_weather_state_t *state);
bool desk_qweather_parse_hourly(const char *json, size_t length, desk_weather_state_t *state);
bool desk_qweather_parse_daily(const char *json, size_t length, desk_weather_state_t *state);
bool desk_qweather_parse_alerts(const char *json, size_t length, desk_weather_state_t *state);
