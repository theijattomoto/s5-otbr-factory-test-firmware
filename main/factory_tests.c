#include "factory_tests.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
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
#define FACTORY_WSEN_EXPECTED_ID       0x44U
#define FACTORY_WSEN_WHO_AM_I_REG      0x0FU
#define FACTORY_WSEN_OUT_X_L_REG       0x28U
#define FACTORY_PWM_MAX_DUTY           1023U

static adc_channel_t channel_from_name(const char *channel)
{
    if (channel != NULL && strcmp(channel, "vrms") == 0) {
        return ADC_CHANNEL_4;
    }
    if (channel != NULL && strcmp(channel, "irms") == 0) {
        return ADC_CHANNEL_5;
    }
    if (channel != NULL && strcmp(channel, "dc5v") == 0) {
        return ADC_CHANNEL_6;
    }
    return (adc_channel_t)-1;
}

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
    } else if (strcmp(name, "lamp_ctrl") == 0) {
        gpio = FACTORY_GPIO_LAMP_CONTROL;
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

    const adc_channel_t adc_channel = channel_from_name(channel);
    if ((int)adc_channel < 0) {
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
    adc_cali_handle_t calibration = NULL;
    const adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id = ADC_UNIT_1,
        .chan = adc_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&calibration_config,
                                              &calibration) == ESP_OK) {
        if (adc_cali_raw_to_voltage(calibration, result->raw_average,
                                    &result->adc_mv) == ESP_OK &&
            strcmp(channel, "dc5v") == 0) {
            result->estimated_input_mv =
                ((uint32_t)result->adc_mv *
                 FACTORY_5V_DIVIDER_TOTAL_OHMS) /
                FACTORY_5V_DIVIDER_BOTTOM_OHMS;
        }
        (void)adc_cali_delete_scheme_curve_fitting(calibration);
    }
    return FACTORY_OK;
}

factory_result_t factory_test_adc_waveform(
    const char *channel, const char *mode, uint32_t samples,
    uint32_t sample_interval_us, factory_waveform_result_t *result)
{
    if (channel == NULL || result == NULL || samples < 2U ||
        samples > FACTORY_ADC_SAMPLES_MAX || sample_interval_us < 10U ||
        sample_interval_us > 10000U) {
        return FACTORY_ERR_INVALID_ARGUMENT;
    }
    const adc_channel_t adc_channel = channel_from_name(channel);
    if (adc_channel != ADC_CHANNEL_4 && adc_channel != ADC_CHANNEL_5) {
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
        if (handle != NULL) (void)adc_oneshot_del_unit(handle);
        return FACTORY_ERR_HARDWARE;
    }

    memset(result, 0, sizeof(*result));
    result->raw_min = INT32_MAX;
    result->raw_max = INT32_MIN;
    double mean = 0.0;
    double sum_squared_delta = 0.0;
    const int64_t started_us = esp_timer_get_time();
    for (uint32_t i = 0; i < samples; ++i) {
        int raw = 0;
        if (adc_oneshot_read(handle, adc_channel, &raw) != ESP_OK) {
            (void)adc_oneshot_del_unit(handle);
            return FACTORY_ERR_HARDWARE;
        }
        const double delta = (double)raw - mean;
        mean += delta / (double)(i + 1U);
        sum_squared_delta += delta * ((double)raw - mean);
        if (raw < result->raw_min) result->raw_min = raw;
        if (raw > result->raw_max) result->raw_max = raw;
        if (raw <= 0 || raw >= 4095) ++result->clipped_samples;
        esp_rom_delay_us(sample_interval_us);
    }
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    if (adc_oneshot_del_unit(handle) != ESP_OK) {
        return FACTORY_ERR_CLEANUP;
    }

    result->samples = samples;
    result->raw_mean = mean;
    result->raw_rms = sqrt(sum_squared_delta / (double)samples);
    result->peak_to_peak = result->raw_max - result->raw_min;
    result->sample_rate_hz =
        elapsed_us > 0 ? ((double)samples * 1000000.0) /
                             (double)elapsed_us : 0.0;
    if (adc_channel == ADC_CHANNEL_4) {
        result->engineering_value =
            result->raw_rms * FACTORY_ADC_VOLT_PER_STEP *
            FACTORY_VRMS_SENSOR_GAIN * FACTORY_VRMS_CAL_FACTOR;
        result->within_range =
            result->engineering_value >= FACTORY_VRMS_MIN_V &&
            result->engineering_value <= FACTORY_VRMS_MAX_V;
    } else {
        result->engineering_value =
            result->raw_rms * FACTORY_ADC_VOLT_PER_STEP *
            FACTORY_IRMS_SENSOR_GAIN * FACTORY_IRMS_CAL_FACTOR;
        if (mode != NULL && strcmp(mode, "no_load") == 0) {
            result->within_range =
                result->engineering_value >= 0.0 &&
                result->engineering_value <
                    FACTORY_IRMS_NO_LOAD_MAX_A;
        } else if (mode != NULL && strcmp(mode, "load") == 0) {
            result->within_range =
                result->engineering_value >=
                    FACTORY_IRMS_NO_LOAD_MAX_A &&
                result->engineering_value <= FACTORY_IRMS_LOAD_MAX_A;
        } else {
            result->within_range = true;
        }
    }
    return result->within_range || (mode != NULL &&
           strcmp(mode, "observe") == 0)
               ? FACTORY_OK : FACTORY_ERR_MEASUREMENT_RANGE;
}

