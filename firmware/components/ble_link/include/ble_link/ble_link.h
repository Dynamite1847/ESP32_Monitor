#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_protocol/ble_protocol.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DESK_BLE_DEVICE_NAME "Codex Micro"
#define DESK_BLE_FRAGMENT_HEADER_SIZE 4

typedef enum {
    DESK_BLE_EVENT_READY = 0,
    DESK_BLE_EVENT_CONNECTED,
    DESK_BLE_EVENT_DISCONNECTED,
    DESK_BLE_EVENT_SECURITY_CHANGED,
    DESK_BLE_EVENT_SUBSCRIPTION_CHANGED,
    DESK_BLE_EVENT_MTU_CHANGED,
    DESK_BLE_EVENT_FRAME_RECEIVED,
    DESK_BLE_EVENT_CODEX_TASK_STATUS,
    DESK_BLE_EVENT_TRANSPORT_ERROR,
} desk_ble_event_type_t;

typedef enum {
    DESK_CODEX_SLOT_UNASSIGNED = 0,
    DESK_CODEX_SLOT_IDLE,
    DESK_CODEX_SLOT_RUNNING,
    DESK_CODEX_SLOT_COMPLETED,
    DESK_CODEX_SLOT_WAITING,
    DESK_CODEX_SLOT_FAILED,
} desk_codex_slot_status_t;

typedef struct {
    desk_ble_event_type_t type;
    uint16_t connection_handle;
    uint16_t mtu;
    int status;
    bool encrypted;
    bool bonded;
    bool subscribed;
    bool codex_subscribed;
    uint8_t codex_slot;
    desk_codex_slot_status_t codex_status;
    size_t frame_length;
    uint8_t frame[DESK_PROTOCOL_MAX_FRAME_SIZE];
} desk_ble_event_t;

/** Start the single-connection NimBLE peripheral and its GATT service. */
esp_err_t desk_ble_link_start(void);

/** Receive one transport event. timeout_ms may be UINT32_MAX to wait forever. */
bool desk_ble_link_receive_event(desk_ble_event_t *event, uint32_t timeout_ms);

/** Send one already encoded logical protocol frame through the notify characteristic. */
esp_err_t desk_ble_link_send_frame(const uint8_t *frame, size_t frame_length);

/** Send one Codex Micro vendor-HID key action (0 release, 1 press, 2 step). */
esp_err_t desk_ble_link_codex_send_key(const char *key_id, uint8_t action, int8_t agent);

/** Send one Codex Micro four-way control event. Angle is normalized to 0..1. */
esp_err_t desk_ble_link_codex_send_joystick(float angle, float distance);

/** True once the Mac has subscribed to the Codex Micro input report. */
bool desk_ble_link_codex_ready(void);

/** Disconnect the current central, if connected. */
esp_err_t desk_ble_link_disconnect(void);

#ifdef __cplusplus
}
#endif
