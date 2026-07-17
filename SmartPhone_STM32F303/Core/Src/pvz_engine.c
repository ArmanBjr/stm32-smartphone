/**
 * @file pvz_engine.c
 * @brief Phase 8: PvZ simulation core. See pvz_engine.h for the design
 *        choices this implements and why they were made.
 */
#include "pvz_engine.h"
#include <string.h>

/** xorshift32, same tiny dependency-free PRNG app_music.c already uses for
 *  its shuffle picks (no <stdlib.h> rand()/srand() for a single-use pick).
 *
 *  Phase 8 hotfix (bug report "plant do not fire those bullets", traced to
 *  spawn_tick()'s row distribution): the original caller pattern *reseeded*
 *  `state` fresh from a small monotonically-increasing salt
 *  (zombies_spawned+1) on every single spawn, then took only one
 *  xorshift32_next() step off that fresh seed. xorshift's avalanche is weak
 *  after just one step from small sequential seeds, so the resulting row
 *  sequence was a near-deterministic rotation (hand-verified for seeds
 *  1..5: rows came out 1,2,3,0,1,2,3,0,...) instead of anything resembling
 *  random -- some lanes could go a very long time without a zombie, making
 *  any plant placed there look like it "never fires" (plants only fire once
 *  a zombie is active in their row, by design -- see pvz_engine.h). Fixed
 *  by seeding `PvzState.rng_state` exactly once in NewGame and letting
 *  every spawn_tick() call advance the *same* persistent state (no
 *  reseed) -- this is a real evolving PRNG stream instead of one-shot
 *  reseeds, still with zero HAL_GetTick()/hardware dependency (this file
 *  stays hardware-free per its own header comment). */
static uint32_t xorshift32_next(uint32_t *state)
{
  if (*state == 0u) {
    *state = 0x2545F491u; /* must be nonzero */
  }
  *state ^= *state << 13;
  *state ^= *state >> 17;
  *state ^= *state << 5;
  return *state;
}

void PvzEngine_GrantIcePlant(PvzState *st, uint8_t count)
{
  /* Saturate at 255 rather than wrap -- inventory is a uint8_t HUD count. */
  uint16_t sum = (uint16_t)st->inventory_ice + (uint16_t)count;
  st->inventory_ice = (sum > 255u) ? 255u : (uint8_t)sum;
}

void PvzEngine_NewGame(PvzState *st, uint8_t start_day, uint8_t start_night,
                        uint8_t damage, uint8_t zombie_speed, uint8_t difficulty)
{
  memset(st, 0, sizeof(*st));
  st->lives = PVZ_START_LIVES;
  st->inventory_day = start_day;
  st->inventory_night = start_night;
  if (damage < 1u) {
    damage = PVZ_DEFAULT_DAMAGE;
  }
  if (zombie_speed < PVZ_ZOMBIE_SPEED_MIN) {
    zombie_speed = PVZ_DEFAULT_ZOMBIE_SPEED;
  }
  if (zombie_speed > PVZ_ZOMBIE_SPEED_MAX) {
    zombie_speed = PVZ_ZOMBIE_SPEED_MAX;
  }
  if (difficulty < 5u) {
    difficulty = PVZ_DEFAULT_DIFFICULTY;
  }
  st->damage = damage;
  st->zombie_speed = zombie_speed;
  st->difficulty = difficulty;
  st->spawn_timer = PVZ_FIRST_SPAWN_TICKS;
  /* Seed the persistent row-spawn RNG once per game (see xorshift32_next()'s
   * header comment) -- mixed from the settings snapshot so back-to-back
   * games with different Settings don't replay an identical row sequence.
   * Never zero (xorshift32_next() also guards this, but keep it explicit). */
  st->rng_state = 0x2545F491u ^ ((uint32_t)damage << 24)
                  ^ ((uint32_t)zombie_speed << 16) ^ ((uint32_t)difficulty << 8)
                  ^ ((uint32_t)start_day << 4) ^ (uint32_t)start_night;
  if (st->rng_state == 0u) {
    st->rng_state = 0x2545F491u;
  }
  PvzEngine_GrantIcePlant(st, PVZ_START_ICE_PLANTS);
}

/** Grants the plan's "every PVZ_SCORE_PER_BONUS_PLANT points -> +1 plant
 *  inventory" bonus, called after any score increase. May grant more than
 *  once per call if a big enough score jump crosses multiple thresholds
 *  (not currently possible with PVZ_SCORE_PER_KILL==1, but correct
 *  regardless of future tuning). `is_night` picks day vs night inventory
 *  (plan section 5.8 / 9.10 note 6) -- never ice. */
