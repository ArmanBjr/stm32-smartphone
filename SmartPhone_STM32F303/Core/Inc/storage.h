/**
 * @file storage.h
 * @brief Phase 6: flash persistence. Source of truth: master plan section
 *        5.6 ("Storage (storage.c)") and section 3's file-tree entry
 *        ("flash persistence (last 2 pages @0x0803F000, 2 KB pages): magic
 *        + version + CRC blob {settings, notes, contacts, uploaded songs,
 *        high scores}. save()/load()/wipe_game_only()").
 *
 *        Notes/contacts are the only payload sections with real read/write
 *        logic as of Phase 6 -- settings (Phase 7 app_settings.c), uploaded
 *        songs (Phase 9 song_upload.py/cmdparse.c), and high scores (Phase
 *        8 app_pvz.c) don't have an owning subsystem yet. Their budget is
 *        reserved in the blob NOW (as zeroed placeholder bytes) so the
 *        on-flash format doesn't need a version bump when those phases
 *        land -- only storage.c's internals change to start populating
 *        those regions instead of leaving them zeroed.
 *
 *        Phase 7 update: settings now has real read/write logic, replacing
 *        the previously-zeroed `settings_reserved[32]` region with a real
 *        `StorageSettings` struct of the exact same 32-byte size (see
 *        storage.c's `_Static_assert` next to it) -- STORAGE_VERSION is
 *        deliberately NOT bumped, per the policy documented above and in
 *        master plan section 9.8 note 3.
 *
 *        Phase 8 update: same treatment for high scores (`StorageHighscore`
 *        replaces `highscores_reserved[16]`) and for 4 new PvZ game-setting
 *        fields carved out of `StorageSettings`' own reserved padding
 *        (pvz_damage/pvz_zombie_speed/pvz_difficulty/pvz_start_plants) --
 *        again no STORAGE_VERSION bump.
 *
 *        Phase 9 update: uploaded songs now have a real owning subsystem
 *        (song_upload.py + cmdparse.c's `/songup-`/`/end` handling, buzzer.c
 *        playback) -- `songs_reserved[512]` (storage.c) replaced with a real
 *        `StorageSong songs[STORAGE_MAX_SONGS]` array. Unlike the
 *        Settings/Highscore precedent above, this array does NOT fit inside
 *        the old 512-byte placeholder unchanged: the plan's own songs
 *        budget figure ("2 uploaded songs x 64 notes", section 5.6) only
 *        ever accounted for note data (buzzer.h's `Note` is `{freqHz,ms}` =
 *        4 bytes), not song *names* -- but section 3's file tree calls
 *        song_upload.py "send *named* melody to device", so a name field is
 *        genuinely part of this phase's scope, not scope creep. The extra
 *        bytes are carved out of this struct's own slack headroom instead
 *        (`sizeof(StorageBlob)` was ~1824 of the 2048-byte page budget
 *        before this change -- storage.c's own `_Static_assert` still
 *        guards the page-fit invariant at compile time). STORAGE_VERSION is
 *        still NOT bumped: a payload-size mismatch on boot already falls
 *        back safely to `storage_zero_init()` (self-healing, no crash),
 *        same safety net the reserved-budget-growth policy above relies on.
 */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>

#define STORAGE_MAX_NOTES         10u
#define STORAGE_NOTE_TITLE_LEN    16u /* incl. NUL, plan section 5.6 budget */
#define STORAGE_NOTE_BODY_LEN     80u /* incl. NUL, plan section 5.6 budget */

#define STORAGE_MAX_CONTACTS      10u
#define STORAGE_CONTACT_NAME_LEN  16u /* incl. NUL, plan section 5.6 budget */
#define STORAGE_CONTACT_PHONE_LEN 12u /* incl. NUL, plan section 5.6 budget */

#define STORAGE_PIN_LEN           5u  /* 4 digits + NUL, plan section 5.9 */

/* Phase 9 (plan section 5.6: "2 uploaded songs x 64 notes"; section 3 file
 * tree: song_upload.py "send named melody to device"). STORAGE_MAX_SONGS
 * matches the plan; STORAGE_SONG_MAX_NOTES was raised 64→80 for the host
 * web-bridge MP3 translator (still fits one 2 KB flash page — see
 * storage.c's sizeof(StorageBlob) assert). STORAGE_SONG_NAME_LEN matches
 * the same incl.-NUL length convention as STORAGE_NOTE_TITLE_LEN /
 * STORAGE_CONTACT_NAME_LEN above rather than inventing a new one. */
#define STORAGE_MAX_SONGS         2u
#define STORAGE_SONG_MAX_NOTES    80u
#define STORAGE_SONG_NAME_LEN     16u

typedef struct {
  char title[STORAGE_NOTE_TITLE_LEN];
  char body[STORAGE_NOTE_BODY_LEN];
} StorageNote;

typedef struct {
  char name[STORAGE_CONTACT_NAME_LEN];
  char phone[STORAGE_CONTACT_PHONE_LEN];
} StorageContact;

