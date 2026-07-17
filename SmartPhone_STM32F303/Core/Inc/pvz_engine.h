/**
 * @file pvz_engine.h
 * @brief Phase 8: PvZ simulation core, split out of app_pvz.c per this
 *        project's "many small files" convention (rules/common/coding-
 *        style.md) -- app_pvz.c owns screens/input/rendering, pvz_engine.c
 *        owns the pure grid/zombie/bullet simulation with zero UI/hardware
 *        calls, so it can be driven directly by unit-style reasoning and
 *        stays under the file-size discipline every other app in this
 *        codebase already follows.
 *
 *        Source of truth: master plan section 5.8 ("PvZ (app_pvz.c)"),
 *        section 9.10, and the project PDF (Micro-Lab_404-2_Final_project.pdf
 *        pp.7-8 + bonus item 4). Fixed-point "hundredths of a cell" position
 *        units -- see app_config.h's PVZ_FIELD_WIDTH_HUNDREDTHS comment.
 *
 *        Design choices not pinned down by the plan/spec text (documented
 *        here rather than guessed silently, per the project's "verify,
 *        don't guess" rule -- these are this file's own engineering
 *        decisions, written back into the master plan's Phase 8
 *        implementation-notes section):
 *        - A zombie blocked by a live plant stops advancing and attacks
 *          that plant every PVZ_ZOMBIE_ATTACK_TICKS instead of pushing
 *          through/around it (classic PvZ lane-blocking behavior).
 *        - Day plants only fire while !is_night, night plants only fire
 *          while is_night (PDF p.7 day/night plant split); ice plants fire
 *          in both (bonus-4 utility unit, not tied to day/night).
 *        - A bullet fired from an ice-plant cell also applies
 *          PVZ_ICE_SLOW_PCT/PVZ_ICE_SLOW_TICKS slow to whatever zombie it
 *          hits, on top of normal damage.
 *        - Score-driven inventory bonus (PDF/plan: "every 10 points -> +1
 *          plant") grants the bonus plant to whichever of day/night matches
 *          the CURRENT is_night state at the moment the threshold is
 *          crossed. Ice plants are never score-granted (PDF has only two
 *          plant types); they start at PVZ_START_ICE_PLANTS via
 *          PvzEngine_GrantIcePlant() from NewGame.
 *        - Bullet hits the front-most (leftmost x) overlapping zombie in
 *          its lane -- PDF p.8 "گیاهان به سمت زامبی ها تیر شلیک" implies
 *          the shot meets the nearest threat first.
 *        - On life loss (plan section 5.8: "−1 life ... and clears"), all
 *          active zombies and bullets are cleared; planted cells stay.
 *          At most one life is lost per Tick even if several zombies
 *          breach in the same 10 Hz window.
 *        - Phase 8 hotfix (post-implementation bug report): spawn rate now
 *          escalates over a run instead of staying pinned at the
 *          Settings-configured pvz_difficulty for the whole game --
 *          spawn_tick() ramps the effective spawn interval down by
 *          PVZ_DIFFICULTY_RAMP_STEP ticks every PVZ_DIFFICULTY_RAMP_EVERY_N
 *          zombies spawned, floored at PVZ_DIFFICULTY_MIN so the game never
 *          becomes unplayably fast. The row-spawn RNG was also switched
 *          from a per-call reseed (near-deterministic cyclic row sequence)
 *          to a persistent stream (`PvzState.rng_state`) seeded once in
 *          NewGame and advanced every spawn -- see xorshift32_next()'s
 *          header comment in pvz_engine.c.
 */
#ifndef PVZ_ENGINE_H
#define PVZ_ENGINE_H

#include <stdint.h>
#include "app_config.h"

typedef enum {
  PVZ_PLANT_NONE = 0,
  PVZ_PLANT_DAY,
  PVZ_PLANT_NIGHT,
  PVZ_PLANT_ICE,
} PvzPlantType;

typedef enum {
  PVZ_ZOMBIE_NORMAL = 0,
  PVZ_ZOMBIE_TANK,
} PvzZombieType;

/** Tick() return value: what happened this call. */
typedef enum {
  PVZ_TICK_NONE = 0,
  PVZ_TICK_LIFE_LOST,
  PVZ_TICK_GAME_OVER,
} PvzTickResult;

typedef struct {
  PvzPlantType type;
  uint8_t      hp;
  uint8_t      fire_cooldown; /* ticks until this plant may fire again */
} PvzCell;

