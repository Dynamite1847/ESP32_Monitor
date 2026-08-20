#include "app_auth/app_auth.h"
#include "app_model/app_model.h"
#include "ble_link/ble_link.h"
#include "ble_protocol/ble_protocol.h"
#include "board/board.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "http_fetch/http_fetch.h"
#include "market/market.h"
#include "network/network.h"
#include "nvs_flash.h"
#include "privacy/privacy_state_machine.h"
#include "storage/storage.h"
#include "ui/ui.h"
#include "weather/weather.h"

static const char *TAG = "desk_console";

enum {
    HEARTBEAT_TIMEOUT_MS = 6000,
    APPLICATION_AUTH_TIMEOUT_MS = 30000,
    SUPERVISOR_POLL_MS = 250,
    SUPERVISOR_STACK_SIZE = 8192,
    /* 不可信 payload 的 JSON 最大嵌套层数；正常业务帧不超过 4~5 层。 */
    JSON_MAX_NESTING_DEPTH = 16,
};

typedef struct {
    desk_app_state_t app_state;
    desk_privacy_context_t privacy;
    desk_sequence_window_t rx_sequences;
    lv_indev_t *touch_input;
    uint32_t auth_started_ms;
    uint16_t tx_sequence;
    QueueHandle_t ui_action_queue;
    uint32_t next_diagnostics_update_ms;
} desk_runtime_t;

static desk_runtime_t runtime;

static uint32_t uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void apply_ui_state(bool show_home)
{
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        ESP_LOGE(TAG, "Unable to lock UI while applying state");
        return;
    }
    if (show_home) {
        desk_ui_show_home();
    }
    desk_ui_apply_state(&runtime.app_state);
    esp_lv_adapter_unlock();
}

static void apply_privacy_actions(desk_privacy_actions_t actions)
{
#if CONFIG_DESK_BRINGUP_DISPLAY_ON
    const desk_privacy_actions_t bypassed =
        DESK_PRIVACY_ACTION_BACKLIGHT_OFF | DESK_PRIVACY_ACTION_TOUCH_DISABLE;
    if ((actions & bypassed) != 0) {
        ESP_LOGW(TAG, "Bring-up mode bypassed automatic screen-off and touch lock");
        actions &= ~bypassed;
    }
#endif

    if ((actions & DESK_PRIVACY_ACTION_BACKLIGHT_OFF) != 0) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(desk_board_backlight_set(false));
    }
    if ((actions & DESK_PRIVACY_ACTION_CLEAR_PRIVATE_DATA) != 0) {
        desk_app_state_clear_private(&runtime.app_state);
    }

    const bool touch_change =
        (actions & (DESK_PRIVACY_ACTION_TOUCH_ENABLE | DESK_PRIVACY_ACTION_TOUCH_DISABLE)) != 0;
    const bool show_home = (actions & DESK_PRIVACY_ACTION_SHOW_HOME) != 0;
    if (touch_change || show_home || (actions & DESK_PRIVACY_ACTION_CLEAR_PRIVATE_DATA) != 0) {
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            if (runtime.touch_input != NULL) {
                if ((actions & DESK_PRIVACY_ACTION_TOUCH_ENABLE) != 0) {
                    lv_indev_enable(runtime.touch_input, true);
                } else if ((actions & DESK_PRIVACY_ACTION_TOUCH_DISABLE) != 0) {
                    lv_indev_enable(runtime.touch_input, false);
                }
            }
            if (show_home) {
                desk_ui_show_home();
            }
            desk_ui_apply_state(&runtime.app_state);
            esp_lv_adapter_unlock();
        }
    }

    if ((actions & DESK_PRIVACY_ACTION_BACKLIGHT_ON) != 0) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(desk_board_backlight_set(true));
    }
    if ((actions & DESK_PRIVACY_ACTION_DISCONNECT_BLE) != 0) {
        const esp_err_t result = desk_ble_link_disconnect();
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "BLE disconnect request failed: %s", esp_err_to_name(result));
        }
    }
}

static bool send_protocol_message(
    desk_message_type_t message_type,
    uint8_t flags,
    const uint8_t *payload,
    size_t payload_length
)
{
    if (payload_length > UINT16_MAX) {
        return false;
    }

    const desk_protocol_frame_t frame = {
        .flags = flags,
        .message_type = message_type,
        .sequence = runtime.tx_sequence++,
        .payload = payload,
        .payload_length = (uint16_t)payload_length,
    };
    /* 只由单线程 supervisor_task 调用；用静态缓冲避免 524 字节占用任务栈。 */
    static uint8_t encoded[DESK_PROTOCOL_MAX_FRAME_SIZE];
    size_t encoded_length = 0;
    if (desk_protocol_encode(&frame, encoded, sizeof(encoded), &encoded_length) != DESK_PROTOCOL_OK) {
        return false;
    }
    return desk_ble_link_send_frame(encoded, encoded_length) == ESP_OK;
}

static bool send_auth_result(desk_auth_result_code_t result, bool enrolled)
{
    const uint8_t payload[DESK_AUTH_RESULT_SIZE] = {
        DESK_AUTH_PAYLOAD_VERSION,
        (uint8_t)result,
        enrolled ? 1U : 0U,
    };
    return send_protocol_message(
        DESK_MESSAGE_AUTH_RESULT,
        DESK_FRAME_FLAG_RESPONSE | (result == DESK_AUTH_RESULT_SUCCEEDED ? 0U : DESK_FRAME_FLAG_ERROR),
        payload,
        sizeof(payload)
    );
}

