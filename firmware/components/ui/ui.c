#include "ui/ui.h"
#include "ui/ui_font.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

enum {
    SCREEN_WIDTH = 800,
    STATUS_BAR_HEIGHT = 24,
    CONTENT_HEIGHT = 392,
    DOCK_HEIGHT = 64,
};

typedef enum {
    PAGE_HOME = 0,
    PAGE_MARKET,
    PAGE_SYSTEM,
    PAGE_CONTROL,
    PAGE_AI,
    PAGE_MEDIA,
    PAGE_LIBRARY,
    PAGE_SETTINGS,
    PAGE_DIAGNOSTICS,
    PAGE_CODEX,
    PAGE_CODEX_COMMANDS,
    PAGE_CODEX_NAVIGATION,
    PAGE_CLAUDE,
    PAGE_COUNT,
    DOCK_PAGE_COUNT = PAGE_LIBRARY + 1,
    DAILY_PAGE_COUNT = PAGE_MEDIA + 1,
} page_id_t;

static const char *const PAGE_TITLES[PAGE_COUNT] = {
    "首页",
    "A股指数",
    "Mac 系统",
    "控制条",
    "AI 状态",
    "媒体",
    "页面库",
    "设置",
    "设备诊断",
    "Codex 控制台",
    "Codex 快捷控制",
    "Codex 导航与旋钮",
    "Claude Code 控制台",
};

static const char *const DOCK_TITLES[DOCK_PAGE_COUNT] = {
    "首页",
    "市场",
    "系统",
    "控制",
    "AI",
    "媒体",
    "页面",
};

static lv_obj_t *root_screen;
static lv_obj_t *content_area;
static lv_obj_t *clock_label;
static lv_obj_t *date_label;
static lv_timer_t *clock_timer;
static lv_obj_t *feedback_panel;
static lv_timer_t *feedback_timer;
static page_id_t current_page = PAGE_HOME;
static desk_app_state_t app_state;
static bool ui_initialized;
static desk_ui_action_callback_t action_callback;
static void *action_callback_context;

enum {
    SYSTEM_HISTORY_COUNT = 30,
};

static uint16_t system_history[2][SYSTEM_HISTORY_COUNT];
static uint8_t system_history_count;
static uint8_t system_history_write_index;
static uint32_t last_system_sample_sequence;

typedef struct {
    lv_obj_t *connection_status;
    lv_obj_t *alert_bar;
    lv_obj_t *alert_title;
    lv_obj_t *home_temperature;
    lv_obj_t *home_condition;
    lv_obj_t *home_forecast;
    lv_obj_t *hourly_time[DESK_HOURLY_FORECAST_COUNT];
    lv_obj_t *hourly_temperature[DESK_HOURLY_FORECAST_COUNT];
    lv_obj_t *hourly_rain[DESK_HOURLY_FORECAST_COUNT];
    lv_obj_t *market_name[DESK_MARKET_INDEX_COUNT];
    lv_obj_t *market_code[DESK_MARKET_INDEX_COUNT];
    lv_obj_t *market_points[DESK_MARKET_INDEX_COUNT];
    lv_obj_t *market_change[DESK_MARKET_INDEX_COUNT];
    lv_obj_t *system_value[4];
    lv_obj_t *system_chart[2];
    lv_chart_series_t *system_series[2];
    lv_obj_t *control_app;
    lv_obj_t *control_action[6];
    lv_obj_t *control_volume;
    lv_obj_t *ai_name[DESK_AI_PROVIDER_COUNT];
    lv_obj_t *ai_primary[DESK_AI_PROVIDER_COUNT];
    lv_obj_t *ai_primary_bar[DESK_AI_PROVIDER_COUNT];
    lv_obj_t *ai_secondary[DESK_AI_PROVIDER_COUNT];
    lv_obj_t *ai_secondary_bar[DESK_AI_PROVIDER_COUNT];
    lv_obj_t *ai_status[DESK_AI_PROVIDER_COUNT];
    lv_obj_t *ai_overview_tasks[DESK_AI_PROVIDER_COUNT];
    lv_obj_t *ai_overview_detail[DESK_AI_PROVIDER_COUNT];
    lv_obj_t *ai_overview_usage[DESK_AI_PROVIDER_COUNT];
    lv_obj_t *ai_page_summary;
    lv_obj_t *ai_reset[2];
    lv_obj_t *ai_slot_button[DESK_AI_TASK_SLOT_COUNT];
    lv_obj_t *ai_slot_name[DESK_AI_TASK_SLOT_COUNT];
    lv_obj_t *ai_slot_status[DESK_AI_TASK_SLOT_COUNT];
    lv_obj_t *ai_slot_detail[DESK_AI_TASK_SLOT_COUNT];
    lv_obj_t *media_source;
    lv_obj_t *media_status;
    lv_obj_t *media_title;
    lv_obj_t *media_artist;
    lv_obj_t *media_play_label;
    lv_obj_t *media_progress;
    lv_obj_t *media_position;
    lv_obj_t *media_duration;
    lv_obj_t *media_volume;
    lv_obj_t *media_volume_value;
    lv_obj_t *media_mute_label;
    lv_obj_t *settings_value[4];
    lv_obj_t *settings_detail[4];
    lv_obj_t *settings_action[4];
    lv_obj_t *settings_action_label[4];
    lv_obj_t *diagnostic_value[6];
    lv_obj_t *diagnostic_detail[6];
    lv_obj_t *diagnostic_summary;
    lv_obj_t *diagnostic_refresh;
    lv_obj_t *diagnostic_snapshot;
} ui_bindings_t;

static ui_bindings_t bindings;

static void render_page(page_id_t page);
static void refresh_current_page(void);

static void clear_system_history(void)
{
    memset(system_history, 0, sizeof(system_history));
    system_history_count = 0;
    system_history_write_index = 0;
    last_system_sample_sequence = 0;
}

static void record_system_sample(const desk_system_state_t *system)
{
    if (system == NULL || !system->valid) {
        clear_system_history();
        return;
    }
    if (system->sample_sequence == 0 || system->sample_sequence == last_system_sample_sequence) {
        return;
    }
    if (last_system_sample_sequence != 0 && system->sample_sequence < last_system_sample_sequence) {
        clear_system_history();
    }
    system_history[0][system_history_write_index] = system->cpu_x10_percent;
    system_history[1][system_history_write_index] = system->memory_x10_percent;
    system_history_write_index = (uint8_t)((system_history_write_index + 1U) % SYSTEM_HISTORY_COUNT);
    if (system_history_count < SYSTEM_HISTORY_COUNT) {
        system_history_count++;
    }
    last_system_sample_sequence = system->sample_sequence;
}

void desk_ui_set_action_callback(desk_ui_action_callback_t callback, void *context)
{
    action_callback = callback;
    action_callback_context = context;
}

static lv_color_t color_background(void)
{
    return lv_color_hex(0x101419);
}

static void remove_all_styles(lv_obj_t *object)
{
    lv_obj_remove_style_all(object);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

static lv_obj_t *create_line(lv_obj_t *parent, int32_t x, int32_t y, int32_t width, int32_t height)
{
    lv_obj_t *line = lv_obj_create(parent);
    remove_all_styles(line);
    lv_obj_set_size(line, width, height);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x303844), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    return line;
}

static void set_label_text(lv_obj_t *label, const char *text)
{
    if (label != NULL) {
        lv_label_set_text(label, text);
    }
}

static void feedback_timer_callback(lv_timer_t *timer)
{
    (void)timer;
    if (feedback_panel != NULL) {
        lv_obj_delete(feedback_panel);
        feedback_panel = NULL;
    }
    feedback_timer = NULL;
}

