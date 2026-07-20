/**
 * @file cgram.c
 * @brief Phase 3 CGRAM bank manager. See cgram.h for the request/apply
 *        split rationale (rule 0.2: only Cgram_Apply(), called from
 *        ui_task, may touch the LCD).
 *
 *        Glyph bitmaps below are original placeholder art (5x8, lower 5
 *        bits of each byte, top row first) -- the master plan specifies
 *        *which* named banks must exist and *when* they load, not their
 *        pixel content, so hand-drawn icons here are a legitimate design
 *        choice, not a guess about a spec requirement.
 */
#include "cgram.h"
#include "LiquidCrystal.h"
#include "serial.h"
#include <string.h>

/* BANK_LOGO (Innovation I12): 4 animation frames × (L,R) = 8 glyphs.
 * Progressive "phone body" fill so the boot screen visibly animates. */
static const uint8_t s_logo_glyphs[8][8] = {
  /* frame 0 L/R — outline only */
  { 0x07, 0x08, 0x10, 0x10, 0x10, 0x10, 0x08, 0x07 },
  { 0x1C, 0x02, 0x01, 0x01, 0x01, 0x01, 0x02, 0x1C },
  /* frame 1 L/R — bottom fill */
  { 0x07, 0x08, 0x10, 0x10, 0x10, 0x1F, 0x0F, 0x07 },
  { 0x1C, 0x02, 0x01, 0x01, 0x01, 0x1F, 0x1E, 0x1C },
  /* frame 2 L/R — mid fill */
  { 0x07, 0x08, 0x10, 0x1F, 0x1F, 0x1F, 0x0F, 0x07 },
  { 0x1C, 0x02, 0x01, 0x1F, 0x1F, 0x1F, 0x1E, 0x1C },
  /* frame 3 L/R — full + screen dot */
  { 0x07, 0x0F, 0x1F, 0x1B, 0x1F, 0x1F, 0x0F, 0x07 },
  { 0x1C, 0x1E, 0x1F, 0x1B, 0x1F, 0x1F, 0x1E, 0x1C },
};

/* BANK_MENU_ICONS_DAY: 8 glyphs (Phase 11 added SMS; Phase 9 added Piano --
 * the LAST free CGRAM slot, see cgram.h's CGRAM_ICON_PIANO comment), order
 * matches app_menu.c's s_items[] icon constants (Note, Contact, SMS, Music,
 * PvZ, Settings, Info, Piano). Indices are per-item CGRAM_ICON_* values, not
 * positional slot numbers in the menu list. */
static const uint8_t s_menu_icons_day[8][8] = {
  { 0x1F, 0x11, 0x15, 0x11, 0x15, 0x11, 0x1F, 0x00 }, /* Note: lined paper */
  { 0x0E, 0x0E, 0x04, 0x1F, 0x04, 0x0A, 0x11, 0x00 }, /* Contact: person */
  { 0x02, 0x03, 0x02, 0x02, 0x0E, 0x1E, 0x0C, 0x00 }, /* Music: eighth note */
  { 0x00, 0x0E, 0x1F, 0x1F, 0x0E, 0x04, 0x04, 0x00 }, /* PvZ: leaf */
  { 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x00, 0x00, 0x00 }, /* Settings: gear teeth */
  { 0x0E, 0x11, 0x0C, 0x04, 0x04, 0x04, 0x0E, 0x00 }, /* Info: "i" */
  { 0x1F, 0x11, 0x15, 0x15, 0x11, 0x1F, 0x04, 0x0E }, /* SMS: envelope */
  { 0x1F, 0x15, 0x15, 0x15, 0x15, 0x1F, 0x00, 0x00 }, /* Piano: keyboard bars */
};

/* BANK_MENU_ICONS_NIGHT (Phase 4, plan section 5.4 LDR day/night):
 * same 8 slots/order as s_menu_icons_day, redrawn with a small filled
 * "dot" accent (bit 0x01 in the corner) so the swap is visibly obvious on
 * real hardware, not just a settings-file flag with no user-facing
 * difference -- pixel content is this file's own design choice per this
 * file's header comment, same as the day set. */
static const uint8_t s_menu_icons_night[8][8] = {
  { 0x1F, 0x11, 0x15, 0x11, 0x15, 0x11, 0x1F, 0x01 }, /* Note */
  { 0x0E, 0x0E, 0x04, 0x1F, 0x04, 0x0A, 0x11, 0x01 }, /* Contact */
  { 0x02, 0x03, 0x02, 0x02, 0x0E, 0x1E, 0x0C, 0x01 }, /* Music */
  { 0x00, 0x0E, 0x1F, 0x1F, 0x0E, 0x04, 0x04, 0x01 }, /* PvZ */
  { 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x00, 0x00, 0x01 }, /* Settings */
  { 0x0E, 0x11, 0x0C, 0x04, 0x04, 0x04, 0x0E, 0x01 }, /* Info */
  { 0x1F, 0x11, 0x15, 0x15, 0x11, 0x1F, 0x04, 0x0F }, /* SMS */
  { 0x1F, 0x15, 0x15, 0x15, 0x15, 0x1F, 0x00, 0x01 }, /* Piano */
};

