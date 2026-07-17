/**
 * @file phone.c
 * @brief See phone.h.
 */
#include "phone.h"
#include "app_config.h"
#include "ui.h"
#include "cgram.h"
#include "keypad.h"
#include "serial.h"
#include "buzzer.h"
#include "analog.h"
#include "seg7.h"
#include "rtc_time.h"
#include "app_music.h"
#include "cmdparse.h"
#include "storage.h"
#include "leds.h"
#include "cmsis_os.h"
#include "stm32f3xx_hal.h"
#include <stddef.h>
#include <stdint.h>

static const App *s_current = NULL;

/* Phase 7: `/start` boot gate (plan section 5.9) -- Phone_Init() only shows
 * the idle screen; Phone_Boot() (cmdparse.c on `/start`) does the actual
 * logo->Menu transition, guarded by s_booted so a stray repeated `/start`
 * is a no-op. */
static uint8_t s_booted = 0u;

/* Phase 7: lock overlay (plan section 5.9) -- remembers whatever app was
 * current at the moment Phone_Lock() ran, so Phone_UnlockToSaved() can
 * resume it exactly. NULL means "not currently locked". */
static const App *s_locked_from = NULL;

/* Phase 7: auto-lock idle timer (plan section 5.9 "innovation": settings.
 * autolock_s, 0 = disabled). Updated on every EV_KEY seen by
 * Phone_Dispatch(), checked in Phone_Tick(). */
static uint32_t s_last_activity_tick = 0u;

/* Phase 8 (PDF p.7: game BGM while in PvZ; phone music stops; plan
 * section 5.8: leaving resumes phone music). Single hardware channel
 * (buzzer.h): entering AppPvz snapshots any in-progress phone melody
 * (index + seek percent) so AppPvz can Buzzer_PlayMelody(PVZ_BGM_*) without
 * destroying the restore point; leaving restores via PlayMelody+Seek or
 * Buzzer_Stop() if nothing was playing. Pause/Resume alone is no longer
 * enough -- PlayMelody overwrites the paused melody index. */
static uint8_t s_pvz_saved_music = 0u;
static uint8_t s_pvz_saved_idx = 0u;
static uint8_t s_pvz_saved_pct = 0u;

/* Phase 9 (plan section 6: piano.py live notes): same single-hardware-
 * channel arbitration as AppPvz above -- entering AppPiano must not lose an
 * in-progress phone melody, since Buzzer_PianoNoteOn()/Off() (buzzer.c)
 * drive the same PWM channel a melody would. Mirrors s_pvz_saved_* exactly;
 * kept as separate variables (not shared with PvZ's) since a user could in
 * principle background-hop Menu->Piano->Menu->PvZ without landing back on
 * Music in between, and each snapshot must restore independently. */
static uint8_t s_piano_saved_music = 0u;
static uint8_t s_piano_saved_idx = 0u;
static uint8_t s_piano_saved_pct = 0u;

/* Phase 9 (plan section 5.3 bonus 3): what the 7-seg display should show
 * when no mode-owning app (Music/PvZ) is currently claiming it. Centralized
 * here (not read directly from Storage by every call site) so both
 * phone.c's own SwitchApp transitions and app_settings.c's live toggle
 * agree on the same rule. */
static Seg7Mode phone_default_seg7_mode(void)
{
  return Storage_GetSettings()->segtime_enable ? SEG_TIME : SEG_OFF;
}

void Phone_ApplySeg7Default(void)
{
  Seg7_SetMode(phone_default_seg7_mode());
}

