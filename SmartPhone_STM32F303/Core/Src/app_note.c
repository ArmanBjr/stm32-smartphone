/**
 * @file app_note.c
 * @brief Phase 6: Note app. Source of truth: master plan section 3
 *        ("app_note.c -- scrollable note list, +, editor (title/body),
 *        auto-scroll") and section 1.2 ("Note list starts in nav mode,
 *        editor in letter mode").
 *
 *        Two screens, tracked by `s_screen`: LIST (list_view over
 *        storage.h's notes, "+" tail to create a new one, NAV mode, 0=
 *        delete selected) and EDIT (text_field over the selected note's
 *        title/body, starts in TYPE mode per the spec sentence above).
 *
 *        LockType inside EDIT toggles TYPE (append chars via multi-tap/
 *        hold-digit) <-> NAV (2/8 switch field, 4/6 move cursor, 0=
 *        backspace, 5=SELECT commits + requests a flash save + returns to
 *        LIST) -- this reuses the existing global LockType key exactly the
 *        way app_music.c already repurposes it for pot-role inside Music
 *        (master plan section 9.7 precedent), because keypad.h only has
 *        two modes (TYPE/NAV) and NAV mode's KEY_EV_NAV_x/DELETE/SELECT
 *        events are the only way to get cursor-move/backspace/confirm
 *        actions out of the keypad engine (verified in keypad.c:
 *        dispatch_digit_key_release() only emits those under
 *        `s_input_mode == INPUT_MODE_NAV`) -- there is no third mode to
 *        invent this in "for free".
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

typedef enum { NOTE_SCREEN_LIST = 0, NOTE_SCREEN_EDIT } NoteScreen;

/* Static context (plan section 4.4: persists across suspend/resume --
 * on_suspend() is a no-op by design, so leaving mid-edit and coming back
 * via Menu resumes exactly where the user left off). */
static NoteScreen s_screen = NOTE_SCREEN_LIST;
static uint8_t    s_sel = 0;         /* LIST: selection, [0, note_count] (note_count == the "+" tail) */
static uint8_t    s_scroll_top = 0;  /* LIST: list_view scroll window */
static uint8_t    s_edit_idx = 0;    /* EDIT: which note is open */
static uint8_t    s_edit_field = 0;  /* EDIT: 0=title, 1=body */
static InputMode  s_edit_mode = INPUT_MODE_TYPE; /* EDIT: cached TYPE/NAV sub-mode, see file comment */
static TextField  s_tf_title;
static TextField  s_tf_body;

static void note_enter_list(void)
{
  s_screen = NOTE_SCREEN_LIST;
  Keypad_SetInputMode(INPUT_MODE_NAV);
  uint8_t count = Storage_GetNoteCount();
  if (s_sel > count) {
    s_sel = count;
  }
}

static void note_enter_edit(uint8_t idx)
{
  StorageNote *n = Storage_GetNoteMutable(idx);
  if (n == NULL) {
    return;
  }
  s_edit_idx = idx;
  s_edit_field = 0;
  s_edit_mode = INPUT_MODE_TYPE; /* plan section 1.2: "editor in letter mode" */
  Widgets_TextFieldBind(&s_tf_title, n->title, (uint8_t)(STORAGE_NOTE_TITLE_LEN - 1u));
  Widgets_TextFieldBind(&s_tf_body, n->body, (uint8_t)(STORAGE_NOTE_BODY_LEN - 1u));
  s_screen = NOTE_SCREEN_EDIT;
  Keypad_SetInputMode(s_edit_mode);
}

static TextField *note_active_field(void)
{
  return (s_edit_field == 0u) ? &s_tf_title : &s_tf_body;
}

static void note_on_enter(void)
{
  if (s_screen == NOTE_SCREEN_LIST) {
    Keypad_SetInputMode(INPUT_MODE_NAV);
  } else {
    /* Rebind in case this is the very first on_enter after boot loading
     * storage from flash into different addresses than a stale pointer
     * (defensive; s_blob is static so addresses are actually stable, but
     * this keeps len/cursor correct without extra bookkeeping either way). */
    StorageNote *n = Storage_GetNoteMutable(s_edit_idx);
    if (n != NULL) {
      Widgets_TextFieldBind(&s_tf_title, n->title, (uint8_t)(STORAGE_NOTE_TITLE_LEN - 1u));
      Widgets_TextFieldBind(&s_tf_body, n->body, (uint8_t)(STORAGE_NOTE_BODY_LEN - 1u));
    }
    Keypad_SetInputMode(s_edit_mode);
  }
}

static void note_list_on_key(KeyEventType kt)
{
  uint8_t count = Storage_GetNoteCount();
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
        uint8_t idx = Storage_AddNote();
        if (idx == 0xFFu) {
          LOG("[NOTE] list full (%u max)", (unsigned)STORAGE_MAX_NOTES);
        } else {
          note_enter_edit(idx);
        }
      } else {
        note_enter_edit(s_sel);
      }
      break;
    case KEY_EV_DELETE:
      if (s_sel < count) {
        Storage_DeleteNote(s_sel);
        Storage_RequestSave();
        LOG("[NOTE] deleted #%u", (unsigned)s_sel);
        count = Storage_GetNoteCount();
        if (s_sel > count) {
          s_sel = count;
        }
      }
      break;
    case KEY_EV_LOCKTYPE:
      /* LockType has nothing to toggle on the LIST screen (no typing here)
       * -- keypad.c already auto-toggled its global mode to TYPE before
       * this event arrived, which would silently break NAV_UP/DOWN/SELECT
       * on this screen; force it back so LIST always stays navigable. */
      Keypad_SetInputMode(INPUT_MODE_NAV);
      break;
    default:
      break;
  }
}

