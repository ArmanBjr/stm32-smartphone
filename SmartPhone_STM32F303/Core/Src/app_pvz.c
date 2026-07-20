/**
 * @file app_pvz.c
 * @brief Phase 8: PvZ app. Source of truth: master plan section 5.8 ("PvZ
 *        (app_pvz.c)") and the user's Persian project-spec excerpt. Owns
 *        screens/input/rendering; pvz_engine.h/.c owns the pure simulation
 *        (grid, zombies, bullets, scoring) -- kept split per this
 *        project's "many small files" convention.
 *
 *        Screen flow: PVZ_SCREEN_MENU (New game/Continue/Settings) ->
 *        PVZ_SCREEN_PLANT_SELECT (pick day/night mix -- PDF p.7 "چهار گیاه"
 *        default, count from Settings pvz_start_plants) -> PVZ_SCREEN_GAME
 *        (live play) -> PVZ_SCREEN_GAME_OVER (stats + high-score check) ->
 *        back to PVZ_SCREEN_MENU. PVZ_SCREEN_SETTINGS is a single-screen row
 *        editor for the 4 pvz_* StorageSettings fields, reusing
 *        app_settings.c's exact row-editor pattern (UP/DOWN move row,
 *        LEFT/RIGHT step value).
 *
 *        PDF p.7 requires game BGM on every PvZ screen (phone music stops);
 *        phone.c snapshots/restores phone melody around AppPvz, and this
 *        file loops PVZ_BGM_MELODY_IDX on EV_SONG_END.
 *
 *        Static context (plan section 4.4: on_suspend() is a no-op, state
 *        persists) is what makes "Continue" work: s_state/s_screen survive
 *        a trip out to Menu/another app and back, same convention
 *        app_music.c's s_shuffle/s_repeat already rely on.
 *
 *        In-field cursor + plant-type selection: NAV mode throughout (no
 *        new InputMode needed, plan section 5.8 doesn't call for one).
 *        UP/DOWN/LEFT/RIGHT move a (row,col) cursor over the 4x17 field;
 *        KEY_EV_LOCKTYPE cycles which inventory plant type (day/night/ice)
 *        SELECT will place -- repurposing LOCKTYPE for an in-app toggle
 *        while staying NAV mode is the same precedent app_music.c's pot
 *        SEEK/VOLUME toggle and app_settings.c's forced-NAV LOCKTYPE
 *        handler already establish; it does not conflict with the global
 *        TYPE<->NAV meaning since this app never needs TYPE mode.
 */
#include "app.h"
#include "app_config.h"
#include "pvz_engine.h"
#include "ui.h"
#include "cgram.h"
#include "widgets.h"
#include "keypad.h"
#include "storage.h"
#include "buzzer.h"
#include "analog.h"
#include "seg7.h"
#include "leds.h"
#include "serial.h"
#include <stdio.h>
#include <string.h>

typedef enum {
  PVZ_SCREEN_MENU = 0,
  PVZ_SCREEN_PLANT_SELECT,
  PVZ_SCREEN_GAME,
  PVZ_SCREEN_GAME_OVER,
  PVZ_SCREEN_SETTINGS,
} PvzScreen;

typedef enum {
  PVZ_MENU_NEW_GAME = 0,
  PVZ_MENU_CONTINUE,
  PVZ_MENU_SETTINGS,
  PVZ_MENU_COUNT
} PvzMenuItem;

typedef enum {
  PVZ_SET_ROW_DAMAGE = 0,
  PVZ_SET_ROW_SPEED,
  PVZ_SET_ROW_DIFFICULTY,
  PVZ_SET_ROW_START_PLANTS,
  PVZ_SET_ROW_COUNT
} PvzSettingsRow;

/* Static context (plan section 4.4): persists across suspend/resume so
 * "Continue" and in-progress Settings edits survive a BACK-out. */
static PvzScreen s_screen = PVZ_SCREEN_MENU;
static PvzState  s_state;
static uint8_t   s_game_active = 0u; /* 1 once a game has been started and not yet game-over'd out */