void desk_ui_show_feedback(const char *message, bool succeeded)
{
    if (!ui_initialized || root_screen == NULL || message == NULL) {
        return;
    }
    if (feedback_timer != NULL) {
        lv_timer_delete(feedback_timer);
        feedback_timer = NULL;
    }
    if (feedback_panel != NULL) {
        lv_obj_delete(feedback_panel);
    }
    feedback_panel = lv_obj_create(root_screen);
    remove_all_styles(feedback_panel);
    lv_obj_set_size(feedback_panel, 440, 46);
    lv_obj_align(feedback_panel, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT + 10);
    lv_obj_set_style_bg_color(
        feedback_panel,
        succeeded ? lv_color_hex(0x174432) : lv_color_hex(0x5A2D2D),
        0
    );
    lv_obj_set_style_bg_opa(feedback_panel, LV_OPA_90, 0);
    lv_obj_set_style_radius(feedback_panel, 12, 0);
    lv_obj_set_style_shadow_width(feedback_panel, 18, 0);
    lv_obj_set_style_shadow_opa(feedback_panel, LV_OPA_30, 0);
    lv_obj_clear_flag(feedback_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *label = create_label(feedback_panel, message, &desk_ui_font_16, lv_color_hex(0xFFFFFF));
    lv_obj_set_width(label, 410);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(label);
    lv_obj_move_foreground(feedback_panel);

    feedback_timer = lv_timer_create(feedback_timer_callback, 1800, NULL);
    lv_timer_set_repeat_count(feedback_timer, 1);
}

static lv_color_t alert_color(desk_alert_level_t level)
{
    static const uint32_t colors[] = {
        0x34516B,
        0x2F74B5,
        0xC9A227,
        0xD87828,
        0xB73A3A,
    };
    const unsigned index = level <= DESK_ALERT_RED ? (unsigned)level : 0U;
    return lv_color_hex(colors[index]);
}

static void update_clock(lv_timer_t *timer)
{
    lv_obj_t *label = lv_timer_get_user_data(timer);
    char buffer[16];
    const time_t now = time(NULL);
    struct tm local_time = {0};
    localtime_r(&now, &local_time);

    if (local_time.tm_year + 1900 >= 2025) {
        static const char *const weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
        strftime(buffer, sizeof(buffer), "%H:%M", &local_time);
        if (date_label != NULL) {
            char date_text[40];
            snprintf(
                date_text,
                sizeof(date_text),
                "%d月%d日  星期%s",
                local_time.tm_mon + 1,
                local_time.tm_mday,
                weekdays[local_time.tm_wday]
            );
            lv_label_set_text(date_label, date_text);
        }
    } else {
        snprintf(buffer, sizeof(buffer), "--:--");
        if (date_label != NULL) {
            lv_label_set_text(date_label, "等待网络校时");
        }
    }
    lv_label_set_text(label, buffer);
}

static void create_alert_bar(const desk_weather_alert_t *alert)
{
    if (alert == NULL || !alert->active) {
        return;
    }

    lv_obj_t *bar = lv_obj_create(content_area);
    remove_all_styles(bar);
    lv_obj_set_size(bar, 768, 34);
    lv_obj_set_pos(bar, 16, 8);
    lv_obj_set_style_radius(bar, 6, 0);
    lv_obj_set_style_bg_color(bar, alert_color(alert->level), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_t *text = create_label(bar, alert->title, &desk_ui_font_16, lv_color_hex(0xFFFFFF));
    lv_obj_set_width(text, 720);
    lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(text, LV_ALIGN_LEFT_MID, 12, 0);
    bindings.alert_bar = bar;
    bindings.alert_title = text;
}

static void refresh_home_page(void)
{
    const desk_weather_state_t *weather = &app_state.weather;
    char value[96];

    if (bindings.alert_bar != NULL && weather->alert.active) {
        lv_obj_set_style_bg_color(bindings.alert_bar, alert_color(weather->alert.level), 0);
        set_label_text(bindings.alert_title, weather->alert.title);
    }

    if (weather->valid) {
        snprintf(value, sizeof(value), "%d°", weather->current_c);
    } else {
        snprintf(value, sizeof(value), "--°");
    }
    set_label_text(bindings.home_temperature, value);

    if (weather->valid) {
        snprintf(value, sizeof(value), "%s   体感 %d°", desk_weather_text(weather->code), weather->feels_like_c);
    } else {
        snprintf(value, sizeof(value), "天气数据等待联网更新");
    }
    set_label_text(bindings.home_condition, value);

    if (weather->valid) {
        snprintf(
            value,
            sizeof(value),
            "今天 %d～%d°     明天 %d～%d°",
            weather->today.low_c,
            weather->today.high_c,
            weather->tomorrow.low_c,
            weather->tomorrow.high_c
        );
    } else {
        snprintf(value, sizeof(value), "今天 --     明天 --");
    }
    set_label_text(bindings.home_forecast, value);

    for (size_t i = 0; i < DESK_HOURLY_FORECAST_COUNT; ++i) {
        const desk_hourly_forecast_t *hour = &weather->hourly[i];
        if (weather->valid) {
            snprintf(value, sizeof(value), "%02u:00", hour->hour);
        } else {
            snprintf(value, sizeof(value), "--:--");
        }
        set_label_text(bindings.hourly_time[i], value);
        if (weather->valid) {
            snprintf(value, sizeof(value), "%d°", hour->temperature_c);
        } else {
            snprintf(value, sizeof(value), "--°");
        }
        set_label_text(bindings.hourly_temperature[i], value);
        if (weather->valid) {
            snprintf(value, sizeof(value), "%u%%", hour->precipitation_percent);
        } else {
            snprintf(value, sizeof(value), "--%%");
        }
        set_label_text(bindings.hourly_rain[i], value);
    }
}

static void create_home_page(void)
{
    const desk_weather_state_t *weather = &app_state.weather;
    const int32_t top_offset = weather->alert.active ? 34 : 0;
    create_alert_bar(&weather->alert);

    create_line(content_area, 304, 28 + top_offset, 1, 200);
    clock_label = create_label(content_area, "15:28", &lv_font_montserrat_48, lv_color_hex(0xF4F7FA));
    lv_obj_set_pos(clock_label, 48, 60 + top_offset);

    date_label = create_label(content_area, "等待网络校时", &desk_ui_font_16, lv_color_hex(0x8E9AA8));
    lv_obj_set_pos(date_label, 70, 126 + top_offset);

    char value[96];
    if (weather->valid) {
        snprintf(value, sizeof(value), "%d°", weather->current_c);
    } else {
        snprintf(value, sizeof(value), "--°");
    }
    lv_obj_t *temperature = create_label(content_area, value, &lv_font_montserrat_48, lv_color_hex(0xF4F7FA));
    lv_obj_set_pos(temperature, 350, 46 + top_offset);
    bindings.home_temperature = temperature;

    if (weather->valid) {
        snprintf(
            value,
            sizeof(value),
            "%s   体感 %d°",
            desk_weather_text(weather->code),
            weather->feels_like_c
        );
    } else {
        snprintf(value, sizeof(value), "天气数据等待联网更新");
    }
    lv_obj_t *condition = create_label(content_area, value, &desk_ui_font_16, lv_color_hex(0x9DD7FF));
    lv_obj_set_pos(condition, 478, 68 + top_offset);
    bindings.home_condition = condition;

    if (weather->valid) {
        snprintf(
            value,
            sizeof(value),
            "今天 %d～%d°     明天 %d～%d°",
            weather->today.low_c,
            weather->today.high_c,
            weather->tomorrow.low_c,
            weather->tomorrow.high_c
        );
    } else {
        snprintf(value, sizeof(value), "今天 --     明天 --");
    }
    lv_obj_t *forecast = create_label(content_area, value, &desk_ui_font_16, lv_color_hex(0xC8D0D9));
    lv_obj_set_pos(forecast, 350, 132 + top_offset);
    bindings.home_forecast = forecast;

    create_line(content_area, 16, 254, 768, 1);
    for (size_t i = 0; i < DESK_HOURLY_FORECAST_COUNT; ++i) {
        const desk_hourly_forecast_t *hour = &weather->hourly[i];
        const int32_t x = 18 + (int32_t)i * 128;
        if (weather->valid) {
            snprintf(value, sizeof(value), "%02u:00", hour->hour);
        } else {
            snprintf(value, sizeof(value), "--:--");
        }
        lv_obj_t *hour_label = create_label(content_area, value, &lv_font_montserrat_16, lv_color_hex(0x8E9AA8));
        lv_obj_set_pos(hour_label, x + 20, 276);
        bindings.hourly_time[i] = hour_label;
        if (weather->valid) {
            snprintf(value, sizeof(value), "%d°", hour->temperature_c);
        } else {
            snprintf(value, sizeof(value), "--°");
        }
        lv_obj_t *temp_label = create_label(content_area, value, &lv_font_montserrat_20, lv_color_hex(0xE0E6EC));
        lv_obj_set_pos(temp_label, x + 10, 312);
        bindings.hourly_temperature[i] = temp_label;
        if (weather->valid) {
            snprintf(value, sizeof(value), "%u%%", hour->precipitation_percent);
        } else {
            snprintf(value, sizeof(value), "--%%");
        }
        lv_obj_t *rain_label = create_label(content_area, value, &lv_font_montserrat_16, lv_color_hex(0x72C7FF));
        lv_obj_set_pos(rain_label, x + 62, 316);
        bindings.hourly_rain[i] = rain_label;
    }

    clock_timer = lv_timer_create(update_clock, 1000, clock_label);
    update_clock(clock_timer);
    refresh_home_page();
}

static void refresh_market_page(void)
{
    char value[48];
    for (size_t i = 0; i < DESK_MARKET_INDEX_COUNT; ++i) {
        const desk_market_index_t *index = &app_state.market.indices[i];
        set_label_text(bindings.market_name[i], index->name[0] != '\0' ? index->name : "指数");
        set_label_text(bindings.market_code[i], index->code[0] != '\0' ? index->code : "------");
        if (!app_state.market.valid) {
            set_label_text(bindings.market_points[i], "----.--");
            set_label_text(bindings.market_change[i], "--.--%");
            if (bindings.market_change[i] != NULL) {
                lv_obj_set_style_text_color(bindings.market_change[i], lv_color_hex(0x697581), 0);
            }
            continue;
        }

        const int32_t fraction = index->points_x100 >= 0 ? index->points_x100 % 100 : -(index->points_x100 % 100);
        snprintf(value, sizeof(value), "%ld.%02ld", (long)(index->points_x100 / 100), (long)fraction);
        set_label_text(bindings.market_points[i], value);

        const bool rising = index->change_basis_points >= 0;
        const int change_abs = rising ? index->change_basis_points : -index->change_basis_points;
        snprintf(value, sizeof(value), "%s%d.%02d%%", rising ? "+" : "-", change_abs / 100, change_abs % 100);
        set_label_text(bindings.market_change[i], value);
        if (bindings.market_change[i] != NULL) {
            lv_obj_set_style_text_color(
                bindings.market_change[i],
                rising ? lv_color_hex(0xFF6B6B) : lv_color_hex(0x4CD890),
                0
            );
        }
    }
}

static void create_market_page(void)
{
    for (size_t i = 0; i < DESK_MARKET_INDEX_COUNT; ++i) {
        const desk_market_index_t *index = &app_state.market.indices[i];
        const int32_t column = (int32_t)(i % 3U);
        const int32_t row = (int32_t)(i / 3U);
        const int32_t x = column * 266;
        const int32_t y = row * 196;
        if (column > 0) {
            create_line(content_area, x, y + 12, 1, 172);
        }
        if (row > 0) {
            create_line(content_area, x + 12, y, 242, 1);
        }

        lv_obj_t *name = create_label(content_area, index->name, &desk_ui_font_16, lv_color_hex(0xC8D0D9));
        lv_obj_set_pos(name, x + 24, y + 24);
        bindings.market_name[i] = name;
        lv_obj_t *code = create_label(content_area, index->code, &lv_font_montserrat_14, lv_color_hex(0x697581));
        lv_obj_set_pos(code, x + 154, y + 26);
        bindings.market_code[i] = code;

        char value[48];
        const int32_t fraction = index->points_x100 >= 0 ? index->points_x100 % 100 : -(index->points_x100 % 100);
        snprintf(value, sizeof(value), "%ld.%02ld", (long)(index->points_x100 / 100), (long)fraction);
        lv_obj_t *points = create_label(content_area, value, &lv_font_montserrat_32, lv_color_hex(0xF4F7FA));
        lv_obj_set_pos(points, x + 24, y + 70);
        bindings.market_points[i] = points;

        const bool rising = index->change_basis_points >= 0;
        const int change_abs = rising ? index->change_basis_points : -index->change_basis_points;
        snprintf(value, sizeof(value), "%s%d.%02d%%", rising ? "+" : "-", change_abs / 100, change_abs % 100);
        lv_obj_t *change = create_label(
            content_area,
            value,
            &lv_font_montserrat_20,
            rising ? lv_color_hex(0xFF6B6B) : lv_color_hex(0x4CD890)
        );
        lv_obj_set_pos(change, x + 24, y + 126);
        bindings.market_change[i] = change;
    }
    refresh_market_page();
}

static lv_obj_t *create_metric_cell(
    int32_t column,
    int32_t row,
    const char *title,
    const char *value,
    const char *detail
)
{
    const int32_t x = column * 400;
    const int32_t y = row * 196;
    if (column > 0) {
        create_line(content_area, x, y + 12, 1, 172);
    }
    if (row > 0) {
        create_line(content_area, x + 16, y, 368, 1);
    }
    lv_obj_t *title_label = create_label(content_area, title, &desk_ui_font_16, lv_color_hex(0x8E9AA8));
    lv_obj_set_pos(title_label, x + 28, y + 26);
    lv_obj_t *value_label = create_label(content_area, value, &lv_font_montserrat_32, lv_color_hex(0xF4F7FA));
    lv_obj_set_pos(value_label, x + 28, y + 74);
    lv_obj_t *detail_label = create_label(content_area, detail, &desk_ui_font_16, lv_color_hex(0x72C7FF));
    lv_obj_set_pos(detail_label, x + 28, y + 132);
    return value_label;
}

static lv_obj_t *create_system_chart(int32_t column, int32_t row, size_t history_index)
{
    const int32_t x = column * 400;
    const int32_t y = row * 196;
    lv_obj_t *chart = lv_chart_create(content_area);
    remove_all_styles(chart);
    lv_obj_set_size(chart, 150, 68);
    lv_obj_set_pos(chart, x + 218, y + 64);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x151B21), 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chart, 8, 0);
    lv_obj_set_style_pad_all(chart, 6, 0);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, SYSTEM_HISTORY_COUNT);
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    lv_chart_set_div_line_count(chart, 0, 0);
    bindings.system_chart[history_index] = chart;
    bindings.system_series[history_index] = lv_chart_add_series(
        chart,
        history_index == 0 ? lv_color_hex(0x72C7FF) : lv_color_hex(0x72E0A8),
        LV_CHART_AXIS_PRIMARY_Y
    );
    return chart;
}

