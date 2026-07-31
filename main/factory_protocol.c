#include "factory_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "factory_board.h"
#include "factory_identity.h"
#include "factory_manifest.h"
#include "factory_request.h"
#include "factory_safety.h"
#include "factory_session.h"

#define USB_RX_BUFFER_SIZE 1024
#define USB_TX_BUFFER_SIZE 1024
#define PROTOCOL_POLL_MS   20

static void usb_write_all(const char *text, size_t length)
{
    size_t written = 0;
    while (written < length) {
        const int result = usb_serial_jtag_write_bytes(
            text + written, length - written, pdMS_TO_TICKS(100));
        if (result <= 0) {
            break;
        }
        written += (size_t)result;
    }
}

static cJSON *response_data_create(void)
{
    cJSON *data = cJSON_CreateObject();
    return data;
}

static void send_response(int32_t sequence, const char *command,
                          factory_result_t result, const char *success_code,
                          cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    if (data == NULL) {
        data = cJSON_CreateObject();
    }

    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        static const char fallback[] =
            FACTORY_PROTOCOL_PREFIX
            "{\"seq\":0,\"cmd\":\"internal\",\"status\":\"error\","
            "\"code\":\"no_memory\",\"firmware\":\"0.1.0\","
            "\"protocol\":\"1.0\",\"product\":\"S5-NODE-OTBR\","
            "\"board\":\"TBD\",\"data\":{}}\n";
        usb_write_all(fallback, sizeof(fallback) - 1);
        return;
    }

    cJSON_AddNumberToObject(root, "seq", sequence);
    cJSON_AddStringToObject(root, "cmd",
                            command != NULL ? command : "unknown");
    cJSON_AddStringToObject(root, "status",
                            result == FACTORY_OK ? "ok" : "error");
    cJSON_AddStringToObject(root, "code",
                            result == FACTORY_OK
                                ? (success_code != NULL ? success_code : "ok")
                                : factory_result_code(result));
    cJSON_AddStringToObject(root, "firmware", FACTORY_FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "protocol", FACTORY_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "product", FACTORY_PRODUCT_NAME);
    cJSON_AddStringToObject(root, "board", FACTORY_BOARD_REVISION);
    cJSON_AddItemToObject(root, "data", data);

    char *json = cJSON_PrintUnformatted(root);
    if (json != NULL) {
        usb_write_all(FACTORY_PROTOCOL_PREFIX,
                      sizeof(FACTORY_PROTOCOL_PREFIX) - 1);
        usb_write_all(json, strlen(json));
        usb_write_all("\n", 1);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}

static void add_identity(cJSON *data, const factory_identity_t *identity)
{
    cJSON_AddStringToObject(data, "target", identity->target);
    cJSON_AddNumberToObject(data, "silicon_revision",
                            identity->silicon_revision);
    cJSON_AddNumberToObject(data, "cores", identity->core_count);
    cJSON_AddNumberToObject(data, "flash_size_bytes",
                            identity->flash_size_bytes);
    cJSON_AddStringToObject(data, "base_mac", identity->base_mac);
    cJSON_AddStringToObject(data, "thread_eui64", identity->thread_eui64);
    cJSON_AddNumberToObject(data, "reset_reason", identity->reset_reason);
}

static void add_session(cJSON *data,
                        const factory_session_snapshot_t *snapshot)
{
    cJSON_AddStringToObject(data, "state",
                            factory_session_state_name(snapshot->state));
    cJSON_AddBoolToObject(data, "active",
                          snapshot->state == FACTORY_SESSION_ACTIVE);
    cJSON_AddStringToObject(data, "unit_id", snapshot->unit_id);
    cJSON_AddNumberToObject(data, "pending", snapshot->manifest.pending);
    cJSON_AddNumberToObject(data, "passed", snapshot->manifest.passed);
    cJSON_AddNumberToObject(data, "failed", snapshot->manifest.failed);
}

static factory_result_t require_session(void)
{
    return factory_session_is_active() ? FACTORY_OK :
                                         FACTORY_ERR_INVALID_STATE;
}

