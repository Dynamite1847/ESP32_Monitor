#include "ui/ui.h"

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
    PAGE_COUNT,
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
};

static const char *const DOCK_TITLES[PAGE_COUNT] = {
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
static lv_timer_t *clock_timer;
static page_id_t current_page = PAGE_HOME;
static desk_app_state_t app_state;
static bool ui_initialized;

static void render_page(page_id_t page);

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

static void update_clock(lv_timer_t *timer)
{
    lv_obj_t *label = lv_timer_get_user_data(timer);
    char buffer[16];
    const time_t now = time(NULL);
    struct tm local_time = {0};
    localtime_r(&now, &local_time);

    if (local_time.tm_year + 1900 >= 2025) {
        strftime(buffer, sizeof(buffer), "%H:%M", &local_time);
    } else {
        const uint32_t total_minutes = (15U * 60U + 28U + lv_tick_get() / 60000U) % (24U * 60U);
        snprintf(
            buffer,
            sizeof(buffer),
            "%02lu:%02lu",
            (unsigned long)(total_minutes / 60U),
            (unsigned long)(total_minutes % 60U)
        );
    }
    lv_label_set_text(label, buffer);
}

static void create_alert_bar(const desk_weather_alert_t *alert)
{
    if (alert == NULL || !alert->active) {
        return;
    }

    static const uint32_t alert_colors[] = {
        0x34516B,
        0x2F74B5,
        0xC9A227,
        0xD87828,
        0xB73A3A,
    };
    const unsigned level = alert->level <= DESK_ALERT_RED ? (unsigned)alert->level : 0U;
    lv_obj_t *bar = lv_obj_create(content_area);
    remove_all_styles(bar);
    lv_obj_set_size(bar, 768, 34);
    lv_obj_set_pos(bar, 16, 8);
    lv_obj_set_style_radius(bar, 6, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(alert_colors[level]), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_t *text = create_label(bar, alert->title, &lv_font_source_han_sans_sc_16_cjk, lv_color_hex(0xFFFFFF));
    lv_obj_align(text, LV_ALIGN_LEFT_MID, 12, 0);
}

static void create_home_page(void)
{
    const desk_weather_state_t *weather = &app_state.weather;
    const int32_t top_offset = weather->alert.active ? 34 : 0;
    create_alert_bar(&weather->alert);

    create_line(content_area, 304, 28 + top_offset, 1, 200);
    clock_label = create_label(content_area, "15:28", &lv_font_montserrat_48, lv_color_hex(0xF4F7FA));
    lv_obj_set_pos(clock_label, 48, 60 + top_offset);

    lv_obj_t *date = create_label(content_area, "8月16日  星期日", &lv_font_source_han_sans_sc_16_cjk, lv_color_hex(0x8E9AA8));
    lv_obj_set_pos(date, 70, 126 + top_offset);

    char value[96];
    if (weather->valid) {
        snprintf(value, sizeof(value), "%d°", weather->current_c);
    } else {
        snprintf(value, sizeof(value), "--°");
    }
    lv_obj_t *temperature = create_label(content_area, value, &lv_font_montserrat_48, lv_color_hex(0xF4F7FA));
    lv_obj_set_pos(temperature, 350, 46 + top_offset);

    snprintf(
        value,
        sizeof(value),
        "%s   体感 %d°",
        desk_weather_text(weather->code),
        weather->feels_like_c
    );
    lv_obj_t *condition = create_label(content_area, value, &lv_font_source_han_sans_sc_16_cjk, lv_color_hex(0x9DD7FF));
    lv_obj_set_pos(condition, 478, 68 + top_offset);

    snprintf(
        value,
        sizeof(value),
        "今天 %d～%d°     明天 %d～%d°",
        weather->today.low_c,
        weather->today.high_c,
        weather->tomorrow.low_c,
        weather->tomorrow.high_c
    );
    lv_obj_t *forecast = create_label(content_area, value, &lv_font_source_han_sans_sc_16_cjk, lv_color_hex(0xC8D0D9));
    lv_obj_set_pos(forecast, 350, 132 + top_offset);

    create_line(content_area, 16, 254, 768, 1);
    for (size_t i = 0; i < DESK_HOURLY_FORECAST_COUNT; ++i) {
        const desk_hourly_forecast_t *hour = &weather->hourly[i];
        const int32_t x = 18 + (int32_t)i * 128;
        snprintf(value, sizeof(value), "%02u:00", hour->hour);
        lv_obj_t *hour_label = create_label(content_area, value, &lv_font_montserrat_16, lv_color_hex(0x8E9AA8));
        lv_obj_set_pos(hour_label, x + 20, 276);
        snprintf(value, sizeof(value), "%d°", hour->temperature_c);
        lv_obj_t *temp_label = create_label(content_area, value, &lv_font_montserrat_20, lv_color_hex(0xE0E6EC));
        lv_obj_set_pos(temp_label, x + 10, 312);
        snprintf(value, sizeof(value), "%u%%", hour->precipitation_percent);
        lv_obj_t *rain_label = create_label(content_area, value, &lv_font_montserrat_16, lv_color_hex(0x72C7FF));
        lv_obj_set_pos(rain_label, x + 62, 316);
    }

    clock_timer = lv_timer_create(update_clock, 1000, clock_label);
    update_clock(clock_timer);
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

        lv_obj_t *name = create_label(content_area, index->name, &lv_font_source_han_sans_sc_16_cjk, lv_color_hex(0xC8D0D9));
        lv_obj_set_pos(name, x + 24, y + 24);
        lv_obj_t *code = create_label(content_area, index->code, &lv_font_montserrat_14, lv_color_hex(0x697581));
        lv_obj_set_pos(code, x + 154, y + 26);

        char value[48];
        const int32_t fraction = index->points_x100 >= 0 ? index->points_x100 % 100 : -(index->points_x100 % 100);
        snprintf(value, sizeof(value), "%ld.%02ld", (long)(index->points_x100 / 100), (long)fraction);
        lv_obj_t *points = create_label(content_area, value, &lv_font_montserrat_32, lv_color_hex(0xF4F7FA));
        lv_obj_set_pos(points, x + 24, y + 70);

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
    }
}

static void create_metric_cell(
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
    lv_obj_t *title_label = create_label(content_area, title, &lv_font_source_han_sans_sc_16_cjk, lv_color_hex(0x8E9AA8));
    lv_obj_set_pos(title_label, x + 28, y + 26);
    lv_obj_t *value_label = create_label(content_area, value, &lv_font_montserrat_32, lv_color_hex(0xF4F7FA));
    lv_obj_set_pos(value_label, x + 28, y + 74);
    lv_obj_t *detail_label = create_label(content_area, detail, &lv_font_source_han_sans_sc_16_cjk, lv_color_hex(0x72C7FF));
    lv_obj_set_pos(detail_label, x + 28, y + 132);
}

static void create_system_page(void)
{
    const desk_system_state_t *system = &app_state.system;
    if (!system->valid) {
        lv_obj_t *empty = create_label(content_area, "Mac 未连接，系统数据已清除", &lv_font_source_han_sans_sc_16_cjk, lv_color_hex(0x8E9AA8));
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
    create_metric_cell(0, 0, "CPU", cpu, "最近 60 秒");
    create_metric_cell(1, 0, "内存", memory, "最近 60 秒");
    create_metric_cell(0, 1, "磁盘可用", disk, "本机存储");
    create_metric_cell(1, 1, "网络", network, "下行 / 上行");
}

static lv_obj_t *create_action_button(lv_obj_t *parent, int32_t x, int32_t y, const char *title)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 112, 72);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1B222A), 0);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_t *label = create_label(button, title, &lv_font_source_han_sans_sc_16_cjk, lv_color_hex(0xDCE3EA));
    lv_obj_center(label);
    return button;
}

