#ifndef FACTORY_BOARD_H
#define FACTORY_BOARD_H

/*
 * Software-visible Node-OTBR mapping taken from the production reference.
 * Electrical behavior still requires verification against the controlled
 * schematic and the exact assembled PCB revision.
 */

#define FACTORY_PRODUCT_NAME              "S5-NODE-OTBR"
#define FACTORY_BOARD_REVISION            "TBD"
#define FACTORY_FIRMWARE_VERSION           "0.3.0"
#define FACTORY_PROTOCOL_VERSION           "1.2"

#define FACTORY_GPIO_LAMP_CONTROL          0
#define FACTORY_GPIO_MODEM_POWER_KEY       1
#define FACTORY_GPIO_STATUS_LED            2
#define FACTORY_GPIO_PSW_EN                3
#define FACTORY_GPIO_VRMS                  4
#define FACTORY_GPIO_IRMS                  5
#define FACTORY_GPIO_5V_MONITOR            6
#define FACTORY_GPIO_LAMP_PWM              7
#define FACTORY_GPIO_CONTROL_LED           8
#define FACTORY_GPIO_MODEM_RESET           9
#define FACTORY_GPIO_USB_D_MINUS           12
#define FACTORY_GPIO_USB_D_PLUS            13
#define FACTORY_GPIO_AC_ZCD                14
#define FACTORY_GPIO_MODEM_RX              16
#define FACTORY_GPIO_MODEM_TX              17
#define FACTORY_GPIO_GPS_RX                18
#define FACTORY_GPIO_GPS_TX                19
#define FACTORY_GPIO_WSEN_CS               20
#define FACTORY_GPIO_WSEN_SCK              21
#define FACTORY_GPIO_WSEN_MOSI             22
#define FACTORY_GPIO_WSEN_MISO             23

#define FACTORY_ADC_CHANNEL_VRMS           4
#define FACTORY_ADC_CHANNEL_IRMS           5
#define FACTORY_ADC_CHANNEL_5V             6

/* Values intentionally match the proven S5-Node V5 measurement profile. */
#define FACTORY_ADC_VOLT_PER_STEP          1.049805
#define FACTORY_VRMS_SENSOR_GAIN           0.467289
#define FACTORY_IRMS_SENSOR_GAIN           0.0023364
#define FACTORY_VRMS_CAL_FACTOR            1.09649
#define FACTORY_IRMS_CAL_FACTOR            1.0
#define FACTORY_VRMS_MIN_V                 220.0
#define FACTORY_VRMS_MAX_V                 260.0
#define FACTORY_IRMS_NO_LOAD_MAX_A         0.020
#define FACTORY_IRMS_LOAD_MAX_A            1.000
#define FACTORY_5V_DIVIDER_TOTAL_OHMS      11100U
#define FACTORY_5V_DIVIDER_BOTTOM_OHMS     2000U

/* Known idle/safe software levels from the production implementation. */
#define FACTORY_SAFE_LAMP_CONTROL_LEVEL    1
#define FACTORY_SAFE_MODEM_POWER_KEY_LEVEL 1
#define FACTORY_SAFE_STATUS_LED_LEVEL      1
#define FACTORY_SAFE_LAMP_PWM_LEVEL        0
#define FACTORY_SAFE_CONTROL_LED_LEVEL     1
#define FACTORY_SAFE_MODEM_RESET_LEVEL     1

/*
 * Deliberately no FACTORY_SAFE_PSW_EN_LEVEL exists.
 *
 * The board revision and PSW_EN electrical safe behavior are unresolved.
 * Phase 1 releases GPIO3 to a high-impedance input with both internal pulls
 * disabled. This is a software non-drive policy, not an electrical safety
 * approval for production hardware.
 */
#define FACTORY_PSW_EN_POLICY_UNRESOLVED   1

#endif