static void queue_ui_action(desk_ui_action_id_t action_id, void *context)
{
    (void)context;
    const uint16_t queued_action = (uint16_t)action_id;
    if (runtime.ui_action_queue == NULL ||
        xQueueSend(runtime.ui_action_queue, &queued_action, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Unable to queue UI action %u", queued_action);
    }
}

static void send_ui_action(uint16_t action_id)
{
    if (runtime.privacy.state != DESK_PRIVACY_ACTIVE) {
        return;
    }
    char payload[48];
    const int payload_length = snprintf(
        payload,
        sizeof(payload),
        "{\"actionId\":%u,\"gesture\":\"tap\"}",
        action_id
    );
    if (payload_length <= 0 || (size_t)payload_length >= sizeof(payload) ||
        !send_protocol_message(
            DESK_MESSAGE_ACTION_TRIGGER,
            DESK_FRAME_FLAG_NONE,
            (const uint8_t *)payload,
            (size_t)payload_length
        )) {
        ESP_LOGW(TAG, "Unable to send UI action %u", action_id);
    }
}

static bool json_read_u32(
    const cJSON *object,
    const char *name,
    uint32_t maximum,
    uint32_t *value
)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > maximum) {
        return false;
    }
    const uint32_t integer = (uint32_t)item->valuedouble;
    if ((double)integer != item->valuedouble) {
        return false;
    }
    *value = integer;
    return true;
}

/*
 * 在把不可信 payload 交给 cJSON 前，先扫一遍限制最大嵌套深度。cJSON 按嵌套层数递归，
 * 512 字节的 payload 可塞进约 250 层 "[[[[…"，会先递归到爆 supervisor 任务栈才触发
 * cJSON 自己的 1000 层保护。这个 O(n)、感知字符串的预扫把深度钉在业务上限内。
 */
static bool json_depth_within_limit(const uint8_t *payload, size_t length, int max_depth)
{
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = 0; i < length; ++i) {
        const char character = (char)payload[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                in_string = false;
            }
            continue;
        }
        if (character == '"') {
            in_string = true;
        } else if (character == '{' || character == '[') {
            if (++depth > max_depth) {
                return false;
            }
        } else if (character == '}' || character == ']') {
            if (depth > 0) {
                depth--;
            }
        }
    }
    return true;
}

