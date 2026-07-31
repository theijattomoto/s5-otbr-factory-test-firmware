#ifndef FACTORY_SAFETY_H
#define FACTORY_SAFETY_H

#include "factory_types.h"

typedef struct {
    factory_result_t (*configure_output)(int gpio, int level);
    factory_result_t (*release_high_impedance)(int gpio);
} factory_safety_backend_t;

factory_result_t factory_safety_init(const factory_safety_backend_t *backend);
factory_result_t factory_safety_apply(void);
bool factory_safety_is_verified(void);

const factory_safety_backend_t *factory_safety_esp_backend(void);

#endif
