#include "market_parser.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static cJSON *parse_root(const char *json, size_t length)
{
    if (json == NULL || length == 0) {
        return NULL;
    }
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &parse_end, false);
    if (root == NULL || !cJSON_IsObject(root) || parse_end == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    while (parse_end < json + length &&
           (*parse_end == ' ' || *parse_end == '\t' || *parse_end == '\r' || *parse_end == '\n')) {
        parse_end++;
    }
    if (parse_end != json + length) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool convert_values(
    double points,
    double change_percent,
    int32_t *points_x100,
    int16_t *change_basis_points
)
{
    if (!isfinite(points) || !isfinite(change_percent) || points < 0 || points > 10000000 ||
        change_percent < -100 || change_percent > 100) {
        return false;
    }
    const long long scaled_points = llround(points * 100.0);
    const long scaled_change = lround(change_percent * 100.0);
    if (scaled_points < INT32_MIN || scaled_points > INT32_MAX ||
        scaled_change < INT16_MIN || scaled_change > INT16_MAX) {
        return false;
    }
    *points_x100 = (int32_t)scaled_points;
    *change_basis_points = (int16_t)scaled_change;
    return true;
}

static const char *find_token(const char *begin, const char *end, const char *token)
{
    const size_t token_length = strlen(token);
    if (begin == NULL || end == NULL || token_length == 0 || end < begin ||
        (size_t)(end - begin) < token_length) {
        return NULL;
    }
    for (const char *cursor = begin; cursor + token_length <= end; ++cursor) {
        if (memcmp(cursor, token, token_length) == 0) {
            return cursor;
        }
    }
    return NULL;
}

static bool read_bounded_string(
    const char *begin,
    const char *end,
    const char *key,
    char *output,
    size_t output_capacity
)
{
    char token[40];
    const int token_length = snprintf(token, sizeof(token), "\"%s\"", key);
    if (token_length <= 0 || (size_t)token_length >= sizeof(token) || output_capacity == 0) {
        return false;
    }
    const char *cursor = find_token(begin, end, token);
    if (cursor == NULL) {
        return false;
    }
    cursor += token_length;
    while (cursor < end && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')) {
        cursor++;
    }
    if (cursor >= end || *cursor++ != ':') {
        return false;
    }
    while (cursor < end && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')) {
        cursor++;
    }
    if (cursor >= end || *cursor++ != '"') {
        return false;
    }
    size_t length = 0;
    while (cursor < end && *cursor != '"') {
        if ((unsigned char)*cursor < 0x20U || *cursor == '\\' || length + 1 >= output_capacity) {
            return false;
        }
        output[length++] = *cursor++;
    }
    if (cursor >= end || *cursor != '"') {
        return false;
    }
    output[length] = '\0';
    return true;
}

static bool parse_decimal_text(const char *text, double *value)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    const double parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed)) {
        return false;
    }
    *value = parsed;
    return true;
}

static int slot_for_sse_code(const char *code)
{
    static const struct {
        const char *code;
        int slot;
    } mappings[] = {
        {"000001", 0},
        {"000688", 3},
        {"000300", 4},
        {"000905", 5},
    };
    for (size_t i = 0; i < sizeof(mappings) / sizeof(mappings[0]); ++i) {
        if (strcmp(code, mappings[i].code) == 0) {
            return mappings[i].slot;
        }
    }
    return -1;
}

static const char *display_name_for_slot(int slot)
{
    static const char *const names[DESK_MARKET_INDEX_COUNT] = {
        "上证综指",
        "深证成指",
        "创业板指",
        "科创50",
        "沪深300",
        "中证500",
    };
    return slot >= 0 && slot < DESK_MARKET_INDEX_COUNT ? names[slot] : "";
}

bool desk_market_parse_sse(const char *json, size_t length, desk_market_state_t *state)
{
    if (state == NULL) {
        return false;
    }
    cJSON *root = parse_root(json, length);
    if (root == NULL) {
        return false;
    }
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "list");
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(root);
        return false;
    }

    unsigned found_mask = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, list) {
        if (!cJSON_IsArray(entry) || cJSON_GetArraySize(entry) < 5) {
            continue;
        }
        const cJSON *points = cJSON_GetArrayItem(entry, 1);
        const cJSON *change = cJSON_GetArrayItem(entry, 2);
        const cJSON *code = cJSON_GetArrayItem(entry, 3);
        if (!cJSON_IsNumber(points) || !cJSON_IsNumber(change) || !cJSON_IsString(code) ||
            code->valuestring == NULL) {
            continue;
        }
        const int slot = slot_for_sse_code(code->valuestring);
        if (slot < 0 || !convert_values(
                            points->valuedouble,
                            change->valuedouble,
                            &state->indices[slot].points_x100,
                            &state->indices[slot].change_basis_points
                        )) {
            continue;
        }
        snprintf(state->indices[slot].name, sizeof(state->indices[slot].name), "%s", display_name_for_slot(slot));
        snprintf(state->indices[slot].code, sizeof(state->indices[slot].code), "%s", code->valuestring);
        found_mask |= 1U << (unsigned)slot;
    }
    cJSON_Delete(root);
    const unsigned required_mask = (1U << 0) | (1U << 3) | (1U << 4) | (1U << 5);
    return (found_mask & required_mask) == required_mask;
}

bool desk_market_parse_szse(
    const char *json,
    size_t length,
    const char *expected_code,
    const char *display_name,
    desk_market_index_t *index
)
{
    if (expected_code == NULL || display_name == NULL || index == NULL) {
        return false;
    }
    if (json == NULL || length == 0) {
        return false;
    }
    const char *begin = json;
    const char *end = json + length;
    while (begin < end && (*begin == ' ' || *begin == '\t' || *begin == '\r' || *begin == '\n')) {
        begin++;
    }
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }
    if (end - begin < 2 || *begin != '{' || end[-1] != '}') {
        return false;
    }

    const char *data_token = find_token(begin, end, "\"data\"");
    if (data_token == NULL) {
        return false;
    }
    char root_status[8];
    if (!read_bounded_string(begin, data_token, "code", root_status, sizeof(root_status)) ||
        strcmp(root_status, "0") != 0) {
        return false;
    }
    const char *data_begin = data_token + strlen("\"data\"");
    while (data_begin < end && *data_begin != '{') {
        if (*data_begin != ':' && *data_begin != ' ' && *data_begin != '\t' &&
            *data_begin != '\r' && *data_begin != '\n') {
            return false;
        }
        data_begin++;
    }
    if (data_begin >= end || *data_begin != '{') {
        return false;
    }
    data_begin++;
    const char *data_end = find_token(data_begin, end, "\"picupdata\"");
    if (data_end == NULL) {
        data_end = end;
    }

    char code[8];
    char points_text[32];
    char change_text[32];
    double points = 0;
    double change_percent = 0;
    const bool valid = read_bounded_string(data_begin, data_end, "code", code, sizeof(code)) &&
                       strcmp(code, expected_code) == 0 &&
                       read_bounded_string(data_begin, data_end, "now", points_text, sizeof(points_text)) &&
                       read_bounded_string(
                           data_begin,
                           data_end,
                           "deltaPercent",
                           change_text,
                           sizeof(change_text)
                       ) &&
                       parse_decimal_text(points_text, &points) &&
                       parse_decimal_text(change_text, &change_percent) &&
                       convert_values(points, change_percent, &index->points_x100, &index->change_basis_points);
    if (!valid) {
        return false;
    }
    snprintf(index->name, sizeof(index->name), "%s", display_name);
    snprintf(index->code, sizeof(index->code), "%s", expected_code);
    return true;
}
