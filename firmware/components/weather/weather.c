#include "weather/weather.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "http_fetch/http_fetch.h"
#include "nvs.h"
#include "qweather_parser.h"

static const char *TAG = "desk_weather";

enum {
    WEATHER_TASK_STACK_SIZE = 12288,
    WEATHER_EVENT_QUEUE_LENGTH = 3,
    WEATHER_REFRESH_INTERVAL_MS = 15 * 60 * 1000,
    WEATHER_RETRY_INTERVAL_MS = 60 * 1000,
    HTTP_RESPONSE_CAPACITY = 24 * 1024,
    HTTP_TIMEOUT_MS = 10000,
};

typedef struct {
    char api_host[DESK_QWEATHER_HOST_MAX_BYTES + 1];
    char api_key[DESK_QWEATHER_API_KEY_MAX_BYTES + 1];
    double longitude;
    double latitude;
    bool valid;
} weather_configuration_t;

static QueueHandle_t weather_event_queue;
static TaskHandle_t weather_task_handle;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static weather_configuration_t configuration;
static bool network_available;

static bool valid_host_character(char character)
{
    return isalnum((unsigned char)character) || character == '.' || character == '-';
}

static bool valid_key_character(char character)
{
    return isalnum((unsigned char)character) || character == '-' || character == '_';
}

static bool validate_configuration(
    const char *api_host,
    size_t api_host_length,
    const char *api_key,
    size_t api_key_length,
    double longitude,
    double latitude
)
{
    if (api_host == NULL || api_key == NULL || api_host_length == 0 ||
        api_host_length > DESK_QWEATHER_HOST_MAX_BYTES || api_key_length == 0 ||
        api_key_length > DESK_QWEATHER_API_KEY_MAX_BYTES || !isfinite(longitude) ||
        !isfinite(latitude) || longitude < -180 || longitude > 180 ||
        latitude < -90 || latitude > 90 || api_host[0] == '.' ||
        api_host[api_host_length - 1] == '.') {
        return false;
    }
    for (size_t i = 0; i < api_host_length; ++i) {
        if (!valid_host_character(api_host[i])) {
            return false;
        }
    }
    for (size_t i = 0; i < api_key_length; ++i) {
        if (!valid_key_character(api_key[i])) {
            return false;
        }
    }
    return true;
}

static bool load_configuration(weather_configuration_t *loaded)
{
    nvs_handle_t handle;
    if (nvs_open("desk_weather", NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t host_length = sizeof(loaded->api_host);
    size_t key_length = sizeof(loaded->api_key);
    size_t longitude_length = sizeof(loaded->longitude);
    size_t latitude_length = sizeof(loaded->latitude);
    const bool read =
        nvs_get_str(handle, "host", loaded->api_host, &host_length) == ESP_OK &&
        nvs_get_str(handle, "api_key", loaded->api_key, &key_length) == ESP_OK &&
        nvs_get_blob(handle, "longitude", &loaded->longitude, &longitude_length) == ESP_OK &&
        nvs_get_blob(handle, "latitude", &loaded->latitude, &latitude_length) == ESP_OK;
    nvs_close(handle);
    if (!read) {
        return false;
    }
    loaded->valid = validate_configuration(
        loaded->api_host,
        strlen(loaded->api_host),
        loaded->api_key,
        strlen(loaded->api_key),
        loaded->longitude,
        loaded->latitude
    );
    return loaded->valid;
}

static esp_err_t save_configuration(const weather_configuration_t *updated)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open("desk_weather", NVS_READWRITE, &handle), TAG, "Weather NVS open failed");
    esp_err_t result = nvs_set_str(handle, "host", updated->api_host);
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "api_key", updated->api_key);
    }
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, "longitude", &updated->longitude, sizeof(updated->longitude));
    }
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, "latitude", &updated->latitude, sizeof(updated->latitude));
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

