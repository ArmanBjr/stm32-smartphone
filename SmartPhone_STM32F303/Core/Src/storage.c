/**
 * @file storage.c
 * @brief See storage.h. Flash blob layout/CRC/erase-program mechanics per
 *        master plan section 5.6.
 */
#include "storage.h"
#include "app_config.h"
#include "serial.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f3xx_hal.h"
#include <string.h>
#include <stddef.h>

/* Reserved placeholder budget for payload sections not yet owned by a
 * built subsystem -- see storage.h's file comment. Sizes taken from plan
 * section 5.6's own figures: "settings ~= 32 B", "scores 16 B".
 *
 * Phase 9: songs no longer uses a flat reserved-byte placeholder (see
 * storage.h's file comment for why StorageSong needed more room than the
 * old 512-byte figure budgeted -- names weren't in that original figure). */
#define STORAGE_SETTINGS_RESERVED_BYTES    32u
#define STORAGE_HIGHSCORES_RESERVED_BYTES  16u

#define STORAGE_MAGIC      0x50484F4Eu
#define STORAGE_VERSION    1u
#define STORAGE_PAGE_SIZE  0x800u /* 2 KB, confirmed against
                                    * stm32f3xx_hal_flash_ex.h's
                                    * FLASH_PAGE_SIZE */
#define STORAGE_PAGE_A     0x0803F000u
#define STORAGE_PAGE_B     0x0803F800u
#define STORAGE_PAGE_NONE  0u

#define STORAGE_SAVE_FLAG  0x1u

typedef struct {
  uint8_t        note_count;
  uint8_t        contact_count;
  /* Phase 9: one byte of the old reserved0[2] padding repurposed for the
   * songs round-robin write cursor (same "carve from reserved padding, no
   * STORAGE_VERSION bump" policy as StorageSettings' pvz_* fields) -- the
   * other byte stays reserved, so this pair is still 4-byte aligned before
   * notes[] starts, unchanged from the original layout. */
  uint8_t        next_song_slot; /* 0..STORAGE_MAX_SONGS-1, next slot
                                   * Storage_UploadSong() will write */
  uint8_t        reserved0;
  StorageNote    notes[STORAGE_MAX_NOTES];
  StorageContact contacts[STORAGE_MAX_CONTACTS];
  StorageSettings settings;   /* Phase 7: real struct, was zeroed padding.
                                * _Static_assert below pins it to exactly
                                * STORAGE_SETTINGS_RESERVED_BYTES so the
                                * blob's total size (and therefore
                                * STORAGE_VERSION) never changes. */
  StorageSong    songs[STORAGE_MAX_SONGS]; /* Phase 9: real struct array,
                                              * was songs_reserved[512] --
                                              * see storage.h's file comment
                                              * for why this grows the
                                              * payload rather than fitting
                                              * the old placeholder exactly. */
  StorageHighscore highscore; /* Phase 8: real struct, was zeroed padding.
                                * _Static_assert below pins it to exactly
                                * STORAGE_HIGHSCORES_RESERVED_BYTES. */
} StoragePayload;

_Static_assert(sizeof(StorageSettings) == STORAGE_SETTINGS_RESERVED_BYTES,
               "StorageSettings must exactly fill the reserved 32 B "
               "budget (storage.c) -- adjust its padding, not this macro");
_Static_assert(sizeof(StorageHighscore) == STORAGE_HIGHSCORES_RESERVED_BYTES,
               "StorageHighscore must exactly fill the reserved 16 B "
               "budget (storage.c) -- adjust its padding, not this macro");
_Static_assert(sizeof(StorageSong) == STORAGE_SONG_NAME_LEN + 4u +
               (STORAGE_SONG_MAX_NOTES * 4u),
               "StorageSong layout drifted from its documented "
               "name+count+reserved+notes byte count (storage.c)");

typedef struct {
  uint32_t       magic;
  uint32_t       version;
  /* Alternate-page wear-leveling (plan section 5.6: "write to alternate
   * page each save ... newest valid CRC wins on boot"). CRC alone proves
   * a page's *integrity*, not which of two valid pages is more recent --
   * `seq` is the monotonically-incrementing counter that actually answers
   * "newest": on boot, among pages with a valid magic/version/crc32, the
   * higher `seq` wins. This is an explicit, documented interpretation of
   * the plan's shorthand, not a guess -- CRC-only comparison has no way to
   * pick between two independently-valid pages. */
  uint32_t       seq;
  uint32_t       len; /* sizeof(StoragePayload), a lightweight extra sanity
                        * check alongside magic/version before trusting a
                        * page's CRC */
  StoragePayload payload;
  uint32_t       crc32;
} StorageBlob;

