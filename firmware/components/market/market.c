#include "market/market.h"

#include <stdio.h>
#include <time.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "http_fetch/http_fetch.h"
#include "market_parser.h"

static const char *TAG = "desk_market";

enum {
    MARKET_TASK_STACK_SIZE = 10240,
    MARKET_EVENT_QUEUE_LENGTH = 3,
    MARKET_TRADING_REFRESH_MS = 60 * 1000,
    MARKET_CLOSED_REFRESH_MS = 15 * 60 * 1000,
    MARKET_RETRY_MS = 60 * 1000,
    SSE_RESPONSE_CAPACITY = 8 * 1024,
    SZSE_RESPONSE_CAPACITY = 48 * 1024,
    HTTP_TIMEOUT_MS = 10000,
};

static const char *const SSE_URL =
    "https://yunhq.sse.com.cn:32042/v1/csip/list/self/000001_000688_000300_000905"
    "?select=prev_close,last,chg_rate,code,name";

static QueueHandle_t market_event_queue;
static TaskHandle_t market_task_handle;
static portMUX_TYPE network_lock = portMUX_INITIALIZER_UNLOCKED;
static bool network_available;

static bool is_trading_time(void)
{
    const time_t now = time(NULL);
    struct tm local_time = {0};
    if (now < 1735689600 || localtime_r(&now, &local_time) == NULL ||
        local_time.tm_wday == 0 || local_time.tm_wday == 6) {
        return false;
    }
    const int minute = local_time.tm_hour * 60 + local_time.tm_min;
    return (minute >= 9 * 60 + 30 && minute <= 11 * 60 + 30) ||
           (minute >= 13 * 60 && minute <= 15 * 60);
}

static bool network_is_available(void)
{
    taskENTER_CRITICAL(&network_lock);
    const bool available = network_available;
    taskEXIT_CRITICAL(&network_lock);
    return available;
}

static void queue_market_event(desk_market_event_type_t type, esp_err_t status, const desk_market_state_t *state)
{
    desk_market_event_t event = {.type = type, .status = status};
    if (state != NULL) {
        event.state = *state;
    }
    if (market_event_queue == NULL || xQueueSend(market_event_queue, &event, 0) != pdTRUE) {
        desk_market_event_t discarded;
        if (market_event_queue != NULL) {
            xQueueReceive(market_event_queue, &discarded, 0);
            xQueueSend(market_event_queue, &event, 0);
        }
    }
}

static esp_err_t fetch_sse(desk_market_state_t *state)
{
    const desk_http_header_t headers[] = {
        {.name = "Referer", .value = "https://www.sse.com.cn/"},
    };
    char *json = NULL;
    size_t json_length = 0;
    ESP_RETURN_ON_ERROR(
        desk_http_get_json(
            SSE_URL,
            headers,
            sizeof(headers) / sizeof(headers[0]),
            SSE_RESPONSE_CAPACITY,
            HTTP_TIMEOUT_MS,
            &json,
            &json_length
        ),
        TAG,
        "SSE index request failed"
    );
    const bool parsed = desk_market_parse_sse(json, json_length, state);
    desk_http_response_free(json);
    return parsed ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t fetch_szse(const char *code, const char *name, desk_market_index_t *index)
{
    char url[256];
    const int url_length = snprintf(
        url,
        sizeof(url),
        "https://www.szse.cn/api/market/ssjjhq/getTimeData?marketId=1&code=%s&random=%lu",
        code,
        (unsigned long)time(NULL)
    );
    if (url_length <= 0 || (size_t)url_length >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const desk_http_header_t headers[] = {
        {.name = "Referer", .value = "https://www.szse.cn/market/trend/index.html"},
    };
    char *json = NULL;
    size_t json_length = 0;
    ESP_RETURN_ON_ERROR(
        desk_http_get_json(
            url,
            headers,
            sizeof(headers) / sizeof(headers[0]),
            SZSE_RESPONSE_CAPACITY,
            HTTP_TIMEOUT_MS,
            &json,
            &json_length
        ),
        TAG,
        "SZSE index request failed"
    );
    const bool parsed = desk_market_parse_szse(json, json_length, code, name, index);
    desk_http_response_free(json);
    return parsed ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t refresh_market(desk_market_state_t *state)
{
    ESP_RETURN_ON_ERROR(fetch_sse(state), TAG, "SSE index parsing failed");
    ESP_RETURN_ON_ERROR(fetch_szse("399001", "深证成指", &state->indices[1]), TAG, "SZSE component index failed");
    ESP_RETURN_ON_ERROR(fetch_szse("399006", "创业板指", &state->indices[2]), TAG, "ChiNext index failed");
    state->valid = true;
    state->trading = is_trading_time();
    state->updated_at_epoch = (uint32_t)time(NULL);
    return ESP_OK;
}

static void market_task(void *argument)
{
    (void)argument;
    desk_market_state_t cached_market = {0};
    TickType_t wait_ticks = portMAX_DELAY;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, wait_ticks);
        if (!network_is_available()) {
            wait_ticks = portMAX_DELAY;
            continue;
        }
        if (time(NULL) < 1735689600) {
            wait_ticks = pdMS_TO_TICKS(5000);
            continue;
        }

        desk_market_state_t updated = cached_market;
        const esp_err_t result = refresh_market(&updated);
        if (result == ESP_OK) {
            cached_market = updated;
            queue_market_event(DESK_MARKET_EVENT_UPDATED, ESP_OK, &cached_market);
            wait_ticks = pdMS_TO_TICKS(
                cached_market.trading ? MARKET_TRADING_REFRESH_MS : MARKET_CLOSED_REFRESH_MS
            );
        } else {
            queue_market_event(DESK_MARKET_EVENT_FETCH_FAILED, result, NULL);
            wait_ticks = pdMS_TO_TICKS(MARKET_RETRY_MS);
        }
    }
}

esp_err_t desk_market_start(void)
{
    if (market_task_handle != NULL) {
        return ESP_OK;
    }
    market_event_queue = xQueueCreate(MARKET_EVENT_QUEUE_LENGTH, sizeof(desk_market_event_t));
    if (market_event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const BaseType_t created = xTaskCreateWithCaps(
        market_task,
        "desk_market",
        MARKET_TASK_STACK_SIZE,
        NULL,
        3,
        &market_task_handle,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (created != pdPASS) {
        market_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "A-share index worker started");
    return ESP_OK;
}

void desk_market_set_network_available(bool available)
{
    taskENTER_CRITICAL(&network_lock);
    network_available = available;
    taskEXIT_CRITICAL(&network_lock);
    if (available) {
        desk_market_request_refresh();
    }
}

void desk_market_request_refresh(void)
{
    if (market_task_handle != NULL) {
        xTaskNotifyGive(market_task_handle);
    }
}

bool desk_market_receive_event(desk_market_event_t *event, uint32_t timeout_ms)
{
    if (market_event_queue == NULL || event == NULL) {
        return false;
    }
    return xQueueReceive(market_event_queue, event, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
