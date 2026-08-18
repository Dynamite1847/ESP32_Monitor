#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DESK_PROTOCOL_MAGIC_0 0x44
#define DESK_PROTOCOL_MAGIC_1 0x43
#define DESK_PROTOCOL_VERSION 1
#define DESK_PROTOCOL_HEADER_SIZE 10
#define DESK_PROTOCOL_CRC_SIZE 2
#define DESK_PROTOCOL_MAX_PAYLOAD_SIZE 512
#define DESK_PROTOCOL_MAX_FRAME_SIZE \
    (DESK_PROTOCOL_HEADER_SIZE + DESK_PROTOCOL_MAX_PAYLOAD_SIZE + DESK_PROTOCOL_CRC_SIZE)
#define DESK_PROTOCOL_SEQUENCE_WINDOW_SIZE 32

typedef enum {
    DESK_PROTOCOL_OK = 0,
    DESK_PROTOCOL_INVALID_ARGUMENT,
    DESK_PROTOCOL_BUFFER_TOO_SMALL,
    DESK_PROTOCOL_BAD_MAGIC,
    DESK_PROTOCOL_UNSUPPORTED_VERSION,
    DESK_PROTOCOL_BAD_LENGTH,
    DESK_PROTOCOL_BAD_CRC,
} desk_protocol_status_t;

typedef enum {
    DESK_MESSAGE_HELLO = 0x0001,
    DESK_MESSAGE_AUTH_CHALLENGE = 0x0002,
    DESK_MESSAGE_AUTH_RESPONSE = 0x0003,
    DESK_MESSAGE_AUTH_RESULT = 0x0004,
    DESK_MESSAGE_HEARTBEAT = 0x0005,
    DESK_MESSAGE_LOCK = 0x0006,
    DESK_MESSAGE_DEVICE_STATUS = 0x0010,

    DESK_MESSAGE_SYSTEM_STATE = 0x0100,
    DESK_MESSAGE_CONTROL_LAYOUT = 0x0110,
    DESK_MESSAGE_AI_STATE = 0x0120,
    DESK_MESSAGE_MEDIA_STATE = 0x0130,

    DESK_MESSAGE_ACTION_TRIGGER = 0x0200,
    DESK_MESSAGE_SLIDER_UPDATE = 0x0201,
    DESK_MESSAGE_OPEN_APP = 0x0202,

    DESK_MESSAGE_WIFI_PROVISION = 0x0300,
    DESK_MESSAGE_WIFI_RESULT = 0x0301,
    DESK_MESSAGE_WEATHER_CONFIG = 0x0310,
    DESK_MESSAGE_WEATHER_CONFIG_RESULT = 0x0311,
} desk_message_type_t;

typedef enum {
    DESK_FRAME_FLAG_NONE = 0,
    DESK_FRAME_FLAG_RESPONSE = 1U << 0,
    DESK_FRAME_FLAG_ERROR = 1U << 1,
    DESK_FRAME_FLAG_ACK_REQUIRED = 1U << 2,
} desk_frame_flags_t;

typedef struct {
    uint8_t flags;
    uint16_t message_type;
    uint16_t sequence;
    /** For decoded frames this points into the input buffer and shares its lifetime. */
    const uint8_t *payload;
    uint16_t payload_length;
} desk_protocol_frame_t;

typedef enum {
    DESK_SEQUENCE_ACCEPTED = 0,
    DESK_SEQUENCE_DUPLICATE,
    DESK_SEQUENCE_TOO_OLD,
} desk_sequence_result_t;

/**
 * Tracks the most recent 32 sequence numbers for one direction of one session.
 * Reset the window whenever a new authenticated BLE session starts.
 */
typedef struct {
    uint16_t newest_sequence;
    uint32_t seen_bitmap;
    bool initialized;
} desk_sequence_window_t;

uint16_t desk_protocol_crc16(const uint8_t *data, size_t length);

/** frame->payload and output must refer to non-overlapping storage. */
desk_protocol_status_t desk_protocol_encode(
    const desk_protocol_frame_t *frame,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
);

desk_protocol_status_t desk_protocol_decode(
    const uint8_t *input,
    size_t input_length,
    desk_protocol_frame_t *frame
);

/**
 * Accept a sequence number once, including limited out-of-order delivery.
 * The uint16_t sequence space is wraparound-safe as long as a peer never jumps
 * forward by more than 32767 messages in one session.
 */
desk_sequence_result_t desk_sequence_window_accept(
    desk_sequence_window_t *window,
    uint16_t sequence
);

#ifdef __cplusplus
}
#endif