static void grant_score_bonus_if_due(PvzState *st, uint16_t score_delta, uint8_t is_night)
{
  st->score_bonus_accum = (uint16_t)(st->score_bonus_accum + score_delta);
  while (st->score_bonus_accum >= PVZ_SCORE_PER_BONUS_PLANT) {
    st->score_bonus_accum = (uint16_t)(st->score_bonus_accum - PVZ_SCORE_PER_BONUS_PLANT);
    if (is_night) {
      st->inventory_night++;
    } else {
      st->inventory_day++;
    }
  }
}

/** Plan section 5.8 "and clears" on life loss: remove every in-flight
 *  threat. Planted cells are left alone (player progress). */
static void clear_threats(PvzState *st)
{
  for (uint8_t z = 0; z < PVZ_MAX_ZOMBIES; z++) {
    st->zombies[z].active = 0u;
  }
  for (uint8_t i = 0; i < PVZ_MAX_BULLETS; i++) {
    st->bullets[i].active = 0u;
  }
}

static void bullets_tick(PvzState *st, uint8_t is_night)
{
  for (uint8_t i = 0; i < PVZ_MAX_BULLETS; i++) {
    PvzBullet *b = &st->bullets[i];
    if (!b->active) {
      continue;
    }
    b->x = (int16_t)(b->x + PVZ_BULLET_SPEED_HUNDREDTHS);
    if (b->x >= PVZ_FIELD_WIDTH_HUNDREDTHS) {
      b->active = 0u; /* flew off the field without hitting anything */
      continue;
    }
    /* Hit the front-most overlapping zombie in this lane (smallest x),
     * not the first pool slot -- otherwise a behind zombie can eat a
     * bullet meant for the one closer to the plants. */
    PvzZombie *hit = NULL;
    for (uint8_t z = 0; z < PVZ_MAX_ZOMBIES; z++) {
      PvzZombie *zb = &st->zombies[z];
      if (!zb->active || zb->row != b->row) {
        continue;
      }
      if (b->x >= zb->x && b->x < (int16_t)(zb->x + 100)) {
        if (hit == NULL || zb->x < hit->x) {
          hit = zb;
        }
      }
    }
    if (hit == NULL) {
      continue;
    }
    hit->hp = (int16_t)(hit->hp - (int16_t)st->damage);
    b->active = 0u;
    st->fx_hits++;
    if (b->from_ice) {
      hit->slow_ticks_left = PVZ_ICE_SLOW_TICKS;
    }
    if (hit->hp <= 0) {
      hit->active = 0u;
      st->fx_kills++;
      st->score = (uint16_t)(st->score + PVZ_SCORE_PER_KILL);
      grant_score_bonus_if_due(st, PVZ_SCORE_PER_KILL, is_night);
    }
  }
}

static void plants_fire(PvzState *st, uint8_t is_night)
{
  for (uint8_t row = 0; row < PVZ_GRID_ROWS; row++) {
    /* Only bother scanning a lane that actually has a live zombie in it --
     * skip firing at an empty lane so PVZ_MAX_BULLETS isn't wasted. */
    uint8_t zombie_in_lane = 0u;
    for (uint8_t z = 0; z < PVZ_MAX_ZOMBIES; z++) {
      if (st->zombies[z].active && st->zombies[z].row == row) {
        zombie_in_lane = 1u;
        break;
      }
    }
    if (!zombie_in_lane) {
      continue;
    }
    for (uint8_t col = 0; col < PVZ_GRID_COLS; col++) {
      PvzCell *c = &st->cells[row][col];
      if (c->type == PVZ_PLANT_NONE || c->hp == 0u) {
        continue;
      }
      if (c->fire_cooldown > 0u) {
        c->fire_cooldown--;
        continue;
      }
      uint8_t may_fire = (c->type == PVZ_PLANT_ICE)
                              || (c->type == PVZ_PLANT_DAY && !is_night)
                              || (c->type == PVZ_PLANT_NIGHT && is_night);
      if (!may_fire) {
        continue;
      }
      /* Bug report ("rate of shooting ... a little bit high and cause some
       * inconsistency"): fire_cooldown used to be set *only* inside the
       * "found a free bullet slot" branch below. Whenever PVZ_MAX_BULLETS
       * (8, shared across every plant on the field) was briefly exhausted,
       * this plant's cooldown stayed at 0 and it re-attempted every single
       * tick with no wait, then fired the instant a slot freed up -- so
       * cadence wasn't a steady ~1 shot/PVZ_PLANT_FIRE_TICKS per plant, it
       * was bursty: silence while the pool was full, then an immediate shot
       * the moment any bullet (from any plant) expired. Fixed by starting
       * the cooldown as soon as this plant is eligible to fire, regardless
       * of whether a bullet slot was actually available -- every plant now
       * has a strict, consistent per-shot cadence, and a shot lost to a
       * full pool is simply lost (same as before) rather than turning into
       * an instant retry. */
      c->fire_cooldown = PVZ_PLANT_FIRE_TICKS;
      for (uint8_t i = 0; i < PVZ_MAX_BULLETS; i++) {
        if (!st->bullets[i].active) {
          st->bullets[i].active = 1u;
          st->bullets[i].row = row;
          st->bullets[i].x = (int16_t)(col * 100);
          st->bullets[i].from_ice = (c->type == PVZ_PLANT_ICE) ? 1u : 0u;
          break;
        }
      }
    }
  }
}

