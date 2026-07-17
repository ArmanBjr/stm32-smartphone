/**
 * @file app.h
 * @brief Phase 3: the App interface. Source of truth: master plan section 3
 *        ("app.h -- the App interface: {on_enter, on_event, on_tick,
 *        render, on_suspend}") and section 4.4 ("on_suspend() is a
 *        no-op by design -- state simply persists in the static struct").
 *
 *        Apps (app_menu.c, app_info.c, and later app_note.c etc.) each
 *        define one `const App` instance and own their state as file-static
 *        variables (never touch hardware directly -- go through events.h,
 *        ui.h, cgram.h, keypad.h's Keypad_SetInputMode()). No per-app
 *        header exists in section 3's file tree, so each app's `const App`
 *        instance is declared `extern` here and defined in its own .c.
 */
#ifndef APP_H
#define APP_H

#include "events.h"

typedef struct App {
  void (*on_enter)(void);          /* called every time this app becomes current (not just first time -- state is NOT reset here, only volatile setup like Cgram_RequestBank()/Keypad_SetInputMode()) */
  void (*on_event)(const Event *ev);
  void (*on_tick)(void);           /* called every APP_TASK_TICK_MS, for blink/animation re-checks */
  void (*render)(void);            /* writes the app's current state into ui.h's back buffer */
  void (*on_suspend)(void);        /* no-op by design per section 4.4; present for interface completeness */
  /** Optional hierarchical BACK. Return 1 if this app consumed BACK (e.g.
   *  nested screen -> parent screen within the same app). Return 0 / leave
   *  NULL to let phone.c exit to Menu (plan section 1.2 root exit). */
  uint8_t (*on_back)(void);
} App;

/* Phase 3 apps. */
extern const App AppMenu;
extern const App AppInfo;

/* Phase 5. */
extern const App AppMusic;

/* Phase 6. */
extern const App AppNote;
extern const App AppContact;

/* Phase 7 (plan section 5.9 lock screen, section 5.4/5.9 Settings app). */
extern const App AppLock;
extern const App AppSettings;

/* Phase 8 (plan section 5.8 "PvZ (app_pvz.c)"). */
extern const App AppPvz;

/* Phase 11 (plan section 5.10 SMS via PC bridge). */
extern const App AppSms;

/* Phase 9 (plan section 6 "PC bonuses": piano.py live note streaming). */
extern const App AppPiano;

#endif /* APP_H */
