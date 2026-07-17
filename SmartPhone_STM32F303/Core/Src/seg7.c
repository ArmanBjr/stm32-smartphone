/**
 * @file seg7.c
 * @brief 7-seg driver: TIM7 @2 kHz multiplex, TIM2 @1 Hz EV_TICK_1S poster,
 *        mode-aware digit buffer (SEG_OFF default / SEG_MUSIC `M.SS n` /
 *        SEG_GAME `SS.CC` / SEG_TIME `HH.MM`, plan section 5.2/5.8/5.3
 *        bonus 3). Polarity (digit select active LOW,
 *        off-before-switch) is taken from the previous-project reference
 *        driver (ToDo/3/HangmanGame/Core/Src/seg7_display.c) and confirmed
 *        on real hardware during Phase 1 bring-up (DP active HIGH, opposite
 *        of the digit selects -- see the comment on Seg7_MuxISR() below).
 */
#include "seg7.h"
#include "pinmap.h"
#include "events.h"
#include "stm32f3xx_hal.h"

static const uint16_t s_digit_pin[4] = {
  PIN_DIGIT0, PIN_DIGIT1, PIN_DIGIT2, PIN_DIGIT3
};

static volatile uint8_t s_digit_val[4]; /* 0-9 BCD value per digit, left to right */
static volatile uint8_t s_mux_index;    /* which digit is enabled next ISR call */
static volatile Seg7Mode s_mode;        /* SEG_OFF by default (plan section 5.2) */

/* Mode-dependent DP slot: SEG_MUSIC's `M.SS n` layout puts the dot right
 * after digit0 (the minute digit) -- "M.SS n" reads minute, dot, two
 * seconds digits, song number. SEG_OFF never lights the DP (blanked in
 * Seg7_MuxISR() before this is even consulted). */
#define SEG_MUSIC_DP_INDEX 0u
/* SEG_GAME's `SS.CC` layout (survival seconds, DP, score) -- DP after
 * digit1 visually separates the two 2-digit fields, same "put the dot where
 * the two halves meet" reasoning SEG_MUSIC_DP_INDEX already uses. */
#define SEG_GAME_DP_INDEX  1u
/* SEG_TIME's `HH.MM` layout (plan section 5.3 bonus 3) -- DP after digit1
 * splits hour from minute, same "dot where the two halves meet" reasoning
 * as SEG_GAME_DP_INDEX. */
#define SEG_TIME_DP_INDEX  1u

