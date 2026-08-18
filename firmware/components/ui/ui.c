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
    lv_obj_t *media_title;
    lv_obj_t *media_play_label;
    lv_obj_t *media_progress;
    lv_obj_t *media_position;
    lv_obj_t *media_duration;
    lv_obj_t *settings_value[4];
    lv_obj_t *settings_detail[4];
    lv_obj_t *diagnostic_value[6];
    lv_obj_t *diagnostic_detail[6];
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
    snprintf(
        value,
        sizeof(value),
        "%lu / %lu",
        (unsigned long)system->network_down_kbps,
        (unsigned long)system->network_up_kbps
    );
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
    snprintf(network, sizeof(network), "%lu / %lu", (unsigned long)system->network_down_kbps, (unsigned long)system->network_up_kbps);
    bindings.system_value[0] = create_metric_cell(0, 0, "CPU", cpu, "最近 60 秒");
    bindings.system_value[1] = create_metric_cell(1, 0, "内存", memory, "最近 60 秒");
    bindings.system_value[2] = create_metric_cell(0, 1, "磁盘可用", disk, "本机存储");
    bindings.system_value[3] = create_metric_cell(1, 1, "网络", network, "下行 / 上行");
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

static void refresh_ai_page(void)
{
    if (!app_state.ai.valid) {
        return;
    }

    char value[64];
    for (size_t i = 0; i < DESK_AI_PROVIDER_COUNT; ++i) {
        const desk_ai_provider_state_t *provider = &app_state.ai.providers[i];
        set_label_text(bindings.ai_name[i], provider->provider);
        if (provider->usage_available) {
            const uint16_t minutes = provider->primary_window_minutes;
            if (minutes >= 1440U && minutes % 1440U == 0) {
                snprintf(value, sizeof(value), "%u 天额度   %u%%", minutes / 1440U, provider->primary_usage_percent);
            } else if (minutes >= 60U && minutes % 60U == 0) {
                snprintf(value, sizeof(value), "%u 小时额度   %u%%", minutes / 60U, provider->primary_usage_percent);
            } else {
                snprintf(value, sizeof(value), "%u 分钟额度   %u%%", minutes, provider->primary_usage_percent);
            }
        } else {
            snprintf(value, sizeof(value), "%s", "用量接口待接入");
        }
        set_label_text(bindings.ai_primary[i], value);
        if (bindings.ai_primary_bar[i] != NULL) {
            lv_bar_set_value(
                bindings.ai_primary_bar[i],
                provider->usage_available && provider->primary_usage_percent <= 100U
                    ? provider->primary_usage_percent : 0,
                LV_ANIM_OFF
            );
        }
        if (provider->secondary_usage_available) {
            const uint16_t minutes = provider->secondary_window_minutes;
            if (minutes >= 1440U && minutes % 1440U == 0) {
                snprintf(value, sizeof(value), "%u 天额度   %u%%", minutes / 1440U, provider->secondary_usage_percent);
            } else if (minutes >= 60U && minutes % 60U == 0) {
                snprintf(value, sizeof(value), "%u 小时额度   %u%%", minutes / 60U, provider->secondary_usage_percent);
            } else {
                snprintf(value, sizeof(value), "%u 分钟额度   %u%%", minutes, provider->secondary_usage_percent);
            }
        } else {
            snprintf(value, sizeof(value), "%s", "无第二额度");
        }
        set_label_text(bindings.ai_secondary[i], value);
        if (bindings.ai_secondary_bar[i] != NULL) {
            lv_bar_set_value(
                bindings.ai_secondary_bar[i],
                provider->secondary_usage_available && provider->secondary_usage_percent <= 100U
                    ? provider->secondary_usage_percent : 0,
                LV_ANIM_OFF
            );
        }
        snprintf(
            value,
            sizeof(value),
            "%s  %u 个任务  %02lu:%02lu",
            desk_ai_status_text(provider->task_status),
            provider->active_task_count,
            (unsigned long)(provider->elapsed_seconds / 60U),
            (unsigned long)(provider->elapsed_seconds % 60U)
        );
        set_label_text(bindings.ai_status[i], value);
    }
}

