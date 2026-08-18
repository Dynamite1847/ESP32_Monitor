#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "storage/log_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DESK_STORAGE_MOUNT_POINT "/sdcard"
#define DESK_STORAGE_ROOT_DIR DESK_STORAGE_MOUNT_POINT "/desk_console"
#define DESK_STORAGE_LOG_DIR DESK_STORAGE_ROOT_DIR "/logs"
#define DESK_STORAGE_SCREENSHOT_DIR DESK_STORAGE_ROOT_DIR "/screenshots"
#define DESK_STORAGE_ASSET_DIR DESK_STORAGE_ROOT_DIR "/assets"

typedef enum {
    DESK_STORAGE_STOPPED = 0,
    DESK_STORAGE_MOUNTING,
    DESK_STORAGE_READY,
    DESK_STORAGE_RETRY_WAIT,
} desk_storage_state_t;

typedef enum {
    DESK_LOG_DEBUG = 0,
    DESK_LOG_INFO,
    DESK_LOG_WARNING,
    DESK_LOG_ERROR,
} desk_log_level_t;

typedef struct {
    desk_storage_state_t state;
    bool mounted;
    esp_err_t last_error;
    uint32_t queued_entries;
    uint32_t written_entries;
    uint32_t dropped_entries;
} desk_storage_status_t;

/** Starts the SD mount/retry and low-priority log writer task. Idempotent. */
esp_err_t desk_storage_start(void);

desk_storage_status_t desk_storage_get_status(void);

/**
 * Queues one log entry without waiting for the card. Callers must classify the
 * content; DESK_LOG_CONTENT_PRIVATE replaces the formatted message before it
 * reaches the queue. Never pass credentials or raw protocol payloads as tags.
 */
bool desk_storage_log(
    desk_log_level_t level,
    desk_log_content_class_t content_class,
    const char *tag,
    const char *format,
    ...
) __attribute__((format(printf, 4, 5)));

#ifdef __cplusplus
}
#endif