/* BANK_MUSIC (Phase 5 + Innovation I8): volume icons + 5 EQ bar heights. */
static const uint8_t s_music_icons[8][8] = {
  { 0x00, 0x01, 0x03, 0x1F, 0x03, 0x01, 0x00, 0x15 }, /* mute */
  { 0x01, 0x03, 0x07, 0x1F, 0x07, 0x03, 0x01, 0x00 }, /* low */
  { 0x01, 0x03, 0x07, 0x1F, 0x07, 0x03, 0x01, 0x0A }, /* high */
  /* EQ levels 0..4 (slots 3..7): bottom-up fill */
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F },
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x1F },
  { 0x00, 0x00, 0x00, 0x00, 0x1F, 0x1F, 0x1F, 0x1F },
  { 0x00, 0x00, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F },
  { 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F },
};

/* BANK_PVZ (Phase 8): distinct 5x8 glyphs (HD44780 CGRAM). Designed so
 * day/night/ice plants and peas are visually different at a glance --
 * earlier sprouts were nearly identical and peas at 1.5 cell/tick were
 * effectively invisible. 8/8 slots used.
 *
 * Phase 8 hotfix (bug report "i want more specific lcd charachters for
 * zombies and plants"): the previous round of glyphs shared near-identical
 * silhouettes for plant day/night/ice (same peashooter-head outline with
 * only 1-2 pixels changed) and for zombie/tank (same body, only eye pixels
 * differed) -- easy to tell apart reading the bitmap, hard to tell apart on
 * an actual 5x8 dot-matrix cell mid-game. Redrawn so every glyph has a
 * distinct *silhouette*, not just distinct detail pixels: peashooter
 * open-mouth head vs. domed spotted mushroom vs. crowned peashooter;
 * narrow arms-out zombie vs. wide full-block armored tank; round pea vs.
 * taller cross-marked snowflake pea. */
static const uint8_t s_pvz_icons[8][8] = {
  /* 0 plant day -- peashooter: open-mouth head, narrow stem, pot */
  { 0x0C, 0x1E, 0x17, 0x0E, 0x1F, 0x04, 0x0E, 0x1F },
  /* 1 plant night -- domed, spotted mushroom cap on a thick stalk */
  { 0x0E, 0x1F, 0x15, 0x1F, 0x0E, 0x0E, 0x04, 0x1F },
  /* 2 zombie -- narrow head, gap-eyes, arms out, staggered legs */
  { 0x0E, 0x1B, 0x0E, 0x04, 0x1F, 0x04, 0x0A, 0x11 },
  /* 3 heart -- HUD lives */
  { 0x00, 0x0A, 0x1F, 0x1F, 0x0E, 0x04, 0x00, 0x00 },
  /* 4 pea -- round projectile */
  { 0x00, 0x0E, 0x1F, 0x1F, 0x0E, 0x00, 0x00, 0x00 },
  /* 5 tank zombie -- full-width armored helmet/body, wide stance */
  { 0x1F, 0x1F, 0x0A, 0x1F, 0x1F, 0x1F, 0x0A, 0x1B },
  /* 6 ice plant -- peashooter head under an ice-crown */
  { 0x0A, 0x1F, 0x0E, 0x1F, 0x04, 0x04, 0x0E, 0x1F },
  /* 7 ice pea -- snowflake (cross-marked) projectile, taller than pea */
  { 0x00, 0x15, 0x0E, 0x1F, 0x0E, 0x15, 0x00, 0x00 },
};

static volatile CgramBankId s_requested = CGRAM_BANK_NONE;
static CgramBankId s_loaded = CGRAM_BANK_NONE; /* ui_task-owned only */

void Cgram_Init(void)
{
  s_requested = CGRAM_BANK_NONE;
  s_loaded = CGRAM_BANK_NONE;
}

void Cgram_RequestBank(CgramBankId id)
{
  s_requested = id; /* single-word write, safe from app_task */
}

static uint8_t load_bank(CgramBankId id)
{
  switch (id) {
    case CGRAM_BANK_LOGO:
      for (uint8_t i = 0; i < 8; i++) {
        createChar(i, (uint8_t *)s_logo_glyphs[i]);
      }
      return 1U;
    case CGRAM_BANK_MENU_ICONS_DAY:
      for (uint8_t i = 0; i < 8; i++) {
        createChar(i, (uint8_t *)s_menu_icons_day[i]);
      }
      return 1U;
    case CGRAM_BANK_MENU_ICONS_NIGHT:
      for (uint8_t i = 0; i < 8; i++) {
        createChar(i, (uint8_t *)s_menu_icons_night[i]);
      }
      return 1U;
    case CGRAM_BANK_MUSIC:
      for (uint8_t i = 0; i < 8; i++) {
        createChar(i, (uint8_t *)s_music_icons[i]);
      }
      return 1U;
    case CGRAM_BANK_PVZ:
      for (uint8_t i = 0; i < 8; i++) {
        createChar(i, (uint8_t *)s_pvz_icons[i]);
      }
      return 1U;
    case CGRAM_BANK_NONE:
    default:
      return 1U;
  }
}

void Cgram_Apply(void)
{
  CgramBankId want = s_requested; /* snapshot: app_task may change it mid-tick, next Apply() catches it */
  if (want == s_loaded) {
    return;
  }
  if (load_bank(want)) {
    s_loaded = want;
  }
}
