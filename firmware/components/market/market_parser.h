#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_model/app_model.h"

/** Parses the official SSE multi-index snapshot into slots 0, 3, 4 and 5. */
bool desk_market_parse_sse(const char *json, size_t length, desk_market_state_t *state);

/** Parses one official SZSE snapshot into the supplied fixed slot. */
bool desk_market_parse_szse(
    const char *json,
    size_t length,
    const char *expected_code,
    const char *display_name,
    desk_market_index_t *index
);