static void write_bcd(uint8_t value)
{
  HAL_GPIO_WritePin(PIN_BCD_PORT, PIN_BCD_A, (value & 0x1u) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(PIN_BCD_PORT, PIN_BCD_B, (value & 0x2u) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(PIN_BCD_PORT, PIN_BCD_C, (value & 0x4u) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(PIN_BCD_PORT, PIN_BCD_D, (value & 0x8u) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void Seg7_Init(void)
{
  s_digit_val[0] = 0;
  s_digit_val[1] = 0;
  s_digit_val[2] = 0;
  s_digit_val[3] = 0;
  s_mux_index = 0;
  s_mode = SEG_OFF;

  /* all digits off, DP off */
  HAL_GPIO_WritePin(PIN_DIGIT_PORT, PIN_DIGIT_MASK, GPIO_PIN_SET);
  HAL_GPIO_WritePin(PIN_SEG_DP_PORT, PIN_SEG_DP, GPIO_PIN_RESET);
}

void Seg7_SetMode(Seg7Mode mode)
{
  s_mode = mode;
}

Seg7Mode Seg7_GetMode(void)
{
  return s_mode;
}

void Seg7_SetMusicInfo(uint8_t minutes, uint8_t seconds, uint8_t song_num)
{
  if (minutes > 9u) {
    minutes = 9u; /* 7448 clamp, plan section 5.2 */
  }
  if (seconds > 59u) {
    seconds = 59u;
  }
  if (song_num > 9u) {
    song_num = 9u;
  }
  s_digit_val[0] = minutes;
  s_digit_val[1] = (uint8_t)(seconds / 10u);
  s_digit_val[2] = (uint8_t)(seconds % 10u);
  s_digit_val[3] = song_num;
}

void Seg7_SetGameInfo(uint8_t survival_s, uint8_t score)
{
  if (survival_s > 59u) {
    survival_s = 59u;
  }
  if (score > 99u) {
    score = 99u;
  }
  s_digit_val[0] = (uint8_t)(survival_s / 10u);
  s_digit_val[1] = (uint8_t)(survival_s % 10u);
  s_digit_val[2] = (uint8_t)(score / 10u);
  s_digit_val[3] = (uint8_t)(score % 10u);
}

void Seg7_SetTimeInfo(uint8_t hh, uint8_t mm)
{
  if (hh > 23u) {
    hh = 23u; /* defense in depth -- RTC_Time_GetHM() already in-range */
  }
  if (mm > 59u) {
    mm = 59u;
  }
  s_digit_val[0] = (uint8_t)(hh / 10u);
  s_digit_val[1] = (uint8_t)(hh % 10u);
  s_digit_val[2] = (uint8_t)(mm / 10u);
  s_digit_val[3] = (uint8_t)(mm % 10u);
}

void Seg7_MuxISR(void)
{
  /* off-before-switch: kill all digit selects first to avoid ghosting */
  HAL_GPIO_WritePin(PIN_DIGIT_PORT, PIN_DIGIT_MASK, GPIO_PIN_SET);

  if (s_mode == SEG_OFF) {
    /* Blank the entire display (plan section 5.2 default) -- DP off, no
     * digit select re-enabled below. Still advance s_mux_index so a mode
     * switch back to SEG_MUSIC picks up mid-cycle cleanly instead of
     * always restarting at digit 0. */
    HAL_GPIO_WritePin(PIN_SEG_DP_PORT, PIN_SEG_DP, GPIO_PIN_RESET);
    s_mux_index = (uint8_t)((s_mux_index + 1u) & 0x3u);
    return;
  }

  uint8_t i = s_mux_index;
  write_bcd(s_digit_val[i]);
  /* DP active HIGH -- confirmed on hardware (Phase 1 bring-up): opposite of
   * the digit selects (SET lights it, RESET turns it off). Do not "fix"
   * this to match PIN_DIGIT* polarity -- that was tried and is wrong for
   * this specific LED. */
  uint8_t dp_index;
  switch (s_mode) {
    case SEG_GAME: dp_index = SEG_GAME_DP_INDEX; break;
    case SEG_TIME: dp_index = SEG_TIME_DP_INDEX; break;
    default:        dp_index = SEG_MUSIC_DP_INDEX; break;
  }
  HAL_GPIO_WritePin(PIN_SEG_DP_PORT, PIN_SEG_DP, (i == dp_index) ? GPIO_PIN_SET : GPIO_PIN_RESET);

  HAL_GPIO_WritePin(PIN_DIGIT_PORT, s_digit_pin[i], GPIO_PIN_RESET); /* active LOW: enable digit i */

  s_mux_index = (uint8_t)((i + 1u) & 0x3u);
}

void Seg7_SecondsTickISR(void)
{
  /* Phase-1's free-running MM:SS counter lived here directly (no event
   * queue existed yet); now that it does (events.h), this just posts
   * EV_TICK_1S and lets whichever module cares consume it from app_task, per
   * plan section 4.2's producer/consumer split. Phase 9: phone.c's
   * EV_TICK_1S handler is now the first real consumer -- refreshes
   * SEG_TIME's HH.MM from RTC_Time_GetHM() once a second (app_music.c still
   * self-updates by polling Buzzer_GetElapsedMs() instead, unrelated to this
   * event). Event_Post() is ISR-safe (0-timeout, never blocks), same
   * contract buzzer.c's EV_SONG_END post already relies on. */
  Event ev = { .type = (uint8_t)EV_TICK_1S, .a = 0, .b = 0, .c = 0 };
  Event_Post(&ev);
}
