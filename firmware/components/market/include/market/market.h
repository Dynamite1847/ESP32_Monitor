#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_model/app_model.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DESK_MARKET_EVENT_UPDATED = 0,
    DESK_MARKET_EVENT_FETCH_FAILED,
} desk_market_event_type_t;

typedef struct {
    desk_market_event_type_t type;
    esp_err_t status;
    desk_market_state_t state;
} desk_market_event_t;

/** Starts the official-exchange A-share index worker. */
esp_err_t desk_market_start(void);

void desk_market_set_network_available(bool available);
void desk_market_request_refresh(void);
bool desk_market_receive_event(desk_market_event_t *event, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
