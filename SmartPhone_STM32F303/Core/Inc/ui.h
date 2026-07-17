/**
 * @file ui.h
 * @brief Phase 3: screen abstraction. Source of truth: master plan section
 *        5.7 ("20x4 shadow buffer + per-cell dirty diffing -> ui_render_dirty()
 *        writes only changed cells") and section 4.3/4.1 ("ONLY module the
 *        LCD task talks to; owns 20x4 shadow framebuffer and diff-based
 *        redraw").
 *
 *        Split for rule 0.2: UI_Print()/UI_PutChar()/UI_Clear()/
 *        UI_BeginFrame()/UI_EndFrame() are called from app_task (apps'
 *        render() callbacks) and only write into a RAM "back" buffer -- no
 *        LCD traffic. UI_RenderDirty() is the only function that touches
 *        the LCD (setCursor+write), so it may ONLY be called from
 *        ui_task's loop.
 *
 *        Diffing implementation note: rather than a separate per-cell
 *        dirty-flag bitmap, this keeps a "shown" buffer (what the LCD
 *        currently displays) alongside the "back" buffer (what apps most
 *        recently wrote) and UI_RenderDirty() writes exactly the cells
 *        where they differ, then copies back->shown for those cells. This
 *        is functionally identical to a flag bitmap -- the plan's
 *        "dirty-flag" language describes *what gets redrawn* (only changed
 *        cells), not a mandated data structure.
 *
 *        Frame batching: UI_Clear()+many Put/Print is not atomic by default
 *        -- ui_task can UI_RenderDirty() between Clear (blank back) and the
 *        content writes, which flashes a blank LCD. Apps that rebuild a
 *        full screen should wrap the rebuild in UI_BeginFrame()/
 *        UI_EndFrame() (clears under the mutex and holds it until End so
 *        RenderDirty cannot observe a half-written frame).
 */
#ifndef UI_H
#define UI_H

#include <stdint.h>

#define UI_COLS 20U
#define UI_ROWS 4U

void UI_Init(void);

/* app_task side: RAM-only, mutex-protected. */
void UI_Clear(void);
void UI_PutChar(uint8_t row, uint8_t col, uint8_t ch);
void UI_Print(uint8_t row, uint8_t col, const char *str);

/** Start an atomic full-screen rebuild: acquires the UI mutex, blanks
 *  s_back, and leaves subsequent Clear/Put/Print unlocked against that
 *  same hold. Must be paired with UI_EndFrame() on every path. Nested
 *  Begin is a no-op (depth counted). */
void UI_BeginFrame(void);
/** Releases the frame lock taken by UI_BeginFrame(). */
void UI_EndFrame(void);

/* ui_task side ONLY: the sole function that writes the physical LCD.
 *
 * Round-5 defense-in-depth (see ui.c UI_Init()'s comment for the boot-loop
 * root cause this pairs with): a full UI_ROWS*UI_COLS dirty redraw is
 * possible any time the app switches screens (not just at boot -- e.g.
 * Menu->Info->Menu), and per LiquidCrystal.c's exact HAL_Delay/pulseEnable
 * timing a full 80-cell redraw can still take up to ~1-2s worst case even
 * though boot itself no longer forces one. That is uncomfortably close to
 * the ~2.0s IWDG window with only a single post-render HAL_IWDG_Refresh().
 * on_row_done, if non-NULL, is invoked once after each row (i.e. up to
 * UI_ROWS times per call, not per cell -- cheap enough to be negligible but
 * frequent enough that a slow full-screen redraw feeds the watchdog several
 * times during its own execution instead of only after). ui.c intentionally
 * has no knowledge of hiwdg (not exposed via main.h -- confirmed by grep),
 * so the caller (main.c's StartDefaultTask) supplies a small wrapper that
 * calls HAL_IWDG_Refresh(&hiwdg). Pass NULL to skip (e.g. from a test
 * harness that doesn't have a watchdog). */
void UI_RenderDirty(void (*on_row_done)(void));

#endif /* UI_H */
