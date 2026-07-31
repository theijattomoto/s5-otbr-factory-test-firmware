#ifndef FACTORY_REQUEST_H
#define FACTORY_REQUEST_H

#include <limits.h>

#include "factory_manifest.h"
#include "factory_session.h"
#include "factory_types.h"

#define FACTORY_PROTOCOL_PREFIX          "@S5OTBRFT "
#define FACTORY_PROTOCOL_MAX_FRAME       767
#define FACTORY_COMMAND_MAX              32
#define FACTORY_TEST_ID_MAX              40
#define FACTORY_NAME_MAX                 24

typedef enum {
    FACTORY_COMMAND_UNKNOWN = 0,
    FACTORY_COMMAND_IDENTITY,
    FACTORY_COMMAND_SESSION_START,
    FACTORY_COMMAND_SESSION_LIST,
    FACTORY_COMMAND_SESSION_FINISH,
    FACTORY_COMMAND_SESSION_ABORT,
    FACTORY_COMMAND_FIXTURE_RECORD,
    FACTORY_COMMAND_SAFE,
    FACTORY_COMMAND_GPIO_WRITE,
    FACTORY_COMMAND_ADC_SAMPLE,
    FACTORY_COMMAND_ADC_WAVEFORM,
    FACTORY_COMMAND_PWM_SET,
    FACTORY_COMMAND_ZCD_CAPTURE,
    FACTORY_COMMAND_SPI_SENSOR,
    FACTORY_COMMAND_GPS_CHECK,
    FACTORY_COMMAND_MODEM_CHECK
} factory_command_t;

typedef struct {
    int32_t seq;
    factory_command_t command;
    char command_text[FACTORY_COMMAND_MAX];
    char unit_id[FACTORY_SESSION_UNIT_ID_MAX];
    char test_id[FACTORY_TEST_ID_MAX];
    bool passed;
    bool has_value;
    double value;
    char unit[FACTORY_MANIFEST_UNIT_MAX];
    char detail[FACTORY_MANIFEST_DETAIL_MAX];
    char name[FACTORY_NAME_MAX];
    char channel[FACTORY_NAME_MAX];
    int level;
    uint32_t samples;
    uint32_t sample_interval_us;
    uint32_t duration_ms;
    int duty_percent;
    int expected_hz;
    char mode[FACTORY_NAME_MAX];
    uint32_t timeout_ms;
} factory_request_t;

factory_result_t factory_request_parse(const char *line, size_t length,
                                       factory_request_t *request);

#endif
