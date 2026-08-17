#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_model/app_model.h"
#include "ble_protocol/ble_protocol.h"
#include "privacy/privacy_state_machine.h"

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

int main(void)
{
    test_privacy_happy_path();
    test_privacy_failure_paths();
    test_privacy_heartbeat_wraparound();
    test_protocol_codec();
    test_protocol_rejects_invalid_frames();
    test_private_data_clear();
    puts("Host tests passed");
    return 0;
}
