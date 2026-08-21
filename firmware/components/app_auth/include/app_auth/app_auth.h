#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DESK_AUTH_PAYLOAD_VERSION 1U
#define DESK_AUTH_NONCE_SIZE 16U
#define DESK_AUTH_SESSION_ID_SIZE 8U
#define DESK_AUTH_SHARED_KEY_SIZE 32U
#define DESK_AUTH_HMAC_SIZE 32U
#define DESK_AUTH_HELLO_SIZE (2U + DESK_AUTH_NONCE_SIZE)
#define DESK_AUTH_CHALLENGE_SIZE (2U + DESK_AUTH_NONCE_SIZE + DESK_AUTH_SESSION_ID_SIZE)
#define DESK_AUTH_ENROLLMENT_CHALLENGE_SIZE (DESK_AUTH_CHALLENGE_SIZE + DESK_AUTH_SHARED_KEY_SIZE)
#define DESK_AUTH_RESPONSE_SIZE (2U + DESK_AUTH_HMAC_SIZE)
#define DESK_AUTH_RESULT_SIZE 3U

typedef enum {
    DESK_AUTH_RESULT_FAILED = 0,
    DESK_AUTH_RESULT_SUCCEEDED = 1,
    DESK_AUTH_RESULT_ENROLLMENT_REQUIRED = 2,
    DESK_AUTH_RESULT_ENROLLMENT_CLOSED = 3,
    DESK_AUTH_RESULT_INVALID_REQUEST = 4,
    DESK_AUTH_RESULT_STORAGE_FAILED = 5,
} desk_auth_result_code_t;

/** Load the application shared key from NVS, if one has already been enrolled. */
esp_err_t desk_app_auth_init(void);

/** Clear per-connection challenge state without deleting the enrolled key. */
void desk_app_auth_reset_session(void);

bool desk_app_auth_has_shared_key(void);

/** Allow one replacement key to be enrolled by the already bonded Mac. */
bool desk_app_auth_migration_enrollment_pending(void);

/**
 * Start a challenge from a HELLO payload. When enrollment is requested and no
 * key exists, allow_enrollment controls whether a new key may be generated.
 */
desk_auth_result_code_t desk_app_auth_begin(
    const uint8_t *hello,
    size_t hello_length,
    bool encrypted,
    bool allow_enrollment,
    uint8_t *challenge,
    size_t challenge_capacity,
    size_t *challenge_length
);

/** Verify AUTH_RESPONSE and persist a newly enrolled key after a valid HMAC. */
desk_auth_result_code_t desk_app_auth_verify(
    const uint8_t *response,
    size_t response_length,
    bool *enrolled
);

#ifdef __cplusplus
}
#endif
