#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_model/app_model.h"
#include "ble_protocol/ble_protocol.h"
#include "market_parser.h"
#include "privacy/privacy_state_machine.h"
#include "qweather_parser.h"
#include "storage/log_policy.h"

static void test_privacy_happy_path(void)
{
    desk_privacy_context_t context;
    desk_privacy_init(&context, 6000);
    assert(context.state == DESK_PRIVACY_LOCKED);

    desk_privacy_actions_t actions = desk_privacy_dispatch(&context, DESK_PRIVACY_EVENT_BOOT, 0);
    assert((actions & DESK_PRIVACY_ACTION_BACKLIGHT_OFF) != 0);
    assert((actions & DESK_PRIVACY_ACTION_TOUCH_DISABLE) != 0);
    assert((actions & DESK_PRIVACY_ACTION_CLEAR_PRIVATE_DATA) != 0);

    actions = desk_privacy_dispatch(&context, DESK_PRIVACY_EVENT_BLE_CONNECTED, 100);
    assert(context.state == DESK_PRIVACY_AUTHENTICATING);
    assert((actions & DESK_PRIVACY_ACTION_BACKLIGHT_OFF) != 0);

    actions = desk_privacy_dispatch(&context, DESK_PRIVACY_EVENT_AUTH_SUCCEEDED, 200);
    assert(context.state == DESK_PRIVACY_ACTIVE);
    assert((actions & DESK_PRIVACY_ACTION_BACKLIGHT_ON) != 0);
    assert((actions & DESK_PRIVACY_ACTION_TOUCH_ENABLE) != 0);
    assert((actions & DESK_PRIVACY_ACTION_SHOW_HOME) != 0);

    assert(desk_privacy_poll(&context, 6200) == DESK_PRIVACY_ACTION_NONE);
    actions = desk_privacy_poll(&context, 6201);
    assert(context.state == DESK_PRIVACY_LOCKED);
    assert((actions & DESK_PRIVACY_ACTION_CLEAR_PRIVATE_DATA) != 0);
    assert((actions & DESK_PRIVACY_ACTION_DISCONNECT_BLE) != 0);
}

static void test_privacy_failure_paths(void)
{
    desk_privacy_context_t context;
    desk_privacy_init(&context, 6000);

    desk_privacy_dispatch(&context, DESK_PRIVACY_EVENT_BLE_CONNECTED, 10);
    desk_privacy_actions_t actions = desk_privacy_dispatch(&context, DESK_PRIVACY_EVENT_AUTH_FAILED, 20);
    assert(context.state == DESK_PRIVACY_LOCKED);
    assert((actions & DESK_PRIVACY_ACTION_DISCONNECT_BLE) != 0);

    desk_privacy_dispatch(&context, DESK_PRIVACY_EVENT_BLE_CONNECTED, 30);
    desk_privacy_dispatch(&context, DESK_PRIVACY_EVENT_AUTH_SUCCEEDED, 40);
    actions = desk_privacy_dispatch(&context, DESK_PRIVACY_EVENT_MAC_LOCKED, 50);
    assert(context.state == DESK_PRIVACY_LOCKED);
    assert((actions & DESK_PRIVACY_ACTION_BACKLIGHT_OFF) != 0);

    actions = desk_privacy_dispatch(&context, DESK_PRIVACY_EVENT_AUTH_SUCCEEDED, 70);
    assert(actions == DESK_PRIVACY_ACTION_NONE);

    actions = desk_privacy_dispatch(&context, DESK_PRIVACY_EVENT_HEARTBEAT, 90);
    assert(actions == DESK_PRIVACY_ACTION_NONE);
    assert(context.last_heartbeat_ms == 0);
}

static void test_privacy_heartbeat_wraparound(void)
{
    desk_privacy_context_t context;
    desk_privacy_init(&context, 6000);
    desk_privacy_dispatch(&context, DESK_PRIVACY_EVENT_BLE_CONNECTED, UINT32_MAX - 5000U);
    desk_privacy_dispatch(&context, DESK_PRIVACY_EVENT_AUTH_SUCCEEDED, UINT32_MAX - 4000U);

    assert(desk_privacy_poll(&context, 1000U) == DESK_PRIVACY_ACTION_NONE);
    const desk_privacy_actions_t actions = desk_privacy_poll(&context, 2001U);
    assert(context.state == DESK_PRIVACY_LOCKED);
    assert((actions & DESK_PRIVACY_ACTION_BACKLIGHT_OFF) != 0);
}

