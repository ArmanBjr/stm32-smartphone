/**
 * @file app_settings.c
 * @brief Phase 7: Settings app. Source of truth: master plan section 5.4
 *        (Settings params: LDR threshold, icon layout, volume, uartset,
 *        autolock, segtime) and section 5.9 (PIN set/enable). Every field
 *        maps 1:1 onto storage.h's `StorageSettings` struct.
 *
 *        Single-screen row editor (no separate LIST/EDIT screens like
 *        app_note.c/app_contact.c -- there is no variable-length collection
 *        here, just a fixed set of 8 rows) -- always NAV mode: UP/DOWN move
 *        the row cursor, LEFT/RIGHT step the selected row's value in place,
 *        SELECT toggles booleans and (PIN row only) opens 4-digit PIN
 *        capture. Every change is applied immediately to the live RAM
 *        settings + Storage_RequestSave() (plan section 5.6: "Settings
 *        change" is a save trigger) -- there is no separate "commit"
 *        gesture, matching how app_contact.c's SELECT-to-save pattern
 *        would be overkill for single-value rows.
 *
 *        PIN capture reuses the same TYPE-mode hold-digit mechanism as
 *        app_lock.c's PIN entry (see that file's header comment for why
 *        there's no dedicated numeric input mode) -- kept independent of
 *        app_lock.c (no shared helper) since the two only share the "4
 *        hold-digits -> pin[]" shape, not any actual behavior (Settings
 *        writes+saves; Lock only compares).
 */
#include "app.h"
#include "app_config.h"
#include "ui.h"
#include "widgets.h"
#include "keypad.h"
#include "storage.h"
#include "buzzer.h"
#include "serial.h"
#include "phone.h" /* Phase 9: Phone_ApplySeg7Default() -- see ROW_SEGTIME below */
#include <string.h>
#include <stdio.h>

#define SETTINGS_PIN_DIGITS 4u /* STORAGE_PIN_LEN - 1 (4 digits + NUL) */

typedef enum {
  ROW_LDR = 0,
  ROW_ICONS,
  ROW_VOLUME,
  ROW_UARTSET,
  ROW_AUTOLOCK,
  ROW_SEGTIME,
  ROW_PIN_ENABLE,
  ROW_PIN_VALUE,
  ROW_COUNT
} SettingsRow;

/* Static context (plan section 4.4). */
static uint8_t s_sel = 0;
static uint8_t s_scroll_top = 0;
static uint8_t s_pin_capturing = 0;
static char    s_pin_buf[SETTINGS_PIN_DIGITS + 1u];
static uint8_t s_pin_len;

static void settings_on_enter(void)
{
  /* Abandon any in-progress PIN capture left over from a BACK-out mid-entry
   * (phone.c's global BACK handler switches straight to Menu without ever
   * routing through settings_on_event() below, so a stale partial capture
   * would otherwise linger and reappear next time Settings is opened --
   * same "never let a stale partial entry linger" rule app_lock.c's header
   * comment documents for its own PIN buffer). */
  s_pin_capturing = 0u;
  Keypad_SetInputMode(INPUT_MODE_NAV);
}

static void settings_start_pin_capture(void)
{
  s_pin_capturing = 1u;
  s_pin_len = 0u;
  s_pin_buf[0] = '\0';
  Keypad_SetInputMode(INPUT_MODE_TYPE);
}

