/**
 * @file phone.h
 * @brief Phase 3: top-level shell. Source of truth: master plan section 3
 *        ("phone.h/.c -- top-level shell: boot logo, menu, lock, app
 *        switching with state preservation, settings store, log tagging")
 *        and section 5.9 ("Boot: idle 'press /start' screen -> /start ->
 *        animated cactus/custom logo + boot jingle -> Menu").
 *
 *        Phase 7 update: both gaps this header used to flag are now
 *        closed. Phone_Init() shows the idle "waiting" screen and does
 *        NOT switch to Menu itself anymore -- Phone_Boot() (called by
 *        cmdparse.c on `/start`) completes the boot sequence. Lock-overlay
 *        logic is wired into Phone_Dispatch()/Phone_Tick() via
 *        Phone_Lock()/Phone_UnlockToSaved(), driven by three sources
 *        (plan section 5.9): the `/lock` UART command (cmdparse.c), the
 *        PB3 quick-lock button (EV_BTN_LOCK, posted by keypad.c's EXTI3
 *        handler) and KEY_EV_SHORTCUT_C, and the auto-lock idle timeout
 *        (Storage's settings.autolock_s, checked in Phone_Tick()).
 */
#ifndef PHONE_H
#define PHONE_H

#include "events.h"
#include "app.h"

/** Shows the idle "waiting for /start" screen (plan section 5.9). Does
 *  NOT switch to Menu -- that only happens once Phone_Boot() runs. Call
 *  once from app_task before entering its event loop. */
void Phone_Init(void);

/** Completes the boot sequence Phone_Init() only started: animated logo
 *  (BOOT_LOGO_MS) then AppMenu. Called by cmdparse.c on `/start`.
 *  Idempotent -- a stray repeated `/start` after boot is a no-op. */
void Phone_Boot(void);

/** Overlays the lock screen (app_lock.c), remembering the current app so
 *  Phone_UnlockToSaved() can resume it exactly (plan section 5.9: "Unlock
 *  resumes the exact pre-lock screen/state"). No-op if not yet booted or
 *  already locked. Also requests a flash save (plan section 5.6: "/lock"
 *  is a save trigger). Called from cmdparse.c (`/lock`), keypad.c's PB3
 *  EXTI path, and Phone_Dispatch()'s KEY_EV_SHORTCUT_C/auto-lock paths. */
void Phone_Lock(void);

/** Switches back to whatever app was current before the matching
 *  Phone_Lock() call (Menu if none). Called by app_lock.c once PIN/any-key
 *  unlock succeeds. */
void Phone_UnlockToSaved(void);

/** Routes one event to the current app, handling shell-global rules no
 *  single app should reimplement: BACK always returns to Menu (plan
 *  section 1.2), lock sources overlay app_lock.c (plan section 5.9).
 *  Re-renders after dispatch. */
void Phone_Dispatch(const Event *ev);

/** Called every APP_TASK_TICK_MS regardless of events, so time-based
 *  widgets (blinking selection, auto-lock idle timeout) can re-evaluate. */
void Phone_Tick(void);

/** Switches the current app. Calls the outgoing app's on_suspend() (no-op
 *  by design, see app.h) then the incoming app's on_enter() and render().
 *  Exposed so app_menu.c can open Info (and, later, other apps) without
 *  phone.c needing to special-case every possible menu item. */
void Phone_SwitchApp(const App *app);

/** Phase 9 (plan section 5.3 bonus 3): re-evaluates the 7-seg "no app
 *  owns the display right now" default -- SEG_TIME if
 *  Storage_GetSettings()->segtime_enable, else SEG_OFF (plan section 5.2's
 *  original default). Called from phone.c itself at the points that used to
 *  hardcode Seg7_SetMode(SEG_OFF) (leaving Music/PvZ, post-boot), and from
 *  app_settings.c right after the user toggles the Segtime row so the
 *  change is visible immediately without waiting for the next app switch. */
void Phone_ApplySeg7Default(void);

/** Innovation I2: 1 while the lock screen is current. */
uint8_t Phone_IsLocked(void);

#endif /* PHONE_H */