static cJSON *parse_json_object(const uint8_t *payload, size_t payload_length)
{
    if (payload == NULL || payload_length == 0) {
        return NULL;
    }
    if (!json_depth_within_limit(payload, payload_length, JSON_MAX_NESTING_DEPTH)) {
        return NULL;
    }

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(
        (const char *)payload,
        payload_length,
        &parse_end,
        false
    );
    if (root == NULL || !cJSON_IsObject(root) || parse_end == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    while (parse_end < (const char *)payload + payload_length &&
           (*parse_end == ' ' || *parse_end == '\t' || *parse_end == '\r' || *parse_end == '\n')) {
        parse_end++;
    }
    if (parse_end != (const char *)payload + payload_length) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool apply_system_state_payload(const uint8_t *payload, size_t payload_length)
{
    cJSON *root = parse_json_object(payload, payload_length);
    if (root == NULL) {
        return false;
    }

    uint32_t cpu_x10 = 0;
    uint32_t memory_x10 = 0;
    uint32_t disk_free_gb = 0;
    uint32_t network_up_kbps = 0;
    uint32_t network_down_kbps = 0;
    uint32_t battery_percent = 0;
    const bool valid =
        json_read_u32(root, "cpu10", 1000, &cpu_x10) &&
        json_read_u32(root, "memory10", 1000, &memory_x10) &&
        json_read_u32(root, "diskFreeGB", UINT32_MAX, &disk_free_gb) &&
        json_read_u32(root, "upKbps", UINT32_MAX, &network_up_kbps) &&
        json_read_u32(root, "downKbps", UINT32_MAX, &network_down_kbps) &&
        json_read_u32(root, "battery", UINT8_MAX, &battery_percent) &&
        (battery_percent <= 100 || battery_percent == UINT8_MAX);
    cJSON_Delete(root);
    if (!valid) {
        return false;
    }

    const uint32_t next_sample_sequence = runtime.app_state.system.sample_sequence + 1U;
    runtime.app_state.system = (desk_system_state_t){
        .valid = true,
        .sample_sequence = next_sample_sequence != 0 ? next_sample_sequence : 1U,
        .cpu_x10_percent = (uint16_t)cpu_x10,
        .memory_x10_percent = (uint16_t)memory_x10,
        .disk_free_gb = disk_free_gb,
        .network_up_kbps = network_up_kbps,
        .network_down_kbps = network_down_kbps,
        .battery_percent = (uint8_t)battery_percent,
    };
    runtime.app_state.revision++;
    apply_ui_state(false);
    return true;
}

static bool apply_control_layout_payload(const uint8_t *payload, size_t payload_length)
{
    cJSON *root = parse_json_object(payload, payload_length);
    if (root == NULL) {
        return false;
    }

    const cJSON *active_app = cJSON_GetObjectItemCaseSensitive(root, "activeApp");
    uint32_t action_count = 0;
    const bool valid =
        cJSON_IsString(active_app) && active_app->valuestring != NULL &&
        active_app->valuestring[0] != '\0' && strlen(active_app->valuestring) < sizeof(runtime.app_state.control.active_app) &&
        json_read_u32(root, "actionCount", 6, &action_count);
    if (valid) {
        snprintf(
            runtime.app_state.control.active_app,
            sizeof(runtime.app_state.control.active_app),
            "%s",
            active_app->valuestring
        );
        runtime.app_state.control.action_count = (uint8_t)action_count;
    }
    cJSON_Delete(root);
    if (!valid) {
        return false;
    }

    runtime.app_state.revision++;
    apply_ui_state(false);
    return true;
}

static bool parse_ai_status(const char *status, desk_ai_task_status_t *output)
{
    if (status == NULL || output == NULL) {
        return false;
    }
    static const struct {
        const char *name;
        desk_ai_task_status_t value;
    } statuses[] = {
        {"idle", DESK_AI_IDLE},
        {"running", DESK_AI_RUNNING},
        {"waiting_permission", DESK_AI_WAITING_PERMISSION},
        {"waiting_input", DESK_AI_WAITING_INPUT},
        {"completed", DESK_AI_COMPLETED},
        {"failed", DESK_AI_FAILED},
    };
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
        if (strcmp(status, statuses[i].name) == 0) {
            *output = statuses[i].value;
            return true;
        }
    }
    return false;
}

static bool apply_ai_state_payload(const uint8_t *payload, size_t payload_length)
{
    cJSON *root = parse_json_object(payload, payload_length);
    const cJSON *providers = root != NULL
                                 ? cJSON_GetObjectItemCaseSensitive(root, "providers")
                                 : NULL;
    if (!cJSON_IsArray(providers) || cJSON_GetArraySize(providers) != DESK_AI_PROVIDER_COUNT) {
        cJSON_Delete(root);
        return false;
    }

    desk_ai_state_t state = {.valid = true};
    bool seen[DESK_AI_PROVIDER_COUNT] = {false};
    const cJSON *provider = NULL;
    cJSON_ArrayForEach(provider, providers) {
        const cJSON *identifier = cJSON_GetObjectItemCaseSensitive(provider, "id");
        const cJSON *usage_available = cJSON_GetObjectItemCaseSensitive(provider, "usageAvailable");
        const cJSON *secondary_available = cJSON_GetObjectItemCaseSensitive(provider, "secondaryAvailable");
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(provider, "status");
        if (!cJSON_IsString(identifier) || identifier->valuestring == NULL ||
            !cJSON_IsBool(usage_available) || !cJSON_IsBool(secondary_available) ||
            !cJSON_IsString(status) || status->valuestring == NULL) {
            cJSON_Delete(root);
            return false;
        }

        size_t index = DESK_AI_PROVIDER_COUNT;
        const char *display_name = NULL;
        if (strcmp(identifier->valuestring, "codex") == 0) {
            index = 0;
            display_name = "Codex";
        } else if (strcmp(identifier->valuestring, "claude") == 0) {
            index = 1;
            display_name = "Claude Code";
        }
        if (index >= DESK_AI_PROVIDER_COUNT || seen[index]) {
            cJSON_Delete(root);
            return false;
        }

        uint32_t primary = 0;
        uint32_t secondary = 0;
        uint32_t primary_window = 0;
        uint32_t secondary_window = 0;
        uint32_t tasks = 0;
        uint32_t elapsed = 0;
        desk_ai_task_status_t task_status = DESK_AI_IDLE;
        if (!json_read_u32(provider, "primary", 100, &primary) ||
            !json_read_u32(provider, "secondary", 100, &secondary) ||
            !json_read_u32(provider, "primaryWindow", UINT16_MAX, &primary_window) ||
            !json_read_u32(provider, "secondaryWindow", UINT16_MAX, &secondary_window) ||
            !json_read_u32(provider, "tasks", 32, &tasks) ||
            !json_read_u32(provider, "elapsed", UINT32_MAX, &elapsed) ||
            !parse_ai_status(status->valuestring, &task_status)) {
            cJSON_Delete(root);
            return false;
        }

        desk_ai_provider_state_t *destination = &state.providers[index];
        snprintf(destination->provider, sizeof(destination->provider), "%s", display_name);
        destination->usage_available = cJSON_IsTrue(usage_available);
        destination->secondary_usage_available = cJSON_IsTrue(secondary_available);
        destination->primary_usage_percent = (uint8_t)primary;
        destination->secondary_usage_percent = (uint8_t)secondary;
        destination->primary_window_minutes = (uint16_t)primary_window;
        destination->secondary_window_minutes = (uint16_t)secondary_window;
        destination->active_task_count = (uint8_t)tasks;
        destination->task_status = task_status;
        destination->elapsed_seconds = elapsed;
        seen[index] = true;
    }
    cJSON_Delete(root);
    if (!seen[0] || !seen[1]) {
        return false;
    }

    runtime.app_state.ai = state;
    runtime.app_state.revision++;
    apply_ui_state(false);
    return true;
}

static bool apply_media_state_payload(const uint8_t *payload, size_t payload_length)
{
    cJSON *root = parse_json_object(payload, payload_length);
    if (root == NULL) {
        return false;
    }

    const cJSON *valid_item = cJSON_GetObjectItemCaseSensitive(root, "valid");
    const cJSON *playing = cJSON_GetObjectItemCaseSensitive(root, "playing");
    const cJSON *title_hidden = cJSON_GetObjectItemCaseSensitive(root, "titleHidden");
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "title");
    uint32_t volume = 0;
    uint32_t position = 0;
    uint32_t duration = 0;
    const bool valid =
        cJSON_IsBool(valid_item) && cJSON_IsTrue(valid_item) &&
        cJSON_IsBool(playing) && cJSON_IsBool(title_hidden) &&
        cJSON_IsString(title) && title->valuestring != NULL && title->valuestring[0] != '\0' &&
        strlen(title->valuestring) < sizeof(runtime.app_state.media.title) &&
        json_read_u32(root, "volume", 100, &volume) &&
        json_read_u32(root, "position", UINT32_MAX, &position) &&
        json_read_u32(root, "duration", UINT32_MAX, &duration) &&
        (duration == 0 || position <= duration);
    if (valid) {
        runtime.app_state.media = (desk_media_state_t){
            .valid = true,
            .playing = cJSON_IsTrue(playing),
            .title_hidden = cJSON_IsTrue(title_hidden),
            .volume_percent = (uint8_t)volume,
            .position_seconds = position,
            .duration_seconds = duration,
        };
        snprintf(
            runtime.app_state.media.title,
            sizeof(runtime.app_state.media.title),
            "%s",
            title->valuestring
        );
    }
    cJSON_Delete(root);
    if (!valid) {
        return false;
    }

    runtime.app_state.revision++;
    apply_ui_state(false);
    return true;
}

