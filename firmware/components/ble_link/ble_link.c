#include "ble_link/ble_link.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void);

static const char *TAG = "desk_ble";

enum {
    EVENT_QUEUE_LENGTH = 8,
    REASSEMBLY_TIMEOUT_MS = 2000,
    DEFAULT_ATT_MTU = 23,
    MAX_ATT_MTU = 247,
    ATT_HEADER_SIZE = 3,
    STATUS_VALUE_SIZE = 8,
};

typedef enum {
    CHARACTERISTIC_RX = 1,
    CHARACTERISTIC_TX,
    CHARACTERISTIC_STATUS,
} characteristic_id_t;

typedef struct {
    bool active;
    uint16_t frame_id;
    uint8_t fragment_count;
    uint8_t next_fragment;
    uint32_t started_ms;
    size_t length;
    uint8_t data[DESK_PROTOCOL_MAX_FRAME_SIZE];
} reassembly_state_t;

typedef struct {
    bool started;
    bool connected;
    bool encrypted;
    bool bonded;
    bool subscribed;
    uint8_t own_address_type;
    uint16_t connection_handle;
    uint16_t mtu;
    uint16_t next_tx_frame_id;
    uint16_t rx_error_count;
    QueueHandle_t event_queue;
    portMUX_TYPE lock;
    reassembly_state_t reassembly;
} ble_link_state_t;

static ble_link_state_t link_state = {
    .connection_handle = BLE_HS_CONN_HANDLE_NONE,
    .mtu = DEFAULT_ATT_MTU,
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

/* NimBLE stores 128-bit UUID bytes in little-endian order. */
static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0x00, 0x4f, 0x1a, 0x8c, 0x2c, 0x3b, 0x8d, 0x9e,
    0x42, 0x4b, 0x1f, 0x5d, 0x01, 0x00, 0x1d, 0x7a
);
static const ble_uuid128_t rx_uuid = BLE_UUID128_INIT(
    0x00, 0x4f, 0x1a, 0x8c, 0x2c, 0x3b, 0x8d, 0x9e,
    0x42, 0x4b, 0x1f, 0x5d, 0x02, 0x00, 0x1d, 0x7a
);
static const ble_uuid128_t tx_uuid = BLE_UUID128_INIT(
    0x00, 0x4f, 0x1a, 0x8c, 0x2c, 0x3b, 0x8d, 0x9e,
    0x42, 0x4b, 0x1f, 0x5d, 0x03, 0x00, 0x1d, 0x7a
);
static const ble_uuid128_t status_uuid = BLE_UUID128_INIT(
    0x00, 0x4f, 0x1a, 0x8c, 0x2c, 0x3b, 0x8d, 0x9e,
    0x42, 0x4b, 0x1f, 0x5d, 0x04, 0x00, 0x1d, 0x7a
);

static uint16_t tx_value_handle;
static uint16_t status_value_handle;

static uint16_t read_u16_le(const uint8_t *input)
{
    return (uint16_t)input[0] | ((uint16_t)input[1] << 8U);
}

static void write_u16_le(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value & 0xffU);
    output[1] = (uint8_t)(value >> 8U);
}

static uint32_t uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void reset_reassembly(void)
{
    memset(&link_state.reassembly, 0, sizeof(link_state.reassembly));
}

static void post_event(const desk_ble_event_t *event)
{
    if (link_state.event_queue == NULL || xQueueSend(link_state.event_queue, event, 0) == pdTRUE) {
        return;
    }

    taskENTER_CRITICAL(&link_state.lock);
    link_state.rx_error_count++;
    taskEXIT_CRITICAL(&link_state.lock);
    ESP_LOGW(TAG, "BLE event queue full; event %d dropped", event->type);
}

static void post_simple_event(desk_ble_event_type_t type, int status)
{
    desk_ble_event_t event = {
        .type = type,
        .status = status,
    };

    taskENTER_CRITICAL(&link_state.lock);
    event.connection_handle = link_state.connection_handle;
    event.mtu = link_state.mtu;
    event.encrypted = link_state.encrypted;
    event.bonded = link_state.bonded;
    event.subscribed = link_state.subscribed;
    taskEXIT_CRITICAL(&link_state.lock);
    post_event(&event);
}