static void refresh_system_charts(void)
{
    for (size_t metric = 0; metric < 2; ++metric) {
        lv_obj_t *chart = bindings.system_chart[metric];
        lv_chart_series_t *series = bindings.system_series[metric];
        if (chart == NULL || series == NULL) {
            continue;
        }
        lv_chart_set_all_values(chart, series, LV_CHART_POINT_NONE);
        const uint8_t oldest = system_history_count == SYSTEM_HISTORY_COUNT
                                   ? system_history_write_index
                                   : 0;
        for (uint8_t i = 0; i < system_history_count; ++i) {
            const uint8_t index = (uint8_t)((oldest + i) % SYSTEM_HISTORY_COUNT);
            lv_chart_set_next_value(chart, series, system_history[metric][index]);
        }
        lv_chart_refresh(chart);
    }
}

/* 把字节/秒的速率格式化为通用文本（与 iStat Menus 同口径）：
   ≥1MB/s 显示 x.x MB，≥1KB/s 显示 x.x KB，否则 x B。都是每秒。 */
static void format_net_rate(char *buf, size_t size, uint32_t bytes_per_sec)
{
    if (bytes_per_sec >= 1024U * 1024U) {
        /* 四舍五入到 0.1 MB，与 iStat 显示一致 */
        uint32_t deci = (uint32_t)(((uint64_t)bytes_per_sec * 10U + (1024U * 1024U) / 2U) / (1024U * 1024U));
        snprintf(buf, size, "%lu.%luMB", (unsigned long)(deci / 10U), (unsigned long)(deci % 10U));
    } else if (bytes_per_sec >= 1024U) {
        /* 四舍五入到 0.1 KB */
        uint32_t deci = (uint32_t)(((uint64_t)bytes_per_sec * 10U + 512U) / 1024U);
        snprintf(buf, size, "%lu.%luKB", (unsigned long)(deci / 10U), (unsigned long)(deci % 10U));
    } else {
        snprintf(buf, size, "%luB", (unsigned long)bytes_per_sec);
    }
}

static void format_network_value(char *buf, size_t size, const desk_system_state_t *system)
{
    char down_s[16];
    char up_s[16];
    format_net_rate(down_s, sizeof(down_s), system->network_down_kbps);
    format_net_rate(up_s, sizeof(up_s), system->network_up_kbps);
    snprintf(buf, size, "%s / %s", down_s, up_s);
}

static void refresh_system_page(void)
{
    const desk_system_state_t *system = &app_state.system;
    if (!system->valid) {
        return;
    }

    char value[48];
    snprintf(value, sizeof(value), "%u.%u%%", system->cpu_x10_percent / 10U, system->cpu_x10_percent % 10U);
    set_label_text(bindings.system_value[0], value);
    snprintf(value, sizeof(value), "%u.%u%%", system->memory_x10_percent / 10U, system->memory_x10_percent % 10U);
    set_label_text(bindings.system_value[1], value);
    snprintf(value, sizeof(value), "%lu GB", (unsigned long)system->disk_free_gb);
    set_label_text(bindings.system_value[2], value);
    format_network_value(value, sizeof(value), system);
    set_label_text(bindings.system_value[3], value);
    refresh_system_charts();
}

static void create_system_page(void)
{
    const desk_system_state_t *system = &app_state.system;
    if (!system->valid) {
        lv_obj_t *empty = create_label(content_area, "Mac 未连接，系统数据已清除", &desk_ui_font_16, lv_color_hex(0x8E9AA8));
        lv_obj_center(empty);
        return;
    }

    char cpu[24];
    char memory[24];
    char disk[24];
    char network[48];
    snprintf(cpu, sizeof(cpu), "%u.%u%%", system->cpu_x10_percent / 10U, system->cpu_x10_percent % 10U);
    snprintf(memory, sizeof(memory), "%u.%u%%", system->memory_x10_percent / 10U, system->memory_x10_percent % 10U);
    snprintf(disk, sizeof(disk), "%lu GB", (unsigned long)system->disk_free_gb);
    format_network_value(network, sizeof(network), system);
    bindings.system_value[0] = create_metric_cell(0, 0, "CPU", cpu, "最近 60 秒");
    bindings.system_value[1] = create_metric_cell(1, 0, "内存", memory, "最近 60 秒");
    bindings.system_value[2] = create_metric_cell(0, 1, "磁盘可用", disk, "本机存储");
    bindings.system_value[3] = create_metric_cell(1, 1, "网络", network, "下行 / 上行 · 每秒");
    create_system_chart(0, 0, 0);
    create_system_chart(1, 0, 1);
    refresh_system_page();
}

static void action_button_callback(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || action_callback == NULL) {
        return;
    }
    const desk_ui_action_id_t action_id =
        (desk_ui_action_id_t)(uintptr_t)lv_event_get_user_data(event);
    action_callback(action_id, action_callback_context);
}

static void action_event_callback(lv_event_t *event)
{
    if (action_callback == NULL) {
        return;
    }
    action_callback(
        (desk_ui_action_id_t)(uintptr_t)lv_event_get_user_data(event),
        action_callback_context
    );
}

static void codex_page_navigation_callback(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        render_page((page_id_t)(intptr_t)lv_event_get_user_data(event));
    }
}

static lv_obj_t *create_action_button(
    lv_obj_t *parent,
    int32_t x,
    int32_t y,
    const char *title,
    desk_ui_action_id_t action_id
)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 112, 72);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1B222A), 0);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_t *label = create_label(button, title, &desk_ui_font_16, lv_color_hex(0xDCE3EA));
    lv_obj_center(label);
    if (action_id != 0) {
        lv_obj_add_event_cb(button, action_button_callback, LV_EVENT_CLICKED, (void *)(uintptr_t)action_id);
    }
    return button;
}

static lv_obj_t *create_compact_action_button(
    lv_obj_t *parent,
    int32_t x,
    int32_t y,
    int32_t width,
    const char *title,
    desk_ui_action_id_t action_id
)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, 40);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x233746), 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x35566D), 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_t *label = create_label(button, title, &desk_ui_font_16, lv_color_hex(0x9DD7FF));
    lv_obj_center(label);
    lv_obj_add_event_cb(button, action_button_callback, LV_EVENT_CLICKED, (void *)(uintptr_t)action_id);
    return button;
}

static void set_button_enabled(lv_obj_t *button, bool enabled)
{
    if (button == NULL) {
        return;
    }
    if (enabled) {
        lv_obj_remove_state(button, LV_STATE_DISABLED);
        lv_obj_set_style_opa(button, LV_OPA_COVER, 0);
    } else {
        lv_obj_add_state(button, LV_STATE_DISABLED);
        lv_obj_set_style_opa(button, LV_OPA_40, 0);
    }
}

static void refresh_control_page(void)
{
    char title[64];
    snprintf(
        title,
        sizeof(title),
        "%s  ·  自动适配",
        app_state.control.active_app[0] != '\0' ? app_state.control.active_app : "Mac"
    );
    set_label_text(bindings.control_app, title);
    for (size_t i = 0; i < 6; ++i) {
        if (bindings.control_action[i] == NULL) {
            continue;
        }
        if (i < app_state.control.action_count) {
            lv_obj_remove_state(bindings.control_action[i], LV_STATE_DISABLED);
            lv_obj_set_style_opa(bindings.control_action[i], LV_OPA_COVER, 0);
        } else {
            lv_obj_add_state(bindings.control_action[i], LV_STATE_DISABLED);
            lv_obj_set_style_opa(bindings.control_action[i], LV_OPA_40, 0);
        }
    }
    if (bindings.control_volume != NULL) {
        lv_bar_set_value(
            bindings.control_volume,
            app_state.media.volume_percent <= 100U ? app_state.media.volume_percent : 100,
            LV_ANIM_OFF
        );
    }
}

static void create_control_page(void)
{
    char title[64];
    snprintf(title, sizeof(title), "%s  ·  自动适配", app_state.control.active_app[0] != '\0' ? app_state.control.active_app : "Mac");
    lv_obj_t *app = create_label(content_area, title, &desk_ui_font_16, lv_color_hex(0x9DD7FF));
    lv_obj_set_pos(app, 24, 24);
    bindings.control_app = app;

    static const char *const actions[] = {"后退", "前进", "刷新", "新标签", "截屏", "终端"};
    for (size_t i = 0; i < 6; ++i) {
        bindings.control_action[i] = create_action_button(
            content_area,
            24 + (int32_t)i * 128,
            76,
            actions[i],
            (desk_ui_action_id_t)(DESK_UI_ACTION_BACK + i)
        );
    }
    lv_obj_t *volume = create_label(content_area, "音量", &desk_ui_font_16, lv_color_hex(0x8E9AA8));
    lv_obj_set_pos(volume, 28, 212);
    lv_obj_t *volume_bar = lv_bar_create(content_area);
    lv_obj_set_size(volume_bar, 300, 12);
    lv_obj_set_pos(volume_bar, 96, 218);
    lv_bar_set_value(
        volume_bar,
        app_state.media.volume_percent <= 100U ? app_state.media.volume_percent : 100,
        LV_ANIM_OFF
    );
    lv_obj_set_style_bg_color(volume_bar, lv_color_hex(0x303844), LV_PART_MAIN);
    lv_obj_set_style_bg_color(volume_bar, lv_color_hex(0x72C7FF), LV_PART_INDICATOR);
    bindings.control_volume = volume_bar;

    lv_obj_t *hint = create_label(content_area, "危险操作需要长按确认", &desk_ui_font_16, lv_color_hex(0x697581));
    lv_obj_set_pos(hint, 28, 304);
    refresh_control_page();
}

static void ai_page_navigation_callback(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        render_page((page_id_t)(intptr_t)lv_event_get_user_data(event));
    }
}

static void format_reset_countdown(char *output, size_t output_size, uint32_t seconds)
{
    if (seconds == 0U) {
        snprintf(output, output_size, "重置时间读取中");
    } else if (seconds >= 86400U) {
        snprintf(output, output_size, "%lu天%lu小时后重置",
                 (unsigned long)(seconds / 86400U),
                 (unsigned long)((seconds % 86400U) / 3600U));
    } else if (seconds >= 3600U) {
        snprintf(output, output_size, "%lu小时%lu分钟后重置",
                 (unsigned long)(seconds / 3600U),
                 (unsigned long)((seconds % 3600U) / 60U));
    } else {
        snprintf(output, output_size, "%lu分钟后重置",
                 (unsigned long)((seconds + 59U) / 60U));
    }
}