static void create_control_page(void)
{
    char title[64];
    snprintf(title, sizeof(title), "%s  ·  自动适配", app_state.control.active_app[0] != '\0' ? app_state.control.active_app : "Mac");
    lv_obj_t *app = create_label(content_area, title, &lv_font_source_han_sans_sc_16_cjk, lv_color_hex(0x9DD7FF));
    lv_obj_set_pos(app, 24, 24);

    static const char *const actions[] = {"后退", "前进", "刷新", "新标签", "截屏", "终端"};
    for (size_t i = 0; i < 6; ++i) {
        create_action_button(content_area, 24 + (int32_t)i * 128, 76, actions[i]);
    }
    lv_obj_t *volume = create_label(content_area, "音量", &lv_font_source_han_sans_sc_16_cjk, lv_color_hex(0x8E9AA8));
    lv_obj_set_pos(volume, 28, 212);
    lv_obj_t *volume_bar = lv_bar_create(content_area);
    lv_obj_set_size(volume_bar, 300, 12);
    lv_obj_set_pos(volume_bar, 96, 218);
    lv_bar_set_value(volume_bar, app_state.media.volume_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(volume_bar, lv_color_hex(0x303844), LV_PART_MAIN);
    lv_obj_set_style_bg_color(volume_bar, lv_color_hex(0x72C7FF), LV_PART_INDICATOR);

    lv_obj_t *hint = create_label(content_area, "危险操作需要长按确认", &lv_font_source_han_sans_sc_16_cjk, lv_color_hex(0x697581));
    lv_obj_set_pos(hint, 28, 304);
}

static void create_ai_provider(int32_t x, const desk_ai_provider_state_t *provider)
{
    lv_obj_t *name = create_label(content_area, provider->provider, &lv_font_montserrat_24, lv_color_hex(0xF4F7FA));
    lv_obj_set_pos(name, x + 28, 34);

    char value[64];
    snprintf(value, sizeof(value), "5h   %u%%", provider->primary_usage_percent);
    lv_obj_t *primary = create_label(content_area, value, &lv_font_montserrat_20, lv_color_hex(0xDCE3EA));
    lv_obj_set_pos(primary, x + 28, 92);
    lv_obj_t *primary_bar = lv_bar_create(content_area);
    lv_obj_set_size(primary_bar, 300, 10);
    lv_obj_set_pos(primary_bar, x + 28, 130);
    lv_bar_set_value(primary_bar, provider->primary_usage_percent, LV_ANIM_OFF);

    snprintf(value, sizeof(value), "Week   %u%%", provider->secondary_usage_percent);
    lv_obj_t *secondary = create_label(content_area, value, &lv_font_montserrat_18, lv_color_hex(0xA5AFBA));
    lv_obj_set_pos(secondary, x + 28, 170);
    lv_obj_t *secondary_bar = lv_bar_create(content_area);
    lv_obj_set_size(secondary_bar, 300, 8);
    lv_obj_set_pos(secondary_bar, x + 28, 204);
    lv_bar_set_value(secondary_bar, provider->secondary_usage_percent, LV_ANIM_OFF);

    snprintf(
        value,
        sizeof(value),
        "%s  %u 个任务  %02lu:%02lu",
        desk_ai_status_text(provider->task_status),
        provider->active_task_count,
        (unsigned long)(provider->elapsed_seconds / 60U),
        (unsigned long)(provider->elapsed_seconds % 60U)
    );
    lv_obj_t *status = create_label(content_area, value, &lv_font_source_han_sans_sc_16_cjk, lv_color_hex(0x72C7FF));
    lv_obj_set_pos(status, x + 28, 260);
}

static void create_ai_page(void)
{
    if (!app_state.ai.valid) {
        lv_obj_t *empty = create_label(content_area, "Mac 未连接，AI 状态已清除", &lv_font_source_han_sans_sc_16_cjk, lv_color_hex(0x8E9AA8));
        lv_obj_center(empty);
        return;
    }
    create_ai_provider(0, &app_state.ai.providers[0]);
    create_line(content_area, 400, 20, 1, 344);
    create_ai_provider(400, &app_state.ai.providers[1]);
}

static void create_media_page(void)
{
    if (!app_state.media.valid) {
        lv_obj_t *empty = create_label(content_area, "当前没有可控制的媒体", &lv_font_source_han_sans_sc_16_cjk, lv_color_hex(0x8E9AA8));
        lv_obj_center(empty);
        return;
    }

    const char *media_title = app_state.media.title_hidden ? "媒体标题已隐藏" : app_state.media.title;
    lv_obj_t *title = create_label(content_area, media_title, &lv_font_source_han_sans_sc_16_cjk, lv_color_hex(0xDCE3EA));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 42);
    create_action_button(content_area, 206, 118, "上一个");
    create_action_button(content_area, 344, 118, app_state.media.playing ? "暂停" : "播放");
    create_action_button(content_area, 482, 118, "下一个");

    lv_obj_t *progress = lv_bar_create(content_area);
    lv_obj_set_size(progress, 536, 10);
    lv_obj_set_pos(progress, 132, 238);
    lv_bar_set_range(progress, 0, app_state.media.duration_seconds > 0 ? (int32_t)app_state.media.duration_seconds : 1);
    lv_bar_set_value(progress, (int32_t)app_state.media.position_seconds, LV_ANIM_OFF);

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
    snprintf(
        time_text,
        sizeof(time_text),
        "%02lu:%02lu",
        (unsigned long)(app_state.media.duration_seconds / 60U),
        (unsigned long)(app_state.media.duration_seconds % 60U)
    );
    lv_obj_t *duration_label = create_label(content_area, time_text, &lv_font_montserrat_14, lv_color_hex(0x8E9AA8));
    lv_obj_align(duration_label, LV_ALIGN_TOP_RIGHT, -132, 260);
}

