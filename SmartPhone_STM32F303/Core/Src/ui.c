/**
 * @file ui.c
 * @brief Phase 3 UI framework implementation. See ui.h for the
 *        app_task (write to "back") vs ui_task (diff "back" vs "shown",
 *        flush to hardware) split.
 */
#include "ui.h"
#include "LiquidCrystal.h"
#include "cmsis_os.h"
#include "serial.h"
#include <string.h>
#include <stdio.h>

static uint8_t s_back[UI_ROWS][UI_COLS];
static uint8_t s_shown[UI_ROWS][UI_COLS];
static osMutexId_t s_mutex;

/* >0 while app_task holds the mutex across a BeginFrame..EndFrame rebuild
 * so Put/Print/Clear skip nested Acquire (would self-deadlock) and
 * ui_task's RenderDirty blocks until EndFrame -- no blank-flash race. */
static uint8_t s_frame_depth = 0u;

void UI_Init(void)
{
  memset(s_back, ' ', sizeof(s_back));
  /* Round-5 boot-loop fix (was: memset(s_shown, 0x00, ...) to force a full
   * redraw on the first frame). That original reasoning was correct about
   * *why* ("s_shown must start as something that can never accidentally
   * match a real cell") but wrong about the consequence: LiquidCrystal.c's
   * begin() already ends with clear(), so the physical LCD genuinely *is*
   * all-spaces the moment ui_task starts -- forcing a redraw of all
   * UI_ROWS*UI_COLS=80 cells on frame 1 was therefore both unnecessary and,
   * per exact HAL_Delay/pulseEnable timing (each cell = setCursor()+write()
   * = 2 command/data sends = 4 write4bits() = 4 pulseEnable() = up to
   * ~4*3*HAL_Delay(1) worst-case-rounded ~= 12-24ms/cell), potentially
   * ~1-2s all by itself -- enough to blow the ~2s IWDG window on top of the
   * pre-kernel main() init overhead, before ui_task's single post-render
   * HAL_IWDG_Refresh() ever runs. Confirmed via UART instrumentation
   * (master plan section 9.5, "round 4"): ui_task logs "alive" and enters
   * its first tick, then nothing -- consistent with a legitimately slow
   * (not hung, not faulted) first render exceeding the watchdog deadline.
   * Initializing s_shown to spaces instead matches the LCD's real
   * known-blank post-init state, so frame 1 only redraws cells the boot
   * screen actually populates (~12, not 80) -- correct *and* fast. */
  memset(s_shown, ' ', sizeof(s_shown));
  s_mutex = osMutexNew(NULL);
  s_frame_depth = 0u;
}

static void ui_lock(void)
{
  if (s_frame_depth == 0u) {
    osMutexAcquire(s_mutex, osWaitForever);
  }
}

static void ui_unlock(void)
{
  if (s_frame_depth == 0u) {
    osMutexRelease(s_mutex);
  }
}

void UI_BeginFrame(void)
{
  if (s_frame_depth == 0u) {
    osMutexAcquire(s_mutex, osWaitForever);
    memset(s_back, ' ', sizeof(s_back));
  }
  s_frame_depth++;
}

void UI_EndFrame(void)
{
  if (s_frame_depth == 0u) {
    return; /* unpaired End -- ignore rather than release an unheld mutex */
  }
  s_frame_depth--;
  if (s_frame_depth == 0u) {
    osMutexRelease(s_mutex);
  }
}

void UI_Clear(void)
{
  ui_lock();
  memset(s_back, ' ', sizeof(s_back));
  ui_unlock();
}

void UI_PutChar(uint8_t row, uint8_t col, uint8_t ch)
{
  if (row >= UI_ROWS || col >= UI_COLS) {
    return;
  }
  ui_lock();
  s_back[row][col] = ch;
  ui_unlock();
}

void UI_Print(uint8_t row, uint8_t col, const char *str)
{
  if (row >= UI_ROWS || str == NULL) {
    return;
  }
  ui_lock();
  for (uint8_t c = col; c < UI_COLS && *str != '\0'; c++, str++) {
    s_back[row][c] = (uint8_t)*str;
  }
  ui_unlock();
}