static void format_reset_time(char *output, size_t output_size, uint32_t seconds)
{
    if (seconds == 0U) {
        snprintf(output, output_size, "刷新时间读取中");
        return;
    }

    const time_t now = time(NULL);
    if (now < 1700000000) {
        format_reset_countdown(output, output_size, seconds);
        return;
    }

    const time_t reset_at = now + (time_t)seconds;
    struct tm local_reset = {0};
    if (localtime_r(&reset_at, &local_reset) == NULL) {
        format_reset_countdown(output, output_size, seconds);
        return;
    }

    if (seconds >= 86400U) {
        snprintf(output, output_size, "%d月%d日 %02d:%02d 刷新 · %lu天%lu小时后",
                 local_reset.tm_mon + 1,
                 local_reset.tm_mday,
                 local_reset.tm_hour,
                 local_reset.tm_min,
                 (unsigned long)(seconds / 86400U),
                 (unsigned long)((seconds % 86400U) / 3600U));
    } else {
        snprintf(output, output_size, "%d月%d日 %02d:%02d 刷新",
                 local_reset.tm_mon + 1,
                 local_reset.tm_mday,
                 local_reset.tm_hour,
                 local_reset.tm_min);
    }
}

static uint8_t quota_remaining(uint8_t used_percent)
{
    return used_percent <= 100U ? (uint8_t)(100U - used_percent) : 0U;
}

static lv_color_t ai_status_color(desk_ai_task_status_t status)
{
    switch (status) {
        case DESK_AI_WAITING_PERMISSION:
        case DESK_AI_WAITING_INPUT:
            return lv_color_hex(0xF2BE63);
        case DESK_AI_COMPLETED:
            return lv_color_hex(0x72E0A8);
        case DESK_AI_FAILED:
            return lv_color_hex(0xFF7777);
        case DESK_AI_RUNNING:
            return lv_color_hex(0x72C7FF);
        case DESK_AI_IDLE:
        default:
            return lv_color_hex(0x8E9AA8);
    }
}

static void refresh_ai_overview(void)
{
    char value[96];
    const desk_ai_provider_state_t *codex = &app_state.ai.providers[0];
    const desk_ai_provider_state_t *claude = &app_state.ai.providers[1];

    snprintf(value, sizeof(value), "%u 个运行任务 · %u 个最近任务",
             codex->active_task_count, codex->available_task_slots);
    set_label_text(bindings.ai_overview_tasks[0], value);
    snprintf(value, sizeof(value), "%s", desk_ai_status_text(codex->task_status));
    set_label_text(bindings.ai_overview_detail[0], value);
    if (bindings.ai_overview_detail[0] != NULL) {
        lv_obj_set_style_text_color(bindings.ai_overview_detail[0], ai_status_color(codex->task_status), 0);
    }
    if (codex->secondary_usage_available) {
        snprintf(value, sizeof(value), "周额度剩余 %u%%",
                 quota_remaining(codex->secondary_usage_percent));
    } else {
        snprintf(value, sizeof(value), "Codex 额度读取中");
    }
    set_label_text(bindings.ai_overview_usage[0], value);

    snprintf(value, sizeof(value), "%u 个终端会话", claude->active_task_count);
    set_label_text(bindings.ai_overview_tasks[1], value);
    snprintf(value, sizeof(value), "%s", desk_ai_status_text(claude->task_status));
    set_label_text(bindings.ai_overview_detail[1], value);
    if (bindings.ai_overview_detail[1] != NULL) {
        lv_obj_set_style_text_color(bindings.ai_overview_detail[1], ai_status_color(claude->task_status), 0);
    }
    set_label_text(bindings.ai_overview_usage[1], "仅显示本机会话状态");

    snprintf(value, sizeof(value), "Codex %u 个活动任务    Claude Code %u 个会话",
             codex->active_task_count, claude->active_task_count);
    set_label_text(bindings.ai_page_summary, value);
}

static void refresh_codex_page(void)
{
    const desk_ai_provider_state_t *codex = &app_state.ai.providers[0];
    char value[96];
    const uint8_t weekly_remaining = quota_remaining(codex->secondary_usage_percent);
    if (codex->secondary_usage_available) {
        snprintf(value, sizeof(value), "周额度剩余  %u%%", weekly_remaining);
    } else {
        snprintf(value, sizeof(value), "周额度读取中");
    }
    set_label_text(bindings.ai_secondary[0], value);
    lv_bar_set_value(bindings.ai_secondary_bar[0],
                     codex->secondary_usage_available ? weekly_remaining : 0,
                     LV_ANIM_OFF);
    if (codex->secondary_usage_available) {
        format_reset_time(value, sizeof(value), codex->secondary_reset_seconds);
    } else {
        snprintf(value, sizeof(value), "刷新时间读取中");
    }
    set_label_text(bindings.ai_reset[1], value);

    for (size_t i = 0; i < DESK_AI_TASK_SLOT_COUNT; ++i) {
        const desk_ai_task_slot_t *task = &app_state.ai.codex_tasks[i];
        const bool assigned = task->assigned && i < codex->available_task_slots;
        if (assigned) {
            snprintf(value, sizeof(value), "%s", task->name);
        } else {
            snprintf(value, sizeof(value), "未分配");
        }
        set_label_text(bindings.ai_slot_name[i], value);
        if (assigned) {
            const char *suffix = task->status == DESK_AI_WAITING_PERMISSION ||
                                         task->status == DESK_AI_WAITING_INPUT
                                     ? " · 点击处理"
                                     : (task->status == DESK_AI_COMPLETED || task->status == DESK_AI_FAILED
                                            ? " · 点击查看"
                                            : " · 点击打开");
            snprintf(value, sizeof(value), "%s%s", desk_ai_status_text(task->status), suffix);
            set_label_text(bindings.ai_slot_status[i], value);
            lv_obj_set_style_text_color(bindings.ai_slot_status[i], ai_status_color(task->status), 0);
        } else {
            set_label_text(bindings.ai_slot_status[i], "暂无任务");
            lv_obj_set_style_text_color(bindings.ai_slot_status[i], lv_color_hex(0x697581), 0);
        }
        set_button_enabled(bindings.ai_slot_button[i], assigned);
    }
}

static void refresh_claude_page(void)
{
    const desk_ai_provider_state_t *claude = &app_state.ai.providers[1];
    char value[64];
    snprintf(value, sizeof(value), "%u 个 Claude Code 会话 · 不统计额度", claude->available_task_slots);
    set_label_text(bindings.ai_page_summary, value);
    for (size_t i = 0; i < DESK_AI_TASK_SLOT_COUNT; ++i) {
        if (bindings.ai_slot_button[i] == NULL) {
            continue;  /* 本页未创建此槽位（Claude 只用前 CLAUDE_SESSION_CARD_COUNT 个）。 */
        }
        const desk_ai_task_slot_t *task = &app_state.ai.claude_tasks[i];
        if (task->assigned) {
            set_label_text(bindings.ai_slot_name[i], task->name);
            char line[64];
            if (task->detail[0] != '\0') {
                snprintf(line, sizeof(line), "%s · %s", desk_ai_status_text(task->status), task->detail);
            } else {
                snprintf(line, sizeof(line), "%s", desk_ai_status_text(task->status));
            }
            set_label_text(bindings.ai_slot_status[i], line);
            lv_obj_set_style_text_color(bindings.ai_slot_status[i], ai_status_color(task->status), 0);
        } else {
            set_label_text(bindings.ai_slot_name[i], "未分配");
            set_label_text(bindings.ai_slot_status[i], "暂无会话");
            lv_obj_set_style_text_color(bindings.ai_slot_status[i], lv_color_hex(0x697581), 0);
        }
        set_button_enabled(bindings.ai_slot_button[i], task->assigned);
    }
}

static void refresh_ai_page(void)
{
    if (!app_state.ai.valid) {
        return;
    }
    if (current_page == PAGE_CODEX) {
        refresh_codex_page();
    } else if (current_page == PAGE_CLAUDE) {
        refresh_claude_page();
    } else {
        refresh_ai_overview();
    }
}

static lv_obj_t *create_ai_overview_card(size_t index, int32_t x, page_id_t destination)
{
    const desk_ai_provider_state_t *provider = &app_state.ai.providers[index];
    lv_obj_t *card = lv_button_create(content_area);
    lv_obj_set_size(card, 373, 276);
    lv_obj_set_pos(card, x, 20);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x171D24), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x233746), LV_STATE_PRESSED);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(index == 0 ? 0x35566D : 0x513C68), 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_add_event_cb(card, ai_page_navigation_callback, LV_EVENT_CLICKED, (void *)(intptr_t)destination);

    bindings.ai_name[index] = create_label(card, provider->provider, &lv_font_montserrat_24, lv_color_hex(0xF4F7FA));
    lv_obj_set_pos(bindings.ai_name[index], 22, 24);
    bindings.ai_overview_detail[index] = create_label(card, "空闲", &desk_ui_font_16, lv_color_hex(0x8E9AA8));
    lv_obj_set_pos(bindings.ai_overview_detail[index], 255, 30);
    bindings.ai_overview_tasks[index] = create_label(card, "正在读取", &desk_ui_font_16, lv_color_hex(0xDCE3EA));
    lv_obj_set_pos(bindings.ai_overview_tasks[index], 22, 92);
    bindings.ai_overview_usage[index] = create_label(card, "正在读取", &desk_ui_font_16, lv_color_hex(0x8E9AA8));
    lv_obj_set_pos(bindings.ai_overview_usage[index], 22, 144);
    lv_obj_t *enter = create_label(card, "进入控制台", &desk_ui_font_16,
                                   lv_color_hex(index == 0 ? 0x72C7FF : 0xD8A8FF));
    lv_obj_set_pos(enter, 22, 212);
    return card;
}

static void create_ai_page(void)
{
    if (!app_state.ai.valid) {
        lv_obj_t *empty = create_label(content_area, "Mac 未连接，AI 状态已清除", &desk_ui_font_16, lv_color_hex(0x8E9AA8));
        lv_obj_center(empty);
        return;
    }
    create_ai_overview_card(0, 18, PAGE_CODEX);
    create_ai_overview_card(1, 409, PAGE_CLAUDE);
    bindings.ai_page_summary = create_label(content_area, "正在读取 AI 状态", &desk_ui_font_16, lv_color_hex(0x8E9AA8));
    lv_obj_align(bindings.ai_page_summary, LV_ALIGN_BOTTOM_MID, 0, -34);
    refresh_ai_overview();
}

static lv_obj_t *create_ai_task_slot(size_t index, desk_ui_action_id_t action_id)
{
    const int32_t x = 18 + (int32_t)(index % 3U) * 261;
    const int32_t y = 104 + (int32_t)(index / 3U) * 100;
    lv_obj_t *button = lv_button_create(content_area);
    lv_obj_set_size(button, 242, 86);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x171D24), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x233746), LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x303844), 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, action_button_callback, LV_EVENT_CLICKED, (void *)(uintptr_t)action_id);
    bindings.ai_slot_button[index] = button;
    bindings.ai_slot_detail[index] = NULL;  /* Codex 槽位无副文本，避免切页后悬垂引用。 */
    bindings.ai_slot_name[index] = create_label(button, "未分配", &desk_ui_font_16, lv_color_hex(0xE8EDF2));
    lv_obj_set_pos(bindings.ai_slot_name[index], 14, 16);
    lv_obj_set_width(bindings.ai_slot_name[index], 214);
    lv_label_set_long_mode(bindings.ai_slot_name[index], LV_LABEL_LONG_DOT);
    bindings.ai_slot_status[index] = create_label(button, "暂无任务", &desk_ui_font_16, lv_color_hex(0x697581));
    lv_obj_set_pos(bindings.ai_slot_status[index], 14, 48);
    return button;
}

