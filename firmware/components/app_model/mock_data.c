#include "app_model/app_model.h"

#include <stdio.h>
#include <string.h>

static void set_market_index(
    desk_market_index_t *index,
    const char *name,
    const char *code,
    int32_t points_x100,
    int16_t change_basis_points
)
{
    snprintf(index->name, sizeof(index->name), "%s", name);
    snprintf(index->code, sizeof(index->code), "%s", code);
    index->points_x100 = points_x100;
    index->change_basis_points = change_basis_points;
}

void desk_app_state_load_mock(desk_app_state_t *state)
{
    if (state == NULL) {
        return;
    }

    desk_app_state_init(state);
    state->revision = 1;
    state->connection.wifi_connected = true;
    state->connection.wifi_rssi_dbm = -52;
    snprintf(state->connection.wifi_ipv4, sizeof(state->connection.wifi_ipv4), "%s", "192.168.1.88");

    state->weather.valid = true;
    state->weather.current_c = 26;
    state->weather.feels_like_c = 28;
    state->weather.humidity_percent = 72;
    state->weather.code = DESK_WEATHER_CLOUDY;
    state->weather.today = (desk_daily_forecast_t){
        .low_c = 23,
        .high_c = 29,
        .precipitation_percent = 60,
        .code = DESK_WEATHER_CLOUDY,
    };
    state->weather.tomorrow = (desk_daily_forecast_t){
        .low_c = 22,
        .high_c = 28,
        .precipitation_percent = 40,
        .code = DESK_WEATHER_RAIN,
    };
    const uint8_t rain[DESK_HOURLY_FORECAST_COUNT] = {20, 30, 60, 50, 20, 10};
    const int8_t temperatures[DESK_HOURLY_FORECAST_COUNT] = {28, 27, 26, 25, 24, 24};
    for (size_t i = 0; i < DESK_HOURLY_FORECAST_COUNT; ++i) {
        state->weather.hourly[i] = (desk_hourly_forecast_t){
            .hour = (uint8_t)(16 + i),
            .temperature_c = temperatures[i],
            .precipitation_percent = rain[i],
            .code = i == 2 || i == 3 ? DESK_WEATHER_RAIN : DESK_WEATHER_CLOUDY,
        };
    }

    state->market.valid = true;
    state->market.trading = true;
    set_market_index(&state->market.indices[0], "上证综指", "000001", 337452, 42);
    set_market_index(&state->market.indices[1], "深证成指", "399001", 1062831, 61);
    set_market_index(&state->market.indices[2], "创业板指", "399006", 217645, 33);
    set_market_index(&state->market.indices[3], "科创50", "000688", 102186, -18);
    set_market_index(&state->market.indices[4], "沪深300", "000300", 394825, 25);
    set_market_index(&state->market.indices[5], "中证500", "000905", 581239, -11);

    state->system = (desk_system_state_t){
        .valid = true,
        .sample_sequence = 1,
        .cpu_x10_percent = 184,
        .memory_x10_percent = 632,
        .disk_free_gb = 412,
        .network_up_kbps = 320,
        .network_down_kbps = 2450,
        .battery_percent = 86,
    };
    snprintf(state->device.firmware_version, sizeof(state->device.firmware_version), "%s", "0.1.0");
    snprintf(state->device.board_name, sizeof(state->device.board_name), "%s", "ESP32-S3 4.3");
    state->device.uptime_seconds = 3723;
    state->device.free_internal_kb = 168;
    state->device.largest_internal_block_kb = 92;
    state->device.free_psram_kb = 5820;
    snprintf(state->control.active_app, sizeof(state->control.active_app), "%s", "Safari");
    state->control.action_count = 6;

    state->ai.valid = true;
    snprintf(state->ai.providers[0].provider, sizeof(state->ai.providers[0].provider), "%s", "Codex");
    state->ai.providers[0].usage_available = true;
    state->ai.providers[0].secondary_usage_available = true;
    state->ai.providers[0].primary_usage_percent = 72;
    state->ai.providers[0].secondary_usage_percent = 28;
    state->ai.providers[0].primary_window_minutes = 300;
    state->ai.providers[0].secondary_window_minutes = 10080;
    state->ai.providers[0].active_task_count = 1;
    state->ai.providers[0].task_status = DESK_AI_RUNNING;
    state->ai.providers[0].elapsed_seconds = 18 * 60 + 42;

    snprintf(state->ai.providers[1].provider, sizeof(state->ai.providers[1].provider), "%s", "Claude Code");
    state->ai.providers[1].usage_available = true;
    state->ai.providers[1].secondary_usage_available = true;
    state->ai.providers[1].primary_usage_percent = 41;
    state->ai.providers[1].secondary_usage_percent = 63;
    state->ai.providers[1].primary_window_minutes = 300;
    state->ai.providers[1].secondary_window_minutes = 10080;
    state->ai.providers[1].task_status = DESK_AI_IDLE;

    state->media.valid = true;
    state->media.playing = true;
    state->media.volume_percent = 42;
    state->media.position_seconds = 88;
    state->media.duration_seconds = 246;
    snprintf(state->media.title, sizeof(state->media.title), "%s", "Private media title hidden in production");
}