/** Advances one zombie by one tick. Returns 1 if this zombie just broke
 *  through the left edge (a life is lost), 0 otherwise. */
static uint8_t zombie_advance(PvzState *st, PvzZombie *zb)
{
  if (zb->attack_ticks > 0u) {
    zb->attack_ticks--;
    if (zb->attack_ticks == 0u) {
      uint8_t col = (uint8_t)(zb->x / 100);
      if (col < PVZ_GRID_COLS) {
        PvzCell *c = &st->cells[zb->row][col];
        if (c->type != PVZ_PLANT_NONE && c->hp > 0u) {
          c->hp--;
          if (c->hp == 0u) {
            c->type = PVZ_PLANT_NONE; /* chewed through -- lane reopens */
          } else {
            zb->attack_ticks = PVZ_ZOMBIE_ATTACK_TICKS; /* keep chewing */
          }
        }
      }
    }
    return 0u;
  }

  uint16_t speed = st->zombie_speed;
  if (zb->slow_ticks_left > 0u) {
    speed = (uint16_t)((speed * (100u - PVZ_ICE_SLOW_PCT)) / 100u);
    zb->slow_ticks_left--;
  }
  int16_t new_x = (int16_t)(zb->x - (int16_t)speed);
  if (new_x <= 0) {
    zb->active = 0u; /* reached the HUD edge -- breached */
    return 1u;
  }
  uint8_t target_col = (uint8_t)(new_x / 100);
  if (target_col < PVZ_GRID_COLS) {
    PvzCell *c = &st->cells[zb->row][target_col];
    if (c->type != PVZ_PLANT_NONE && c->hp > 0u) {
      /* Blocked -- park at the plant's near edge and start chewing instead
       * of moving into/through the occupied cell this tick. Clamp so x
       * never lands on col PVZ_GRID_COLS (off the right edge / invisible). */
      int16_t park_x = (int16_t)((target_col + 1u) * 100u - 1);
      if (park_x >= PVZ_FIELD_WIDTH_HUNDREDTHS) {
        park_x = (int16_t)(PVZ_FIELD_WIDTH_HUNDREDTHS - 1);
      }
      zb->x = park_x;
      zb->attack_ticks = PVZ_ZOMBIE_ATTACK_TICKS;
      return 0u;
    }
  }
  zb->x = new_x;
  return 0u;
}

static void zombies_tick(PvzState *st, uint8_t *life_lost)
{
  for (uint8_t z = 0; z < PVZ_MAX_ZOMBIES; z++) {
    PvzZombie *zb = &st->zombies[z];
    if (!zb->active) {
      continue;
    }
    if (zombie_advance(st, zb)) {
      /* At most one life per Tick even if several zombies breach together. */
      if (!*life_lost) {
        *life_lost = 1u;
        if (st->lives > 0u) {
          st->lives--;
        }
      }
    }
  }
}

