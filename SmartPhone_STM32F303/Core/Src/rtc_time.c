/**
 * @file rtc_time.c
 * @brief See rtc_time.h. Epoch<->calendar conversion uses the standard
 *        Howard Hinnant "civil_from_days" algorithm (proleptic Gregorian,
 *        no library dependency, no year-2038-style overflow issue at our
 *        32-bit-epoch/2026 timeframe) -- picked over HAL_RTC's own BCD
 *        packing helpers because HAL has no epoch<->calendar conversion of
 *        its own, only BCD<->binary for fields we already have.
 */
#include "rtc_time.h"
#include "serial.h"
#include "stm32f3xx_hal.h"
#include <stdio.h>

extern RTC_HandleTypeDef hrtc; /* defined in main.c (CubeMX), same
                                 * extern-from-.c pattern buzzer.c/analog.c
                                 * already use for their own HAL handles */

static uint8_t s_synced;

/* civil_from_days: days since 1970-01-01 -> {y, m, d}. */
static void civil_from_days(int32_t z, int *y, unsigned *m, unsigned *d)
{
  z += 719468;
  int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int32_t yr = (int32_t)yoe + era * 400;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint32_t mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp + (mp < 10 ? 3 : -9);
  *y = (int)(yr + (*m <= 2 ? 1 : 0));
}

void RTC_Time_Init(void)
{
  s_synced = 0u;
  LOG("[RTC] unsynced");
}

void RTC_Time_SetFromEpoch(uint32_t epoch_s)
{
  int32_t days = (int32_t)(epoch_s / 86400u);
  uint32_t secs_of_day = epoch_s % 86400u;

  int y;
  unsigned m, d;
  civil_from_days(days, &y, &m, &d);

  RTC_TimeTypeDef t = {0};
  t.Hours   = (uint8_t)(secs_of_day / 3600u);
  t.Minutes = (uint8_t)((secs_of_day / 60u) % 60u);
  t.Seconds = (uint8_t)(secs_of_day % 60u);
  t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  t.StoreOperation = RTC_STOREOPERATION_RESET;

  RTC_DateTypeDef dt = {0};
  dt.Year  = (uint8_t)((y - 2000) & 0xFF); /* HAL's Year field is 0-99 (2000-2099) */
  dt.Month = (uint8_t)m;
  dt.Date  = (uint8_t)d;
  dt.WeekDay = RTC_WEEKDAY_MONDAY; /* not used by anything downstream
                                     * (lock screen only shows HH:MM:SS) --
                                     * HAL requires *some* value, exact
                                     * weekday is cosmetic only here */

  HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BIN);
  HAL_RTC_SetDate(&hrtc, &dt, RTC_FORMAT_BIN);

  s_synced = 1u;
  LOG("[RTC] synced from epoch %lu", (unsigned long)epoch_s);
}

void RTC_Time_GetString(char out[9])
{
  RTC_TimeTypeDef t = {0};
  RTC_DateTypeDef d = {0}; /* HAL_RTC_GetTime() requires a paired
                             * HAL_RTC_GetDate() call to unlock the
                             * shadow registers (RM0316), even though we
                             * don't use the date fields here */
  HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN);
  HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);

  (void)snprintf(out, 9, "%02u:%02u:%02u", (unsigned)t.Hours,
                 (unsigned)t.Minutes, (unsigned)t.Seconds);
}

uint8_t RTC_Time_IsSynced(void)
{
  return s_synced;
}

void RTC_Time_GetHM(uint8_t *hh, uint8_t *mm)
{
  RTC_TimeTypeDef t = {0};
  RTC_DateTypeDef d = {0}; /* see RTC_Time_GetString()'s comment on this
                             * paired call being required, not optional */
  HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN);
  HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);
  *hh = t.Hours;
  *mm = t.Minutes;
}