/** Phase 7 (plan section 5.4 Settings params + section 5.9 PIN/auto-lock +
 *  section 5.5 uartset gate + section 5.3 segtime). Field-by-field mapping
 *  to app_settings.c's editable rows; see storage.c for the exact 32-byte
 *  layout/padding this must stay within (STORAGE_SETTINGS_RESERVED_BYTES,
 *  no STORAGE_VERSION bump allowed). */
typedef struct {
  /* Both 2-byte fields declared first and contiguous so no compiler
   * alignment padding sneaks between them and the 1-byte fields that
   * follow (storage.c's _Static_assert pins this struct to exactly 32 B
   * -- keeping the layout padding-free by construction, rather than
   * discovering a mismatch at compile time, is the deliberate reason for
   * this field order over the more "readable" grouping-by-topic order). */
  uint16_t ldr_threshold; /* raw 12-bit ADC domain, replaces the
                            * compile-time ANALOG_LDR_THRESHOLD_RAW once
                            * app_settings.c exists */
  uint16_t autolock_s;    /* idle seconds before auto-lock; 0 = disabled
                            * (plan section 5.9 "innovation") */
  uint8_t  icon_layout;   /* one of MENU_LAYOUT_6X1/3X2/2X3 (app_config.h)
                            * -- user-selectable override of the
                            * compile-time default */
  uint8_t  volume;        /* 0-100, last-saved buzzer volume, applied by
                            * Buzzer_Init() at boot instead of the
                            * compile-time BUZZER_DEFAULT_VOLUME_PCT once a
                            * settings save has happened */
  uint8_t  uart_settings_enable; /* 0/1: gates cmdparse.c's /setting-*
                                   * command per plan section 5.5 */
  uint8_t  segtime_enable; /* 0/1: 7-seg shows SEG_TIME (HH.MM from RTC)
                             * instead of the per-app mode (bonus 3, plan
                             * section 5.3) */
  uint8_t  pin_enabled;   /* 0/1: lock screen requires PIN entry vs
                            * any-key unlock (plan section 5.9) */
  char     pin[STORAGE_PIN_LEN]; /* 4 digits + NUL; only meaningful when
                                   * pin_enabled */
  /* Phase 8 (plan section 5.8 Settings params: "damage, zombie speed,
   * difficulty=spawn rate, starting plants"). Carved out of the same
   * `reserved` padding this struct already budgeted for, exactly the policy
   * storage.h's own file comment / master plan section 9.8 note 3
   * describes -- STORAGE_VERSION is NOT bumped. */
  uint8_t  pvz_damage;        /* HP removed per bullet hit */
  uint8_t  pvz_zombie_speed;  /* hundredths of a cell per game tick */
  uint8_t  pvz_difficulty;    /* ticks between zombie spawns; lower=harder */
  uint8_t  pvz_start_plants;  /* plant-select screen's pick count */
  uint8_t  reserved[32u - 18u]; /* pads the struct to exactly 32 bytes
                                  * (STORAGE_SETTINGS_RESERVED_BYTES) so
                                  * future settings fields can grow into
                                  * this space without another
                                  * STORAGE_VERSION bump -- same reserved-
                                  * budget policy this struct itself was
                                  * carved out of (master plan section
                                  * 9.8 note 3). 18 = the sum of the named
                                  * fields above (2+2+1+1+1+1+1+5+1+1+1+1). */
} StorageSettings;

/** Phase 8 (plan section 5.6 blob field "high scores", section 5.8 "0 lives
 *  -> game over jingle, high-score check"). Replaces the previously-zeroed
 *  `highscores_reserved[16]` placeholder with a real struct of the exact
 *  same size (see storage.c's `_Static_assert`) -- same
 *  reserve-now/populate-later policy as StorageSettings' own Phase 7
 *  upgrade (master plan section 9.8 note 3), STORAGE_VERSION unchanged. */
typedef struct {
  uint16_t score;       /* best score ever recorded */
  uint16_t survival_s;  /* survival time (seconds) of that same best run */
  uint8_t  reserved[16u - 4u]; /* pads to STORAGE_HIGHSCORES_RESERVED_BYTES */
} StorageHighscore;

/** Phase 9 (plan section 5.5 bonus protocol: "`/songup-{name}-{count}` then
 *  `count` lines `freq,ms`"). Matches buzzer.h's private per-note {freq,ms}
 *  layout exactly, but declared here (not buzzer.h) since storage.c/cmdparse.c
 *  need it and must not depend on buzzer.h for it. */
typedef struct {
  uint16_t freqHz;
  uint16_t ms;
} StorageSongNote;

/** One uploaded-song slot. `note_count == 0` means the slot is still empty
 *  (never uploaded into) -- Storage_GetSongCount() counts populated slots. */
typedef struct {
  char            name[STORAGE_SONG_NAME_LEN];
  uint8_t         note_count;
  uint8_t         reserved[3]; /* keeps notes[] 4-byte aligned; see
                                 * storage.c's _Static_assert for the exact
                                 * per-slot size this must stay within */
  StorageSongNote notes[STORAGE_SONG_MAX_NOTES];
} StorageSong;

