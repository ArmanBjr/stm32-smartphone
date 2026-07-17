/**
 * @file app_music.c
 * @brief Phase 5: real Music Player app. See app_music.h + master plan
 *        section 3 ("app_music.c") and section 9.7 for the full set of
 *        design decisions this file implements (key mapping, background
 *        playback architecture, pot-role default, CGRAM/progress-bar
 *        budget, shuffle PRNG). Replaces the Phase 4 AppMusicTest
 *        placeholder (app_music_test.c, now deleted).
 *
 *        Controls (plan section 3's "5/4/6/2/8", NAV mode): the file tree
 *        names exactly 5 distinct Music behaviors -- play/pause, prev,
 *        next, shuffle, repeat -- a 1:1 count match with the 5 listed
 *        keys. Mapped: 5=SELECT=play/pause, 4=LEFT=prev, 6=RIGHT=next,
 *        2=UP=toggle shuffle, 8=DOWN=toggle repeat. LockType (plan section
 *        5.1: "also the pot-function toggle inside Music") toggles the pot
 *        between VOLUME and SEEK.
 */
#include "app.h"
#include "app_music.h"
#include "app_config.h"
#include "ui.h"
#include "cgram.h"
#include "widgets.h"
#include "keypad.h"
#include "buzzer.h"
#include "analog.h"
#include "seg7.h"
#include "serial.h"
#include "stm32f3xx_hal.h"
#include <stdio.h>

/* Static context (plan section 4.4: persists across suspend/resume --
 * on_suspend() is a no-op by design, so shuffle/repeat survive a trip back
 * to Menu and back, same as app_menu.c's s_sel). */
static uint8_t s_shuffle = 0;
static uint8_t s_repeat  = 0;

/** xorshift32 (plan section 3's shuffle needs *some* PRNG; xorshift avoids
 *  pulling in <stdlib.h>'s rand()/srand() for a single-use shuffle pick --
 *  a deliberately tiny, dependency-free generator, not a guess about a
 *  spec requirement since the plan names no algorithm). Seeded once from
 *  HAL_GetTick() on first use so repeated app runs don't replay the same
 *  shuffle sequence. */
