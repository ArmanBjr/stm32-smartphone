/**
 * @file leds.h
 * @brief Phase 7: green/red LED driver (plan section 3 file tree: "leds.h/.c
 *        -- green/red LED patterns: solid, blink-n, tick-driven"). GPIO
 *        pins (PB0 green, PB1 red -- pinmap.h) are already configured as
 *        outputs by CubeMX's MX_GPIO_Init() (main.c); this module only owns
 *        the on/off + timed-blink-count state machine on top of them.
 *        "Tick-driven" per the file-tree wording means Leds_Tick() is
 *        polled from app_task's existing APP_TASK_TICK_MS cadence
 *        (Phone_Tick(), same pattern Widgets_BlinkOn() uses for its own
 *        HAL_GetTick()-based 500 ms toggle) rather than owning a dedicated
 *        timer/ISR -- no plan section asks for sub-tick LED timing
 *        precision, so this is the smallest change that satisfies the
 *        spec (YAGNI, common/coding-style.md).
 *
 *        Consumers (plan section 5.8/9 spirit, applied where a later phase
 *        needs a status indicator): red LED blinks on PvZ life-loss
 *        (Phase 8, not this phase's concern), green LED can mark
 *        general "OK"/boot-complete status. Phase 7 itself only needs the
 *        driver to exist and be exercised by Settings/lock hardware tests.
 */
#ifndef LEDS_H
#define LEDS_H

#include <stdint.h>

typedef enum {
  LED_GREEN = 0,
  LED_RED,
} LedId;

void Leds_Init(void);

/** Solid on/off, cancels any in-progress blink pattern on this LED. */
void Leds_Set(LedId id, uint8_t on);

/** Starts a blink-`count` pattern (each "blink" = one on-half + one
 *  off-half, LED_BLINK_HALF_MS each, app_config.h) then leaves the LED off.
 *  count==0 is a no-op. Overrides any in-progress pattern on this LED. */
void Leds_Blink(LedId id, uint8_t count);

/** Advances any in-progress blink pattern. Call every APP_TASK_TICK_MS from
 *  app_task context (Phone_Tick()) -- never from an ISR. */
void Leds_Tick(void);

#endif /* LEDS_H */