static uint8_t s_menu_sel = 0u;

static uint8_t s_plant_split = 0u; /* day count; night = start_plants - split */
static uint8_t s_start_plants = PVZ_PLANT_SELECT_COUNT; /* live pick-count for plant-select */

static uint8_t s_cur_row = 0u;
static uint8_t s_cur_col = 0u;
static PvzPlantType s_place_type = PVZ_PLANT_DAY;

static uint8_t s_settings_sel = 0u;

static uint8_t s_game_over_is_new_best = 0u;
static uint8_t s_game_over_new_ach = 0u; /* Innovation I5: newly unlocked bits */

static uint16_t s_last_zombies_spawned = 0u;

static void pvz_ensure_bgm(void)
{
  /* PDF p.7: game BGM while any PvZ screen is showing. Skip while the
   * game-over jingle owns the channel (PVZ_SCREEN_GAME_OVER). */
  if (s_screen == PVZ_SCREEN_GAME_OVER) {
    return;
  }
  if (!Buzzer_IsPlaying() || Buzzer_GetMelodyIndex() != PVZ_BGM_MELODY_IDX) {
    Buzzer_PlayMelody(PVZ_BGM_MELODY_IDX);
  }
}

static void pvz_sync_life_led(void)
{
  /* Pin map / plan section 1.1: green LED = "PvZ life OK". */
  Leds_Set(LED_GREEN, (s_game_active && s_state.lives > 0u) ? 1u : 0u);
}

static void pvz_start_new_game(void)
{
  StorageSettings *sm = Storage_GetSettingsMutable();
  /* Soft-migrate the old arcade-fast default (20) to the new paced default
   * so boards that already saved flash don't stay stuck at 2 cells/s. */
  if (sm->pvz_zombie_speed == 20u) {
    sm->pvz_zombie_speed = PVZ_DEFAULT_ZOMBIE_SPEED;
    Storage_RequestSave();
  }
  if (sm->pvz_difficulty == 30u) {
    sm->pvz_difficulty = PVZ_DEFAULT_DIFFICULTY;
    Storage_RequestSave();
  }
  const StorageSettings *s = sm;
  uint8_t n = s_start_plants;
  if (s_plant_split > n) {
    s_plant_split = n;
  }
  uint8_t night = (uint8_t)(n - s_plant_split);
  PvzEngine_NewGame(&s_state, s_plant_split, night,
                     s->pvz_damage, s->pvz_zombie_speed, s->pvz_difficulty);
  s_game_active = 1u;
  s_cur_row = 0u;
  s_cur_col = 0u;
  s_place_type = (s_plant_split > 0u) ? PVZ_PLANT_DAY
               : (night > 0u) ? PVZ_PLANT_NIGHT
                              : PVZ_PLANT_ICE;
  s_screen = PVZ_SCREEN_GAME;
  Seg7_SetMode(SEG_GAME);
  Seg7_SetGameInfo(s_state.survival_s, (uint8_t)(s_state.score > 99u ? 99u : s_state.score));
  pvz_sync_life_led();
  pvz_ensure_bgm();
  s_last_zombies_spawned = 0u;
  LOG("[PVZ] new game: day=%u night=%u ice=%u dmg=%u spd=%u diff=%u",
      (unsigned)s_plant_split, (unsigned)night, (unsigned)s_state.inventory_ice,
      (unsigned)s_state.damage, (unsigned)s_state.zombie_speed,
      (unsigned)s_state.difficulty);
}

/** Advances s_place_type to the next type that actually has inventory > 0,
 *  wrapping day->night->ice->day. No-op (leaves selection unchanged) if
 *  every type is out of stock -- SELECT will just keep failing silently in
 *  that case, same as any other "no inventory" placement attempt. */
