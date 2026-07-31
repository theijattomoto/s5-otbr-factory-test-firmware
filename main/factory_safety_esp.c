#include "factory_safety.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "factory_safety";

static factory_result_t log_gpio_failure(const char *operation,
                                         gpio_num_t gpio,
                                         esp_err_t error)
{
    ESP_LOGE(TAG, "safe GPIO failure: operation=%s gpio=%d error=%s",
             operation, (int)gpio, esp_err_to_name(error));
    return FACTORY_ERR_HARDWARE;
}

static factory_result_t configure_output(int gpio_number, int level)
{
    const gpio_num_t gpio = (gpio_num_t)gpio_number;
    esp_err_t error;

    /*
     * Preload the output latch before enabling the output driver to minimize
     * transitions during initialization.
     */
    error = gpio_set_level(gpio, (uint32_t)level);
    if (error != ESP_OK) {
        return log_gpio_failure("preload_level", gpio, error);
    }
    error = gpio_set_pull_mode(gpio, GPIO_FLOATING);
    if (error != ESP_OK) {
        return log_gpio_failure("disable_pulls", gpio, error);
    }
    error = gpio_set_intr_type(gpio, GPIO_INTR_DISABLE);
    if (error != ESP_OK) {
        return log_gpio_failure("disable_interrupt", gpio, error);
    }
    error = gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    if (error != ESP_OK) {
        return log_gpio_failure("enable_output", gpio, error);
    }

    /*
     * Do not compare gpio_get_level() with the requested level here.
     * gpio_get_level() samples the physical pad. External loading, an
     * assembled driver, or board circuitry can make it differ from the output
     * latch even when ESP-IDF configured the GPIO successfully. Fixture
     * observation, not pad readback, proves the assembled net.
     */
    ESP_LOGI(TAG, "safe GPIO configured: gpio=%d level=%d",
             (int)gpio, level);
    return FACTORY_OK;
}

static factory_result_t release_high_impedance(int gpio_number)
{
    const gpio_num_t gpio = (gpio_num_t)gpio_number;
    esp_err_t error = gpio_reset_pin(gpio);
    if (error != ESP_OK) {
        return log_gpio_failure("reset_pin", gpio, error);
    }
    error = gpio_set_direction(gpio, GPIO_MODE_INPUT);
    if (error != ESP_OK) {
        return log_gpio_failure("set_input", gpio, error);
    }
    error = gpio_set_pull_mode(gpio, GPIO_FLOATING);
    if (error != ESP_OK) {
        return log_gpio_failure("disable_pulls", gpio, error);
    }
    error = gpio_set_intr_type(gpio, GPIO_INTR_DISABLE);
    if (error != ESP_OK) {
        return log_gpio_failure("disable_interrupt", gpio, error);
    }

    ESP_LOGI(TAG, "GPIO released high impedance: gpio=%d", (int)gpio);
    return FACTORY_OK;
}

static const factory_safety_backend_t s_esp_backend = {
    .configure_output = configure_output,
    .release_high_impedance = release_high_impedance,
};

const factory_safety_backend_t *factory_safety_esp_backend(void)
{
    return &s_esp_backend;
}