static esp_err_t fetch_json(
    const weather_configuration_t *current_configuration,
    const char *path,
    char **json,
    size_t *json_length
)
{
    char url[320];
    const int url_length = snprintf(
        url,
        sizeof(url),
        "https://%s%s",
        current_configuration->api_host,
        path
    );
    if (url_length <= 0 || (size_t)url_length >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const desk_http_header_t headers[] = {
        {.name = "X-QW-Api-Key", .value = current_configuration->api_key},
    };
    return desk_http_get_json(
        url,
        headers,
        sizeof(headers) / sizeof(headers[0]),
        HTTP_RESPONSE_CAPACITY,
        HTTP_TIMEOUT_MS,
        json,
        json_length
    );
}

static esp_err_t fetch_and_parse(
    const weather_configuration_t *current_configuration,
    const char *path,
    bool (*parser)(const char *, size_t, desk_weather_state_t *),
    desk_weather_state_t *state
)
{
    char *json = NULL;
    size_t json_length = 0;
    ESP_RETURN_ON_ERROR(fetch_json(current_configuration, path, &json, &json_length), TAG, "Weather request failed");
    /* 需核对字段单位时用 esp_log_level_set("desk_weather", ESP_LOG_DEBUG) 打开。 */
    ESP_LOGD(TAG, "Weather response %u bytes: %.120s", (unsigned)json_length, json != NULL ? json : "(null)");
    const bool parsed = parser(json, json_length, state);
    desk_http_response_free(json);
    return parsed ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static void queue_weather_event(desk_weather_event_type_t type, esp_err_t status, const desk_weather_state_t *state)
{
    desk_weather_event_t event = {.type = type, .status = status};
    if (state != NULL) {
        event.state = *state;
    }
    if (weather_event_queue == NULL || xQueueSend(weather_event_queue, &event, 0) != pdTRUE) {
        desk_weather_event_t discarded;
        if (weather_event_queue != NULL) {
            xQueueReceive(weather_event_queue, &discarded, 0);
            xQueueSend(weather_event_queue, &event, 0);
        }
    }
}

static bool copy_runtime_state(weather_configuration_t *current_configuration)
{
    bool available;
    taskENTER_CRITICAL(&state_lock);
    *current_configuration = configuration;
    available = network_available;
    taskEXIT_CRITICAL(&state_lock);
    return available && current_configuration->valid;
}

static esp_err_t refresh_weather(
    const weather_configuration_t *current_configuration,
    desk_weather_state_t *weather
)
{
    char path[192];
    int written = snprintf(
        path,
        sizeof(path),
        "/weather/v1/current/%.2f/%.2f?lang=zh&localTime=true",
        current_configuration->latitude,
        current_configuration->longitude
    );
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(
        fetch_and_parse(current_configuration, path, desk_qweather_parse_current, weather),
        TAG,
        "Current weather unavailable"
    );

    written = snprintf(
        path,
        sizeof(path),
        "/weather/v1/hourly/%.2f/%.2f?hours=6&lang=zh&localTime=true",
        current_configuration->latitude,
        current_configuration->longitude
    );
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(
        fetch_and_parse(current_configuration, path, desk_qweather_parse_hourly, weather),
        TAG,
        "Hourly weather unavailable"
    );

    written = snprintf(
        path,
        sizeof(path),
        "/weather/v1/daily/%.2f/%.2f?days=2&lang=zh&localTime=true",
        current_configuration->latitude,
        current_configuration->longitude
    );
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(
        fetch_and_parse(current_configuration, path, desk_qweather_parse_daily, weather),
        TAG,
        "Daily weather unavailable"
    );

    written = snprintf(
        path,
        sizeof(path),
        "/weatheralert/v1/current/%.2f/%.2f?lang=zh&localTime=true",
        current_configuration->latitude,
        current_configuration->longitude
    );
    if (written > 0 && (size_t)written < sizeof(path)) {
        const esp_err_t alert_result =
            fetch_and_parse(current_configuration, path, desk_qweather_parse_alerts, weather);
        if (alert_result != ESP_OK) {
            ESP_LOGW(TAG, "Weather alerts unavailable: %s", esp_err_to_name(alert_result));
        }
    }

    weather->valid = true;
    weather->updated_at_epoch = (uint32_t)time(NULL);
    return ESP_OK;
}

static void weather_task(void *argument)
{
    (void)argument;
    desk_weather_state_t cached_weather = {0};
    TickType_t wait_ticks = portMAX_DELAY;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, wait_ticks);
        weather_configuration_t current_configuration;
        if (!copy_runtime_state(&current_configuration)) {
            wait_ticks = portMAX_DELAY;
            continue;
        }
        if (time(NULL) < 1735689600) {
            wait_ticks = pdMS_TO_TICKS(5000);
            continue;
        }

        desk_weather_state_t updated = cached_weather;
        const esp_err_t result = refresh_weather(&current_configuration, &updated);
        if (result == ESP_OK) {
            cached_weather = updated;
            queue_weather_event(DESK_WEATHER_EVENT_UPDATED, ESP_OK, &cached_weather);
            wait_ticks = pdMS_TO_TICKS(WEATHER_REFRESH_INTERVAL_MS);
        } else {
            queue_weather_event(DESK_WEATHER_EVENT_FETCH_FAILED, result, NULL);
            wait_ticks = pdMS_TO_TICKS(WEATHER_RETRY_INTERVAL_MS);
        }
    }
}