static void pvz_cycle_place_type(void)
{
  for (uint8_t tries = 0; tries < 3u; tries++) {
    s_place_type = (PvzPlantType)((s_place_type == PVZ_PLANT_ICE) ? PVZ_PLANT_DAY : (s_place_type + 1));
    uint8_t have = (s_place_type == PVZ_PLANT_DAY) ? s_state.inventory_day
                   : (s_place_type == PVZ_PLANT_NIGHT) ? s_state.inventory_night
                                                        : s_state.inventory_ice;
    if (have > 0u) {
      break;
    }
  }
}

static void pvz_on_game_over(void)
{
  s_game_active = 0u;
  Leds_Set(LED_GREEN, 0u);
  s_game_over_is_new_best = Storage_SetHighscoreIfBetter(s_state.score,
                                                         s_state.survival_total_s);
  s_game_over_new_ach = Storage_RecordAchievements(s_state.survival_total_s,
                                                   s_state.kills);
  if (s_game_over_is_new_best || s_game_over_new_ach != 0u) {
    Storage_RequestSave(); /* plan section 5.6 save trigger: "new high score" */
  }
  /* Plan section 5.8: "game over jingle" -- multi-note Alert melody, not
   * the single-tone error SFX (that remains the life-loss sad SFX). */
  Buzzer_PlayMelody(PVZ_GAMEOVER_MELODY_IDX);
  s_screen = PVZ_SCREEN_GAME_OVER;
  Seg7_SetMode(SEG_OFF);
  LOG("[PVZ] game over: score=%u survival=%u kills=%u best=%u ach=0x%02X",
      (unsigned)s_state.score, (unsigned)s_state.survival_total_s,
      (unsigned)s_state.kills, (unsigned)s_game_over_is_new_best,
      (unsigned)s_game_over_new_ach);
}

static void pvz_on_enter(void)
{
  Keypad_SetInputMode(INPUT_MODE_NAV);
  Cgram_RequestBank(CGRAM_BANK_PVZ);
  if (s_screen == PVZ_SCREEN_GAME) {
    Seg7_SetMode(SEG_GAME);
    Seg7_SetGameInfo(s_state.survival_s, (uint8_t)(s_state.score > 99u ? 99u : s_state.score));
    pvz_sync_life_led();
  }
  pvz_ensure_bgm();
}

static void pvz_menu_on_key(KeyEventType kt)
{
  switch (kt) {
    case KEY_EV_NAV_UP:
      s_menu_sel = (uint8_t)((s_menu_sel + PVZ_MENU_COUNT - 1u) % PVZ_MENU_COUNT);
      break;
    case KEY_EV_NAV_DOWN:
      s_menu_sel = (uint8_t)((s_menu_sel + 1u) % PVZ_MENU_COUNT);
      break;
    case KEY_EV_SELECT:
      if (s_menu_sel == (uint8_t)PVZ_MENU_NEW_GAME) {
        /* PDF p.8 Settings "تعداد پایه گیاهان" drives the pick count;
         * default/ceiling remains PVZ_PLANT_SELECT_COUNT (PDF p.7 "چهار"). */
        s_start_plants = Storage_GetSettings()->pvz_start_plants;
        if (s_start_plants < 1u) {
          s_start_plants = 1u;
        }
        if (s_start_plants > PVZ_PLANT_SELECT_COUNT) {
          s_start_plants = PVZ_PLANT_SELECT_COUNT;
        }
        s_plant_split = s_start_plants; /* default: all-day, user adjusts */
        s_screen = PVZ_SCREEN_PLANT_SELECT;
      } else if (s_menu_sel == (uint8_t)PVZ_MENU_CONTINUE) {
        if (s_game_active) {
          s_screen = PVZ_SCREEN_GAME;
          Seg7_SetMode(SEG_GAME);
          Seg7_SetGameInfo(s_state.survival_s, (uint8_t)(s_state.score > 99u ? 99u : s_state.score));
          pvz_sync_life_led();
          pvz_ensure_bgm();
        }
        /* PDF p.7: Continue inactive when no current game -- SELECT no-ops. */
      } else {
        s_settings_sel = 0u;
        s_screen = PVZ_SCREEN_SETTINGS;
      }
      break;
    default:
      break;
  }
}