static bool apply_wifi_provision_payload(const uint8_t *payload, size_t payload_length, esp_err_t *result)
{
    cJSON *root = parse_json_object(payload, payload_length);
    if (root == NULL || result == NULL) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (!cJSON_IsString(ssid) || ssid->valuestring == NULL ||
        !cJSON_IsString(password) || password->valuestring == NULL) {
        cJSON_Delete(root);
        return false;
    }
    const size_t ssid_length = strlen(ssid->valuestring);
    const size_t password_length = strlen(password->valuestring);
    if (ssid_length == 0 || ssid_length > DESK_WIFI_SSID_MAX_BYTES ||
        password_length > DESK_WIFI_PASSWORD_MAX_BYTES ||
        (password_length > 0 && password_length < 8)) {
        cJSON_Delete(root);
        return false;
    }

    *result = desk_network_provision(
        ssid->valuestring,
        ssid_length,
        password->valuestring,
        password_length
    );
    cJSON_Delete(root);
    return true;
}

static void send_wifi_result(esp_err_t result)
{
    char payload[48];
    const int payload_length = snprintf(
        payload,
        sizeof(payload),
        "{\"ok\":%s,\"code\":%ld}",
        result == ESP_OK ? "true" : "false",
        (long)result
    );
    if (payload_length <= 0 || (size_t)payload_length >= sizeof(payload) ||
        !send_protocol_message(
            DESK_MESSAGE_WIFI_RESULT,
            DESK_FRAME_FLAG_RESPONSE | (result == ESP_OK ? 0U : DESK_FRAME_FLAG_ERROR),
            (const uint8_t *)payload,
            (size_t)payload_length
        )) {
        ESP_LOGW(TAG, "Unable to send Wi-Fi provisioning result");
    }
}

static bool apply_weather_config_payload(const uint8_t *payload, size_t payload_length, esp_err_t *result)
{
    cJSON *root = parse_json_object(payload, payload_length);
    if (root == NULL || result == NULL) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *provider = cJSON_GetObjectItemCaseSensitive(root, "provider");
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(root, "host");
    const cJSON *api_key = cJSON_GetObjectItemCaseSensitive(root, "apiKey");
    const cJSON *longitude = cJSON_GetObjectItemCaseSensitive(root, "longitude");
    const cJSON *latitude = cJSON_GetObjectItemCaseSensitive(root, "latitude");
    if (!cJSON_IsString(provider) || provider->valuestring == NULL ||
        strcmp(provider->valuestring, "qweather") != 0 ||
        !cJSON_IsString(host) || host->valuestring == NULL ||
        !cJSON_IsString(api_key) || api_key->valuestring == NULL ||
        !cJSON_IsNumber(longitude) || !isfinite(longitude->valuedouble) ||
        !cJSON_IsNumber(latitude) || !isfinite(latitude->valuedouble)) {
        cJSON_Delete(root);
        return false;
    }

    const size_t host_length = strlen(host->valuestring);
    const size_t api_key_length = strlen(api_key->valuestring);
    if (host_length == 0 || host_length > DESK_QWEATHER_HOST_MAX_BYTES ||
        api_key_length == 0 || api_key_length > DESK_QWEATHER_API_KEY_MAX_BYTES) {
        cJSON_Delete(root);
        return false;
    }
    *result = desk_weather_configure_qweather(
        host->valuestring,
        host_length,
        api_key->valuestring,
        api_key_length,
        longitude->valuedouble,
        latitude->valuedouble
    );
    cJSON_Delete(root);
    return true;
}

