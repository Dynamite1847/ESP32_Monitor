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

static void show_ui_feedback(const char *message, bool succeeded)
{
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    desk_ui_show_feedback(message, succeeded);
    esp_lv_adapter_unlock();
}

static void apply_privacy_actions(desk_privacy_actions_t actions)
{
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

static bool send_ui_action(uint16_t action_id)
{
    if (runtime.privacy.state != DESK_PRIVACY_ACTIVE) {
        return false;
    }
    char payload[48];
    const int payload_length = snprintf(
        payload,
        sizeof(payload),
        "{\"actionId\":%u,\"gesture\":\"tap\"}",
        action_id
    );
    const bool sent = payload_length > 0 && (size_t)payload_length < sizeof(payload) &&
        send_protocol_message(
            DESK_MESSAGE_ACTION_TRIGGER,
            DESK_FRAME_FLAG_NONE,
            (const uint8_t *)payload,
            (size_t)payload_length
        );
    if (!sent) {
        ESP_LOGW(TAG, "Unable to send UI action %u", action_id);
    }
    return sent;
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
        uint32_t primary_reset = 0;
        uint32_t secondary_reset = 0;
        uint32_t tasks = 0;
        uint32_t slots = 0;
        uint32_t elapsed = 0;
        desk_ai_task_status_t task_status = DESK_AI_IDLE;
        if (!json_read_u32(provider, "primary", 100, &primary) ||
            !json_read_u32(provider, "secondary", 100, &secondary) ||
            !json_read_u32(provider, "primaryWindow", UINT16_MAX, &primary_window) ||
            !json_read_u32(provider, "secondaryWindow", UINT16_MAX, &secondary_window) ||
            !json_read_u32(provider, "primaryReset", UINT32_MAX, &primary_reset) ||
            !json_read_u32(provider, "secondaryReset", UINT32_MAX, &secondary_reset) ||
            !json_read_u32(provider, "tasks", 32, &tasks) ||
            !json_read_u32(provider, "slots", DESK_AI_TASK_SLOT_COUNT, &slots) ||
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
        destination->primary_reset_seconds = primary_reset;
        destination->secondary_reset_seconds = secondary_reset;
        destination->active_task_count = (uint8_t)tasks;
        destination->available_task_slots = (uint8_t)slots;
        destination->task_status = task_status;
        destination->elapsed_seconds = elapsed;
        seen[index] = true;
    }
    cJSON_Delete(root);
    if (!seen[0] || !seen[1]) {
        return false;
    }

    /* ai_state 整体覆盖前，保住任务槽位（它们由独立的 tasks 消息维护）。
     * codex_tasks 与 claude_tasks 都要保留，否则每 2 秒的 ai_state 会清空槽位、
     * 与 tasks 消息交替导致 AI 页闪烁。available_task_slots 同理不被 ai_state 覆盖。 */
    memcpy(state.codex_tasks, runtime.app_state.ai.codex_tasks, sizeof(state.codex_tasks));
    memcpy(state.claude_tasks, runtime.app_state.ai.claude_tasks, sizeof(state.claude_tasks));
    state.providers[0].available_task_slots = runtime.app_state.ai.providers[0].available_task_slots;
    state.providers[1].available_task_slots = runtime.app_state.ai.providers[1].available_task_slots;
    runtime.app_state.ai = state;
    runtime.app_state.revision++;
    apply_ui_state(false);
    return true;
}

static bool apply_ai_tasks_payload(const uint8_t *payload, size_t payload_length)
{
    cJSON *root = parse_json_object(payload, payload_length);
    const cJSON *tasks = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, "t") : NULL;
    const int task_count = cJSON_IsArray(tasks) ? cJSON_GetArraySize(tasks) : -1;
    if (task_count < 0 || task_count > DESK_AI_TASK_SLOT_COUNT) {
        cJSON_Delete(root);
        return false;
    }

    desk_ai_task_slot_t parsed[DESK_AI_TASK_SLOT_COUNT] = {0};
    const cJSON *task = NULL;
    int index = 0;
    cJSON_ArrayForEach(task, tasks) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(task, "n");
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(task, "s");
        desk_ai_task_status_t parsed_status = DESK_AI_IDLE;
        if (!cJSON_IsString(name) || name->valuestring == NULL || name->valuestring[0] == '\0' ||
            strlen(name->valuestring) >= sizeof(parsed[index].name) ||
            !cJSON_IsString(status) || status->valuestring == NULL ||
            !parse_ai_status(status->valuestring, &parsed_status)) {
            cJSON_Delete(root);
            return false;
        }
        parsed[index].assigned = true;
        parsed[index].status = parsed_status;
        snprintf(parsed[index].name, sizeof(parsed[index].name), "%s", name->valuestring);
        index++;
    }
    cJSON_Delete(root);

    memcpy(runtime.app_state.ai.codex_tasks, parsed, sizeof(parsed));
    runtime.app_state.ai.providers[0].available_task_slots = (uint8_t)task_count;
    runtime.app_state.revision++;
    apply_ui_state(false);
    return true;
}

