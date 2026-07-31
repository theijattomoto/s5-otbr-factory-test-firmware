#include "factory_safety.h"

#include "driver/gpio.h"

static factory_result_t configure_output(int gpio_number, int level)
{
    const gpio_num_t gpio = (gpio_num_t)gpio_number;

    /*
     * Preload the output latch before enabling the output driver to minimize
     * transitions during initialization.
     */
    if (gpio_set_level(gpio, (uint32_t)level) != ESP_OK ||
        gpio_set_pull_mode(gpio, GPIO_FLOATING) != ESP_OK ||
        gpio_set_intr_type(gpio, GPIO_INTR_DISABLE) != ESP_OK ||
        gpio_set_direction(gpio, GPIO_MODE_OUTPUT) != ESP_OK ||
        gpio_get_level(gpio) != level) {
        return FACTORY_ERR_HARDWARE;
    }

    return FACTORY_OK;
}

static factory_result_t release_high_impedance(int gpio_number)
{
    const gpio_num_t gpio = (gpio_num_t)gpio_number;
    if (gpio_reset_pin(gpio) != ESP_OK ||
        gpio_set_direction(gpio, GPIO_MODE_INPUT) != ESP_OK ||
        gpio_set_pull_mode(gpio, GPIO_FLOATING) != ESP_OK ||
        gpio_set_intr_type(gpio, GPIO_INTR_DISABLE) != ESP_OK) {
        return FACTORY_ERR_HARDWARE;
    }

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
