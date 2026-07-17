/**
 * @file app_config.h
 * @brief Tunables. Populated incrementally as each phase needs them
 *        (see plan section 3 file tree) -- no speculative entries.
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* Phase 1: raw keypad scan debounce, matches the previous-project reference
 * (ToDo/3/HangmanGame) and plan section 5.1's DEBOUNCE_MS ~= 25 ms figure.
 * Phase 2: same constant now also gates the TIM6-driven state machine's
 * press debounce AND its release debounce (plan section 5.1 steps 2 & 4
 * both cite "DEBOUNCE_MS ~= 25 ms" for a single shared figure). */
#define KEYPAD_DEBOUNCE_MS   25u

/* Phase 2 (plan section 5.1 step 4): held >= this long in letter mode
 * emits the DIGIT event immediately instead of waiting for release. */
#define KEYPAD_HOLD_MS       900u

/* Phase 2 (plan section 5.1 step 5): same key retapped within this window
 * advances the multi-tap letter cycle; a different key or window expiry
 * commits the pending character. */
#define KEYPAD_TAP_WINDOW_MS 1000u

/* Phase 2 (plan section 4.2): cross-module event queue depth. */
#define EVENT_QUEUE_LEN      16u

/* Phase 3 (plan section 4.3 task table: "ui_render_dirty(); cgram_apply();
 * IWDG kick; osDelay(30)"): ui_task's fixed LCD-refresh period, taken
 * verbatim from the table's own osDelay(30) figure. */
#define UI_RENDER_TICK_MS    30u

/* Phase 3: app_task's osMessageQueueGet() timeout. The plan's table only
 * says "osMessageQueueGet(eventQueue) -> phone_dispatch(ev)" with no
 * explicit tick, but section 5.7 requires a "blinking selection" widget,
 * which needs a periodic re-check of elapsed time somewhere outside an
 * ISR. Using a bounded queue-get timeout (instead of infinite block) to
 * drive Phone_Tick()/on_tick() is the smallest change that satisfies both:
 * the queue is still ISR-fed (rule 0.1 unaffected -- this bounds a
 * *wait*, it does not poll hardware), and blink state gets re-evaluated
 * every APP_TASK_TICK_MS regardless of key traffic. */
#define APP_TASK_TICK_MS     50u

/* Phase 3 (plan section 5.7: "blink_cursor ... toggled by TIM6-derived
 * 500 ms soft timer"). softtimer.c (plan section 3, kernel group) is not
 * yet built -- deferred per YAGNI until a second consumer needs it: for
 * now Widgets_BlinkOn() reads HAL_GetTick() directly from app_task
 * context (never from an ISR), which is functionally identical for a
 * single 500 ms toggle. Revisit if/when softtimer.c lands. */
#define UI_BLINK_MS          500u

/* Phase 11 (plan section 5.10): 7-icon menu layouts. Settings/
 * `/setting-icons-{0,1,2}` range stays 0-2; dimensions remapped from the
 * old 6-cell grids (6x1/3x2/2x3) to 7-capable ones. Old macro names kept
 * as aliases so storage defaults / comments that still mention them remain
 * readable.
 *
 * Phase 9 (plan section 6, "Piano" bonus app added as an 8th menu icon):
 * the one-row layout had *zero* spare cells at 7 items (UI_COLS/7 == 2,
 * cols 0-6 exactly fill row 0), so adding an 8th item overflows it --
 * menu_move()'s row/col wraparound math (app_menu.c) would never be able
 * to reach idx 7 since rows stayed pinned at 1. Remapped 7x1 -> 8x1
 * (cols 0-7, still UI_COLS/8 == 2 px/cell, same "icon only, no label"
 * cell width as before -- cell_w>2 label path in menu_render() was never
 * reachable at 7 cols either). The 4x2 (8 cells) and 3x3 (9 cells) grids
 * already had exactly enough/spare capacity for an 8th item with no
 * change, matching how 3x3 already carried 2 unused cells at 7 items.
 * Numeric values (0/1/2) are UNCHANGED -- only what 0 measures out to
 * changed -- because icon_layout is a persisted flash field
 * (StorageSettings.icon_layout) and old blobs must keep meaning the same
 * setting after this firmware update, same precedent as the Phase 11
 * 6x1->7x1 remap on this same line. */
