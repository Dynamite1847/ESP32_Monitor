#include "app_auth/app_auth.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/md.h"
#include "nvs.h"

static const char *TAG = "desk_auth";
static const char *NVS_NAMESPACE = "desk_auth";
static const char *NVS_SHARED_KEY = "shared_key";
static const char *NVS_STORAGE_REVISION = "store_rev";
static const char *NVS_MIGRATION_ENROLLMENT = "migrate_enroll";
static const uint32_t STORAGE_REVISION = 4U;
static const uint8_t HMAC_DOMAIN[] = "desk-console-auth-v1";

enum {
    HELLO_FLAG_REQUEST_ENROLLMENT = 1U << 0,
    CHALLENGE_FLAG_CONTAINS_ENROLLMENT_KEY = 1U << 0,
};

typedef struct {
    bool initialized;
    bool has_shared_key;
    bool migration_enrollment_pending;
    bool challenge_active;
    bool pending_enrollment;
    nvs_handle_t nvs;
    uint8_t shared_key[DESK_AUTH_SHARED_KEY_SIZE];
    uint8_t pending_key[DESK_AUTH_SHARED_KEY_SIZE];
    uint8_t client_nonce[DESK_AUTH_NONCE_SIZE];
    uint8_t device_nonce[DESK_AUTH_NONCE_SIZE];
    uint8_t session_id[DESK_AUTH_SESSION_ID_SIZE];
} app_auth_state_t;

static app_auth_state_t auth_state;

static void clear_session_material(void)
{
    memset(auth_state.pending_key, 0, sizeof(auth_state.pending_key));
    memset(auth_state.client_nonce, 0, sizeof(auth_state.client_nonce));
    memset(auth_state.device_nonce, 0, sizeof(auth_state.device_nonce));
    memset(auth_state.session_id, 0, sizeof(auth_state.session_id));
    auth_state.challenge_active = false;
    auth_state.pending_enrollment = false;
}

static bool constant_time_equal(const uint8_t *left, const uint8_t *right, size_t length)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < length; ++i) {
        difference |= left[i] ^ right[i];
    }
    return difference == 0;
}

static bool calculate_hmac(const uint8_t *key, uint8_t output[DESK_AUTH_HMAC_SIZE])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) {
        return false;
    }

    mbedtls_md_context_t context;
    mbedtls_md_init(&context);
    int result = mbedtls_md_setup(&context, info, 1);
    if (result == 0) {
        result = mbedtls_md_hmac_starts(&context, key, DESK_AUTH_SHARED_KEY_SIZE);
    }
    if (result == 0) {
        result = mbedtls_md_hmac_update(&context, HMAC_DOMAIN, sizeof(HMAC_DOMAIN) - 1U);
    }
    if (result == 0) {
        result = mbedtls_md_hmac_update(
            &context,
            auth_state.client_nonce,
            sizeof(auth_state.client_nonce)
        );
    }
    if (result == 0) {
        result = mbedtls_md_hmac_update(
            &context,
            auth_state.device_nonce,
            sizeof(auth_state.device_nonce)
        );
    }
    if (result == 0) {
        result = mbedtls_md_hmac_update(&context, auth_state.session_id, sizeof(auth_state.session_id));
    }
    if (result == 0) {
        result = mbedtls_md_hmac_finish(&context, output);
    }
    mbedtls_md_free(&context);
    return result == 0;
}

esp_err_t desk_app_auth_init(void)
{
    if (auth_state.initialized) {
        return ESP_OK;
    }

    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &auth_state.nvs);
    if (result != ESP_OK) {
        return result;
    }

    uint32_t stored_revision = 0U;
    result = nvs_get_u32(auth_state.nvs, NVS_STORAGE_REVISION, &stored_revision);
    if (result == ESP_ERR_NVS_NOT_FOUND || stored_revision < STORAGE_REVISION) {
        /* Revision 4 rotates the application key once after removing the Mac
         * FileProtection option that made the local copy unreadable after a
         * restart. BLE bonding, Wi-Fi and all other NVS namespaces stay intact. */
        esp_err_t erase_result = nvs_erase_key(auth_state.nvs, NVS_SHARED_KEY);
        if (erase_result != ESP_OK && erase_result != ESP_ERR_NVS_NOT_FOUND) {
            return erase_result;
        }
        result = nvs_set_u32(auth_state.nvs, NVS_STORAGE_REVISION, STORAGE_REVISION);
        if (result == ESP_OK) {
            result = nvs_set_u8(auth_state.nvs, NVS_MIGRATION_ENROLLMENT, 1U);
        }
        if (result == ESP_OK) {
            result = nvs_commit(auth_state.nvs);
        }
        if (result != ESP_OK) {
            return result;
        }
        auth_state.migration_enrollment_pending = true;
        ESP_LOGI(TAG, "Application authentication storage migrated to revision %u", STORAGE_REVISION);
    } else if (result != ESP_OK) {
        return result;
    } else {
        uint8_t migration_pending = 0U;
        result = nvs_get_u8(auth_state.nvs, NVS_MIGRATION_ENROLLMENT, &migration_pending);
        if (result == ESP_ERR_NVS_NOT_FOUND) {
            result = ESP_OK;
        } else if (result != ESP_OK) {
            return result;
        }
        auth_state.migration_enrollment_pending = migration_pending == 1U;
    }

    size_t key_length = sizeof(auth_state.shared_key);
    result = nvs_get_blob(auth_state.nvs, NVS_SHARED_KEY, auth_state.shared_key, &key_length);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        memset(auth_state.shared_key, 0, sizeof(auth_state.shared_key));
        auth_state.has_shared_key = false;
        result = ESP_OK;
    } else if (result == ESP_OK && key_length == sizeof(auth_state.shared_key)) {
        auth_state.has_shared_key = true;
    } else if (result == ESP_OK) {
        ESP_LOGE(TAG, "Stored shared key has invalid length %u", (unsigned)key_length);
        result = ESP_ERR_INVALID_SIZE;
    }

    if (result == ESP_OK) {
        auth_state.initialized = true;
        clear_session_material();
        ESP_LOGI(TAG, "Application key is %s", auth_state.has_shared_key ? "enrolled" : "not enrolled");
    }
    return result;
}

