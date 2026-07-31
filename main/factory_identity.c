#include "factory_identity.h"

#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_system.h"

factory_result_t factory_identity_read(factory_identity_t *identity)
{
    if (identity == NULL) {
        return FACTORY_ERR_INVALID_ARGUMENT;
    }

    memset(identity, 0, sizeof(*identity));

    esp_chip_info_t chip = {0};
    uint8_t base_mac[6] = {0};
    uint8_t eui64[8] = {0};
    uint32_t flash_size = 0;

    esp_chip_info(&chip);
    if (esp_efuse_mac_get_default(base_mac) != ESP_OK ||
        esp_read_mac(eui64, ESP_MAC_IEEE802154) != ESP_OK ||
        esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        return FACTORY_ERR_HARDWARE;
    }

    snprintf(identity->target, sizeof(identity->target), "%s", "esp32c6");
    identity->silicon_revision = (uint32_t)chip.revision;
    identity->core_count = (uint32_t)chip.cores;
    identity->flash_size_bytes = flash_size;
    identity->reset_reason = (uint32_t)esp_reset_reason();

    snprintf(identity->base_mac, sizeof(identity->base_mac),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             base_mac[0], base_mac[1], base_mac[2],
             base_mac[3], base_mac[4], base_mac[5]);
    snprintf(identity->thread_eui64, sizeof(identity->thread_eui64),
             "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             eui64[0], eui64[1], eui64[2], eui64[3],
             eui64[4], eui64[5], eui64[6], eui64[7]);

    return FACTORY_OK;
}
