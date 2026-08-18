#include "storage/log_policy.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

enum {
    VALID_CLOCK_MINIMUM_EPOCH = 1735689600U,
};

size_t desk_log_normalize_message(
    desk_log_content_class_t content_class,
    const char *input,
    char *output,
    size_t output_capacity
)
{
    if (output == NULL || output_capacity == 0) {
        return 0;
    }

    if (content_class == DESK_LOG_CONTENT_PRIVATE) {
        static const char marker[] = "[private event omitted]";
        const size_t length = sizeof(marker) - 1U < output_capacity - 1U
                                  ? sizeof(marker) - 1U
                                  : output_capacity - 1U;
        memcpy(output, marker, length);
        output[length] = '\0';
        return length;
    }

    if (input == NULL) {
        output[0] = '\0';
        return 0;
    }

    size_t length = 0;
    while (*input != '\0' && length + 1U < output_capacity) {
        const unsigned char byte = (unsigned char)*input++;
        output[length++] = byte < 0x20U || byte == 0x7FU ? ' ' : (char)byte;
    }
    output[length] = '\0';
    return length;
}

bool desk_log_should_rotate(uint64_t current_bytes, size_t pending_bytes, uint64_t max_bytes)
{
    if (max_bytes == 0 || current_bytes >= max_bytes) {
        return true;
    }
    return (uint64_t)pending_bytes >= max_bytes - current_bytes;
}

bool desk_log_bucket_key(uint32_t epoch, char output[DESK_LOG_BUCKET_KEY_SIZE])
{
    if (output == NULL) {
        return false;
    }
    if (epoch < VALID_CLOCK_MINIMUM_EPOCH) {
        memcpy(output, "boot", sizeof("boot"));
        return true;
    }

    const time_t timestamp = (time_t)epoch;
    struct tm local_time = {0};
    if (localtime_r(&timestamp, &local_time) == NULL) {
        return false;
    }
    const int written = snprintf(
        output,
        DESK_LOG_BUCKET_KEY_SIZE,
        "%04d%02d%02d",
        local_time.tm_year + 1900,
        local_time.tm_mon + 1,
        local_time.tm_mday
    );
    return written == 8;
}

bool desk_log_make_filename(
    const char *directory,
    uint32_t epoch,
    uint32_t boot_id,
    unsigned segment,
    char *output,
    size_t output_capacity
)
{
    if (directory == NULL || directory[0] == '\0' || output == NULL || output_capacity == 0) {
        return false;
    }

    char bucket[DESK_LOG_BUCKET_KEY_SIZE];
    if (!desk_log_bucket_key(epoch, bucket)) {
        return false;
    }
    const int written = snprintf(
        output,
        output_capacity,
        "%s/desk-%s-%08lx-%03u.log",
        directory,
        bucket,
        (unsigned long)boot_id,
        segment
    );
    return written > 0 && (size_t)written < output_capacity;
}
