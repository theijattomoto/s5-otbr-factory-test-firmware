#include "factory_manifest.h"

#include <math.h>
#include <string.h>

#define ENTRY(test_id, text, test_owner) \
    { (test_id), (text), (test_owner), FACTORY_TEST_PENDING, false, 0.0, "", "" }

static factory_manifest_entry_t s_entries[] = {
    ENTRY("device_identity", "ESP32-C6 identity readable", FACTORY_TEST_AUTOMATIC),
    ENTRY("usb_protocol", "Native USB framed protocol", FACTORY_TEST_AUTOMATIC),
    ENTRY("base_mac", "Base MAC readable", FACTORY_TEST_AUTOMATIC),
    ENTRY("thread_eui64", "IEEE 802.15.4 EUI-64 readable", FACTORY_TEST_AUTOMATIC),
    ENTRY("firmware_identity", "Factory firmware identity", FACTORY_TEST_AUTOMATIC),
    ENTRY("gps_uart_rx", "GPS UART receive path", FACTORY_TEST_AUTOMATIC),
    ENTRY("modem_uart", "EG912 UART response", FACTORY_TEST_AUTOMATIC),
    ENTRY("modem_identity", "EG912 model identity", FACTORY_TEST_AUTOMATIC),
    ENTRY("sim_presence", "SIM presence/readiness", FACTORY_TEST_AUTOMATIC),
    ENTRY("rail_3v3", "Fixture-measured 3.3 V rail", FACTORY_TEST_STATION),
    ENTRY("rail_5v", "Fixture-measured 5 V rail", FACTORY_TEST_STATION),
    ENTRY("gpio_lamp_ctrl", "Lamp control observed low/high", FACTORY_TEST_STATION),
    ENTRY("gpio_psw_en", "PSW_EN observed under approved policy", FACTORY_TEST_STATION),
    ENTRY("gpio_modem_pwrkey", "Modem PWRKEY observed", FACTORY_TEST_STATION),
    ENTRY("gpio_modem_reset", "Modem reset observed", FACTORY_TEST_STATION),
    ENTRY("gpio_status_led", "Status LED electrical/optical response", FACTORY_TEST_STATION),
    ENTRY("gpio_ctrl_led", "Control LED electrical/optical response", FACTORY_TEST_STATION),
    ENTRY("adc_vrms", "VRMS ADC verified at approved points", FACTORY_TEST_STATION),
    ENTRY("adc_irms", "IRMS ADC verified at approved points", FACTORY_TEST_STATION),
    ENTRY("adc_5v", "5 V monitor ADC verified at approved points", FACTORY_TEST_STATION),
    ENTRY("pwm", "PWM frequency/duty fixture measurement", FACTORY_TEST_STATION),
    ENTRY("zcd_gpio", "ZCD static low/high input", FACTORY_TEST_STATION),
    ENTRY("zcd_50hz", "Simulated 50 Hz ZCD input", FACTORY_TEST_STATION),
    ENTRY("zcd_60hz", "Simulated 60 Hz ZCD input", FACTORY_TEST_STATION),
    ENTRY("gps_uart_tx", "GPS UART transmit path", FACTORY_TEST_STATION),
    ENTRY("modem_uart_tx_rx", "EG912 bidirectional UART path", FACTORY_TEST_STATION),
};

static factory_manifest_entry_t *find_entry(const char *test_id)
{
    if (test_id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < factory_manifest_count(); ++i) {
        if (strcmp(s_entries[i].id, test_id) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

static bool bounded_copy(char *destination, size_t capacity, const char *source)
{
    if (source == NULL) {
        destination[0] = '\0';
        return true;
    }
    const size_t length = strlen(source);
    if (length >= capacity) {
        return false;
    }
    memcpy(destination, source, length + 1);
    return true;
}

void factory_manifest_reset(void)
{
    for (size_t i = 0; i < factory_manifest_count(); ++i) {
        s_entries[i].status = FACTORY_TEST_PENDING;
        s_entries[i].has_value = false;
        s_entries[i].value = 0.0;
        s_entries[i].unit[0] = '\0';
        s_entries[i].detail[0] = '\0';
    }
}

size_t factory_manifest_count(void)
{
    return sizeof(s_entries) / sizeof(s_entries[0]);
}

factory_result_t factory_manifest_get(size_t index,
                                      factory_manifest_entry_t *entry)
{
    if (entry == NULL || index >= factory_manifest_count()) {
        return FACTORY_ERR_INVALID_ARGUMENT;
    }
    *entry = s_entries[index];
    return FACTORY_OK;
}

factory_result_t factory_manifest_record(const char *test_id,
                                         factory_test_owner_t actor,
                                         bool passed,
                                         bool has_value,
                                         double value,
                                         const char *unit,
                                         const char *detail)
{
    factory_manifest_entry_t *entry = find_entry(test_id);
    if (entry == NULL) {
        return FACTORY_ERR_NOT_FOUND;
    }
    if (entry->owner != actor) {
        return FACTORY_ERR_UNAUTHORIZED;
    }
    if (entry->status != FACTORY_TEST_PENDING) {
        return FACTORY_ERR_IMMUTABLE;
    }
    if (has_value && !isfinite(value)) {
        return FACTORY_ERR_INVALID_ARGUMENT;
    }

    char checked_unit[FACTORY_MANIFEST_UNIT_MAX];
    char checked_detail[FACTORY_MANIFEST_DETAIL_MAX];
    if (!bounded_copy(checked_unit, sizeof(checked_unit), unit) ||
        !bounded_copy(checked_detail, sizeof(checked_detail), detail)) {
        return FACTORY_ERR_INVALID_ARGUMENT;
    }

    entry->status = passed ? FACTORY_TEST_PASS : FACTORY_TEST_FAIL;
    entry->has_value = has_value;
    entry->value = has_value ? value : 0.0;
    memcpy(entry->unit, checked_unit, sizeof(entry->unit));
    memcpy(entry->detail, checked_detail, sizeof(entry->detail));
    return FACTORY_OK;
}

void factory_manifest_summary(factory_manifest_summary_t *summary)
{
    if (summary == NULL) {
        return;
    }
    memset(summary, 0, sizeof(*summary));
    for (size_t i = 0; i < factory_manifest_count(); ++i) {
        switch (s_entries[i].status) {
            case FACTORY_TEST_PASS:
                ++summary->passed;
                break;
            case FACTORY_TEST_FAIL:
                ++summary->failed;
                break;
            default:
                ++summary->pending;
                break;
        }
    }
}

const char *factory_test_status_name(factory_test_status_t status)
{
    return status == FACTORY_TEST_PASS ? "pass" :
           status == FACTORY_TEST_FAIL ? "fail" : "pending";
}

const char *factory_test_owner_name(factory_test_owner_t owner)
{
    return owner == FACTORY_TEST_STATION ? "station" : "automatic";
}
