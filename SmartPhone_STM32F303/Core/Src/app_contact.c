/**
 * @file app_contact.c
 * @brief Phase 6: Contact app. Source of truth: master plan section 3
 *        ("app_contact.c -- scrollable contact list, +, editor (name/phone)")
 *        and section 1.2 ("Contact 'Phone' field forces numeric mode; ...
 *        editor in letter mode").
 *
 *        Structurally a near-mirror of app_note.c (LIST/EDIT screens, same
 *        list_view + text_field + LockType-repurposed-as-editor-NAV-toggle
 *        pattern -- see app_note.c's file comment for the full reasoning on
 *        why LockType is reused this way, which applies identically here).
 *        The one real behavioral difference this file adds is the Phone
 *        field's forced-numeric-mode requirement from section 1.2: since
 *        keypad.h only has TYPE (multi-tap CHAR + hold DIGIT) and NAV modes
 *        -- confirmed by reading keypad.c's dispatch_digit_key_release(),
 *        same verification app_note.c's header cites -- there is no
 *        separate "numeric TYPE" mode to switch into. Instead, while the
 *        Phone field has focus in TYPE sub-mode, contact_on_event() below
 *        simply discards KEY_EV_CHAR (multi-tap letters) and only accepts
 *        KEY_EV_DIGIT (hold-digit literal), which is the smallest change
 *        that satisfies "forces numeric mode" without inventing new keypad
 *        engine behavior.
 */
#include "app.h"
#include "app_config.h"
#include "ui.h"
#include "widgets.h"
#include "keypad.h"
#include "storage.h"
#include "serial.h"
#include <string.h>
#include <stdio.h>

typedef enum { CONTACT_SCREEN_LIST = 0, CONTACT_SCREEN_EDIT } ContactScreen;

/* Static context (plan section 4.4: persists across suspend/resume). */
static ContactScreen s_screen = CONTACT_SCREEN_LIST;
static uint8_t    s_sel = 0;         /* LIST: selection, [0, contact_count] ("+" tail) */
static uint8_t    s_scroll_top = 0;  /* LIST: list_view scroll window */
static uint8_t    s_edit_idx = 0;    /* EDIT: which contact is open */
static uint8_t    s_edit_field = 0;  /* EDIT: 0=name, 1=phone */
static InputMode  s_edit_mode = INPUT_MODE_TYPE; /* EDIT: cached TYPE/NAV sub-mode */
static TextField  s_tf_name;
static TextField  s_tf_phone;

static void contact_enter_list(void)
{
  s_screen = CONTACT_SCREEN_LIST;
  Keypad_SetInputMode(INPUT_MODE_NAV);
  uint8_t count = Storage_GetContactCount();
  if (s_sel > count) {
    s_sel = count;
  }
}

static void contact_enter_edit(uint8_t idx)
{
  StorageContact *c = Storage_GetContactMutable(idx);
  if (c == NULL) {
    return;
  }
  s_edit_idx = idx;
  s_edit_field = 0;
  s_edit_mode = INPUT_MODE_TYPE; /* plan section 1.2: "editor in letter mode" */
  Widgets_TextFieldBind(&s_tf_name, c->name, (uint8_t)(STORAGE_CONTACT_NAME_LEN - 1u));
  Widgets_TextFieldBind(&s_tf_phone, c->phone, (uint8_t)(STORAGE_CONTACT_PHONE_LEN - 1u));
  s_screen = CONTACT_SCREEN_EDIT;
  Keypad_SetInputMode(s_edit_mode);
}

static TextField *contact_active_field(void)
{
  return (s_edit_field == 0u) ? &s_tf_name : &s_tf_phone;
}