static factory_result_t mark_identity_results(void)
{
    static const char *const ids[] = {
        "device_identity",
        "usb_protocol",
        "base_mac",
        "thread_eui64",
        "firmware_identity",
    };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        const factory_result_t result =
            factory_manifest_record(ids[i], FACTORY_TEST_AUTOMATIC, true,
                                    false, 0.0, NULL,
                                    "verified during session.start");
        if (result != FACTORY_OK) {
            return result;
        }
    }
    return FACTORY_OK;
}

static factory_result_t handle_request(const factory_request_t *request,
                                       cJSON *data,
                                       const char **success_code)
{
    *success_code = "ok";

    if (request->command == FACTORY_COMMAND_IDENTITY) {
        factory_identity_t identity;
        const factory_result_t result = factory_identity_read(&identity);
        if (result == FACTORY_OK) add_identity(data, &identity);
        return result;
    }

    if (request->command == FACTORY_COMMAND_SAFE) {
        return factory_safety_apply();
    }

    if (request->command == FACTORY_COMMAND_SESSION_ABORT) {
        const factory_result_t result = factory_session_abort();
        factory_session_snapshot_t snapshot;
        factory_session_snapshot(&snapshot);
        add_session(data, &snapshot);
        return result;
    }

    if (request->command == FACTORY_COMMAND_SESSION_START) {
        factory_identity_t identity;
        factory_result_t result = factory_identity_read(&identity);
        if (result != FACTORY_OK) return result;

        result = factory_session_start(request->unit_id,
                                       esp_timer_get_time());
        if (result != FACTORY_OK) return result;

        result = mark_identity_results();
        if (result != FACTORY_OK) {
            (void)factory_session_abort();
            return result;
        }

        factory_session_snapshot_t snapshot;
        factory_session_snapshot(&snapshot);
        add_identity(data, &identity);
        add_session(data, &snapshot);
        return FACTORY_OK;
    }

    factory_result_t result = require_session();
    if (result != FACTORY_OK) return result;

    if (request->command == FACTORY_COMMAND_SESSION_LIST) {
        result = factory_session_touch(esp_timer_get_time());
        if (result != FACTORY_OK) return result;

        cJSON *tests = cJSON_AddArrayToObject(data, "tests");
        if (tests == NULL) return FACTORY_ERR_NO_MEMORY;

        for (size_t i = 0; i < factory_manifest_count(); ++i) {
            factory_manifest_entry_t entry;
            if (factory_manifest_get(i, &entry) != FACTORY_OK) {
                return FACTORY_ERR_INVALID_STATE;
            }
            cJSON *item = cJSON_CreateObject();
            if (item == NULL) return FACTORY_ERR_NO_MEMORY;
            cJSON_AddStringToObject(item, "id", entry.id);
            cJSON_AddStringToObject(item, "description", entry.description);
            cJSON_AddStringToObject(item, "owner",
                                    factory_test_owner_name(entry.owner));
            cJSON_AddStringToObject(item, "status",
                                    factory_test_status_name(entry.status));
            if (entry.has_value) {
                cJSON_AddNumberToObject(item, "value", entry.value);
                if (entry.unit[0] != '\0') {
                    cJSON_AddStringToObject(item, "unit", entry.unit);
                }
            }
            if (entry.detail[0] != '\0') {
                cJSON_AddStringToObject(item, "detail", entry.detail);
            }
            cJSON_AddItemToArray(tests, item);
        }
        factory_session_snapshot_t snapshot;
        factory_session_snapshot(&snapshot);
        add_session(data, &snapshot);
        return FACTORY_OK;
    }

    if (request->command == FACTORY_COMMAND_FIXTURE_RECORD) {
        result = factory_manifest_record(
            request->test_id, FACTORY_TEST_STATION, request->passed,
            request->has_value, request->value,
            request->unit[0] != '\0' ? request->unit : NULL,
            request->detail[0] != '\0' ? request->detail : NULL);
        if (result != FACTORY_OK) return result;
        result = factory_session_touch(esp_timer_get_time());
        if (result != FACTORY_OK) return result;

        cJSON_AddStringToObject(data, "test_id", request->test_id);
        cJSON_AddBoolToObject(data, "pass", request->passed);
        if (!request->passed && factory_safety_apply() != FACTORY_OK) {
            return FACTORY_ERR_CLEANUP;
        }
        return FACTORY_OK;
    }

    if (request->command == FACTORY_COMMAND_SESSION_FINISH) {
        factory_session_snapshot_t snapshot;
        result = factory_session_finish(&snapshot);
        add_session(data, &snapshot);
        cJSON_AddBoolToObject(data, "overall_pass",
                              result == FACTORY_OK);
        if (result == FACTORY_OK) *success_code = "pass";
        return result;
    }

    return FACTORY_ERR_UNKNOWN_COMMAND;
}