void Phone_SwitchApp(const App *app)
{
  if (app == NULL || app == s_current) {
    return;
  }
  if (s_current != NULL && s_current->on_suspend != NULL) {
    s_current->on_suspend(); /* no-op by design, see app.h */
  }
  if (s_current == &AppMusic && app != &AppMusic) {
    /* Pot role (VOLUME vs SEEK) and 7-seg mode (OFF vs MUSIC) are
     * cross-cutting global states that should only be non-default while
     * Music is actually the active app -- centralized here rather than in
     * AppMusic's on_suspend() (deliberately a no-op by design, plan
     * section 4.4) because on_suspend() is not meant for cleanup. Mirrors
     * the existing `s_current != &AppMenu` pointer-comparison pattern
     * already used for the global BACK handler below. See master plan
     * section 9.7 for the full rationale. */
    Analog_SetPotRole(ANALOG_POT_ROLE_VOLUME);
    Phone_ApplySeg7Default();
  }
  if (s_current != &AppPvz && app == &AppPvz) {
    /* Snapshot phone music before AppPvz's on_enter() starts game BGM. */
    if (Buzzer_IsPlaying()) {
      s_pvz_saved_music = 1u;
      s_pvz_saved_idx = Buzzer_GetMelodyIndex();
      {
        uint32_t dur = Buzzer_GetMelodyDurationMs(s_pvz_saved_idx);
        uint32_t el = Buzzer_GetElapsedMs();
        s_pvz_saved_pct = (dur > 0u) ? (uint8_t)((el * 100u) / dur) : 0u;
        if (s_pvz_saved_pct > 100u) {
          s_pvz_saved_pct = 100u;
        }
      }
    } else {
      s_pvz_saved_music = 0u;
    }
  } else if (s_current == &AppPvz && app != &AppPvz) {
    /* Leaving PvZ: stop game BGM / restore phone music; blank 7-seg and
     * LEDs PvZ may have left on (green "life OK", red blink). */
    if (s_pvz_saved_music) {
      Buzzer_PlayMelody(s_pvz_saved_idx);
      Buzzer_SeekPercent(s_pvz_saved_pct);
      s_pvz_saved_music = 0u;
    } else {
      Buzzer_Stop();
    }
    Phone_ApplySeg7Default();
    Leds_Set(LED_GREEN, 0u);
    Leds_Set(LED_RED, 0u);
  }
  if (s_current != &AppPiano && app == &AppPiano) {
    /* Snapshot phone music before AppPiano starts driving the buzzer
     * channel directly via Buzzer_PianoNoteOn()/Off() -- same reasoning as
     * the AppPvz snapshot above. */
    if (Buzzer_IsPlaying()) {
      s_piano_saved_music = 1u;
      s_piano_saved_idx = Buzzer_GetMelodyIndex();
      {
        uint32_t dur = Buzzer_GetMelodyDurationMs(s_piano_saved_idx);
        uint32_t el = Buzzer_GetElapsedMs();
        s_piano_saved_pct = (dur > 0u) ? (uint8_t)((el * 100u) / dur) : 0u;
        if (s_piano_saved_pct > 100u) {
          s_piano_saved_pct = 100u;
        }
      }
    } else {
      s_piano_saved_music = 0u;
    }
  } else if (s_current == &AppPiano && app != &AppPiano) {
    /* Leaving Piano: make sure no note is left stuck on (user could switch
     * apps mid-keypress via a shortcut/BACK), then restore phone music. */
    Buzzer_PianoNoteOff();
    /* Machine token for host bridge — disarms PC keyboard capture. */
    LOG("[PIANO] exit");
    if (s_piano_saved_music) {
      Buzzer_PlayMelody(s_piano_saved_idx);
      Buzzer_SeekPercent(s_piano_saved_pct);
      s_piano_saved_music = 0u;
    } else {
      Buzzer_Stop();
    }
  }
  s_current = app;
  LOG("[PHONE] switch app");
  /* Round-4 boot-loop chase instrumentation (bracketing on_enter()/render()
   * with "[PHONE] on_enter done"/"[PHONE] render done") removed in round 5:
   * that bracketing is what proved on_enter() completed but render() never
   * printed its "done" line, which traced the boot loop to UI_RenderDirty()
   * holding s_mutex for a legitimately slow first full-screen render (see
   * ui.c's UI_Init() comment for the root cause and fix). Master plan
   * section 9.5 records the full diagnostic history. */
  if (s_current->on_enter != NULL) {
    s_current->on_enter();
  }
  if (s_current->render != NULL) {
    s_current->render();
  }
}

void Phone_Init(void)
{
  /* Idle "press /start" screen (plan section 5.9). Does NOT switch to Menu
   * -- that only happens once Phone_Boot() runs (`/start`, cmdparse.c). */
  UI_Clear();
  UI_Print(1, 3, "SmartPhone");
  UI_Print(2, 1, "press /start...");
  LOG("[BOOT] waiting for /start");
}

