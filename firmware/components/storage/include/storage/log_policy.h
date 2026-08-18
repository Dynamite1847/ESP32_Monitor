#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DESK_LOG_BUCKET_KEY_SIZE 16

typedef enum {
    DESK_LOG_CONTENT_PUBLIC = 0,
    DESK_LOG_CONTENT_PRIVATE,
} desk_log_content_class_t;

/**
 * Converts one log message to a single safe line. Private content is replaced
 * with a fixed marker so notification text, media titles, protocol payloads
 * and credentials are never written to the card.
 */
size_t desk_log_normalize_message(
    desk_log_content_class_t content_class,
    const char *input,
    char *output,
    size_t output_capacity
);

/** Returns true when appending pending_bytes would meet or exceed max_bytes. */
bool desk_log_should_rotate(uint64_t current_bytes, size_t pending_bytes, uint64_t max_bytes);

/** Creates YYYYMMDD after time sync, or "boot" while the RTC is invalid. */
bool desk_log_bucket_key(uint32_t epoch, char output[DESK_LOG_BUCKET_KEY_SIZE]);

/** Builds a collision-resistant FAT-compatible log path. */
bool desk_log_make_filename(
    const char *directory,
    uint32_t epoch,
    uint32_t boot_id,
    unsigned segment,
    char *output,
    size_t output_capacity
);

#ifdef __cplusplus
}
#endif
