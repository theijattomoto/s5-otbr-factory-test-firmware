#ifndef FACTORY_TESTS_H
#define FACTORY_TESTS_H

#include "factory_types.h"

#define FACTORY_SAMPLE_TEXT_MAX 96
#define FACTORY_MODEM_TEXT_MAX  96

typedef struct {
    const char *channel;
    uint32_t samples;
    int raw_average;
    int raw_min;
    int raw_max;
    int raw_noise;
    int adc_mv;
    uint32_t estimated_input_mv;
} factory_adc_result_t;

typedef struct {
    uint32_t samples;
    double raw_mean;
    double raw_rms;
    int raw_min;
    int raw_max;
    int peak_to_peak;
    uint32_t clipped_samples;
    double sample_rate_hz;
    double engineering_value;
    bool within_range;
} factory_waveform_result_t;

typedef struct {
    uint32_t edges;
    double frequency_hz;
    double duration_ms;
    bool within_tolerance;
} factory_zcd_result_t;

typedef struct {
    uint8_t who_am_i;
    int16_t x;
    int16_t y;
    int16_t z;
} factory_wsen_result_t;

typedef struct {
    bool nmea_received;
    uint32_t valid_nmea_sentences;
    uint32_t valid_rmc_sentences;
    char sample[FACTORY_SAMPLE_TEXT_MAX];
} factory_gps_result_t;

typedef struct {
    bool at_ok;
    bool identity_ok;
    bool sim_ready;
    char identity[FACTORY_MODEM_TEXT_MAX];
    char sim_status[FACTORY_MODEM_TEXT_MAX];
} factory_modem_result_t;

factory_result_t factory_test_gpio_write(const char *name, int level,
                                         int *gpio_out);
factory_result_t factory_test_adc_sample(const char *channel, uint32_t samples,
                                         factory_adc_result_t *result);
factory_result_t factory_test_adc_waveform(
    const char *channel, const char *mode, uint32_t samples,
    uint32_t sample_interval_us, factory_waveform_result_t *result);
factory_result_t factory_test_pwm_set(int duty_percent);
factory_result_t factory_test_zcd_capture(uint32_t duration_ms,
                                          int expected_hz,
                                          factory_zcd_result_t *result);
factory_result_t factory_test_wsen(factory_wsen_result_t *result);
factory_result_t factory_test_gps(uint32_t timeout_ms,
                                  factory_gps_result_t *result);
factory_result_t factory_test_modem(uint32_t timeout_ms,
                                    factory_modem_result_t *result);

#endif
