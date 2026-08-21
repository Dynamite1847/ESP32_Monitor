#include "network/network.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "desk_network";

enum {
    NETWORK_EVENT_QUEUE_LENGTH = 8,
    FAST_RETRY_LIMIT = 5,
    RECONNECT_BASE_DELAY_MS = 1000,
    RECONNECT_MAX_DELAY_MS = 60000,
};

/* state_lock 保护下面这组跨任务共享状态：Wi-Fi 事件任务、esp_timer 任务和
 * 调用 desk_network_provision 的 supervisor 任务会并发读改写它们。 */
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t network_event_queue;
static esp_timer_handle_t reconnect_timer;
static bool network_started;
static bool credentials_available;
static bool station_connected;
static bool provisioning_disconnect_pending;
static bool exhausted_notified;
static uint32_t retry_count;
static uint8_t last_disconnect_reason;

static void queue_network_event(const desk_network_event_t *event)
{
    if (network_event_queue == NULL || xQueueSend(network_event_queue, event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Network event queue is full");
    }
}

static bool stored_credentials_available(void)
{
    wifi_config_t configuration = {0};
    return esp_wifi_get_config(WIFI_IF_STA, &configuration) == ESP_OK &&
           configuration.sta.ssid[0] != '\0';
}

/* 退避序列：1s、2s、4s、8s、16s、32s，之后夹在 60s 上限持续重连，永不放弃。 */
static uint32_t reconnect_delay_ms(uint32_t attempts)
{
    uint32_t delay = RECONNECT_BASE_DELAY_MS;
    for (uint32_t i = 0; i < attempts && delay < RECONNECT_MAX_DELAY_MS; ++i) {
        delay *= 2;
    }
    return delay > RECONNECT_MAX_DELAY_MS ? RECONNECT_MAX_DELAY_MS : delay;
}

static void reconnect_timer_callback(void *argument)
{
    (void)argument;
    bool connect;
    taskENTER_CRITICAL(&state_lock);
    connect = credentials_available;
    taskEXIT_CRITICAL(&state_lock);
    if (connect) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
    }
}

/* 断开后按退避排一次重连。永不永久放弃：快重试次数用尽后按 60s 上限慢重连，
 * 路由器恢复即自动接回。 */
static void schedule_reconnect(void)
{
    uint32_t attempts;
    taskENTER_CRITICAL(&state_lock);
    attempts = retry_count;
    if (retry_count < UINT32_MAX) {
        retry_count++;
    }
    taskEXIT_CRITICAL(&state_lock);

    if (reconnect_timer == NULL) {
        return;
    }
    esp_timer_stop(reconnect_timer);  /* 幂等：未运行时返回错误，忽略即可 */
    const uint32_t delay_ms = reconnect_delay_ms(attempts);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_start_once(reconnect_timer, (uint64_t)delay_ms * 1000ULL));
    ESP_LOGI(TAG, "Wi-Fi reconnect in %u ms (attempt %u)", (unsigned)delay_ms, (unsigned)attempts + 1U);
}

