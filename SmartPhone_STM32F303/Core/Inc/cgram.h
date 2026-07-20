/**
 * @file cgram.h
 * @brief Phase 3: CGRAM bank manager. Source of truth: master plan section
 *        5.7 ("CGRAM budget is 8 custom characters TOTAL ... named banks
 *        ... App's on_enter requests its bank; cgram loads lazily. Never
 *        assume a glyph is resident across apps.") and section 4.3's
 *        ui_task loop, which literally lists `cgram_apply()` as one of its
 *        three calls.
 *
 *        Split into request/apply for rule 0.2: Cgram_RequestBank() is
 *        state-only (safe from app_task -- it just records which bank
 *        *should* be resident) and Cgram_Apply() is the only function that
 *        actually issues createChar() HD44780 writes, so it may ONLY be
 *        called from ui_task's loop, matching the plan's own
 *        "ui_render_dirty(); cgram_apply(); IWDG kick" line exactly.
 *
 *        Phase 10 CGRAM audit (screen → bank → slots used / 8):
 *          Boot logo     → BANK_LOGO            → 8 (4 frames × L/R)
 *          Menu (day)    → BANK_MENU_ICONS_DAY  → 8 (Note…Piano)
 *          Menu (night)  → BANK_MENU_ICONS_NIGHT→ 8 (same layout)
 *          Music         → BANK_MUSIC           → 8 (mute/low/high + 5 EQ bars)
 *          PvZ           → BANK_PVZ             → 8 (plants/zombies/…)
 *        Note/Contact/Settings/Info/SMS/Lock/Piano reuse Menu or no CGRAM.
 */
#ifndef CGRAM_H
#define CGRAM_H

#include <stdint.h>

/** Named banks per plan section 5.7. Only the banks actually consumed by
 *  a built phase get a real glyph table (populated incrementally, same
 *  policy as app_config.h) -- the rest are declared here because section
 *  5.7 names them explicitly, but stay unimplemented (NULL bank) until
 *  their owning phase needs them: BANK_MENU_ICONS_NIGHT (Phase 4, LDR
 *  day/night), BANK_MUSIC (Phase 5), BANK_PVZ (Phase 8, now implemented --
 *  see cgram.c). */
typedef enum {
  CGRAM_BANK_NONE = 0,
  CGRAM_BANK_LOGO,
  CGRAM_BANK_MENU_ICONS_DAY,
  CGRAM_BANK_MENU_ICONS_NIGHT,
  CGRAM_BANK_MUSIC,
  CGRAM_BANK_PVZ,
} CgramBankId;

/* Custom-char codes within each bank, for callers that print(...) them
 * (LiquidCrystal's write() treats 0-7 as CGRAM indices). Only valid while
 * the matching bank is actually loaded -- callers must Cgram_RequestBank()
 * the right bank before printing these. */
#define CGRAM_LOGO_L        0U
#define CGRAM_LOGO_R        1U
/* Innovation I12: frames 0..3 use slots (2*f)/(2*f+1). Frame 0 aliases
 * CGRAM_LOGO_L/R above. */
#define CGRAM_LOGO_FRAMES   4U
#define CGRAM_ICON_NOTE     0U
#define CGRAM_ICON_CONTACT  1U
#define CGRAM_ICON_MUSIC    2U
#define CGRAM_ICON_PVZ      3U
#define CGRAM_ICON_SETTINGS 4U
#define CGRAM_ICON_INFO     5U
#define CGRAM_ICON_SMS      6U /* Phase 11 (plan section 5.10): 7th menu glyph */
#define CGRAM_ICON_PIANO    7U /* Phase 9 (plan section 6): 8th and LAST menu
                                 * glyph -- fills the CGRAM 8-glyph ceiling
                                 * exactly, no slots remain after this one. */
/* BANK_MUSIC (Phase 5 + Innovation I8): slots 0-2 volume icons; slots 3-7
 * equalizer bar heights (0..4). Progress bar still uses built-in 0xFF. */
#define CGRAM_ICON_VOL_MUTE 0U
#define CGRAM_ICON_VOL_LOW  1U
#define CGRAM_ICON_VOL_HIGH 2U
#define CGRAM_ICON_EQ_0     3U
#define CGRAM_ICON_EQ_1     4U
#define CGRAM_ICON_EQ_2     5U
#define CGRAM_ICON_EQ_3     6U
#define CGRAM_ICON_EQ_4     7U
#define CGRAM_EQ_LEVELS     5U

/* BANK_PVZ (Phase 8, plan section 5.7: "BANK_PVZ (plant day, plant night,
 * zombie, special, heart, bullet)" and section 5.8 bonus 4's tank
 * zombie/ice plant). All 8 CGRAM slots used -- ice-pea is the free 8th
 * (plan risk register allowed dropping the bullet glyph; we keep both
 * pea and ice-pea instead). Bitmaps designed for 5x8 HD44780 cells
 * (same tool family as https://maxpromer.github.io/LCD-Character-Creator/). */
#define CGRAM_ICON_PLANT_DAY    0U  /* peashooter-like, faces right */
#define CGRAM_ICON_PLANT_NIGHT  1U  /* spotted mushroom */
#define CGRAM_ICON_ZOMBIE       2U
#define CGRAM_ICON_HEART        3U
#define CGRAM_ICON_BULLET       4U  /* day/night pea */
#define CGRAM_ICON_ZOMBIE_TANK  5U
#define CGRAM_ICON_PLANT_ICE    6U  /* snow-pea plant */
#define CGRAM_ICON_BULLET_ICE   7U  /* ice pea projectile */

void Cgram_Init(void);

/** Called from app_task (e.g. an App's on_enter). Records the desired
 *  bank; does NOT touch the LCD. No-op if this bank is already the
 *  requested one. */
void Cgram_RequestBank(CgramBankId id);

/** Called from ui_task ONLY, once per loop iteration. If the requested
 *  bank differs from what's currently loaded in hardware, writes it via
 *  createChar() (8 glyphs max) and updates the loaded-bank marker. */
void Cgram_Apply(void);

#endif /* CGRAM_H */