#define MENU_LAYOUT_8X1 0
#define MENU_LAYOUT_4X2 1
#define MENU_LAYOUT_3X3 2
#define MENU_LAYOUT_7X1 MENU_LAYOUT_8X1 /* alias: was 7x1, now 8x1 */
#define MENU_LAYOUT_6X1 MENU_LAYOUT_8X1 /* alias: was 6x1, now 8x1 */
#define MENU_LAYOUT_3X2 MENU_LAYOUT_4X2 /* alias: was 3x2, now 4x2 */
#define MENU_LAYOUT_2X3 MENU_LAYOUT_3X3 /* alias: was 2x3, now 3x3 */
#define MENU_LAYOUT     MENU_LAYOUT_4X2

/* Phase 11 (plan section 5.10): SMS send wait timeout. softtimer.h is
 * still not built -- AppSms uses a HAL_GetTick() deadline in app_task
 * (same Widgets_BlinkOn precedent, section 9.5 / 9.11). */
#define SMS_SEND_TIMEOUT_MS 10000u

/* Phase 3 (plan section 5.9): boot logo display time before auto-advancing
 * to Menu. Temporary stand-in for the real `/start`-gated boot (cmdparse.c
 * is Phase 7) -- see phone.h's header comment. */
#define BOOT_LOGO_MS    1500u

/* Phase 4 (plan section 5.3: buzzer/melody engine). Initial volume applied
 * by Buzzer_Init() before the pot's first filtered sample arrives (~250 ms
 * after boot at 20 Hz / 5-sample median, plan section 5.4) -- picked as a
 * moderate, clearly-audible-but-not-max default; the pot or Vol+/- keys
 * immediately override it once analog.c/phone.c start applying real input. */
#define BUZZER_DEFAULT_VOLUME_PCT 60u

/* Phase 4 (plan section 1.2: "A = Vol+, B = Vol-"). Step size per keypress,
 * in the same 0-100 volume units Buzzer_SetVolume() takes. No figure is
 * given in the plan text, so a round, clearly-perceptible-per-press value
 * is used (10 presses = full range) -- consistent with how other
 * plan-silent constants in this file (e.g. BOOT_LOGO_MS) are chosen. */
#define BUZZER_VOLUME_STEP_PCT    10u

/* Phase 4 (plan section 5.4: "LDR: compare vs settings.ldr_threshold with
 * hysteresis +-5%"). settings.c/storage.c (Phase 6) don't exist yet, so
 * settings.ldr_threshold has no backing store to read -- this is a
 * temporary compile-time stand-in for that field, same deferred-config
 * pattern already used for MENU_LAYOUT above. Raw 12-bit ADC domain
 * (0-4095, right-aligned per MX_ADC1_Init(), higher raw count = more
 * light for a standard LDR-in-a-divider wiring), threshold picked at
 * mid-scale since the actual photoresistor/divider values aren't
 * characterized yet -- revisit once Settings (Phase 6) exposes a
 * user-tunable value and/or the divider is measured on real hardware. */
#define ANALOG_LDR_THRESHOLD_RAW  2048u

/* +-5% of the full 12-bit span (4095), per plan section 5.4's literal
 * "hysteresis +-5%" -- interpreted as +-5% of full-scale rather than +-5%
 * of the threshold value itself, since full-scale gives a fixed band
 * regardless of where ANALOG_LDR_THRESHOLD_RAW ends up being tuned, which
 * is the more predictable definition to reason about. */
#define ANALOG_LDR_HYSTERESIS_RAW 205u

/* Phase 4 (plan section 5.4: "Pot deadband +-2/100 to stop volume jitter").
 * Units: 0-100 (post-scaling), matching the plan's own "2/100" notation. */
#define ANALOG_POT_DEADBAND_PCT   2u

/* Phase 5 (plan section 3: "live progress bar"). Interior width of the
 * `[####......]`-style bar in app_music.c's render(), in characters --
 * chosen so the bar plus its 2 bracket chars plus row2's other rows all
 * stay within UI_COLS==20 with margin, same "round, legible" pattern as
 * BUZZER_VOLUME_STEP_PCT above (no figure given in the plan text). */
