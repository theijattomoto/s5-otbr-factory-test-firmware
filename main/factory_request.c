#include "factory_request.h"

#include <math.h>
#include <string.h>

#include "cJSON.h"
#include "factory_manifest.h"
#include "factory_session.h"

static bool valid_utf8(const unsigned char *text, size_t length)
{
    for (size_t i = 0; i < length;) {
        const unsigned char c = text[i++];
        if (c < 0x80) {
            if (c == 0) return false;
            continue;
        }
        size_t continuation = 0;
        uint32_t codepoint = 0;
        if ((c & 0xE0) == 0xC0) {
            continuation = 1; codepoint = c & 0x1F;
            if (codepoint < 2) return false;
        } else if ((c & 0xF0) == 0xE0) {
            continuation = 2; codepoint = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            continuation = 3; codepoint = c & 0x07;
        } else {
            return false;
        }
        if (i + continuation > length) return false;
        for (size_t j = 0; j < continuation; ++j) {
            const unsigned char next = text[i++];
            if ((next & 0xC0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3F);
        }
        if ((continuation == 2 && codepoint < 0x800) ||
            (continuation == 3 && codepoint < 0x10000) ||
            codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            return false;
        }
    }
    return true;
}

static bool has_duplicate_keys(const cJSON *object)
{
    for (const cJSON *a = object->child; a != NULL; a = a->next) {
        for (const cJSON *b = a->next; b != NULL; b = b->next) {
            if (a->string != NULL && b->string != NULL &&
                strcmp(a->string, b->string) == 0) {
                return true;
            }
        }
    }
    return false;
}

static factory_command_t command_from_text(const char *text)
{
    if (strcmp(text, "identity") == 0) return FACTORY_COMMAND_IDENTITY;
    if (strcmp(text, "session.start") == 0) return FACTORY_COMMAND_SESSION_START;
    if (strcmp(text, "session.list") == 0) return FACTORY_COMMAND_SESSION_LIST;
    if (strcmp(text, "session.finish") == 0) return FACTORY_COMMAND_SESSION_FINISH;
    if (strcmp(text, "session.abort") == 0) return FACTORY_COMMAND_SESSION_ABORT;
    if (strcmp(text, "fixture.record") == 0) return FACTORY_COMMAND_FIXTURE_RECORD;
    if (strcmp(text, "safe") == 0) return FACTORY_COMMAND_SAFE;
    return FACTORY_COMMAND_UNKNOWN;
}

static bool field_allowed(factory_command_t command, const char *field)
{
    if (strcmp(field, "seq") == 0 || strcmp(field, "cmd") == 0) return true;
    if (command == FACTORY_COMMAND_SESSION_START) {
        return strcmp(field, "unit_id") == 0;
    }
    if (command == FACTORY_COMMAND_FIXTURE_RECORD) {
        return strcmp(field, "test_id") == 0 ||
               strcmp(field, "pass") == 0 ||
               strcmp(field, "value") == 0 ||
               strcmp(field, "unit") == 0 ||
               strcmp(field, "detail") == 0;
    }
    return false;
}

static bool copy_json_string(const cJSON *root, const char *name,
                             char *destination, size_t capacity, bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (item == NULL) return !required;
    if (!cJSON_IsString(item) || item->valuestring == NULL) return false;
    const size_t length = strlen(item->valuestring);
    if (length == 0 || length >= capacity) return false;
    memcpy(destination, item->valuestring, length + 1);
    return true;
}

factory_result_t factory_request_parse(const char *line, size_t length,
                                       factory_request_t *request)
{
    if (line == NULL || request == NULL) return FACTORY_ERR_INVALID_ARGUMENT;
    memset(request, 0, sizeof(*request));
    request->seq = -1;
    memcpy(request->command_text, "unknown", sizeof("unknown"));

    if (length > FACTORY_PROTOCOL_MAX_FRAME) return FACTORY_ERR_FRAME_TOO_LONG;
    const size_t prefix_length = sizeof(FACTORY_PROTOCOL_PREFIX) - 1;
    if (length <= prefix_length ||
        memcmp(line, FACTORY_PROTOCOL_PREFIX, prefix_length) != 0) {
        return FACTORY_ERR_INVALID_FRAME;
    }
    if (!valid_utf8((const unsigned char *)line, length)) {
        return FACTORY_ERR_INVALID_REQUEST;
    }

    const char *payload = line + prefix_length;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(payload, length - prefix_length,
                                             &end, false);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return FACTORY_ERR_INVALID_JSON;
    }
    const char *payload_end = payload + (length - prefix_length);
    while (end < payload_end &&
           (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        ++end;
    }
    if (end != payload_end || has_duplicate_keys(root)) {
        cJSON_Delete(root);
        return FACTORY_ERR_INVALID_REQUEST;
    }

    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsNumber(seq) || !isfinite(seq->valuedouble) ||
        seq->valuedouble < 1 || seq->valuedouble > INT32_MAX ||
        floor(seq->valuedouble) != seq->valuedouble ||
        !cJSON_IsString(cmd) || cmd->valuestring == NULL ||
        strlen(cmd->valuestring) == 0 ||
        strlen(cmd->valuestring) >= sizeof(request->command_text)) {
        cJSON_Delete(root);
        return FACTORY_ERR_INVALID_REQUEST;
    }

    request->seq = (int32_t)seq->valuedouble;
    memcpy(request->command_text, cmd->valuestring,
           strlen(cmd->valuestring) + 1);
    request->command = command_from_text(request->command_text);
    if (request->command == FACTORY_COMMAND_UNKNOWN) {
        cJSON_Delete(root);
        return FACTORY_ERR_UNKNOWN_COMMAND;
    }

    for (const cJSON *item = root->child; item != NULL; item = item->next) {
        if (item->string == NULL ||
            !field_allowed(request->command, item->string)) {
            cJSON_Delete(root);
            return FACTORY_ERR_INVALID_REQUEST;
        }
    }

    bool valid = true;
    if (request->command == FACTORY_COMMAND_SESSION_START) {
        valid = copy_json_string(root, "unit_id", request->unit_id,
                                 sizeof(request->unit_id), true);
    } else if (request->command == FACTORY_COMMAND_FIXTURE_RECORD) {
        valid = copy_json_string(root, "test_id", request->test_id,
                                 sizeof(request->test_id), true);
        const cJSON *passed = cJSON_GetObjectItemCaseSensitive(root, "pass");
        valid = valid && cJSON_IsBool(passed);
        if (valid) request->passed = cJSON_IsTrue(passed);

        const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "value");
        if (value != NULL) {
            valid = valid && cJSON_IsNumber(value) &&
                    isfinite(value->valuedouble);
            if (valid) {
                request->has_value = true;
                request->value = value->valuedouble;
            }
        }
        valid = valid &&
            copy_json_string(root, "unit", request->unit,
                             sizeof(request->unit), false) &&
            copy_json_string(root, "detail", request->detail,
                             sizeof(request->detail), false);
    }

    cJSON_Delete(root);
    return valid ? FACTORY_OK : FACTORY_ERR_INVALID_REQUEST;
}