static void create_ai_provider(size_t index, int32_t x, const desk_ai_provider_state_t *provider)
{
    lv_obj_t *name = create_label(content_area, provider->provider, &lv_font_montserrat_24, lv_color_hex(0xF4F7FA));
    lv_obj_set_pos(name, x + 28, 34);
    bindings.ai_name[index] = name;

    char value[64];
    snprintf(value, sizeof(value), "%s", provider->usage_available ? "额度读取中" : "用量接口待接入");
    lv_obj_t *primary = create_label(content_area, value, &lv_font_montserrat_20, lv_color_hex(0xDCE3EA));
    lv_obj_set_pos(primary, x + 28, 92);
    bindings.ai_primary[index] = primary;
    lv_obj_t *primary_bar = lv_bar_create(content_area);
    lv_obj_set_size(primary_bar, 300, 10);
    lv_obj_set_pos(primary_bar, x + 28, 130);
    lv_bar_set_value(primary_bar, provider->primary_usage_percent <= 100U ? provider->primary_usage_percent : 100, LV_ANIM_OFF);
    bindings.ai_primary_bar[index] = primary_bar;

    snprintf(value, sizeof(value), "%s", provider->secondary_usage_available ? "额度读取中" : "无第二额度");
    lv_obj_t *secondary = create_label(content_area, value, &lv_font_montserrat_18, lv_color_hex(0xA5AFBA));
    lv_obj_set_pos(secondary, x + 28, 170);
    bindings.ai_secondary[index] = secondary;
    lv_obj_t *secondary_bar = lv_bar_create(content_area);
    lv_obj_set_size(secondary_bar, 300, 8);
    lv_obj_set_pos(secondary_bar, x + 28, 204);
    lv_bar_set_value(
        secondary_bar,
        provider->secondary_usage_percent <= 100U ? provider->secondary_usage_percent : 100,
        LV_ANIM_OFF
    );
    bindings.ai_secondary_bar[index] = secondary_bar;

    snprintf(
        value,
        sizeof(value),
        "%s  %u 个任务  %02lu:%02lu",
        desk_ai_status_text(provider->task_status),
        provider->active_task_count,
        (unsigned long)(provider->elapsed_seconds / 60U),
        (unsigned long)(provider->elapsed_seconds % 60U)
    );
    lv_obj_t *status = create_label(content_area, value, &desk_ui_font_16, lv_color_hex(0x72C7FF));
    lv_obj_set_pos(status, x + 28, 260);
    bindings.ai_status[index] = status;
}

static void create_ai_page(void)
{
    if (!app_state.ai.valid) {
        lv_obj_t *empty = create_label(content_area, "Mac 未连接，AI 状态已清除", &desk_ui_font_16, lv_color_hex(0x8E9AA8));
        lv_obj_center(empty);
        return;
    }
    create_ai_provider(0, 0, &app_state.ai.providers[0]);
    create_line(content_area, 400, 20, 1, 344);
    create_ai_provider(1, 400, &app_state.ai.providers[1]);
    refresh_ai_page();
}

