#include "factory_protocol.h"
#include "factory_safety.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "s5otbrft";

void app_main(void)
{
    factory_result_t result =
        factory_safety_init(factory_safety_esp_backend());
    if (result != FACTORY_OK) {
        ESP_LOGE(TAG, "safe initialization failed: %s",
                 factory_result_code(result));
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }

    result = factory_protocol_init();
    if (result != FACTORY_OK) {
        (void)factory_safety_apply();
        ESP_LOGE(TAG, "protocol initialization failed: %s",
                 factory_result_code(result));
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }

    factory_protocol_run();
}