/* Fits in one 2 KB page: budget check at compile time, not a runtime
 * guess. If this ever fails, either trim a *_RESERVED_BYTES figure above
 * or revisit the plan's own per-field budget. */
_Static_assert(sizeof(StorageBlob) <= STORAGE_PAGE_SIZE,
               "StorageBlob exceeds one flash page (storage.c)");

/* Live RAM working copy -- also the exact bytes flashed on save, so
 * Storage_WaitAndSave() has no separate snapshot/copy step. Static (not a
 * stack local): storage_task's planned stack budget (plan section 4.3:
 * "256 w" = 1024 bytes) has no room for a ~1.8 KB local of this size. */
static StorageBlob s_blob;
static uint32_t    s_active_page; /* which flash page currently holds the
                                    * valid copy matching s_blob; write
                                    * target on next save is the *other*
                                    * page. STORAGE_PAGE_NONE until the
                                    * first successful save (fresh part). */

static osThreadId_t s_storage_task_id;
static volatile uint8_t s_save_pending; /* covers a Storage_RequestSave()
                                          * that races ahead of
                                          * s_storage_task_id being known
                                          * (pre-kernel or very first
                                          * scheduling tick) */

/* Standard CRC-32 (IEEE 802.3 polynomial 0xEDB88320), bit-by-bit -- no
 * table needed at this data size (~1.8 KB, saved rarely, not a hot path),
 * and avoids pulling in the HAL CRC peripheral (would need a CubeMX/.ioc
 * change out of this phase's scope). */