factory_result_t factory_test_pwm_set(int duty_percent)
{
    if (duty_percent < 0 || duty_percent > 100 ||
        duty_percent % 10 != 0) {
        return FACTORY_ERR_INVALID_ARGUMENT;
    }
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = FACTORY_GPIO_LAMP_PWM,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    if (ledc_timer_config(&timer) != ESP_OK ||
        ledc_channel_config(&channel) != ESP_OK) {
        return FACTORY_ERR_HARDWARE;
    }
    const uint32_t duty =
        ((uint32_t)duty_percent * FACTORY_PWM_MAX_DUTY) / 100U;
    return ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty)
                   == ESP_OK &&
           ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0) == ESP_OK
               ? FACTORY_OK : FACTORY_ERR_HARDWARE;
}

factory_result_t factory_test_zcd_capture(uint32_t duration_ms,
                                          int expected_hz,
                                          factory_zcd_result_t *result)
{
    if (result == NULL || duration_ms < 100U || duration_ms > 5000U ||
        (expected_hz != 50 && expected_hz != 60)) {
        return FACTORY_ERR_INVALID_ARGUMENT;
    }
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << FACTORY_GPIO_AC_ZCD,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&config) != ESP_OK) return FACTORY_ERR_HARDWARE;
    memset(result, 0, sizeof(*result));
    int previous = gpio_get_level(FACTORY_GPIO_AC_ZCD);
    const int64_t started = esp_timer_get_time();
    const int64_t duration_us = (int64_t)duration_ms * 1000LL;
    while (esp_timer_get_time() - started < duration_us) {
        const int level = gpio_get_level(FACTORY_GPIO_AC_ZCD);
        if (level != previous) {
            ++result->edges;
            previous = level;
        }
        esp_rom_delay_us(100);
    }
    result->duration_ms =
        (double)(esp_timer_get_time() - started) / 1000.0;
    result->frequency_hz =
        result->duration_ms > 0.0
            ? ((double)result->edges * 500.0) / result->duration_ms
            : 0.0;
    result->within_tolerance =
        result->frequency_hz >= (double)expected_hz - 5.0 &&
        result->frequency_hz <= (double)expected_hz + 5.0;
    return result->within_tolerance ? FACTORY_OK :
                                      FACTORY_ERR_FREQUENCY_RANGE;
}

factory_result_t factory_test_wsen(factory_wsen_result_t *result)
{
    if (result == NULL) return FACTORY_ERR_INVALID_ARGUMENT;
    const spi_bus_config_t bus = {
        .mosi_io_num = FACTORY_GPIO_WSEN_MOSI,
        .miso_io_num = FACTORY_GPIO_WSEN_MISO,
        .sclk_io_num = FACTORY_GPIO_WSEN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return FACTORY_ERR_HARDWARE;
    }
    spi_device_handle_t device = NULL;
    const spi_device_interface_config_t config = {
        .clock_speed_hz = 1000000,
        .mode = 3,
        .spics_io_num = FACTORY_GPIO_WSEN_CS,
        .queue_size = 1,
    };
    if (spi_bus_add_device(SPI2_HOST, &config, &device) != ESP_OK) {
        (void)spi_bus_free(SPI2_HOST);
        return FACTORY_ERR_HARDWARE;
    }
    uint8_t values[7] = {0};
    const uint8_t registers[] = {
        FACTORY_WSEN_WHO_AM_I_REG,
        FACTORY_WSEN_OUT_X_L_REG, FACTORY_WSEN_OUT_X_L_REG + 1U,
        FACTORY_WSEN_OUT_X_L_REG + 2U, FACTORY_WSEN_OUT_X_L_REG + 3U,
        FACTORY_WSEN_OUT_X_L_REG + 4U, FACTORY_WSEN_OUT_X_L_REG + 5U,
    };
    factory_result_t status = FACTORY_OK;
    for (size_t i = 0; i < sizeof(registers); ++i) {
        uint8_t tx[2] = {(uint8_t)(registers[i] | 0x80U), 0};
        uint8_t rx[2] = {0};
        spi_transaction_t transaction = {
            .length = 16,
            .tx_buffer = tx,
            .rx_buffer = rx,
        };
        if (spi_device_transmit(device, &transaction) != ESP_OK) {
            status = FACTORY_ERR_HARDWARE;
            break;
        }
        values[i] = rx[1];
    }
    (void)spi_bus_remove_device(device);
    (void)spi_bus_free(SPI2_HOST);
    if (status != FACTORY_OK) return status;
    memset(result, 0, sizeof(*result));
    result->who_am_i = values[0];
    result->x = (int16_t)((uint16_t)values[2] << 8U | values[1]);
    result->y = (int16_t)((uint16_t)values[4] << 8U | values[3]);
    result->z = (int16_t)((uint16_t)values[6] << 8U | values[5]);
    return result->who_am_i == FACTORY_WSEN_EXPECTED_ID
               ? FACTORY_OK : FACTORY_ERR_HARDWARE;
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
                snprintf(result->sample, sizeof(result->sample), "%s",
                         line);
                result->nmea_received = true;
                if (nmea_is_rmc(line)) {
                    ++result->valid_rmc_sentences;
                }
                break;
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
