/**
 * @file pinmap.h
 * @brief Every pin/port + timer/channel macro in ONE place.
 *        Source of truth: SmartPhone_STM32F303_MasterPlan.md section 1.1 (Hardware map).
 *        Cross-checked against the CubeMX-generated MX_GPIO_Init() in main.c
 *        (Phase 0) so these macros describe hardware that is ALREADY configured,
 *        not guessed pin assignments.
 */
#ifndef PINMAP_H
#define PINMAP_H

#include "stm32f3xx_hal.h"

/* ---- 7448 BCD input (to 7-seg decoder), LSB..MSB ------------------------ */
#define PIN_BCD_PORT        GPIOD
#define PIN_BCD_A           GPIO_PIN_0
#define PIN_BCD_B           GPIO_PIN_1
#define PIN_BCD_C           GPIO_PIN_2
#define PIN_BCD_D           GPIO_PIN_3
#define PIN_BCD_MASK        (PIN_BCD_A | PIN_BCD_B | PIN_BCD_C | PIN_BCD_D)

/* ---- 7-seg decimal point (via 330 ohm), active HIGH -----------------------
 * Confirmed on hardware (Phase 1 bring-up): SET lights it, RESET turns it
 * off -- OPPOSITE polarity from the digit selects below (this LED is wired
 * directly via its own resistor, not through the shared cathode circuit). */
#define PIN_SEG_DP_PORT      GPIOD
#define PIN_SEG_DP           GPIO_PIN_7

/* ---- 7-seg digit selects (cathodes), active LOW -------------------------
 * Polarity confirmed from previous-project reference driver
 * (ToDo/3/HangmanGame/Core/Src/seg7_display.c): digit is turned ON by
 * writing its select pin RESET (LOW) and all others SET (HIGH).
 * Physically re-verified on THIS board during Phase 1 bring-up -- correct
 * as coded, no flip needed. */
#define PIN_DIGIT_PORT       GPIOC
#define PIN_DIGIT0           GPIO_PIN_0
#define PIN_DIGIT1           GPIO_PIN_1
#define PIN_DIGIT2           GPIO_PIN_2
#define PIN_DIGIT3           GPIO_PIN_3
#define PIN_DIGIT_MASK       (PIN_DIGIT0 | PIN_DIGIT1 | PIN_DIGIT2 | PIN_DIGIT3)

/* ---- LEDs ----------------------------------------------------------------*/
#define PIN_LED_GREEN_PORT   GPIOB
#define PIN_LED_GREEN        GPIO_PIN_0
#define PIN_LED_RED_PORT     GPIOB
#define PIN_LED_RED          GPIO_PIN_1

/* ---- Quick-lock push button (EXTI3, pull-down, rising edge) --------------*/
#define PIN_BTN_LOCK_PORT    GPIOB
#define PIN_BTN_LOCK         GPIO_PIN_3

/* ---- LCD 20x4 HD44780 4-bit ------------------------------------------------
 * RS=PD8 RW=PD9 EN=PD10 D4..D7=PD11..PD14 */
#define PIN_LCD_PORT         GPIOD
#define PIN_LCD_RS           GPIO_PIN_8
#define PIN_LCD_RW           GPIO_PIN_9
#define PIN_LCD_EN           GPIO_PIN_10
#define PIN_LCD_D4           GPIO_PIN_11
#define PIN_LCD_D5           GPIO_PIN_12
#define PIN_LCD_D6           GPIO_PIN_13
#define PIN_LCD_D7           GPIO_PIN_14

/* ---- Keypad 4x4 -----------------------------------------------------------
 * Rows 0-3 (inputs, EXTI rising, pull-down): PE7/PE8/PE9/PE10
 *   EXTI9_5 covers PE7-9, EXTI15_10 covers PE10.
 * Cols 0-3 (outputs, push-pull, idle HIGH): PE11/PE12/PE13/PE14 */
#define PIN_KEY_ROW_PORT     GPIOE
#define PIN_KEY_ROW0         GPIO_PIN_7
#define PIN_KEY_ROW1         GPIO_PIN_8
#define PIN_KEY_ROW2         GPIO_PIN_9
#define PIN_KEY_ROW3         GPIO_PIN_10
#define PIN_KEY_ROW_MASK     (PIN_KEY_ROW0 | PIN_KEY_ROW1 | PIN_KEY_ROW2 | PIN_KEY_ROW3)

#define PIN_KEY_COL_PORT     GPIOE
#define PIN_KEY_COL0         GPIO_PIN_11
#define PIN_KEY_COL1         GPIO_PIN_12
#define PIN_KEY_COL2         GPIO_PIN_13
#define PIN_KEY_COL3         GPIO_PIN_14
#define PIN_KEY_COL_MASK     (PIN_KEY_COL0 | PIN_KEY_COL1 | PIN_KEY_COL2 | PIN_KEY_COL3)

/* ---- Buzzer (passive), TIM8_CH1 PWM (AF4) --------------------------------*/
#define PIN_BUZZER_PORT      GPIOC
#define PIN_BUZZER           GPIO_PIN_6

/* ---- Potentiometer, ADC2_IN12 --------------------------------------------*/
#define PIN_POT_PORT         GPIOB
#define PIN_POT              GPIO_PIN_2

/* ---- LDR, ADC1_IN2 ---------------------------------------------------------*/
#define PIN_LDR_PORT         GPIOA
#define PIN_LDR              GPIO_PIN_1

/* ---- UART to PC (Termite): PC4=TX PC5=RX, AF7, 115200 8N1 -----------------*/
#define PIN_UART_TX_PORT     GPIOC
#define PIN_UART_TX          GPIO_PIN_4
#define PIN_UART_RX_PORT     GPIOC
#define PIN_UART_RX          GPIO_PIN_5

#endif /* PINMAP_H */