esp_err_t desk_weather_start(void)
{
    if (weather_task_handle != NULL) {
        return ESP_OK;
    }
    weather_event_queue = xQueueCreate(WEATHER_EVENT_QUEUE_LENGTH, sizeof(desk_weather_event_t));
    if (weather_event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    weather_configuration_t loaded = {0};
    load_configuration(&loaded);
    taskENTER_CRITICAL(&state_lock);
    configuration = loaded;
    taskEXIT_CRITICAL(&state_lock);

    const BaseType_t created = xTaskCreateWithCaps(
        weather_task,
        "desk_weather",
        WEATHER_TASK_STACK_SIZE,
        NULL,
        3,
        &weather_task_handle,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (created != pdPASS) {
        weather_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Weather worker started; configured=%d", loaded.valid ? 1 : 0);
    return ESP_OK;
}

esp_err_t desk_weather_configure_qweather(
    const char *api_host,
    size_t api_host_length,
    const char *api_key,
    size_t api_key_length,
    double longitude,
    double latitude
)
{
    if (!validate_configuration(
            api_host,
            api_host_length,
            api_key,
            api_key_length,
            longitude,
            latitude
        )) {
        return ESP_ERR_INVALID_ARG;
    }
    weather_configuration_t updated = {
        .longitude = longitude,
        .latitude = latitude,
        .valid = true,
    };
    memcpy(updated.api_host, api_host, api_host_length);
    memcpy(updated.api_key, api_key, api_key_length);
    ESP_RETURN_ON_ERROR(save_configuration(&updated), TAG, "Weather configuration save failed");

    taskENTER_CRITICAL(&state_lock);
    configuration = updated;
    taskEXIT_CRITICAL(&state_lock);
    desk_weather_request_refresh();
    ESP_LOGI(TAG, "Weather provider configuration updated");
    return ESP_OK;
}

bool desk_weather_has_configuration(void)
{
    taskENTER_CRITICAL(&state_lock);
    const bool configured = configuration.valid;
    taskEXIT_CRITICAL(&state_lock);
    return configured;
}

void desk_weather_set_network_available(bool available)
{
    taskENTER_CRITICAL(&state_lock);
    network_available = available;
    taskEXIT_CRITICAL(&state_lock);
    if (available) {
        desk_weather_request_refresh();
    }
}

void desk_weather_request_refresh(void)
{
    if (weather_task_handle != NULL) {
        xTaskNotifyGive(weather_task_handle);
    }
}

bool desk_weather_receive_event(desk_weather_event_t *event, uint32_t timeout_ms)
{
    if (weather_event_queue == NULL || event == NULL) {
        return false;
    }
    return xQueueReceive(weather_event_queue, event, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