static void refresh_media_page(void)
{
    if (!app_state.media.valid) {
        return;
    }

    set_label_text(
        bindings.media_title,
        app_state.media.title_hidden ? "媒体标题已隐藏" : app_state.media.title
    );
    set_label_text(bindings.media_play_label, "播放/暂停");

    const int32_t duration = app_state.media.duration_seconds > (uint32_t)INT32_MAX
                                 ? INT32_MAX
                                 : (int32_t)app_state.media.duration_seconds;
    const int32_t position = app_state.media.position_seconds > (uint32_t)duration
                                 ? duration
                                 : (int32_t)app_state.media.position_seconds;
    if (bindings.media_progress != NULL) {
        lv_bar_set_range(bindings.media_progress, 0, duration > 0 ? duration : 1);
        lv_bar_set_value(bindings.media_progress, position, LV_ANIM_OFF);
    }

    char time_text[24];
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

static void create_media_page(void)
{
    if (!app_state.media.valid) {
        lv_obj_t *empty = create_label(content_area, "当前没有可控制的媒体", &desk_ui_font_16, lv_color_hex(0x8E9AA8));
        lv_obj_center(empty);
        return;
    }

    const char *media_title = app_state.media.title_hidden ? "媒体标题已隐藏" : app_state.media.title;
    lv_obj_t *title = create_label(content_area, media_title, &desk_ui_font_16, lv_color_hex(0xDCE3EA));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 42);
    bindings.media_title = title;
    create_action_button(content_area, 24, 118, "静音", DESK_UI_ACTION_MEDIA_MUTE);
    create_action_button(content_area, 152, 118, "音量－", DESK_UI_ACTION_MEDIA_VOLUME_DOWN);
    create_action_button(content_area, 280, 118, "上一个", DESK_UI_ACTION_MEDIA_PREVIOUS);
    lv_obj_t *play_button = create_action_button(
        content_area,
        408,
        118,
        "播放/暂停",
        DESK_UI_ACTION_MEDIA_PLAY_PAUSE
    );
    bindings.media_play_label = lv_obj_get_child(play_button, 0);
    create_action_button(content_area, 536, 118, "下一个", DESK_UI_ACTION_MEDIA_NEXT);
    create_action_button(content_area, 664, 118, "音量＋", DESK_UI_ACTION_MEDIA_VOLUME_UP);

    lv_obj_t *progress = lv_bar_create(content_area);
    lv_obj_set_size(progress, 536, 10);
    lv_obj_set_pos(progress, 132, 238);
    lv_bar_set_range(progress, 0, 1);
    lv_bar_set_value(progress, 0, LV_ANIM_OFF);
    bindings.media_progress = progress;

    char time_text[24];
    snprintf(
        time_text,
        sizeof(time_text),
        "%02lu:%02lu",
        (unsigned long)(app_state.media.position_seconds / 60U),
        (unsigned long)(app_state.media.position_seconds % 60U)
    );
    lv_obj_t *position_label = create_label(content_area, time_text, &lv_font_montserrat_14, lv_color_hex(0x8E9AA8));
    lv_obj_set_pos(position_label, 132, 260);
    bindings.media_position = position_label;
    snprintf(
        time_text,
        sizeof(time_text),
        "%02lu:%02lu",
        (unsigned long)(app_state.media.duration_seconds / 60U),
        (unsigned long)(app_state.media.duration_seconds % 60U)
    );
    lv_obj_t *duration_label = create_label(content_area, time_text, &lv_font_montserrat_14, lv_color_hex(0x8E9AA8));
    lv_obj_align(duration_label, LV_ALIGN_TOP_RIGHT, -132, 260);
    bindings.media_duration = duration_label;
    refresh_media_page();
}

static void create_information_card(
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
    lv_obj_set_pos(detail, 18, diagnostics ? 100 : 112);
    lv_obj_set_width(detail, width - 36);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_MODE_DOTS);

    if (diagnostics) {
        bindings.diagnostic_value[binding_index] = value;
        bindings.diagnostic_detail[binding_index] = detail;
    } else {
        bindings.settings_value[binding_index] = value;
        bindings.settings_detail[binding_index] = detail;
    }
}

static void refresh_settings_page(void)
{
    char value[96];
    char detail[96];
    if (app_state.connection.wifi_connected) {
        snprintf(value, sizeof(value), "已连接  ·  %d dBm", app_state.connection.wifi_rssi_dbm);
        snprintf(
            detail,
            sizeof(detail),
            "IP %s",
            app_state.connection.wifi_ipv4[0] != '\0' ? app_state.connection.wifi_ipv4 : "获取中"
        );
    } else {
        snprintf(value, sizeof(value), "%s", "未连接");
        snprintf(detail, sizeof(detail), "%s", "请在 Mac 助手中配置 2.4GHz Wi-Fi");
    }
    set_label_text(bindings.settings_value[0], value);
    set_label_text(bindings.settings_detail[0], detail);

    snprintf(
        value,
        sizeof(value),
        "%s",
        app_state.connection.mac_authenticated ? "Mac 已认证" :
        (app_state.connection.ble_connected ? "蓝牙已连接，等待认证" : "蓝牙未连接")
    );
    set_label_text(bindings.settings_value[1], value);
    set_label_text(bindings.settings_detail[1], "断开连接或心跳超时后立即熄屏");

    snprintf(
        value,
        sizeof(value),
        "天气 %s  ·  行情 %s",
        app_state.weather.valid ? "正常" : "待配置",
        app_state.market.valid ? "正常" : "等待联网"
    );
    set_label_text(bindings.settings_value[2], value);
    set_label_text(bindings.settings_detail[2], "公共数据由设备通过 HTTPS 直接获取");

    set_label_text(bindings.settings_value[3], "横屏 800×480  ·  深色主题");
    set_label_text(bindings.settings_detail[3], "通知正文隐藏，私密状态断连后清除");
}