#define MUSIC_PROGRESS_BAR_COLS   18u

/* Phase 6 (plan section 5.7: list_view "scroll window, cursor row, '+'
 * tail"). app_note.c/app_contact.c use the full 4-row LCD as the visible
 * list window (no separate header row -- every row is either a real item
 * or the synthetic "+ New" tail), so this just documents that choice
 * rather than importing ui.h's UI_ROWS into this dependency-free tunables
 * file (same standalone-macro style already used throughout). Must match
 * ui.h's UI_ROWS==4. */
#define NOTE_LIST_VISIBLE_ROWS 4u

/* Phase 7 (plan section 3: "leds.h/.c -- green/red LED patterns: solid,
 * blink-n, tick-driven"). Half-period of one blink (on-time == off-time),
 * in the same "round, legible, no figure given in the plan" spirit as
 * BUZZER_VOLUME_STEP_PCT/MUSIC_PROGRESS_BAR_COLS above. */
#define LED_BLINK_HALF_MS 150u

/* Phase 7 (plan section 5.5 RX line assembly: "byte ISR appends to line
 * buffer until \n/\r"). Max command line length, generously above the
 * longest real command (`/setting-autolock-65535` is 24 chars).
 *
 * Phase 9 update: `/songup-{name}-{count}` only carries the header (name +
 * note count) on this line -- the notes themselves stream as plain
 * `freq,ms` lines (plan section 5.5 literal; song_upload / bridge
 * `:songup`), one note per line, so no single line ever needs to hold a
 * whole song's worth of notes. Longest realistic songup header stays
 * comfortably under this cap. Excess bytes on an oversized line are
 * dropped, not buffer-overflowed. */
#define SERIAL_RX_LINE_CAP 63u

/* Phase 7 (plan section 5.9: lock sources include "auto-lock timeout
 * (innovation, Settings)"). Default idle-seconds-before-auto-lock applied
 * by Storage_Init() the very first boot (no saved settings blob yet) --
 * 0 means "disabled" everywhere this field is read, so a deliberately
 * non-zero default here is what makes the feature demoable out of the box
 * without requiring a Settings visit first. */
#define SETTINGS_DEFAULT_AUTOLOCK_S 60u

/* Phase 7 (plan section 5.4, LDR threshold default -- see this file's
 * existing ANALOG_LDR_THRESHOLD_RAW comment for why raw ADC domain).
 * Settings.ldr_threshold is now the *live* value analog.c reads once
 * app_settings.c exists; this is only its first-boot seed, kept equal to
 * the old compile-time constant so behavior doesn't change until the user
 * actually visits Settings. */
#define SETTINGS_DEFAULT_LDR_THRESHOLD ANALOG_LDR_THRESHOLD_RAW

/* Phase 7 (plan section 5.9 lock screen, PIN entry). How many
 * APP_TASK_TICK_MS ticks a "Wrong PIN" message stays on screen before
 * app_lock.c clears it and lets the user retry -- round figure in the same
 * "no explicit plan figure" spirit as LED_BLINK_HALF_MS above (~1s at the
 * current 50 ms tick). */
#define LOCK_WRONG_PIN_FLASH_TICKS 20u

/* Phase 8 (plan section 5.8 "PvZ (app_pvz.c)"): grid model. HUD is
 * UI_COLS' leftmost 3 columns (hearts/score/inventory), the field is the
 * remaining 17 columns (col 3-19) -- "matches spec screenshot" per the
 * plan's own wording. PVZ_HUD_COLS + PVZ_GRID_COLS must equal UI_COLS==20. */
#define PVZ_GRID_ROWS   4u
#define PVZ_GRID_COLS   17u
#define PVZ_HUD_COLS    3u

/* Fixed-size entity pools -- pvz_engine.c never allocates. Sized generously
 * above what PVZ_GRID_ROWS*PVZ_GRID_COLS geometry can plausibly need on
 * screen at once (at most one zombie/bullet "in flight" per row per few
 * columns at the tuned speeds below), not a plan-given figure. */
#define PVZ_MAX_ZOMBIES 6u
#define PVZ_MAX_BULLETS 8u