/* Claude 会话槽位：与 Codex 任务同格式 {"t":[{"n","s"}]}，写入独立的 claude_tasks，
 * 不触碰 Codex 路径。名称为项目名，状态为运行中/空闲（隐私红线：不含提示词/内容）。 */
static bool apply_claude_tasks_payload(const uint8_t *payload, size_t payload_length)
{
    cJSON *root = parse_json_object(payload, payload_length);
    const cJSON *tasks = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, "t") : NULL;
    const int task_count = cJSON_IsArray(tasks) ? cJSON_GetArraySize(tasks) : -1;
    if (task_count < 0 || task_count > DESK_AI_TASK_SLOT_COUNT) {
        cJSON_Delete(root);
        return false;
    }

    desk_ai_task_slot_t parsed[DESK_AI_TASK_SLOT_COUNT] = {0};
    const cJSON *task = NULL;
    int index = 0;
    cJSON_ArrayForEach(task, tasks) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(task, "n");
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(task, "s");
        const cJSON *detail = cJSON_GetObjectItemCaseSensitive(task, "d");
        desk_ai_task_status_t parsed_status = DESK_AI_IDLE;
        if (!cJSON_IsString(name) || name->valuestring == NULL || name->valuestring[0] == '\0' ||
            strlen(name->valuestring) >= sizeof(parsed[index].name) ||
            !cJSON_IsString(status) || status->valuestring == NULL ||
            !parse_ai_status(status->valuestring, &parsed_status)) {
            cJSON_Delete(root);
            return false;
        }
        parsed[index].assigned = true;
        parsed[index].status = parsed_status;
        snprintf(parsed[index].name, sizeof(parsed[index].name), "%s", name->valuestring);
        if (cJSON_IsString(detail) && detail->valuestring != NULL &&
            strlen(detail->valuestring) < sizeof(parsed[index].detail)) {
            snprintf(parsed[index].detail, sizeof(parsed[index].detail), "%s", detail->valuestring);
        }
        index++;
    }
    cJSON_Delete(root);

    /* 数据未变则不刷新：助手每 2 秒推送一次，无谓重绘会让 Claude 页闪烁。 */
    if (memcmp(runtime.app_state.ai.claude_tasks, parsed, sizeof(parsed)) == 0 &&
        runtime.app_state.ai.providers[1].available_task_slots == (uint8_t)task_count) {
        return true;
    }
    memcpy(runtime.app_state.ai.claude_tasks, parsed, sizeof(parsed));
    runtime.app_state.ai.providers[1].available_task_slots = (uint8_t)task_count;
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
    const cJSON *metadata_available = cJSON_GetObjectItemCaseSensitive(root, "metadataAvailable");
    const cJSON *playing = cJSON_GetObjectItemCaseSensitive(root, "playing");
    const cJSON *title_hidden = cJSON_GetObjectItemCaseSensitive(root, "titleHidden");
    const cJSON *muted = cJSON_GetObjectItemCaseSensitive(root, "muted");
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "title");
    const cJSON *artist = cJSON_GetObjectItemCaseSensitive(root, "artist");
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(root, "source");
    uint32_t volume = 0;
    uint32_t position = 0;
    uint32_t duration = 0;
    const bool valid =
        cJSON_IsBool(valid_item) && cJSON_IsTrue(valid_item) &&
        cJSON_IsBool(metadata_available) && cJSON_IsBool(playing) &&
        cJSON_IsBool(title_hidden) && cJSON_IsBool(muted) &&
        cJSON_IsString(title) && title->valuestring != NULL && title->valuestring[0] != '\0' &&
        cJSON_IsString(artist) && artist->valuestring != NULL &&
        cJSON_IsString(source) && source->valuestring != NULL && source->valuestring[0] != '\0' &&
        strlen(title->valuestring) < sizeof(runtime.app_state.media.title) &&
        strlen(artist->valuestring) < sizeof(runtime.app_state.media.artist) &&
        strlen(source->valuestring) < sizeof(runtime.app_state.media.source) &&
        json_read_u32(root, "volume", 100, &volume) &&
        json_read_u32(root, "position", UINT32_MAX, &position) &&
        json_read_u32(root, "duration", UINT32_MAX, &duration) &&
        (duration == 0 || position <= duration);
    if (valid) {
        runtime.app_state.media = (desk_media_state_t){
            .valid = true,
            .metadata_available = cJSON_IsTrue(metadata_available),
            .playing = cJSON_IsTrue(playing),
            .title_hidden = cJSON_IsTrue(title_hidden),
            .muted = cJSON_IsTrue(muted),
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
        snprintf(
            runtime.app_state.media.artist,
            sizeof(runtime.app_state.media.artist),
            "%s",
            artist->valuestring
        );
        snprintf(
            runtime.app_state.media.source,
            sizeof(runtime.app_state.media.source),
            "%s",
            source->valuestring
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
#if CONFIG_DESK_ALLOW_APP_ENROLLMENT
                true,
#else
                event->bonded && desk_app_auth_migration_enrollment_pending(),
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

        case DESK_MESSAGE_AI_CLAUDE_TASKS:
            if (runtime.privacy.state != DESK_PRIVACY_ACTIVE) {
                ESP_LOGW(TAG, "Rejected Claude tasks before application authentication");
            } else if (!apply_claude_tasks_payload(frame.payload, frame.payload_length)) {
                ESP_LOGW(TAG, "Rejected malformed Claude tasks payload");
            }
            break;

        case DESK_MESSAGE_AI_TASKS:
            if (runtime.privacy.state != DESK_PRIVACY_ACTIVE) {
                ESP_LOGW(TAG, "Rejected AI tasks before application authentication");
            } else if (!apply_ai_tasks_payload(frame.payload, frame.payload_length)) {
                ESP_LOGW(TAG, "Rejected malformed AI tasks payload");
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
            runtime.app_state.device.ble_bonded = event->bonded;
            runtime.app_state.revision++;
            apply_ui_state(false);
            apply_privacy_actions(
                desk_privacy_dispatch(&runtime.privacy, DESK_PRIVACY_EVENT_BLE_CONNECTED, now_ms)
            );
            break;

        case DESK_BLE_EVENT_DISCONNECTED:
            desk_app_auth_reset_session();
            runtime.auth_started_ms = 0;
            runtime.app_state.device.ble_bonded = false;
            runtime.app_state.device.ble_mtu = 0;
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
                runtime.app_state.device.ble_bonded = event->bonded;
                ESP_LOGI(TAG, "Encrypted BLE link established; waiting for application authentication");
            }
            break;

        case DESK_BLE_EVENT_FRAME_RECEIVED:
            handle_protocol_frame(event);
            break;

        case DESK_BLE_EVENT_MTU_CHANGED:
            runtime.app_state.device.ble_mtu = event->mtu;
            ESP_LOGI(TAG, "BLE transport MTU is %u", event->mtu);
            break;

        case DESK_BLE_EVENT_SUBSCRIPTION_CHANGED:
            ESP_LOGI(
                TAG,
                "BLE subscriptions private=%s codex=%s",
                event->subscribed ? "enabled" : "disabled",
                event->codex_subscribed ? "enabled" : "disabled"
            );
            break;

        case DESK_BLE_EVENT_CODEX_TASK_STATUS:
            if (runtime.privacy.state == DESK_PRIVACY_ACTIVE &&
                event->codex_slot < DESK_AI_TASK_SLOT_COUNT &&
                runtime.app_state.ai.codex_tasks[event->codex_slot].assigned) {
                static const desk_ai_task_status_t mapped[] = {
                    DESK_AI_IDLE,
                    DESK_AI_IDLE,
                    DESK_AI_RUNNING,
                    DESK_AI_COMPLETED,
                    DESK_AI_WAITING_INPUT,
                    DESK_AI_FAILED,
                };
                if ((size_t)event->codex_status < sizeof(mapped) / sizeof(mapped[0])) {
                    runtime.app_state.ai.codex_tasks[event->codex_slot].status = mapped[event->codex_status];
                    runtime.app_state.revision++;
                    apply_ui_state(false);
                }
            }
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
    const desk_network_status_t network = desk_network_get_status();
    device->uptime_seconds = now_ms / 1000U;
    device->free_internal_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U);
    device->largest_internal_block_kb =
        (uint32_t)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024U);
    device->free_psram_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U);
    device->storage_mounted = storage.mounted;
    device->storage_last_error = storage.last_error;
    device->log_written_entries = storage.written_entries;
    device->log_dropped_entries = storage.dropped_entries;
    device->log_queued_entries = storage.queued_entries;
    device->wifi_reconnect_attempts = network.reconnect_attempts;
    device->wifi_last_disconnect_reason = network.last_disconnect_reason;
    device->wifi_credentials_available = network.credentials_available;
    snprintf(device->wifi_ssid, sizeof(device->wifi_ssid), "%s", network.ssid);
    device->heartbeat_age_ms = runtime.privacy.state == DESK_PRIVACY_ACTIVE &&
                                       runtime.privacy.last_heartbeat_ms != 0U
                                   ? now_ms - runtime.privacy.last_heartbeat_ms
                                   : UINT32_MAX;
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

