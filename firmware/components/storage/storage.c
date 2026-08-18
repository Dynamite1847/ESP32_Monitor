#include "storage/storage.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "board/board.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "desk_storage";

enum {
    SD_PIN_MOSI = 11,
    SD_PIN_MISO = 13,
    SD_PIN_CLOCK = 12,
    LOG_QUEUE_LENGTH = 32,
    LOG_TAG_CAPACITY = 24,
    LOG_MESSAGE_CAPACITY = 192,
    LOG_PATH_CAPACITY = 160,
    LOG_TASK_STACK_SIZE = 6144,
    LOG_FLUSH_ENTRY_COUNT = 8,
    LOG_FLUSH_INTERVAL_MS = 2000,
    MOUNT_RETRY_INTERVAL_MS = 10000,
    LOG_ROTATE_BYTES = 2 * 1024 * 1024,
};

typedef struct {
    uint32_t epoch;
    uint32_t uptime_ms;
    desk_log_level_t level;
    char tag[LOG_TAG_CAPACITY];
    char message[LOG_MESSAGE_CAPACITY];
} queued_log_entry_t;

typedef struct {
    sdmmc_host_t host;
    sdmmc_card_t *card;
    bool spi_initialized;
} sd_context_t;

static QueueHandle_t log_queue;
static TaskHandle_t storage_task_handle;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static desk_storage_status_t storage_status;

static void unmount_card(sd_context_t *context);

static const char *log_level_text(desk_log_level_t level)
{
    switch (level) {
        case DESK_LOG_DEBUG:
            return "DEBUG";
        case DESK_LOG_WARNING:
            return "WARN";
        case DESK_LOG_ERROR:
            return "ERROR";
        case DESK_LOG_INFO:
        default:
            return "INFO";
    }
}

static void set_storage_state(desk_storage_state_t state, bool mounted, esp_err_t error)
{
    taskENTER_CRITICAL(&status_lock);
    storage_status.state = state;
    storage_status.mounted = mounted;
    storage_status.last_error = error;
    taskEXIT_CRITICAL(&status_lock);
}

static void note_written_entry(void)
{
    taskENTER_CRITICAL(&status_lock);
    storage_status.written_entries++;
    taskEXIT_CRITICAL(&status_lock);
}

static void note_dropped_entry(void)
{
    taskENTER_CRITICAL(&status_lock);
    storage_status.dropped_entries++;
    taskEXIT_CRITICAL(&status_lock);
}

static bool ensure_directory(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return true;
    }
    ESP_LOGE(TAG, "Directory creation failed: %s (%d)", path, errno);
    return false;
}

static esp_err_t mount_card(sd_context_t *context)
{
    set_storage_state(DESK_STORAGE_MOUNTING, false, ESP_OK);
    ESP_RETURN_ON_ERROR(desk_board_sd_select(true), TAG, "SD chip-select failed");

    context->host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    const spi_bus_config_t bus_config = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_CLOCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16 * 1024,
    };
    esp_err_t result = spi_bus_initialize(context->host.slot, &bus_config, SDSPI_DEFAULT_DMA);
    if (result != ESP_OK) {
        desk_board_sd_select(false);
        return result;
    }
    context->spi_initialized = true;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = GPIO_NUM_NC;
    slot_config.host_id = context->host.slot;
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    result = esp_vfs_fat_sdspi_mount(
        DESK_STORAGE_MOUNT_POINT,
        &context->host,
        &slot_config,
        &mount_config,
        &context->card
    );
    if (result != ESP_OK) {
        spi_bus_free(context->host.slot);
        context->spi_initialized = false;
        desk_board_sd_select(false);
        return result;
    }

    if (!ensure_directory(DESK_STORAGE_ROOT_DIR) || !ensure_directory(DESK_STORAGE_LOG_DIR) ||
        !ensure_directory(DESK_STORAGE_SCREENSHOT_DIR) || !ensure_directory(DESK_STORAGE_ASSET_DIR)) {
        unmount_card(context);
        return ESP_FAIL;
    }
    set_storage_state(DESK_STORAGE_READY, true, ESP_OK);
    ESP_LOGI(TAG, "microSD mounted at %s", DESK_STORAGE_MOUNT_POINT);
    return ESP_OK;
}