static void test_protocol_codec(void)
{
    static const uint8_t crc_vector[] = "123456789";
    assert(desk_protocol_crc16(crc_vector, sizeof(crc_vector) - 1) == 0x29B1);

    const uint8_t payload[] = {0x10, 0x20, 0x30};
    const desk_protocol_frame_t source = {
        .flags = DESK_FRAME_FLAG_ACK_REQUIRED,
        .message_type = DESK_MESSAGE_HEARTBEAT,
        .sequence = 42,
        .payload = payload,
        .payload_length = sizeof(payload),
    };

    uint8_t encoded[DESK_PROTOCOL_MAX_FRAME_SIZE];
    size_t encoded_length = 0;
    assert(desk_protocol_encode(&source, encoded, sizeof(encoded), &encoded_length) == DESK_PROTOCOL_OK);
    assert(encoded_length == DESK_PROTOCOL_HEADER_SIZE + sizeof(payload) + DESK_PROTOCOL_CRC_SIZE);

    desk_protocol_frame_t decoded = {0};
    assert(desk_protocol_decode(encoded, encoded_length, &decoded) == DESK_PROTOCOL_OK);
    assert(decoded.flags == source.flags);
    assert(decoded.message_type == source.message_type);
    assert(decoded.sequence == source.sequence);
    assert(decoded.payload_length == sizeof(payload));
    assert(memcmp(decoded.payload, payload, sizeof(payload)) == 0);

    encoded[DESK_PROTOCOL_HEADER_SIZE] ^= 0x01;
    assert(desk_protocol_decode(encoded, encoded_length, &decoded) == DESK_PROTOCOL_BAD_CRC);
}

static void test_protocol_rejects_invalid_frames(void)
{
    uint8_t encoded[DESK_PROTOCOL_MAX_FRAME_SIZE];
    size_t encoded_length = 0;
    const desk_protocol_frame_t empty_frame = {
        .message_type = DESK_MESSAGE_LOCK,
        .sequence = 7,
    };
    assert(desk_protocol_encode(&empty_frame, encoded, sizeof(encoded), &encoded_length) == DESK_PROTOCOL_OK);

    desk_protocol_frame_t decoded = {0};
    encoded[0] = 0;
    assert(desk_protocol_decode(encoded, encoded_length, &decoded) == DESK_PROTOCOL_BAD_MAGIC);
    encoded[0] = DESK_PROTOCOL_MAGIC_0;

    encoded[2] = DESK_PROTOCOL_VERSION + 1;
    assert(desk_protocol_decode(encoded, encoded_length, &decoded) == DESK_PROTOCOL_UNSUPPORTED_VERSION);
    encoded[2] = DESK_PROTOCOL_VERSION;

    assert(desk_protocol_decode(encoded, encoded_length - 1U, &decoded) == DESK_PROTOCOL_BAD_LENGTH);

    const desk_protocol_frame_t oversized_frame = {
        .message_type = DESK_MESSAGE_SYSTEM_STATE,
        .payload = encoded,
        .payload_length = DESK_PROTOCOL_MAX_PAYLOAD_SIZE + 1U,
    };
    assert(desk_protocol_encode(&oversized_frame, encoded, sizeof(encoded), &encoded_length) == DESK_PROTOCOL_BAD_LENGTH);

    assert(desk_protocol_encode(NULL, encoded, sizeof(encoded), &encoded_length) == DESK_PROTOCOL_INVALID_ARGUMENT);
    assert(desk_protocol_encode(&empty_frame, NULL, sizeof(encoded), &encoded_length) == DESK_PROTOCOL_INVALID_ARGUMENT);
    assert(desk_protocol_encode(&empty_frame, encoded, sizeof(encoded), NULL) == DESK_PROTOCOL_INVALID_ARGUMENT);
    assert(
        desk_protocol_encode(&empty_frame, encoded, DESK_PROTOCOL_HEADER_SIZE, &encoded_length) ==
        DESK_PROTOCOL_BUFFER_TOO_SMALL
    );
    assert(desk_protocol_decode(NULL, encoded_length, &decoded) == DESK_PROTOCOL_INVALID_ARGUMENT);
    assert(desk_protocol_decode(encoded, encoded_length, NULL) == DESK_PROTOCOL_INVALID_ARGUMENT);
}