static void process_line(const char *line, size_t length)
{
    factory_request_t request;
    factory_result_t result = factory_request_parse(line, length, &request);
    cJSON *data = response_data_create();
    const char *success_code = "ok";

    if (result == FACTORY_OK) {
        result = handle_request(&request, data, &success_code);
    }

    result = factory_apply_error_cleanup(result, factory_safety_apply);

    send_response(request.seq, request.command_text, result, success_code,
                  data);
}

static void send_ready(void)
{
    cJSON *data = response_data_create();
    if (data != NULL) {
        cJSON_AddNumberToObject(data, "max_frame_bytes",
                                FACTORY_PROTOCOL_MAX_FRAME);
        cJSON_AddNumberToObject(data, "session_timeout_s",
                                FACTORY_SESSION_TIMEOUT_US / 1000000LL);
        cJSON_AddStringToObject(data, "psw_en_policy",
                                "unresolved_high_impedance");
        cJSON_AddBoolToObject(data, "production_release_capable", false);
    }
    send_response(0, "ready", FACTORY_OK, "ok", data);
}

factory_result_t factory_protocol_init(void)
{
    usb_serial_jtag_driver_config_t config = {
        .rx_buffer_size = USB_RX_BUFFER_SIZE,
        .tx_buffer_size = USB_TX_BUFFER_SIZE,
    };
    if (usb_serial_jtag_driver_install(&config) != ESP_OK) {
        return FACTORY_ERR_HARDWARE;
    }
    return factory_session_init(factory_safety_apply);
}

void factory_protocol_run(void)
{
    char line[FACTORY_PROTOCOL_MAX_FRAME + 1];
    size_t length = 0;
    bool discard = false;

    send_ready();

    for (;;) {
        uint8_t byte = 0;
        const int read = usb_serial_jtag_read_bytes(
            &byte, 1, pdMS_TO_TICKS(PROTOCOL_POLL_MS));

        bool expired = false;
        const factory_result_t expiry =
            factory_session_expire(esp_timer_get_time(), &expired);
        if (expired) {
            cJSON *data = response_data_create();
            if (data != NULL) {
                cJSON_AddBoolToObject(data, "outputs_safe",
                                      expiry == FACTORY_ERR_TIMEOUT);
            }
            send_response(0, "session.timeout",
                          expiry == FACTORY_ERR_TIMEOUT
                              ? FACTORY_ERR_TIMEOUT : FACTORY_ERR_CLEANUP,
                          NULL, data);
        }

        if (read != 1) continue;

        if (byte == '\n' || byte == '\r') {
            if (discard) {
                send_response(0, "unknown", FACTORY_ERR_FRAME_TOO_LONG,
                              NULL, response_data_create());
                discard = false;
                length = 0;
            } else if (length > 0) {
                process_line(line, length);
                length = 0;
            }
            continue;
        }

        if (discard) continue;

        if (length < FACTORY_PROTOCOL_MAX_FRAME) {
            line[length++] = (char)byte;
        } else {
            discard = true;
            length = 0;
            (void)factory_safety_apply();
        }
    }
}