static void create_codex_page(void)
{
    if (!app_state.ai.valid) {
        lv_obj_t *empty = create_label(content_area, "Mac 未连接，Codex 状态已清除", &desk_ui_font_16, lv_color_hex(0x8E9AA8));
        lv_obj_center(empty);
        return;
    }
    bindings.ai_secondary[0] = create_label(content_area, "Codex 周额度读取中", &desk_ui_font_16, lv_color_hex(0xDCE3EA));
    lv_obj_set_pos(bindings.ai_secondary[0], 18, 12);
    bindings.ai_secondary_bar[0] = lv_bar_create(content_area);
    lv_obj_set_size(bindings.ai_secondary_bar[0], 370, 10);
    lv_obj_set_pos(bindings.ai_secondary_bar[0], 18, 43);
    bindings.ai_reset[1] = create_label(content_area, "刷新时间读取中", &desk_ui_font_16, lv_color_hex(0x697581));
    lv_obj_set_pos(bindings.ai_reset[1], 18, 64);

    for (size_t i = 0; i < DESK_AI_TASK_SLOT_COUNT; ++i) {
        create_ai_task_slot(i, (desk_ui_action_id_t)(DESK_UI_ACTION_CODEX_TASK_1 + i));
    }
    lv_obj_t *controls = create_compact_action_button(content_area, 470, 318, 150, "快捷控制", 0);
    lv_obj_remove_event_cb(controls, action_button_callback);
    lv_obj_add_event_cb(
        controls,
        codex_page_navigation_callback,
        LV_EVENT_CLICKED,
        (void *)(intptr_t)PAGE_CODEX_COMMANDS
    );
    create_compact_action_button(content_area, 632, 318, 150, "打开 Codex", DESK_UI_ACTION_OPEN_CODEX);
    lv_obj_t *hint = create_label(content_area, "只同步标题和状态 · 正文与路径留在 Mac", &desk_ui_font_16, lv_color_hex(0x697581));
    lv_obj_set_pos(hint, 18, 329);
    refresh_codex_page();
}

typedef enum {
    CODEX_CONTROL_TAP = 0,
    CODEX_CONTROL_LONG_PRESS,
    CODEX_CONTROL_HOLD,
} codex_control_gesture_t;

static lv_obj_t *create_codex_control_button(
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    const char *title,
    const char *hint,
    desk_ui_action_id_t primary_action,
    desk_ui_action_id_t release_action,
    codex_control_gesture_t gesture
)
{
    lv_obj_t *button = lv_button_create(content_area);
    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x171D24), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x29465A), LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 11, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(
        button,
        gesture == CODEX_CONTROL_LONG_PRESS ? lv_color_hex(0x765A2E) : lv_color_hex(0x35566D),
        0
    );
    lv_obj_set_style_shadow_width(button, 0, 0);

    lv_obj_t *title_label = create_label(button, title, &desk_ui_font_16, lv_color_hex(0xE8EDF2));
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 14, 13);
    lv_obj_t *hint_label = create_label(
        button,
        hint,
        &desk_ui_font_16,
        gesture == CODEX_CONTROL_LONG_PRESS ? lv_color_hex(0xF2BE63) : lv_color_hex(0x7894A8)
    );
    lv_obj_align(hint_label, LV_ALIGN_BOTTOM_LEFT, 14, -12);

    if (gesture == CODEX_CONTROL_HOLD) {
        lv_obj_add_event_cb(button, action_event_callback, LV_EVENT_PRESSED, (void *)(uintptr_t)primary_action);
        lv_obj_add_event_cb(button, action_event_callback, LV_EVENT_RELEASED, (void *)(uintptr_t)release_action);
        lv_obj_add_event_cb(button, action_event_callback, LV_EVENT_PRESS_LOST, (void *)(uintptr_t)release_action);
    } else {
        lv_obj_add_event_cb(
            button,
            action_event_callback,
            gesture == CODEX_CONTROL_LONG_PRESS ? LV_EVENT_LONG_PRESSED : LV_EVENT_CLICKED,
            (void *)(uintptr_t)primary_action
        );
    }
    return button;
}

static void create_codex_commands_page(void)
{
    lv_obj_t *intro = create_label(
        content_area,
        "六个原生命令键 · 批准和拒绝需长按，语音键按住生效",
        &desk_ui_font_16,
        lv_color_hex(0x8E9AA8)
    );
    lv_obj_set_pos(intro, 18, 14);

    static const char *const titles[6] = {
        "快速模式", "长按批准", "长按拒绝", "续开新对话", "按住说话", "发送"
    };
    static const char *const hints[6] = {
        "切换 Fast", "防误触", "防误触", "从当前任务续开", "支持按住与双击", "提交输入"
    };
    static const desk_ui_action_id_t actions[6] = {
        DESK_UI_ACTION_CODEX_FAST,
        DESK_UI_ACTION_CODEX_APPROVE,
        DESK_UI_ACTION_CODEX_DECLINE,
        DESK_UI_ACTION_CODEX_CONTINUE,
        DESK_UI_ACTION_CODEX_MIC_PRESS,
        DESK_UI_ACTION_CODEX_SEND,
    };
    for (size_t i = 0; i < 6; ++i) {
        const int32_t x = 18 + (int32_t)(i % 3U) * 261;
        const int32_t y = 54 + (int32_t)(i / 3U) * 104;
        const codex_control_gesture_t gesture =
            i == 1 || i == 2 ? CODEX_CONTROL_LONG_PRESS :
            (i == 4 ? CODEX_CONTROL_HOLD : CODEX_CONTROL_TAP);
        create_codex_control_button(
            x,
            y,
            242,
            90,
            titles[i],
            hints[i],
            actions[i],
            i == 4 ? DESK_UI_ACTION_CODEX_MIC_RELEASE : 0,
            gesture
        );
    }

    lv_obj_t *navigation = create_compact_action_button(content_area, 566, 286, 216, "导航与旋钮", 0);
    lv_obj_remove_event_cb(navigation, action_button_callback);
    lv_obj_add_event_cb(
        navigation,
        codex_page_navigation_callback,
        LV_EVENT_CLICKED,
        (void *)(intptr_t)PAGE_CODEX_NAVIGATION
    );
    lv_obj_t *hint = create_label(
        content_area,
        "按键可在 Codex 设置中重映射；默认命令与官方 Codex Micro 一致",
        &desk_ui_font_16,
        lv_color_hex(0x697581)
    );
    lv_obj_set_pos(hint, 18, 335);
}

static void create_codex_navigation_page(void)
{
    lv_obj_t *intro = create_label(
        content_area,
        "方向键和旋钮由 Codex 原生解释；可映射计划、历史、侧栏、滚动、推理和技能",
        &desk_ui_font_16,
        lv_color_hex(0x8E9AA8)
    );
    lv_obj_set_pos(intro, 18, 14);

    create_codex_control_button(142, 56, 150, 62, "上", "计划模式", DESK_UI_ACTION_CODEX_UP, 0, CODEX_CONTROL_TAP);
    create_codex_control_button(18, 128, 150, 62, "左", "历史后退", DESK_UI_ACTION_CODEX_LEFT, 0, CODEX_CONTROL_TAP);
    create_codex_control_button(266, 128, 150, 62, "右", "历史前进", DESK_UI_ACTION_CODEX_RIGHT, 0, CODEX_CONTROL_TAP);
    create_codex_control_button(142, 200, 150, 62, "下", "切换侧栏", DESK_UI_ACTION_CODEX_DOWN, 0, CODEX_CONTROL_TAP);

    create_codex_control_button(454, 56, 150, 82, "逆时针", "上一个 / 减少", DESK_UI_ACTION_CODEX_DIAL_CCW, 0, CODEX_CONTROL_TAP);
    create_codex_control_button(620, 56, 162, 82, "顺时针", "下一个 / 增加", DESK_UI_ACTION_CODEX_DIAL_CW, 0, CODEX_CONTROL_TAP);
    create_codex_control_button(
        454,
        154,
        328,
        108,
        "旋钮按下",
        "点击选择 · 长按打开控制设置",
        DESK_UI_ACTION_CODEX_DIAL_PRESS,
        DESK_UI_ACTION_CODEX_DIAL_RELEASE,
        CODEX_CONTROL_HOLD
    );

    lv_obj_t *hint = create_label(
        content_area,
        "在 Codex 的控制设备设置中选择旋钮模式，也可把任一方向分配给自定义技能",
        &desk_ui_font_16,
        lv_color_hex(0x697581)
    );
    lv_obj_set_pos(hint, 18, 318);
}

#define CLAUDE_SESSION_CARD_COUNT 4

static lv_obj_t *create_claude_session_card(size_t index, int32_t x, int32_t y)
{
    lv_obj_t *button = lv_button_create(content_area);
    lv_obj_set_size(button, 378, 88);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x171D24), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x233746), LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x303844), 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(
        button,
        action_button_callback,
        LV_EVENT_CLICKED,
        (void *)(uintptr_t)(DESK_UI_ACTION_CLAUDE_SESSION_1 + index)
    );
    bindings.ai_slot_button[index] = button;

    bindings.ai_slot_name[index] = create_label(button, "未分配", &desk_ui_font_16, lv_color_hex(0xE8EDF2));
    lv_obj_set_pos(bindings.ai_slot_name[index], 16, 18);
    lv_obj_set_width(bindings.ai_slot_name[index], 346);
    lv_label_set_long_mode(bindings.ai_slot_name[index], LV_LABEL_LONG_DOT);

    /* 状态行合并“运行中 · 刚刚活动”，两行居中分布，避免时间贴底、上方留空。 */
    bindings.ai_slot_status[index] = create_label(button, "暂无会话", &desk_ui_font_16, lv_color_hex(0x697581));
    lv_obj_set_pos(bindings.ai_slot_status[index], 16, 50);
    lv_obj_set_width(bindings.ai_slot_status[index], 346);
    lv_label_set_long_mode(bindings.ai_slot_status[index], LV_LABEL_LONG_DOT);

    bindings.ai_slot_detail[index] = NULL;  /* 时间并入状态行，无独立副文本。 */
    return button;
}

static void create_claude_page(void)
{
    if (!app_state.ai.valid) {
        lv_obj_t *empty = create_label(content_area, "Mac 未连接，Claude 状态已清除", &desk_ui_font_16, lv_color_hex(0x8E9AA8));
        lv_obj_center(empty);
        return;
    }
    bindings.ai_page_summary = create_label(content_area, "正在读取 Claude Code 会话", &desk_ui_font_16, lv_color_hex(0xD8A8FF));
    lv_obj_set_pos(bindings.ai_page_summary, 18, 34);
    lv_obj_t *detail = create_label(content_area, "最近会话按活跃排序，轻点卡片跳回该对话", &desk_ui_font_16, lv_color_hex(0x697581));
    lv_obj_set_pos(detail, 18, 64);

    static const int32_t card_x[CLAUDE_SESSION_CARD_COUNT] = {18, 404, 18, 404};
    static const int32_t card_y[CLAUDE_SESSION_CARD_COUNT] = {96, 96, 194, 194};
    for (size_t i = 0; i < CLAUDE_SESSION_CARD_COUNT; ++i) {
        create_claude_session_card(i, card_x[i], card_y[i]);
    }
    /* 未使用的槽位绑定置空：refresh 遍历全部槽位时据此跳过，避免触碰上一页残留控件。 */
    for (size_t i = CLAUDE_SESSION_CARD_COUNT; i < DESK_AI_TASK_SLOT_COUNT; ++i) {
        bindings.ai_slot_button[i] = NULL;
        bindings.ai_slot_name[i] = NULL;
        bindings.ai_slot_status[i] = NULL;
        bindings.ai_slot_detail[i] = NULL;
    }
    lv_obj_t *hint = create_label(content_area, "任务内容不发送到设备，仅同步会话名与活跃状态", &desk_ui_font_16, lv_color_hex(0x697581));
    lv_obj_set_pos(hint, 18, 300);
    refresh_claude_page();
}

