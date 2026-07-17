/**
 * @file keypad.c
 * @brief Phase 2 full keypad engine. Source of truth: master plan section
 *        5.1 state machine + section 1.2 logical layout. Row/col wiring
 *        (PE7-10 rows, PE11-14 cols) carried over unchanged from the
 *        Phase 1 / HangmanGame-proven slice.
 *
 *        Multi-tap cycle order note (verified against the plan, not
 *        guessed): section 5.1's shorthand "1->a->b->c->1..." only lists
 *        the *members* of the cycle; section 1.2's worked example --
 *        "press `5` x3 -> `o`" -- is the concrete, testable spec. Key 5's
 *        letters are "mno" (3 letters); landing on 'o' after exactly 3
 *        taps only works if the cycle is LETTERS-FIRST, DIGIT-LAST:
 *        tap1='m', tap2='n', tap3='o', tap4=wraps to '5'. A digit-first
 *        cycle would land tap3 on 'n', contradicting the example. Applied
 *        uniformly below via key_table[]: keys 1-8 cycle 3 letters then
 *        their digit (4 positions), key 9 cycles 2 letters ("yz") then
 *        its digit (3 positions), key 0 has no letters at all (space only,
 *        no multi-tap -- plan section 5.1: "0 = space (TYPE) / delete
 *        (NAV)").
 */
#include "keypad.h"
#include "pinmap.h"
#include "app_config.h"
#include "events.h"
#include "serial.h"
#include "stm32f3xx_hal.h"

static const uint16_t row_pins[4] = {
  PIN_KEY_ROW0, PIN_KEY_ROW1, PIN_KEY_ROW2, PIN_KEY_ROW3
};
static const uint16_t col_pins[4] = {
  PIN_KEY_COL0, PIN_KEY_COL1, PIN_KEY_COL2, PIN_KEY_COL3
};

typedef enum {
  KC_DIGIT = 0, /* digit key, letters[] may be empty (key 0) */
  KC_ACTION,    /* back / LockType / shortcut A-D -- mode-independent */
} KeyClass;

typedef enum {
  NAV_NONE = 0,
  NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT, NAV_SELECT, NAV_DELETE
} NavRole;

typedef enum {
  ACT_NONE = 0,
  ACT_BACK,
  ACT_LOCKTYPE,
  ACT_SHORTCUT_A,
  ACT_SHORTCUT_B,
  ACT_SHORTCUT_C,
  ACT_SHORTCUT_D,
} KeyAction;

typedef struct {
  KeyClass    cls;
  char        digit;        /* '0'-'9' for KC_DIGIT, 0 for KC_ACTION */
  const char *letters;       /* "" if this key has no letters (key 0) */
  uint8_t     letter_count;  /* 3 for keys 1-8, 2 for key 9, 0 for key 0 */
  NavRole     nav;           /* NAV mode direct mapping, NAV_NONE if none */
  KeyAction   action;        /* KC_ACTION identity, ACT_NONE otherwise */
} KeyDef;

/* Physical layout from plan section 1.2 -- do not reorder without
 * re-reading that section; this is the ground truth, not the previous
 * generic phone/hex layout Phase 1 used as a placeholder. */
