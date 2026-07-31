#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "factory_board.h"
#include "factory_manifest.h"
#include "factory_request.h"
#include "factory_safety.h"
#include "factory_session.h"
#include "factory_types.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,         \
                    #condition);                                             \
            return false;                                                    \
        }                                                                    \
    } while (0)

static int s_cleanup_calls;
static bool s_cleanup_fails;

static factory_result_t cleanup_mock(void)
{
    ++s_cleanup_calls;
    return s_cleanup_fails ? FACTORY_ERR_HARDWARE : FACTORY_OK;
}

static factory_result_t parse(const char *line, factory_request_t *request)
{
    return factory_request_parse(line, strlen(line), request);
}

static bool test_protocol_validation(void)
{
    factory_request_t request;
    CHECK(parse("@S5OTBRFT {\"seq\":1,\"cmd\":\"identity\"}", &request)
          == FACTORY_OK);
    CHECK(request.seq == 1);
    CHECK(request.command == FACTORY_COMMAND_IDENTITY);

    CHECK(parse("@S5OTBRFT {\"seq\":2,\"cmd\":\"session.start\","
                "\"unit_id\":\"PCB-001\"}", &request) == FACTORY_OK);
    CHECK(strcmp(request.unit_id, "PCB-001") == 0);

    CHECK(parse("@S5OTBRFT {\"seq\":3,\"cmd\":\"fixture.record\","
                "\"test_id\":\"rail_3v3\",\"pass\":true,"
                "\"value\":3.3,\"unit\":\"V\",\"detail\":\"fixture\"}",
                &request) == FACTORY_OK);
    CHECK(request.passed && request.has_value && request.value == 3.3);

    CHECK(parse("{\"seq\":1,\"cmd\":\"identity\"}", &request)
          == FACTORY_ERR_INVALID_FRAME);
    CHECK(parse("@S5OTBRFT {bad}", &request) == FACTORY_ERR_INVALID_JSON);
    CHECK(parse("@S5OTBRFT {\"cmd\":\"identity\"}", &request)
          == FACTORY_ERR_INVALID_REQUEST);
    CHECK(parse("@S5OTBRFT {\"seq\":\"1\",\"cmd\":\"identity\"}", &request)
          == FACTORY_ERR_INVALID_REQUEST);
    CHECK(parse("@S5OTBRFT {\"seq\":1.5,\"cmd\":\"identity\"}", &request)
          == FACTORY_ERR_INVALID_REQUEST);
    CHECK(parse("@S5OTBRFT {\"seq\":0,\"cmd\":\"identity\"}", &request)
          == FACTORY_ERR_INVALID_REQUEST);
    CHECK(parse("@S5OTBRFT {\"seq\":1,\"seq\":2,\"cmd\":\"identity\"}",
                &request) == FACTORY_ERR_INVALID_REQUEST);
    CHECK(parse("@S5OTBRFT {\"seq\":1,\"cmd\":\"identity\",\"extra\":1}",
                &request) == FACTORY_ERR_INVALID_REQUEST);
    CHECK(parse("@S5OTBRFT {\"seq\":1,\"cmd\":\"not.real\"}", &request)
          == FACTORY_ERR_UNKNOWN_COMMAND);
    CHECK(parse("@S5OTBRFT {\"seq\":1,\"cmd\":\"identity\"}x", &request)
          == FACTORY_ERR_INVALID_REQUEST);
    CHECK(parse("@S5OTBRFT {\"seq\":1,\"cmd\":\"fixture.record\","
                "\"test_id\":\"rail_3v3\",\"pass\":1}", &request)
          == FACTORY_ERR_INVALID_REQUEST);
    CHECK(parse("@S5OTBRFT {\"seq\":1,\"cmd\":\"fixture.record\","
                "\"test_id\":\"rail_3v3\",\"pass\":true,\"value\":NaN}",
                &request) != FACTORY_OK);

    char oversized[FACTORY_PROTOCOL_MAX_FRAME + 2];
    memset(oversized, 'A', sizeof(oversized));
    CHECK(factory_request_parse(oversized, sizeof(oversized), &request)
          == FACTORY_ERR_FRAME_TOO_LONG);

    char invalid_utf8[] =
        "@S5OTBRFT {\"seq\":1,\"cmd\":\"identity\",\"x\":\"\xC0\x80\"}";
    CHECK(factory_request_parse(invalid_utf8, sizeof(invalid_utf8) - 1,
                                &request) == FACTORY_ERR_INVALID_REQUEST);

    char long_unit[FACTORY_MANIFEST_UNIT_MAX + 128];
    memset(long_unit, 'U', sizeof(long_unit));
    long_unit[sizeof(long_unit) - 1] = '\0';
    char frame[400];
    snprintf(frame, sizeof(frame),
             "@S5OTBRFT {\"seq\":1,\"cmd\":\"fixture.record\","
             "\"test_id\":\"rail_3v3\",\"pass\":true,\"unit\":\"%s\"}",
             long_unit);
    CHECK(parse(frame, &request) == FACTORY_ERR_INVALID_REQUEST);
    return true;
}