static int handle_rx_fragment(struct os_mbuf *packet)
{
    const uint16_t packet_length = OS_MBUF_PKTLEN(packet);
    if (packet_length <= DESK_BLE_FRAGMENT_HEADER_SIZE || packet_length > MAX_ATT_MTU - ATT_HEADER_SIZE) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t flat[MAX_ATT_MTU - ATT_HEADER_SIZE];
    uint16_t copied = 0;
    if (ble_hs_mbuf_to_flat(packet, flat, sizeof(flat), &copied) != 0 || copied != packet_length) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const uint16_t frame_id = read_u16_le(flat);
    const uint8_t fragment_index = flat[2];
    const uint8_t fragment_count = flat[3];
    const uint32_t now_ms = uptime_ms();
    const size_t fragment_length = packet_length - DESK_BLE_FRAGMENT_HEADER_SIZE;
    reassembly_state_t *assembly = &link_state.reassembly;

    if (assembly->active && (uint32_t)(now_ms - assembly->started_ms) > REASSEMBLY_TIMEOUT_MS) {
        reset_reassembly();
    }

    if (fragment_count == 0 || fragment_index >= fragment_count) {
        reset_reassembly();
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    if (fragment_index == 0) {
        reset_reassembly();
        assembly->active = true;
        assembly->frame_id = frame_id;
        assembly->fragment_count = fragment_count;
        assembly->started_ms = now_ms;
    }

    if (!assembly->active || assembly->frame_id != frame_id ||
        assembly->fragment_count != fragment_count || assembly->next_fragment != fragment_index ||
        assembly->length + fragment_length > sizeof(assembly->data)) {
        reset_reassembly();
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    memcpy(&assembly->data[assembly->length], &flat[DESK_BLE_FRAGMENT_HEADER_SIZE], fragment_length);
    assembly->length += fragment_length;
    assembly->next_fragment++;

    if (assembly->next_fragment != assembly->fragment_count) {
        return 0;
    }

    desk_protocol_frame_t decoded;
    const desk_protocol_status_t protocol_status =
        desk_protocol_decode(assembly->data, assembly->length, &decoded);
    if (protocol_status != DESK_PROTOCOL_OK) {
        ESP_LOGW(TAG, "Rejected invalid logical frame; status=%d", protocol_status);
        reset_reassembly();
        return BLE_ATT_ERR_UNLIKELY;
    }

    desk_ble_event_t event = {
        .type = DESK_BLE_EVENT_FRAME_RECEIVED,
        .frame_length = assembly->length,
    };
    taskENTER_CRITICAL(&link_state.lock);
    event.connection_handle = link_state.connection_handle;
    event.mtu = link_state.mtu;
    event.encrypted = link_state.encrypted;
    event.bonded = link_state.bonded;
    event.subscribed = link_state.subscribed;
    taskEXIT_CRITICAL(&link_state.lock);
    memcpy(event.frame, assembly->data, assembly->length);
    reset_reassembly();
    post_event(&event);
    return 0;
}

static int gatt_access(
    uint16_t connection_handle,
    uint16_t attribute_handle,
    struct ble_gatt_access_ctxt *context,
    void *argument
)
{
    (void)connection_handle;
    (void)attribute_handle;
    const characteristic_id_t id = (characteristic_id_t)(intptr_t)argument;

    if (id == CHARACTERISTIC_RX && context->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return handle_rx_fragment(context->om);
    }

    if (id == CHARACTERISTIC_STATUS && context->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t value[STATUS_VALUE_SIZE] = {0};
        value[0] = DESK_PROTOCOL_VERSION;
        taskENTER_CRITICAL(&link_state.lock);
        value[1] = (link_state.connected ? 1U : 0U) |
                   (link_state.encrypted ? 2U : 0U) |
                   (link_state.bonded ? 4U : 0U) |
                   (link_state.subscribed ? 8U : 0U);
        write_u16_le(&value[2], link_state.mtu);
        write_u16_le(&value[4], link_state.rx_error_count);
        taskEXIT_CRITICAL(&link_state.lock);
        return os_mbuf_append(context->om, value, sizeof(value)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &rx_uuid.u,
                .access_cb = gatt_access,
                .arg = (void *)(intptr_t)CHARACTERISTIC_RX,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                         BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid = &tx_uuid.u,
                .access_cb = gatt_access,
                .arg = (void *)(intptr_t)CHARACTERISTIC_TX,
                .val_handle = &tx_value_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &status_uuid.u,
                .access_cb = gatt_access,
                .arg = (void *)(intptr_t)CHARACTERISTIC_STATUS,
                .val_handle = &status_value_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    {0},
};

static int gap_event(struct ble_gap_event *event, void *argument);

static bool start_advertising(void)
{
    struct ble_hs_adv_fields advertising_fields = {0};
    advertising_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    advertising_fields.uuids128 = (ble_uuid128_t *)&service_uuid;
    advertising_fields.num_uuids128 = 1;
    advertising_fields.uuids128_is_complete = 1;

    int result = ble_gap_adv_set_fields(&advertising_fields);
    if (result != 0) {
        ESP_LOGE(TAG, "Advertising fields failed: %d", result);
        post_simple_event(DESK_BLE_EVENT_TRANSPORT_ERROR, result);
        return false;
    }

    const char *name = ble_svc_gap_device_name();
    struct ble_hs_adv_fields response_fields = {0};
    response_fields.name = (uint8_t *)name;
    response_fields.name_len = strlen(name);
    response_fields.name_is_complete = 1;
    response_fields.tx_pwr_lvl_is_present = 1;
    response_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    result = ble_gap_adv_rsp_set_fields(&response_fields);
    if (result != 0) {
        ESP_LOGE(TAG, "Scan response fields failed: %d", result);
        post_simple_event(DESK_BLE_EVENT_TRANSPORT_ERROR, result);
        return false;
    }

    struct ble_gap_adv_params parameters = {0};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    result = ble_gap_adv_start(
        link_state.own_address_type,
        NULL,
        BLE_HS_FOREVER,
        &parameters,
        gap_event,
        NULL
    );
    if (result != 0 && result != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "Advertising start failed: %d", result);
        post_simple_event(DESK_BLE_EVENT_TRANSPORT_ERROR, result);
        return false;
    }
    return true;
}

static int gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                ESP_LOGW(TAG, "Connection attempt failed: %d", event->connect.status);
                start_advertising();
                return 0;
            }
            struct ble_gap_conn_desc description;
            const bool has_description =
                ble_gap_conn_find(event->connect.conn_handle, &description) == 0;
            const bool initially_encrypted =
                has_description && description.sec_state.encrypted;
            const bool initially_bonded =
                has_description && description.sec_state.bonded;
            taskENTER_CRITICAL(&link_state.lock);
            link_state.connected = true;
            link_state.encrypted = initially_encrypted;
            link_state.bonded = initially_bonded;
            /* Do NOT reset subscribed here: on a bonded reconnect NimBLE may
             * dispatch the GATT CCCD subscribe event before the connect-complete
             * event, so a reset would wipe the fresh subscription and the next
             * notify would be blocked. DISCONNECT already clears it. */
            link_state.connection_handle = event->connect.conn_handle;
            link_state.mtu = DEFAULT_ATT_MTU;
            taskEXIT_CRITICAL(&link_state.lock);
            reset_reassembly();
            ESP_LOGI(TAG, "Mac connected; handle=%u", event->connect.conn_handle);
            post_simple_event(DESK_BLE_EVENT_CONNECTED, 0);
            if (initially_encrypted) {
                ESP_LOGI(TAG, "Connection arrived with link encryption already active");
                post_simple_event(DESK_BLE_EVENT_SECURITY_CHANGED, 0);
            } else {
                const int result = ble_gap_security_initiate(event->connect.conn_handle);
                if (result != 0 && result != BLE_HS_EALREADY) {
                    ESP_LOGW(TAG, "Security initiation failed: %d", result);
                    post_simple_event(DESK_BLE_EVENT_TRANSPORT_ERROR, result);
                }
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Mac disconnected; reason=%d", event->disconnect.reason);
            taskENTER_CRITICAL(&link_state.lock);
            link_state.connected = false;
            link_state.encrypted = false;
            link_state.bonded = false;
            link_state.subscribed = false;
            link_state.connection_handle = BLE_HS_CONN_HANDLE_NONE;
            link_state.mtu = DEFAULT_ATT_MTU;
            taskEXIT_CRITICAL(&link_state.lock);
            reset_reassembly();
            post_simple_event(DESK_BLE_EVENT_DISCONNECTED, event->disconnect.reason);
            start_advertising();
            return 0;

        case BLE_GAP_EVENT_ENC_CHANGE: {
            taskENTER_CRITICAL(&link_state.lock);
            const bool is_current_connection =
                link_state.connected &&
                link_state.connection_handle == event->enc_change.conn_handle;
            taskEXIT_CRITICAL(&link_state.lock);
            if (!is_current_connection) {
                ESP_LOGW(
                    TAG,
                    "Ignoring stale security event for handle=%u status=%d",
                    event->enc_change.conn_handle,
                    event->enc_change.status
                );
                return 0;
            }
            struct ble_gap_conn_desc description;
            const int result = ble_gap_conn_find(event->enc_change.conn_handle, &description);
            if (result == 0) {
                taskENTER_CRITICAL(&link_state.lock);
                link_state.encrypted = description.sec_state.encrypted;
                link_state.bonded = description.sec_state.bonded;
                taskEXIT_CRITICAL(&link_state.lock);
            }
            ESP_LOGI(
                TAG,
                "Security changed; status=%d encrypted=%d bonded=%d",
                event->enc_change.status,
                link_state.encrypted,
                link_state.bonded
            );
            post_simple_event(DESK_BLE_EVENT_SECURITY_CHANGED, event->enc_change.status);
            return 0;
        }

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == tx_value_handle ||
                event->subscribe.attr_handle == status_value_handle) {
                taskENTER_CRITICAL(&link_state.lock);
                if (event->subscribe.attr_handle == tx_value_handle) {
                    link_state.subscribed = event->subscribe.cur_notify != 0;
                }
                taskEXIT_CRITICAL(&link_state.lock);
                post_simple_event(DESK_BLE_EVENT_SUBSCRIPTION_CHANGED, 0);
            }
            return 0;

        case BLE_GAP_EVENT_MTU:
            taskENTER_CRITICAL(&link_state.lock);
            link_state.mtu = event->mtu.value;
            taskEXIT_CRITICAL(&link_state.lock);
            ESP_LOGI(TAG, "MTU changed to %u", event->mtu.value);
            post_simple_event(DESK_BLE_EVENT_MTU_CHANGED, 0);
            return 0;

        case BLE_GAP_EVENT_REPEAT_PAIRING: {
#if defined(CONFIG_DESK_BRINGUP_DISPLAY_ON) && CONFIG_DESK_BRINGUP_DISPLAY_ON
            struct ble_gap_conn_desc description;
            const int result = ble_gap_conn_find(
                event->repeat_pairing.conn_handle,
                &description
            );
            if (result == 0) {
                ESP_LOGW(TAG, "Replacing stale peer binding during bring-up");
                ble_store_util_delete_peer(&description.peer_id_addr);
                return BLE_GAP_REPEAT_PAIRING_RETRY;
            }
            ESP_LOGW(TAG, "Could not inspect repeated pairing peer: %d", result);
#else
            ESP_LOGW(TAG, "Repeated pairing was rejected; physical recovery is required");
#endif
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }

        case BLE_GAP_EVENT_ADV_COMPLETE:
            start_advertising();
            return 0;

        default:
            return 0;
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE host reset; reason=%d", reason);
    post_simple_event(DESK_BLE_EVENT_TRANSPORT_ERROR, reason);
}

static void on_sync(void)
{
    int result = ble_hs_util_ensure_addr(0);
    if (result == 0) {
        result = ble_hs_id_infer_auto(0, &link_state.own_address_type);
    }
    if (result != 0) {
        ESP_LOGE(TAG, "BLE address setup failed: %d", result);
        post_simple_event(DESK_BLE_EVENT_TRANSPORT_ERROR, result);
        return;
    }

    if (start_advertising()) {
        ESP_LOGI(TAG, "BLE ready as '%s'", DESK_BLE_DEVICE_NAME);
        post_simple_event(DESK_BLE_EVENT_READY, 0);
    }
}

static void host_task(void *argument)
{
    (void)argument;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t desk_ble_link_start(void)
{
    if (link_state.started) {
        return ESP_ERR_INVALID_STATE;
    }

    link_state.event_queue = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(desk_ble_event_t));
    if (link_state.event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed: %s", esp_err_to_name(result));
        return result;
    }

    result = nimble_port_init();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE initialization failed: %s", esp_err_to_name(result));
        return result;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int host_result = ble_gatts_count_cfg(gatt_services);
    if (host_result == 0) {
        host_result = ble_gatts_add_svcs(gatt_services);
    }
    if (host_result == 0) {
        host_result = ble_svc_gap_device_name_set(DESK_BLE_DEVICE_NAME);
    }
    if (host_result != 0) {
        ESP_LOGE(TAG, "GATT service initialization failed: %d", host_result);
        return ESP_FAIL;
    }

    ble_store_config_init();
    link_state.started = true;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

bool desk_ble_link_receive_event(desk_ble_event_t *event, uint32_t timeout_ms)
{
    if (event == NULL || link_state.event_queue == NULL) {
        return false;
    }
    const TickType_t ticks = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xQueueReceive(link_state.event_queue, event, ticks) == pdTRUE;
}

esp_err_t desk_ble_link_send_frame(const uint8_t *frame, size_t frame_length)
{
    if (frame == NULL || frame_length == 0 || frame_length > DESK_PROTOCOL_MAX_FRAME_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    desk_protocol_frame_t decoded;
    if (desk_protocol_decode(frame, frame_length, &decoded) != DESK_PROTOCOL_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t connection_handle;
    uint16_t mtu;
    uint16_t frame_id;
    bool can_notify;
    taskENTER_CRITICAL(&link_state.lock);
    connection_handle = link_state.connection_handle;
    mtu = link_state.mtu;
    frame_id = link_state.next_tx_frame_id++;
    can_notify = link_state.connected && link_state.encrypted && link_state.subscribed;
    taskEXIT_CRITICAL(&link_state.lock);
    if (!can_notify || connection_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(
            TAG,
            "Send blocked: connected=%d encrypted=%d subscribed=%d handle=%u mtu=%u",
            link_state.connected,
            link_state.encrypted,
            link_state.subscribed,
            link_state.connection_handle,
            link_state.mtu
        );
        return ESP_ERR_INVALID_STATE;
    }

    const size_t packet_capacity = mtu > ATT_HEADER_SIZE + DESK_BLE_FRAGMENT_HEADER_SIZE
                                       ? mtu - ATT_HEADER_SIZE
                                       : DEFAULT_ATT_MTU - ATT_HEADER_SIZE;
    const size_t fragment_capacity = packet_capacity - DESK_BLE_FRAGMENT_HEADER_SIZE;
    const size_t fragment_count_size = (frame_length + fragment_capacity - 1U) / fragment_capacity;
    if (fragment_count_size == 0 || fragment_count_size > UINT8_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t fragment_count = (uint8_t)fragment_count_size;

    uint8_t packet[MAX_ATT_MTU - ATT_HEADER_SIZE];
    size_t offset = 0;
    for (uint8_t index = 0; index < fragment_count; ++index) {
        const size_t remaining = frame_length - offset;
        const size_t chunk = remaining < fragment_capacity ? remaining : fragment_capacity;
        write_u16_le(packet, frame_id);
        packet[2] = index;
        packet[3] = fragment_count;
        memcpy(&packet[DESK_BLE_FRAGMENT_HEADER_SIZE], &frame[offset], chunk);

        struct os_mbuf *output = ble_hs_mbuf_from_flat(
            packet,
            (uint16_t)(DESK_BLE_FRAGMENT_HEADER_SIZE + chunk)
        );
        if (output == NULL) {
            return ESP_ERR_NO_MEM;
        }
        const int result = ble_gatts_notify_custom(connection_handle, tx_value_handle, output);
        if (result != 0) {
            ESP_LOGW(TAG, "Notification fragment %u/%u failed: %d", index + 1U, fragment_count, result);
            return ESP_FAIL;
        }
        offset += chunk;
    }
    return ESP_OK;
}

esp_err_t desk_ble_link_disconnect(void)
{
    uint16_t connection_handle;
    taskENTER_CRITICAL(&link_state.lock);
    connection_handle = link_state.connection_handle;
    taskEXIT_CRITICAL(&link_state.lock);
    if (connection_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    return ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM) == 0 ? ESP_OK : ESP_FAIL;
}
