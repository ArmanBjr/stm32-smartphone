/**
 * @file analog.c
 * @brief See analog.h. Implements plan section 5.4's filter pipeline:
 *        5-sample median (kills single-sample ADC spikes) feeding an
 *        EMA(alpha ~= 0.3) (smooths the remaining jitter), independently
 *        per channel (LDR on ADC1, pot on ADC2).
 */
#include "analog.h"
#include "app_config.h"
#include "events.h"
#include "buzzer.h"
#include "serial.h"
#include <stddef.h>

#define MEDIAN_WINDOW 5U

/* Plan section 5.4 says "median(5)+EMA filter", no alpha figure given --
 * 0.3 is the plan's own illustrative value ("EMA filter (alpha ~= 0.3)"
 * quoted verbatim in this file's header), so it is not a guess. */
#define EMA_ALPHA 0.3f

typedef struct {
  uint16_t samples[MEDIAN_WINDOW];
  uint8_t  fill_idx;   /* next slot to overwrite (circular) */
  uint8_t  filled;     /* number of valid samples so far, saturates at MEDIAN_WINDOW */
  float    ema;
  uint8_t  ema_init;
} ChannelFilter;

static ChannelFilter s_ldr_filter;
static ChannelFilter s_pot_filter;

static uint8_t s_pot_applied_pct;   /* last percent actually applied (post-deadband) */
static uint8_t s_pot_have_applied;  /* 0 until the first sample bootstraps s_pot_applied_pct */

static uint8_t s_ldr_is_night;      /* Analog_IsNight()'s return value */
static uint8_t s_ldr_have_state;    /* 0 until the first LDR sample bootstraps s_ldr_is_night */

static volatile AnalogPotRole s_pot_role = ANALOG_POT_ROLE_VOLUME;

static void filter_reset(ChannelFilter *f)
{
  f->fill_idx = 0;
  f->filled = 0;
  f->ema = 0.0f;
  f->ema_init = 0;
}

/** Inserts `raw`, returns the filtered (median-then-EMA) value. Median is
 *  computed over whatever samples are filled so far (< 5 during the first
 *  4 conversions after boot/reset), so filtering is meaningful from the
 *  very first sample instead of waiting for a full window. */