static const KeyDef key_table[4][4] = {
  /* col0 */                                    /* col1 */                                     /* col2 */                                    /* col3 */
  { {KC_DIGIT, '1', "abc", 3, NAV_NONE,   ACT_NONE},   {KC_DIGIT, '2', "def", 3, NAV_UP,     ACT_NONE},   {KC_DIGIT, '3', "ghi", 3, NAV_NONE,   ACT_NONE},   {KC_ACTION, 0, "", 0, NAV_NONE, ACT_SHORTCUT_A} },
  { {KC_DIGIT, '4', "jkl", 3, NAV_LEFT,   ACT_NONE},   {KC_DIGIT, '5', "mno", 3, NAV_SELECT, ACT_NONE},   {KC_DIGIT, '6', "pqr", 3, NAV_RIGHT,  ACT_NONE},   {KC_ACTION, 0, "", 0, NAV_NONE, ACT_SHORTCUT_B} },
  { {KC_DIGIT, '7', "stu", 3, NAV_NONE,   ACT_NONE},   {KC_DIGIT, '8', "vwx", 3, NAV_DOWN,   ACT_NONE},   {KC_DIGIT, '9', "yz",  2, NAV_NONE,   ACT_NONE},   {KC_ACTION, 0, "", 0, NAV_NONE, ACT_SHORTCUT_C} },
  { {KC_ACTION, 0, "",    0, NAV_NONE,   ACT_BACK},    {KC_DIGIT, '0', "",    0, NAV_DELETE, ACT_NONE},   {KC_ACTION, 0, "",    0, NAV_NONE,   ACT_LOCKTYPE}, {KC_ACTION, 0, "", 0, NAV_NONE, ACT_SHORTCUT_D} },
};

typedef enum {
  KP_IDLE = 0,
  KP_DEBOUNCE, /* EXTI fired, waiting KEYPAD_DEBOUNCE_MS before scanning */
  KP_HELD,     /* unique key resolved, watching for hold-timeout / release */
} KpState;

static volatile KpState kp_state = KP_IDLE;
static volatile uint32_t kp_event_tick;        /* EXTI-fire timestamp (KP_DEBOUNCE) */
static int8_t  kp_row = -1, kp_col = -1;        /* resolved key position */
static uint32_t kp_held_since;                  /* HELD entry timestamp, for HOLD_MS */
static uint32_t kp_release_low_since;           /* 0 = row not currently seen LOW */
static uint8_t  kp_hold_consumed;

/* Multi-tap engine state: the pending, not-yet-committed character. */
static int8_t   mt_row = -1, mt_col = -1;       /* -1,-1 = nothing pending */
static uint8_t  mt_index;
static uint32_t mt_last_tap_tick;

/* volatile: written from EXTI/TIM6 ISR context (LockType key) AND, as of
 * Phase 3, from app_task context (Keypad_SetInputMode()) -- shared across
 * contexts, single-word write is atomic on Cortex-M, volatile just stops
 * the compiler caching a stale copy across the boundary. */
static volatile InputMode s_input_mode = INPUT_MODE_TYPE;

static void columns_all_low(void)
{
  HAL_GPIO_WritePin(PIN_KEY_COL_PORT, PIN_KEY_COL_MASK, GPIO_PIN_RESET);
}

static void columns_all_high(void)
{
  HAL_GPIO_WritePin(PIN_KEY_COL_PORT, PIN_KEY_COL_MASK, GPIO_PIN_SET);
}

/* Plan section 5.1 step 3: "col HIGH, tiny settle (few NOPs), read rows". */
static void settle_delay(void)
{
  for (volatile uint32_t i = 0; i < 20u; i++) {
    __NOP();
  }
}

static void emit_key_event(KeyEventType type, char ch)
{
  Event ev;
  ev.type = (uint8_t)EV_KEY;
  ev.a    = (uint8_t)type;
  ev.b    = (uint16_t)(uint8_t)ch;
  ev.c    = 0;
  (void)Event_Post(&ev);
}

static char multitap_char_at(const KeyDef *kd, uint8_t index)
{
  if (index < kd->letter_count) {
    return kd->letters[index];
  }
  return kd->digit; /* wraps to the key's own digit as the last cycle position */
}

/** Commit whatever multi-tap sequence is pending (emit CHAR), if any.
 *  Plan section 5.1 step 5: "different key or window expiry commits the
 *  pending char (emit CHAR)". */
static void multitap_commit(void)
{
  if (mt_row < 0) {
    return;
  }

  const KeyDef *kd = &key_table[mt_row][mt_col];
  char ch = multitap_char_at(kd, mt_index);

  emit_key_event(KEY_EV_CHAR, ch);
  LOG("[KEY] CHAR '%c'", ch);

  mt_row = -1;
  mt_col = -1;
}

