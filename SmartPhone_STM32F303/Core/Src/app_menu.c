/**
 * @file app_menu.c
 * @brief Phase 3: Menu app. Source of truth: master plan section 3
 *        ("app_menu.c -- icon grid (6x1 / 3x2 / 2x3), blinking selection")
 *        and section 6 exit criterion ("Menu with 3 icon layouts +
 *        blinking selection ... layouts switchable in code").
 *
 *        Phase 6 update: Note and Contact now have built target apps
 *        (app_note.c/app_contact.c + storage.c).
 *
 *        Phase 7 update: Settings now has a built target app (app_settings.c)
 *        too.
 *
 *        Phase 11 update: SMS app added (7th menu icon). Grid layouts
 *        remapped per plan section 5.10 from 6-cell (6x1/3x2/2x3) to
 *        7-capable (7x1/4x2/3x3); Settings icon_layout 0-2 unchanged.
 *
 *        Phase 9 update: Piano app added (8th and last menu icon, plan
 *        section 6). MENU_LAYOUT_7X1 remapped to MENU_LAYOUT_8X1
 *        (app_config.h) since the one-row layout had zero spare cells at
 *        7 items -- see that macro's own comment for the full reasoning.
 *
 *        Post-Phase-7 hardware fix: the grid was still keyed off the
 *        compile-time MENU_LAYOUT macro (Phase 3 exit criterion only ever
 *        asked for "layouts switchable in code"), even though section 5.4
 *        lists icon layout as a runtime Settings param and Phase 7 built a
 *        real Settings row + `/setting-icons-{0-2}` UART command for it --
 *        so changing that row/command had zero visible effect. Fixed by
 *        reading `Storage_GetSettings()->icon_layout` at every point the
 *        grid geometry is needed (menu_move()/menu_render()) instead of the
 *        `#if MENU_LAYOUT` compile-time branch; MENU_LAYOUT itself is kept
 *        only as `storage_zero_init()`'s first-boot default value
 *        (storage.c), not as the runtime source of truth anymore.
 */
#include "app.h"
#include "app_config.h"
#include "ui.h"
#include "cgram.h"
#include "widgets.h"
#include "keypad.h"
#include "phone.h"
#include "serial.h"
#include "analog.h"
#include "storage.h"
#include <string.h>

#define MENU_ITEM_COUNT 8U

/** Runtime grid geometry for the given icon_layout setting value (plan
 *  section 5.10: 0/1/2 = MENU_LAYOUT_8X1/_4X2/_3X3, remapped from 7x1 in
 *  Phase 9 -- see app_config.h's MENU_LAYOUT_8X1 comment). Falls back to
 *  4x2 for any out-of-range value rather than dividing by zero --
 *  defensive only, settings_row_adjust()/cmdparse.c's handle_setting()
 *  already clamp/reject values outside 0-2 before they ever reach storage. */
static void menu_layout_dims(uint8_t *cols, uint8_t *rows)
{
  switch (Storage_GetSettings()->icon_layout) {
    case MENU_LAYOUT_8X1: *cols = 8U; *rows = 1U; break;
    case MENU_LAYOUT_3X3: *cols = 3U; *rows = 3U; break;
    case MENU_LAYOUT_4X2:
    default:              *cols = 4U; *rows = 2U; break;
  }
}

typedef struct {
  const char *label;
  uint8_t     icon;
  const App  *target; /* NULL = not yet built */
} MenuItem;

static const MenuItem s_items[MENU_ITEM_COUNT] = {
  { "Note",     CGRAM_ICON_NOTE,     &AppNote },
  { "Contact",  CGRAM_ICON_CONTACT,  &AppContact },
  { "SMS",      CGRAM_ICON_SMS,      &AppSms },
  { "Music",    CGRAM_ICON_MUSIC,    &AppMusic },
  { "PvZ",      CGRAM_ICON_PVZ,      &AppPvz },
  { "Settings", CGRAM_ICON_SETTINGS, &AppSettings },
  { "Info",     CGRAM_ICON_INFO,     &AppInfo },
  { "Piano",    CGRAM_ICON_PIANO,    &AppPiano },
};

/* Static context (plan section 4.4: persists across suspend/resume,
 * on_suspend() is a no-op, cursor position must survive back-navigation). */
static uint8_t s_sel = 0;

static void menu_request_icon_bank(void)
{
  /* cgram.h: BANK_MENU_ICONS_NIGHT is explicitly Phase-4 scope (unlike
   * BANK_MUSIC, still Phase-5) -- Analog_IsNight() reflects the LDR
   * pipeline's last computed day/night state (analog.c, plan section
   * 5.4). */
  Cgram_RequestBank(Analog_IsNight() ? CGRAM_BANK_MENU_ICONS_NIGHT
                                      : CGRAM_BANK_MENU_ICONS_DAY);
}