static void create_library_page(void)
{
    static const char *const entries[] = {"设置", "诊断", "程序坞编辑", "自动化（二期）", "通知（二期）", "会议（二期）"};
    for (size_t i = 0; i < 6; ++i) {
        const int32_t x = 34 + (int32_t)(i % 3U) * 256;
        const int32_t y = 42 + (int32_t)(i / 3U) * 154;
        lv_obj_t *button = create_action_button(content_area, x, y, entries[i]);
        lv_obj_set_width(button, 220);
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
    if (direction == LV_DIR_LEFT && current_page + 1 < DAILY_PAGE_COUNT) {
        render_page((page_id_t)(current_page + 1));
    } else if (direction == LV_DIR_RIGHT && current_page > PAGE_HOME) {
        render_page((page_id_t)(current_page - 1));
    }
    lv_event_stop_bubbling(event);
}

static void create_status_bar(void)
{
    lv_obj_t *status_bar = lv_obj_create(root_screen);
    remove_all_styles(status_bar);
    lv_obj_set_size(status_bar, SCREEN_WIDTH, STATUS_BAR_HEIGHT);
    lv_obj_set_pos(status_bar, 0, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x171C22), 0);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);

    lv_obj_t *title = create_label(status_bar, PAGE_TITLES[current_page], &lv_font_source_han_sans_sc_16_cjk, lv_color_hex(0xB9C3CD));
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
    lv_obj_t *status = create_label(status_bar, connections, &lv_font_source_han_sans_sc_16_cjk, lv_color_hex(0x6F7A86));
    lv_obj_align(status, LV_ALIGN_RIGHT_MID, -16, 0);
}