/** Same key retapped within TAP_WINDOW_MS advances the cycle; a different
 *  key (or an expired window, defensively re-checked here) first commits
 *  whatever was pending, then starts a fresh cycle at index 0. */
static void multitap_begin_or_advance(uint8_t row, uint8_t col, uint32_t now)
{
  const KeyDef *kd = &key_table[row][col];

  if (mt_row == (int8_t)row && mt_col == (int8_t)col &&
      (now - mt_last_tap_tick) < KEYPAD_TAP_WINDOW_MS) {
    mt_index = (uint8_t)((mt_index + 1u) % (kd->letter_count + 1u));
  } else {
    multitap_commit();
    mt_row = (int8_t)row;
    mt_col = (int8_t)col;
    mt_index = 0;
  }

  mt_last_tap_tick = now;
  LOG("[KEY] multitap pending '%c' (key %c)", multitap_char_at(kd, mt_index), kd->digit);
}

static void dispatch_action_key(const KeyDef *kd)
{
  /* An action key (back/LockType/shortcut) always ends any in-flight
   * multi-tap sequence -- these are mode-independent per plan section 1.2
   * ("back: always exits current app", "LockType toggles global
   * input_mode"), not part of the letter-cycling flow. */
  multitap_commit();

  switch (kd->action) {
    case ACT_BACK:
      emit_key_event(KEY_EV_BACK, 0);
      LOG("[KEY] BACK");
      break;
    case ACT_LOCKTYPE:
      s_input_mode = (s_input_mode == INPUT_MODE_TYPE) ? INPUT_MODE_NAV : INPUT_MODE_TYPE;
      emit_key_event(KEY_EV_LOCKTYPE, 0);
      LOG("[KEY] LOCKTYPE -> %s", (s_input_mode == INPUT_MODE_TYPE) ? "TYPE" : "NAV");
      break;
    case ACT_SHORTCUT_A:
      emit_key_event(KEY_EV_SHORTCUT_A, 0);
      LOG("[KEY] SHORTCUT_A (Vol+)");
      break;
    case ACT_SHORTCUT_B:
      emit_key_event(KEY_EV_SHORTCUT_B, 0);
      LOG("[KEY] SHORTCUT_B (Vol-)");
      break;
    case ACT_SHORTCUT_C:
      emit_key_event(KEY_EV_SHORTCUT_C, 0);
      LOG("[KEY] SHORTCUT_C (QuickLock)");
      break;
    case ACT_SHORTCUT_D:
      emit_key_event(KEY_EV_SHORTCUT_D, 0);
      LOG("[KEY] SHORTCUT_D (Screenshot)");
      break;
    default:
      break;
  }
}

static void dispatch_digit_key_release(const KeyDef *kd, uint8_t row, uint8_t col, uint32_t now)
{
  if (s_input_mode == INPUT_MODE_NAV) {
    /* Plan section 5.1: "in nav mode, keys map directly ... with no
     * multi-tap." Keys 1/3/7/9 (nav==NAV_NONE) have no nav-mode mapping. */
    multitap_commit();
    switch (kd->nav) {
      case NAV_UP:     emit_key_event(KEY_EV_NAV_UP, 0);    LOG("[KEY] NAV_UP");    break;
      case NAV_DOWN:   emit_key_event(KEY_EV_NAV_DOWN, 0);  LOG("[KEY] NAV_DOWN");  break;
      case NAV_LEFT:   emit_key_event(KEY_EV_NAV_LEFT, 0);  LOG("[KEY] NAV_LEFT");  break;
      case NAV_RIGHT:  emit_key_event(KEY_EV_NAV_RIGHT, 0); LOG("[KEY] NAV_RIGHT"); break;
      case NAV_SELECT: emit_key_event(KEY_EV_SELECT, 0);    LOG("[KEY] SELECT");    break;
      case NAV_DELETE: emit_key_event(KEY_EV_DELETE, 0);    LOG("[KEY] DELETE");    break;
      default:
        LOG("[KEY] nav mode: key %c ignored (no mapping)", kd->digit);
        break;
    }
    return;
  }

  /* TYPE mode. Key 0 has no letters -- plan: "0 = space (TYPE)". Single
   * fixed value, no multi-tap cycling needed. */
  if (kd->letter_count == 0) {
    multitap_commit();
    emit_key_event(KEY_EV_CHAR, ' ');
    LOG("[KEY] CHAR ' ' (space)");
    return;
  }

  multitap_begin_or_advance(row, col, now);
}