static void contact_on_enter(void)
{
  if (s_screen == CONTACT_SCREEN_LIST) {
    Keypad_SetInputMode(INPUT_MODE_NAV);
  } else {
    /* Rebind defensively, same rationale as app_note.c's note_on_enter(). */
    StorageContact *c = Storage_GetContactMutable(s_edit_idx);
    if (c != NULL) {
      Widgets_TextFieldBind(&s_tf_name, c->name, (uint8_t)(STORAGE_CONTACT_NAME_LEN - 1u));
      Widgets_TextFieldBind(&s_tf_phone, c->phone, (uint8_t)(STORAGE_CONTACT_PHONE_LEN - 1u));
    }
    Keypad_SetInputMode(s_edit_mode);
  }
}

static void contact_list_on_key(KeyEventType kt)
{
  uint8_t count = Storage_GetContactCount();
  uint8_t max_sel = count; /* the "+" tail */
  switch (kt) {
    case KEY_EV_NAV_UP:
      if (s_sel > 0u) {
        s_sel--;
      }
      break;
    case KEY_EV_NAV_DOWN:
      if (s_sel < max_sel) {
        s_sel++;
      }
      break;
    case KEY_EV_SELECT:
      if (s_sel == count) {
        uint8_t idx = Storage_AddContact();
        if (idx == 0xFFu) {
          LOG("[CONTACT] list full (%u max)", (unsigned)STORAGE_MAX_CONTACTS);
        } else {
          contact_enter_edit(idx);
        }
      } else {
        contact_enter_edit(s_sel);
      }
      break;
    case KEY_EV_DELETE:
      if (s_sel < count) {
        Storage_DeleteContact(s_sel);
        Storage_RequestSave();
        LOG("[CONTACT] deleted #%u", (unsigned)s_sel);
        count = Storage_GetContactCount();
        if (s_sel > count) {
          s_sel = count;
        }
      }
      break;
    case KEY_EV_LOCKTYPE:
      /* Same rationale as app_note.c: LIST has nothing to toggle, force
       * the keypad engine's auto-toggle back to NAV so this screen stays
       * navigable. */
      Keypad_SetInputMode(INPUT_MODE_NAV);
      break;
    default:
      break;
  }
}

static void contact_edit_on_key(KeyEventType kt)
{
  if (s_edit_mode == INPUT_MODE_TYPE) {
    if (kt == KEY_EV_LOCKTYPE) {
      s_edit_mode = Keypad_GetInputMode(); /* keypad.c already flipped it */
    }
    return;
  }

  /* NAV sub-mode. */
  switch (kt) {
    case KEY_EV_NAV_UP:
    case KEY_EV_NAV_DOWN:
      s_edit_field = (uint8_t)(1u - s_edit_field);
      break;
    case KEY_EV_NAV_LEFT:
      Widgets_TextFieldMoveCursor(contact_active_field(), -1);
      break;
    case KEY_EV_NAV_RIGHT:
      Widgets_TextFieldMoveCursor(contact_active_field(), 1);
      break;
    case KEY_EV_DELETE:
      Widgets_TextFieldBackspace(contact_active_field());
      break;
    case KEY_EV_SELECT:
      Storage_RequestSave();
      LOG("[CONTACT] saved #%u", (unsigned)s_edit_idx);
      contact_enter_list();
      break;
    case KEY_EV_LOCKTYPE:
      s_edit_mode = Keypad_GetInputMode();
      break;
    default:
      break;
  }
}

static void contact_on_event(const Event *ev)
{
  if (ev->type != (uint8_t)EV_KEY) {
    return;
  }
  KeyEventType kt = (KeyEventType)ev->a;

  if (s_screen == CONTACT_SCREEN_LIST) {
    contact_list_on_key(kt);
    return;
  }

  /* EDIT screen, TYPE sub-mode text insertion, same char-payload rationale
   * as app_note.c's note_on_event(). The Phone field additionally drops
   * KEY_EV_CHAR here (plan section 1.2's "forces numeric mode" -- see file
   * comment): only KEY_EV_DIGIT (hold-digit literal) is accepted while
   * s_edit_field == 1 (phone). The Name field accepts both, same as Note's
   * fields. */
  if (s_edit_mode == INPUT_MODE_TYPE && (kt == KEY_EV_CHAR || kt == KEY_EV_DIGIT)) {
    uint8_t is_phone = (uint8_t)(s_edit_field == 1u);
    if (is_phone && kt == KEY_EV_CHAR) {
      return; /* multi-tap letters rejected on the numeric-only Phone field */
    }
    Widgets_TextFieldInsert(contact_active_field(), (char)ev->b);
    return;
  }
  contact_edit_on_key(kt);
}