static void pvz_plant_select_on_key(KeyEventType kt)
{
  switch (kt) {
    case KEY_EV_NAV_LEFT:
      if (s_plant_split > 0u) s_plant_split--;
      break;
    case KEY_EV_NAV_RIGHT:
      if (s_plant_split < s_start_plants) s_plant_split++;
      break;
    case KEY_EV_SELECT:
      pvz_start_new_game();
      break;
    default:
      break;
  }
}

static void pvz_game_on_key(KeyEventType kt)
{
  switch (kt) {
    case KEY_EV_NAV_UP:
      if (s_cur_row > 0u) s_cur_row--;
      break;
    case KEY_EV_NAV_DOWN:
      if (s_cur_row < (uint8_t)(PVZ_GRID_ROWS - 1u)) s_cur_row++;
      break;
    case KEY_EV_NAV_LEFT:
      if (s_cur_col > 0u) s_cur_col--;
      break;
    case KEY_EV_NAV_RIGHT:
      if (s_cur_col < (uint8_t)(PVZ_GRID_COLS - 1u)) s_cur_col++;
      break;
    case KEY_EV_LOCKTYPE:
      pvz_cycle_place_type();
      break;
    case KEY_EV_SELECT: {
      uint8_t ok = PvzEngine_PlacePlant(&s_state, s_cur_row, s_cur_col, s_place_type);
      if (ok) {
        Buzzer_PlaySFX(BUZZER_SFX_CLICK);
      }
      break;
    }
    default:
      break;
  }
}

static void pvz_settings_on_key(KeyEventType kt)
{
  StorageSettings *s = Storage_GetSettingsMutable();
  int16_t delta = 0;
  switch (kt) {
    case KEY_EV_NAV_UP:
      if (s_settings_sel > 0u) s_settings_sel--;
      return;
    case KEY_EV_NAV_DOWN:
      if (s_settings_sel < (uint8_t)(PVZ_SET_ROW_COUNT - 1u)) s_settings_sel++;
      return;
    case KEY_EV_NAV_LEFT:
      delta = -1;
      break;
    case KEY_EV_NAV_RIGHT:
    case KEY_EV_SELECT:
      delta = 1;
      break;
    default:
      return;
  }
  switch ((PvzSettingsRow)s_settings_sel) {
    case PVZ_SET_ROW_DAMAGE: {
      int32_t v = (int32_t)s->pvz_damage + delta;
      if (v < 1) v = 1;
      if (v > 9) v = 9;
      s->pvz_damage = (uint8_t)v;
      break;
    }
    case PVZ_SET_ROW_SPEED: {
      int32_t v = (int32_t)s->pvz_zombie_speed + delta;
      if (v < (int32_t)PVZ_ZOMBIE_SPEED_MIN) v = (int32_t)PVZ_ZOMBIE_SPEED_MIN;
      if (v > (int32_t)PVZ_ZOMBIE_SPEED_MAX) v = (int32_t)PVZ_ZOMBIE_SPEED_MAX;
      s->pvz_zombie_speed = (uint8_t)v;
      break;
    }
    case PVZ_SET_ROW_DIFFICULTY: {
      int32_t v = (int32_t)s->pvz_difficulty + delta * 5;
      if (v < 5) v = 5;
      if (v > 100) v = 100;
      s->pvz_difficulty = (uint8_t)v;
      break;
    }
    case PVZ_SET_ROW_START_PLANTS: {
      int32_t v = (int32_t)s->pvz_start_plants + delta;
      if (v < 1) v = 1;
      if (v > (int32_t)PVZ_PLANT_SELECT_COUNT) v = PVZ_PLANT_SELECT_COUNT;
      s->pvz_start_plants = (uint8_t)v;
      break;
    }
    default:
      return;
  }
  Storage_RequestSave(); /* plan section 5.6: "Settings change" is a save trigger */
}

