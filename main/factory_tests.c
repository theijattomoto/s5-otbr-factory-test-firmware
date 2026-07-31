#include "factory_tests.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "factory_board.h"

#define FACTORY_ADC_SAMPLES_MIN       1U
#define FACTORY_ADC_SAMPLES_MAX       1024U
#define FACTORY_GPS_TIMEOUT_MIN_MS    100U
#define FACTORY_GPS_TIMEOUT_MAX_MS    30000U
#define FACTORY_MODEM_TIMEOUT_MIN_MS  100U
#define FACTORY_MODEM_TIMEOUT_MAX_MS  10000U

#define FACTORY_GPS_UART              UART_NUM_0
#define FACTORY_MODEM_UART            UART_NUM_1
#define FACTORY_GPS_BAUD              9600
#define FACTORY_MODEM_BAUD            115200
#define UART_RX_BUFFER_SIZE            512

static factory_result_t configure_uart(uart_port_t port, int baud,
                                       int tx_gpio, int rx_gpio)
{
    const uart_config_t config = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_param_config(port, &config) != ESP_OK ||
        uart_set_pin(port, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE) != ESP_OK ||
        uart_driver_install(port, UART_RX_BUFFER_SIZE, 0, 0, NULL, 0)
            != ESP_OK) {
        (void)uart_driver_delete(port);
        return FACTORY_ERR_HARDWARE;
    }
    (void)uart_flush_input(port);
    return FACTORY_OK;
}

factory_result_t factory_test_gpio_write(const char *name, int level,
                                         int *gpio_out)
{
    if (name == NULL || gpio_out == NULL || (level != 0 && level != 1)) {
        return FACTORY_ERR_INVALID_ARGUMENT;
    }

    int gpio = -1;
    if (strcmp(name, "status_led") == 0) {
        gpio = FACTORY_GPIO_STATUS_LED;
    } else if (strcmp(name, "ctrl_led") == 0) {
        gpio = FACTORY_GPIO_CONTROL_LED;
    } else {
        return FACTORY_ERR_UNAUTHORIZED;
    }

    if (gpio_set_level((gpio_num_t)gpio, (uint32_t)level) != ESP_OK) {
        return FACTORY_ERR_HARDWARE;
    }
    *gpio_out = gpio;
    return FACTORY_OK;
}

factory_result_t factory_test_adc_sample(const char *channel, uint32_t samples,
                                         factory_adc_result_t *result)
{
    if (channel == NULL || result == NULL ||
        samples < FACTORY_ADC_SAMPLES_MIN ||
        samples > FACTORY_ADC_SAMPLES_MAX) {
        return FACTORY_ERR_INVALID_ARGUMENT;
    }

    adc_channel_t adc_channel;
    if (strcmp(channel, "vrms") == 0) {
        adc_channel = ADC_CHANNEL_4;
    } else if (strcmp(channel, "irms") == 0) {
        adc_channel = ADC_CHANNEL_5;
    } else if (strcmp(channel, "dc5v") == 0) {
        adc_channel = ADC_CHANNEL_6;
    } else {
        return FACTORY_ERR_NOT_FOUND;
    }

    adc_oneshot_unit_handle_t handle = NULL;
    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };

    if (adc_oneshot_new_unit(&unit_config, &handle) != ESP_OK ||
        adc_oneshot_config_channel(handle, adc_channel, &channel_config)
            != ESP_OK) {
        if (handle != NULL) {
            (void)adc_oneshot_del_unit(handle);
        }
        return FACTORY_ERR_HARDWARE;
    }

    int minimum = INT32_MAX;
    int maximum = INT32_MIN;
    int64_t total = 0;
    factory_result_t status = FACTORY_OK;
    for (uint32_t i = 0; i < samples; ++i) {
        int raw = 0;
        if (adc_oneshot_read(handle, adc_channel, &raw) != ESP_OK) {
            status = FACTORY_ERR_HARDWARE;
            break;
        }
        if (raw < minimum) minimum = raw;
        if (raw > maximum) maximum = raw;
        total += raw;
    }
    if (adc_oneshot_del_unit(handle) != ESP_OK && status == FACTORY_OK) {
        status = FACTORY_ERR_CLEANUP;
    }
    if (status != FACTORY_OK) {
        return status;
    }

    memset(result, 0, sizeof(*result));
    result->channel = channel;
    result->samples = samples;
    result->raw_average = (int)(total / (int64_t)samples);
    result->raw_min = minimum;
    result->raw_max = maximum;
    result->raw_noise = maximum - minimum;
    return FACTORY_OK;
}

static bool nmea_checksum_valid(const char *line)
{
    if (line == NULL || line[0] != '$') return false;
    const char *asterisk = strchr(line, '*');
    if (asterisk == NULL || !isxdigit((unsigned char)asterisk[1]) ||
        !isxdigit((unsigned char)asterisk[2])) {
        return false;
    }
    unsigned int expected = 0;
    if (sscanf(asterisk + 1, "%2x", &expected) != 1) return false;
    uint8_t actual = 0;
    for (const char *cursor = line + 1; cursor < asterisk; ++cursor) {
        actual ^= (uint8_t)*cursor;
    }
    return actual == (uint8_t)expected;
}

