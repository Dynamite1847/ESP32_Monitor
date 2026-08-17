#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DESK_PRIVACY_LOCKED = 0,
    DESK_PRIVACY_AUTHENTICATING,
    DESK_PRIVACY_ACTIVE,
} desk_privacy_state_t;

typedef enum {
    DESK_PRIVACY_EVENT_BOOT = 0,
    DESK_PRIVACY_EVENT_BLE_CONNECTED,
    DESK_PRIVACY_EVENT_AUTH_SUCCEEDED,
    DESK_PRIVACY_EVENT_AUTH_FAILED,
    DESK_PRIVACY_EVENT_HEARTBEAT,
    DESK_PRIVACY_EVENT_HEARTBEAT_TIMEOUT,
    DESK_PRIVACY_EVENT_BLE_DISCONNECTED,
    DESK_PRIVACY_EVENT_MAC_LOCKED,
    DESK_PRIVACY_EVENT_MAC_SLEEPING,
} desk_privacy_event_t;

typedef uint32_t desk_privacy_actions_t;

enum {
    DESK_PRIVACY_ACTION_NONE = 0,
    DESK_PRIVACY_ACTION_BACKLIGHT_ON = 1U << 0,
    DESK_PRIVACY_ACTION_BACKLIGHT_OFF = 1U << 1,
    DESK_PRIVACY_ACTION_TOUCH_ENABLE = 1U << 2,
    DESK_PRIVACY_ACTION_TOUCH_DISABLE = 1U << 3,
    DESK_PRIVACY_ACTION_CLEAR_PRIVATE_DATA = 1U << 4,
    DESK_PRIVACY_ACTION_SHOW_HOME = 1U << 5,
    DESK_PRIVACY_ACTION_DISCONNECT_BLE = 1U << 6,
};

typedef struct {
    desk_privacy_state_t state;
    uint32_t heartbeat_timeout_ms;
    uint32_t last_heartbeat_ms;
} desk_privacy_context_t;

void desk_privacy_init(desk_privacy_context_t *context, uint32_t heartbeat_timeout_ms);

desk_privacy_actions_t desk_privacy_dispatch(
    desk_privacy_context_t *context,
    desk_privacy_event_t event,
    uint32_t now_ms
);

desk_privacy_actions_t desk_privacy_poll(desk_privacy_context_t *context, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
