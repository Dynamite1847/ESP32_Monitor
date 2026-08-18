#include "qweather_parser.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static cJSON *parse_root(const char *json, size_t length)
{
    if (json == NULL || length == 0) {
        return NULL;
    }
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &parse_end, false);
    if (root == NULL || !cJSON_IsObject(root) || parse_end == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    while (parse_end < json + length &&
           (*parse_end == ' ' || *parse_end == '\t' || *parse_end == '\r' || *parse_end == '\n')) {
        parse_end++;
    }
    if (parse_end != json + length) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool read_number(const cJSON *object, const char *name, double minimum, double maximum, double *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < minimum || item->valuedouble > maximum) {
        return false;
    }
    *value = item->valuedouble;
    return true;
}

static bool read_nested_number(
    const cJSON *object,
    const char *container_name,
    const char *value_name,
    double minimum,
    double maximum,
    double *value
)
{
    const cJSON *container = cJSON_GetObjectItemCaseSensitive(object, container_name);
    return cJSON_IsObject(container) && read_number(container, value_name, minimum, maximum, value);
}

static bool read_condition(const cJSON *object, desk_weather_code_t *weather_code)
{
    const cJSON *condition = cJSON_GetObjectItemCaseSensitive(object, "condition");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(condition, "code");
    if (!cJSON_IsString(code) || code->valuestring == NULL || code->valuestring[0] == '\0') {
        return false;
    }
    char *end = NULL;
    const long numeric_code = strtol(code->valuestring, &end, 10);
    if (end == code->valuestring || *end != '\0' || numeric_code < 0 || numeric_code > 9999) {
        return false;
    }

    if (numeric_code == 100) {
        *weather_code = DESK_WEATHER_CLEAR;
    } else if (numeric_code >= 101 && numeric_code <= 104) {
        *weather_code = DESK_WEATHER_CLOUDY;
    } else if (numeric_code >= 302 && numeric_code <= 304) {
        *weather_code = DESK_WEATHER_STORM;
    } else if (numeric_code >= 300 && numeric_code <= 399) {
        *weather_code = DESK_WEATHER_RAIN;
    } else if (numeric_code >= 400 && numeric_code <= 499) {
        *weather_code = DESK_WEATHER_SNOW;
    } else if (numeric_code >= 500 && numeric_code <= 515) {
        *weather_code = DESK_WEATHER_FOG;
    } else {
        *weather_code = DESK_WEATHER_UNKNOWN;
    }
    return true;
}

static uint8_t ratio_to_percent(double ratio)
{
    return (uint8_t)lround(fmin(1.0, fmax(0.0, ratio)) * 100.0);
}

static int8_t temperature_to_int8(double temperature)
{
    return (int8_t)lround(fmin(100.0, fmax(-100.0, temperature)));
}

static bool parse_hour(const char *timestamp, uint8_t *hour)
{
    if (timestamp == NULL || hour == NULL) {
        return false;
    }
    const char *separator = strchr(timestamp, 'T');
    if (separator == NULL || separator[1] < '0' || separator[1] > '2' ||
        separator[2] < '0' || separator[2] > '9') {
        return false;
    }
    const unsigned parsed = (unsigned)(separator[1] - '0') * 10U + (unsigned)(separator[2] - '0');
    if (parsed > 23U) {
        return false;
    }
    *hour = (uint8_t)parsed;
    return true;
}

static void copy_utf8(char *destination, size_t capacity, const char *source)
{
    if (capacity == 0) {
        return;
    }
    size_t length = source != NULL ? strlen(source) : 0;
    if (length >= capacity) {
        length = capacity - 1;
        while (length > 0 && ((unsigned char)source[length] & 0xC0U) == 0x80U) {
            length--;
        }
    }
    if (length > 0) {
        memcpy(destination, source, length);
    }
    destination[length] = '\0';
}