static void test_protocol_payload_boundaries(void)
{
    uint8_t payload[DESK_PROTOCOL_MAX_PAYLOAD_SIZE];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)i;
    }

    const desk_protocol_frame_t source = {
        .message_type = DESK_MESSAGE_SYSTEM_STATE,
        .sequence = UINT16_MAX,
        .payload = payload,
        .payload_length = sizeof(payload),
    };
    uint8_t encoded[DESK_PROTOCOL_MAX_FRAME_SIZE];
    size_t encoded_length = 0;
    assert(desk_protocol_encode(&source, encoded, sizeof(encoded), &encoded_length) == DESK_PROTOCOL_OK);
    assert(encoded_length == sizeof(encoded));

    desk_protocol_frame_t decoded = {0};
    assert(desk_protocol_decode(encoded, encoded_length, &decoded) == DESK_PROTOCOL_OK);
    assert(decoded.payload_length == sizeof(payload));
    assert(memcmp(decoded.payload, payload, sizeof(payload)) == 0);

    const desk_protocol_frame_t empty = {
        .message_type = DESK_MESSAGE_HEARTBEAT,
        .sequence = 0,
    };
    assert(desk_protocol_encode(&empty, encoded, sizeof(encoded), &encoded_length) == DESK_PROTOCOL_OK);
    assert(encoded_length == DESK_PROTOCOL_HEADER_SIZE + DESK_PROTOCOL_CRC_SIZE);
    assert(desk_protocol_decode(encoded, encoded_length, &decoded) == DESK_PROTOCOL_OK);
    assert(decoded.payload_length == 0);
}

static void test_sequence_window(void)
{
    desk_sequence_window_t window = {0};
    assert(desk_sequence_window_accept(&window, 100) == DESK_SEQUENCE_ACCEPTED);
    assert(desk_sequence_window_accept(&window, 100) == DESK_SEQUENCE_DUPLICATE);
    assert(desk_sequence_window_accept(&window, 102) == DESK_SEQUENCE_ACCEPTED);
    assert(desk_sequence_window_accept(&window, 101) == DESK_SEQUENCE_ACCEPTED);
    assert(desk_sequence_window_accept(&window, 101) == DESK_SEQUENCE_DUPLICATE);

    assert(desk_sequence_window_accept(&window, 134) == DESK_SEQUENCE_ACCEPTED);
    assert(desk_sequence_window_accept(&window, 102) == DESK_SEQUENCE_TOO_OLD);
    assert(desk_sequence_window_accept(&window, 103) == DESK_SEQUENCE_ACCEPTED);
    assert(desk_sequence_window_accept(&window, 103) == DESK_SEQUENCE_DUPLICATE);

    desk_sequence_window_t wraparound = {0};
    assert(desk_sequence_window_accept(&wraparound, UINT16_MAX - 1U) == DESK_SEQUENCE_ACCEPTED);
    assert(desk_sequence_window_accept(&wraparound, 1) == DESK_SEQUENCE_ACCEPTED);
    assert(desk_sequence_window_accept(&wraparound, UINT16_MAX) == DESK_SEQUENCE_ACCEPTED);
    assert(desk_sequence_window_accept(&wraparound, 0) == DESK_SEQUENCE_ACCEPTED);
    assert(desk_sequence_window_accept(&wraparound, UINT16_MAX) == DESK_SEQUENCE_DUPLICATE);

    assert(desk_sequence_window_accept(NULL, 1) == DESK_SEQUENCE_TOO_OLD);
}

static void test_private_data_clear(void)
{
    desk_app_state_t state;
    desk_app_state_load_mock(&state);
    assert(state.weather.valid);
    assert(state.market.valid);
    assert(state.system.valid);
    assert(state.ai.valid);
    assert(state.media.valid);

    const uint32_t revision = state.revision;
    desk_app_state_clear_private(&state);
    assert(state.weather.valid);
    assert(state.market.valid);
    assert(!state.system.valid);
    assert(!state.ai.valid);
    assert(!state.media.valid);
    assert(state.control.active_app[0] == '\0');
    assert(!state.connection.mac_authenticated);
    assert(state.revision == revision + 1);
}

