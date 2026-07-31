#include "factory_safety.h"

#include "factory_board.h"

typedef struct {
    int gpio;
    int level;
} safe_output_t;

static const safe_output_t s_safe_outputs[] = {
    { FACTORY_GPIO_LAMP_CONTROL, FACTORY_SAFE_LAMP_CONTROL_LEVEL },
    { FACTORY_GPIO_MODEM_POWER_KEY, FACTORY_SAFE_MODEM_POWER_KEY_LEVEL },
    { FACTORY_GPIO_STATUS_LED, FACTORY_SAFE_STATUS_LED_LEVEL },
    { FACTORY_GPIO_LAMP_PWM, FACTORY_SAFE_LAMP_PWM_LEVEL },
    { FACTORY_GPIO_CONTROL_LED, FACTORY_SAFE_CONTROL_LED_LEVEL },
    { FACTORY_GPIO_MODEM_RESET, FACTORY_SAFE_MODEM_RESET_LEVEL },
};

static const factory_safety_backend_t *s_backend;
static bool s_verified;

factory_result_t factory_safety_init(const factory_safety_backend_t *backend)
{
    if (backend == NULL || backend->configure_output == NULL ||
        backend->release_high_impedance == NULL) {
        return FACTORY_ERR_INVALID_ARGUMENT;
    }

    s_backend = backend;
    s_verified = false;
    return factory_safety_apply();
}

factory_result_t factory_safety_apply(void)
{
    if (s_backend == NULL) {
        return FACTORY_ERR_INVALID_STATE;
    }

    s_verified = false;
    factory_result_t first_error = FACTORY_OK;

    for (size_t i = 0; i < sizeof(s_safe_outputs) / sizeof(s_safe_outputs[0]);
         ++i) {
        const factory_result_t result =
            s_backend->configure_output(s_safe_outputs[i].gpio,
                                        s_safe_outputs[i].level);
        if (result != FACTORY_OK && first_error == FACTORY_OK) {
            first_error = result;
        }
    }

    const factory_result_t psw_result =
        s_backend->release_high_impedance(FACTORY_GPIO_PSW_EN);
    if (psw_result != FACTORY_OK && first_error == FACTORY_OK) {
        first_error = psw_result;
    }

    s_verified = first_error == FACTORY_OK;
    return first_error;
}

bool factory_safety_is_verified(void)
{
    return s_verified;
}