static void unmount_card(sd_context_t *context)
{
    if (context->card != NULL) {
        esp_vfs_fat_sdcard_unmount(DESK_STORAGE_MOUNT_POINT, context->card);
        context->card = NULL;
    }
    if (context->spi_initialized) {
        spi_bus_free(context->host.slot);
        context->spi_initialized = false;
    }
    desk_board_sd_select(false);
}

static void format_entry_time(const queued_log_entry_t *entry, char *output, size_t output_capacity)
{
    if (entry->epoch >= 1735689600U) {
        const time_t timestamp = (time_t)entry->epoch;
        struct tm local_time = {0};
        if (localtime_r(&timestamp, &local_time) != NULL &&
            strftime(output, output_capacity, "%Y-%m-%d %H:%M:%S", &local_time) > 0) {
            return;
        }
    }
    snprintf(
        output,
        output_capacity,
        "+%lu.%03lu",
        (unsigned long)(entry->uptime_ms / 1000U),
        (unsigned long)(entry->uptime_ms % 1000U)
    );
}

static FILE *open_log_file(
    const queued_log_entry_t *entry,
    uint32_t boot_id,
    unsigned segment,
    char *path,
    size_t path_capacity,
    uint64_t *current_bytes
)
{
    if (!desk_log_make_filename(
            DESK_STORAGE_LOG_DIR,
            entry->epoch,
            boot_id,
            segment,
            path,
            path_capacity
        )) {
        return NULL;
    }

    FILE *file = fopen(path, "a");
    if (file == NULL) {
        ESP_LOGE(TAG, "Log open failed: %s (%d)", path, errno);
        return NULL;
    }
    setvbuf(file, NULL, _IOFBF, 4096);
    if (fseek(file, 0, SEEK_END) == 0) {
        const long position = ftell(file);
        *current_bytes = position >= 0 ? (uint64_t)position : 0;
    } else {
        *current_bytes = 0;
    }
    return file;
}

static bool write_log_entry(
    FILE *file,
    const queued_log_entry_t *entry,
    uint64_t *current_bytes
)
{
    char timestamp[32];
    format_entry_time(entry, timestamp, sizeof(timestamp));
    const int written = fprintf(
        file,
        "%s [%s] %s: %s\n",
        timestamp,
        log_level_text(entry->level),
        entry->tag,
        entry->message
    );
    if (written < 0) {
        return false;
    }
    *current_bytes += (uint64_t)written;
    note_written_entry();
    return true;
}

