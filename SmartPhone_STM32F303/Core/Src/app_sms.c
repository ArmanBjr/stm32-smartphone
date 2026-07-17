/**
 * @file app_sms.c
 * @brief Phase 11: SMS compose+send via PC bridge. Source of truth: master
 *        plan section 5.10. Screens: recipient list (contacts + Manual
 *        number) -> text compose -> sending/result. Emits SMS_SEND|to|text
 *        over UART; awaits EV_SMS_RESULT (or HAL_GetTick timeout).
 */
#include "app.h"
#include "app_config.h"
#include "ui.h"
#include "widgets.h"
#include "keypad.h"
#include "storage.h"
#include "serial.h"
#include "stm32f3xx_hal.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

typedef enum {
  SMS_SCREEN_LIST = 0,
  SMS_SCREEN_MANUAL,
  SMS_SCREEN_TEXT,
  SMS_SCREEN_SENDING,
  SMS_SCREEN_RESULT,
} SmsScreen;

/* Static context (plan section 4.4). */
static SmsScreen s_screen = SMS_SCREEN_LIST;
static uint8_t   s_sel = 0;
static uint8_t   s_scroll_top = 0;

static char s_to[STORAGE_CONTACT_PHONE_LEN];
static char s_text[STORAGE_NOTE_BODY_LEN];
static char s_manual[STORAGE_CONTACT_PHONE_LEN];
static char s_result_detail[48]; /* recId shown on Sent; unused on fail */

static TextField s_tf_manual;
static TextField s_tf_text;
static InputMode s_edit_mode = INPUT_MODE_TYPE;

static uint8_t  s_waiting = 0u;
static uint32_t s_deadline = 0u;
static uint8_t  s_send_ok = 0u;

static void sms_enter_list(void)
{
  s_screen = SMS_SCREEN_LIST;
  s_waiting = 0u;
  Keypad_SetInputMode(INPUT_MODE_NAV);
  uint8_t count = Storage_GetContactCount();
  /* sel: 0 = Manual, 1..count = contacts */
  uint8_t max_sel = count; /* count contacts => indices 1..count; max is count */
  if (s_sel > max_sel) {
    s_sel = 0u;
  }
}

static void sms_enter_manual(void)
{
  memset(s_manual, 0, sizeof(s_manual));
  Widgets_TextFieldBind(&s_tf_manual, s_manual,
                        (uint8_t)(STORAGE_CONTACT_PHONE_LEN - 1u));
  s_edit_mode = INPUT_MODE_TYPE;
  s_screen = SMS_SCREEN_MANUAL;
  Keypad_SetInputMode(s_edit_mode);
}

static void sms_enter_text(void)
{
  memset(s_text, 0, sizeof(s_text));
  Widgets_TextFieldBind(&s_tf_text, s_text,
                        (uint8_t)(STORAGE_NOTE_BODY_LEN - 1u));
  s_edit_mode = INPUT_MODE_TYPE;
  s_screen = SMS_SCREEN_TEXT;
  Keypad_SetInputMode(s_edit_mode);
}

static void sms_do_send(void)
{
  if (s_to[0] == '\0') {
    LOG("[SMS] refuse send: empty To");
    return;
  }
  if (s_text[0] == '\0') {
    LOG("[SMS] refuse send: empty text");
    return;
  }
  LOG("SMS_SEND|%s|%s", s_to, s_text);
  s_waiting = 1u;
  s_deadline = HAL_GetTick() + SMS_SEND_TIMEOUT_MS;
  s_screen = SMS_SCREEN_SENDING;
  Keypad_SetInputMode(INPUT_MODE_NAV);
  LOG("[SMS] sending to %s", s_to);
}

static void sms_finish_result(uint8_t ok, const char *detail)
{
  s_waiting = 0u;
  s_send_ok = ok;
  s_result_detail[0] = '\0';
  if (detail != NULL && detail[0] != '\0') {
    strncpy(s_result_detail, detail, sizeof(s_result_detail) - 1u);
    s_result_detail[sizeof(s_result_detail) - 1u] = '\0';
  }
  s_screen = SMS_SCREEN_RESULT;
  Keypad_SetInputMode(INPUT_MODE_NAV);
}

static void sms_on_enter(void)
{
  if (s_screen == SMS_SCREEN_LIST) {
    Keypad_SetInputMode(INPUT_MODE_NAV);
  } else if (s_screen == SMS_SCREEN_MANUAL) {
    Widgets_TextFieldBind(&s_tf_manual, s_manual,
                          (uint8_t)(STORAGE_CONTACT_PHONE_LEN - 1u));
    Keypad_SetInputMode(s_edit_mode);
  } else if (s_screen == SMS_SCREEN_TEXT) {
    Widgets_TextFieldBind(&s_tf_text, s_text,
                          (uint8_t)(STORAGE_NOTE_BODY_LEN - 1u));
    Keypad_SetInputMode(s_edit_mode);
  } else {
    Keypad_SetInputMode(INPUT_MODE_NAV);
  }
}

