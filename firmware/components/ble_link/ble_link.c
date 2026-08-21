#include "ble_link/ble_link.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/bas/ble_svc_bas.h"
#include "services/dis/ble_svc_dis.h"
#include "services/hid/ble_svc_hid.h"

void ble_store_config_init(void);

static const char *TAG = "desk_ble";

enum {
    EVENT_QUEUE_LENGTH = 8,
    REASSEMBLY_TIMEOUT_MS = 2000,
    DEFAULT_ATT_MTU = 23,
    MAX_ATT_MTU = 247,
    ATT_HEADER_SIZE = 3,
    STATUS_VALUE_SIZE = 8,
    CODEX_REPORT_ID = 6,
    CODEX_REPORT_BODY_SIZE = 63,
    CODEX_REPORT_PAYLOAD_SIZE = 61,
    CODEX_RPC_BUFFER_SIZE = 4096,
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
    bool codex_subscribed;
    uint8_t own_address_type;
    uint16_t connection_handle;
    uint16_t mtu;
    uint16_t next_tx_frame_id;
    uint16_t rx_error_count;
    QueueHandle_t event_queue;
    SemaphoreHandle_t codex_send_mutex;
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
static uint16_t codex_input_value_handle;
static uint16_t codex_output_value_handle;
static struct ble_svc_hid_params codex_hid_parameters;
static char codex_rpc_buffer[CODEX_RPC_BUFFER_SIZE + 1];
static size_t codex_rpc_length;

static const uint8_t codex_report_map[] = {
    0x06, 0x00, 0xFF,       /* Usage Page (Vendor Defined 0xFF00) */
    0x09, 0x01,             /* Usage (1) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, CODEX_REPORT_ID,  /* Report ID (6) */
    0x15, 0x00,             /* Logical Minimum (0) */
    0x26, 0xFF, 0x00,       /* Logical Maximum (255) */
    0x75, 0x08,             /* Report Size (8) */
    0x95, 0x3F,             /* Report Count (63) */
    0x09, 0x01,             /* Usage (1) */
    0x81, 0x02,             /* Input */
    0x95, 0x3F,             /* Report Count (63) */
    0x09, 0x02,             /* Usage (2) */
    0x91, 0x02,             /* Output */
    0xC0,
};

static const char codex_pnp_id[8] = {
    0x02, 0x3A, 0x30, 0x60, (char)0x83, 0x01, 0x01, '\0'
};
static const ble_uuid16_t codex_hid_service_uuid = BLE_UUID16_INIT(BLE_SVC_HID_UUID16);

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
    event.codex_subscribed = link_state.codex_subscribed;
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
    event.codex_subscribed = link_state.codex_subscribed;
    taskEXIT_CRITICAL(&link_state.lock);
    memcpy(event.frame, assembly->data, assembly->length);
    reset_reassembly();
    post_event(&event);
    return 0;
}

static desk_codex_slot_status_t codex_status_from_light(
    uint32_t color,
    double brightness,
    const char *effect
)
{
    if (brightness <= 0.01 || (effect != NULL && strcmp(effect, "off") == 0)) {
        return DESK_CODEX_SLOT_UNASSIGNED;
    }
    const uint8_t red = (uint8_t)(color >> 16U);
    const uint8_t green = (uint8_t)(color >> 8U);
    const uint8_t blue = (uint8_t)color;
    const uint8_t maximum = red > green ? (red > blue ? red : blue) : (green > blue ? green : blue);
    const uint8_t minimum = red < green ? (red < blue ? red : blue) : (green < blue ? green : blue);
    if ((uint8_t)(maximum - minimum) < 32U) {
        return DESK_CODEX_SLOT_IDLE;
    }
    if (red > 180U && green < 150U) {
        return DESK_CODEX_SLOT_FAILED;
    }
    if (red > 170U && green > 100U && blue < 120U) {
        return DESK_CODEX_SLOT_WAITING;
    }
    if (green > red && green > blue) {
        return DESK_CODEX_SLOT_COMPLETED;
    }
    if (blue >= red && blue >= green) {
        return DESK_CODEX_SLOT_RUNNING;
    }
    return DESK_CODEX_SLOT_IDLE;
}