static void storage_task(void *argument)
{
    (void)argument;
    const uint32_t boot_id = esp_random();
    sd_context_t context = {0};

    for (;;) {
        const esp_err_t mount_result = mount_card(&context);
        if (mount_result != ESP_OK) {
            set_storage_state(DESK_STORAGE_RETRY_WAIT, false, mount_result);
            ESP_LOGW(TAG, "microSD unavailable (%s), retrying", esp_err_to_name(mount_result));
            vTaskDelay(pdMS_TO_TICKS(MOUNT_RETRY_INTERVAL_MS));
            continue;
        }

        FILE *log_file = NULL;
        char log_path[LOG_PATH_CAPACITY] = {0};
        char active_bucket[DESK_LOG_BUCKET_KEY_SIZE] = {0};
        uint64_t current_bytes = 0;
        unsigned segment = 0;
        unsigned entries_since_flush = 0;
        bool card_healthy = true;

        while (card_healthy) {
            queued_log_entry_t entry;
            const BaseType_t received = xQueueReceive(
                log_queue,
                &entry,
                pdMS_TO_TICKS(LOG_FLUSH_INTERVAL_MS)
            );
            if (received != pdTRUE) {
                if (log_file != NULL && fflush(log_file) != 0) {
                    card_healthy = false;
                }
                entries_since_flush = 0;
                continue;
            }

            char bucket[DESK_LOG_BUCKET_KEY_SIZE];
            if (!desk_log_bucket_key(entry.epoch, bucket)) {
                note_dropped_entry();
                continue;
            }
            if (strcmp(active_bucket, bucket) != 0) {
                if (log_file != NULL) {
                    fclose(log_file);
                    log_file = NULL;
                }
                snprintf(active_bucket, sizeof(active_bucket), "%s", bucket);
                segment = 0;
                current_bytes = 0;
            }

            const size_t estimated_bytes = strlen(entry.tag) + strlen(entry.message) + 64U;
            if (log_file != NULL && desk_log_should_rotate(current_bytes, estimated_bytes, LOG_ROTATE_BYTES)) {
                fclose(log_file);
                log_file = NULL;
                segment++;
                current_bytes = 0;
            }
            if (log_file == NULL) {
                log_file = open_log_file(
                    &entry,
                    boot_id,
                    segment,
                    log_path,
                    sizeof(log_path),
                    &current_bytes
                );
                if (log_file == NULL) {
                    card_healthy = false;
                    note_dropped_entry();
                    continue;
                }
            }

            if (!write_log_entry(log_file, &entry, &current_bytes)) {
                card_healthy = false;
                note_dropped_entry();
                continue;
            }
            entries_since_flush++;
            if (entries_since_flush >= LOG_FLUSH_ENTRY_COUNT) {
                if (fflush(log_file) != 0) {
                    card_healthy = false;
                }
                entries_since_flush = 0;
            }
        }

        if (log_file != NULL) {
            fclose(log_file);
        }
        unmount_card(&context);
        set_storage_state(DESK_STORAGE_RETRY_WAIT, false, ESP_FAIL);
        vTaskDelay(pdMS_TO_TICKS(MOUNT_RETRY_INTERVAL_MS));
    }
}

esp_err_t desk_storage_start(void)
{
    if (storage_task_handle != NULL) {
        return ESP_OK;
    }

    log_queue = xQueueCreate(LOG_QUEUE_LENGTH, sizeof(queued_log_entry_t));
    if (log_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const BaseType_t created = xTaskCreate(
        storage_task,
        "desk_storage",
        LOG_TASK_STACK_SIZE,
        NULL,
        2,
        &storage_task_handle
    );
    if (created != pdPASS) {
        vQueueDelete(log_queue);
        log_queue = NULL;
        storage_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

desk_storage_status_t desk_storage_get_status(void)
{
    taskENTER_CRITICAL(&status_lock);
    desk_storage_status_t snapshot = storage_status;
    taskEXIT_CRITICAL(&status_lock);
    snapshot.queued_entries = log_queue != NULL ? (uint32_t)uxQueueMessagesWaiting(log_queue) : 0;
    return snapshot;
}

bool desk_storage_log(
    desk_log_level_t level,
    desk_log_content_class_t content_class,
    const char *tag,
    const char *format,
    ...
)
{
    if (log_queue == NULL || tag == NULL || format == NULL) {
        return false;
    }

    const time_t now = time(NULL);
    queued_log_entry_t entry = {
        .epoch = now >= 0 && (uint64_t)now <= UINT32_MAX ? (uint32_t)now : 0,
        .uptime_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .level = level,
    };
    desk_log_normalize_message(DESK_LOG_CONTENT_PUBLIC, tag, entry.tag, sizeof(entry.tag));

    if (content_class == DESK_LOG_CONTENT_PRIVATE) {
        desk_log_normalize_message(content_class, NULL, entry.message, sizeof(entry.message));
    } else {
        char raw_message[LOG_MESSAGE_CAPACITY];
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(raw_message, sizeof(raw_message), format, arguments);
        va_end(arguments);
        desk_log_normalize_message(content_class, raw_message, entry.message, sizeof(entry.message));
    }

    if (xQueueSend(log_queue, &entry, 0) != pdTRUE) {
        note_dropped_entry();
        return false;
    }
    return true;
}