static uint32_t storage_crc32(const uint8_t *data, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8u; bit++) {
      uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

static uint8_t storage_page_valid(uint32_t page_addr)
{
  const StorageBlob *b = (const StorageBlob *)page_addr;
  if (b->magic != STORAGE_MAGIC || b->version != STORAGE_VERSION ||
      b->len != sizeof(StoragePayload)) {
    return 0u;
  }
  uint32_t crc = storage_crc32((const uint8_t *)&b->payload, sizeof(StoragePayload));
  return (crc == b->crc32) ? 1u : 0u;
}

static void storage_zero_init(void)
{
  memset(&s_blob, 0, sizeof(s_blob));
  s_blob.magic = STORAGE_MAGIC;
  s_blob.version = STORAGE_VERSION;
  s_blob.len = sizeof(StoragePayload);
  s_blob.seq = 0u;

  /* Phase 7: first-ever-boot settings defaults (plan section 5.4/5.5/5.9
   * params). Everything else in the memset'd blob is correctly 0/off by
   * default (uart_settings_enable=0 i.e. OFF per spec, segtime_enable=0,
   * pin_enabled=0 i.e. no PIN, pin="" ). */
  s_blob.payload.settings.ldr_threshold = SETTINGS_DEFAULT_LDR_THRESHOLD;
  s_blob.payload.settings.autolock_s    = SETTINGS_DEFAULT_AUTOLOCK_S;
  s_blob.payload.settings.icon_layout   = MENU_LAYOUT;
  s_blob.payload.settings.volume        = BUZZER_DEFAULT_VOLUME_PCT;

  /* Phase 8: first-ever-boot PvZ settings defaults (plan section 5.8
   * params). highscore stays all-zero (memset above) -- "no run yet". */
  s_blob.payload.settings.pvz_damage        = PVZ_DEFAULT_DAMAGE;
  s_blob.payload.settings.pvz_zombie_speed  = PVZ_DEFAULT_ZOMBIE_SPEED;
  s_blob.payload.settings.pvz_difficulty    = PVZ_DEFAULT_DIFFICULTY;
  s_blob.payload.settings.pvz_start_plants  = PVZ_DEFAULT_START_PLANTS;

  /* First-ever save target: pretend page B is "currently valid" so the
   * alternate-page rule picks page A for the very first write. */
  s_active_page = STORAGE_PAGE_B;
}

void Storage_Init(void)
{
  uint8_t a_valid = storage_page_valid(STORAGE_PAGE_A);
  uint8_t b_valid = storage_page_valid(STORAGE_PAGE_B);

  if (!a_valid && !b_valid) {
    LOG("[STORAGE] no valid page found, starting fresh");
    storage_zero_init();
    return;
  }

  uint32_t winner = STORAGE_PAGE_A;
  if (a_valid && b_valid) {
    const StorageBlob *ba = (const StorageBlob *)STORAGE_PAGE_A;
    const StorageBlob *bb = (const StorageBlob *)STORAGE_PAGE_B;
    winner = (bb->seq > ba->seq) ? STORAGE_PAGE_B : STORAGE_PAGE_A;
  } else {
    winner = a_valid ? STORAGE_PAGE_A : STORAGE_PAGE_B;
  }

  memcpy(&s_blob, (const void *)winner, sizeof(s_blob));
  s_active_page = winner;
  LOG("[STORAGE] loaded page 0x%08lX seq=%lu notes=%u contacts=%u",
      (unsigned long)winner, (unsigned long)s_blob.seq,
      (unsigned)s_blob.payload.note_count, (unsigned)s_blob.payload.contact_count);
}

uint8_t Storage_GetNoteCount(void)
{
  return s_blob.payload.note_count;
}

const StorageNote *Storage_GetNote(uint8_t idx)
{
  return (idx < s_blob.payload.note_count) ? &s_blob.payload.notes[idx] : NULL;
}

StorageNote *Storage_GetNoteMutable(uint8_t idx)
{
  return (idx < s_blob.payload.note_count) ? &s_blob.payload.notes[idx] : NULL;
}

uint8_t Storage_AddNote(void)
{
  if (s_blob.payload.note_count >= STORAGE_MAX_NOTES) {
    return 0xFFu;
  }
  uint8_t idx = s_blob.payload.note_count;
  memset(&s_blob.payload.notes[idx], 0, sizeof(StorageNote));
  s_blob.payload.note_count++;
  return idx;
}

void Storage_DeleteNote(uint8_t idx)
{
  if (idx >= s_blob.payload.note_count) {
    return;
  }
  for (uint8_t i = idx; i < (uint8_t)(s_blob.payload.note_count - 1u); i++) {
    s_blob.payload.notes[i] = s_blob.payload.notes[i + 1u];
  }
  s_blob.payload.note_count--;
}

const StorageSettings *Storage_GetSettings(void)
{
  return &s_blob.payload.settings;
}

StorageSettings *Storage_GetSettingsMutable(void)
{
  return &s_blob.payload.settings;
}

uint8_t Storage_GetContactCount(void)
{
  return s_blob.payload.contact_count;
}

const StorageContact *Storage_GetContact(uint8_t idx)
{
  return (idx < s_blob.payload.contact_count) ? &s_blob.payload.contacts[idx] : NULL;
}

StorageContact *Storage_GetContactMutable(uint8_t idx)
{
  return (idx < s_blob.payload.contact_count) ? &s_blob.payload.contacts[idx] : NULL;
}

uint8_t Storage_AddContact(void)
{
  if (s_blob.payload.contact_count >= STORAGE_MAX_CONTACTS) {
    return 0xFFu;
  }
  uint8_t idx = s_blob.payload.contact_count;
  memset(&s_blob.payload.contacts[idx], 0, sizeof(StorageContact));
  s_blob.payload.contact_count++;
  return idx;
}

void Storage_DeleteContact(uint8_t idx)
{
  if (idx >= s_blob.payload.contact_count) {
    return;
  }
  for (uint8_t i = idx; i < (uint8_t)(s_blob.payload.contact_count - 1u); i++) {
    s_blob.payload.contacts[i] = s_blob.payload.contacts[i + 1u];
  }
  s_blob.payload.contact_count--;
}

const StorageHighscore *Storage_GetHighscore(void)
{
  return &s_blob.payload.highscore;
}

uint8_t Storage_SetHighscoreIfBetter(uint16_t score, uint16_t survival_s)
{
  if (score <= s_blob.payload.highscore.score) {
    return 0u;
  }
  s_blob.payload.highscore.score = score;
  s_blob.payload.highscore.survival_s = survival_s;
  return 1u;
}

uint8_t Storage_RecordAchievements(uint16_t survival_total_s, uint16_t kills)
{
  StorageHighscore *hs = &s_blob.payload.highscore;
  uint8_t newly = 0u;

  if (survival_total_s > hs->best_survival_total_s) {
    hs->best_survival_total_s = survival_total_s;
  }
  if (kills > hs->best_kills) {
    hs->best_kills = kills;
  }

  if (survival_total_s >= 180u && (hs->achievements & 0x01u) == 0u) {
    hs->achievements |= 0x01u;
    newly |= 0x01u;
  }
  if (kills >= 10u && (hs->achievements & 0x02u) == 0u) {
    hs->achievements |= 0x02u;
    newly |= 0x02u;
  }
  return newly;
}

uint8_t Storage_GetSongCount(void)
{
  uint8_t n = 0u;
  for (uint8_t i = 0; i < STORAGE_MAX_SONGS; i++) {
    if (s_blob.payload.songs[i].note_count == 0u) {
      break; /* round-robin always fills in slot order -- first empty slot
              * means every slot after it is empty too */
    }
    n++;
  }
  return n;
}

const StorageSong *Storage_GetSong(uint8_t idx)
{
  return (idx < Storage_GetSongCount()) ? &s_blob.payload.songs[idx] : NULL;
}

uint8_t Storage_UploadSong(const char *name, const StorageSongNote *notes, uint8_t note_count)
{
  if (name == NULL || notes == NULL || note_count == 0u) {
    return 0u; /* defensive -- cmdparse rejects empty songs before calling;
                * returning slot 0 here is only a sentinel for a no-op write
                * that should never happen on the happy path. */
  }
  if (note_count > STORAGE_SONG_MAX_NOTES) {
    note_count = STORAGE_SONG_MAX_NOTES;
  }

  uint8_t slot = s_blob.payload.next_song_slot;
  StorageSong *dst = &s_blob.payload.songs[slot];

  memset(dst, 0, sizeof(*dst));
  size_t name_len = strlen(name);
  if (name_len >= STORAGE_SONG_NAME_LEN) {
    name_len = STORAGE_SONG_NAME_LEN - 1u;
  }
  memcpy(dst->name, name, name_len);
  dst->name[name_len] = '\0';
  memcpy(dst->notes, notes, (size_t)note_count * sizeof(StorageSongNote));
  dst->note_count = note_count;

  s_blob.payload.next_song_slot = (uint8_t)((slot + 1u) % STORAGE_MAX_SONGS);
  return slot;
}

void Storage_RequestSave(void)
{
  s_save_pending = 1u;
  if (s_storage_task_id != NULL) {
    (void)osThreadFlagsSet(s_storage_task_id, STORAGE_SAVE_FLAG);
  }
}

void Storage_WaitAndSave(void)
{
  if (s_storage_task_id == NULL) {
    s_storage_task_id = osThreadGetId();
  }

  if (!s_save_pending) {
    (void)osThreadFlagsWait(STORAGE_SAVE_FLAG, osFlagsWaitAny, osWaitForever);
  }
  s_save_pending = 0u;

  uint32_t target_page = (s_active_page == STORAGE_PAGE_A) ? STORAGE_PAGE_B : STORAGE_PAGE_A;

  /* Plan section 5.6: "Executed only in storage_task (never ISR),
   * scheduler suspended around HAL_FLASH erase/program of the region."
   * The CRC/seq stamping of s_blob is included in the suspended window
   * too (not just the erase/program calls) so the whole
   * snapshot-then-write is atomic against app_task concurrently mutating
   * s_blob via Storage_Set()/Add()/Delete() -- app_task is higher priority
   * than storage_task (plan section 4.3: normal vs low) and could
   * otherwise preempt mid-CRC-computation. */
  vTaskSuspendAll();

  s_blob.seq++;
  s_blob.crc32 = storage_crc32((const uint8_t *)&s_blob.payload, sizeof(StoragePayload));

  HAL_FLASH_Unlock();
  FLASH_EraseInitTypeDef erase = {
    .TypeErase   = FLASH_TYPEERASE_PAGES,
    .PageAddress = target_page,
    .NbPages     = 1u,
  };
  uint32_t page_error = 0u;
  HAL_StatusTypeDef erase_status = HAL_FLASHEx_Erase(&erase, &page_error);

  HAL_StatusTypeDef prog_status = HAL_OK;
  if (erase_status == HAL_OK) {
    const uint16_t *src = (const uint16_t *)&s_blob;
    uint32_t nhalfwords = sizeof(s_blob) / 2u; /* exact: every field above
                                                  * is 2/4-byte sized, no
                                                  * odd-byte remainder */
    for (uint32_t i = 0; i < nhalfwords && prog_status == HAL_OK; i++) {
      prog_status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                                       target_page + (i * 2u), src[i]);
    }
  }
  HAL_FLASH_Lock();

  (void)xTaskResumeAll();

  if (erase_status == HAL_OK && prog_status == HAL_OK) {
    s_active_page = target_page;
    LOG("[STORAGE] saved page 0x%08lX seq=%lu", (unsigned long)target_page,
        (unsigned long)s_blob.seq);
  } else {
    LOG("[STORAGE] save FAILED (erase=%d prog=%d)", (int)erase_status, (int)prog_status);
  }
}
