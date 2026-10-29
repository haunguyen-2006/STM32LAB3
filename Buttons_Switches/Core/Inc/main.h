#ifndef __MAIN_H
#define __MAIN_H

#include "stm32f1xx_hal.h"

/* Traffic LEDs: active LOW (GPIO RESET = LED ON). */
#define R_1_Pin         GPIO_PIN_1
#define G_1_Pin         GPIO_PIN_2
#define Y_1_Pin         GPIO_PIN_3
#define R_2_Pin         GPIO_PIN_4
#define G_2_Pin         GPIO_PIN_5
#define Y_2_Pin         GPIO_PIN_6

#define TRAFFIC_LED_GPIO_Port GPIOA

/* Seven-segment digit enables: assumed active LOW. */
#define EN0_Pin         GPIO_PIN_8
#define EN1_Pin         GPIO_PIN_9
#define EN2_Pin         GPIO_PIN_10
#define EN3_Pin         GPIO_PIN_11

#define SEG_ENABLE_GPIO_Port GPIOA

/* Vertical seven-segment segment bus: PB0-PB6. */
#define SEG1_0_Pin      GPIO_PIN_0
#define SEG1_1_Pin      GPIO_PIN_1
#define SEG1_2_Pin      GPIO_PIN_2
#define SEG1_3_Pin      GPIO_PIN_3
#define SEG1_4_Pin      GPIO_PIN_4
#define SEG1_5_Pin      GPIO_PIN_5
#define SEG1_6_Pin      GPIO_PIN_6

#define SEG1_GPIO_Port  GPIOB

/* Horizontal seven-segment segment bus: PB9-PB15. */
#define SEG2_0_Pin      GPIO_PIN_9
#define SEG2_1_Pin      GPIO_PIN_10
#define SEG2_2_Pin      GPIO_PIN_11
#define SEG2_3_Pin      GPIO_PIN_12
#define SEG2_4_Pin      GPIO_PIN_13
#define SEG2_5_Pin      GPIO_PIN_14
#define SEG2_6_Pin      GPIO_PIN_15

#define SEG2_GPIO_Port  GPIOB

/* Buttons: active LOW with pull-up. */
#define BUT1_Pin        GPIO_PIN_13  /* Select next mode */
#define BUT2_Pin        GPIO_PIN_14  /* Short: increase; hold: decrease */
#define BUT3_Pin        GPIO_PIN_15  /* Confirm selected time */

#define BUTTON_GPIO_Port GPIOA

void Error_Handler(void);

#endif /* __MAIN_H */