void Phone_Boot(void)
{
  if (s_booted) {
    return; /* idempotent -- stray repeated `/start` after boot is a no-op */
  }
  s_booted = 1u;

  /* Animated logo (plan section 5.9: "animated cactus/custom logo + boot
   * jingle"). One-time boot delay in app_task, not a polling loop, so this
   * does not violate rule 0.1. */
  Cgram_RequestBank(CGRAM_BANK_LOGO);
  UI_Clear();
  UI_PutChar(1, 8, (uint8_t)CGRAM_LOGO_L);
  UI_PutChar(1, 9, (uint8_t)CGRAM_LOGO_R);
  UI_Print(2, 5, "SmartPhone");
  LOG("[BOOT] Phase 7: /start -> boot sequence");

  osDelay(BOOT_LOGO_MS);

  s_last_activity_tick = HAL_GetTick();
  Phone_SwitchApp(&AppMenu);
  /* Phase 9: apply the segtime_enable-driven 7-seg default now that Storage
   * (loaded before osKernelStart(), main.c) is available -- picks up
   * SEG_TIME instead of SEG_OFF if the user had it on from a previous
   * power-up. */
  Phone_ApplySeg7Default();
}

void Phone_Lock(void)
{
  if (!s_booted || s_current == &AppLock) {
    return; /* no-op if not yet booted or already locked */
  }
  s_locked_from = s_current;
  Storage_RequestSave(); /* plan section 5.6: "/lock" is a save trigger */
  Phone_SwitchApp(&AppLock);
}

void Phone_UnlockToSaved(void)
{
  const App *target = (s_locked_from != NULL) ? s_locked_from : &AppMenu;
  s_locked_from = NULL;
  s_last_activity_tick = HAL_GetTick();
  Phone_SwitchApp(target);
}