static void sms_list_on_key(KeyEventType kt)
{
  uint8_t count = Storage_GetContactCount();
  uint8_t max_sel = count; /* 0=Manual, 1..count=contacts */
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
      if (s_sel == 0u) {
        sms_enter_manual();
      } else {
        const StorageContact *c = Storage_GetContact((uint8_t)(s_sel - 1u));
        if (c == NULL || c->phone[0] == '\0') {
          LOG("[SMS] contact has empty phone");
          break;
        }
        strncpy(s_to, c->phone, sizeof(s_to) - 1u);
        s_to[sizeof(s_to) - 1u] = '\0';
        sms_enter_text();
      }
      break;
    case KEY_EV_LOCKTYPE:
      Keypad_SetInputMode(INPUT_MODE_NAV);
      break;
    default:
      break;
  }
}

static void sms_manual_on_key(KeyEventType kt)
{
  if (s_edit_mode == INPUT_MODE_TYPE) {
    if (kt == KEY_EV_LOCKTYPE) {
      s_edit_mode = Keypad_GetInputMode();
    }
    return;
  }
  switch (kt) {
    case KEY_EV_NAV_LEFT:
      Widgets_TextFieldMoveCursor(&s_tf_manual, -1);
      break;
    case KEY_EV_NAV_RIGHT:
      Widgets_TextFieldMoveCursor(&s_tf_manual, 1);
      break;
    case KEY_EV_DELETE:
      Widgets_TextFieldBackspace(&s_tf_manual);
      break;
    case KEY_EV_SELECT:
      if (s_manual[0] == '\0') {
        LOG("[SMS] refuse: empty manual number");
        break;
      }
      strncpy(s_to, s_manual, sizeof(s_to) - 1u);
      s_to[sizeof(s_to) - 1u] = '\0';
      sms_enter_text();
      break;
    case KEY_EV_LOCKTYPE:
      s_edit_mode = Keypad_GetInputMode();
      break;
    default:
      break;
  }
}

static void sms_text_on_key(KeyEventType kt)
{
  if (s_edit_mode == INPUT_MODE_TYPE) {
    if (kt == KEY_EV_LOCKTYPE) {
      s_edit_mode = Keypad_GetInputMode();
    }
    return;
  }
  switch (kt) {
    case KEY_EV_NAV_LEFT:
      Widgets_TextFieldMoveCursor(&s_tf_text, -1);
      break;
    case KEY_EV_NAV_RIGHT:
      Widgets_TextFieldMoveCursor(&s_tf_text, 1);
      break;
    case KEY_EV_DELETE:
      Widgets_TextFieldBackspace(&s_tf_text);
      break;
    case KEY_EV_SELECT:
      sms_do_send();
      break;
    case KEY_EV_LOCKTYPE:
      s_edit_mode = Keypad_GetInputMode();
      break;
    default:
      break;
  }
}

static void sms_on_event(const Event *ev)
{
  if (ev->type == (uint8_t)EV_SMS_RESULT) {
    if (!s_waiting) {
      LOG("[SMS] late SMS_RESULT ignored");
      return;
    }
    const char *detail = (const char *)(uintptr_t)ev->c;
    if (ev->a == 0u) {
      sms_finish_result(1u, detail);
      LOG("[SMS] sent ok");
    } else {
      sms_finish_result(0u, detail);
      LOG("[SMS] send failed");
    }
    return;
  }

  if (ev->type != (uint8_t)EV_KEY) {
    return;
  }
  KeyEventType kt = (KeyEventType)ev->a;

  if (s_screen == SMS_SCREEN_LIST) {
    sms_list_on_key(kt);
    return;
  }

  if (s_screen == SMS_SCREEN_MANUAL) {
    if (s_edit_mode == INPUT_MODE_TYPE &&
        (kt == KEY_EV_CHAR || kt == KEY_EV_DIGIT)) {
      if (kt == KEY_EV_CHAR) {
        return; /* numeric-only, same as Contact Phone */
      }
      Widgets_TextFieldInsert(&s_tf_manual, (char)ev->b);
      return;
    }
    sms_manual_on_key(kt);
    return;
  }

  if (s_screen == SMS_SCREEN_TEXT) {
    if (s_edit_mode == INPUT_MODE_TYPE &&
        (kt == KEY_EV_CHAR || kt == KEY_EV_DIGIT)) {
      Widgets_TextFieldInsert(&s_tf_text, (char)ev->b);
      return;
    }
    sms_text_on_key(kt);
    return;
  }

  if (s_screen == SMS_SCREEN_RESULT && kt == KEY_EV_SELECT) {
    sms_enter_list();
    return;
  }
}

static void sms_on_tick(void)
{
  if (s_waiting && (HAL_GetTick() >= s_deadline)) {
    LOG("[SMS] send timeout (%u ms)", (unsigned)SMS_SEND_TIMEOUT_MS);
    sms_finish_result(0u, "timeout");
  }
}