static void send_weather_config_result(esp_err_t result)
{
    char payload[48];
    const int payload_length = snprintf(
        payload,
        sizeof(payload),
        "{\"ok\":%s,\"code\":%ld}",
        result == ESP_OK ? "true" : "false",
        (long)result
    );
    if (payload_length <= 0 || (size_t)payload_length >= sizeof(payload) ||
        !send_protocol_message(
            DESK_MESSAGE_WEATHER_CONFIG_RESULT,
            DESK_FRAME_FLAG_RESPONSE | (result == ESP_OK ? 0U : DESK_FRAME_FLAG_ERROR),
            (const uint8_t *)payload,
            (size_t)payload_length
        )) {
        ESP_LOGW(TAG, "Unable to send weather configuration result");
    }
}

static void handle_protocol_frame(const desk_ble_event_t *event)
{
    desk_protocol_frame_t frame;
    const desk_protocol_status_t decode_status =
        desk_protocol_decode(event->frame, event->frame_length, &frame);
    if (decode_status != DESK_PROTOCOL_OK) {
        ESP_LOGW(TAG, "Invalid frame reached supervisor: %d", decode_status);
        return;
    }

    if (desk_sequence_window_accept(&runtime.rx_sequences, frame.sequence) != DESK_SEQUENCE_ACCEPTED) {
        ESP_LOGW(TAG, "Ignored repeated or expired BLE sequence %u", frame.sequence);
        return;
    }

    const uint32_t now_ms = uptime_ms();
    switch (frame.message_type) {
        case DESK_MESSAGE_HEARTBEAT:
            apply_privacy_actions(
                desk_privacy_dispatch(&runtime.privacy, DESK_PRIVACY_EVENT_HEARTBEAT, now_ms)
            );
            break;

        case DESK_MESSAGE_LOCK:
            apply_privacy_actions(
                desk_privacy_dispatch(&runtime.privacy, DESK_PRIVACY_EVENT_MAC_LOCKED, now_ms)
            );
            break;

        case DESK_MESSAGE_HELLO: {
            uint8_t challenge[DESK_AUTH_ENROLLMENT_CHALLENGE_SIZE];
            size_t challenge_length = 0;
            const desk_auth_result_code_t result = desk_app_auth_begin(
                frame.payload,
                frame.payload_length,
                event->encrypted,
#if CONFIG_DESK_BRINGUP_DISPLAY_ON
                true,
#else
                false,
#endif
                challenge,
                sizeof(challenge),
                &challenge_length
            );
            if (result == DESK_AUTH_RESULT_SUCCEEDED) {
                ESP_LOGI(TAG, "Application authentication challenge created");
                if (!send_protocol_message(
                        DESK_MESSAGE_AUTH_CHALLENGE,
                        DESK_FRAME_FLAG_RESPONSE,
                        challenge,
                        challenge_length
                    )) {
                    ESP_LOGW(TAG, "Unable to send application authentication challenge");
                    apply_privacy_actions(
                        desk_privacy_dispatch(&runtime.privacy, DESK_PRIVACY_EVENT_AUTH_FAILED, now_ms)
                    );
                }
            } else {
                ESP_LOGW(TAG, "Application authentication request rejected: %d", result);
                send_auth_result(result, false);
                apply_privacy_actions(
                    desk_privacy_dispatch(&runtime.privacy, DESK_PRIVACY_EVENT_AUTH_FAILED, now_ms)
                );
            }
            break;
        }

        case DESK_MESSAGE_AUTH_RESPONSE: {
            bool enrolled = false;
            const desk_auth_result_code_t result = desk_app_auth_verify(
                frame.payload,
                frame.payload_length,
                &enrolled
            );
            if (!send_auth_result(result, enrolled)) {
                ESP_LOGW(TAG, "Unable to send application authentication result");
            }
            if (result == DESK_AUTH_RESULT_SUCCEEDED) {
                ESP_LOGI(TAG, "Mac application authentication succeeded; enrolled=%d", enrolled);
                runtime.auth_started_ms = 0;
                runtime.app_state.connection.ble_connected = true;
                runtime.app_state.connection.mac_authenticated = true;
                runtime.app_state.revision++;
                apply_ui_state(false);
                apply_privacy_actions(
                    desk_privacy_dispatch(&runtime.privacy, DESK_PRIVACY_EVENT_AUTH_SUCCEEDED, now_ms)
                );
            } else {
                ESP_LOGW(TAG, "Mac application authentication failed: %d", result);
                apply_privacy_actions(
                    desk_privacy_dispatch(&runtime.privacy, DESK_PRIVACY_EVENT_AUTH_FAILED, now_ms)
                );
            }
            break;
        }

        case DESK_MESSAGE_SYSTEM_STATE:
            if (runtime.privacy.state != DESK_PRIVACY_ACTIVE) {
                ESP_LOGW(TAG, "Rejected system state before application authentication");
            } else if (!apply_system_state_payload(frame.payload, frame.payload_length)) {
                ESP_LOGW(TAG, "Rejected malformed system state payload");
            }
            break;

        case DESK_MESSAGE_CONTROL_LAYOUT:
            if (runtime.privacy.state != DESK_PRIVACY_ACTIVE) {
                ESP_LOGW(TAG, "Rejected control layout before application authentication");
            } else if (!apply_control_layout_payload(frame.payload, frame.payload_length)) {
                ESP_LOGW(TAG, "Rejected malformed control layout payload");
            }
            break;

        case DESK_MESSAGE_AI_STATE:
            if (runtime.privacy.state != DESK_PRIVACY_ACTIVE) {
                ESP_LOGW(TAG, "Rejected AI state before application authentication");
            } else if (!apply_ai_state_payload(frame.payload, frame.payload_length)) {
                ESP_LOGW(TAG, "Rejected malformed AI state payload");
            }
            break;

        case DESK_MESSAGE_MEDIA_STATE:
            if (runtime.privacy.state != DESK_PRIVACY_ACTIVE) {
                ESP_LOGW(TAG, "Rejected media state before application authentication");
            } else if (!apply_media_state_payload(frame.payload, frame.payload_length)) {
                ESP_LOGW(TAG, "Rejected malformed media state payload");
            }
            break;

        case DESK_MESSAGE_WIFI_PROVISION: {
            if (runtime.privacy.state != DESK_PRIVACY_ACTIVE) {
                ESP_LOGW(TAG, "Rejected Wi-Fi provisioning before application authentication");
                break;
            }
            esp_err_t provision_result = ESP_ERR_INVALID_ARG;
            if (!apply_wifi_provision_payload(frame.payload, frame.payload_length, &provision_result)) {
                ESP_LOGW(TAG, "Rejected malformed Wi-Fi provisioning payload");
                send_wifi_result(ESP_ERR_INVALID_ARG);
            } else {
                send_wifi_result(provision_result);
            }
            break;
        }

        case DESK_MESSAGE_WEATHER_CONFIG: {
            if (runtime.privacy.state != DESK_PRIVACY_ACTIVE) {
                ESP_LOGW(TAG, "Rejected weather configuration before application authentication");
                break;
            }
            esp_err_t config_result = ESP_ERR_INVALID_ARG;
            if (!apply_weather_config_payload(frame.payload, frame.payload_length, &config_result)) {
                ESP_LOGW(TAG, "Rejected malformed weather configuration payload");
                send_weather_config_result(ESP_ERR_INVALID_ARG);
            } else {
                send_weather_config_result(config_result);
            }
            break;
        }

        default:
            if (runtime.privacy.state != DESK_PRIVACY_ACTIVE) {
                ESP_LOGW(TAG, "Rejected message 0x%04x before application authentication", frame.message_type);
            } else {
                ESP_LOGI(TAG, "Authenticated message 0x%04x is awaiting its feature handler", frame.message_type);
            }
            break;
    }
}