static void settings_row_adjust(SettingsRow row, int16_t delta)
{
  StorageSettings *s = Storage_GetSettingsMutable();
  switch (row) {
    case ROW_LDR: {
      int32_t v = (int32_t)s->ldr_threshold + delta * 64;
      if (v < 0) v = 0;
      if (v > 4095) v = 4095;
      s->ldr_threshold = (uint16_t)v;
      break;
    }
    case ROW_ICONS: {
      int32_t v = (int32_t)s->icon_layout + delta;
      if (v < 0) v = 2;
      if (v > 2) v = 0;
      s->icon_layout = (uint8_t)v;
      break;
    }
    case ROW_VOLUME: {
      int32_t v = (int32_t)s->volume + delta * 5;
      if (v < 0) v = 0;
      if (v > 100) v = 100;
      s->volume = (uint8_t)v;
      Buzzer_SetVolume(s->volume);
      break;
    }
    case ROW_UARTSET:
      s->uart_settings_enable = (uint8_t)(s->uart_settings_enable ? 0u : 1u);
      break;
    case ROW_AUTOLOCK: {
      int32_t v = (int32_t)s->autolock_s + delta * 10;
      if (v < 0) v = 0;
      if (v > 65535) v = 65535;
      s->autolock_s = (uint16_t)v;
      break;
    }
    case ROW_SEGTIME:
      s->segtime_enable = (uint8_t)(s->segtime_enable ? 0u : 1u);
      /* Phase 9: apply immediately -- Settings is never a 7-seg mode-owning
       * app itself (only Music/PvZ are), so this is always safe to call
       * unconditionally here, unlike the AppMusic/AppPvz exit paths in
       * phone.c which only call it while actually leaving that app. */
      Phone_ApplySeg7Default();
      break;
    case ROW_PIN_ENABLE:
      s->pin_enabled = (uint8_t)(s->pin_enabled ? 0u : 1u);
      break;
    default:
      return; /* ROW_PIN_VALUE has no LEFT/RIGHT step -- SELECT-only */
  }
  Storage_RequestSave(); /* plan section 5.6: "Settings change" is a save trigger */
  LOG("[SETTINGS] row=%u changed", (unsigned)row);
}

static void settings_pin_on_digit(char digit)
{
  if (s_pin_len < SETTINGS_PIN_DIGITS) {
    s_pin_buf[s_pin_len] = digit;
    s_pin_len++;
    s_pin_buf[s_pin_len] = '\0';
  }
  if (s_pin_len == SETTINGS_PIN_DIGITS) {
    StorageSettings *s = Storage_GetSettingsMutable();
    memcpy(s->pin, s_pin_buf, sizeof(s->pin));
    Storage_RequestSave();
    LOG("[SETTINGS] PIN updated");
    s_pin_capturing = 0u;
    Keypad_SetInputMode(INPUT_MODE_NAV);
  }
}

static void settings_on_event(const Event *ev)
{
  if (ev->type != (uint8_t)EV_KEY) {
    return;
  }
  KeyEventType kt = (KeyEventType)ev->a;

  if (s_pin_capturing) {
    /* KEY_EV_BACK is consumed by settings_on_back() (phone.c calls App.on_back
     * before exiting to Menu) -- cancels capture and stays in Settings. */
    if (kt == KEY_EV_DIGIT) {
      settings_pin_on_digit((char)ev->b);
    } else if (kt == KEY_EV_DELETE && s_pin_len > 0u) {
      s_pin_len--;
      s_pin_buf[s_pin_len] = '\0';
    }
    return;
  }

  switch (kt) {
    case KEY_EV_NAV_UP:
      if (s_sel > 0u) s_sel--;
      break;
    case KEY_EV_NAV_DOWN:
      if (s_sel < (uint8_t)(ROW_COUNT - 1u)) s_sel++;
      break;
    case KEY_EV_NAV_LEFT:
      settings_row_adjust((SettingsRow)s_sel, -1);
      break;
    case KEY_EV_NAV_RIGHT:
      settings_row_adjust((SettingsRow)s_sel, 1);
      break;
    case KEY_EV_SELECT:
      if (s_sel == (uint8_t)ROW_PIN_VALUE) {
        settings_start_pin_capture();
      } else {
        settings_row_adjust((SettingsRow)s_sel, 1); /* toggles for bool rows */
      }
      break;
    case KEY_EV_LOCKTYPE:
      /* Nothing to toggle here (always NAV outside PIN capture) -- force
       * the keypad engine's auto-toggle back to NAV, same rationale as
       * app_note.c/app_contact.c's LIST screens. */
      Keypad_SetInputMode(INPUT_MODE_NAV);
      break;
    default:
      break;
  }
}