static uint16_t filter_push(ChannelFilter *f, uint16_t raw)
{
  f->samples[f->fill_idx] = raw;
  f->fill_idx = (uint8_t)((f->fill_idx + 1U) % MEDIAN_WINDOW);
  if (f->filled < MEDIAN_WINDOW) {
    f->filled++;
  }

  /* Insertion sort a small scratch copy -- filled <= 5, so this is cheap
   * enough to run every conversion (20 Hz per plan's TIM4 trigger rate). */
  uint16_t sorted[MEDIAN_WINDOW];
  for (uint8_t i = 0; i < f->filled; i++) {
    sorted[i] = f->samples[i];
  }
  for (uint8_t i = 1; i < f->filled; i++) {
    uint16_t key = sorted[i];
    int8_t j = (int8_t)(i - 1);
    while (j >= 0 && sorted[j] > key) {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = key;
  }
  uint16_t median = sorted[f->filled / 2U];

  if (!f->ema_init) {
    f->ema = (float)median; /* bootstrap: no lag on the very first sample */
    f->ema_init = 1;
  } else {
    f->ema += EMA_ALPHA * ((float)median - f->ema);
  }
  return (uint16_t)(f->ema + 0.5f);
}

void Analog_Init(void)
{
  filter_reset(&s_ldr_filter);
  filter_reset(&s_pot_filter);
  s_pot_applied_pct = 0;
  s_pot_have_applied = 0;
  s_ldr_is_night = 0;
  s_ldr_have_state = 0;
}

/** Applies `pct` per the current pot role (volume vs. Phase 5 seek) and
 *  posts EV_ADC_POT so any listening app (e.g. app_music.c's live 3-state
 *  icon / progress-bar refresh) can react without polling. */
static void apply_pot_pct(uint8_t pct)
{
  if (s_pot_role == ANALOG_POT_ROLE_SEEK) {
    Buzzer_SeekPercent(pct);
    LOG("[ADC] pot seek=%u", (unsigned)pct);
  } else {
    Buzzer_SetVolume(pct);
    LOG("[ADC] pot volume=%u", (unsigned)pct);
  }
  Event ev = { .type = (uint8_t)EV_ADC_POT, .a = pct, .b = 0, .c = 0 };
  Event_Post(&ev);
}

static void handle_pot(uint16_t raw)
{
  uint16_t filtered = filter_push(&s_pot_filter, raw);
  /* 12-bit ADC domain (0-4095) scaled to the 0-100 volume/seek units
   * Buzzer_SetVolume()/Buzzer_SeekPercent() take (app_config.h's own
   * BUZZER_* comments use this same 0-100 unit convention). */
  uint8_t pct = (uint8_t)(((uint32_t)filtered * 100U) / 4095U);

  if (!s_pot_have_applied) {
    /* First sample: apply immediately, no deadband gate -- there is no
     * previous value to debounce against yet. */
    s_pot_applied_pct = pct;
    s_pot_have_applied = 1;
    apply_pot_pct(pct);
    return;
  }

  int16_t diff = (int16_t)pct - (int16_t)s_pot_applied_pct;
  if (diff < 0) {
    diff = (int16_t)(-diff);
  }
  if ((uint16_t)diff < ANALOG_POT_DEADBAND_PCT) {
    return; /* plan section 5.4: "+-2/100 to stop volume jitter" */
  }

  s_pot_applied_pct = pct;
  apply_pot_pct(pct);
}

static void handle_ldr(uint16_t raw)
{
  uint16_t filtered = filter_push(&s_ldr_filter, raw);

  if (!s_ldr_have_state) {
    /* Bootstrap from a plain threshold compare (no prior state to
     * hysteresis against) -- higher raw count = more light (app_config.h's
     * ANALOG_LDR_THRESHOLD_RAW comment), so below-threshold means night. */
    s_ldr_is_night = (filtered < ANALOG_LDR_THRESHOLD_RAW) ? 1U : 0U;
    s_ldr_have_state = 1;
    LOG("[LDR] initial state: %s", s_ldr_is_night ? "night" : "day");
    return;
  }

  if (!s_ldr_is_night && filtered < (ANALOG_LDR_THRESHOLD_RAW - ANALOG_LDR_HYSTERESIS_RAW)) {
    s_ldr_is_night = 1U;
    Event ev = { .type = (uint8_t)EV_ADC_LDR_EDGE, .a = s_ldr_is_night, .b = 0, .c = 0 };
    Event_Post(&ev);
    LOG("[LDR] day->night");
  } else if (s_ldr_is_night && filtered > (ANALOG_LDR_THRESHOLD_RAW + ANALOG_LDR_HYSTERESIS_RAW)) {
    s_ldr_is_night = 0U;
    Event ev = { .type = (uint8_t)EV_ADC_LDR_EDGE, .a = s_ldr_is_night, .b = 0, .c = 0 };
    Event_Post(&ev);
    LOG("[LDR] night->day");
  }
  /* else: inside the hysteresis band or no crossing -- no-op, per plan
   * section 5.4's "+-5%" hysteresis requirement. */
}

void Analog_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc == NULL) {
    return;
  }
  uint16_t raw = (uint16_t)HAL_ADC_GetValue(hadc);
  if (hadc->Instance == ADC1) {
    handle_ldr(raw);
  } else if (hadc->Instance == ADC2) {
    handle_pot(raw);
  }

  /* MX_ADCx_Init() configures single (non-continuous) conversion mode
   * triggered by TIM4's TRGO (20 Hz, plan section 2.3/5.4): per the
   * reference manual, ADSTART is cleared by hardware at end-of-conversion
   * in this mode, so the next TRGO edge would otherwise be silently
   * ignored. Re-arming here (not in main.c) keeps main.c's dispatcher a
   * pure pass-through and keeps all ADC-triggering knowledge in the one
   * module that owns the filter pipeline consuming it. */
  HAL_ADC_Start_IT(hadc);
}

uint8_t Analog_IsNight(void)
{
  return s_ldr_is_night;
}

void Analog_SetPotRole(AnalogPotRole role)
{
  s_pot_role = role;
  /* Deliberately does NOT reset s_pot_have_applied/s_pot_applied_pct: a
   * role switch shouldn't cause a spurious deadband-bypassed re-apply on
   * the very next sample (e.g. flipping VOLUME->SEEK->VOLUME rapidly via
   * LockType shouldn't jitter the volume). */
}

AnalogPotRole Analog_GetPotRole(void)
{
  return s_pot_role;
}