static bool test_manifest(void)
{
    factory_manifest_reset();
    factory_manifest_summary_t summary;
    factory_manifest_summary(&summary);
    CHECK(summary.pending == factory_manifest_count());
    CHECK(summary.passed == 0 && summary.failed == 0);

    CHECK(factory_manifest_record("rail_3v3", FACTORY_TEST_STATION, true,
                                  true, 3.3, "V", "DMM")
          == FACTORY_OK);
    CHECK(factory_manifest_record("rail_3v3", FACTORY_TEST_STATION, false,
                                  false, 0, NULL, NULL)
          == FACTORY_ERR_IMMUTABLE);
    CHECK(factory_manifest_record("device_identity", FACTORY_TEST_STATION,
                                  true, false, 0, NULL, NULL)
          == FACTORY_ERR_UNAUTHORIZED);
    CHECK(factory_manifest_record("missing", FACTORY_TEST_STATION, true,
                                  false, 0, NULL, NULL)
          == FACTORY_ERR_NOT_FOUND);

    factory_manifest_summary(&summary);
    CHECK(summary.passed == 1);
    return true;
}

static bool mark_all_manifest_pass(void)
{
    for (size_t i = 0; i < factory_manifest_count(); ++i) {
        factory_manifest_entry_t entry;
        CHECK(factory_manifest_get(i, &entry) == FACTORY_OK);
        CHECK(factory_manifest_record(entry.id, entry.owner, true,
                                      false, 0, NULL, "host test")
              == FACTORY_OK);
    }
    return true;
}

static bool test_session(void)
{
    s_cleanup_calls = 0;
    s_cleanup_fails = false;
    CHECK(factory_session_init(cleanup_mock) == FACTORY_OK);
    CHECK(factory_session_start("PCB-001", 100) == FACTORY_OK);
    CHECK(s_cleanup_calls == 1);
    CHECK(factory_session_start("PCB-002", 101)
          == FACTORY_ERR_INVALID_STATE);
    CHECK(factory_session_start("bad id", 101)
          == FACTORY_ERR_INVALID_ARGUMENT);

    factory_session_snapshot_t snapshot;
    CHECK(factory_session_finish(&snapshot) == FACTORY_ERR_INCOMPLETE);
    CHECK(snapshot.manifest.pending == factory_manifest_count());
    CHECK(factory_session_is_active());

    CHECK(mark_all_manifest_pass());
    CHECK(factory_session_finish(&snapshot) == FACTORY_OK);
    CHECK(snapshot.state == FACTORY_SESSION_COMPLETED);
    CHECK(!factory_session_is_active());

    CHECK(factory_session_abort() == FACTORY_OK);
    CHECK(factory_session_abort() == FACTORY_OK);

    CHECK(factory_session_start("PCB-003", 1000) == FACTORY_OK);
    bool expired = false;
    CHECK(factory_session_expire(1000 + FACTORY_SESSION_TIMEOUT_US - 1,
                                 &expired) == FACTORY_OK);
    CHECK(!expired);
    CHECK(factory_session_expire(1000 + FACTORY_SESSION_TIMEOUT_US,
                                 &expired) == FACTORY_ERR_TIMEOUT);
    CHECK(expired);
    factory_session_snapshot(&snapshot);
    CHECK(snapshot.state == FACTORY_SESSION_EXPIRED);

    s_cleanup_fails = true;
    CHECK(factory_session_start("PCB-004", 2000) == FACTORY_ERR_CLEANUP);
    s_cleanup_fails = false;
    return true;
}

