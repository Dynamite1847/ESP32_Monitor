#include "privacy/privacy_state_machine.h"

#include <stddef.h>

static const desk_privacy_actions_t LOCK_ACTIONS =
    DESK_PRIVACY_ACTION_BACKLIGHT_OFF |
    DESK_PRIVACY_ACTION_TOUCH_DISABLE |
    DESK_PRIVACY_ACTION_CLEAR_PRIVATE_DATA;

static void enter_locked(desk_privacy_context_t *context)
{
    context->state = DESK_PRIVACY_LOCKED;
    context->last_heartbeat_ms = 0;
}

void desk_privacy_init(desk_privacy_context_t *context, uint32_t heartbeat_timeout_ms)
{
    if (context == NULL) {
        return;
    }
    context->state = DESK_PRIVACY_LOCKED;
    context->heartbeat_timeout_ms = heartbeat_timeout_ms;
    context->last_heartbeat_ms = 0;
}

desk_privacy_actions_t desk_privacy_dispatch(
    desk_privacy_context_t *context,
    desk_privacy_event_t event,
    uint32_t now_ms
)
{
    if (context == NULL) {
        return DESK_PRIVACY_ACTION_NONE;
    }

    switch (event) {
        case DESK_PRIVACY_EVENT_BOOT:
            enter_locked(context);
            return LOCK_ACTIONS;

        case DESK_PRIVACY_EVENT_BLE_CONNECTED:
            context->state = DESK_PRIVACY_AUTHENTICATING;
            context->last_heartbeat_ms = 0;
            return DESK_PRIVACY_ACTION_BACKLIGHT_OFF | DESK_PRIVACY_ACTION_TOUCH_DISABLE;

        case DESK_PRIVACY_EVENT_AUTH_SUCCEEDED:
            if (context->state != DESK_PRIVACY_AUTHENTICATING) {
                return DESK_PRIVACY_ACTION_NONE;
            }
            context->state = DESK_PRIVACY_ACTIVE;
            context->last_heartbeat_ms = now_ms;
            return DESK_PRIVACY_ACTION_BACKLIGHT_ON |
                   DESK_PRIVACY_ACTION_TOUCH_ENABLE |
                   DESK_PRIVACY_ACTION_SHOW_HOME;

        case DESK_PRIVACY_EVENT_AUTH_FAILED:
            enter_locked(context);
            return LOCK_ACTIONS | DESK_PRIVACY_ACTION_DISCONNECT_BLE;

        case DESK_PRIVACY_EVENT_HEARTBEAT:
            if (context->state == DESK_PRIVACY_ACTIVE) {
                context->last_heartbeat_ms = now_ms;
            }
            return DESK_PRIVACY_ACTION_NONE;

        case DESK_PRIVACY_EVENT_HEARTBEAT_TIMEOUT:
        case DESK_PRIVACY_EVENT_MAC_SLEEPING:
            enter_locked(context);
            return LOCK_ACTIONS | DESK_PRIVACY_ACTION_DISCONNECT_BLE;

        case DESK_PRIVACY_EVENT_BLE_DISCONNECTED:
        case DESK_PRIVACY_EVENT_MAC_LOCKED:
            enter_locked(context);
            return LOCK_ACTIONS;

        default:
            return DESK_PRIVACY_ACTION_NONE;
    }
}

desk_privacy_actions_t desk_privacy_poll(desk_privacy_context_t *context, uint32_t now_ms)
{
    if (context == NULL || context->state != DESK_PRIVACY_ACTIVE || context->heartbeat_timeout_ms == 0) {
        return DESK_PRIVACY_ACTION_NONE;
    }

    if ((uint32_t)(now_ms - context->last_heartbeat_ms) <= context->heartbeat_timeout_ms) {
        return DESK_PRIVACY_ACTION_NONE;
    }

    return desk_privacy_dispatch(context, DESK_PRIVACY_EVENT_HEARTBEAT_TIMEOUT, now_ms);
}