static void refresh_media_page(void)
{
    if (!app_state.media.valid) {
        return;
    }

    set_label_text(bindings.media_source, app_state.media.source);
    set_label_text(
        bindings.media_status,
        app_state.media.metadata_available ? (app_state.media.playing ? "正在播放" : "已暂停") : "当前无媒体"
    );
    if (bindings.media_status != NULL) {
        lv_obj_set_style_text_color(
            bindings.media_status,
            app_state.media.playing ? lv_color_hex(0x72E0A8) : lv_color_hex(0x8E9AA8),
            0
        );
    }
    set_label_text(
        bindings.media_title,
        app_state.media.title_hidden ? "媒体标题已隐藏" : app_state.media.title
    );
    set_label_text(
        bindings.media_artist,
        app_state.media.title_hidden ? "艺人信息已隐藏" :
        (app_state.media.artist[0] != '\0' ? app_state.media.artist :
         (app_state.media.metadata_available ? "未提供艺人信息" : "在 Mac 上开始播放后即可控制"))
    );
    set_label_text(bindings.media_play_label, app_state.media.playing ? "暂停" : "播放");
    set_label_text(bindings.media_mute_label, app_state.media.muted ? "取消静音" : "静音");

    const int32_t duration = app_state.media.duration_seconds > (uint32_t)INT32_MAX
                                 ? INT32_MAX
                                 : (int32_t)app_state.media.duration_seconds;
    const int32_t position = app_state.media.position_seconds > (uint32_t)duration
                                 ? duration
                                 : (int32_t)app_state.media.position_seconds;
    if (bindings.media_progress != NULL) {
        lv_bar_set_range(bindings.media_progress, 0, duration > 0 ? duration : 1);
        lv_bar_set_value(bindings.media_progress, position, LV_ANIM_OFF);
        lv_obj_set_style_opa(
            bindings.media_progress,
            app_state.media.metadata_available && duration > 0 ? LV_OPA_COVER : LV_OPA_40,
            0
        );
    }

    char time_text[32];
    if (!app_state.media.metadata_available) {
        set_label_text(bindings.media_position, "--:--");
        set_label_text(bindings.media_duration, "--:--");
    } else if (app_state.media.duration_seconds == 0U) {
        set_label_text(bindings.media_position, "--:--");
        set_label_text(bindings.media_duration, "直播 / 未知时长");
    } else {
        snprintf(
            time_text,
            sizeof(time_text),
            "%02lu:%02lu",
            (unsigned long)(app_state.media.position_seconds / 60U),
            (unsigned long)(app_state.media.position_seconds % 60U)
        );
        set_label_text(bindings.media_position, time_text);
        snprintf(
            time_text,
            sizeof(time_text),
            "%02lu:%02lu",
            (unsigned long)(app_state.media.duration_seconds / 60U),
            (unsigned long)(app_state.media.duration_seconds % 60U)
        );
        set_label_text(bindings.media_duration, time_text);
    }
    if (bindings.media_volume != NULL) {
        lv_bar_set_value(
            bindings.media_volume,
            app_state.media.volume_percent <= 100U ? app_state.media.volume_percent : 100U,
            LV_ANIM_OFF
        );
    }
    snprintf(time_text, sizeof(time_text), "%u%%", app_state.media.volume_percent);
    set_label_text(bindings.media_volume_value, time_text);
    if (bindings.media_volume != NULL) {
        lv_obj_set_style_opa(bindings.media_volume, app_state.media.muted ? LV_OPA_40 : LV_OPA_COVER, 0);
    }
}