static int s_output_count;
static int s_output_gpio[8];
static int s_output_level[8];
static int s_released_gpio;
static int s_backend_fail_index;

static factory_result_t output_mock(int gpio, int level)
{
    const int index = s_output_count++;
    s_output_gpio[index] = gpio;
    s_output_level[index] = level;
    return index == s_backend_fail_index ? FACTORY_ERR_HARDWARE : FACTORY_OK;
}

static factory_result_t release_mock(int gpio)
{
    s_released_gpio = gpio;
    return FACTORY_OK;
}

static bool test_safety(void)
{
    const factory_safety_backend_t backend = {
        .configure_output = output_mock,
        .release_high_impedance = release_mock,
    };
    s_output_count = 0;
    s_released_gpio = -1;
    s_backend_fail_index = -1;
    CHECK(factory_safety_init(&backend) == FACTORY_OK);
    CHECK(factory_safety_is_verified());
    CHECK(s_output_count == 6);
    CHECK(s_output_gpio[0] == FACTORY_GPIO_LAMP_CONTROL);
    CHECK(s_output_level[0] == FACTORY_SAFE_LAMP_CONTROL_LEVEL);
    CHECK(s_output_gpio[1] == FACTORY_GPIO_MODEM_POWER_KEY);
    CHECK(s_output_level[1] == FACTORY_SAFE_MODEM_POWER_KEY_LEVEL);
    CHECK(s_output_gpio[5] == FACTORY_GPIO_MODEM_RESET);
    CHECK(s_output_level[5] == FACTORY_SAFE_MODEM_RESET_LEVEL);
    CHECK(s_released_gpio == FACTORY_GPIO_PSW_EN);

    s_output_count = 0;
    CHECK(factory_safety_apply() == FACTORY_OK);
    CHECK(s_output_count == 6);

    s_output_count = 0;
    s_backend_fail_index = 2;
    CHECK(factory_safety_apply() == FACTORY_ERR_HARDWARE);
    CHECK(!factory_safety_is_verified());
    CHECK(s_output_count == 6);
    CHECK(s_released_gpio == FACTORY_GPIO_PSW_EN);
    s_backend_fail_index = -1;
    return true;
}

static bool test_error_cleanup_guard(void)
{
    s_cleanup_calls = 0;
    s_cleanup_fails = false;
    CHECK(factory_apply_error_cleanup(FACTORY_OK, cleanup_mock) == FACTORY_OK);
    CHECK(s_cleanup_calls == 0);

    for (factory_result_t result = FACTORY_ERR_INVALID_ARGUMENT;
         result <= FACTORY_ERR_NO_MEMORY;
         result = (factory_result_t)(result + 1)) {
        CHECK(factory_apply_error_cleanup(result, cleanup_mock) == result);
    }
    CHECK(s_cleanup_calls == FACTORY_ERR_NO_MEMORY);

    s_cleanup_fails = true;
    CHECK(factory_apply_error_cleanup(FACTORY_ERR_INVALID_JSON, cleanup_mock)
          == FACTORY_ERR_CLEANUP);
    s_cleanup_fails = false;
    return true;
}

int main(void)
{
    const struct {
        const char *name;
        bool (*function)(void);
    } tests[] = {
        { "protocol validation", test_protocol_validation },
        { "manifest", test_manifest },
        { "session", test_session },
        { "safety", test_safety },
        { "error cleanup guard", test_error_cleanup_guard },
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (!tests[i].function()) {
            fprintf(stderr, "Test failed: %s\n", tests[i].name);
            return EXIT_FAILURE;
        }
        printf("PASS: %s\n", tests[i].name);
    }
    return EXIT_SUCCESS;
}