/* Phase 8 hotfix, ROUND 2 (bug report "when a zombie moves for a second
 * something like its shadow is seen on lcd"): the first attempt at this fix
 * (periodic force-every-cell-dirty full 80-cell redraw, ~once/second) was
 * REVERTED after hardware testing showed it caused a severe regression --
 * see master plan section 9.10a addendum. Root cause of the regression:
 * UI_RenderDirty() holds s_mutex for its *entire* redraw loop, and
 * app_task's render() path (UI_BeginFrame/Put/Print/EndFrame) blocks on
 * that same mutex (ui_lock() in this file). A full 80-cell HD44780 redraw
 * costs roughly cells*~12ms (LiquidCrystal.c's pulseEnable() does 3x
 * HAL_Delay(1) per write4bits(), 2x per write()/command()) -- i.e. up to
 * ~1s, once per second, entirely inside the mutex. That stalled app_task's
 * render() for up to ~1s at a time, which stalled app_task's event-drain
 * loop (render() is called synchronously after every dispatched event/tick,
 * per phone.c), which let EV_GAME_TICK (TIM16 @10Hz) pile up faster than
 * app_task could drain the 16-deep eventQueue -- exactly the
 * "[EVT] queue full, dropped type=2" flood the user reported, which in turn
 * starved PvzEngine_Tick() and looked like "plants don't shoot" / "zombies
 * don't die" even though that logic was never actually broken.
 *
 * Reverted to plain dirty-diff (no forced full redraw). The original
 * "shadow" symptom's root cause was never confirmed at the software level
 * (see the now-removed comment's analysis: every render() rebuilds s_back
 * from scratch, so a stale glyph can only come from a single dropped/lost
 * HD44780 write at the hardware level) -- if it recurs, the fix needs to be
 * *cheap* (e.g. one forced cell per render call, spread across many ticks)
 * rather than a single blocking full-screen pass. */
void UI_RenderDirty(void (*on_row_done)(void))
{
  osMutexAcquire(s_mutex, osWaitForever);
  for (uint8_t r = 0; r < UI_ROWS; r++) {
    for (uint8_t c = 0; c < UI_COLS; c++) {
      if (s_back[r][c] != s_shown[r][c]) {
        setCursor(c, r);
        write(s_back[r][c]); /* raw byte write -- correctly handles both
                               * ASCII and CGRAM codes 0-7 (print()'s
                               * null-terminated string form cannot, since
                               * code 0 would be mistaken for a string
                               * terminator). */
        s_shown[r][c] = s_back[r][c];
      }
    }
    /* Round-5 defense-in-depth, see ui.h's UI_RenderDirty() doc comment:
     * feed the watchdog after every row, not just once after the whole
     * loop, so a worst-case full 80-cell redraw can't by itself approach
     * the IWDG window. */
    if (on_row_done != NULL) {
      on_row_done();
    }
  }
  osMutexRelease(s_mutex);
}

void UI_CopyFrame(uint8_t dst[UI_ROWS][UI_COLS])
{
  if (dst == NULL) {
    return;
  }
  ui_lock();
  memcpy(dst, s_back, sizeof(s_back));
  ui_unlock();
}

void UI_DumpShot(void)
{
  uint8_t frame[UI_ROWS][UI_COLS];
  char line[UI_COLS + 1u];
  uint8_t r, c;

  UI_CopyFrame(frame);
  LOG("[SHOT]");
  for (r = 0u; r < UI_ROWS; r++) {
    for (c = 0u; c < UI_COLS; c++) {
      uint8_t ch = frame[r][c];
      if (ch < 8u) {
        ch = (uint8_t)'#'; /* CGRAM glyphs */
      } else if (ch < 0x20u || ch > 0x7Eu) {
        ch = (uint8_t)'.';
      }
      line[c] = (char)ch;
    }
    line[UI_COLS] = '\0';
    LOG("|%s|", line);
  }
  LOG("[/SHOT]");
}