static bool nmea_is_rmc(const char *line)
{
    return line != NULL &&
           (strncmp(line, "$GPRMC,", 7) == 0 ||
            strncmp(line, "$GNRMC,", 7) == 0);
}

factory_result_t factory_test_gps(uint32_t timeout_ms,
                                  factory_gps_result_t *result)
{
    if (result == NULL || timeout_ms < FACTORY_GPS_TIMEOUT_MIN_MS ||
        timeout_ms > FACTORY_GPS_TIMEOUT_MAX_MS) {
        return FACTORY_ERR_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));

    factory_result_t status = configure_uart(
        FACTORY_GPS_UART, FACTORY_GPS_BAUD,
        FACTORY_GPIO_GPS_TX, FACTORY_GPIO_GPS_RX);
    if (status != FACTORY_OK) return status;

    char line[FACTORY_SAMPLE_TEXT_MAX] = {0};
    size_t length = 0;
    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() - started < timeout_ticks) {
        uint8_t byte = 0;
        if (uart_read_bytes(FACTORY_GPS_UART, &byte, 1,
                            pdMS_TO_TICKS(20)) != 1) {
            continue;
        }
        if (byte == '\r' || byte == '\n') {
            if (length == 0) continue;
            line[length] = '\0';
            if (nmea_checksum_valid(line)) {
                ++result->valid_nmea_sentences;
                if (nmea_is_rmc(line)) {
                    ++result->valid_rmc_sentences;
                    snprintf(result->sample, sizeof(result->sample), "%s",
                             line);
                    result->nmea_received = true;
                    break;
                }
            }
            length = 0;
            continue;
        }
        if (length + 1 < sizeof(line)) {
            line[length++] = (char)byte;
        } else {
            length = 0;
        }
    }

    if (uart_driver_delete(FACTORY_GPS_UART) != ESP_OK) {
        return FACTORY_ERR_CLEANUP;
    }
    return result->nmea_received ? FACTORY_OK : FACTORY_ERR_TIMEOUT;
}

static bool modem_command(const char *command, uint32_t timeout_ms,
                          char *response, size_t capacity)
{
    (void)uart_flush_input(FACTORY_MODEM_UART);
    const int command_length = (int)strlen(command);
    if (uart_write_bytes(FACTORY_MODEM_UART, command, command_length)
            != command_length ||
        uart_wait_tx_done(FACTORY_MODEM_UART, pdMS_TO_TICKS(1000))
            != ESP_OK) {
        return false;
    }

    size_t length = 0;
    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() - started < timeout_ticks &&
           length + 1 < capacity) {
        int count = uart_read_bytes(
            FACTORY_MODEM_UART, (uint8_t *)&response[length],
            capacity - length - 1, pdMS_TO_TICKS(20));
        if (count > 0) {
            length += (size_t)count;
            response[length] = '\0';
            if (strstr(response, "\r\nOK\r\n") != NULL ||
                strstr(response, "\r\nERROR\r\n") != NULL) {
                break;
            }
        }
    }
    return strstr(response, "OK") != NULL;
}

static void compact_modem_text(char *text)
{
    if (text == NULL) return;
    size_t write = 0;
    bool previous_space = false;
    for (size_t read = 0; text[read] != '\0'; ++read) {
        unsigned char c = (unsigned char)text[read];
        bool space = isspace(c) != 0;
        if (space && previous_space) continue;
        text[write++] = space ? ' ' : (char)c;
        previous_space = space;
    }
    while (write > 0 && text[write - 1] == ' ') --write;
    text[write] = '\0';
}

factory_result_t factory_test_modem(uint32_t timeout_ms,
                                    factory_modem_result_t *result)
{
    if (result == NULL || timeout_ms < FACTORY_MODEM_TIMEOUT_MIN_MS ||
        timeout_ms > FACTORY_MODEM_TIMEOUT_MAX_MS) {
        return FACTORY_ERR_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));

    factory_result_t status = configure_uart(
        FACTORY_MODEM_UART, FACTORY_MODEM_BAUD,
        FACTORY_GPIO_MODEM_TX, FACTORY_GPIO_MODEM_RX);
    if (status != FACTORY_OK) return status;

    char at_response[64] = {0};
    result->at_ok = modem_command("AT\r", timeout_ms, at_response,
                                  sizeof(at_response));
    if (result->at_ok) {
        result->identity_ok = modem_command(
            "ATI\r", timeout_ms, result->identity,
            sizeof(result->identity));
        bool sim_query_ok = modem_command(
            "AT+CPIN?\r", timeout_ms, result->sim_status,
            sizeof(result->sim_status));
        result->sim_ready = sim_query_ok &&
                            strstr(result->sim_status, "READY") != NULL;
        compact_modem_text(result->identity);
        compact_modem_text(result->sim_status);
    }

    if (uart_driver_delete(FACTORY_MODEM_UART) != ESP_OK) {
        return FACTORY_ERR_CLEANUP;
    }
    return result->at_ok && result->identity_ok && result->sim_ready
               ? FACTORY_OK : FACTORY_ERR_HARDWARE;
}