static void handle_ble_event(const desk_ble_event_t *event)
{
    const uint32_t now_ms = uptime_ms();
    switch (event->type) {
        case DESK_BLE_EVENT_READY:
            ESP_LOGI(TAG, "BLE transport is advertising");
            break;

        case DESK_BLE_EVENT_CONNECTED:
            memset(&runtime.rx_sequences, 0, sizeof(runtime.rx_sequences));
            desk_app_auth_reset_session();
            runtime.auth_started_ms = now_ms;
            runtime.tx_sequence = 0;
            runtime.app_state.connection.ble_connected = true;
            runtime.app_state.connection.mac_authenticated = false;
            runtime.app_state.revision++;
            apply_ui_state(false);
            apply_privacy_actions(
                desk_privacy_dispatch(&runtime.privacy, DESK_PRIVACY_EVENT_BLE_CONNECTED, now_ms)
            );
            break;

        case DESK_BLE_EVENT_DISCONNECTED:
            desk_app_auth_reset_session();
            runtime.auth_started_ms = 0;
            apply_privacy_actions(
                desk_privacy_dispatch(&runtime.privacy, DESK_PRIVACY_EVENT_BLE_DISCONNECTED, now_ms)
            );
            break;

        case DESK_BLE_EVENT_SECURITY_CHANGED:
            if (event->status != 0 || !event->encrypted) {
                ESP_LOGW(TAG, "BLE link security failed; status=%d", event->status);
                apply_privacy_actions(
                    desk_privacy_dispatch(&runtime.privacy, DESK_PRIVACY_EVENT_AUTH_FAILED, now_ms)
                );
            } else {
                ESP_LOGI(TAG, "Encrypted BLE link established; waiting for application authentication");
            }
            break;

        case DESK_BLE_EVENT_FRAME_RECEIVED:
            handle_protocol_frame(event);
            break;

        case DESK_BLE_EVENT_MTU_CHANGED:
            ESP_LOGI(TAG, "BLE transport MTU is %u", event->mtu);
            break;

        case DESK_BLE_EVENT_SUBSCRIPTION_CHANGED:
            ESP_LOGI(TAG, "BLE event subscription is %s", event->subscribed ? "enabled" : "disabled");
            break;

        case DESK_BLE_EVENT_TRANSPORT_ERROR:
            ESP_LOGW(TAG, "BLE transport error: %d", event->status);
            break;

        default:
            break;
    }
}

