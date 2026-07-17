/**
 * @file widgets.h
 * @brief Phase 3: reusable UI primitives (plan section 5.7). Populated
 *        incrementally, same policy as app_config.h -- only what the
 *        current phase's apps actually consume. `progress_bar`/`volume_icon`
 *        landed in Phase 5 (app_music.c renders them directly, no shared
 *        helper needed there). `list_view` and `text_field` land here in
 *        Phase 6 for app_note.c/app_contact.c.
 *        Phase 3 (Menu's "blinking selection") only needs `blink_cursor`.
 */
#ifndef WIDGETS_H
#define WIDGETS_H

#include <stdint.h>

/** Returns 1 during the "on" half of a UI_BLINK_MS square wave, 0 during
 *  the "off" half. Plan section 5.7: "blink_cursor (block char toggled by
 *  TIM6-derived 500 ms soft timer -- menu icon blink and text insertion
 *  cursor both use it)". See app_config.h's UI_BLINK_MS comment for why
 *  this reads HAL_GetTick() directly instead of a not-yet-built softtimer
 *  module -- call only from app_task context, never from an ISR. */
uint8_t Widgets_BlinkOn(void);

/** Phase 6 (plan section 5.7: "list_view (scroll window, cursor row, '+'
 *  tail per spec screenshots)"). Pure scroll-window math only -- the app
 *  still owns rendering of each visible row's text and cursor glyph; this
 *  just keeps `*scroll_top` (index of the first visible row) tracking the
 *  selection `sel` so it's always within the visible window
 *  [*scroll_top, *scroll_top + visible_rows). `item_count` is the total
 *  number of selectable rows, INCLUDING any synthetic "+" tail row the
 *  caller appends past its real items (e.g. item_count == note_count + 1).
 *  Idempotent -- safe to call every render regardless of whether sel or
 *  item_count actually changed since the last call. */
void Widgets_ListEnsureVisible(uint8_t *scroll_top, uint8_t sel,
                                uint8_t visible_rows, uint8_t item_count);

/** Phase 6 (plan section 5.7: "text_field (insert/delete at cursor,
 *  auto-scroll when full -- spec: long note bodies scroll and follow the
 *  cursor)"). Operates directly on a caller-owned NUL-terminated buffer of
 *  size `cap+1` (cap usable chars + the NUL) -- storage.h's fixed-size
 *  title/body/name/phone arrays are bound as-is, no separate copy. */
typedef struct {
  char    *buf;
  uint8_t  cap;    /* max chars, excluding the NUL terminator */
  uint8_t  len;    /* current chars; buf[len] == '\0' is always maintained */
  uint8_t  cursor; /* 0..len insertion point */
} TextField;

/** Binds `tf` to `buf` (size `cap+1`, already NUL-terminated by the
 *  caller -- storage.h's arrays are memset to 0 on a fresh note/contact,
 *  which already satisfies this) and derives len/cursor from the buffer's
 *  current contents (cursor starts at end-of-text, natural for "resume
 *  editing where you left off" per plan section 4.4). */
void Widgets_TextFieldBind(TextField *tf, char *buf, uint8_t cap);

/** Inserts `ch` at the cursor, shifting trailing chars right. Returns 0
 *  (no-op) if the field is already at `cap`. */
uint8_t Widgets_TextFieldInsert(TextField *tf, char ch);

/** Deletes the char immediately before the cursor (classic backspace),
 *  shifting trailing chars left. No-op at cursor==0. */
void Widgets_TextFieldBackspace(TextField *tf);

/** Moves the cursor by `delta`, clamped to [0,len]. */
void Widgets_TextFieldMoveCursor(TextField *tf, int8_t delta);

/** Horizontal scroll offset (first visible char index) so the cursor stays
 *  within a `visible_cols`-wide window -- "long note bodies scroll and
 *  follow the cursor" per plan section 5.7. */
uint8_t Widgets_TextFieldScrollOffset(const TextField *tf, uint8_t visible_cols);

#endif /* WIDGETS_H */