static void pvz_on_event(const Event *ev)
{
  if (ev->type == (uint8_t)EV_SONG_END) {
    /* Loop game BGM while in PvZ; stay silent after the game-over jingle. */
    if (s_screen != PVZ_SCREEN_GAME_OVER) {
      Buzzer_PlayMelody(PVZ_BGM_MELODY_IDX);
    }
    return;
  }

  if (ev->type == (uint8_t)EV_GAME_TICK) {
    if (s_screen != PVZ_SCREEN_GAME) {
      return; /* simulation only advances while the live game screen is showing */
    }
    PvzTickResult r = PvzEngine_Tick(&s_state, Analog_IsNight());
    if (s_state.zombies_spawned != s_last_zombies_spawned) {
      LOG("[PVZ] zombie spawn #%u", (unsigned)s_state.zombies_spawned);
      s_last_zombies_spawned = s_state.zombies_spawned;
    }
    /* Prefer kill SFX over hit when both happened this tick (one channel). */
    if (s_state.fx_kills > 0u) {
      Buzzer_PlaySFX(BUZZER_SFX_KILL);
    } else if (s_state.fx_hits > 0u) {
      Buzzer_PlaySFX(BUZZER_SFX_HIT);
    }
    Seg7_SetGameInfo(s_state.survival_s, (uint8_t)(s_state.score > 99u ? 99u : s_state.score));
    if (r == PVZ_TICK_LIFE_LOST) {
      /* Plan section 5.8: red LED blink on life loss (including the fatal
       * breach -- engine returns LIFE_LOST with game_over set). Sad SFX for
       * non-fatal; fatal goes straight to the game-over jingle so the
       * single-tone error does not get overwritten mid-note. */
      Leds_Blink(LED_RED, 1u);
      pvz_sync_life_led();
      if (s_state.game_over) {
        pvz_on_game_over();
      } else {
        Buzzer_PlaySFX(BUZZER_SFX_ERROR);
      }
    } else if (r == PVZ_TICK_GAME_OVER) {
      pvz_on_game_over();
    }
    return;
  }

  if (ev->type == (uint8_t)EV_TICK_1S) {
    if (s_screen == PVZ_SCREEN_GAME && s_game_active) {
      PvzEngine_OnSecondTick(&s_state, Analog_IsNight());
      Seg7_SetGameInfo(s_state.survival_s, (uint8_t)(s_state.score > 99u ? 99u : s_state.score));
    }
    return;
  }

  if (ev->type != (uint8_t)EV_KEY) {
    return;
  }
  KeyEventType kt = (KeyEventType)ev->a;

  switch (s_screen) {
    case PVZ_SCREEN_MENU:          pvz_menu_on_key(kt); break;
    case PVZ_SCREEN_PLANT_SELECT:  pvz_plant_select_on_key(kt); break;
    case PVZ_SCREEN_GAME:          pvz_game_on_key(kt); break;
    case PVZ_SCREEN_SETTINGS:      pvz_settings_on_key(kt); break;
    case PVZ_SCREEN_GAME_OVER:
      if (kt == KEY_EV_SELECT) {
        s_screen = PVZ_SCREEN_MENU;
        Seg7_SetMode(SEG_OFF);
        pvz_ensure_bgm();
      }
      break;
    default:
      break;
  }
}

static void pvz_on_tick(void)
{
  /* nothing time-based beyond the blink read at render time and the
   * EV_GAME_TICK/EV_TICK_1S-driven simulation above */
}