static void network_event_handler(
    void *context,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)context;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        const desk_network_event_t event = {.type = DESK_NETWORK_EVENT_STARTED, .status = ESP_OK};
        queue_network_event(&event);
        bool connect;
        taskENTER_CRITICAL(&state_lock);
        connect = credentials_available;
        taskEXIT_CRITICAL(&state_lock);
        if (connect) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        const desk_network_event_t event = {
            .type = DESK_NETWORK_EVENT_DISCONNECTED,
            .status = ESP_FAIL,
            .disconnect_reason = disconnected != NULL ? disconnected->reason : 0,
        };
        queue_network_event(&event);

        bool provisioning;
        bool have_credentials;
        uint32_t attempts;
        bool notify_exhausted = false;
        taskENTER_CRITICAL(&state_lock);
        station_connected = false;
        last_disconnect_reason = disconnected != NULL ? disconnected->reason : 0;
        provisioning = provisioning_disconnect_pending;
        provisioning_disconnect_pending = false;
        have_credentials = credentials_available;
        attempts = retry_count;
        if (have_credentials && !provisioning && attempts >= FAST_RETRY_LIMIT && !exhausted_notified) {
            exhausted_notified = true;
            notify_exhausted = true;
        }
        taskEXIT_CRITICAL(&state_lock);

        if (provisioning || !have_credentials) {
            return;
        }
        if (notify_exhausted) {
            /* 通知一次“已离线”，UI 转离线态；随后仍按慢节奏持续重连。 */
            const desk_network_event_t exhausted = {
                .type = DESK_NETWORK_EVENT_RETRY_EXHAUSTED,
                .status = ESP_FAIL,
            };
            queue_network_event(&exhausted);
        }
        schedule_reconnect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got_ip = event_data;
        wifi_ap_record_t access_point = {0};
        const int8_t rssi = esp_wifi_sta_get_ap_info(&access_point) == ESP_OK ? access_point.rssi : 0;
        taskENTER_CRITICAL(&state_lock);
        station_connected = true;
        retry_count = 0;
        exhausted_notified = false;
        taskEXIT_CRITICAL(&state_lock);
        if (reconnect_timer != NULL) {
            esp_timer_stop(reconnect_timer);
        }
        desk_network_event_t event = {
            .type = DESK_NETWORK_EVENT_CONNECTED,
            .status = ESP_OK,
            .rssi_dbm = rssi,
            .ipv4_address = got_ip != NULL ? got_ip->ip_info.ip.addr : 0,
        };
        if (got_ip != NULL) {
            esp_ip4addr_ntoa(&got_ip->ip_info.ip, event.ipv4, sizeof(event.ipv4));
        }
        queue_network_event(&event);
    }
}

esp_err_t desk_network_start(void)
{
    if (network_started) {
        return ESP_OK;
    }

    network_event_queue = xQueueCreate(NETWORK_EVENT_QUEUE_LENGTH, sizeof(desk_network_event_t));
    if (network_event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_sntp_config_t time_configuration = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    ESP_RETURN_ON_ERROR(
        esp_netif_sntp_init(&time_configuration),
        TAG,
        "Network time initialization failed"
    );

    const wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&initialization), TAG, "Wi-Fi initialization failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            network_event_handler,
            NULL,
            NULL
        ),
        TAG,
        "Wi-Fi event registration failed"
    );
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            network_event_handler,
            NULL,
            NULL
        ),
        TAG,
        "IP event registration failed"
    );
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "Wi-Fi storage setup failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi station mode failed");

    const esp_timer_create_args_t reconnect_args = {
        .callback = reconnect_timer_callback,
        .name = "wifi_reconnect",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&reconnect_args, &reconnect_timer), TAG, "Reconnect timer creation failed");

    const bool have_saved = stored_credentials_available();
    taskENTER_CRITICAL(&state_lock);
    credentials_available = have_saved;
    station_connected = false;
    retry_count = 0;
    exhausted_notified = false;
    provisioning_disconnect_pending = false;
    last_disconnect_reason = 0;
    taskEXIT_CRITICAL(&state_lock);

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");
    network_started = true;
    ESP_LOGI(TAG, "Wi-Fi station started; saved network=%d", have_saved ? 1 : 0);
    return ESP_OK;
}