static void note_edit_on_key(KeyEventType kt)
{
  if (s_edit_mode == INPUT_MODE_TYPE) {
    /* CHAR/DIGIT are the only events the keypad emits from digit keys
     * while in TYPE mode (keypad.c: dispatch_digit_key_release()). */
    if (kt == KEY_EV_CHAR || kt == KEY_EV_DIGIT) {
      /* ev->b (the char) is read by the caller before this is invoked --
       * see note_on_event() below, which passes it separately. */
    }
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
      Widgets_TextFieldMoveCursor(note_active_field(), -1);
      break;
    case KEY_EV_NAV_RIGHT:
      Widgets_TextFieldMoveCursor(note_active_field(), 1);
      break;
    case KEY_EV_DELETE:
      Widgets_TextFieldBackspace(note_active_field());
      break;
    case KEY_EV_SELECT:
      Storage_RequestSave();
      LOG("[NOTE] saved #%u", (unsigned)s_edit_idx);
      note_enter_list();
      break;
    case KEY_EV_LOCKTYPE:
      s_edit_mode = Keypad_GetInputMode();
      break;
    default:
      break;
  }
}

static void note_on_event(const Event *ev)
{
  if (ev->type != (uint8_t)EV_KEY) {
    return;
  }
  KeyEventType kt = (KeyEventType)ev->a;

  if (s_screen == NOTE_SCREEN_LIST) {
    note_list_on_key(kt);
    return;
  }

  /* EDIT screen, TYPE sub-mode text insertion needs the char payload
   * (ev->b), which note_edit_on_key() doesn't receive -- handled here
   * directly instead of threading the char through that function. */
  if (s_edit_mode == INPUT_MODE_TYPE && (kt == KEY_EV_CHAR || kt == KEY_EV_DIGIT)) {
    Widgets_TextFieldInsert(note_active_field(), (char)ev->b);
    return;
  }
  note_edit_on_key(kt);
}

static void note_on_tick(void)
{
  /* nothing time-based beyond the blink read at render time */
}

static void note_render_list(void)
{
  UI_BeginFrame();
  uint8_t count = Storage_GetNoteCount();
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
      UI_Print(r, 1, "+ New Note");
    } else {
      const StorageNote *n = Storage_GetNote(item);
      const char *title = (n != NULL && n->title[0] != '\0') ? n->title : "(untitled)";
      UI_Print(r, 1, title);
    }
  }
  UI_EndFrame();
}

static void note_render_field_row(uint8_t row, const char *label, const TextField *tf, uint8_t is_active)
{
  char line[UI_COLS + 1];
  uint8_t visible_cols = (uint8_t)(UI_COLS - 2u);
  uint8_t offset = Widgets_TextFieldScrollOffset(tf, visible_cols);
  snprintf(line, sizeof(line), "%s%s", label, tf->buf + offset);
  UI_Print(row, 0, line);
  if (is_active && Widgets_BlinkOn()) {
    uint8_t cursor_col = (uint8_t)(2u + (tf->cursor - offset));
    if (cursor_col < UI_COLS) {
      uint8_t ch = (tf->cursor < tf->len) ? (uint8_t)tf->buf[tf->cursor] : (uint8_t)' ';
      (void)ch;
      UI_PutChar(row, cursor_col, 0xFFu); /* block cursor, same glyph app_music.c's progress bar uses */
    }
  }
}

static void note_render_edit(void)
{
  UI_BeginFrame();
  char status[UI_COLS + 1];
  snprintf(status, sizeof(status), "Note %u/%u %s", (unsigned)(s_edit_idx + 1u),
           (unsigned)Storage_GetNoteCount(), (s_edit_mode == INPUT_MODE_TYPE) ? "TYPE" : "NAV");
  UI_Print(0, 0, status);
  note_render_field_row(1, "T:", &s_tf_title, (uint8_t)(s_edit_field == 0u));
  note_render_field_row(3, "B:", &s_tf_body, (uint8_t)(s_edit_field == 1u));
  UI_EndFrame();
}

static void note_render(void)
{
  if (s_screen == NOTE_SCREEN_LIST) {
    note_render_list();
  } else {
    note_render_edit();
  }
}

static void note_on_suspend(void)
{
  /* no-op by design (plan section 4.4) */
}

/** Hierarchical BACK: editor returns to list without saving (SELECT is the
 *  commit path); list exits to phone Menu. */
static uint8_t note_on_back(void)
{
  if (s_screen == NOTE_SCREEN_EDIT) {
    note_enter_list();
    return 1u;
  }
  return 0u;
}

const App AppNote = {
  .on_enter   = note_on_enter,
  .on_event   = note_on_event,
  .on_tick    = note_on_tick,
  .render     = note_render,
  .on_suspend = note_on_suspend,
  .on_back    = note_on_back,
};