void desk_app_auth_reset_session(void)
{
    clear_session_material();
}

bool desk_app_auth_has_shared_key(void)
{
    return auth_state.has_shared_key;
}

bool desk_app_auth_migration_enrollment_pending(void)
{
    return auth_state.migration_enrollment_pending;
}

desk_auth_result_code_t desk_app_auth_begin(
    const uint8_t *hello,
    size_t hello_length,
    bool encrypted,
    bool allow_enrollment,
    uint8_t *challenge,
    size_t challenge_capacity,
    size_t *challenge_length
)
{
    if (!auth_state.initialized || hello == NULL || challenge == NULL || challenge_length == NULL ||
        hello_length != DESK_AUTH_HELLO_SIZE || hello[0] != DESK_AUTH_PAYLOAD_VERSION || !encrypted) {
        return DESK_AUTH_RESULT_INVALID_REQUEST;
    }

    clear_session_material();
    const bool enrollment_requested = (hello[1] & HELLO_FLAG_REQUEST_ENROLLMENT) != 0;
    if (!auth_state.has_shared_key) {
        if (!enrollment_requested) {
            return DESK_AUTH_RESULT_ENROLLMENT_REQUIRED;
        }
        if (!allow_enrollment) {
            return DESK_AUTH_RESULT_ENROLLMENT_CLOSED;
        }
        auth_state.pending_enrollment = true;
        esp_fill_random(auth_state.pending_key, sizeof(auth_state.pending_key));
    }

    const size_t required_size = auth_state.pending_enrollment
                                     ? DESK_AUTH_ENROLLMENT_CHALLENGE_SIZE
                                     : DESK_AUTH_CHALLENGE_SIZE;
    if (challenge_capacity < required_size) {
        clear_session_material();
        return DESK_AUTH_RESULT_INVALID_REQUEST;
    }

    memcpy(auth_state.client_nonce, &hello[2], sizeof(auth_state.client_nonce));
    esp_fill_random(auth_state.device_nonce, sizeof(auth_state.device_nonce));
    esp_fill_random(auth_state.session_id, sizeof(auth_state.session_id));

    challenge[0] = DESK_AUTH_PAYLOAD_VERSION;
    challenge[1] = auth_state.pending_enrollment ? CHALLENGE_FLAG_CONTAINS_ENROLLMENT_KEY : 0;
    memcpy(&challenge[2], auth_state.device_nonce, sizeof(auth_state.device_nonce));
    memcpy(&challenge[2 + DESK_AUTH_NONCE_SIZE], auth_state.session_id, sizeof(auth_state.session_id));
    if (auth_state.pending_enrollment) {
        memcpy(&challenge[DESK_AUTH_CHALLENGE_SIZE], auth_state.pending_key, sizeof(auth_state.pending_key));
    }

    auth_state.challenge_active = true;
    *challenge_length = required_size;
    return DESK_AUTH_RESULT_SUCCEEDED;
}

desk_auth_result_code_t desk_app_auth_verify(
    const uint8_t *response,
    size_t response_length,
    bool *enrolled
)
{
    if (enrolled != NULL) {
        *enrolled = false;
    }
    if (!auth_state.challenge_active || response == NULL ||
        response_length != DESK_AUTH_RESPONSE_SIZE || response[0] != DESK_AUTH_PAYLOAD_VERSION) {
        clear_session_material();
        return DESK_AUTH_RESULT_INVALID_REQUEST;
    }

    uint8_t expected[DESK_AUTH_HMAC_SIZE];
    const uint8_t *key = auth_state.pending_enrollment ? auth_state.pending_key : auth_state.shared_key;
    if (!calculate_hmac(key, expected) || !constant_time_equal(expected, &response[2], sizeof(expected))) {
        memset(expected, 0, sizeof(expected));
        clear_session_material();
        return DESK_AUTH_RESULT_FAILED;
    }
    memset(expected, 0, sizeof(expected));

    if (auth_state.pending_enrollment) {
        esp_err_t result = nvs_set_blob(
            auth_state.nvs,
            NVS_SHARED_KEY,
            auth_state.pending_key,
            sizeof(auth_state.pending_key)
        );
        if (result == ESP_OK && auth_state.migration_enrollment_pending) {
            result = nvs_erase_key(auth_state.nvs, NVS_MIGRATION_ENROLLMENT);
        }
        if (result == ESP_OK) {
            result = nvs_commit(auth_state.nvs);
        }
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Unable to save application key: %s", esp_err_to_name(result));
            clear_session_material();
            return DESK_AUTH_RESULT_STORAGE_FAILED;
        }
        memcpy(auth_state.shared_key, auth_state.pending_key, sizeof(auth_state.shared_key));
        auth_state.has_shared_key = true;
        auth_state.migration_enrollment_pending = false;
        if (enrolled != NULL) {
            *enrolled = true;
        }
    }

    clear_session_material();
    return DESK_AUTH_RESULT_SUCCEEDED;
}