static uint32_t xorshift32_next(void)
{
  static uint32_t state = 0;
  if (state == 0u) {
    state = HAL_GetTick() | 1u; /* must be nonzero */
  }
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

/** Picks a melody index in [0,count) different from `current` (when
 *  count>1; returns `current` unchanged for the degenerate count<=1 case
 *  so callers never need a separate guard). */
static uint8_t music_pick_shuffle_idx(uint8_t current, uint8_t count)
{
  if (count <= 1u) {
    return current;
  }
  uint8_t idx;
  do {
    idx = (uint8_t)(xorshift32_next() % count);
  } while (idx == current);
  return idx;
}

/** Manual "next" (plan section 3's user-driven skip, distinct from
 *  EV_SONG_END's auto-next -- buzzer.h's own Buzzer_Next() comment). Only
 *  forward navigation consults shuffle; prev intentionally stays sequential
 *  (Buzzer_Prev()) since there's no shuffle-history stack (YAGNI) -- this
 *  lets the user reliably back up even mid-shuffle instead of bouncing to
 *  another random track. */
static void music_manual_next(void)
{
  if (s_shuffle) {
    uint8_t count = Buzzer_GetMelodyCount();
    uint8_t current = Buzzer_GetMelodyIndex();
    Buzzer_PlayMelody(music_pick_shuffle_idx(current, count));
  } else {
    Buzzer_Next();
  }
}

/** Pushes the currently-loaded melody's elapsed time + song number to the
 *  7-seg's `M.SS n` layout (plan section 5.2). Minutes/seconds derive from
 *  Buzzer_GetElapsedMs() rather than EV_TICK_1S -- app_music.c polls it
 *  directly from on_tick() (APP_TASK_TICK_MS granularity, plan section
 *  9.7), so it stays correct even across a background auto-next that
 *  happens while some other app is on screen. */
static void music_update_seg7(void)
{
  uint32_t elapsed_sec = Buzzer_GetElapsedMs() / 1000u;
  uint8_t minutes = (uint8_t)(elapsed_sec / 60u);
  uint8_t seconds = (uint8_t)(elapsed_sec % 60u);
  uint8_t song_num = (uint8_t)(Buzzer_GetMelodyIndex() + 1u);
  Seg7_SetMusicInfo(minutes, seconds, song_num);
}

static void music_on_enter(void)
{
  Keypad_SetInputMode(INPUT_MODE_NAV);
  Cgram_RequestBank(CGRAM_BANK_MUSIC);
  Seg7_SetMode(SEG_MUSIC);
  /* Plan section 5.4 literal wording: "Music mode: pot value maps to seek %
   * while pot_role=SEEK" -- SEEK is Music's own default pot role; LockType
   * (section 5.1) toggles it to VOLUME and back while Music stays active.
   * phone.c's Phone_SwitchApp() forces this back to VOLUME on the way out
   * (see master plan section 9.7), so entry always starts from a known
   * SEEK state regardless of what the pot role was left at last time. */
  Analog_SetPotRole(ANALOG_POT_ROLE_SEEK);
  if (!Buzzer_IsPlaying()) {
    Buzzer_PlayMelody(Buzzer_GetMelodyIndex());
  }
  music_update_seg7();
}

static void music_on_event(const Event *ev)
{
  if (ev->type != (uint8_t)EV_KEY) {
    return;
  }
  KeyEventType kt = (KeyEventType)ev->a;
  switch (kt) {
    case KEY_EV_SELECT: /* 5 */
      Buzzer_TogglePause();
      break;
    case KEY_EV_NAV_LEFT: /* 4 */
      Buzzer_Prev();
      break;
    case KEY_EV_NAV_RIGHT: /* 6 */
      music_manual_next();
      break;
    case KEY_EV_NAV_UP: /* 2 */
      s_shuffle = !s_shuffle;
      LOG("[MUSIC] shuffle=%u", (unsigned)s_shuffle);
      break;
    case KEY_EV_NAV_DOWN: /* 8 */
      s_repeat = !s_repeat;
      LOG("[MUSIC] repeat=%u", (unsigned)s_repeat);
      break;
    case KEY_EV_LOCKTYPE:
      Analog_SetPotRole(Analog_GetPotRole() == ANALOG_POT_ROLE_SEEK
                             ? ANALOG_POT_ROLE_VOLUME
                             : ANALOG_POT_ROLE_SEEK);
      LOG("[MUSIC] pot_role=%s",
          Analog_GetPotRole() == ANALOG_POT_ROLE_SEEK ? "SEEK" : "VOLUME");
      break;
    default:
      break;
  }
  music_update_seg7();
}

static void music_on_tick(void)
{
  music_update_seg7();
}

static void music_render(void)
{
  UI_BeginFrame();
  char line[UI_COLS + 1];

  uint8_t idx = Buzzer_GetMelodyIndex();
  uint8_t count = Buzzer_GetMelodyCount();
  snprintf(line, sizeof(line), "%u/%u %s", (unsigned)(idx + 1u), (unsigned)count,
           Buzzer_GetMelodyName(idx));
  UI_Print(0, 0, line);

  /* Live progress bar (plan section 3), built-in 0xFF block char for
   * filled cells / '.' for empty -- deliberately NOT a CGRAM glyph, to
   * stay well under the 8-glyph budget (plan risk register item 2; see
   * cgram.h's CGRAM_ICON_VOL_* comment for where the 3 spent glyphs go). */
  uint32_t duration_ms = Buzzer_GetMelodyDurationMs(idx);
  uint32_t elapsed_ms = Buzzer_GetElapsedMs();
  uint8_t filled = 0;
  if (duration_ms > 0u) {
    uint32_t pct = (elapsed_ms >= duration_ms) ? 100u : (elapsed_ms * 100u) / duration_ms;
    filled = (uint8_t)((pct * MUSIC_PROGRESS_BAR_COLS) / 100u);
  }
  UI_PutChar(1, 0, (uint8_t)'[');
  for (uint8_t i = 0; i < MUSIC_PROGRESS_BAR_COLS; i++) {
    UI_PutChar(1, (uint8_t)(1u + i), (i < filled) ? 0xFFu : (uint8_t)'.');
  }
  UI_PutChar(1, (uint8_t)(1u + MUSIC_PROGRESS_BAR_COLS), (uint8_t)']');

  snprintf(line, sizeof(line), "%s Sh:%s Rp:%s",
           Buzzer_IsPlaying() ? ">" : "||",
           s_shuffle ? "On" : "Off",
           s_repeat ? "On" : "Off");
  UI_Print(2, 0, line);

  uint8_t vol = Buzzer_GetVolume();
  uint8_t icon = (vol == 0u) ? (uint8_t)CGRAM_ICON_VOL_MUTE
                              : (vol <= 50u) ? (uint8_t)CGRAM_ICON_VOL_LOW
                                             : (uint8_t)CGRAM_ICON_VOL_HIGH;
  UI_PutChar(3, 0, icon);
  snprintf(line, sizeof(line), " Vol:%3u%% Pot:%s", (unsigned)vol,
            Analog_GetPotRole() == ANALOG_POT_ROLE_SEEK ? "SEEK" : "VOL");
  UI_Print(3, 1, line);
  UI_EndFrame();
}

static void music_on_suspend(void)
{
  /* no-op by design (plan section 4.4). Pot-role/7-seg-mode reset happens
   * centrally in phone.c's Phone_SwitchApp() instead (master plan section
   * 9.7) -- on_suspend() is deliberately not the place for cleanup. */
}

const App AppMusic = {
  .on_enter   = music_on_enter,
  .on_event   = music_on_event,
  .on_tick    = music_on_tick,
  .render     = music_render,
  .on_suspend = music_on_suspend,
};

void Music_HandleSongEnd(const Event *ev)
{
  (void)ev; /* ev->a carries the melody idx that just ended, but that's
             * already reflected in Buzzer_GetMelodyIndex() -- no need to
             * re-derive it from the event payload. */
  if (s_repeat) {
    Buzzer_PlayMelody(Buzzer_GetMelodyIndex()); /* replay the same track */
    return;
  }
  if (s_shuffle) {
    uint8_t count = Buzzer_GetMelodyCount();
    uint8_t current = Buzzer_GetMelodyIndex();
    Buzzer_PlayMelody(music_pick_shuffle_idx(current, count));
    return;
  }
  Buzzer_Next(); /* sequential auto-advance, wraps */
}