static void menu_on_enter(void)
{
  menu_request_icon_bank();
  Keypad_SetInputMode(INPUT_MODE_NAV); /* menu navigation is always NAV mode */
}

static void menu_move(int8_t dcol, int8_t drow)
{
  uint8_t menu_cols, menu_rows;
  menu_layout_dims(&menu_cols, &menu_rows);

  /* Plain `int` throughout, deliberately: mixing uint8_t with a signed
   * int8_t delta forces the usual arithmetic conversions to unsigned,
   * which happens to still wrap correctly here but is exactly the kind of
   * implicit-conversion trap a reviewer would (rightly) flag -- doing it
   * in signed int makes the wraparound obviously correct by inspection
   * instead of by proof. */
  int row = (int)(s_sel / menu_cols);
  int col = (int)(s_sel % menu_cols);
  row = (row + (int)menu_rows + drow) % (int)menu_rows;
  col = (col + (int)menu_cols + dcol) % (int)menu_cols;
  uint8_t idx = (uint8_t)(row * (int)menu_cols + col);
  if (idx < MENU_ITEM_COUNT) {
    s_sel = idx;
  }
}

static void menu_on_event(const Event *ev)
{
  if (ev->type == (uint8_t)EV_ADC_LDR_EDGE) {
    /* Hot-swap the icon bank while Menu is already on screen (not just at
     * on_enter time), so a day/night crossing that happens while the user
     * is sitting in Menu is visibly reflected without needing to
     * back-out/re-enter. */
    menu_request_icon_bank();
    return;
  }
  if (ev->type != (uint8_t)EV_KEY) {
    return;
  }
  KeyEventType kt = (KeyEventType)ev->a;
  switch (kt) {
    case KEY_EV_NAV_LEFT:  menu_move(-1, 0); break;
    case KEY_EV_NAV_RIGHT: menu_move(1, 0);  break;
    case KEY_EV_NAV_UP:    menu_move(0, -1); break;
    case KEY_EV_NAV_DOWN:  menu_move(0, 1);  break;
    case KEY_EV_SELECT:
      if (s_items[s_sel].target != NULL) {
        LOG("[MENU] open %s", s_items[s_sel].label);
        Phone_SwitchApp(s_items[s_sel].target);
      } else {
        LOG("[MENU] %s not yet implemented", s_items[s_sel].label);
      }
      break;
    default:
      break;
  }
}

static void menu_on_tick(void)
{
  /* nothing time-based beyond the blink read at render time */
}

static void menu_render(void)
{
  UI_BeginFrame();
  uint8_t menu_cols, menu_rows;
  menu_layout_dims(&menu_cols, &menu_rows);
  (void)menu_rows; /* only cell width needs cols; row count is implicit in
                     * the loop over MENU_ITEM_COUNT below */
  uint8_t cell_w = (uint8_t)(UI_COLS / menu_cols);
  uint8_t blink_on = Widgets_BlinkOn();

  for (uint8_t i = 0; i < MENU_ITEM_COUNT; i++) {
    uint8_t row = (uint8_t)(i / menu_cols);
    uint8_t col = (uint8_t)(i % menu_cols);
    uint8_t col0 = (uint8_t)(col * cell_w);

    uint8_t is_sel = (i == s_sel) ? 1U : 0U;
    uint8_t cursor_ch = (is_sel && blink_on) ? (uint8_t)'>' : (uint8_t)' ';
    UI_PutChar(row, col0, cursor_ch);

    if (cell_w >= 2U) {
      UI_PutChar(row, (uint8_t)(col0 + 1U), s_items[i].icon);
    }
    if (cell_w > 2U) {
      uint8_t label_w = (uint8_t)(cell_w - 2U);
      uint8_t len = (uint8_t)strlen(s_items[i].label);
      if (len > label_w) {
        len = label_w;
      }
      for (uint8_t k = 0; k < len; k++) {
        UI_PutChar(row, (uint8_t)(col0 + 2U + k), (uint8_t)s_items[i].label[k]);
      }
    }
  }
  UI_EndFrame();
}

static void menu_on_suspend(void)
{
  /* no-op by design (plan section 4.4) */
}

const App AppMenu = {
  .on_enter   = menu_on_enter,
  .on_event   = menu_on_event,
  .on_tick    = menu_on_tick,
  .render     = menu_render,
  .on_suspend = menu_on_suspend,
};
