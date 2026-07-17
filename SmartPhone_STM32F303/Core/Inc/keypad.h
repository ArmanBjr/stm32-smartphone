/**
 * @file keypad.h
 * @brief Phase 2: full 4x4 matrix keypad engine -- EXTI + TIM6 1 kHz state
 *        machine (debounce, hold-digit, multi-tap letters, ghost rejection,
 *        TYPE/NAV input modes), emitting high-level KeyEvents onto the
 *        cross-module event queue. Source of truth: master plan section 5.1
 *        ("Keypad engine (keypad.c) -- the trickiest module; follow
 *        exactly") and section 1.2 (logical layout + multi-tap example).
 *        Supersedes the Phase 1 raw-scan slice (Keypad_Process() polling
 *        loop removed; replaced by Keypad_TickISR() driven from TIM6).
 */
#ifndef KEYPAD_H
#define KEYPAD_H

#include <stdint.h>

/** High-level key event kinds carried in Event.a for Event.type==EV_KEY
 *  (plan section 4.2: "EV_KEY (KeyEvent in a/b)"). Event.b carries the
 *  ASCII character for KEY_EV_CHAR / KEY_EV_DIGIT; 0 for everything else.
 *  Set matches plan section 3's keypad.h/.c file-tree entry exactly:
 *  "CHAR, DIGIT, NAV_UP/DOWN/L/R, SELECT, BACK, DELETE, LOCKTYPE,
 *  SHORTCUT_A..D". */
typedef enum {
  KEY_EV_CHAR = 0,
  KEY_EV_DIGIT,
  KEY_EV_NAV_UP,
  KEY_EV_NAV_DOWN,
  KEY_EV_NAV_LEFT,
  KEY_EV_NAV_RIGHT,
  KEY_EV_SELECT,
  KEY_EV_BACK,
  KEY_EV_DELETE,
  KEY_EV_LOCKTYPE,
  KEY_EV_SHORTCUT_A,
  KEY_EV_SHORTCUT_B,
  KEY_EV_SHORTCUT_C,
  KEY_EV_SHORTCUT_D,
} KeyEventType;

/** Global input mode, toggled by the LockType key (plan section 5.1: "LockType
 *  toggles global input_mode (TYPE<->NAV)"). TYPE = multi-tap letters +
 *  hold-digit; NAV = direct 2/4/6/8/5/0 navigation mapping, no multi-tap. */
typedef enum {
  INPUT_MODE_TYPE = 0,
  INPUT_MODE_NAV,
} InputMode;

void Keypad_Init(void);

/** State-machine tick: debounce, scan, hold-timeout, release-debounce,
 *  multi-tap window expiry. Call from the TIM6 1 kHz period-elapsed ISR
 *  ONLY (plan section 5.1: "all timing from TIM6 1 kHz tick") -- never
 *  from task context. */
void Keypad_TickISR(void);

/** Current TYPE/NAV mode, for other modules (e.g. a later LCD mode icon). */
InputMode Keypad_GetInputMode(void);

/** Phase 3 addition: apps set the mode on entry (plan section 1.2: "Contact
 *  'Phone' field forces numeric mode; Note list starts in nav mode, editor
 *  in letter mode" -- this sentence only makes sense if something can set
 *  the mode programmatically, not just the user's LockType key). Not in
 *  section 3's file tree as a named function, but required by section
 *  1.2's own text; keypad.h/.c is the sole owner of input_mode per that
 *  same file tree, so the setter lives here. Safe to call from app_task
 *  (plain state write, no ISR/task race beyond the existing volatile mode
 *  flag already read by Keypad_TickISR()). */
void Keypad_SetInputMode(InputMode mode);

#endif /* KEYPAD_H */
