/**
 * @file widgets.c
 * @brief See widgets.h.
 */
#include "widgets.h"
#include "app_config.h"
#include "stm32f3xx_hal.h"
#include <string.h>

uint8_t Widgets_BlinkOn(void)
{
  return (uint8_t)((HAL_GetTick() / UI_BLINK_MS) & 0x1U);
}

void Widgets_ListEnsureVisible(uint8_t *scroll_top, uint8_t sel,
                                uint8_t visible_rows, uint8_t item_count)
{
  if (visible_rows == 0u || item_count == 0u) {
    *scroll_top = 0u;
    return;
  }
  if (sel < *scroll_top) {
    *scroll_top = sel;
  } else if (sel >= (uint8_t)(*scroll_top + visible_rows)) {
    *scroll_top = (uint8_t)(sel - visible_rows + 1u);
  }
  /* Clamp so the window never scrolls past the end of a short list (e.g.
   * after a delete shrinks item_count below the previous scroll_top). */
  if (item_count <= visible_rows) {
    *scroll_top = 0u;
  } else if (*scroll_top > (uint8_t)(item_count - visible_rows)) {
    *scroll_top = (uint8_t)(item_count - visible_rows);
  }
}

void Widgets_TextFieldBind(TextField *tf, char *buf, uint8_t cap)
{
  tf->buf = buf;
  tf->cap = cap;
  tf->len = (uint8_t)strnlen(buf, cap);
  tf->cursor = tf->len;
}

uint8_t Widgets_TextFieldInsert(TextField *tf, char ch)
{
  if (tf->len >= tf->cap) {
    return 0u;
  }
  for (uint8_t i = tf->len; i > tf->cursor; i--) {
    tf->buf[i] = tf->buf[i - 1u];
  }
  tf->buf[tf->cursor] = ch;
  tf->len++;
  tf->cursor++;
  tf->buf[tf->len] = '\0';
  return 1u;
}

void Widgets_TextFieldBackspace(TextField *tf)
{
  if (tf->cursor == 0u) {
    return;
  }
  for (uint8_t i = (uint8_t)(tf->cursor - 1u); i < (uint8_t)(tf->len - 1u); i++) {
    tf->buf[i] = tf->buf[i + 1u];
  }
  tf->len--;
  tf->cursor--;
  tf->buf[tf->len] = '\0';
}

void Widgets_TextFieldMoveCursor(TextField *tf, int8_t delta)
{
  int16_t c = (int16_t)tf->cursor + delta;
  if (c < 0) {
    c = 0;
  } else if (c > (int16_t)tf->len) {
    c = (int16_t)tf->len;
  }
  tf->cursor = (uint8_t)c;
}

uint8_t Widgets_TextFieldScrollOffset(const TextField *tf, uint8_t visible_cols)
{
  if (visible_cols == 0u || tf->len < visible_cols) {
    return 0u;
  }
  if (tf->cursor < visible_cols) {
    return 0u;
  }
  uint8_t max_offset = (uint8_t)(tf->len - visible_cols + 1u);
  uint8_t offset = (uint8_t)(tf->cursor - visible_cols + 1u);
  return (offset > max_offset) ? max_offset : offset;
}
