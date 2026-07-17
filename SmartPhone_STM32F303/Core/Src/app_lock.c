/**
 * @file app_lock.c
 * @brief Phase 7: lock screen. Source of truth: master plan section 5.9
 *        ("Lock screen: shows live time (RTC), PIN entry if pin set in
 *        Settings, else any key unlocks. Unlock resumes the exact pre-lock
 *        screen/state.").
 *
 *        phone.c owns *when* AppLock becomes current (Phone_Lock(), called
 *        from cmdparse.c's `/lock`, keypad.c's PB3 EXTI path, and
 *        Phone_Dispatch()'s KEY_EV_SHORTCUT_C/auto-lock paths) and *what to
 *        resume to* (Phone_UnlockToSaved(), remembering the pre-lock app).
 *        This file only owns what's drawn on the lock screen itself and the
 *        PIN/any-key unlock gesture.
 *
 *        PIN entry mode: keypad.h has no dedicated numeric input mode (only
 *        TYPE and NAV -- confirmed by app_contact.c's Phone-field comment,
 *        same verification applies here), so PIN entry reuses TYPE mode's
 *        hold-digit KEY_EV_DIGIT literal, exactly like app_contact.c's Phone
 *        field discards KEY_EV_CHAR and only accepts KEY_EV_DIGIT. LockType
 *        is not needed here (there is no sub-field to toggle) so
 *        KEY_EV_LOCKTYPE is simply ignored if it arrives.
 */
#include "app.h"
#include "app_config.h"
#include "ui.h"
#include "widgets.h"
#include "keypad.h"
#include "storage.h"
#include "rtc_time.h"
#include "phone.h"
#include "serial.h"
#include <string.h>
#include <stdio.h>

#define LOCK_PIN_DIGITS 4u

/* Static context (plan section 4.4). s_pin_buf never persists across a
 * successful/failed attempt -- reset to empty on_enter() and after every
 * wrong-PIN attempt, so a stale partial entry never lingers. */
static char    s_pin_buf[LOCK_PIN_DIGITS + 1u];
static uint8_t s_pin_len;
static uint8_t s_wrong_flash; /* >0: show "Wrong PIN" for a few ticks */

static void lock_reset_entry(void)
{
  s_pin_buf[0] = '\0';
  s_pin_len = 0u;
}

static void lock_on_enter(void)
{
  lock_reset_entry();
  s_wrong_flash = 0u;
  Keypad_SetInputMode(INPUT_MODE_TYPE); /* hold-digit needed for PIN entry;
                                          * harmless no-op for the any-key
                                          * path since any key (CHAR/DIGIT/
                                          * NAV/SELECT/etc.) still reaches
                                          * lock_on_event() below. */
}

static void lock_try_unlock_any_key(void)
{
  const StorageSettings *s = Storage_GetSettings();
  if (!s->pin_enabled) {
    Phone_UnlockToSaved();
  }
}

static void lock_on_event(const Event *ev)
{
  if (ev->type != (uint8_t)EV_KEY) {
    return;
  }
  KeyEventType kt = (KeyEventType)ev->a;
  const StorageSettings *s = Storage_GetSettings();

  if (!s->pin_enabled) {
    /* plan section 5.9: "else any key unlocks". */
    lock_try_unlock_any_key();
    return;
  }

  if (kt == KEY_EV_DIGIT) {
    if (s_wrong_flash > 0u) {
      /* Any key after a wrong-PIN flash clears the message and starts a
       * fresh attempt, rather than silently appending to garbage. */
      s_wrong_flash = 0u;
      lock_reset_entry();
    }
    if (s_pin_len < LOCK_PIN_DIGITS) {
      s_pin_buf[s_pin_len] = (char)ev->b;
      s_pin_len++;
      s_pin_buf[s_pin_len] = '\0';
    }
    if (s_pin_len == LOCK_PIN_DIGITS) {
      if (strcmp(s_pin_buf, s->pin) == 0) {
        LOG("[LOCK] PIN OK");
        lock_reset_entry();
        Phone_UnlockToSaved();
      } else {
        LOG("[LOCK] PIN wrong");
        s_wrong_flash = 1u;
        lock_reset_entry();
      }
    }
    return;
  }

  if (kt == KEY_EV_DELETE && s_pin_len > 0u) {
    s_pin_len--;
    s_pin_buf[s_pin_len] = '\0';
  }
  /* KEY_EV_CHAR (multi-tap letters), NAV/SELECT/BACK/LOCKTYPE and shortcuts
   * are all deliberately ignored while pin_enabled -- BACK is additionally
   * blocked one layer up in phone.c's Phone_Dispatch() so it can't bypass
   * the lock screen at all (plan section 5.9). */
}

static void lock_on_tick(void)
{
  if (s_wrong_flash > 0u && s_wrong_flash < 0xFFu) {
    s_wrong_flash++;
    if (s_wrong_flash > (LOCK_WRONG_PIN_FLASH_TICKS)) {
      s_wrong_flash = 0u;
      lock_reset_entry();
    }
  }
}

static void lock_render(void)
{
  UI_BeginFrame();
  UI_Print(0, 5, "-- Locked --");

  char clock_str[9];
  RTC_Time_GetString(clock_str);
  UI_Print(1, 6, clock_str);
  if (!RTC_Time_IsSynced()) {
    UI_Print(2, 2, "(unsynced -- /time)");
  }

  const StorageSettings *s = Storage_GetSettings();
  if (s->pin_enabled) {
    if (s_wrong_flash > 0u) {
      UI_Print(3, 4, "Wrong PIN");
    } else {
      char dots[LOCK_PIN_DIGITS + 1u];
      uint8_t i;
      for (i = 0; i < s_pin_len; i++) {
        dots[i] = '*';
      }
      dots[i] = '\0';
      char line[UI_COLS + 1];
      snprintf(line, sizeof(line), "PIN: %s", dots);
      UI_Print(3, 2, line);
    }
  } else {
    UI_Print(3, 1, "Press any key...");
  }
  UI_EndFrame();
}

static void lock_on_suspend(void)
{
  /* no-op by design (plan section 4.4) */
}

const App AppLock = {
  .on_enter   = lock_on_enter,
  .on_event   = lock_on_event,
  .on_tick    = lock_on_tick,
  .render     = lock_render,
  .on_suspend = lock_on_suspend,
};