static void pvz_render_menu(void)
{
  UI_BeginFrame();
  UI_Print(0, 4, "== PvZ ==");
  const char *labels[PVZ_MENU_COUNT] = { "New game", "Continue", "Settings" };
  uint8_t blink_on = Widgets_BlinkOn();
  for (uint8_t i = 0; i < PVZ_MENU_COUNT; i++) {
    uint8_t is_sel = (i == s_menu_sel) ? 1u : 0u;
    UI_PutChar((uint8_t)(1u + i), 2, (is_sel && blink_on) ? (uint8_t)'>' : (uint8_t)' ');
    UI_Print((uint8_t)(1u + i), 4, labels[i]);
    if (i == (uint8_t)PVZ_MENU_CONTINUE && !s_game_active) {
      UI_Print((uint8_t)(1u + i), 14, "(none)");
    }
  }
  UI_EndFrame();
}

static void pvz_render_plant_select(void)
{
  UI_BeginFrame();
  UI_Print(0, 1, "Pick your plants");
  char line[UI_COLS + 1];
  uint8_t night = (uint8_t)(s_start_plants - s_plant_split);
  /* Show the actual plant glyphs so the player learns day vs night shapes
   * before the match (CGRAM_BANK_PVZ already requested in on_enter). */
  UI_PutChar(1, 2, (uint8_t)CGRAM_ICON_PLANT_DAY);
  snprintf(line, sizeof(line), " Day:%u", (unsigned)s_plant_split);
  UI_Print(1, 3, line);
  UI_PutChar(1, 11, (uint8_t)CGRAM_ICON_PLANT_NIGHT);
  snprintf(line, sizeof(line), " Night:%u", (unsigned)night);
  UI_Print(1, 12, line);
  UI_PutChar(2, 2, (uint8_t)CGRAM_ICON_PLANT_ICE);
  UI_Print(2, 4, "+1 ice on start");
  snprintf(line, sizeof(line), "Total %u  L/R adjust", (unsigned)s_start_plants);
  UI_Print(3, 1, line);
  UI_EndFrame();
}

static uint8_t pvz_field_col_from_x(int16_t x)
{
  if (x < 0) {
    return 0u;
  }
  uint8_t col = (uint8_t)(x / 100);
  if (col >= PVZ_GRID_COLS) {
    col = (uint8_t)(PVZ_GRID_COLS - 1u);
  }
  return col;
}

static uint8_t pvz_cell_glyph(uint8_t row, uint8_t col, uint8_t blink_on)
{
  /* Zombies/bullets draw over plants (front-most first). */
  for (uint8_t i = 0; i < PVZ_MAX_ZOMBIES; i++) {
    const PvzZombie *z = &s_state.zombies[i];
    if (z->active && z->row == row && pvz_field_col_from_x(z->x) == col) {
      /* Slowed zombies blink to '*' so the ice effect is visible without
       * spending another CGRAM slot. */
      if (z->slow_ticks_left > 0u && !blink_on) {
        return (uint8_t)'*';
      }
      return (z->type == PVZ_ZOMBIE_TANK) ? (uint8_t)CGRAM_ICON_ZOMBIE_TANK
                                           : (uint8_t)CGRAM_ICON_ZOMBIE;
    }
  }
  for (uint8_t i = 0; i < PVZ_MAX_BULLETS; i++) {
    const PvzBullet *b = &s_state.bullets[i];
    if (b->active && b->row == row && pvz_field_col_from_x(b->x) == col) {
      return b->from_ice ? (uint8_t)CGRAM_ICON_BULLET_ICE
                         : (uint8_t)CGRAM_ICON_BULLET;
    }
  }
  const PvzCell *c = &s_state.cells[row][col];
  if (c->type == PVZ_PLANT_DAY) return (uint8_t)CGRAM_ICON_PLANT_DAY;
  if (c->type == PVZ_PLANT_NIGHT) return (uint8_t)CGRAM_ICON_PLANT_NIGHT;
  if (c->type == PVZ_PLANT_ICE) return (uint8_t)CGRAM_ICON_PLANT_ICE;
  return (uint8_t)' ';
}