static void handle_network_event(const desk_network_event_t *event)
{
    switch (event->type) {
        case DESK_NETWORK_EVENT_CONNECTED:
            runtime.app_state.connection.wifi_connected = true;
            runtime.app_state.connection.wifi_rssi_dbm = event->rssi_dbm;
            snprintf(
                runtime.app_state.connection.wifi_ipv4,
                sizeof(runtime.app_state.connection.wifi_ipv4),
                "%s",
                event->ipv4
            );
            ESP_LOGI(TAG, "Wi-Fi connected; RSSI=%d dBm", event->rssi_dbm);
            desk_weather_set_network_available(true);
            desk_market_set_network_available(true);
            break;
        case DESK_NETWORK_EVENT_DISCONNECTED:
            runtime.app_state.connection.wifi_connected = false;
            runtime.app_state.connection.wifi_rssi_dbm = 0;
            runtime.app_state.connection.wifi_ipv4[0] = '\0';
            ESP_LOGW(TAG, "Wi-Fi disconnected; reason=%u", event->disconnect_reason);
            desk_weather_set_network_available(false);
            desk_market_set_network_available(false);
            break;
        case DESK_NETWORK_EVENT_RETRY_EXHAUSTED:
            runtime.app_state.connection.wifi_connected = false;
            runtime.app_state.connection.wifi_rssi_dbm = 0;
            runtime.app_state.connection.wifi_ipv4[0] = '\0';
            ESP_LOGW(TAG, "Wi-Fi reconnect attempts exhausted");
            desk_weather_set_network_available(false);
            desk_market_set_network_available(false);
            break;
        case DESK_NETWORK_EVENT_STARTED:
        default:
            return;
    }
    runtime.app_state.revision++;
    apply_ui_state(false);
}

static void update_device_diagnostics(uint32_t now_ms)
{
    desk_device_state_t *device = &runtime.app_state.device;
    const desk_storage_status_t storage = desk_storage_get_status();
    device->uptime_seconds = now_ms / 1000U;
    device->free_internal_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U);
    device->largest_internal_block_kb =
        (uint32_t)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024U);
    device->free_psram_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U);
    device->storage_mounted = storage.mounted;
    device->storage_last_error = storage.last_error;
    device->log_written_entries = storage.written_entries;
    device->log_dropped_entries = storage.dropped_entries;
    runtime.app_state.revision++;
    apply_ui_state(false);
}

static void handle_weather_event(const desk_weather_event_t *event)
{
    if (event->type == DESK_WEATHER_EVENT_UPDATED) {
        runtime.app_state.weather = event->state;
        runtime.app_state.revision++;
        apply_ui_state(false);
        ESP_LOGI(TAG, "Weather state updated");
    } else {
        ESP_LOGW(TAG, "Weather refresh failed: %s", esp_err_to_name(event->status));
    }
}

static void handle_market_event(const desk_market_event_t *event)
{
    if (event->type == DESK_MARKET_EVENT_UPDATED) {
        runtime.app_state.market = event->state;
        runtime.app_state.revision++;
        apply_ui_state(false);
        ESP_LOGI(TAG, "A-share index state updated");
    } else {
        ESP_LOGW(TAG, "A-share index refresh failed: %s", esp_err_to_name(event->status));
    }
}

static void supervisor_task(void *argument)
{
    (void)argument;
    for (;;) {
        desk_ble_event_t event;
        if (desk_ble_link_receive_event(&event, SUPERVISOR_POLL_MS)) {
            handle_ble_event(&event);
        }

        uint16_t action_id = 0;
        while (runtime.ui_action_queue != NULL &&
               xQueueReceive(runtime.ui_action_queue, &action_id, 0) == pdTRUE) {
            send_ui_action(action_id);
        }

        desk_network_event_t network_event;
        while (desk_network_receive_event(&network_event, 0)) {
            handle_network_event(&network_event);
        }

        desk_weather_event_t weather_event;
        while (desk_weather_receive_event(&weather_event, 0)) {
            handle_weather_event(&weather_event);
        }

        desk_market_event_t market_event;
        while (desk_market_receive_event(&market_event, 0)) {
            handle_market_event(&market_event);
        }

        const uint32_t now_ms = uptime_ms();
        if ((int32_t)(now_ms - runtime.next_diagnostics_update_ms) >= 0) {
            runtime.next_diagnostics_update_ms = now_ms + 5000U;
            update_device_diagnostics(now_ms);
        }
        apply_privacy_actions(desk_privacy_poll(&runtime.privacy, now_ms));
        if (runtime.privacy.state == DESK_PRIVACY_AUTHENTICATING && runtime.auth_started_ms != 0 &&
            (uint32_t)(now_ms - runtime.auth_started_ms) > APPLICATION_AUTH_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Application authentication timed out");
            runtime.auth_started_ms = 0;
            apply_privacy_actions(
                desk_privacy_dispatch(&runtime.privacy, DESK_PRIVACY_EVENT_AUTH_FAILED, now_ms)
            );
        }
    }
}