static void settings_on_tick(void)
{
  /* nothing time-based beyond the blink read at render time */
}

static void settings_render_row(uint8_t row, char *line, size_t cap)
{
  const StorageSettings *s = Storage_GetSettings();
  switch ((SettingsRow)row) {
    case ROW_LDR:
      snprintf(line, cap, "LDR thresh: %u", (unsigned)s->ldr_threshold);
      break;
    case ROW_ICONS:
      snprintf(line, cap, "Icon layout: %u", (unsigned)s->icon_layout);
      break;
    case ROW_VOLUME:
      snprintf(line, cap, "Volume: %u%%", (unsigned)s->volume);
      break;
    case ROW_UARTSET:
      snprintf(line, cap, "UART set: %s", s->uart_settings_enable ? "On" : "Off");
      break;
    case ROW_AUTOLOCK:
      snprintf(line, cap, "Autolock: %us", (unsigned)s->autolock_s);
      break;
    case ROW_SEGTIME:
      snprintf(line, cap, "Segtime: %s", s->segtime_enable ? "On" : "Off");
      break;
    case ROW_PIN_ENABLE:
      snprintf(line, cap, "PIN lock: %s", s->pin_enabled ? "On" : "Off");
      break;
    case ROW_PIN_VALUE:
      snprintf(line, cap, "Set PIN: [SELECT]");
      break;
    default:
      line[0] = '\0';
      break;
  }
}

static void settings_render(void)
{
  UI_BeginFrame();

  if (s_pin_capturing) {
    UI_Print(0, 4, "Set new PIN");
    char dots[SETTINGS_PIN_DIGITS + 1u];
    uint8_t i;
    for (i = 0; i < s_pin_len; i++) {
      dots[i] = '*';
    }
    dots[i] = '\0';
    char line[UI_COLS + 1];
    snprintf(line, sizeof(line), "PIN: %s", dots);
    UI_Print(2, 4, line);
    UI_Print(3, 1, "BACK to cancel");
    UI_EndFrame();
    return;
  }

  Widgets_ListEnsureVisible(&s_scroll_top, s_sel, NOTE_LIST_VISIBLE_ROWS, (uint8_t)ROW_COUNT);
  uint8_t blink_on = Widgets_BlinkOn();

  for (uint8_t r = 0; r < NOTE_LIST_VISIBLE_ROWS; r++) {
    uint8_t item = (uint8_t)(s_scroll_top + r);
    if (item >= (uint8_t)ROW_COUNT) {
      continue;
    }
    uint8_t is_sel = (item == s_sel) ? 1u : 0u;
    UI_PutChar(r, 0, (is_sel && blink_on) ? (uint8_t)'>' : (uint8_t)' ');
    char line[UI_COLS + 1];
    settings_render_row(item, line, sizeof(line));
    UI_Print(r, 1, line);
  }
  UI_EndFrame();
}

static void settings_on_suspend(void)
{
  /* no-op by design (plan section 4.4) */
}

/** Hierarchical BACK: cancel in-progress PIN capture (UI says "BACK to
 *  cancel"); settings list itself exits to phone Menu. */
static uint8_t settings_on_back(void)
{
  if (s_pin_capturing) {
    s_pin_capturing = 0u;
    s_pin_len = 0u;
    s_pin_buf[0] = '\0';
    Keypad_SetInputMode(INPUT_MODE_NAV);
    return 1u;
  }
  return 0u;
}

const App AppSettings = {
  .on_enter   = settings_on_enter,
  .on_event   = settings_on_event,
  .on_tick    = settings_on_tick,
  .render     = settings_render,
  .on_suspend = settings_on_suspend,
  .on_back    = settings_on_back,
};