static void sms_render_list(void)
{
  UI_BeginFrame();
  uint8_t count = Storage_GetContactCount();
  uint8_t total_rows = (uint8_t)(count + 1u); /* + Manual row */
  Widgets_ListEnsureVisible(&s_scroll_top, s_sel, NOTE_LIST_VISIBLE_ROWS, total_rows);
  uint8_t blink_on = Widgets_BlinkOn();

  for (uint8_t r = 0; r < NOTE_LIST_VISIBLE_ROWS; r++) {
    uint8_t item = (uint8_t)(s_scroll_top + r);
    if (item >= total_rows) {
      continue;
    }
    uint8_t is_sel = (item == s_sel) ? 1u : 0u;
    UI_PutChar(r, 0, (is_sel && blink_on) ? (uint8_t)'>' : (uint8_t)' ');
    if (item == 0u) {
      UI_Print(r, 1, "Manual number");
    } else {
      const StorageContact *c = Storage_GetContact((uint8_t)(item - 1u));
      const char *name = (c != NULL && c->name[0] != '\0') ? c->name : "(unnamed)";
      UI_Print(r, 1, name);
    }
  }
  UI_EndFrame();
}

static void sms_render_field(uint8_t row, const char *label, const TextField *tf)
{
  char line[UI_COLS + 1];
  uint8_t label_len = (uint8_t)strlen(label);
  uint8_t visible_cols = (uint8_t)(UI_COLS - label_len);
  uint8_t offset = Widgets_TextFieldScrollOffset(tf, visible_cols);
  snprintf(line, sizeof(line), "%s%s", label, tf->buf + offset);
  UI_Print(row, 0, line);
  if (Widgets_BlinkOn()) {
    uint8_t cursor_col = (uint8_t)(label_len + (tf->cursor - offset));
    if (cursor_col < UI_COLS) {
      UI_PutChar(row, cursor_col, 0xFFu);
    }
  }
}

static void sms_render_manual(void)
{
  UI_BeginFrame();
  UI_Print(0, 0, "To (digits):");
  sms_render_field(1, "", &s_tf_manual);
  UI_Print(3, 0, (s_edit_mode == INPUT_MODE_TYPE) ? "TYPE  SEL=next" : "NAV   SEL=next");
  UI_EndFrame();
}

static void sms_render_text(void)
{
  UI_BeginFrame();
  char hdr[UI_COLS + 1];
  snprintf(hdr, sizeof(hdr), "To:%s", s_to);
  UI_Print(0, 0, hdr);
  sms_render_field(1, "", &s_tf_text);
  UI_Print(3, 0, (s_edit_mode == INPUT_MODE_TYPE) ? "TYPE  SEL=send" : "NAV   SEL=send");
  UI_EndFrame();
}

static void sms_render_sending(void)
{
  UI_BeginFrame();
  UI_Print(1, 4, "Sending...");
  UI_Print(2, 2, s_to);
  UI_EndFrame();
}

static void sms_render_result(void)
{
  UI_BeginFrame();
  if (s_send_ok) {
    UI_Print(1, 7, "Sent");
    if (s_result_detail[0] != '\0') {
      char line[UI_COLS + 1];
      /* Precision caps recId to the LCD width ("id:" = 3 chars → 17 left). */
      snprintf(line, sizeof(line), "id:%.17s", s_result_detail);
      UI_Print(2, 0, line);
    }
  } else {
    UI_Print(1, 3, "Send failed");
  }
  UI_Print(3, 1, "SEL: new SMS");
  UI_EndFrame();
}

static void sms_render(void)
{
  switch (s_screen) {
    case SMS_SCREEN_LIST:    sms_render_list(); break;
    case SMS_SCREEN_MANUAL:  sms_render_manual(); break;
    case SMS_SCREEN_TEXT:    sms_render_text(); break;
    case SMS_SCREEN_SENDING: sms_render_sending(); break;
    case SMS_SCREEN_RESULT:  sms_render_result(); break;
    default: break;
  }
}

static void sms_on_suspend(void)
{
  /* no-op by design (plan section 4.4) */
}

/** Hierarchical BACK: compose/manual/result return to the recipient list
 *  ("SMS panel"); list itself exits to phone Menu. Sending cancels wait
 *  and returns to list (user can retry). */
static uint8_t sms_on_back(void)
{
  switch (s_screen) {
    case SMS_SCREEN_MANUAL:
    case SMS_SCREEN_TEXT:
    case SMS_SCREEN_RESULT:
      sms_enter_list();
      return 1u;
    case SMS_SCREEN_SENDING:
      s_waiting = 0u;
      sms_enter_list();
      return 1u;
    case SMS_SCREEN_LIST:
    default:
      return 0u;
  }
}

const App AppSms = {
  .on_enter   = sms_on_enter,
  .on_event   = sms_on_event,
  .on_tick    = sms_on_tick,
  .render     = sms_render,
  .on_suspend = sms_on_suspend,
  .on_back    = sms_on_back,
};