typedef struct {
  uint8_t       active;
  uint8_t       row;
  int16_t       x;               /* hundredths of a cell, field-relative */
  int16_t       hp;
  PvzZombieType type;
  uint16_t      slow_ticks_left; /* PVZ_ICE_SLOW_TICKS countdown, 0 = normal speed */
  uint16_t      attack_ticks;    /* >0 while blocked on a plant and chewing it */
} PvzZombie;

typedef struct {
  uint8_t active;
  uint8_t row;
  int16_t x;         /* hundredths of a cell, field-relative */
  uint8_t from_ice;  /* 1 if fired by a PVZ_PLANT_ICE cell (applies slow on hit) */
} PvzBullet;

typedef struct {
  PvzCell   cells[PVZ_GRID_ROWS][PVZ_GRID_COLS];
  PvzZombie zombies[PVZ_MAX_ZOMBIES];
  PvzBullet bullets[PVZ_MAX_BULLETS];

  uint8_t  lives;
  uint16_t score;
  uint16_t survival_s;   /* wraps at 60, plan section 5.8's 7-seg SS field */
  uint8_t  game_over;

  uint8_t  inventory_day;
  uint8_t  inventory_night;
  uint8_t  inventory_ice;

  uint16_t zombies_spawned;
  uint16_t spawn_timer;   /* ticks remaining until next spawn */
  uint16_t score_bonus_accum; /* progress toward next PVZ_SCORE_PER_BONUS_PLANT */

  /* Phase 8 hotfix (bug report "zombie rate should change" + row-spawn
   * bias): persistent xorshift32 stream, seeded once in NewGame and
   * advanced (never reseeded) by every spawn_tick() call -- see
   * pvz_engine.c's xorshift32_next() header comment for why the previous
   * per-call reseed produced a near-deterministic cyclic row sequence. */
  uint32_t rng_state;

  /* Settings snapshot taken at PvzEngine_NewGame() time (storage.h's
   * pvz_damage/pvz_zombie_speed/pvz_difficulty) -- a running game is not
   * retroactively affected by a mid-game Settings change, same "snapshot
   * at start" behavior app_music.c's melody duration math implicitly
   * relies on. */
  uint8_t damage;
  uint8_t zombie_speed;
  uint8_t difficulty;

  /* Cleared at the start of each Tick; app_pvz reads them afterward for
   * hit/kill SFX (engine stays hardware-free). */
  uint8_t fx_hits;
  uint8_t fx_kills;
} PvzState;

/** Zeroes `st` and seeds lives/inventory/settings snapshot for a fresh run.
 *  `start_day`+`start_night` should sum to the plant-select pick count
 *  (Settings' pvz_start_plants, default PVZ_PLANT_SELECT_COUNT) but this
 *  function does not enforce that itself -- app_pvz.c's selection screen
 *  already guarantees it by construction. Also grants PVZ_START_ICE_PLANTS
 *  via PvzEngine_GrantIcePlant(). */
void PvzEngine_NewGame(PvzState *st, uint8_t start_day, uint8_t start_night,
                        uint8_t damage, uint8_t zombie_speed, uint8_t difficulty);

/** Bonus-4 ice inventory grant. NewGame calls this with
 *  PVZ_START_ICE_PLANTS; kept public so a later hook can grant more without
 *  touching score-bonus rules. */
void PvzEngine_GrantIcePlant(PvzState *st, uint8_t count);

/** Advances the simulation by one EV_GAME_TICK (10 Hz, app_config.h).
 *  `is_night` gates day-plant/night-plant firing AND the score-driven
 *  inventory bonus type (Analog_IsNight(), app_pvz.c's caller). No-op
 *  (returns PVZ_TICK_GAME_OVER immediately) once st->game_over is already
 *  set. */
PvzTickResult PvzEngine_Tick(PvzState *st, uint8_t is_night);

/** Called once per EV_TICK_1S (1 Hz) while a game is live: advances
 *  survival_s, wrapping at 60 with a PVZ_SCORE_PER_MINUTE_WRAP score bonus
 *  on wrap (PDF p.8 / plan section 5.8). The wrap score feeds the same
 *  score-driven inventory bonus as kills (`is_night` picks day vs night).
 *  Kept separate from PvzEngine_Tick() because it runs off a different
 *  producer (TIM2 vs TIM16) at a different rate. */
void PvzEngine_OnSecondTick(PvzState *st, uint8_t is_night);

/** Places `type` at (row,col) if the cell is empty, in bounds, and the
 *  matching inventory count is > 0 (decremented on success). Returns 1 on
 *  success, 0 otherwise (occupied/out-of-range/no inventory). */
uint8_t PvzEngine_PlacePlant(PvzState *st, uint8_t row, uint8_t col, PvzPlantType type);

#endif /* PVZ_ENGINE_H */