bool desk_qweather_parse_current(const char *json, size_t length, desk_weather_state_t *state)
{
    if (state == NULL) {
        return false;
    }
    cJSON *root = parse_root(json, length);
    if (root == NULL) {
        return false;
    }

    double temperature = 0;
    double feels_like = 0;
    double humidity = 0;
    desk_weather_code_t weather_code = DESK_WEATHER_UNKNOWN;
    const bool valid =
        read_nested_number(root, "temperature", "value", -100, 100, &temperature) &&
        read_nested_number(root, "feelsLike", "value", -100, 100, &feels_like) &&
        read_number(root, "humidity", 0, 1, &humidity) &&
        read_condition(root, &weather_code);
    cJSON_Delete(root);
    if (!valid) {
        return false;
    }

    state->current_c = temperature_to_int8(temperature);
    state->feels_like_c = temperature_to_int8(feels_like);
    state->humidity_percent = ratio_to_percent(humidity);
    state->code = weather_code;
    return true;
}

bool desk_qweather_parse_hourly(const char *json, size_t length, desk_weather_state_t *state)
{
    if (state == NULL) {
        return false;
    }
    cJSON *root = parse_root(json, length);
    if (root == NULL) {
        return false;
    }
    const cJSON *hours = cJSON_GetObjectItemCaseSensitive(root, "hours");
    if (!cJSON_IsArray(hours) || cJSON_GetArraySize(hours) < DESK_HOURLY_FORECAST_COUNT) {
        cJSON_Delete(root);
        return false;
    }

    desk_hourly_forecast_t parsed[DESK_HOURLY_FORECAST_COUNT] = {0};
    bool valid = true;
    for (size_t i = 0; i < DESK_HOURLY_FORECAST_COUNT; ++i) {
        const cJSON *entry = cJSON_GetArrayItem(hours, (int)i);
        const cJSON *forecast_time = cJSON_GetObjectItemCaseSensitive(entry, "forecastTime");
        const cJSON *precipitation = cJSON_GetObjectItemCaseSensitive(entry, "precipitation");
        double temperature = 0;
        double probability = 0;
        valid = valid && cJSON_IsObject(entry) && cJSON_IsString(forecast_time) &&
                parse_hour(forecast_time->valuestring, &parsed[i].hour) &&
                read_nested_number(entry, "temperature", "value", -100, 100, &temperature) &&
                cJSON_IsObject(precipitation) &&
                read_number(precipitation, "probability", 0, 1, &probability) &&
                read_condition(entry, &parsed[i].code);
        if (!valid) {
            break;
        }
        parsed[i].temperature_c = temperature_to_int8(temperature);
        parsed[i].precipitation_percent = ratio_to_percent(probability);
    }
    cJSON_Delete(root);
    if (!valid) {
        return false;
    }
    memcpy(state->hourly, parsed, sizeof(parsed));
    return true;
}

static bool parse_daily_entry(const cJSON *entry, desk_daily_forecast_t *forecast)
{
    const cJSON *daytime = cJSON_GetObjectItemCaseSensitive(entry, "daytime");
    const cJSON *precipitation = cJSON_GetObjectItemCaseSensitive(daytime, "precipitation");
    double low = 0;
    double high = 0;
    double probability = 0;
    if (!cJSON_IsObject(entry) || !cJSON_IsObject(daytime) || !cJSON_IsObject(precipitation) ||
        !read_nested_number(entry, "temperatureMin", "value", -100, 100, &low) ||
        !read_nested_number(entry, "temperatureMax", "value", -100, 100, &high) ||
        !read_number(precipitation, "probability", 0, 1, &probability) ||
        !read_condition(daytime, &forecast->code)) {
        return false;
    }
    forecast->low_c = temperature_to_int8(low);
    forecast->high_c = temperature_to_int8(high);
    forecast->precipitation_percent = ratio_to_percent(probability);
    return true;
}