static void reset_to_idle(void)
{
  columns_all_high();

  /* Clear EXTI pending flags BEFORE unmasking (plan section 5.1 step 5:
   * "clear EXTI pending flags before unmasking, state=IDLE"). */
  __HAL_GPIO_EXTI_CLEAR_IT(PIN_KEY_ROW_MASK);

  HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

  kp_row = -1;
  kp_col = -1;
  kp_release_low_since = 0;
  kp_hold_consumed = 0;
  kp_state = KP_IDLE;
}

static void finalize_release(const KeyDef *kd)
{
  if (!kp_hold_consumed) {
    if (kd->cls == KC_ACTION) {
      dispatch_action_key(kd);
    } else {
      dispatch_digit_key_release(kd, (uint8_t)kp_row, (uint8_t)kp_col, HAL_GetTick());
    }
  }
  reset_to_idle();
}

/** Plan section 5.1 step 3: drive all columns LOW, then strobe col=0..3
 *  HIGH one at a time, reading all 4 rows each time. Full scan (not an
 *  early-exit on first hit) so ghosting (>=2 simultaneous keys) can be
 *  detected per section 5.1's ghosting policy, not just the "first hit"
 *  shorthand in step 3. */
static void scan_and_resolve(uint32_t now)
{
  int8_t hit_row = -1, hit_col = -1;
  uint8_t hit_count = 0;

  columns_all_low();

  for (uint8_t col = 0; col < 4; col++) {
    HAL_GPIO_WritePin(PIN_KEY_COL_PORT, col_pins[col], GPIO_PIN_SET);
    settle_delay();
    for (uint8_t row = 0; row < 4; row++) {
      if (HAL_GPIO_ReadPin(PIN_KEY_ROW_PORT, row_pins[row]) == GPIO_PIN_SET) {
        hit_row = (int8_t)row;
        hit_col = (int8_t)col;
        hit_count++;
      }
    }
    HAL_GPIO_WritePin(PIN_KEY_COL_PORT, col_pins[col], GPIO_PIN_RESET);
  }

  columns_all_high();

  if (hit_count == 0) {
    /* bounce: nothing settled, re-arm and go back to idle silently */
    reset_to_idle();
    return;
  }

  if (hit_count >= 2) {
    LOG("[KEY] ghost/multi ignored");
    reset_to_idle();
    return;
  }

  kp_row = hit_row;
  kp_col = hit_col;
  kp_held_since = now;
  kp_release_low_since = 0;
  kp_hold_consumed = 0;
  kp_state = KP_HELD;
}

void Keypad_Init(void)
{
  /* GPIO mode/pull already configured by MX_GPIO_Init (Phase 0):
   * rows PE7-10 = EXTI rising, pull-down; cols PE11-14 = output PP. */
  columns_all_high();

  kp_state = KP_IDLE;
  kp_row = -1;
  kp_col = -1;
  kp_event_tick = 0;
  kp_held_since = 0;
  kp_release_low_since = 0;
  kp_hold_consumed = 0;

  mt_row = -1;
  mt_col = -1;
  mt_index = 0;
  mt_last_tap_tick = 0;

  s_input_mode = INPUT_MODE_TYPE;
}

