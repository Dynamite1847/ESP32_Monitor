#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    const char *value;
} desk_http_header_t;

/** Creates the shared request lock used to avoid concurrent TLS handshakes. */
esp_err_t desk_http_fetch_init(void);

/**
 * Performs one certificate-verified HTTPS GET and returns a NUL-terminated
 * PSRAM buffer. Redirects, non-200 responses, empty bodies and oversized
 * responses are rejected. The caller owns the returned buffer and frees it
 * with desk_http_response_free().
 */
esp_err_t desk_http_get_json(
    const char *url,
    const desk_http_header_t *headers,
    size_t header_count,
    size_t response_capacity,
    int timeout_ms,
    char **response,
    size_t *response_length
);

void desk_http_response_free(char *response);

#ifdef __cplusplus
}
#endif
