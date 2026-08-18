#include "ble_protocol/ble_protocol.h"

#include <string.h>

static void write_u16_le(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value & 0xFFU);
    output[1] = (uint8_t)(value >> 8U);
}

static uint16_t read_u16_le(const uint8_t *input)
{
    return (uint16_t)input[0] | ((uint16_t)input[1] << 8U);
}

uint16_t desk_protocol_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFFU;
    if (data == NULL && length != 0) {
        return 0;
    }

    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8U;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) != 0 ? (uint16_t)((crc << 1U) ^ 0x1021U) : (uint16_t)(crc << 1U);
        }
    }
    return crc;
}

desk_protocol_status_t desk_protocol_encode(
    const desk_protocol_frame_t *frame,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length
)
{
    if (frame == NULL || output == NULL || output_length == NULL) {
        return DESK_PROTOCOL_INVALID_ARGUMENT;
    }
    if (frame->payload_length > DESK_PROTOCOL_MAX_PAYLOAD_SIZE ||
        (frame->payload_length > 0 && frame->payload == NULL)) {
        return DESK_PROTOCOL_BAD_LENGTH;
    }

    const size_t total_length = DESK_PROTOCOL_HEADER_SIZE + frame->payload_length + DESK_PROTOCOL_CRC_SIZE;
    if (output_capacity < total_length) {
        return DESK_PROTOCOL_BUFFER_TOO_SMALL;
    }

    output[0] = DESK_PROTOCOL_MAGIC_0;
    output[1] = DESK_PROTOCOL_MAGIC_1;
    output[2] = DESK_PROTOCOL_VERSION;
    output[3] = frame->flags;
    write_u16_le(&output[4], frame->message_type);
    write_u16_le(&output[6], frame->sequence);
    write_u16_le(&output[8], frame->payload_length);

    if (frame->payload_length > 0) {
        memcpy(&output[DESK_PROTOCOL_HEADER_SIZE], frame->payload, frame->payload_length);
    }

    const uint16_t crc = desk_protocol_crc16(output, DESK_PROTOCOL_HEADER_SIZE + frame->payload_length);
    write_u16_le(&output[DESK_PROTOCOL_HEADER_SIZE + frame->payload_length], crc);
    *output_length = total_length;
    return DESK_PROTOCOL_OK;
}

desk_protocol_status_t desk_protocol_decode(
    const uint8_t *input,
    size_t input_length,
    desk_protocol_frame_t *frame
)
{
    if (input == NULL || frame == NULL) {
        return DESK_PROTOCOL_INVALID_ARGUMENT;
    }
    if (input_length < DESK_PROTOCOL_HEADER_SIZE + DESK_PROTOCOL_CRC_SIZE) {
        return DESK_PROTOCOL_BAD_LENGTH;
    }
    if (input[0] != DESK_PROTOCOL_MAGIC_0 || input[1] != DESK_PROTOCOL_MAGIC_1) {
        return DESK_PROTOCOL_BAD_MAGIC;
    }
    if (input[2] != DESK_PROTOCOL_VERSION) {
        return DESK_PROTOCOL_UNSUPPORTED_VERSION;
    }

    const uint16_t payload_length = read_u16_le(&input[8]);
    if (payload_length > DESK_PROTOCOL_MAX_PAYLOAD_SIZE) {
        return DESK_PROTOCOL_BAD_LENGTH;
    }
    const size_t expected_length = DESK_PROTOCOL_HEADER_SIZE + payload_length + DESK_PROTOCOL_CRC_SIZE;
    if (input_length != expected_length) {
        return DESK_PROTOCOL_BAD_LENGTH;
    }

    const uint16_t expected_crc = read_u16_le(&input[DESK_PROTOCOL_HEADER_SIZE + payload_length]);
    const uint16_t actual_crc = desk_protocol_crc16(input, DESK_PROTOCOL_HEADER_SIZE + payload_length);
    if (actual_crc != expected_crc) {
        return DESK_PROTOCOL_BAD_CRC;
    }

    frame->flags = input[3];
    frame->message_type = read_u16_le(&input[4]);
    frame->sequence = read_u16_le(&input[6]);
    frame->payload = &input[DESK_PROTOCOL_HEADER_SIZE];
    frame->payload_length = payload_length;
    return DESK_PROTOCOL_OK;
}

desk_sequence_result_t desk_sequence_window_accept(
    desk_sequence_window_t *window,
    uint16_t sequence
)
{
    if (window == NULL) {
        return DESK_SEQUENCE_TOO_OLD;
    }

    if (!window->initialized) {
        window->initialized = true;
        window->newest_sequence = sequence;
        window->seen_bitmap = 1U;
        return DESK_SEQUENCE_ACCEPTED;
    }

    const uint16_t forward = (uint16_t)(sequence - window->newest_sequence);
    if (forward != 0 && forward < 0x8000U) {
        window->seen_bitmap = forward >= DESK_PROTOCOL_SEQUENCE_WINDOW_SIZE
                                  ? 1U
                                  : (window->seen_bitmap << forward) | 1U;
        window->newest_sequence = sequence;
        return DESK_SEQUENCE_ACCEPTED;
    }

    if (forward == 0) {
        return DESK_SEQUENCE_DUPLICATE;
    }

    const uint16_t age = (uint16_t)(window->newest_sequence - sequence);
    if (age >= DESK_PROTOCOL_SEQUENCE_WINDOW_SIZE) {
        return DESK_SEQUENCE_TOO_OLD;
    }

    const uint32_t mask = 1U << age;
    if ((window->seen_bitmap & mask) != 0) {
        return DESK_SEQUENCE_DUPLICATE;
    }

    window->seen_bitmap |= mask;
    return DESK_SEQUENCE_ACCEPTED;
}