void Keypad_TickISR(void)
{
  const uint32_t now = HAL_GetTick();

  /* Multi-tap window expiry is checked every tick regardless of key
   * state, so a pending letter commits even with no further keypresses. */
  if (mt_row >= 0 && (now - mt_last_tap_tick) >= KEYPAD_TAP_WINDOW_MS) {
    multitap_commit();
  }

  switch (kp_state) {
    case KP_IDLE:
      break;

    case KP_DEBOUNCE:
      if ((now - kp_event_tick) >= KEYPAD_DEBOUNCE_MS) {
        scan_and_resolve(now);
      }
      break;

    case KP_HELD: {
      const KeyDef *kd = &key_table[kp_row][kp_col];

      if (!kp_hold_consumed && s_input_mode == INPUT_MODE_TYPE && kd->cls == KC_DIGIT &&
          (now - kp_held_since) >= KEYPAD_HOLD_MS) {
        multitap_commit();
        emit_key_event(KEY_EV_DIGIT, kd->digit);
        LOG("[KEY] DIGIT '%c' (hold)", kd->digit);
        kp_hold_consumed = 1;
      }

      if (HAL_GPIO_ReadPin(PIN_KEY_ROW_PORT, row_pins[kp_row]) == GPIO_PIN_RESET) {
        if (kp_release_low_since == 0) {
          kp_release_low_since = now;
        } else if ((now - kp_release_low_since) >= KEYPAD_DEBOUNCE_MS) {
          finalize_release(kd);
        }
      } else {
        kp_release_low_since = 0; /* bounced back HIGH, restart release timer */
      }
      break;
    }

    default:
      break;
  }
}

InputMode Keypad_GetInputMode(void)
{
  return s_input_mode;
}

void Keypad_SetInputMode(InputMode mode)
{
  s_input_mode = mode;
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  /* NOTE: this is the single global HAL EXTI dispatch point. PB3 (lock
   * button, EXTI3) and any other EXTI source added in later phases must
   * be dispatched from here too (by GPIO_Pin) -- only one non-weak
   * definition of this callback may exist link-wide. */
  if (GPIO_Pin == PIN_BTN_LOCK) {
    /* Phase 7 (plan section 5.9 lock sources): PB3 quick-lock button.
     * Dispatched BEFORE the kp_state guard below, deliberately -- PB3 is a
     * single button on its own NVIC/EXTI3 line, entirely independent of
     * the row/col matrix debounce state machine, so it must never be
     * dropped just because a keypad press happens to be mid-debounce. No
     * software debounce here (unlike the row pins): plan section 1.1 wires
     * it with a pull-down for a clean rising edge, and a stray extra
     * Phone_Lock() from switch bounce is a harmless no-op (Phone_Lock()
     * itself no-ops if already locked). */
    Event ev;
    ev.type = (uint8_t)EV_BTN_LOCK;
    ev.a = 0u;
    ev.b = 0u;
    ev.c = 0u;
    (void)Event_Post(&ev);
    return;
  }

  if (kp_state != KP_IDLE) {
    return;
  }

  if (GPIO_Pin != PIN_KEY_ROW0 && GPIO_Pin != PIN_KEY_ROW1 &&
      GPIO_Pin != PIN_KEY_ROW2 && GPIO_Pin != PIN_KEY_ROW3) {
    return;
  }

  /* Plan section 5.1 step 2: "mask row EXTIs, timestamp, state=SCAN, do
   * NOT scan in EXTI". Unmasked again in reset_to_idle() once this press
   * cycle resolves (key emitted, bounce, or ghost). PB3/EXTI3 is a
   * separate NVIC line, unaffected by masking these two. */
  HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
  HAL_NVIC_DisableIRQ(EXTI15_10_IRQn);

  kp_event_tick = HAL_GetTick();
  kp_state = KP_DEBOUNCE;
}
