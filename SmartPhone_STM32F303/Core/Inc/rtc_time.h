/**
 * @file rtc_time.h
 * @brief Phase 7: thin wrapper around the CubeMX-generated `hrtc` (internal
 *        RTC, LSI clock source -- master plan section 1.1: "F3-Disco has no
 *        guaranteed LSE crystal; LSI drift is fine because time is
 *        live-synced from PC (bonus 3)"). MX_RTC_Init() (main.c) already
 *        constructs and inits `hrtc` before osKernelStart() with an
 *        arbitrary placeholder date/time (2000-01-01 00:00:00) -- this
 *        module owns only the "is it synced yet" bookkeeping and the
 *        epoch<->calendar conversion the bonus 3 `/time-{unix_epoch}`
 *        command needs (plan section 5.5), plus the HH:MM:SS string the
 *        lock screen displays (plan section 5.9).
 */
#ifndef RTC_TIME_H
#define RTC_TIME_H

#include <stdint.h>

/** Marks "unsynced" (no `/time-` received yet this power-up -- RTC keeps
 *  ticking from MX_RTC_Init()'s placeholder date across a call to this, it
 *  is not re-inited). Call once from main(), before osKernelStart(), same
 *  timing as Storage_Init()/Buzzer_Init(). */
void RTC_Time_Init(void);

/** Sets the RTC calendar from a UNIX epoch (UTC, no timezone/DST -- plan
 *  gives no timezone spec, and the lock screen only ever shows HH:MM:SS,
 *  so UTC-as-is is the simplest correct choice). Marks synced. Called from
 *  cmdparse.c on `/time-{unix_epoch}` (bonus 3, plan section 5.5). */
void RTC_Time_SetFromEpoch(uint32_t epoch_s);

/** Writes "HH:MM:SS\0" (8 chars + NUL) into `out` (caller-owned, >= 9
 *  bytes) from the RTC's current calendar, regardless of sync state --
 *  plan section 5.9: "before first /time sync it shows RTC's default
 *  epoch -- acceptable, log a [RTC] unsynced note" (that note is logged
 *  once by RTC_Time_Init(), not repeated here). */
void RTC_Time_GetString(char out[9]);

/** 1 once RTC_Time_SetFromEpoch() has been called this power-up, 0 until
 *  then (plan section 5.9's "unsynced" condition). */
uint8_t RTC_Time_IsSynced(void);

/** Phase 9 (plan section 5.3 bonus "4-digit 7-segment ... Settings 7-seg
 *  time mode"). Writes the RTC's current hour/minute directly as binary
 *  values (0-23 / 0-59), for seg7.c's Seg7_SetTimeInfo() -- avoids a
 *  string-format-then-reparse round trip through RTC_Time_GetString() for
 *  what phone.c's EV_TICK_1S handler needs. Same paired
 *  HAL_RTC_GetTime()/HAL_RTC_GetDate() call as RTC_Time_GetString() (RM0316
 *  shadow-register unlock requirement). */
void RTC_Time_GetHM(uint8_t *hh, uint8_t *mm);

#endif /* RTC_TIME_H */
