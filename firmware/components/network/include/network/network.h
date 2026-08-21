#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DESK_WIFI_SSID_MAX_BYTES 32
#define DESK_WIFI_PASSWORD_MAX_BYTES 63

typedef enum {
    DESK_NETWORK_EVENT_STARTED = 0,
    DESK_NETWORK_EVENT_CONNECTED,
    DESK_NETWORK_EVENT_DISCONNECTED,
    DESK_NETWORK_EVENT_RETRY_EXHAUSTED,
} desk_network_event_type_t;

typedef struct {
    desk_network_event_type_t type;
    esp_err_t status;
    int8_t rssi_dbm;
    uint8_t disconnect_reason;
    uint32_t ipv4_address;
    char ipv4[16];
} desk_network_event_t;

typedef struct {
    bool connected;
    bool credentials_available;
    uint32_t reconnect_attempts;
    uint8_t last_disconnect_reason;
    char ssid[DESK_WIFI_SSID_MAX_BYTES + 1];
} desk_network_status_t;

/** Starts Wi-Fi station mode and reconnects using credentials retained by ESP-IDF. */
esp_err_t desk_network_start(void);

/**
 * Stores a 2.4GHz personal-network configuration in NVS and starts connecting.
 * Password may be empty for an open network, otherwise it must contain 8-63 bytes.
 */
esp_err_t desk_network_provision(
    const char *ssid,
    size_t ssid_length,
    const char *password,
    size_t password_length
);

/** Immediately retries the saved station configuration. */
esp_err_t desk_network_reconnect(void);

/** Returns a lock-protected snapshot of connection and retry state. */
desk_network_status_t desk_network_get_status(void);

/** Receives one public connection-state event. */
bool desk_network_receive_event(desk_network_event_t *event, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
