#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DESK_HOURLY_FORECAST_COUNT 6
#define DESK_MARKET_INDEX_COUNT 6
#define DESK_AI_PROVIDER_COUNT 2
#define DESK_AI_TASK_SLOT_COUNT 6

typedef enum {
    DESK_WEATHER_UNKNOWN = 0,
    DESK_WEATHER_CLEAR,
    DESK_WEATHER_CLOUDY,
    DESK_WEATHER_RAIN,
    DESK_WEATHER_STORM,
    DESK_WEATHER_SNOW,
    DESK_WEATHER_FOG,
} desk_weather_code_t;

typedef enum {
    DESK_ALERT_NONE = 0,
    DESK_ALERT_BLUE,
    DESK_ALERT_YELLOW,
    DESK_ALERT_ORANGE,
    DESK_ALERT_RED,
} desk_alert_level_t;

typedef enum {
    DESK_AI_IDLE = 0,
    DESK_AI_RUNNING,
    DESK_AI_WAITING_PERMISSION,
    DESK_AI_WAITING_INPUT,
    DESK_AI_COMPLETED,
    DESK_AI_FAILED,
} desk_ai_task_status_t;

typedef struct {
    bool wifi_connected;
    int8_t wifi_rssi_dbm;
    char wifi_ipv4[16];
    bool ble_connected;
    bool mac_authenticated;
} desk_connection_state_t;

typedef struct {
    uint8_t hour;
    int8_t temperature_c;
    uint8_t precipitation_percent;
    desk_weather_code_t code;
} desk_hourly_forecast_t;

typedef struct {
    int8_t low_c;
    int8_t high_c;
    uint8_t precipitation_percent;
    desk_weather_code_t code;
} desk_daily_forecast_t;

typedef struct {
    bool active;
    desk_alert_level_t level;
    char title[96];
} desk_weather_alert_t;

typedef struct {
    bool valid;
    int8_t current_c;
    int8_t feels_like_c;
    uint8_t humidity_percent;
    desk_weather_code_t code;
    desk_daily_forecast_t today;
    desk_daily_forecast_t tomorrow;
    desk_hourly_forecast_t hourly[DESK_HOURLY_FORECAST_COUNT];
    desk_weather_alert_t alert;
    uint32_t updated_at_epoch;
} desk_weather_state_t;

typedef struct {
    char name[20];
    char code[8];
    int32_t points_x100;
    int16_t change_basis_points;
} desk_market_index_t;

typedef struct {
    bool valid;
    bool trading;
    desk_market_index_t indices[DESK_MARKET_INDEX_COUNT];
    uint32_t updated_at_epoch;
} desk_market_state_t;

typedef struct {
    bool valid;
    uint32_t sample_sequence;
    uint16_t cpu_x10_percent;
    uint16_t memory_x10_percent;
    uint32_t disk_free_gb;
    uint32_t network_up_kbps;
    uint32_t network_down_kbps;
    uint8_t battery_percent;
} desk_system_state_t;

typedef struct {
    char firmware_version[20];
    char board_name[32];
    uint32_t uptime_seconds;
    uint32_t free_internal_kb;
    uint32_t largest_internal_block_kb;
    uint32_t free_psram_kb;
    bool storage_mounted;
    int32_t storage_last_error;
    uint32_t log_written_entries;
    uint32_t log_dropped_entries;
    uint32_t log_queued_entries;
    uint32_t wifi_reconnect_attempts;
    uint8_t wifi_last_disconnect_reason;
    bool wifi_credentials_available;
    char wifi_ssid[33];
    bool ble_bonded;
    uint16_t ble_mtu;
    uint32_t heartbeat_age_ms;
    uint32_t diagnostic_snapshots;
} desk_device_state_t;

typedef struct {
    char provider[16];
    bool usage_available;
    bool secondary_usage_available;
    uint8_t primary_usage_percent;
    uint8_t secondary_usage_percent;
    uint16_t primary_window_minutes;
    uint16_t secondary_window_minutes;
    uint32_t primary_reset_seconds;
    uint32_t secondary_reset_seconds;
    uint8_t active_task_count;
    uint8_t available_task_slots;
    desk_ai_task_status_t task_status;
    uint32_t elapsed_seconds;
} desk_ai_provider_state_t;

typedef struct {
    bool assigned;
    char name[40];
    desk_ai_task_status_t status;
    char detail[24];  /* 可选副文本，如“3 分钟前”；Codex 留空，Claude 显示上次活动。 */
} desk_ai_task_slot_t;

typedef struct {
    bool valid;
    desk_ai_provider_state_t providers[DESK_AI_PROVIDER_COUNT];
    desk_ai_task_slot_t codex_tasks[DESK_AI_TASK_SLOT_COUNT];
    desk_ai_task_slot_t claude_tasks[DESK_AI_TASK_SLOT_COUNT];
} desk_ai_state_t;

typedef struct {
    bool valid;
    bool metadata_available;
    bool playing;
    bool title_hidden;
    bool muted;
    uint8_t volume_percent;
    uint32_t position_seconds;
    uint32_t duration_seconds;
    char title[96];
    char artist[64];
    char source[32];
} desk_media_state_t;

typedef struct {
    char active_app[32];
    uint8_t action_count;
} desk_control_state_t;

typedef struct {
    uint32_t revision;
    desk_connection_state_t connection;
    desk_weather_state_t weather;
    desk_market_state_t market;
    desk_system_state_t system;
    desk_control_state_t control;
    desk_ai_state_t ai;
    desk_media_state_t media;
    desk_device_state_t device;
} desk_app_state_t;

void desk_app_state_init(desk_app_state_t *state);
void desk_app_state_load_mock(desk_app_state_t *state);
void desk_app_state_clear_private(desk_app_state_t *state);
const char *desk_weather_text(desk_weather_code_t code);
const char *desk_ai_status_text(desk_ai_task_status_t status);

#ifdef __cplusplus
}
#endif