/** Loads the RAM working copy from whichever of the two alternate flash
 *  pages holds the newest valid blob (or zero-initializes it if neither
 *  page is valid -- first-ever boot). Call once from main(), before
 *  osKernelStart() (plain flash reads, no HAL_FLASH unlock needed, same
 *  pre-kernel timing as Buzzer_Init()/Analog_Init()). */
void Storage_Init(void);

uint8_t Storage_GetNoteCount(void);
const StorageNote *Storage_GetNote(uint8_t idx);
/** Direct pointer into the live RAM copy, for in-place text_field editing
 *  (app_note.c). NULL if idx is out of range. */
StorageNote *Storage_GetNoteMutable(uint8_t idx);
/** Appends a zero-initialized note and returns its index, or 0xFF if
 *  STORAGE_MAX_NOTES is already reached. */
uint8_t Storage_AddNote(void);
/** Removes note `idx`, shifting later entries down by one. No-op if idx is
 *  out of range. */
void Storage_DeleteNote(uint8_t idx);

/** Read-only view of the live settings, for renderers that never mutate
 *  (app_lock.c reading autolock_s/pin_enabled/pin). Never NULL. */
const StorageSettings *Storage_GetSettings(void);
/** Direct pointer into the live RAM copy, for in-place editing
 *  (app_settings.c), same convention as Storage_GetNoteMutable(). Never
 *  NULL -- unlike notes/contacts there is exactly one settings record,
 *  always present (zero/default-initialized on first-ever boot). */
StorageSettings *Storage_GetSettingsMutable(void);

uint8_t Storage_GetContactCount(void);
const StorageContact *Storage_GetContact(uint8_t idx);
StorageContact *Storage_GetContactMutable(uint8_t idx);
uint8_t Storage_AddContact(void);
void Storage_DeleteContact(uint8_t idx);

/** Read-only view of the best-ever PvZ run, never NULL (zeroed until the
 *  first game-over that beats it). */
const StorageHighscore *Storage_GetHighscore(void);
/** Records `score`/`survival_s` as the new best IFF it beats the current
 *  one (strictly greater score; survival_s is informational only, not a
 *  tiebreaker -- plan section 5.8 only ever talks about score for the
 *  high-score check). Returns 1 if it was actually a new best (caller,
 *  app_pvz.c, uses this to decide whether to Storage_RequestSave() and show
 *  a "New high score!" line), 0 otherwise. */
uint8_t Storage_SetHighscoreIfBetter(uint16_t score, uint16_t survival_s);

/** Number of populated song slots, 0..STORAGE_MAX_SONGS. Always contiguous
 *  from slot 0 (Storage_UploadSong()'s round-robin cursor always fills in
 *  slot order, so "populated" never has a gap before an empty slot). */
uint8_t Storage_GetSongCount(void);
/** Read-only view of song slot `idx`, or NULL if idx >= Storage_GetSongCount()
 *  (i.e. that slot has never been uploaded into). */
const StorageSong *Storage_GetSong(uint8_t idx);
/** Uploads `notes[0..note_count)` under `name` into the next round-robin
 *  slot (0, then 1, then wraps back to 0 once STORAGE_MAX_SONGS uploads have
 *  happened, overwriting the oldest). Unlike Storage_AddNote()/AddContact(),
 *  this never "runs out" of room and never returns a failure sentinel --
 *  round-robin always has a slot to (over)write. `note_count` beyond
 *  STORAGE_SONG_MAX_NOTES is clamped (excess notes dropped, not
 *  buffer-overflowed); `name` longer than STORAGE_SONG_NAME_LEN-1 is
 *  truncated. Returns the slot index actually written (0..STORAGE_MAX_SONGS-1)
 *  so the caller (cmdparse.c) can ack `OK stored as #k`. Does NOT call
 *  Storage_RequestSave() itself -- caller's responsibility, same convention
 *  as Storage_AddNote()/Storage_AddContact(). */
uint8_t Storage_UploadSong(const char *name, const StorageSongNote *notes, uint8_t note_count);

/** Requests a flash save (plan section 5.6 save triggers: "leaving
 *  Note/Contact editor with changes, Settings change, song upload, new
 *  high score, and on /lock"). Non-blocking, safe from app_task context --
 *  just wakes storage_task, which snapshots the current RAM state and
 *  performs the actual erase/program (never from ISR, scheduler suspended
 *  around the HAL_FLASH calls, exactly per plan section 5.6). Multiple
 *  requests before storage_task gets to run are coalesced into one save. */
void Storage_RequestSave(void);

/** storage_task's body (plan section 4.3: "waits on save-request flag;
 *  performs flash erase/write"). Blocks until a save is requested, then
 *  saves once, then returns -- main.c's StartStorageTask() wraps this in
 *  the task's own for(;;) loop, matching ui_task/app_task's existing
 *  thin-wrapper convention. */
void Storage_WaitAndSave(void);

#endif /* STORAGE_H */