/* Plan section 5.8: "lives=2". */
#define PVZ_START_LIVES 2u

/* PDF p.7 + plan section 5.8: start-of-game plant-select count defaults to
 * 4 ("چهار گیاه"); Settings' pvz_start_plants may lower it (PDF p.8
 * "تعداد پایه گیاهان در شروع بازی") but not raise it past this ceiling. */
#define PVZ_PLANT_SELECT_COUNT 4u

/* Plan section 5.8 bonus 4 ice plant: NewGame grants this many ice plants
 * so the unit is placeable. Score-driven bonuses stay day/night-only
 * (section 9.10 note 6) -- ice has no score grant path in the PDF either
 * (PDF p.7: only two plant *types*, day/night; ice is the plan's bonus-4
 * elaboration of PDF bonus item 4). */
#define PVZ_START_ICE_PLANTS 1u

/* Plan section 5.8: "+1 score per zombie kill ... every 10 points -> +1
 * plant inventory ... TIM2 seconds: at 60 -> wrap + bonus point". */
#define PVZ_SCORE_PER_KILL          1u
#define PVZ_SCORE_PER_MINUTE_WRAP   1u
#define PVZ_SCORE_PER_BONUS_PLANT   10u

/* PDF p.7: "در تمام بخش های بازی آهنگ پس زمینه بازی اجرا خواهد شد".
 * Melody indices into buzzer.c's s_melodies[] -- PDF/plan name no specific
 * track, so these are this file's design choice (same footing as which
 * built-in songs exist). BGM is a longer loopable tune; game-over uses the
 * short Alert jingle (plan section 5.8 "game over jingle"). */
/* Phase 8 hotfix (bug report "music is not plants vs. zombies"): repointed
 * from the generic "Bones" track (index 6) to a new original whimsical
 * garden-march loop composed for this project, buzzer.c's s_melody_pvz
 * (last table entry -- see buzzer.c's MELODY_COUNT/s_melodies comment). */
#define PVZ_BGM_MELODY_IDX        13u /* "PvZ Theme" */
#define PVZ_GAMEOVER_MELODY_IDX   3u /* "Alert" */

/* Positions/speeds use fixed-point "hundredths of a cell" (100 == one full
 * PVZ_GRID_COLS cell width) so zombies/bullets can move sub-cell distances
 * each 10 Hz EV_GAME_TICK without floating point -- same "signed int, no
 * implicit-conversion trap" reasoning app_menu.c's menu_move() already
 * documents for its own wraparound math. */
#define PVZ_FIELD_WIDTH_HUNDREDTHS  ((int16_t)(PVZ_GRID_COLS * 100u))
/* Pea travel: ~0.4 cell/tick @ 10 Hz ≈ 4 cells/s -- slow enough to SEE on
 * the 17-col field (old 150 = 1.5 cells/tick was nearly invisible). */
#define PVZ_BULLET_SPEED_HUNDREDTHS 40

/* Settings-adjustable game params (StorageSettings, storage.h) -- first-boot
 * defaults, same "round, legible, no figure given in the plan beyond a
 * relative description" spirit as BUZZER_VOLUME_STEP_PCT above. Plan section
 * 5.8's own params list is {damage, zombie speed, difficulty=spawn rate,
 * starting plants} -- exact numeric defaults are this file's own design
 * choice, not a guessed spec requirement. */
#define PVZ_DEFAULT_DAMAGE        1u   /* HP removed per bullet hit */
/* Hundredths of a cell per 10 Hz tick. Default 6 ≈ 0.6 cell/s ≈ ~28 s to
 * cross the field -- closer to classic PvZ pacing than the old 20 (~2 cell/s).
 * Still Settings-tunable (app_pvz steps by 1 in range below). */
#define PVZ_DEFAULT_ZOMBIE_SPEED  6u
#define PVZ_ZOMBIE_SPEED_MIN      2u
#define PVZ_ZOMBIE_SPEED_MAX      40u
#define PVZ_DEFAULT_DIFFICULTY    40u  /* ticks between spawns (~4 s @ 10 Hz);
                                         * LOWER value = harder */
