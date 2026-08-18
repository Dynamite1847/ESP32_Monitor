#include "http_fetch/http_fetch.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool overflow;
} response_context_t;

static SemaphoreHandle_t request_mutex;

esp_err_t desk_http_fetch_init(void)
{
    if (request_mutex != NULL) {
        return ESP_OK;
    }
    request_mutex = xSemaphoreCreateMutex();
    return request_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    response_context_t *context = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || context == NULL || event->data_len <= 0) {
        return ESP_OK;
    }
    if (context->length + (size_t)event->data_len > context->capacity) {
        context->overflow = true;
        return ESP_ERR_NO_MEM;
    }
    memcpy(context->data + context->length, event->data, (size_t)event->data_len);
    context->length += (size_t)event->data_len;
    return ESP_OK;
}

esp_err_t desk_http_get_json(
    const char *url,
    const desk_http_header_t *headers,
    size_t header_count,
    size_t response_capacity,
    int timeout_ms,
    char **response,
    size_t *response_length
)
{
    if (url == NULL || response == NULL || response_length == NULL || response_capacity == 0 ||
        timeout_ms <= 0 || (header_count > 0 && headers == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    *response = NULL;
    *response_length = 0;

    if (request_mutex == NULL) {
        const esp_err_t initialization_result = desk_http_fetch_init();
        if (initialization_result != ESP_OK) {
            return initialization_result;
        }
    }
    if (xSemaphoreTake(request_mutex, pdMS_TO_TICKS((uint32_t)timeout_ms + 2000U)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    char *buffer = heap_caps_malloc(response_capacity + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        xSemaphoreGive(request_mutex);
        return ESP_ERR_NO_MEM;
    }
    response_context_t context = {
        .data = buffer,
        .capacity = response_capacity,
    };
    const esp_http_client_config_t configuration = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &context,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = timeout_ms,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .keep_alive_enable = true,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&configuration);
    if (client == NULL) {
        free(buffer);
        xSemaphoreGive(request_mutex);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "DeskConsole-ESP32/1");
    for (size_t i = 0; i < header_count; ++i) {
        if (headers[i].name == NULL || headers[i].value == NULL) {
            esp_http_client_cleanup(client);
            free(buffer);
            xSemaphoreGive(request_mutex);
            return ESP_ERR_INVALID_ARG;
        }
        esp_http_client_set_header(client, headers[i].name, headers[i].value);
    }

    const esp_err_t request_result = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    xSemaphoreGive(request_mutex);
    if (request_result != ESP_OK || context.overflow || status_code != 200 || context.length == 0) {
        free(buffer);
        if (request_result != ESP_OK) {
            return request_result;
        }
        return context.overflow ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    buffer[context.length] = '\0';
    *response = buffer;
    *response_length = context.length;
    return ESP_OK;
}

void desk_http_response_free(char *response)
{
    free(response);
}
