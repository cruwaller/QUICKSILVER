#include "config.h"

// PORTS
#define SPI_PORTS   \
  SPI1_PA5PA6PA7    \
  SPI2_PB13PB14PB15 \
  SPI3_PB3PB4PB5

#define USART_PORTS \
  USART1_PB7PB6     \
  USART2_PA3PA2

// #define USB_DETECT_PIN LL_GPIO_PIN_5
// #define USB_DETECT_PORT GPIOC

// LEDS, Buzzer, FPV
#define LED_NUMBER 1
#define LED1PIN PIN_A4
#define LED1_INVERT
#define BUZZER_PIN PIN_C13
#define BUZZER_INVERT
#define FPV_PIN PIN_A13

// GYRO
#define GYRO_SPI_PORT SPI_PORT2
#define GYRO_NSS PIN_B12
#define GYRO_INT PIN_A8
#define GYRO_ORIENTATION GYRO_ROTATE_90_CW

// RADIO
#define USART2_INVERTER_PIN PIN_C14

// OSD
#define USE_MAX7456
#define MAX7456_SPI_PORT SPI_PORT2
#define MAX7456_NSS PIN_A10

// VOLTAGE DIVIDER
#define VBAT_PIN PIN_A5
#define VBAT_DIVIDER_R1 10000
#define VBAT_DIVIDER_R2 1000

// MOTOR PINS
// S3_OUT
#define MOTOR_PIN0 MOTOR_PIN_PB8
// S4_OUT
#define MOTOR_PIN1 MOTOR_PIN_PB1
// S1_OUT
#define MOTOR_PIN2 MOTOR_PIN_PA1
// S2_OUT
#define MOTOR_PIN3 MOTOR_PIN_PA7
