/**
 * @file seg7.h
 * @brief 4-digit 7-segment display buffer + TIM7 mux ISR body + TIM2 1 Hz
 *        seconds tick. Mode set per plan section 5.2: SEG_OFF (default --
 *        "خاموش در تمام مدت", off the entire time unless a mode-owning app
 *        says otherwise) and SEG_MUSIC (`M.SS n` layout, Phase 5).
 *
 *        Phase 8 update: SEG_GAME now exists (plan section 5.8 exit
 *        criterion "7-seg game mode" + user spec: "seven-segment shows
 *        [seconds-survived, wrapping at 60][score]") -- left two digits =
 *        survival seconds (0-59, wraps), right two = score (clamped to the
 *        7448's 0-99 single-BCD-digit-pair display range, same "7448 shows
 *        garbage >9 -- clamp" rule SEG_MUSIC already follows).
 *
 *        Phase 9 update: SEG_TIME now exists (plan section 5.3 bonus 3:
 *        "Settings 7-seg time mode") -- this was the mode this file's own
 *        header comment deliberately deferred until Phase 9's owning app
 *        (phone.c's centralized default-mode logic, driven by
 *        StorageSettings.segtime_enable) existed to verify against. `HH.MM`
 *        layout, refreshed once a second from phone.c's EV_TICK_1S handler
 *        (RTC_Time_GetHM()) -- not every TIM7 mux tick, since RTC minute
 *        resolution never changes faster than 1 Hz anyway.
 */
#ifndef SEG7_H
#define SEG7_H

#include <stdint.h>

typedef enum {
  SEG_OFF = 0, /* plan section 5.2 default: blank, all digits + DP off */
  SEG_MUSIC,   /* `M.SS n` -- digit0=minute, DP, digit1-2=seconds, digit3=song# */
  SEG_GAME,    /* Phase 8: digit0-1=survival seconds, digit2-3=score */
  SEG_TIME,    /* Phase 9: `HH.MM` from RTC, plan section 5.3 bonus 3 */
} Seg7Mode;

void Seg7_Init(void);

/** Sets the active display mode. Safe to call from app_task. Switching to
 *  SEG_OFF blanks immediately (next Seg7_MuxISR() call goes fully dark);
 *  switching to SEG_MUSIC keeps whatever digits Seg7_SetMusicInfo() last
 *  wrote (0/0/0 until the owning app calls it). */
void Seg7_SetMode(Seg7Mode mode);
Seg7Mode Seg7_GetMode(void);

/** Only meaningful while mode==SEG_MUSIC (plan section 5.2's `M.SS n`
 *  layout). `minutes` and `song_num` are clamped to 0-9 before display --
 *  the 7448 BCD decoder shows garbage above 9 on a single digit (plan
 *  section 5.2: "7448 shows garbage >9 -- clamp"), and a single digit is
 *  all the layout has room for. `seconds` is clamped to 0-59 and split
 *  across the two seconds digits (tens/ones), same as the Phase-1
 *  MM:SS placeholder this supersedes. Safe to call from app_task. */
void Seg7_SetMusicInfo(uint8_t minutes, uint8_t seconds, uint8_t song_num);

/** Only meaningful while mode==SEG_GAME. `survival_s` is clamped to 0-59
 *  (caller, app_pvz.c, already wraps its own counter at 60 per plan section
 *  5.8 -- clamping here too is just defense-in-depth, same belt-and-
 *  suspenders spirit as other storage.c/settings clamps in this codebase).
 *  `score` is clamped to 0-99 (two BCD digits, 7448 garbage-above-9-per-
 *  digit limit). Safe to call from app_task. */
void Seg7_SetGameInfo(uint8_t survival_s, uint8_t score);

/** Only meaningful while mode==SEG_TIME (plan section 5.3 bonus 3's `HH.MM`
 *  layout). `hh` is clamped to 0-23, `mm` to 0-59 (defense in depth -- the
 *  only caller, phone.c's EV_TICK_1S handler, already gets in-range values
 *  from RTC_Time_GetHM(), same belt-and-suspenders spirit as
 *  Seg7_SetGameInfo()'s clamp). Each is split into tens/ones BCD digits
 *  across the four digit slots. Safe to call from app_task. */
void Seg7_SetTimeInfo(uint8_t hh, uint8_t mm);

/** Called from TIM7 ISR @ 2 kHz: off-before-switch digit multiplex. */
void Seg7_MuxISR(void);

/** Called from TIM2 ISR @ 1 Hz. Posts EV_TICK_1S (plan section 4.2's event
 *  table) instead of touching the digit buffer directly -- this finally
 *  supersedes the Phase-1 free-running MM:SS placeholder that lived here
 *  (see git history / master plan section 9.7): no current consumer needs
 *  EV_TICK_1S yet (app_music.c self-updates by polling
 *  Buzzer_GetElapsedMs() from its own on_tick(), not from this event), but
 *  posting it now fulfills the event queue's own declared contract in
 *  events.h and costs nothing (unhandled event types are silently dropped
 *  by app_task's default switch case, same as every other posted-but-
 *  unconsumed EventType today). */
void Seg7_SecondsTickISR(void);

#endif /* SEG7_H */