static void post_codex_task_status(uint8_t slot, desk_codex_slot_status_t status)
{
    desk_ble_event_t event = {
        .type = DESK_BLE_EVENT_CODEX_TASK_STATUS,
        .codex_slot = slot,
        .codex_status = status,
    };
    taskENTER_CRITICAL(&link_state.lock);
    event.connection_handle = link_state.connection_handle;
    event.mtu = link_state.mtu;
    event.encrypted = link_state.encrypted;
    event.bonded = link_state.bonded;
    event.subscribed = link_state.subscribed;
    event.codex_subscribed = link_state.codex_subscribed;
    taskEXIT_CRITICAL(&link_state.lock);
    post_event(&event);
}

static esp_err_t codex_notify_json(const char *json)
{
    if (json == NULL || codex_input_value_handle == 0 || link_state.codex_send_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(link_state.codex_send_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const size_t json_length = strlen(json);
    const size_t framed_length = json_length + 1U;
    size_t offset = 0;
    esp_err_t outcome = ESP_OK;
    while (offset < framed_length) {
        uint16_t connection_handle;
        bool can_notify;
        taskENTER_CRITICAL(&link_state.lock);
        connection_handle = link_state.connection_handle;
        can_notify = link_state.connected && link_state.encrypted && link_state.codex_subscribed;
        taskEXIT_CRITICAL(&link_state.lock);
        if (!can_notify || connection_handle == BLE_HS_CONN_HANDLE_NONE) {
            outcome = ESP_ERR_INVALID_STATE;
            break;
        }

        const size_t remaining = framed_length - offset;
        const size_t chunk = remaining < CODEX_REPORT_PAYLOAD_SIZE
                                 ? remaining
                                 : CODEX_REPORT_PAYLOAD_SIZE;
        uint8_t report[CODEX_REPORT_BODY_SIZE] = {0};
        report[0] = 2;
        report[1] = (uint8_t)chunk;
        for (size_t i = 0; i < chunk; ++i) {
            const size_t source_offset = offset + i;
            report[2 + i] = source_offset < json_length ? (uint8_t)json[source_offset] : (uint8_t)'\n';
        }

        struct os_mbuf *packet = ble_hs_mbuf_from_flat(report, sizeof(report));
        if (packet == NULL) {
            outcome = ESP_ERR_NO_MEM;
            break;
        }
        const int result = ble_gatts_notify_custom(connection_handle, codex_input_value_handle, packet);
        if (result != 0) {
            ESP_LOGW(TAG, "Codex HID notification failed: %d", result);
            outcome = ESP_FAIL;
            break;
        }
        offset += chunk;
        vTaskDelay(pdMS_TO_TICKS(4));
    }
    xSemaphoreGive(link_state.codex_send_mutex);
    return outcome;
}

static void codex_send_response(cJSON *request_id, cJSON *result, cJSON *error)
{
    cJSON *response = cJSON_CreateObject();
    if (response == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(error);
        return;
    }
    cJSON_AddItemToObject(
        response,
        "id",
        request_id != NULL ? cJSON_Duplicate(request_id, true) : cJSON_CreateNull()
    );
    if (result != NULL) {
        cJSON_AddItemToObject(response, "result", result);
    } else if (error != NULL) {
        cJSON_AddItemToObject(response, "error", error);
    }
    char *serialized = cJSON_PrintUnformatted(response);
    if (serialized != NULL) {
        codex_notify_json(serialized);
        cJSON_free(serialized);
    }
    cJSON_Delete(response);
}

static void codex_send_success(cJSON *request_id)
{
    cJSON *result = cJSON_CreateObject();
    if (result != NULL) {
        cJSON_AddBoolToObject(result, "ok", true);
    }
    codex_send_response(request_id, result, NULL);
}

static void handle_codex_rpc(cJSON *request)
{
    cJSON *method_value = cJSON_GetObjectItemCaseSensitive(request, "method");
    cJSON *request_id = cJSON_GetObjectItemCaseSensitive(request, "id");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(request, "params");
    const char *method = cJSON_IsString(method_value) ? method_value->valuestring : "";

    if (strcmp(method, "sys.version") == 0) {
        cJSON *result = cJSON_CreateObject();
        if (result != NULL) {
            cJSON_AddStringToObject(result, "version", "0.2.0-desk");
        }
        codex_send_response(request_id, result, NULL);
        return;
    }
    if (strcmp(method, "device.status") == 0) {
        cJSON *result = cJSON_CreateObject();
        if (result != NULL) {
            cJSON_AddStringToObject(result, "version", "0.2.0-desk");
            cJSON_AddNumberToObject(result, "profile_index", 0);
            cJSON_AddNumberToObject(result, "layer_index", 1);
            cJSON_AddNumberToObject(result, "battery", 100);
            cJSON_AddBoolToObject(result, "is_charging", false);
        }
        codex_send_response(request_id, result, NULL);
        return;
    }
    if (strcmp(method, "v.oai.thstatus") == 0 && cJSON_IsArray(params)) {
        cJSON *value = NULL;
        cJSON_ArrayForEach(value, params) {
            const cJSON *slot_value = cJSON_GetObjectItemCaseSensitive(value, "id");
            const cJSON *color_value = cJSON_GetObjectItemCaseSensitive(value, "c");
            const cJSON *brightness_value = cJSON_GetObjectItemCaseSensitive(value, "b");
            const cJSON *effect_value = cJSON_GetObjectItemCaseSensitive(value, "e");
            if (!cJSON_IsNumber(slot_value) || slot_value->valueint < 0 || slot_value->valueint >= 6) {
                continue;
            }
            const uint32_t color = cJSON_IsNumber(color_value) ? (uint32_t)color_value->valuedouble : 0U;
            const double brightness = cJSON_IsNumber(brightness_value) ? brightness_value->valuedouble : 0.0;
            const char *effect = cJSON_IsString(effect_value) ? effect_value->valuestring : "off";
            post_codex_task_status(
                (uint8_t)slot_value->valueint,
                codex_status_from_light(color, brightness, effect)
            );
        }
        codex_send_success(request_id);
        return;
    }
    if (strcmp(method, "v.oai.rgbcfg") == 0 ||
        strcmp(method, "lights.preview") == 0 ||
        strcmp(method, "host.focused_app") == 0) {
        codex_send_success(request_id);
        return;
    }

    cJSON *error = cJSON_CreateObject();
    if (error != NULL) {
        cJSON_AddNumberToObject(error, "code", -32601);
        cJSON_AddStringToObject(error, "message", "Method not found");
    }
    codex_send_response(request_id, NULL, error);
}

static void codex_report_written(
    uint16_t attribute_handle,
    uint8_t report_type,
    uint8_t report_id,
    const uint8_t *data,
    uint16_t length
)
{
    (void)attribute_handle;
    if (report_type != BLE_SVC_HID_RPT_TYPE_OUTPUT || report_id != CODEX_REPORT_ID ||
        data == NULL || length < 2) {
        return;
    }

    size_t input_offset = length >= CODEX_REPORT_BODY_SIZE + 1U && data[0] == CODEX_REPORT_ID ? 1U : 0U;
    if (length < input_offset + 2U || data[input_offset] != 2U) {
        return;
    }
    size_t payload_length = data[input_offset + 1U];
    if (payload_length > CODEX_REPORT_PAYLOAD_SIZE || length < input_offset + 2U + payload_length) {
        return;
    }
    const char *payload = (const char *)&data[input_offset + 2U];
    static const char method_prefix[] = "{\"method\"";
    if (payload_length >= sizeof(method_prefix) - 1U &&
        memcmp(payload, method_prefix, sizeof(method_prefix) - 1U) == 0 && codex_rpc_length != 0U) {
        codex_rpc_length = 0;
    }
    if (codex_rpc_length == 0U) {
        while (payload_length > 0U && *payload != '{') {
            payload++;
            payload_length--;
        }
    }
    if (payload_length == 0U || codex_rpc_length + payload_length > CODEX_RPC_BUFFER_SIZE) {
        codex_rpc_length = 0;
        return;
    }
    memcpy(&codex_rpc_buffer[codex_rpc_length], payload, payload_length);
    codex_rpc_length += payload_length;
    codex_rpc_buffer[codex_rpc_length] = '\0';

    cJSON *request = cJSON_ParseWithLength(codex_rpc_buffer, codex_rpc_length);
    if (request == NULL) {
        return;
    }
    handle_codex_rpc(request);
    cJSON_Delete(request);
    codex_rpc_length = 0;
}

static void gatt_register_callback(struct ble_gatt_register_ctxt *context, void *argument)
{
    (void)argument;
    if (context->op != BLE_GATT_REGISTER_OP_CHR ||
        ble_uuid_u16(context->chr.svc_def->uuid) != BLE_SVC_HID_UUID16 ||
        ble_uuid_u16(context->chr.chr_def->uuid) != BLE_SVC_HID_CHR_UUID16_RPT) {
        return;
    }
    if ((context->chr.chr_def->flags & BLE_GATT_CHR_F_NOTIFY) != 0U) {
        codex_input_value_handle = context->chr.val_handle;
    } else {
        codex_output_value_handle = context->chr.val_handle;
    }
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
    advertising_fields.uuids16 = (ble_uuid16_t *)&codex_hid_service_uuid;
    advertising_fields.num_uuids16 = 1;
    advertising_fields.uuids16_is_complete = 1;
    advertising_fields.uuids128 = (ble_uuid128_t *)&service_uuid;
    advertising_fields.num_uuids128 = 1;
    advertising_fields.uuids128_is_complete = 1;
    advertising_fields.appearance = 0x03C0;
    advertising_fields.appearance_is_present = 1;

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
            link_state.codex_subscribed = false;
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
                event->subscribe.attr_handle == status_value_handle ||
                event->subscribe.attr_handle == codex_input_value_handle) {
                taskENTER_CRITICAL(&link_state.lock);
                if (event->subscribe.attr_handle == tx_value_handle) {
                    link_state.subscribed = event->subscribe.cur_notify != 0;
                } else if (event->subscribe.attr_handle == codex_input_value_handle) {
                    link_state.codex_subscribed = event->subscribe.cur_notify != 0;
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
#if defined(CONFIG_DESK_ALLOW_APP_ENROLLMENT) && CONFIG_DESK_ALLOW_APP_ENROLLMENT
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
            /* A bonded Mac can legitimately request fresh keys after the
             * firmware gains the HID service.  Accept that migration only
             * while the current link is already encrypted with the old bond;
             * an unknown central still cannot replace the enrolled peer. */
            bool trusted_migration;
            taskENTER_CRITICAL(&link_state.lock);
            trusted_migration =
                link_state.connected &&
                link_state.encrypted &&
                link_state.connection_handle == event->repeat_pairing.conn_handle;
            taskEXIT_CRITICAL(&link_state.lock);
            if (trusted_migration) {
                struct ble_gap_conn_desc description;
                const int result = ble_gap_conn_find(
                    event->repeat_pairing.conn_handle,
                    &description
                );
                if (result == 0) {
                    ESP_LOGW(TAG, "Migrating the trusted Mac bond for the updated services");
                    ble_store_util_delete_peer(&description.peer_id_addr);
                    return BLE_GAP_REPEAT_PAIRING_RETRY;
                }
                ESP_LOGW(TAG, "Trusted bond migration inspection failed: %d", result);
            }
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

    /* The device exposes both the private desk-console service and the Codex
     * HID service.  macOS aggressively caches the attribute table for bonded
     * peripherals, so publish a Service Changed indication on every boot.
     * NimBLE remembers it for bonded peers that are currently disconnected.
     * This also makes future firmware upgrades self-healing without asking the
     * user to remove and pair the device again just to refresh GATT handles. */
    ble_svc_gatt_changed(0x0001, 0xffff);

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
    link_state.codex_send_mutex = xSemaphoreCreateMutex();
    if (link_state.codex_send_mutex == NULL) {
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
    ble_hs_cfg.gatts_register_cb = gatt_register_callback;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    /* Keep the private service before newly added standard services.  Existing
     * macOS installations cache these handles, and preserving their order lets
     * the signed helper reconnect across this firmware upgrade immediately. */
    int host_result = ble_gatts_count_cfg(gatt_services);
    if (host_result == 0) {
        host_result = ble_gatts_add_svcs(gatt_services);
    }

    memset(&codex_hid_parameters, 0, sizeof(codex_hid_parameters));
    codex_hid_parameters.rpts_len = 2;
    codex_hid_parameters.rpts[0].type = BLE_SVC_HID_RPT_TYPE_INPUT;
    codex_hid_parameters.rpts[0].id = CODEX_REPORT_ID;
    codex_hid_parameters.rpts[0].len = CODEX_REPORT_BODY_SIZE;
    codex_hid_parameters.rpts[1].type = BLE_SVC_HID_RPT_TYPE_OUTPUT;
    codex_hid_parameters.rpts[1].id = CODEX_REPORT_ID;
    codex_hid_parameters.rpts[1].len = CODEX_REPORT_BODY_SIZE;
    memcpy(codex_hid_parameters.report_map, codex_report_map, sizeof(codex_report_map));
    codex_hid_parameters.report_map_len = sizeof(codex_report_map);
    codex_hid_parameters.external_rpt_ref = 0x2A19;
    codex_hid_parameters.hid_info = 0x01000111U;

    if (host_result == 0) {
        host_result = ble_svc_hid_add(codex_hid_parameters);
    }
    if (host_result == 0) {
        ble_svc_hid_register_report_write_cb(codex_report_written);
        ble_svc_hid_init();
        ble_svc_bas_init();
        ble_svc_dis_init();
        ble_svc_dis_manufacturer_name_set("Work Louder");
        ble_svc_dis_firmware_revision_set("0.2.0-desk");
        ble_svc_dis_pnp_id_set(codex_pnp_id);
        ble_svc_bas_battery_level_set(100);
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

esp_err_t desk_ble_link_codex_send_key(const char *key_id, uint8_t action, int8_t agent)
{
    if (key_id == NULL || key_id[0] == '\0' || action > 2U || agent < -1 || agent > 5) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *message = cJSON_CreateObject();
    cJSON *params = message != NULL ? cJSON_AddObjectToObject(message, "params") : NULL;
    if (message == NULL || params == NULL ||
        cJSON_AddStringToObject(message, "method", "v.oai.hid") == NULL ||
        cJSON_AddStringToObject(params, "k", key_id) == NULL ||
        cJSON_AddNumberToObject(params, "act", action) == NULL) {
        cJSON_Delete(message);
        return ESP_ERR_NO_MEM;
    }
    if (agent >= 0 && cJSON_AddNumberToObject(params, "ag", agent) == NULL) {
        cJSON_Delete(message);
        return ESP_ERR_NO_MEM;
    }
    char *serialized = cJSON_PrintUnformatted(message);
    cJSON_Delete(message);
    if (serialized == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t result = codex_notify_json(serialized);
    cJSON_free(serialized);
    return result;
}

esp_err_t desk_ble_link_codex_send_joystick(float angle, float distance)
{
    if (!isfinite(angle) || !isfinite(distance) || angle < 0.0f || angle > 1.0f ||
        distance < 0.0f || distance > 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *message = cJSON_CreateObject();
    cJSON *params = message != NULL ? cJSON_AddObjectToObject(message, "params") : NULL;
    if (message == NULL || params == NULL ||
        cJSON_AddStringToObject(message, "method", "v.oai.rad") == NULL ||
        cJSON_AddNumberToObject(params, "a", angle) == NULL ||
        cJSON_AddNumberToObject(params, "d", distance) == NULL) {
        cJSON_Delete(message);
        return ESP_ERR_NO_MEM;
    }
    char *serialized = cJSON_PrintUnformatted(message);
    cJSON_Delete(message);
    if (serialized == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t result = codex_notify_json(serialized);
    cJSON_free(serialized);
    return result;
}

bool desk_ble_link_codex_ready(void)
{
    bool ready;
    taskENTER_CRITICAL(&link_state.lock);
    ready = link_state.connected && link_state.encrypted && link_state.codex_subscribed;
    taskEXIT_CRITICAL(&link_state.lock);
    return ready;
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
