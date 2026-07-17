## Complete Pin Assignment — HangmanGame (STM32F303VCTx)

Verified directly against `HangmanGame/Core/Src/main.c`, `stm32f3xx_hal_msp.c`,
`LiquidCrystal.c`, `keypad.c`, `led_feedback.c`, and `seg7_display.c`.

### Seven Segment / 7448 Decoder

| STM32 Pin | Connected to | Purpose |
|-----------|-------------|---------|
| PD0 | 7448 pin 7 (A) | BCD bit 0 (LSB) |
| PD1 | 7448 pin 1 (B) | BCD bit 1 |
| PD2 | 7448 pin 2 (C) | BCD bit 2 |
| PD3 | 7448 pin 6 (D) | BCD bit 3 (MSB) |
| PD7 | Display DP pin + 330Ω | Decimal point |
| PC0 | Display digit 1 cathode | Leftmost digit select |
| PC1 | Display digit 2 cathode | Second digit select |
| PC2 | Display digit 3 cathode | Third digit select |
| PC3 | Display digit 4 cathode | Rightmost digit select |

### LED / Button Circuit

| STM32 Pin | Connected to | Purpose |
|-----------|-------------|---------|
| PB0 | 330Ω → LED anode | Green LED output |
| PB1 | 330Ω → LED anode | Red LED output |
| PB3 | Button leg (other leg → GND) | Push button input (moved from PD8) |

### LCD — 20×4 HD44780, 4-bit mode

`LiquidCrystal(GPIOD, RS=PD8, RW=PD9, EN=PD10, D0=PD11, D1=PD12, D2=PD13, D3=PD14)`

| STM32 Pin | Connected to | Purpose |
|-----------|-------------|---------|
| PD8 | LCD RS | Register select |
| PD9 | LCD RW | Read/write |
| PD10 | LCD E | Enable |
| PD11 | LCD D4 | Data bit 0 (4-bit mode) |
| PD12 | LCD D5 | Data bit 1 |
| PD13 | LCD D6 | Data bit 2 |
| PD14 | LCD D7 | Data bit 3 |

### Keypad — 4×4 matrix

| STM32 Pin | Connected to | Purpose |
|-----------|-------------|---------|
| PE7 | Row 0 | EXTI rising edge, pull-down |
| PE8 | Row 1 | EXTI rising edge, pull-down |
| PE9 | Row 2 | EXTI rising edge, pull-down |
| PE10 | Row 3 | EXTI rising edge, pull-down |
| PE11 | Column 0 | Output push-pull, idle HIGH |
| PE12 | Column 1 | Output push-pull, idle HIGH |
| PE13 | Column 2 | Output push-pull, idle HIGH |
| PE14 | Column 3 | Output push-pull, idle HIGH |

### Buzzer (PWM)

| STM32 Pin | Connected to | Purpose |
|-----------|-------------|---------|
| PC6 | Buzzer drive | TIM8_CH1 PWM output |

### Volume Potentiometer (ADC)

| STM32 Pin | Connected to | Purpose |
|-----------|-------------|---------|
| PB2 | Potentiometer wiper | ADC2_IN12, analog input |

### UART (ST-Link Virtual COM)

| Pin | Reason |
|-----|--------|
| PC4 | USART1 TX (ST-Link) |
| PC5 | USART1 RX (ST-Link) |

### Free pins available for next project

| Port | Free pins |
|------|-----------|
| PD | PD4, PD5, PD6, PD15 |
| PB | PB4, PB5, PB6, PB7, PB8, PB9 |
| PA | Most of PA (check ST-Link usage) |
| PC | PC7 (PC0–PC6 used, PC4/PC5 reserved by ST-Link) |
| PF | Available (clock enabled but unused in GPIO init) |

---

### Changelog vs. previous revision

- **Button moved**: PD8 → PB3 (PD8 is now LCD RS).
- **Added red LED** on PB1 (previously only PB0 green LED existed).
- **Added LCD** on PD8–PD14 (7 pins).
- **Added keypad** on PE7–PE14 (8 pins).
- **Added buzzer** PWM on PC6 (TIM8_CH1).
- **Added volume potentiometer** on PB2 (ADC2_IN12).
- UART and 7-segment/LED assignments unchanged from previous revision.