esp_err_t desk_network_provision(
    const char *ssid,
    size_t ssid_length,
    const char *password,
    size_t password_length
)
{
    if (!network_started || ssid == NULL || password == NULL || ssid_length == 0 ||
        ssid_length > DESK_WIFI_SSID_MAX_BYTES || password_length > DESK_WIFI_PASSWORD_MAX_BYTES ||
        (password_length > 0 && password_length < 8)) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t previous_configuration = {0};
    const bool previous_configuration_read =
        esp_wifi_get_config(WIFI_IF_STA, &previous_configuration) == ESP_OK;
    bool previously_available;
    taskENTER_CRITICAL(&state_lock);
    previously_available = credentials_available;
    taskEXIT_CRITICAL(&state_lock);

    wifi_config_t configuration = {0};
    memcpy(configuration.sta.ssid, ssid, ssid_length);
    memcpy(configuration.sta.password, password, password_length);
    configuration.sta.threshold.authmode = WIFI_AUTH_OPEN;
    configuration.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    /* 先置 pending 再断开：esp_wifi_disconnect 会异步触发 DISCONNECTED，晚置标志会漏抑制，
     * 导致这次配网断开被误当作真实掉线而触发重连风暴。 */
    taskENTER_CRITICAL(&state_lock);
    credentials_available = false;
    provisioning_disconnect_pending = true;
    taskEXIT_CRITICAL(&state_lock);
    if (reconnect_timer != NULL) {
        esp_timer_stop(reconnect_timer);
    }

    const esp_err_t disconnect_result = esp_wifi_disconnect();
    if (disconnect_result != ESP_OK) {
        /* 没有真正断开 → 不会有 DISCONNECTED 事件来消费 pending，这里清掉以免滞留。 */
        taskENTER_CRITICAL(&state_lock);
        provisioning_disconnect_pending = false;
        taskEXIT_CRITICAL(&state_lock);
        if (disconnect_result != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(TAG, "Wi-Fi disconnect before reconfiguration failed: %s", esp_err_to_name(disconnect_result));
        }
    }

    const esp_err_t configuration_result = esp_wifi_set_config(WIFI_IF_STA, &configuration);
    if (configuration_result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi configuration rejected: %s", esp_err_to_name(configuration_result));
        if (previous_configuration_read) {
            const esp_err_t rollback_result =
                esp_wifi_set_config(WIFI_IF_STA, &previous_configuration);
            if (rollback_result != ESP_OK) {
                ESP_LOGE(TAG, "Previous Wi-Fi configuration restore failed: %s", esp_err_to_name(rollback_result));
            }
        }
        taskENTER_CRITICAL(&state_lock);
        credentials_available = previously_available;
        provisioning_disconnect_pending = false;
        retry_count = 0;
        exhausted_notified = false;
        taskEXIT_CRITICAL(&state_lock);
        if (previously_available) {
            schedule_reconnect();
        }
        return configuration_result;
    }

    taskENTER_CRITICAL(&state_lock);
    credentials_available = true;
    retry_count = 0;
    exhausted_notified = false;
    taskEXIT_CRITICAL(&state_lock);
    ESP_LOGI(TAG, "Wi-Fi network configuration updated");
    const esp_err_t connect_result = esp_wifi_connect();
    if (connect_result != ESP_OK) {
        ESP_LOGW(TAG, "Immediate Wi-Fi connect failed; background retry scheduled: %s", esp_err_to_name(connect_result));
        schedule_reconnect();
    }
    return connect_result;
}

esp_err_t desk_network_reconnect(void)
{
    if (!network_started) {
        return ESP_ERR_INVALID_STATE;
    }

    bool have_credentials;
    bool connected;
    taskENTER_CRITICAL(&state_lock);
    have_credentials = credentials_available;
    connected = station_connected;
    retry_count = 0;
    exhausted_notified = false;
    taskEXIT_CRITICAL(&state_lock);
    if (!have_credentials) {
        return ESP_ERR_NOT_FOUND;
    }
    if (reconnect_timer != NULL) {
        esp_timer_stop(reconnect_timer);
    }

    if (connected) {
        const esp_err_t result = esp_wifi_disconnect();
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "Manual Wi-Fi reconnect requested");
            return ESP_OK;
        }
        if (result != ESP_ERR_WIFI_NOT_CONNECT) {
            return result;
        }
    }

    const esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK) {
        schedule_reconnect();
    }
    return result;
}

desk_network_status_t desk_network_get_status(void)
{
    desk_network_status_t status = {0};
    taskENTER_CRITICAL(&state_lock);
    status.connected = station_connected;
    status.credentials_available = credentials_available;
    status.reconnect_attempts = retry_count;
    status.last_disconnect_reason = last_disconnect_reason;
    taskEXIT_CRITICAL(&state_lock);
    wifi_config_t configuration = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &configuration) == ESP_OK) {
        memcpy(status.ssid, configuration.sta.ssid, DESK_WIFI_SSID_MAX_BYTES);
        status.ssid[DESK_WIFI_SSID_MAX_BYTES] = '\0';
    }
    return status;
}

bool desk_network_receive_event(desk_network_event_t *event, uint32_t timeout_ms)
{
    if (network_event_queue == NULL || event == NULL) {
        return false;
    }
    return xQueueReceive(network_event_queue, event, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
