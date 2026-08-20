#include "http_fetch/http_fetch.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "miniz.h"

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

/*
 * 解压一段 gzip 数据。QWeather 现代 API host 无条件返回 gzip（没有 gzip=n 开关，
 * Accept-Encoding 也被忽略），固件必须自解压。要点：
 *   - tinfl 解压器的霍夫曼表约 11KB，太大不能放调用方任务栈（天气/行情任务栈仅
 *     10~12KB），因此在 PSRAM 上分配；
 *   - 用非环绕输出缓冲一次解完，成功判据是 TINFL_STATUS_DONE（失败返回负值，
 *     绝不能用“写出 0 字节”来判失败）；
 *   - 输出缓冲多留 1 字节给 NUL 结尾。
 * gzip 结构（RFC 1952）：10 字节固定头 + 按 FLG 的可选字段 + deflate 原始流 + 8 字节尾。
 */
static esp_err_t inflate_gzip(
    const char *compressed,
    size_t compressed_length,
    size_t max_output,
    char **out_data,
    size_t *out_length
)
{
    if (compressed_length < 18 || (unsigned char)compressed[2] != 0x08) {
        return ESP_ERR_INVALID_RESPONSE;  /* 非 deflate 压缩方法 */
    }
    const unsigned char flg = (unsigned char)compressed[3];
    size_t header = 10;
    if ((flg & 0x04U) != 0) {  /* FEXTRA */
        if (header + 2 > compressed_length) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const size_t extra_length = (size_t)(unsigned char)compressed[header] |
                                    ((size_t)(unsigned char)compressed[header + 1] << 8U);
        header += 2 + extra_length;
    }
    if ((flg & 0x08U) != 0) {  /* FNAME */
        while (header < compressed_length && compressed[header] != '\0') {
            header++;
        }
        header++;
    }
    if ((flg & 0x10U) != 0) {  /* FCOMMENT */
        while (header < compressed_length && compressed[header] != '\0') {
            header++;
        }
        header++;
    }
    if ((flg & 0x02U) != 0) {  /* FHCRC */
        header += 2;
    }
    if (header + 8 > compressed_length) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const size_t deflate_length = compressed_length - header - 8;

    /* 尾部 ISIZE = 原始长度 mod 2^32，用作输出缓冲尺寸的提示（不可信，需夹紧）。 */
    const size_t isize = (size_t)(unsigned char)compressed[compressed_length - 4] |
                         ((size_t)(unsigned char)compressed[compressed_length - 3] << 8U) |
                         ((size_t)(unsigned char)compressed[compressed_length - 2] << 16U) |
                         ((size_t)(unsigned char)compressed[compressed_length - 1] << 24U);
    size_t out_capacity = isize;
    if (out_capacity == 0 || out_capacity > max_output) {
        out_capacity = max_output;
    }
    if (out_capacity < 1024) {
        out_capacity = 1024;
    }

    char *out = heap_caps_malloc(out_capacity + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == NULL) {
        return ESP_ERR_NO_MEM;
    }
    tinfl_decompressor *decompressor =
        heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (decompressor == NULL) {
        free(out);
        return ESP_ERR_NO_MEM;
    }
    tinfl_init(decompressor);
    size_t in_bytes = deflate_length;
    size_t out_bytes = out_capacity;
    const tinfl_status status = tinfl_decompress(
        decompressor,
        (const mz_uint8 *)(compressed + header),
        &in_bytes,
        (mz_uint8 *)out,
        (mz_uint8 *)out,
        &out_bytes,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF
    );
    free(decompressor);
    if (status != TINFL_STATUS_DONE) {
        free(out);
        return ESP_ERR_INVALID_RESPONSE;
    }
    out[out_bytes] = '\0';
    *out_data = out;
    *out_length = out_bytes;
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
    /* QWeather 现代 API host 无条件返回 gzip（无 gzip=n 开关），本组件自行解压。 */
    esp_http_client_set_header(client, "Accept-Encoding", "gzip");
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

    /* gzip 响应（magic 0x1f 0x8b）由 inflate_gzip 在 PSRAM 上解压。 */
    if (context.length >= 2 && (unsigned char)buffer[0] == 0x1f && (unsigned char)buffer[1] == 0x8b) {
        size_t max_output = response_capacity * 8;
        if (max_output > 256 * 1024) {
            max_output = 256 * 1024;
        }
        char *inflated = NULL;
        size_t inflated_length = 0;
        const esp_err_t inflate_result =
            inflate_gzip(buffer, context.length, max_output, &inflated, &inflated_length);
        free(buffer);
        if (inflate_result != ESP_OK) {
            return inflate_result;
        }
        *response = inflated;
        *response_length = inflated_length;
        return ESP_OK;
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