static void create_media_page(void)
{
    if (!app_state.media.valid) {
        lv_obj_t *empty = create_label(content_area, "当前没有可控制的媒体", &desk_ui_font_16, lv_color_hex(0x8E9AA8));
        lv_obj_center(empty);
        return;
    }

    bindings.media_source = create_label(
        content_area,
        app_state.media.source,
        &desk_ui_font_16,
        lv_color_hex(0x72C7FF)
    );
    lv_obj_set_pos(bindings.media_source, 24, 18);
    bindings.media_status = create_label(
        content_area,
        "等待播放",
        &desk_ui_font_16,
        lv_color_hex(0x8E9AA8)
    );
    lv_obj_align(bindings.media_status, LV_ALIGN_TOP_RIGHT, -24, 18);

    const char *media_title = app_state.media.title_hidden ? "媒体标题已隐藏" : app_state.media.title;
    bindings.media_title = create_label(content_area, media_title, &desk_ui_font_16, lv_color_hex(0xF4F7FA));
    lv_obj_set_pos(bindings.media_title, 24, 54);
    lv_obj_set_width(bindings.media_title, 752);
    lv_label_set_long_mode(bindings.media_title, LV_LABEL_LONG_MODE_DOTS);
    bindings.media_artist = create_label(content_area, "", &desk_ui_font_16, lv_color_hex(0xA5AFBA));
    lv_obj_set_pos(bindings.media_artist, 24, 86);
    lv_obj_set_width(bindings.media_artist, 752);
    lv_label_set_long_mode(bindings.media_artist, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *progress = lv_bar_create(content_area);
    lv_obj_set_size(progress, 752, 8);
    lv_obj_set_pos(progress, 24, 124);
    lv_bar_set_range(progress, 0, 1);
    lv_bar_set_value(progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(progress, lv_color_hex(0x303844), LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress, lv_color_hex(0x72C7FF), LV_PART_INDICATOR);
    bindings.media_progress = progress;

    bindings.media_position = create_label(content_area, "00:00", &desk_ui_font_16, lv_color_hex(0x8E9AA8));
    lv_obj_set_pos(bindings.media_position, 24, 142);
    bindings.media_duration = create_label(content_area, "00:00", &desk_ui_font_16, lv_color_hex(0x8E9AA8));
    lv_obj_set_width(bindings.media_duration, 200);
    lv_obj_set_style_text_align(bindings.media_duration, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(bindings.media_duration, LV_ALIGN_TOP_RIGHT, -24, 142);

    create_action_button(content_area, 208, 184, "上一个", DESK_UI_ACTION_MEDIA_PREVIOUS);
    lv_obj_t *play_button = create_action_button(
        content_area,
        344,
        184,
        "播放",
        DESK_UI_ACTION_MEDIA_PLAY_PAUSE
    );
    bindings.media_play_label = lv_obj_get_child(play_button, 0);
    create_action_button(content_area, 480, 184, "下一个", DESK_UI_ACTION_MEDIA_NEXT);

    lv_obj_t *volume_title = create_label(
        content_area, "系统音量", &desk_ui_font_16, lv_color_hex(0x8E9AA8)
    );
    lv_obj_set_pos(volume_title, 24, 302);
    lv_obj_t *mute_button = create_compact_action_button(
        content_area, 112, 290, 112, "静音", DESK_UI_ACTION_MEDIA_MUTE
    );
    bindings.media_mute_label = lv_obj_get_child(mute_button, 0);
    create_compact_action_button(content_area, 238, 290, 100, "音量－", DESK_UI_ACTION_MEDIA_VOLUME_DOWN);
    bindings.media_volume = lv_bar_create(content_area);
    lv_obj_set_size(bindings.media_volume, 250, 10);
    lv_obj_set_pos(bindings.media_volume, 354, 305);
    lv_bar_set_range(bindings.media_volume, 0, 100);
    lv_obj_set_style_bg_color(bindings.media_volume, lv_color_hex(0x303844), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bindings.media_volume, lv_color_hex(0x72E0A8), LV_PART_INDICATOR);
    bindings.media_volume_value = create_label(
        content_area, "0%", &lv_font_montserrat_14, lv_color_hex(0xDCE3EA)
    );
    lv_obj_set_pos(bindings.media_volume_value, 618, 299);
    create_compact_action_button(content_area, 678, 290, 100, "音量＋", DESK_UI_ACTION_MEDIA_VOLUME_UP);
    refresh_media_page();
}

static lv_obj_t *create_information_card(
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    const char *title,
    size_t binding_index,
    bool diagnostics
)
{
    lv_obj_t *card = lv_obj_create(content_area);
    remove_all_styles(card);
    lv_obj_set_size(card, width, height);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x171D24), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = create_label(card, title, &desk_ui_font_16, lv_color_hex(0x8E9AA8));
    lv_obj_set_pos(title_label, 18, 16);
    lv_obj_t *value = create_label(card, "--", &desk_ui_font_16, lv_color_hex(0xF4F7FA));
    lv_obj_set_pos(value, 18, diagnostics ? 58 : 62);
    lv_obj_t *detail = create_label(card, "--", &desk_ui_font_16, lv_color_hex(0x72C7FF));
    lv_obj_set_pos(detail, 18, diagnostics ? 86 : 96);
    lv_obj_set_width(detail, width - 36);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_MODE_DOTS);

    if (diagnostics) {
        bindings.diagnostic_value[binding_index] = value;
        bindings.diagnostic_detail[binding_index] = detail;
    } else {
        bindings.settings_value[binding_index] = value;
        bindings.settings_detail[binding_index] = detail;
    }
    return card;
}

static void format_data_age(uint32_t epoch, char *output, size_t output_capacity)
{
    const time_t now = time(NULL);
    if (epoch == 0 || now < (time_t)epoch) {
        snprintf(output, output_capacity, "%s", "等待首次更新");
        return;
    }
    const uint32_t seconds = (uint32_t)(now - (time_t)epoch);
    if (seconds < 60U) {
        snprintf(output, output_capacity, "%s", "刚刚更新");
    } else if (seconds < 3600U) {
        snprintf(output, output_capacity, "%lu 分钟前", (unsigned long)(seconds / 60U));
    } else {
        snprintf(output, output_capacity, "%lu 小时前", (unsigned long)(seconds / 3600U));
    }
}

static const char *wifi_signal_text(int8_t rssi_dbm)
{
    if (rssi_dbm >= -55) {
        return "信号优秀";
    }
    if (rssi_dbm >= -67) {
        return "信号良好";
    }
    if (rssi_dbm >= -75) {
        return "信号一般";
    }
    return "信号较弱";
}

static const char *wifi_disconnect_text(uint8_t reason)
{
    switch (reason) {
        case 201:
            return "未找到已保存的网络";
        case 202:
            return "Wi-Fi 认证失败";
        case 203:
            return "无法关联到路由器";
        case 204:
            return "Wi-Fi 握手超时";
        case 205:
            return "Wi-Fi 连接失败";
        default:
            return reason == 0U ? "等待网络连接" : "Wi-Fi 连接已断开";
    }
}

static void refresh_settings_page(void)
{
    char value[96];
    char detail[96];
    if (app_state.connection.wifi_connected) {
        snprintf(
            value,
            sizeof(value),
            "%s  ·  %s",
            app_state.device.wifi_ssid[0] != '\0' ? app_state.device.wifi_ssid : "Wi-Fi 在线",
            wifi_signal_text(app_state.connection.wifi_rssi_dbm)
        );
        snprintf(
            detail,
            sizeof(detail),
            "%s  ·  %d dBm",
            app_state.connection.wifi_ipv4[0] != '\0' ? app_state.connection.wifi_ipv4 : "正在获取地址",
            app_state.connection.wifi_rssi_dbm
        );
    } else {
        snprintf(
            value,
            sizeof(value),
            "%s  ·  离线",
            app_state.device.wifi_ssid[0] != '\0' ? app_state.device.wifi_ssid : "Wi-Fi"
        );
        if (app_state.device.wifi_credentials_available) {
            snprintf(
                detail,
                sizeof(detail),
                "%s  ·  第 %lu 次重试",
                wifi_disconnect_text(app_state.device.wifi_last_disconnect_reason),
                (unsigned long)app_state.device.wifi_reconnect_attempts
            );
        } else {
            snprintf(detail, sizeof(detail), "%s", "请在 Mac 助手中配置 2.4GHz Wi-Fi");
        }
    }
    set_label_text(bindings.settings_value[0], value);
    set_label_text(bindings.settings_detail[0], detail);

    if (app_state.connection.mac_authenticated) {
        snprintf(value, sizeof(value), "Mac 已认证  ·  MTU %u", app_state.device.ble_mtu);
        const uint32_t heartbeat_ms = app_state.device.heartbeat_age_ms;
        snprintf(
            detail,
            sizeof(detail),
            "心跳 %lu.%lu 秒  ·  %s",
            (unsigned long)(heartbeat_ms / 1000U),
            (unsigned long)((heartbeat_ms % 1000U) / 100U),
            app_state.device.ble_bonded ? "系统已绑定" : "等待系统绑定"
        );
    } else {
        snprintf(
            value,
            sizeof(value),
            "%s",
            app_state.connection.ble_connected ? "蓝牙已连接，等待认证" : "蓝牙未连接"
        );
        snprintf(detail, sizeof(detail), "%s", "加密认证后亮屏  ·  断开立即熄屏");
    }
    set_label_text(bindings.settings_value[1], value);
    set_label_text(bindings.settings_detail[1], detail);

    char weather_age[32];
    char market_age[32];
    format_data_age(app_state.weather.updated_at_epoch, weather_age, sizeof(weather_age));
    format_data_age(app_state.market.updated_at_epoch, market_age, sizeof(market_age));
    snprintf(
        value,
        sizeof(value),
        "天气 %s  ·  行情 %s",
        app_state.weather.valid ? "正常" : "等待",
        app_state.market.valid ? "正常" : "等待"
    );
    set_label_text(bindings.settings_value[2], value);
    snprintf(detail, sizeof(detail), "天气 %s  ·  行情 %s", weather_age, market_age);
    set_label_text(bindings.settings_detail[2], detail);

    set_label_text(bindings.settings_value[3], "隐私保护已启用");
    set_label_text(
        bindings.settings_detail[3],
        app_state.media.title_hidden ? "媒体标题已隐藏  ·  私密状态断连清除" :
                                       "媒体标题可见  ·  私密状态断连清除"
    );
    set_label_text(
        bindings.settings_action_label[3],
        app_state.media.title_hidden ? "显示标题" : "隐藏标题"
    );
    set_button_enabled(bindings.settings_action[0], true);
    set_button_enabled(bindings.settings_action[1], app_state.connection.mac_authenticated);
    set_button_enabled(bindings.settings_action[2], app_state.connection.wifi_connected);
}

static void create_settings_page(void)
{
    static const char *const titles[] = {"网络", "蓝牙与安全", "公共数据", "显示与隐私"};
    for (size_t i = 0; i < 4; ++i) {
        const int32_t x = 18 + (int32_t)(i % 2U) * 391;
        const int32_t y = 18 + (int32_t)(i / 2U) * 187;
        lv_obj_t *card = create_information_card(x, y, 373, 169, titles[i], i, false);
        if (i == 0U) {
            lv_obj_set_width(bindings.settings_detail[i], 220);
            bindings.settings_action[i] = create_compact_action_button(
                card, 243, 113, 112, "重新连接", DESK_UI_ACTION_WIFI_RECONNECT
            );
        } else if (i == 1U) {
            lv_obj_set_width(bindings.settings_detail[i], 220);
            bindings.settings_action[i] = create_compact_action_button(
                card, 243, 113, 112, "Mac 设置", DESK_UI_ACTION_OPEN_MAC_HELPER
            );
        } else if (i == 2U) {
            lv_obj_set_width(bindings.settings_detail[i], 220);
            bindings.settings_action[i] = create_compact_action_button(
                card, 243, 113, 112, "立即刷新", DESK_UI_ACTION_PUBLIC_REFRESH
            );
        } else if (i == 3U) {
            lv_obj_set_width(bindings.settings_detail[i], 220);
            bindings.settings_action[i] = create_compact_action_button(
                card, 243, 113, 112, "隐藏标题", DESK_UI_ACTION_MEDIA_TITLE_TOGGLE
            );
            bindings.settings_action_label[i] = lv_obj_get_child(bindings.settings_action[i], 0);
        }
    }
    refresh_settings_page();
}

static void format_uptime(uint32_t seconds, char *output, size_t output_capacity)
{
    const uint32_t days = seconds / 86400U;
    const uint32_t hours = (seconds / 3600U) % 24U;
    const uint32_t minutes = (seconds / 60U) % 60U;
    if (days > 0) {
        snprintf(output, output_capacity, "%lu 天 %02lu:%02lu", (unsigned long)days, (unsigned long)hours, (unsigned long)minutes);
    } else {
        snprintf(output, output_capacity, "%02lu:%02lu", (unsigned long)hours, (unsigned long)minutes);
    }
}

static void refresh_diagnostics_page(void)
{
    const desk_device_state_t *device = &app_state.device;
    char value[80];
    char detail[96];
    char uptime[32];

    snprintf(value, sizeof(value), "固件 %s", device->firmware_version[0] != '\0' ? device->firmware_version : "unknown");
    set_label_text(bindings.diagnostic_value[0], value);
    format_uptime(device->uptime_seconds, uptime, sizeof(uptime));
    snprintf(
        detail,
        sizeof(detail),
        "%.31s  ·  运行 %.31s",
        device->board_name[0] != '\0' ? device->board_name : "ESP32-S3",
        uptime
    );
    set_label_text(bindings.diagnostic_detail[0], detail);

    snprintf(
        value,
        sizeof(value),
        "内部 %lu KB  ·  PSRAM %lu KB",
        (unsigned long)device->free_internal_kb,
        (unsigned long)device->free_psram_kb
    );
    set_label_text(bindings.diagnostic_value[1], value);
    snprintf(detail, sizeof(detail), "最大连续内存块 %lu KB", (unsigned long)device->largest_internal_block_kb);
    set_label_text(bindings.diagnostic_detail[1], detail);

    snprintf(
        value,
        sizeof(value),
        "Wi-Fi %s  ·  蓝牙 %s",
        app_state.connection.wifi_connected ? "正常" : "离线",
        app_state.connection.mac_authenticated ? "正常" : "未认证"
    );
    set_label_text(bindings.diagnostic_value[2], value);
    if (app_state.connection.wifi_connected) {
        snprintf(
            detail,
            sizeof(detail),
            "%s  ·  心跳 %lu.%lu 秒",
            app_state.connection.wifi_ipv4,
            (unsigned long)(device->heartbeat_age_ms / 1000U),
            (unsigned long)((device->heartbeat_age_ms % 1000U) / 100U)
        );
    } else {
        snprintf(detail, sizeof(detail), "%s", wifi_disconnect_text(device->wifi_last_disconnect_reason));
    }
    set_label_text(bindings.diagnostic_detail[2], detail);

    snprintf(
        value,
        sizeof(value),
        "%s  ·  日志 %lu 条",
        device->storage_mounted ? "microSD 正常" : "microSD 未就绪",
        (unsigned long)device->log_written_entries
    );
    set_label_text(bindings.diagnostic_value[3], value);
    snprintf(
        detail,
        sizeof(detail),
        "排队 %lu  ·  丢弃 %lu  ·  状态码 %ld",
        (unsigned long)device->log_queued_entries,
        (unsigned long)device->log_dropped_entries,
        (long)device->storage_last_error
    );
    set_label_text(bindings.diagnostic_detail[3], detail);

    const bool healthy = device->largest_internal_block_kb >= 32U &&
                         device->free_internal_kb >= 64U &&
                         device->storage_mounted &&
                         device->log_dropped_entries == 0U &&
                         app_state.connection.wifi_connected &&
                         app_state.connection.mac_authenticated;
    snprintf(
        value,
        sizeof(value),
        "%s  ·  已记录 %lu 份诊断快照",
        healthy ? "设备状态正常" : "有项目需要检查",
        (unsigned long)device->diagnostic_snapshots
    );
    set_label_text(bindings.diagnostic_summary, value);
    if (bindings.diagnostic_summary != NULL) {
        lv_obj_set_style_text_color(
            bindings.diagnostic_summary,
            healthy ? lv_color_hex(0x72E0A8) : lv_color_hex(0xF2BE63),
            0
        );
    }
    set_button_enabled(bindings.diagnostic_snapshot, device->storage_mounted);
}

static void create_diagnostics_page(void)
{
    static const char *const titles[] = {"设备", "内存", "连接", "存储与日志"};
    for (size_t i = 0; i < 4; ++i) {
        const int32_t x = 18 + (int32_t)(i % 2U) * 391;
        const int32_t y = 12 + (int32_t)(i / 2U) * 157;
        create_information_card(x, y, 373, 145, titles[i], i, true);
    }
    bindings.diagnostic_summary = create_label(
        content_area, "正在读取设备状态", &desk_ui_font_16, lv_color_hex(0x8E9AA8)
    );
    lv_obj_set_pos(bindings.diagnostic_summary, 20, 337);
    bindings.diagnostic_refresh = create_compact_action_button(
        content_area, 520, 326, 116, "立即刷新", DESK_UI_ACTION_DIAGNOSTIC_REFRESH
    );
    bindings.diagnostic_snapshot = create_compact_action_button(
        content_area, 650, 326, 130, "记录诊断", DESK_UI_ACTION_DIAGNOSTIC_SNAPSHOT
    );
    refresh_diagnostics_page();
}

static void library_navigation_callback(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        render_page((page_id_t)(intptr_t)lv_event_get_user_data(event));
    }
}

static void create_library_page(void)
{
    static const char *const entries[] = {"设置", "诊断", "程序坞编辑", "自动化（二期）", "通知（二期）", "会议（二期）"};
    for (size_t i = 0; i < 6; ++i) {
        const int32_t x = 34 + (int32_t)(i % 3U) * 256;
        const int32_t y = 42 + (int32_t)(i / 3U) * 154;
        lv_obj_t *button = create_action_button(content_area, x, y, entries[i], 0);
        lv_obj_set_width(button, 220);
        if (i < 2) {
            const page_id_t destination = i == 0 ? PAGE_SETTINGS : PAGE_DIAGNOSTICS;
            lv_obj_add_event_cb(
                button,
                library_navigation_callback,
                LV_EVENT_CLICKED,
                (void *)(intptr_t)destination
            );
        } else {
            lv_obj_add_state(button, LV_STATE_DISABLED);
            lv_obj_set_style_opa(button, LV_OPA_40, 0);
        }
    }
}