static void create_dock(void)
{
    lv_obj_t *dock = lv_obj_create(root_screen);
    remove_all_styles(dock);
    lv_obj_set_size(dock, SCREEN_WIDTH, DOCK_HEIGHT);
    lv_obj_set_pos(dock, 0, STATUS_BAR_HEIGHT + CONTENT_HEIGHT);
    lv_obj_set_style_bg_color(dock, lv_color_hex(0x171C22), 0);
    lv_obj_set_style_bg_opa(dock, LV_OPA_COVER, 0);

    for (int i = 0; i < PAGE_COUNT; ++i) {
        lv_obj_t *button = lv_button_create(dock);
        lv_obj_set_size(button, 104, 48);
        lv_obj_set_pos(button, 16 + i * 110, 8);
        lv_obj_set_style_radius(button, 8, 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_style_bg_color(button, i == current_page ? lv_color_hex(0x233746) : lv_color_hex(0x171C22), 0);
        lv_obj_add_event_cb(button, dock_click_callback, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *label = create_label(
            button,
            DOCK_TITLES[i],
            &lv_font_source_han_sans_sc_16_cjk,
            i == current_page ? lv_color_hex(0x72C7FF) : lv_color_hex(0xA5AFBA)
        );
        lv_obj_center(label);
    }
}

static void render_page(page_id_t page)
{
    if (!ui_initialized || root_screen == NULL) {
        return;
    }

    current_page = page;
    clock_label = NULL;
    if (clock_timer != NULL) {
        lv_timer_delete(clock_timer);
        clock_timer = NULL;
    }

    lv_obj_clean(root_screen);
    create_status_bar();

    content_area = lv_obj_create(root_screen);
    remove_all_styles(content_area);
    lv_obj_set_size(content_area, SCREEN_WIDTH, CONTENT_HEIGHT);
    lv_obj_set_pos(content_area, 0, STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_color(content_area, color_background(), 0);
    lv_obj_set_style_bg_opa(content_area, LV_OPA_COVER, 0);
    lv_obj_clear_flag(content_area, LV_OBJ_FLAG_SCROLLABLE);
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
        default:
            break;
    }

    create_dock();
}

void desk_ui_init(const desk_app_state_t *initial_state)
{
    if (initial_state != NULL) {
        app_state = *initial_state;
    } else {
        desk_app_state_init(&app_state);
    }

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
    app_state = *state;
    render_page(current_page);
}

void desk_ui_show_home(void)
{
    render_page(PAGE_HOME);
}
