/**
 * @file app_info.c
 * @brief Phase 3: Info app. Source of truth: master plan section 3
 *        ("app_info.c -- team names + student numbers").
 *
 * Team data confirmed by the user (2026-07-16), no longer placeholder.
 *
 * Bugfix (Phase 7 cycle, reported by user): the original 4-row layout
 * reserved row 0 for a "-- Team Info --" header and crammed member 2's
 * name+ID onto a single row 3 ("Arman Bijari 4021262131" = 23 chars,
 * UI_Print() silently truncates at UI_COLS==20 -- confirmed by reading
 * ui.c's UI_Print(), which stops writing at col==UI_COLS with no
 * wraparound/scroll of its own), which cut off the last 3 digits of
 * TEAM_MEMBER_2_ID. Fixed by dropping the header and giving each of the
 * 4 fields (name1/id1/name2/id2) its own full row -- exactly fits UI_ROWS
 * == 4 with no truncation, so no scrolling is needed for this app's fixed,
 * short content (unlike Note/Contact's variable-length user text, which is
 * why list_view/text_field's scrolling lives there instead).
 */
#include "app.h"
#include "ui.h"
#include "keypad.h"

#define TEAM_MEMBER_1_NAME "Amin Hasanzadeh"
#define TEAM_MEMBER_1_ID   "4022262091"
#define TEAM_MEMBER_2_NAME "Arman Bijari"
#define TEAM_MEMBER_2_ID   "4021262131"

static void info_on_enter(void)
{
  Keypad_SetInputMode(INPUT_MODE_NAV); /* only BACK is used, NAV is the
                                         * shell-wide default per phone.c */
}

static void info_on_event(const Event *ev)
{
  (void)ev; /* BACK is handled globally by phone.c; nothing else to do here */
}

static void info_on_tick(void)
{
}

static void info_render(void)
{
  UI_BeginFrame();
  UI_Print(0, 0, TEAM_MEMBER_1_NAME);
  UI_Print(1, 0, TEAM_MEMBER_1_ID);
  UI_Print(2, 0, TEAM_MEMBER_2_NAME);
  UI_Print(3, 0, TEAM_MEMBER_2_ID);
  UI_EndFrame();
}

static void info_on_suspend(void)
{
  /* no-op by design (plan section 4.4) */
}

const App AppInfo = {
  .on_enter   = info_on_enter,
  .on_event   = info_on_event,
  .on_tick    = info_on_tick,
  .render     = info_render,
  .on_suspend = info_on_suspend,
};