bool desk_qweather_parse_daily(const char *json, size_t length, desk_weather_state_t *state)
{
    if (state == NULL) {
        return false;
    }
    cJSON *root = parse_root(json, length);
    if (root == NULL) {
        return false;
    }
    const cJSON *days = cJSON_GetObjectItemCaseSensitive(root, "days");
    desk_daily_forecast_t today = {0};
    desk_daily_forecast_t tomorrow = {0};
    const bool valid = cJSON_IsArray(days) && cJSON_GetArraySize(days) >= 2 &&
                       parse_daily_entry(cJSON_GetArrayItem(days, 0), &today) &&
                       parse_daily_entry(cJSON_GetArrayItem(days, 1), &tomorrow);
    cJSON_Delete(root);
    if (!valid) {
        return false;
    }
    state->today = today;
    state->tomorrow = tomorrow;
    return true;
}

static unsigned alert_priority(const cJSON *alert)
{
    const cJSON *color = cJSON_GetObjectItemCaseSensitive(alert, "color");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(color, "code");
    if (cJSON_IsString(code) && code->valuestring != NULL) {
        if (strcmp(code->valuestring, "red") == 0 || strcmp(code->valuestring, "purple") == 0) {
            return 4;
        }
        if (strcmp(code->valuestring, "orange") == 0 || strcmp(code->valuestring, "amber") == 0) {
            return 3;
        }
        if (strcmp(code->valuestring, "yellow") == 0) {
            return 2;
        }
        if (strcmp(code->valuestring, "blue") == 0) {
            return 1;
        }
    }
    const cJSON *severity = cJSON_GetObjectItemCaseSensitive(alert, "severity");
    if (cJSON_IsString(severity) && severity->valuestring != NULL) {
        if (strcmp(severity->valuestring, "extreme") == 0) {
            return 4;
        }
        if (strcmp(severity->valuestring, "severe") == 0) {
            return 3;
        }
        if (strcmp(severity->valuestring, "moderate") == 0) {
            return 2;
        }
        if (strcmp(severity->valuestring, "minor") == 0) {
            return 1;
        }
    }
    return 1;
}

bool desk_qweather_parse_alerts(const char *json, size_t length, desk_weather_state_t *state)
{
    if (state == NULL) {
        return false;
    }
    cJSON *root = parse_root(json, length);
    if (root == NULL) {
        return false;
    }
    const cJSON *alerts = cJSON_GetObjectItemCaseSensitive(root, "alerts");
    if (!cJSON_IsArray(alerts)) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *selected = NULL;
    unsigned selected_priority = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, alerts) {
        const cJSON *message_type = cJSON_GetObjectItemCaseSensitive(entry, "messageType");
        const cJSON *message_code = cJSON_GetObjectItemCaseSensitive(message_type, "code");
        const cJSON *headline = cJSON_GetObjectItemCaseSensitive(entry, "headline");
        if (!cJSON_IsObject(entry) || !cJSON_IsString(headline) || headline->valuestring == NULL ||
            (cJSON_IsString(message_code) && strcmp(message_code->valuestring, "cancel") == 0)) {
            continue;
        }
        const unsigned priority = alert_priority(entry);
        if (selected == NULL || priority > selected_priority) {
            selected = entry;
            selected_priority = priority;
        }
    }

    state->alert = (desk_weather_alert_t){0};
    if (selected != NULL) {
        const cJSON *headline = cJSON_GetObjectItemCaseSensitive(selected, "headline");
        state->alert.active = true;
        state->alert.level = selected_priority >= 4 ? DESK_ALERT_RED :
                             selected_priority == 3 ? DESK_ALERT_ORANGE :
                             selected_priority == 2 ? DESK_ALERT_YELLOW : DESK_ALERT_BLUE;
        copy_utf8(state->alert.title, sizeof(state->alert.title), headline->valuestring);
    }
    cJSON_Delete(root);
    return true;
}