static void contact_on_tick(void)
{
  /* nothing time-based beyond the blink read at render time */
}

static void contact_render_list(void)
{
  UI_BeginFrame();
  uint8_t count = Storage_GetContactCount();
  uint8_t total_rows = (uint8_t)(count + 1u); /* +1 for the "+" tail */
  Widgets_ListEnsureVisible(&s_scroll_top, s_sel, NOTE_LIST_VISIBLE_ROWS, total_rows);
  uint8_t blink_on = Widgets_BlinkOn();

  for (uint8_t r = 0; r < NOTE_LIST_VISIBLE_ROWS; r++) {
    uint8_t item = (uint8_t)(s_scroll_top + r);
    if (item >= total_rows) {
      continue;
    }
    uint8_t is_sel = (item == s_sel) ? 1u : 0u;
    UI_PutChar(r, 0, (is_sel && blink_on) ? (uint8_t)'>' : (uint8_t)' ');
    if (item == count) {
      UI_Print(r, 1, "+ New Contact");
    } else {
      const StorageContact *c = Storage_GetContact(item);
      const char *name = (c != NULL && c->name[0] != '\0') ? c->name : "(unnamed)";
      UI_Print(r, 1, name);
    }
  }
  UI_EndFrame();
}

static void contact_render_field_row(uint8_t row, const char *label, const TextField *tf, uint8_t is_active)
{
  char line[UI_COLS + 1];
  uint8_t visible_cols = (uint8_t)(UI_COLS - 2u);
  uint8_t offset = Widgets_TextFieldScrollOffset(tf, visible_cols);
  snprintf(line, sizeof(line), "%s%s", label, tf->buf + offset);
  UI_Print(row, 0, line);
  if (is_active && Widgets_BlinkOn()) {
    uint8_t cursor_col = (uint8_t)(2u + (tf->cursor - offset));
    if (cursor_col < UI_COLS) {
      UI_PutChar(row, cursor_col, 0xFFu); /* block cursor, same glyph app_music.c's progress bar uses */
    }
  }
}

static void contact_render_edit(void)
{
  UI_BeginFrame();
  char status[UI_COLS + 1];
  snprintf(status, sizeof(status), "Contact %u/%u %s", (unsigned)(s_edit_idx + 1u),
           (unsigned)Storage_GetContactCount(), (s_edit_mode == INPUT_MODE_TYPE) ? "TYPE" : "NAV");
  UI_Print(0, 0, status);
  contact_render_field_row(1, "N:", &s_tf_name, (uint8_t)(s_edit_field == 0u));
  contact_render_field_row(3, "P:", &s_tf_phone, (uint8_t)(s_edit_field == 1u));
  UI_EndFrame();
}

static void contact_render(void)
{
  if (s_screen == CONTACT_SCREEN_LIST) {
    contact_render_list();
  } else {
    contact_render_edit();
  }
}

static void contact_on_suspend(void)
{
  /* no-op by design (plan section 4.4) */
}

/** Hierarchical BACK: editor returns to list without saving (SELECT is the
 *  commit path); list exits to phone Menu. */
static uint8_t contact_on_back(void)
{
  if (s_screen == CONTACT_SCREEN_EDIT) {
    contact_enter_list();
    return 1u;
  }
  return 0u;
}

const App AppContact = {
  .on_enter   = contact_on_enter,
  .on_event   = contact_on_event,
  .on_tick    = contact_on_tick,
  .render     = contact_render,
  .on_suspend = contact_on_suspend,
  .on_back    = contact_on_back,
};
