#include "network/network.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "desk_network";

enum {
    NETWORK_EVENT_QUEUE_LENGTH = 8,
    MAXIMUM_CONNECT_RETRIES = 5,
};

static QueueHandle_t network_event_queue;
static bool network_started;
static bool credentials_available;
static bool provisioning_disconnect_pending;
static uint8_t retry_count;

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
        if (credentials_available) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        desk_network_event_t event = {
            .type = DESK_NETWORK_EVENT_DISCONNECTED,
            .status = ESP_FAIL,
            .disconnect_reason = disconnected != NULL ? disconnected->reason : 0,
        };
        queue_network_event(&event);

        if (provisioning_disconnect_pending) {
            provisioning_disconnect_pending = false;
            return;
        }

        if (credentials_available && retry_count < MAXIMUM_CONNECT_RETRIES) {
            retry_count++;
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
        } else if (credentials_available) {
            event.type = DESK_NETWORK_EVENT_RETRY_EXHAUSTED;
            queue_network_event(&event);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got_ip = event_data;
        wifi_ap_record_t access_point = {0};
        const int8_t rssi = esp_wifi_sta_get_ap_info(&access_point) == ESP_OK ? access_point.rssi : 0;
        retry_count = 0;
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

    credentials_available = stored_credentials_available();
    retry_count = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");
    network_started = true;
    ESP_LOGI(TAG, "Wi-Fi station started; saved network=%d", credentials_available ? 1 : 0);
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

    wifi_config_t configuration = {0};
    memcpy(configuration.sta.ssid, ssid, ssid_length);
    memcpy(configuration.sta.password, password, password_length);
    configuration.sta.threshold.authmode = WIFI_AUTH_OPEN;
    configuration.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    credentials_available = false;
    const esp_err_t disconnect_result = esp_wifi_disconnect();
    provisioning_disconnect_pending = disconnect_result == ESP_OK;
    if (disconnect_result != ESP_OK && disconnect_result != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "Wi-Fi disconnect before reconfiguration failed: %s", esp_err_to_name(disconnect_result));
    }
    ESP_RETURN_ON_ERROR(
        esp_wifi_set_config(WIFI_IF_STA, &configuration),
        TAG,
        "Wi-Fi configuration rejected"
    );
    credentials_available = true;
    retry_count = 0;
    ESP_LOGI(TAG, "Wi-Fi network configuration updated");
    return esp_wifi_connect();
}

bool desk_network_receive_event(desk_network_event_t *event, uint32_t timeout_ms)
{
    if (network_event_queue == NULL || event == NULL) {
        return false;
    }
    return xQueueReceive(network_event_queue, event, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