static void create_settings_page(void)
{
    static const char *const titles[] = {"网络", "蓝牙与安全", "公共数据", "显示与隐私"};
    for (size_t i = 0; i < 4; ++i) {
        const int32_t x = 18 + (int32_t)(i % 2U) * 391;
        const int32_t y = 18 + (int32_t)(i / 2U) * 187;
        create_information_card(x, y, 373, 169, titles[i], i, false);
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

    snprintf(value, sizeof(value), "固件 %s", device->firmware_version[0] != '\0' ? device->firmware_version : "unknown");
    set_label_text(bindings.diagnostic_value[0], value);
    set_label_text(bindings.diagnostic_detail[0], device->board_name[0] != '\0' ? device->board_name : "ESP32-S3");

    format_uptime(device->uptime_seconds, value, sizeof(value));
    set_label_text(bindings.diagnostic_value[1], value);
    set_label_text(bindings.diagnostic_detail[1], "每 5 秒刷新");

    snprintf(value, sizeof(value), "%lu KB 可用", (unsigned long)device->free_internal_kb);
    snprintf(detail, sizeof(detail), "最大连续块 %lu KB", (unsigned long)device->largest_internal_block_kb);
    set_label_text(bindings.diagnostic_value[2], value);
    set_label_text(bindings.diagnostic_detail[2], detail);

    snprintf(value, sizeof(value), "%lu KB 可用", (unsigned long)device->free_psram_kb);
    set_label_text(bindings.diagnostic_value[3], value);
    set_label_text(bindings.diagnostic_detail[3], "图形缓冲与网络响应优先使用 PSRAM");

    set_label_text(bindings.diagnostic_value[4], device->storage_mounted ? "microSD 已挂载" : "microSD 未就绪");
    snprintf(detail, sizeof(detail), "最近状态码 %ld", (long)device->storage_last_error);
    set_label_text(bindings.diagnostic_detail[4], detail);

    snprintf(value, sizeof(value), "%lu 条已写入", (unsigned long)device->log_written_entries);
    snprintf(detail, sizeof(detail), "%lu 条丢弃", (unsigned long)device->log_dropped_entries);
    set_label_text(bindings.diagnostic_value[5], value);
    set_label_text(bindings.diagnostic_detail[5], detail);
}

static void create_diagnostics_page(void)
{
    static const char *const titles[] = {"设备", "运行时间", "内部内存", "PSRAM", "存储卡", "日志"};
    for (size_t i = 0; i < 6; ++i) {
        const int32_t x = 16 + (int32_t)(i % 3U) * 261;
        const int32_t y = 18 + (int32_t)(i / 3U) * 187;
        create_information_card(x, y, 245, 169, titles[i], i, true);
    }
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

static void create_status_bar(void)
{
    lv_obj_t *status_bar = lv_obj_create(root_screen);
    remove_all_styles(status_bar);
    lv_obj_set_size(status_bar, SCREEN_WIDTH, STATUS_BAR_HEIGHT);
    lv_obj_set_pos(status_bar, 0, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x171C22), 0);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);

    lv_obj_t *title = create_label(status_bar, PAGE_TITLES[current_page], &desk_ui_font_16, lv_color_hex(0xB9C3CD));
    lv_obj_set_pos(title, 16, 2);

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

    const page_id_t selected_page = current_page >= PAGE_LIBRARY ? PAGE_LIBRARY : current_page;
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
            refresh_ai_page();
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
            return before->ai.valid != after->ai.valid;
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