#define PVZ_DEFAULT_START_PLANTS  PVZ_PLANT_SELECT_COUNT

/* Phase 8 hotfix (bug report "zombie rate should change shouldn't it?"):
 * pvz_difficulty was previously a flat spawn interval for the entire run.
 * pvz_engine.c's spawn_tick() now ramps the effective interval down as
 * zombies_spawned grows, so later game gets harder -- values are this
 * file's own design choice (not a plan/PDF figure): every 5 zombies
 * spawned, the interval drops by 3 ticks (~0.3 s).
 *
 * Phase 8 hotfix ROUND 2 (bug report "why do two zombies come at once,
 * that's not right"): spawn_tick() only ever spawns one zombie per call and
 * this ramp already floors the interval well above 0, so a true same-tick
 * double-spawn isn't reachable from this file's logic. The far more likely
 * cause was the now-fixed §9.10b regression (ui.c's forced-full-redraw held
 * s_mutex for up to ~1 s, which blocked app_task's render() and let
 * EV_GAME_TICK events pile up/get dropped) -- when app_task got a chance to
 * catch up, it could drain several backlogged ticks back-to-back faster
 * than ui_task's 30 ms render cadence could visibly flush each intermediate
 * frame, so two spawn-timer expiries landing in that catch-up burst could
 * appear on the physical LCD as if they happened "at once" even though they
 * were sequential internally. With that regression reverted, ticks are
 * delivered at their real 10 Hz cadence and this shouldn't reproduce.
 * PVZ_DIFFICULTY_MIN raised 12 -> 20 (~1.2 s -> ~2.0 s) as an extra pacing
 * safety margin on top of that fix, so even late-game the floor stays
 * comfortably above a single 30 ms render tick's worth of slack. */
#define PVZ_DIFFICULTY_RAMP_EVERY_N  5u
#define PVZ_DIFFICULTY_RAMP_STEP     3u
#define PVZ_DIFFICULTY_MIN           20u

/* Ticks until the first zombie after NewGame (10 Hz). Full difficulty
 * interval applies to every spawn after that. Kept shorter than
 * pvz_difficulty so the first wave is visible quickly -- the old
 * spawn_timer=difficulty-only path meant ~3 s of an empty field before
 * anything appeared, and x==FIELD_WIDTH put col 17 off the 0..16 grid. */
#define PVZ_FIRST_SPAWN_TICKS     15u

/* Plan section 5.8 bonus 4: "Tank zombie (3x HP, spawns every Nth)",
 * "Ice plant (hit slows zombie 50% for 3 s)". */
#define PVZ_TANK_HP_MULT       3u
#define PVZ_TANK_EVERY_N       5u
#define PVZ_ICE_SLOW_PCT       50u
#define PVZ_ICE_SLOW_TICKS     40u /* 4 s at the 10 Hz game tick */

/* Zombie base HP and attack-on-plant rate, this file's own design choice
 * (not given a figure by the plan) -- attack removes 1 plant HP every
 * PVZ_ZOMBIE_ATTACK_TICKS while adjacent, so a freshly-placed plant (see
 * PVZ_PLANT_START_HP) survives a few seconds of chewing before a player can
 * react. */
#define PVZ_ZOMBIE_BASE_HP        3u
#define PVZ_ZOMBIE_ATTACK_TICKS   15u /* 1 HP removed every ~1.5 s */
#define PVZ_PLANT_START_HP        5u

/* Plant auto-fire cadence -- ticks between shots, per plant.
 *
 * Phase 8 hotfix ROUND 2 (bug report "rate of shooting from those plants
 * are a little bit high and cause some inconsistency"): raised 10 -> 16
 * (~1.0 s -> ~1.6 s) to slow the per-plant rate down, and see
 * pvz_engine.c's plants_fire() for the accompanying fix to a real
 * consistency bug -- fire_cooldown used to only get set when a bullet slot
 * was actually free, so a plant firing into a momentarily-full
 * PVZ_MAX_BULLETS pool would retry every tick with no cooldown at all and
 * then fire instantly the moment a slot freed, instead of keeping a steady
 * cadence. */
#define PVZ_PLANT_FIRE_TICKS      16u

#endif /* APP_CONFIG_H */