static void test_log_policy(void)
{
    char output[128];
    assert(
        desk_log_normalize_message(
            DESK_LOG_CONTENT_PUBLIC,
            "first\nsecond\tthird\x7f",
            output,
            sizeof(output)
        ) == strlen("first second third ")
    );
    assert(strcmp(output, "first second third ") == 0);

    assert(
        desk_log_normalize_message(
            DESK_LOG_CONTENT_PRIVATE,
            "secret notification text",
            output,
            sizeof(output)
        ) == strlen("[private event omitted]")
    );
    assert(strcmp(output, "[private event omitted]") == 0);
    assert(strstr(output, "secret") == NULL);

    char short_output[5];
    assert(desk_log_normalize_message(DESK_LOG_CONTENT_PUBLIC, "abcdef", short_output, sizeof(short_output)) == 4);
    assert(strcmp(short_output, "abcd") == 0);
    assert(desk_log_normalize_message(DESK_LOG_CONTENT_PUBLIC, "x", NULL, 0) == 0);

    assert(!desk_log_should_rotate(100, 20, 1000));
    assert(desk_log_should_rotate(980, 20, 1000));
    assert(desk_log_should_rotate(UINT64_MAX - 2U, 10, UINT64_MAX));
    assert(desk_log_should_rotate(0, 0, 0));

    char bucket[DESK_LOG_BUCKET_KEY_SIZE];
    assert(desk_log_bucket_key(0, bucket));
    assert(strcmp(bucket, "boot") == 0);
    assert(desk_log_bucket_key(1735689600U, bucket));
    assert(strlen(bucket) == 8);
    for (size_t i = 0; i < 8; ++i) {
        assert(bucket[i] >= '0' && bucket[i] <= '9');
    }

    assert(desk_log_make_filename("/logs", 0, 0x1234ABCDU, 2, output, sizeof(output)));
    assert(strcmp(output, "/logs/desk-boot-1234abcd-002.log") == 0);
    assert(!desk_log_make_filename("/logs", 0, 1, 0, short_output, sizeof(short_output)));
    assert(!desk_log_make_filename(NULL, 0, 1, 0, output, sizeof(output)));
}

