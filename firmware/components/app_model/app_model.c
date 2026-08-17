#include "app_model/app_model.h"

#include <string.h>

void desk_app_state_init(desk_app_state_t *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

void desk_app_state_clear_private(desk_app_state_t *state)
{
    if (state == NULL) {
        return;
    }

    memset(&state->system, 0, sizeof(state->system));
    memset(&state->control, 0, sizeof(state->control));
    memset(&state->ai, 0, sizeof(state->ai));
    memset(&state->media, 0, sizeof(state->media));
    state->connection.ble_connected = false;
    state->connection.mac_authenticated = false;
    state->revision++;
}

const char *desk_weather_text(desk_weather_code_t code)
{
    switch (code) {
        case DESK_WEATHER_CLEAR:
            return "晴";
        case DESK_WEATHER_CLOUDY:
            return "多云";
        case DESK_WEATHER_RAIN:
            return "有雨";
        case DESK_WEATHER_STORM:
            return "雷暴";
        case DESK_WEATHER_SNOW:
            return "有雪";
        case DESK_WEATHER_FOG:
            return "有雾";
        case DESK_WEATHER_UNKNOWN:
        default:
            return "未知";
    }
}

const char *desk_ai_status_text(desk_ai_task_status_t status)
{
    switch (status) {
        case DESK_AI_RUNNING:
            return "运行中";
        case DESK_AI_WAITING_PERMISSION:
            return "等待权限";
        case DESK_AI_WAITING_INPUT:
            return "等待输入";
        case DESK_AI_COMPLETED:
            return "已完成";
        case DESK_AI_FAILED:
            return "失败";
        case DESK_AI_IDLE:
        default:
            return "空闲";
    }
}
