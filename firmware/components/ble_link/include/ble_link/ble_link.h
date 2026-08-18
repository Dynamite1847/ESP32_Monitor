#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_protocol/ble_protocol.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DESK_BLE_DEVICE_NAME "Desk Console 4.3"
#define DESK_BLE_FRAGMENT_HEADER_SIZE 4

typedef enum {
    DESK_BLE_EVENT_READY = 0,
    DESK_BLE_EVENT_CONNECTED,
    DESK_BLE_EVENT_DISCONNECTED,
    DESK_BLE_EVENT_SECURITY_CHANGED,
    DESK_BLE_EVENT_SUBSCRIPTION_CHANGED,
    DESK_BLE_EVENT_MTU_CHANGED,
    DESK_BLE_EVENT_FRAME_RECEIVED,
    DESK_BLE_EVENT_TRANSPORT_ERROR,
} desk_ble_event_type_t;

typedef struct {
    desk_ble_event_type_t type;
    uint16_t connection_handle;
    uint16_t mtu;
    int status;
    bool encrypted;
    bool bonded;
    bool subscribed;
    size_t frame_length;
    uint8_t frame[DESK_PROTOCOL_MAX_FRAME_SIZE];
} desk_ble_event_t;

/** Start the single-connection NimBLE peripheral and its GATT service. */
esp_err_t desk_ble_link_start(void);

/** Receive one transport event. timeout_ms may be UINT32_MAX to wait forever. */
bool desk_ble_link_receive_event(desk_ble_event_t *event, uint32_t timeout_ms);

/** Send one already encoded logical protocol frame through the notify characteristic. */
esp_err_t desk_ble_link_send_frame(const uint8_t *frame, size_t frame_length);

/** Disconnect the current central, if connected. */
esp_err_t desk_ble_link_disconnect(void);

#ifdef __cplusplus
}
#endif
