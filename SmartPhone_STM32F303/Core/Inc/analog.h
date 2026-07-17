/**
 * @file analog.h
 * @brief Phase 4: ADC pipeline (pot volume + LDR day/night). Source of
 *        truth: master plan section 5.4 ("Pot: median(5)+EMA filter,
 *        deadband +-2/100, drives buzzer volume. LDR: compare vs
 *        settings.ldr_threshold with hysteresis +-5%, posts
 *        EV_ADC_LDR_EDGE") and section 2.3/2.4's TIM4 (20 Hz ADC trigger,
 *        TRGO only, no IRQ) / ADC1 (LDR, PA1/IN2) / ADC2 (pot, PB2/IN12)
 *        rows.
 *
 *        Ownership split, mirroring buzzer.h/ui.h's app_task-vs-ISR split
 *        (rule 0.2): Analog_Init() runs once from app-init context (before
 *        the scheduler drains events); Analog_ConvCpltCallback() runs ONLY
 *        from main.c's HAL_ADC_ConvCpltCallback() dispatch (ADC1/ADC2 EOC
 *        IRQ context) -- never from task context. Unlike buzzer.c (which
 *        externs htim8 directly, since TIM8/PC6 is buzzer-exclusive
 *        hardware), analog.c needs NO ADC handle externs at all:
 *        HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc) already receives
 *        the firing handle as a parameter, so main.c's dispatcher passes it
 *        straight through -- there is no shared-resource question to
 *        design around here.
 *
 *        Analog_IsNight() is a plain accessor (single-word volatile read,
 *        safe from app_task) consumed by app_menu.c to pick
 *        CGRAM_BANK_MENU_ICONS_DAY vs _NIGHT (cgram.h: BANK_MENU_ICONS_NIGHT
 *        is explicitly Phase-4 scope).
 */
#ifndef ANALOG_H
#define ANALOG_H

#include "stm32f3xx_hal.h"
#include <stdint.h>

/** Phase 5 (plan section 5.4: "Music mode: pot value maps to seek % while
 *  pot_role=SEEK; LockType...is also the pot-function toggle inside Music"
 *  -- section 5.1's framing, read together with 5.4, makes this a toggle
 *  between the two roles while Music is active, not a one-way flip).
 *  Default is VOLUME everywhere outside Music; app_music.c flips to SEEK
 *  on entry/LockType and phone.c's Phone_SwitchApp() forces it back to
 *  VOLUME when leaving Music (mirrors Seg7_SetMode()'s same reset). */
typedef enum {
  ANALOG_POT_ROLE_VOLUME = 0,
  ANALOG_POT_ROLE_SEEK,
} AnalogPotRole;

void Analog_SetPotRole(AnalogPotRole role);
AnalogPotRole Analog_GetPotRole(void);

void Analog_Init(void);

/** Call ONLY from main.c's HAL_ADC_ConvCpltCallback() dispatch (ADC1/ADC2
 *  EOC IRQ context) -- never from task context. Dispatches on
 *  hadc->Instance (ADC1 == LDR, ADC2 == pot), runs the median(5)+EMA
 *  filter for that channel, and on pot: applies the deadband and calls
 *  Buzzer_SetVolume() + posts EV_ADC_POT on change; on LDR: checks the
 *  hysteresis band and posts EV_ADC_LDR_EDGE on a day/night crossing. */
void Analog_ConvCpltCallback(ADC_HandleTypeDef *hadc);

/** 1 if the LDR's filtered reading currently reads "night" (raw counts
 *  below threshold, hysteresis-adjusted), 0 for "day". Safe to call from
 *  app_task (single-word volatile read). Reflects the last computed
 *  edge state -- valid once the first LDR conversion has completed. */
uint8_t Analog_IsNight(void);

#endif /* ANALOG_H */