void Phone_Dispatch(const Event *ev)
{
  if (ev == NULL) {
    return;
  }

  if (ev->type == (uint8_t)EV_UART_CMD) {
    /* Must be handled before the s_current==NULL guard below -- the very
     * first `/start` command has to be processable while s_current is
     * still NULL (pre-boot). ev->c holds a pointer (see serial.c) into one
     * of the two ping-pong rx_line[] buffers; ev->a/ev->b (active index,
     * length) are not needed here since the line is already NUL-terminated
     * by serial.c's RX ISR before the event is posted.
     *
     * Bug fix (post-Phase-7 hardware report: Settings screen appearing to
     * "reload" on its own): UART command traffic must count as activity
     * for the auto-lock idle timer too, same as EV_KEY below -- otherwise a
     * user actively driving the phone over UART (e.g. testing
     * `/setting-*` commands while Settings is on screen, with no physical
     * keypad presses) still gets silently auto-locked once
     * settings.autolock_s elapses, then instantly unlocked back to the
     * exact same screen on their next keypress (pin_enabled defaults to
     * 0 -- any key unlocks) -- which looks indistinguishable from the
     * screen spontaneously "reloading" itself. Updating the timer here
     * closes that gap; harmless to also touch it pre-boot (s_current is
     * still NULL then, so Phone_Tick()'s autolock check is a no-op until
     * Phone_Boot() runs anyway). */
    s_last_activity_tick = HAL_GetTick();
    Cmdparse_HandleLine((const char *)(uintptr_t)ev->c);
    return;
  }

  if (s_current == NULL) {
    return;
  }

  if (ev->type == (uint8_t)EV_TICK_1S) {
    /* Phase 9 (plan section 5.3 bonus 3): refresh SEG_TIME's HH.MM once a
     * second from the RTC. Cheap no-op when a mode-owning app (Music/PvZ)
     * currently has the display in a different mode -- Seg7_SetTimeInfo()
     * would just write into a digit buffer Seg7_MuxISR() isn't reading from
     * right now, no harm done, and it saves a Seg7_GetMode() branch here for
     * a once-a-second event. */
    uint8_t hh, mm;
    RTC_Time_GetHM(&hh, &mm);
    Seg7_SetTimeInfo(hh, mm);
    return;
  }

  if (ev->type == (uint8_t)EV_BTN_LOCK) {
    /* PB3 quick-lock button (keypad.c's EXTI3 handler, plan section 5.9). */
    Phone_Lock();
    return;
  }

  if (ev->type == (uint8_t)EV_SONG_END) {
    /* Background playback + auto-next (plan section 3) must keep working
     * even when Music isn't the app on screen -- handled globally here.
     * While AppPvz is current, PDF p.7 game BGM owns the channel:
     * Music_HandleSongEnd must NOT steal the next track; AppPvz's own
     * on_event() restarts PVZ_BGM_MELODY_IDX (or stays silent on
     * game-over jingle end). See master plan section 9.10. */
    /* AppPvz owns game BGM; AppPiano owns the channel via BUZZER_PIANO
     * (melody timing frozen -- EV_SONG_END should not fire there, but
     * exclude defensively so Music auto-next never steals the channel). */
    if (s_current != &AppPvz && s_current != &AppPiano) {
      Music_HandleSongEnd(ev);
    }
  }

  if (ev->type == (uint8_t)EV_KEY) {
    KeyEventType kt = (KeyEventType)ev->a;
    s_last_activity_tick = HAL_GetTick(); /* any key resets the auto-lock
                                            * idle timer (plan section 5.9
                                            * "innovation") */
    if (kt == KEY_EV_BACK && s_current != &AppMenu && s_current != &AppLock) {
      /* Nested screens (PvZ Settings, SMS compose, Note editor, ...)
       * consume BACK via App.on_back() and stay in the app. Root screen
       * (or apps with no on_back) still exits to Menu per plan section
       * 1.2: "back: always exits current app -> Menu, preserving app
       * state". Excludes AppLock so BACK cannot bypass the lock screen. */
      if (s_current->on_back != NULL && s_current->on_back() != 0u) {
        if (s_current->render != NULL) {
          s_current->render();
        }
        return;
      }
      Phone_SwitchApp(&AppMenu);
      return;
    }
    if (kt == KEY_EV_SHORTCUT_C && s_current != &AppLock) {
      /* plan section 1.2/5.9: C = QuickLock, now live now that app_lock.c
       * exists (mirrors A/B's own reasoning below for why they went live
       * in Phase 4 once the buzzer subsystem existed). */
      Phone_Lock();
      return;
    }
    if (kt == KEY_EV_SHORTCUT_A || kt == KEY_EV_SHORTCUT_B) {
      /* plan section 1.2: "A = Vol+, B = Vol-". Handled globally (like
       * BACK above) so Vol+/- works from anywhere in the shell, not just
       * inside a music app -- matches I11's (plan section 8 risk
       * register) own reasoning that shortcut wiring is deferred only
       * "until those subsystems ... exist": the buzzer/volume subsystem
       * exists as of Phase 4, so A/B become live now, while C/D
       * (QuickLock/Screenshot) stay deferred since their subsystems
       * (lock screen, UART screenshot) still don't exist.
       * Deliberately NOT `return`ing here: the event still falls through
       * to on_event()/render() below so the current app's own handling
       * (if any) still runs. */
      int16_t vol = (int16_t)Buzzer_GetVolume();
      vol += (kt == KEY_EV_SHORTCUT_A) ? (int16_t)BUZZER_VOLUME_STEP_PCT
                                        : -(int16_t)BUZZER_VOLUME_STEP_PCT;
      if (vol < 0) {
        vol = 0;
      } else if (vol > 100) {
        vol = 100;
      }
      Buzzer_SetVolume((uint8_t)vol);
      LOG("[BUZZER] volume=%u", (unsigned)vol);
    }
  }

  if (s_current->on_event != NULL) {
    s_current->on_event(ev);
  }
  if (s_current->render != NULL) {
    s_current->render();
  }
}

void Phone_Tick(void)
{
  Leds_Tick(); /* needs no per-app cooperation, unlike Widgets_BlinkOn()
                * which each app's own render() calls individually */

  if (s_current == NULL) {
    return;
  }

  if (s_current != &AppLock) {
    /* Auto-lock idle timeout (plan section 5.9 "innovation":
     * settings.autolock_s, 0 = disabled). Checked here, every
     * APP_TASK_TICK_MS, rather than as a separate timer/task. */
    uint16_t autolock_s = Storage_GetSettings()->autolock_s;
    if (autolock_s > 0u &&
        (HAL_GetTick() - s_last_activity_tick) >= ((uint32_t)autolock_s * 1000u)) {
      Phone_Lock();
      return; /* skip the rest of this tick -- avoid double-rendering the
               * just-suspended app */
    }
  }

  if (s_current->on_tick != NULL) {
    s_current->on_tick();
  }
  if (s_current->render != NULL) {
    s_current->render();
  }
}