static void pvz_render_game(void)
{
  UI_BeginFrame();
  uint8_t blink_on = Widgets_BlinkOn();

  /* HUD: cols 0-2 (PVZ_HUD_COLS). Row3 shows day/night stock at a glance. */
  char hud[PVZ_HUD_COLS + 1u];
  UI_PutChar(0, 0, (uint8_t)CGRAM_ICON_HEART);
  snprintf(hud, sizeof(hud), "%2u", (unsigned)s_state.lives);
  UI_Print(0, 1, hud);

  snprintf(hud, sizeof(hud), "%3u", (unsigned)(s_state.score % 1000u));
  UI_Print(1, 0, hud);

  uint8_t inv = (s_place_type == PVZ_PLANT_DAY) ? s_state.inventory_day
                : (s_place_type == PVZ_PLANT_NIGHT) ? s_state.inventory_night
                                                     : s_state.inventory_ice;
  uint8_t type_icon = (s_place_type == PVZ_PLANT_DAY) ? (uint8_t)CGRAM_ICON_PLANT_DAY
                      : (s_place_type == PVZ_PLANT_NIGHT) ? (uint8_t)CGRAM_ICON_PLANT_NIGHT
                                                            : (uint8_t)CGRAM_ICON_PLANT_ICE;
  UI_PutChar(2, 0, type_icon);
  snprintf(hud, sizeof(hud), "%2u", (unsigned)inv);
  UI_Print(2, 1, hud);

  /* Compact day/night inventory (ice is the selected type on row 2 when
   * cycling). Cap display digits so 3 HUD cols never overflow. */
  {
    uint8_t d = s_state.inventory_day;
    uint8_t n = s_state.inventory_night;
    if (d > 9u) d = 9u;
    if (n > 9u) n = 9u;
    UI_PutChar(3, 0, (uint8_t)('0' + d));
    UI_PutChar(3, 1, (uint8_t)'/');
    UI_PutChar(3, 2, (uint8_t)('0' + n));
  }

  /* Field: cols PVZ_HUD_COLS..UI_COLS-1, rows 0..PVZ_GRID_ROWS-1. */
  for (uint8_t row = 0; row < PVZ_GRID_ROWS; row++) {
    for (uint8_t col = 0; col < PVZ_GRID_COLS; col++) {
      uint8_t glyph = pvz_cell_glyph(row, col, blink_on);
      uint8_t is_cursor = (row == s_cur_row && col == s_cur_col) ? 1u : 0u;
      if (is_cursor && glyph == (uint8_t)' ' && !blink_on) {
        glyph = (uint8_t)'_';
      } else if (is_cursor && blink_on) {
        glyph = (uint8_t)'+';
      }
      UI_PutChar(row, (uint8_t)(PVZ_HUD_COLS + col), glyph);
    }
  }
  UI_EndFrame();
}

static void pvz_render_game_over(void)
{
  UI_BeginFrame();
  UI_Print(0, 3, "GAME OVER");
  char line[UI_COLS + 1];
  snprintf(line, sizeof(line), "Score: %u", (unsigned)s_state.score);
  UI_Print(1, 2, line);
  snprintf(line, sizeof(line), "Survived: %us", (unsigned)s_state.survival_total_s);
  UI_Print(2, 2, line);
  /* Innovation I5: one-line achievement toast when newly unlocked. */
  if (s_game_over_new_ach != 0u) {
    if ((s_game_over_new_ach & 0x03u) == 0x03u) {
      UI_Print(3, 0, "Ach:3min+10kills");
    } else if (s_game_over_new_ach & 0x01u) {
      UI_Print(3, 1, "Ach: 3min!");
    } else {
      UI_Print(3, 0, "Ach: 10 kills!");
    }
  } else {
    UI_Print(3, 1, s_game_over_is_new_best ? "New high score! SEL" : "SEL to continue");
  }
  UI_EndFrame();
}

