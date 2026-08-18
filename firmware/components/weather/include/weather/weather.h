#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_model/app_model.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DESK_QWEATHER_HOST_MAX_BYTES 127
#define DESK_QWEATHER_API_KEY_MAX_BYTES 95

typedef enum {
    DESK_WEATHER_EVENT_UPDATED = 0,
    DESK_WEATHER_EVENT_FETCH_FAILED,
} desk_weather_event_type_t;

typedef struct {
    desk_weather_event_type_t type;
    esp_err_t status;
    desk_weather_state_t state;
} desk_weather_event_t;

/** Starts the weather worker. It remains idle until Wi-Fi and a configuration are available. */
esp_err_t desk_weather_start(void);

/** Stores a dedicated QWeather API host, API key and coordinate in NVS. */
esp_err_t desk_weather_configure_qweather(
    const char *api_host,
    size_t api_host_length,
    const char *api_key,
    size_t api_key_length,
    double longitude,
    double latitude
);

bool desk_weather_has_configuration(void);

/** Wakes or pauses public weather retrieval after a Wi-Fi state change. */
void desk_weather_set_network_available(bool available);

/** Requests an immediate refresh without discarding a valid cached state. */
void desk_weather_request_refresh(void);

bool desk_weather_receive_event(desk_weather_event_t *event, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