static void spawn_tick(PvzState *st)
{
  if (st->spawn_timer > 0u) {
    st->spawn_timer--;
    return;
  }

  /* Phase 8 hotfix (bug report "zombie rate should change shouldn't it?"):
   * ramp the effective spawn interval down as the run progresses instead of
   * pinning it at the flat Settings-configured st->difficulty forever --
   * see app_config.h's PVZ_DIFFICULTY_RAMP_* comment for the numbers. */
  uint16_t ramp = (uint16_t)((st->zombies_spawned / PVZ_DIFFICULTY_RAMP_EVERY_N)
                              * PVZ_DIFFICULTY_RAMP_STEP);
  uint16_t effective_difficulty = (ramp < st->difficulty)
                                       ? (uint16_t)(st->difficulty - ramp)
                                       : 0u;
  if (effective_difficulty < PVZ_DIFFICULTY_MIN) {
    effective_difficulty = PVZ_DIFFICULTY_MIN;
  }
  st->spawn_timer = effective_difficulty;

  uint8_t slot = 0xFFu;
  for (uint8_t i = 0; i < PVZ_MAX_ZOMBIES; i++) {
    if (!st->zombies[i].active) {
      slot = i;
      break;
    }
  }
  if (slot == 0xFFu) {
    return; /* pool full -- skip this spawn, tries again next difficulty window */
  }

  uint8_t row = (uint8_t)(xorshift32_next(&st->rng_state) % PVZ_GRID_ROWS);

  st->zombies_spawned++;
  uint8_t is_tank = (uint8_t)((st->zombies_spawned % PVZ_TANK_EVERY_N) == 0u);

  PvzZombie *zb = &st->zombies[slot];
  zb->active = 1u;
  zb->row = row;
  /* Enter the rightmost visible column (col PVZ_GRID_COLS-1). Spawning at
   * x==FIELD_WIDTH (col 17) was off the 0..16 grid and never drawn. */
  zb->x = (int16_t)(PVZ_FIELD_WIDTH_HUNDREDTHS - 1);
  zb->type = is_tank ? PVZ_ZOMBIE_TANK : PVZ_ZOMBIE_NORMAL;
  zb->hp = (int16_t)(PVZ_ZOMBIE_BASE_HP * (is_tank ? PVZ_TANK_HP_MULT : 1u));
  zb->slow_ticks_left = 0u;
  zb->attack_ticks = 0u;
}

PvzTickResult PvzEngine_Tick(PvzState *st, uint8_t is_night)
{
  if (st->game_over) {
    return PVZ_TICK_GAME_OVER;
  }

  st->fx_hits = 0u;
  st->fx_kills = 0u;

  plants_fire(st, is_night);
  bullets_tick(st, is_night);

  uint8_t life_lost = 0u;
  zombies_tick(st, &life_lost);

  if (life_lost) {
    clear_threats(st); /* plan section 5.8: life loss "and clears" */
  }

  if (st->lives == 0u) {
    st->game_over = 1u;
    /* Report LIFE_LOST (not GAME_OVER) when the fatal breach just
     * happened this tick so app_pvz can fire red-LED/sad-SFX and then
     * transition to game-over -- a bare GAME_OVER alone used to skip
     * that feedback. Skip spawn_tick: the run is over. */
    return life_lost ? PVZ_TICK_LIFE_LOST : PVZ_TICK_GAME_OVER;
  }

  spawn_tick(st);
  return life_lost ? PVZ_TICK_LIFE_LOST : PVZ_TICK_NONE;
}

void PvzEngine_OnSecondTick(PvzState *st, uint8_t is_night)
{
  if (st->game_over) {
    return;
  }
  st->survival_s++;
  if (st->survival_s >= 60u) {
    st->survival_s = 0u;
    st->score = (uint16_t)(st->score + PVZ_SCORE_PER_MINUTE_WRAP);
    grant_score_bonus_if_due(st, PVZ_SCORE_PER_MINUTE_WRAP, is_night);
  }
}

uint8_t PvzEngine_PlacePlant(PvzState *st, uint8_t row, uint8_t col, PvzPlantType type)
{
  if (row >= PVZ_GRID_ROWS || col >= PVZ_GRID_COLS) {
    return 0u;
  }
  PvzCell *c = &st->cells[row][col];
  if (c->type != PVZ_PLANT_NONE) {
    return 0u; /* occupied */
  }
  switch (type) {
    case PVZ_PLANT_DAY:
      if (st->inventory_day == 0u) return 0u;
      st->inventory_day--;
      break;
    case PVZ_PLANT_NIGHT:
      if (st->inventory_night == 0u) return 0u;
      st->inventory_night--;
      break;
    case PVZ_PLANT_ICE:
      if (st->inventory_ice == 0u) return 0u;
      st->inventory_ice--;
      break;
    default:
      return 0u;
  }
  c->type = type;
  c->hp = PVZ_PLANT_START_HP;
  c->fire_cooldown = 0u;
  return 1u;
}