static void test_qweather_parsers(void)
{
    desk_weather_state_t state = {0};
    static const char current[] =
        "{\"condition\":{\"text\":\"多云\",\"code\":\"102\"},"
        "\"temperature\":{\"value\":31.7,\"unit\":\"°C\"},"
        "\"feelsLike\":{\"value\":33.6,\"unit\":\"°C\"},\"humidity\":0.69}";
    assert(desk_qweather_parse_current(current, strlen(current), &state));
    assert(state.current_c == 32);
    assert(state.feels_like_c == 34);
    assert(state.humidity_percent == 69);
    assert(state.code == DESK_WEATHER_CLOUDY);

    static const char hourly[] =
        "{\"hours\":["
        "{\"forecastTime\":\"2026-08-18T09:00+08:00\",\"condition\":{\"code\":\"100\"},\"temperature\":{\"value\":28},\"precipitation\":{\"probability\":0.1}},"
        "{\"forecastTime\":\"2026-08-18T10:00+08:00\",\"condition\":{\"code\":\"101\"},\"temperature\":{\"value\":29},\"precipitation\":{\"probability\":0.2}},"
        "{\"forecastTime\":\"2026-08-18T11:00+08:00\",\"condition\":{\"code\":\"305\"},\"temperature\":{\"value\":30},\"precipitation\":{\"probability\":0.3}},"
        "{\"forecastTime\":\"2026-08-18T12:00+08:00\",\"condition\":{\"code\":\"302\"},\"temperature\":{\"value\":31},\"precipitation\":{\"probability\":0.4}},"
        "{\"forecastTime\":\"2026-08-18T13:00+08:00\",\"condition\":{\"code\":\"400\"},\"temperature\":{\"value\":32},\"precipitation\":{\"probability\":0.5}},"
        "{\"forecastTime\":\"2026-08-18T14:00+08:00\",\"condition\":{\"code\":\"501\"},\"temperature\":{\"value\":33},\"precipitation\":{\"probability\":0.6}}]}";
    assert(desk_qweather_parse_hourly(hourly, strlen(hourly), &state));
    assert(state.hourly[0].hour == 9);
    assert(state.hourly[2].precipitation_percent == 30);
    assert(state.hourly[3].code == DESK_WEATHER_STORM);
    assert(state.hourly[5].code == DESK_WEATHER_FOG);

    static const char daily[] =
        "{\"days\":["
        "{\"temperatureMin\":{\"value\":24.2},\"temperatureMax\":{\"value\":32.8},\"daytime\":{\"condition\":{\"code\":\"305\"},\"precipitation\":{\"probability\":0.72}}},"
        "{\"temperatureMin\":{\"value\":23.8},\"temperatureMax\":{\"value\":31.1},\"daytime\":{\"condition\":{\"code\":\"101\"},\"precipitation\":{\"probability\":0.25}}}]}";
    assert(desk_qweather_parse_daily(daily, strlen(daily), &state));
    assert(state.today.low_c == 24);
    assert(state.today.high_c == 33);
    assert(state.today.precipitation_percent == 72);
    assert(state.tomorrow.code == DESK_WEATHER_CLOUDY);

    static const char alerts[] =
        "{\"metadata\":{\"zeroResult\":false},\"alerts\":["
        "{\"messageType\":{\"code\":\"alert\"},\"severity\":\"moderate\",\"color\":{\"code\":\"yellow\"},\"headline\":\"暴雨黄色预警\"},"
        "{\"messageType\":{\"code\":\"update\"},\"severity\":\"severe\",\"color\":{\"code\":\"orange\"},\"headline\":\"台风橙色预警\"}]}";
    assert(desk_qweather_parse_alerts(alerts, strlen(alerts), &state));
    assert(state.alert.active);
    assert(state.alert.level == DESK_ALERT_ORANGE);
    assert(strcmp(state.alert.title, "台风橙色预警") == 0);

    static const char no_alerts[] = "{\"metadata\":{\"zeroResult\":true},\"alerts\":[]}";
    assert(desk_qweather_parse_alerts(no_alerts, strlen(no_alerts), &state));
    assert(!state.alert.active);
    assert(!desk_qweather_parse_current("{} trailing", strlen("{} trailing"), &state));
}

static void test_market_parsers(void)
{
    desk_market_state_t state = {0};
    static const char sse[] =
        "{\"date\":20260818,\"time\":94949,\"total\":4,\"list\":["
        "[3982.6535,3984.1985,0.04,\"000001\",\"上证指数\"],"
        "[1788.8502,1786.2657,-0.14,\"000688\",\"科创50\"],"
        "[4741.0981,4724.2981,-0.35,\"000300\",\"沪深300\"],"
        "[8184.6407,8154.8942,-0.36,\"000905\",\"中证500\"]]}";
    assert(desk_market_parse_sse(sse, strlen(sse), &state));
    assert(state.indices[0].points_x100 == 398420);
    assert(state.indices[0].change_basis_points == 4);
    assert(state.indices[3].change_basis_points == -14);
    assert(strcmp(state.indices[4].name, "沪深300") == 0);

    static const char szse[] =
        "{\"datetime\":\"2026-08-18 09:52\",\"code\":\"0\",\"data\":{"
        "\"code\":\"399001\",\"name\":\"深证成指\",\"close\":\"14704.27\","
        "\"now\":\"14608.93\",\"deltaPercent\":\"-0.65\",\"picupdata\":[[\"09:30\",\"14692.03\"]]}}";
    assert(desk_market_parse_szse(
        szse,
        strlen(szse),
        "399001",
        "深证成指",
        &state.indices[1]
    ));
    assert(state.indices[1].points_x100 == 1460893);
    assert(state.indices[1].change_basis_points == -65);
    assert(!desk_market_parse_szse(szse, strlen(szse), "399006", "创业板指", &state.indices[2]));
    assert(!desk_market_parse_sse("{} trailing", strlen("{} trailing"), &state));
}

int main(void)
{
    test_privacy_happy_path();
    test_privacy_failure_paths();
    test_privacy_heartbeat_wraparound();
    test_protocol_codec();
    test_protocol_rejects_invalid_frames();
    test_protocol_payload_boundaries();
    test_sequence_window();
    test_private_data_clear();
    test_log_policy();
    test_qweather_parsers();
    test_market_parsers();
    puts("Host tests passed");
    return 0;
}
