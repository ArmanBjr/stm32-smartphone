/**
 * @file leds.c
 * @brief See leds.h.
 */
#include "leds.h"
#include "pinmap.h"
#include "app_config.h"
#include "stm32f3xx_hal.h"

typedef struct {
  GPIO_TypeDef *port;
  uint16_t      pin;
  uint8_t       blinks_left; /* 0 = idle/solid, no pattern in progress */
  uint8_t       phase_on;    /* current half of the blink (0/1) */
  uint32_t      last_toggle;
} LedState;

static LedState s_led[2];

static void led_write(LedState *ls, uint8_t on)
{
  HAL_GPIO_WritePin(ls->port, ls->pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void Leds_Init(void)
{
  s_led[LED_GREEN].port = PIN_LED_GREEN_PORT;
  s_led[LED_GREEN].pin  = PIN_LED_GREEN;
  s_led[LED_RED].port   = PIN_LED_RED_PORT;
  s_led[LED_RED].pin    = PIN_LED_RED;

  for (uint8_t i = 0; i < 2u; i++) {
    s_led[i].blinks_left = 0u;
    s_led[i].phase_on    = 0u;
    led_write(&s_led[i], 0u);
  }
}

void Leds_Set(LedId id, uint8_t on)
{
  if (id > LED_RED) {
    return;
  }
  s_led[id].blinks_left = 0u; /* solid write cancels any pattern */
  led_write(&s_led[id], on);
}

void Leds_Blink(LedId id, uint8_t count)
{
  if (id > LED_RED || count == 0u) {
    return;
  }
  LedState *ls = &s_led[id];
  ls->blinks_left = count;
  ls->phase_on = 1u;
  ls->last_toggle = HAL_GetTick();
  led_write(ls, 1u);
}

void Leds_Tick(void)
{
  uint32_t now = HAL_GetTick();
  for (uint8_t i = 0; i < 2u; i++) {
    LedState *ls = &s_led[i];
    if (ls->blinks_left == 0u) {
      continue;
    }
    if ((uint32_t)(now - ls->last_toggle) < LED_BLINK_HALF_MS) {
      continue;
    }
    ls->last_toggle = now;
    if (ls->phase_on) {
      /* end of the on-half: turn off, that completes one blink */
      led_write(ls, 0u);
      ls->phase_on = 0u;
      ls->blinks_left--;
    } else if (ls->blinks_left > 0u) {
      /* start the next blink's on-half */
      led_write(ls, 1u);
      ls->phase_on = 1u;
    }
  }
}