static bool send_codex_key_tap(const char *key, int8_t agent)
{
    if (desk_ble_link_codex_send_key(key, 1, agent) != ESP_OK) {
        return false;
    }
    return desk_ble_link_codex_send_key(key, 0, agent) == ESP_OK;
}

static bool send_codex_direction(float angle)
{
    if (desk_ble_link_codex_send_joystick(angle, 1.0f) != ESP_OK) {
        return false;
    }
    return desk_ble_link_codex_send_joystick(angle, 0.0f) == ESP_OK;
}

static void handle_ui_action(uint16_t action_id, uint32_t now_ms)
{
    if (runtime.privacy.state != DESK_PRIVACY_ACTIVE) {
        return;
    }

    switch ((desk_ui_action_id_t)action_id) {
        case DESK_UI_ACTION_WIFI_RECONNECT: {
            const esp_err_t result = desk_network_reconnect();
            desk_storage_log(
                result == ESP_OK ? DESK_LOG_INFO : DESK_LOG_WARNING,
                DESK_LOG_CONTENT_PUBLIC,
                TAG,
                "Manual Wi-Fi reconnect result=%s",
                esp_err_to_name(result)
            );
            update_device_diagnostics(now_ms);
            show_ui_feedback(
                result == ESP_OK ? "正在重新连接 Wi-Fi" :
                (result == ESP_ERR_NOT_FOUND ? "请先在 Mac 助手中配置 Wi-Fi" : "Wi-Fi 重连失败"),
                result == ESP_OK
            );
            break;
        }
        case DESK_UI_ACTION_PUBLIC_REFRESH:
            if (runtime.app_state.connection.wifi_connected) {
                desk_weather_request_refresh();
                desk_market_request_refresh();
                desk_storage_log(
                    DESK_LOG_INFO,
                    DESK_LOG_CONTENT_PUBLIC,
                    TAG,
                    "Manual public-data refresh requested"
                );
                show_ui_feedback("已请求刷新天气和行情", true);
            } else {
                show_ui_feedback("Wi-Fi 离线，暂时无法刷新", false);
            }
            break;
        case DESK_UI_ACTION_DIAGNOSTIC_REFRESH:
            update_device_diagnostics(now_ms);
            show_ui_feedback("设备状态已刷新", true);
            break;
        case DESK_UI_ACTION_DIAGNOSTIC_SNAPSHOT: {
            const desk_device_state_t *device = &runtime.app_state.device;
            const bool recorded = desk_storage_log(
                    DESK_LOG_INFO,
                    DESK_LOG_CONTENT_PUBLIC,
                    "diagnostic",
                    "snapshot firmware=%s uptime=%lu internal_kb=%lu largest_kb=%lu psram_kb=%lu sd=%u "
                    "wifi=%u ble=%u authenticated=%u retries=%lu logs_written=%lu logs_dropped=%lu",
                    device->firmware_version,
                    (unsigned long)device->uptime_seconds,
                    (unsigned long)device->free_internal_kb,
                    (unsigned long)device->largest_internal_block_kb,
                    (unsigned long)device->free_psram_kb,
                    device->storage_mounted ? 1U : 0U,
                    runtime.app_state.connection.wifi_connected ? 1U : 0U,
                    runtime.app_state.connection.ble_connected ? 1U : 0U,
                    runtime.app_state.connection.mac_authenticated ? 1U : 0U,
                    (unsigned long)device->wifi_reconnect_attempts,
                    (unsigned long)device->log_written_entries,
                    (unsigned long)device->log_dropped_entries
                );
            if (recorded) {
                runtime.app_state.device.diagnostic_snapshots++;
            }
            update_device_diagnostics(now_ms);
            show_ui_feedback(
                recorded ? "诊断快照已写入存储卡" : "诊断快照写入失败",
                recorded
            );
            break;
        }
        case DESK_UI_ACTION_CODEX_TASK_1:
        case DESK_UI_ACTION_CODEX_TASK_2:
        case DESK_UI_ACTION_CODEX_TASK_3:
        case DESK_UI_ACTION_CODEX_TASK_4:
        case DESK_UI_ACTION_CODEX_TASK_5:
        case DESK_UI_ACTION_CODEX_TASK_6: {
            const uint8_t slot = (uint8_t)(action_id - DESK_UI_ACTION_CODEX_TASK_1);
            char key[5];
            snprintf(key, sizeof(key), "AG%02u", slot);
            const bool native_sent = send_codex_key_tap(key, (int8_t)slot);
            const bool fallback_sent = native_sent ? false : send_ui_action(action_id);
            show_ui_feedback(
                native_sent ? "已切换 Codex 任务；双击可聚焦" :
                (fallback_sent ? "正在 Mac 中打开 Codex 任务" : "Codex 控制尚未连接"),
                native_sent || fallback_sent
            );
            break;
        }
        case DESK_UI_ACTION_CODEX_FAST:
        case DESK_UI_ACTION_CODEX_APPROVE:
        case DESK_UI_ACTION_CODEX_DECLINE:
        case DESK_UI_ACTION_CODEX_CONTINUE:
        case DESK_UI_ACTION_CODEX_SEND: {
            const char *key = action_id == DESK_UI_ACTION_CODEX_FAST ? "ACT06" :
                              action_id == DESK_UI_ACTION_CODEX_APPROVE ? "ACT07" :
                              action_id == DESK_UI_ACTION_CODEX_DECLINE ? "ACT08" :
                              action_id == DESK_UI_ACTION_CODEX_CONTINUE ? "ACT09" : "ACT12";
            const bool sent = send_codex_key_tap(key, -1);
            show_ui_feedback(sent ? "Codex 命令已发送" : "请先在 Codex 中连接控制设备", sent);
            break;
        }
        case DESK_UI_ACTION_CODEX_MIC_PRESS:
        case DESK_UI_ACTION_CODEX_MIC_RELEASE: {
            const uint8_t key_action = action_id == DESK_UI_ACTION_CODEX_MIC_PRESS ? 1U : 0U;
            const bool sent = desk_ble_link_codex_send_key("ACT10", key_action, -1) == ESP_OK;
            if (action_id == DESK_UI_ACTION_CODEX_MIC_PRESS) {
                show_ui_feedback(sent ? "语音键已按下" : "请先在 Codex 中连接控制设备", sent);
            }
            break;
        }
        case DESK_UI_ACTION_CODEX_UP:
        case DESK_UI_ACTION_CODEX_RIGHT:
        case DESK_UI_ACTION_CODEX_DOWN:
        case DESK_UI_ACTION_CODEX_LEFT: {
            const float angle = action_id == DESK_UI_ACTION_CODEX_RIGHT ? 0.0f :
                                action_id == DESK_UI_ACTION_CODEX_DOWN ? 0.25f :
                                action_id == DESK_UI_ACTION_CODEX_LEFT ? 0.5f : 0.75f;
            const bool sent = send_codex_direction(angle);
            show_ui_feedback(sent ? "Codex 导航命令已发送" : "请先在 Codex 中连接控制设备", sent);
            break;
        }
        case DESK_UI_ACTION_CODEX_DIAL_CCW:
        case DESK_UI_ACTION_CODEX_DIAL_CW: {
            const char *key = action_id == DESK_UI_ACTION_CODEX_DIAL_CCW ? "ENC_CC" : "ENC_CW";
            const bool sent = desk_ble_link_codex_send_key(key, 2, -1) == ESP_OK;
            show_ui_feedback(sent ? "Codex 旋钮命令已发送" : "请先在 Codex 中连接控制设备", sent);
            break;
        }
        case DESK_UI_ACTION_CODEX_DIAL_PRESS:
        case DESK_UI_ACTION_CODEX_DIAL_RELEASE: {
            const uint8_t key_action = action_id == DESK_UI_ACTION_CODEX_DIAL_PRESS ? 1U : 0U;
            const bool sent = desk_ble_link_codex_send_key("ENC", key_action, -1) == ESP_OK;
            if (action_id == DESK_UI_ACTION_CODEX_DIAL_PRESS && !sent) {
                show_ui_feedback("请先在 Codex 中连接控制设备", false);
            }
            break;
        }
        default: {
            const bool sent = send_ui_action(action_id);
            if (action_id >= DESK_UI_ACTION_MEDIA_PREVIOUS &&
                action_id <= DESK_UI_ACTION_MEDIA_VOLUME_UP) {
                show_ui_feedback(sent ? "媒体命令已发送" : "媒体命令发送失败", sent);
            } else if (action_id == DESK_UI_ACTION_MEDIA_TITLE_TOGGLE) {
                show_ui_feedback(sent ? "正在切换媒体隐私设置" : "设置发送失败", sent);
            } else if (action_id == DESK_UI_ACTION_OPEN_MAC_HELPER) {
                show_ui_feedback(sent ? "已在 Mac 打开助手" : "无法通知 Mac 助手", sent);
            } else if (action_id >= DESK_UI_ACTION_CODEX_TASK_1 &&
                       action_id <= DESK_UI_ACTION_CODEX_TASK_6) {
                show_ui_feedback(sent ? "正在 Mac 中打开 Codex 任务" : "Codex 任务打开失败", sent);
            } else if (action_id == DESK_UI_ACTION_OPEN_CODEX) {
                show_ui_feedback(sent ? "正在 Mac 中打开 Codex" : "Codex 打开失败", sent);
            } else if (action_id == DESK_UI_ACTION_OPEN_CLAUDE) {
                show_ui_feedback(sent ? "正在 Mac 中打开 Warp" : "Warp 打开失败", sent);
            }
            break;
        }
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

        const uint32_t now_ms = uptime_ms();
        uint16_t action_id = 0;
        while (runtime.ui_action_queue != NULL &&
               xQueueReceive(runtime.ui_action_queue, &action_id, 0) == pdTRUE) {
            handle_ui_action(action_id, now_ms);
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
    bool waiting_logged = false;
    for (;;) {
        if (!desk_storage_get_status().mounted) {
            if (!waiting_logged) {
                ESP_LOGW(TAG, "Waiting for SD before loading the CJK fallback font");
                waiting_logged = true;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        waiting_logged = false;
        /* 锁外加载：lv_binfont_create 只用堆（CLIB malloc 线程安全）与文件系统，
         * 构建中的字体对象在接入前是孤立的，不触碰共享 LVGL 状态。 */
        lv_font_t *cjk = lv_binfont_create(path);
        if (cjk != NULL && esp_lv_adapter_lock(-1) == ESP_OK) {
            desk_ui_set_cjk_fallback(cjk);
            esp_lv_adapter_unlock();
            ESP_LOGI(TAG, "CJK fallback font loaded (%s)", path);
            break;
        }
        if (cjk != NULL) {
            lv_binfont_destroy(cjk);
        }
        ESP_LOGW(TAG, "CJK fallback font load failed; retrying in 30 seconds (%s)", path);
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
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

    const BaseType_t font_task_created = xTaskCreate(font_loader_task, "font_load", 6144, NULL, 1, NULL);
    if (font_task_created != pdPASS) {
        ESP_LOGW(TAG, "Unable to create the CJK fallback font loader task");
    }

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

    /* Finish GATT registration before Wi-Fi starts authenticating. The Wi-Fi
     * driver briefly suspends flash-cache access during radio setup; HID/DIS
     * registration reads service metadata from flash and must not race it. */
    const esp_err_t ble_result = desk_ble_link_start();
    if (ble_result != ESP_OK) {
        ESP_LOGE(TAG, "BLE startup failed; display remains available: %s", esp_err_to_name(ble_result));
    }

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