static void dock_click_callback(lv_event_t *event)
{
    const page_id_t page = (page_id_t)(intptr_t)lv_event_get_user_data(event);
    render_page(page);
}

static void gesture_callback(lv_event_t *event)
{
    lv_indev_t *input = lv_indev_active();
    if (input == NULL || current_page >= DAILY_PAGE_COUNT) {
        return;
    }

    const lv_dir_t direction = lv_indev_get_gesture_dir(input);
    /* Page rendering deletes the current event target, so stop propagation
     * before switching screens. */
    lv_event_stop_bubbling(event);
    if (direction == LV_DIR_LEFT && current_page + 1 < DAILY_PAGE_COUNT) {
        render_page((page_id_t)(current_page + 1));
    } else if (direction == LV_DIR_RIGHT && current_page > PAGE_HOME) {
        render_page((page_id_t)(current_page - 1));
    }
}

static void refresh_status_bar(void)
{
    char connections[96];
    snprintf(
        connections,
        sizeof(connections),
        "Wi-Fi %s    蓝牙 %s    Mac %s",
        app_state.connection.wifi_connected ? "已连" : "未连",
        app_state.connection.ble_connected ? "已连" : "未连",
        app_state.connection.mac_authenticated ? "已认证" : "未认证"
    );
    set_label_text(bindings.connection_status, connections);
}

static void secondary_page_back_callback(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        if (current_page == PAGE_CODEX_COMMANDS) {
            render_page(PAGE_CODEX);
        } else if (current_page == PAGE_CODEX_NAVIGATION) {
            render_page(PAGE_CODEX_COMMANDS);
        } else {
            render_page(current_page == PAGE_CODEX || current_page == PAGE_CLAUDE ? PAGE_AI : PAGE_LIBRARY);
        }
    }
}

static void create_status_bar(void)
{
    lv_obj_t *status_bar = lv_obj_create(root_screen);
    remove_all_styles(status_bar);
    lv_obj_set_size(status_bar, SCREEN_WIDTH, STATUS_BAR_HEIGHT);
    lv_obj_set_pos(status_bar, 0, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x171C22), 0);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);

    int32_t title_x = 16;
    if (current_page == PAGE_SETTINGS || current_page == PAGE_DIAGNOSTICS ||
        current_page == PAGE_CODEX || current_page == PAGE_CODEX_COMMANDS ||
        current_page == PAGE_CODEX_NAVIGATION || current_page == PAGE_CLAUDE) {
        lv_obj_t *back = lv_button_create(status_bar);
        remove_all_styles(back);
        lv_obj_set_size(back, 64, STATUS_BAR_HEIGHT);
        lv_obj_set_pos(back, 8, 0);
        lv_obj_set_style_bg_color(back, lv_color_hex(0x233746), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(back, 6, LV_STATE_PRESSED);
        lv_obj_add_event_cb(back, secondary_page_back_callback, LV_EVENT_CLICKED, NULL);

        lv_obj_t *back_label = create_label(back, "返回", &desk_ui_font_16, lv_color_hex(0x72C7FF));
        lv_obj_center(back_label);
        title_x = 82;
    }

    lv_obj_t *title = create_label(status_bar, PAGE_TITLES[current_page], &desk_ui_font_16, lv_color_hex(0xB9C3CD));
    lv_obj_set_pos(title, title_x, 2);

    char connections[96];
    snprintf(
        connections,
        sizeof(connections),
        "Wi-Fi %s    蓝牙 %s    Mac %s",
        app_state.connection.wifi_connected ? "已连" : "未连",
        app_state.connection.ble_connected ? "已连" : "未连",
        app_state.connection.mac_authenticated ? "已认证" : "未认证"
    );
    lv_obj_t *status = create_label(status_bar, connections, &desk_ui_font_16, lv_color_hex(0x6F7A86));
    lv_obj_align(status, LV_ALIGN_RIGHT_MID, -16, 0);
    bindings.connection_status = status;
    refresh_status_bar();
}

static void create_dock(void)
{
    lv_obj_t *dock = lv_obj_create(root_screen);
    remove_all_styles(dock);
    lv_obj_set_size(dock, SCREEN_WIDTH, DOCK_HEIGHT);
    lv_obj_set_pos(dock, 0, STATUS_BAR_HEIGHT + CONTENT_HEIGHT);
    lv_obj_set_style_bg_color(dock, lv_color_hex(0x171C22), 0);
    lv_obj_set_style_bg_opa(dock, LV_OPA_COVER, 0);

    const page_id_t selected_page =
        current_page == PAGE_CODEX || current_page == PAGE_CODEX_COMMANDS ||
                current_page == PAGE_CODEX_NAVIGATION || current_page == PAGE_CLAUDE
            ? PAGE_AI
            : (current_page >= PAGE_LIBRARY ? PAGE_LIBRARY : current_page);
    for (int i = 0; i < DOCK_PAGE_COUNT; ++i) {
        lv_obj_t *button = lv_button_create(dock);
        lv_obj_set_size(button, 104, 48);
        lv_obj_set_pos(button, 16 + i * 110, 8);
        lv_obj_set_style_radius(button, 8, 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_style_bg_color(button, i == selected_page ? lv_color_hex(0x233746) : lv_color_hex(0x171C22), 0);
        lv_obj_add_event_cb(button, dock_click_callback, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *label = create_label(
            button,
            DOCK_TITLES[i],
            &desk_ui_font_16,
            i == selected_page ? lv_color_hex(0x72C7FF) : lv_color_hex(0xA5AFBA)
        );
        lv_obj_center(label);
    }
}

static void refresh_current_page(void)
{
    refresh_status_bar();
    switch (current_page) {
        case PAGE_HOME:
            refresh_home_page();
            break;
        case PAGE_MARKET:
            refresh_market_page();
            break;
        case PAGE_SYSTEM:
            refresh_system_page();
            break;
        case PAGE_CONTROL:
            refresh_control_page();
            break;
        case PAGE_AI:
        case PAGE_CODEX:
        case PAGE_CLAUDE:
            refresh_ai_page();
            break;
        case PAGE_CODEX_COMMANDS:
        case PAGE_CODEX_NAVIGATION:
            break;
        case PAGE_MEDIA:
            refresh_media_page();
            break;
        case PAGE_SETTINGS:
            refresh_settings_page();
            break;
        case PAGE_DIAGNOSTICS:
            refresh_diagnostics_page();
            break;
        case PAGE_LIBRARY:
        default:
            break;
    }
}

static bool page_shape_changed(const desk_app_state_t *before, const desk_app_state_t *after)
{
    switch (current_page) {
        case PAGE_HOME:
            return before->weather.alert.active != after->weather.alert.active;
        case PAGE_SYSTEM:
            return before->system.valid != after->system.valid;
        case PAGE_AI:
        case PAGE_CODEX:
        case PAGE_CLAUDE:
            return before->ai.valid != after->ai.valid;
        case PAGE_CODEX_COMMANDS:
        case PAGE_CODEX_NAVIGATION:
            return false;
        case PAGE_MEDIA:
            return before->media.valid != after->media.valid;
        case PAGE_MARKET:
        case PAGE_CONTROL:
        case PAGE_LIBRARY:
        case PAGE_SETTINGS:
        case PAGE_DIAGNOSTICS:
        default:
            return false;
    }
}

static void route_child_gestures_to_content(lv_obj_t *parent)
{
    const uint32_t child_count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t *child = lv_obj_get_child(parent, (int32_t)i);
        lv_obj_add_flag(child, LV_OBJ_FLAG_GESTURE_BUBBLE);
        route_child_gestures_to_content(child);
    }
}

static void render_page(page_id_t page)
{
    if (!ui_initialized || root_screen == NULL) {
        return;
    }

    current_page = page;
    if (feedback_timer != NULL) {
        lv_timer_delete(feedback_timer);
        feedback_timer = NULL;
    }
    feedback_panel = NULL;
    clock_label = NULL;
    date_label = NULL;
    if (clock_timer != NULL) {
        lv_timer_delete(clock_timer);
        clock_timer = NULL;
    }

    lv_obj_clean(root_screen);
    memset(&bindings, 0, sizeof(bindings));
    create_status_bar();

    content_area = lv_obj_create(root_screen);
    remove_all_styles(content_area);
    lv_obj_set_size(content_area, SCREEN_WIDTH, CONTENT_HEIGHT);
    lv_obj_set_pos(content_area, 0, STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_color(content_area, color_background(), 0);
    lv_obj_set_style_bg_opa(content_area, LV_OPA_COVER, 0);
    lv_obj_clear_flag(content_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(content_area, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(content_area, gesture_callback, LV_EVENT_GESTURE, NULL);

    switch (page) {
        case PAGE_HOME:
            create_home_page();
            break;
        case PAGE_MARKET:
            create_market_page();
            break;
        case PAGE_SYSTEM:
            create_system_page();
            break;
        case PAGE_CONTROL:
            create_control_page();
            break;
        case PAGE_AI:
            create_ai_page();
            break;
        case PAGE_MEDIA:
            create_media_page();
            break;
        case PAGE_LIBRARY:
            create_library_page();
            break;
        case PAGE_SETTINGS:
            create_settings_page();
            break;
        case PAGE_DIAGNOSTICS:
            create_diagnostics_page();
            break;
        case PAGE_CODEX:
            create_codex_page();
            break;
        case PAGE_CODEX_COMMANDS:
            create_codex_commands_page();
            break;
        case PAGE_CODEX_NAVIGATION:
            create_codex_navigation_page();
            break;
        case PAGE_CLAUDE:
            create_claude_page();
            break;
        default:
            break;
    }

    route_child_gestures_to_content(content_area);
    create_dock();
}

void desk_ui_init(const desk_app_state_t *initial_state)
{
    if (initial_state != NULL) {
        app_state = *initial_state;
    } else {
        desk_app_state_init(&app_state);
    }
    record_system_sample(&app_state.system);

    root_screen = lv_screen_active();
    lv_obj_set_style_bg_color(root_screen, color_background(), 0);
    lv_obj_set_style_bg_opa(root_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root_screen, LV_OBJ_FLAG_SCROLLABLE);
    ui_initialized = true;
    render_page(PAGE_HOME);
}

void desk_ui_apply_state(const desk_app_state_t *state)
{
    if (state == NULL) {
        return;
    }
    record_system_sample(&state->system);
    const bool rebuild = ui_initialized && page_shape_changed(&app_state, state);
    app_state = *state;
    if (!ui_initialized) {
        return;
    }
    if (rebuild) {
        render_page(current_page);
    } else {
        refresh_current_page();
    }
}

void desk_ui_show_home(void)
{
    render_page(PAGE_HOME);
}

void desk_ui_set_cjk_fallback(lv_font_t *font)
{
    desk_ui_font_16.fallback = font;
    /* 让已创建的中文标签用上新 fallback 重绘。 */
    lv_obj_t *screen = lv_screen_active();
    if (screen != NULL) {
        lv_obj_invalidate(screen);
    }
}