static void pvz_render_settings(void)
{
  UI_BeginFrame();
  const StorageSettings *s = Storage_GetSettings();
  char line[UI_COLS + 1];
  uint8_t blink_on = Widgets_BlinkOn();

  const char *labels[PVZ_SET_ROW_COUNT];
  labels[PVZ_SET_ROW_DAMAGE] = "Damage";
  labels[PVZ_SET_ROW_SPEED] = "Zombie speed";
  labels[PVZ_SET_ROW_DIFFICULTY] = "Difficulty";
  labels[PVZ_SET_ROW_START_PLANTS] = "Start plants";

  uint16_t values[PVZ_SET_ROW_COUNT];
  values[PVZ_SET_ROW_DAMAGE] = s->pvz_damage;
  values[PVZ_SET_ROW_SPEED] = s->pvz_zombie_speed;
  values[PVZ_SET_ROW_DIFFICULTY] = s->pvz_difficulty;
  values[PVZ_SET_ROW_START_PLANTS] = s->pvz_start_plants;

  for (uint8_t r = 0; r < PVZ_SET_ROW_COUNT; r++) {
    uint8_t is_sel = (r == s_settings_sel) ? 1u : 0u;
    UI_PutChar(r, 0, (is_sel && blink_on) ? (uint8_t)'>' : (uint8_t)' ');
    snprintf(line, sizeof(line), "%s: %u", labels[r], (unsigned)values[r]);
    UI_Print(r, 1, line);
  }
  UI_EndFrame();
}

static void pvz_render(void)
{
  switch (s_screen) {
    case PVZ_SCREEN_MENU:          pvz_render_menu(); break;
    case PVZ_SCREEN_PLANT_SELECT:  pvz_render_plant_select(); break;
    case PVZ_SCREEN_GAME:          pvz_render_game(); break;
    case PVZ_SCREEN_GAME_OVER:     pvz_render_game_over(); break;
    case PVZ_SCREEN_SETTINGS:      pvz_render_settings(); break;
    default: break;
  }
}

static void pvz_on_suspend(void)
{
  /* no-op by design (plan section 4.4) -- s_state/s_screen simply persist,
   * which is exactly what makes "Continue" work. Seg7/pot-role reset on
   * the way out is centralized in phone.c's Phone_SwitchApp(), same
   * pattern app_music.c's header comment already documents for itself. */
}

/** Hierarchical BACK: every nested PvZ screen -- including a live game --
 *  returns to the PvZ menu; only the PvZ menu itself leaves BACK to
 *  phone.c's Menu exit (back to the smartphone shell).
 *
 *  Bug report ("if i'm in a PvZ game and i press back, it has to get back
 *  to the menu of PvZ not the menu of smartphone itself"): PVZ_SCREEN_GAME
 *  used to fall through to `return 0u`, handing BACK to phone.c which always
 *  exits straight to AppMenu. Fixed by treating PVZ_SCREEN_GAME the same as
 *  the other nested screens: switch back to PVZ_SCREEN_MENU and consume the
 *  key (return 1u). This is a pause, not an abandon -- pvz_on_event()'s
 *  EV_GAME_TICK handler already no-ops whenever `s_screen != PVZ_SCREEN_GAME`
 *  (see its "simulation only advances while the live game screen is
 *  showing" comment), so s_state simply stops advancing while parked on the
 *  menu, and "Continue" (already wired via s_game_active) resumes exactly
 *  where the player left off -- no new state needed. */
static uint8_t pvz_on_back(void)
{
  switch (s_screen) {
    case PVZ_SCREEN_SETTINGS:
    case PVZ_SCREEN_PLANT_SELECT:
    case PVZ_SCREEN_GAME_OVER:
    case PVZ_SCREEN_GAME:
      s_screen = PVZ_SCREEN_MENU;
      Seg7_SetMode(SEG_OFF);
      pvz_ensure_bgm();
      return 1u;
    case PVZ_SCREEN_MENU:
    default:
      return 0u;
  }
}

const App AppPvz = {
  .on_enter   = pvz_on_enter,
  .on_event   = pvz_on_event,
  .on_tick    = pvz_on_tick,
  .render     = pvz_render,
  .on_suspend = pvz_on_suspend,
  .on_back    = pvz_on_back,
};