static void font_loader_task(void *argument)
{
    (void)argument;
    /* CJK 后备字库已移出固件镜像，改由 SD 提供。加载 ~2MB LVGL 二进制字体较慢
     * （逐字节解析，约几十秒），故放后台低优先级任务、锁外加载，避免阻塞联网与
     * UI；加载完成后仅在设置 fallback 的一瞬间加 LVGL 锁。 */
    const char *path = "S:" DESK_STORAGE_ROOT_DIR "/assets/desk_ui_cjk_font_16.bin";
    bool mounted = false;
    for (int i = 0; i < 50; ++i) {  /* 最多等 5 秒 SD 挂载 */
        if (desk_storage_get_status().mounted) {
            mounted = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!mounted) {
        ESP_LOGW(TAG, "SD not mounted; CJK fallback unavailable (fixed UI text still renders)");
        vTaskDelete(NULL);
        return;
    }
    /* 锁外加载：lv_binfont_create 只用堆（CLIB malloc 线程安全）与文件系统，
     * 构建中的字体对象在接入前是孤立的，不触碰共享 LVGL 状态。 */
    lv_font_t *cjk = lv_binfont_create(path);
    if (cjk != NULL && esp_lv_adapter_lock(-1) == ESP_OK) {
        desk_ui_set_cjk_fallback(cjk);
        esp_lv_adapter_unlock();
    }
    ESP_LOGI(TAG, "CJK fallback font %s (%s)", cjk != NULL ? "loaded" : "load FAILED", path);
    vTaskDelete(NULL);
}

void app_main(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    const desk_board_profile_t *profile = desk_board_get_profile();
    ESP_LOGI(TAG, "Board profile: %s", profile->display_name);

    /* 开机不加载 mock：公共数据（天气/行情）初始为 invalid，UI 显示“等待联网更新/
     * ----”，真实数据到达后替换，避免先闪一屏假数据。私有数据本就随认证前清空。 */
    desk_app_state_init(&runtime.app_state);
    const esp_app_desc_t *application = esp_app_get_description();
    snprintf(
        runtime.app_state.device.firmware_version,
        sizeof(runtime.app_state.device.firmware_version),
        "%.19s",
        application != NULL ? application->version : "unknown"
    );
    snprintf(
        runtime.app_state.device.board_name,
        sizeof(runtime.app_state.device.board_name),
        "%s",
        profile->display_name
    );
    runtime.ui_action_queue = xQueueCreate(8, sizeof(uint16_t));
    ESP_ERROR_CHECK(runtime.ui_action_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    desk_ui_set_action_callback(queue_ui_action, NULL);
    desk_privacy_init(&runtime.privacy, HEARTBEAT_TIMEOUT_MS);
    ESP_LOGI(TAG, "Privacy state initialized: %d", runtime.privacy.state);

    const esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
    const esp_lv_adapter_tear_avoid_mode_t tear_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;
    const uint8_t frame_buffer_count = esp_lv_adapter_get_required_frame_buffer_count(tear_mode, rotation);

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_touch_handle_t touch_handle = NULL;
    ESP_ERROR_CHECK(desk_board_display_init(frame_buffer_count, &panel_handle, &touch_handle));

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_stack_size = 12 * 1024;
    adapter_config.stack_in_psram = true;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_config));

    esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
        panel_handle,
        NULL,
        DESK_LCD_H_RES,
        DESK_LCD_V_RES,
        rotation
    );
    display_config.profile.use_psram = true;

    lv_display_t *display = esp_lv_adapter_register_display(&display_config);
    ESP_ERROR_CHECK(display != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    if (touch_handle != NULL) {
        esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(display, touch_handle);
        runtime.touch_input = esp_lv_adapter_register_touch(&touch_config);
        ESP_ERROR_CHECK(runtime.touch_input != NULL ? ESP_OK : ESP_ERR_NO_MEM);
        lv_indev_set_gesture_min_distance(runtime.touch_input, 80);
        lv_indev_set_gesture_min_velocity(runtime.touch_input, 2);
    }

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
    desk_ui_init(&runtime.app_state);
    esp_lv_adapter_unlock();

#if CONFIG_DESK_BRINGUP_DISPLAY_ON
    ESP_LOGW(TAG, "Bring-up mode is active: backlight stays on without BLE authentication");
    ESP_ERROR_CHECK(desk_board_backlight_set(true));
#else
    ESP_ERROR_CHECK(desk_board_backlight_set(false));
#endif

    ESP_ERROR_CHECK(desk_storage_start());
    desk_storage_log(
        DESK_LOG_INFO,
        DESK_LOG_CONTENT_PUBLIC,
        TAG,
        "Firmware started with board profile %s",
        profile->display_name
    );

    xTaskCreate(font_loader_task, "font_load", 6144, NULL, 1, NULL);

    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* 首刷/改分区表/OTA 版本变化时 NVS 需重建，否则 ESP_ERROR_CHECK 会 abort 成重启循环。 */
        ESP_LOGW(TAG, "NVS needs reformat (%s); erasing", esp_err_to_name(nvs_result));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);
    ESP_ERROR_CHECK(desk_app_auth_init());
    ESP_ERROR_CHECK(desk_http_fetch_init());

    const esp_err_t network_result = desk_network_start();
    if (network_result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi startup failed; public network data is unavailable: %s", esp_err_to_name(network_result));
    }
    const esp_err_t weather_result = desk_weather_start();
    if (weather_result != ESP_OK) {
        ESP_LOGE(TAG, "Weather worker startup failed: %s", esp_err_to_name(weather_result));
    }
    const esp_err_t market_result = desk_market_start();
    if (market_result != ESP_OK) {
        ESP_LOGE(TAG, "A-share index worker startup failed: %s", esp_err_to_name(market_result));
    }

    const esp_err_t ble_result = desk_ble_link_start();
    if (ble_result != ESP_OK) {
        ESP_LOGE(TAG, "BLE startup failed; display remains available: %s", esp_err_to_name(ble_result));
    }

    const BaseType_t task_created = xTaskCreate(
        supervisor_task,
        "desk_supervisor",
        SUPERVISOR_STACK_SIZE,
        NULL,
        6,
        NULL
    );
    ESP_ERROR_CHECK(task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
