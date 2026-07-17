/**
 * @file buzzer.c
 * @brief See buzzer.h. Source of truth: master plan section 5.3 + section
 *        2.3's TIM3/TIM8 rows (exact PSC/ARR values, duty-math formula).
 */
#include "buzzer.h"
#include "app_config.h"
#include "events.h"
#include "storage.h"
#include "stm32f3xx_hal.h"
#include <stddef.h>
#include <string.h>

/* buzzer.c owns TIM8 directly -- see buzzer.h's header comment for why this
 * differs from ui.c's IWDG-callback approach. htim8 itself is still
 * MX_TIM8_Init()'d and HAL_TIM_MspPostInit()'d by main.c (peripheral
 * clock/GPIO bring-up stays CubeMX-owned); this extern mirrors the existing
 * precedent in stm32f3xx_it.c (`extern UART_HandleTypeDef huart1;`) for a
 * main.c-owned handle a non-main.c file needs direct register access to. */
extern TIM_HandleTypeDef htim8;

typedef struct {
  uint16_t freqHz; /* 0 = rest (silence for this note's duration) */
  uint16_t ms;
} Note;

typedef struct {
  const Note *notes;
  uint16_t     count;
  const char  *name;
} Melody;

/* Equal-temperament note frequencies (Hz). Values match Songs/Pitches/pitches.h
 * (HiBit Arduino melody source). Only macros referenced by the built-in
 * melody tables below are defined here. */
#define NOTE_GS3  208u
#define NOTE_AS3  233u
#define NOTE_B3   247u
#define NOTE_C4   262u
#define NOTE_CS4  277u
#define NOTE_D4   294u
#define NOTE_DS4  311u
#define NOTE_E4   330u
#define NOTE_F4   349u
#define NOTE_FS4  370u
#define NOTE_G4   392u
#define NOTE_GS4  415u
#define NOTE_A4   440u
#define NOTE_AS4  466u
#define NOTE_B4   494u
#define NOTE_C5   523u
#define NOTE_CS5  554u
#define NOTE_D5   587u
#define NOTE_DS5  622u
#define NOTE_E5   659u
#define NOTE_F5   698u
#define NOTE_G5   784u
#define NOTE_GS5  831u
#define NOTE_A5   880u
#define NOTE_AS5  932u
#define NOTE_C6  1047u
#define NOTE_CS6 1109u
#define NOTE_DS6 1245u
#define NOTE_F6  1397u
#define NOTE_REST   0u

/* "Twinkle Twinkle Little Star", opening phrase. */
static const Note s_melody_twinkle[] = {
  { NOTE_C4, 400 }, { NOTE_C4, 400 }, { NOTE_G4, 400 }, { NOTE_G4, 400 },
  { NOTE_A4, 400 }, { NOTE_A4, 400 }, { NOTE_G4, 800 },
  { NOTE_REST, 200 },
  { NOTE_F4, 400 }, { NOTE_F4, 400 }, { NOTE_E4, 400 }, { NOTE_E4, 400 },
  { NOTE_D4, 400 }, { NOTE_D4, 400 }, { NOTE_C4, 800 },
};

/* Simple ascending/descending scale run -- deliberately the "odd one out"
 * (not a known tune) so pause/next/prev are easy to tell apart by ear from
 * the other three during the hardware demo. */
static const Note s_melody_scale[] = {
  { NOTE_C4, 200 }, { NOTE_D4, 200 }, { NOTE_E4, 200 }, { NOTE_F4, 200 },
  { NOTE_G4, 200 }, { NOTE_A4, 200 }, { NOTE_B4, 200 }, { NOTE_C5, 400 },
  { NOTE_REST, 150 },
  { NOTE_C5, 200 }, { NOTE_B4, 200 }, { NOTE_A4, 200 }, { NOTE_G4, 200 },
  { NOTE_F4, 200 }, { NOTE_E4, 200 }, { NOTE_D4, 200 }, { NOTE_C4, 400 },
};

/* "Happy Birthday", opening phrase. */
static const Note s_melody_birthday[] = {
  { NOTE_C4, 250 }, { NOTE_C4, 150 }, { NOTE_D4, 400 }, { NOTE_C4, 400 },
  { NOTE_F4, 400 }, { NOTE_E4, 800 },
  { NOTE_REST, 200 },
  { NOTE_C4, 250 }, { NOTE_C4, 150 }, { NOTE_D4, 400 }, { NOTE_C4, 400 },
  { NOTE_G4, 400 }, { NOTE_F4, 800 },
};

/* A short two-tone "alert" jingle, distinct in character from the three
 * melodic tunes above (rapid alternation rather than a sung phrase). */
static const Note s_melody_alert[] = {
  { NOTE_G4, 120 }, { NOTE_C5, 120 }, { NOTE_G4, 120 }, { NOTE_C5, 120 },
  { NOTE_REST, 150 },
  { NOTE_G4, 120 }, { NOTE_C5, 120 }, { NOTE_G4, 120 }, { NOTE_C5, 300 },
};

/* Songs/ folder (HiBit .ino melodies): converted via tools/convert_songs.py
 * using the Arduino timing model (1000/duration note length + 30% gap). */
#include "buzzer_new_songs.inc"

/* Phase 8 hotfix (bug report "music is not plants vs. zombies. make it
 * moreaccurate"): PVZ_BGM_MELODY_IDX (app_config.h) previously pointed at
 * "Bones" above, a generic track with no relation to the game. This is an
 * original short loop (not a transcription of any copyrighted score) built
 * to *feel* like the genre's bouncy, whimsical garden-march character --
 * repeating staccato triad line with a call-and-response second phrase and
 * a held final note, in a jaunty major key. */
static const Note s_melody_pvz[] = {
  { NOTE_E4, 150 }, { NOTE_G4, 150 }, { NOTE_C5, 150 }, { NOTE_G4, 150 },
  { NOTE_E4, 200 }, { NOTE_REST, 100 },
  { NOTE_D4, 150 }, { NOTE_F4, 150 }, { NOTE_A4, 150 }, { NOTE_F4, 150 },
  { NOTE_D4, 200 }, { NOTE_REST, 100 },
  { NOTE_E4, 150 }, { NOTE_G4, 150 }, { NOTE_C5, 150 }, { NOTE_G4, 150 },
  { NOTE_E4, 150 }, { NOTE_C5, 150 }, { NOTE_G4, 300 },
  { NOTE_REST, 150 },
  { NOTE_C5, 180 }, { NOTE_B4, 180 }, { NOTE_A4, 180 }, { NOTE_G4, 500 },
};

#define MELODY_COUNT 14u
static const Melody s_melodies[MELODY_COUNT] = {
  { s_melody_twinkle,    sizeof(s_melody_twinkle)    / sizeof(Note), "Twinkle"    },
  { s_melody_scale,      sizeof(s_melody_scale)      / sizeof(Note), "Scale"      },
  { s_melody_birthday,   sizeof(s_melody_birthday)   / sizeof(Note), "Birthday"   },
  { s_melody_alert,      sizeof(s_melody_alert)      / sizeof(Note), "Alert"      },
  { s_melody_got,        sizeof(s_melody_got)        / sizeof(Note), "GoT"        },
  { s_melody_harry,      sizeof(s_melody_harry)      / sizeof(Note), "Harry P."   },
  { s_melody_bones,      sizeof(s_melody_bones)      / sizeof(Note), "Bones"      },
  { s_melody_pink,       sizeof(s_melody_pink)       / sizeof(Note), "Pink Pan."  },
  { s_melody_pirates,    sizeof(s_melody_pirates)    / sizeof(Note), "Pirates"    },
  { s_melody_shape,      sizeof(s_melody_shape)      / sizeof(Note), "Shape of U" },
  { s_melody_starwars,   sizeof(s_melody_starwars)   / sizeof(Note), "Star Wars"  },
  { s_melody_dre,        sizeof(s_melody_dre)        / sizeof(Note), "Still Dre"  },
  { s_melody_godfather,  sizeof(s_melody_godfather)  / sizeof(Note), "Godfather"  },
  { s_melody_pvz,        sizeof(s_melody_pvz)        / sizeof(Note), "PvZ Theme"  },
};

typedef enum {
  BUZZER_IDLE = 0,   /* no melody ever started (or one finished and wasn't restarted) */
  BUZZER_PLAYING,
  BUZZER_PAUSED,
  BUZZER_PIANO,      /* Phase 9: continuous piano tone, melody timing frozen */
} BuzzerState;

static volatile BuzzerState s_state = BUZZER_IDLE;
static volatile uint8_t  s_melody_idx = 0;
static volatile uint16_t s_note_idx = 0;
static volatile uint16_t s_remain_ms = 0;
static volatile uint16_t s_cur_note_freq = 0; /* cached for SFX restore */
static volatile uint8_t  s_volume = 0;        /* 0-100 */
static volatile uint16_t s_sfx_remain_ms = 0; /* >0 while an SFX override is sounding */
static volatile uint16_t s_piano_freq = 0;    /* Phase 9: last Buzzer_PianoNoteOn() freq, 0 if off */

/* Phase 9 (plan section 6: song_upload.py): the melody index space is
 * extended past the built-in MELODY_COUNT tracks to also cover uploaded
 * songs (Storage_GetSongCount() of them, indices MELODY_COUNT..
 * MELODY_COUNT+count-1) -- see buzzer_total_melody_count(). s_active_melody
 * points at whichever table is actually loaded (a fixed built-in Melody, or
 * s_uploaded_melody below) so Buzzer_TickISR()/Buzzer_SeekPercent() don't
 * need an idx>=MELODY_COUNT branch on every tick. */
static const Melody *s_active_melody = NULL;

/* Phase 9: shadow copy of the uploaded song currently loaded into
 * s_uploaded_melody -- copied out of Storage's static blob at
 * Buzzer_PlayMelody() time (not read live from Storage on every tick) so a
 * later Storage_UploadSong() onto the same round-robin slot (e.g. user
 * uploads a 3rd song while an earlier one is still playing) cannot tear the
 * note data Buzzer_TickISR() is reading mid-note. */
static Note s_uploaded_notes[STORAGE_SONG_MAX_NOTES];
static char s_uploaded_name[STORAGE_SONG_NAME_LEN];
static Melody s_uploaded_melody;

static uint8_t buzzer_total_melody_count(void)
{
  return (uint8_t)(MELODY_COUNT + Storage_GetSongCount());
}

/* Phase 5 (plan section 5.4 pot-seek): elapsed time into the loaded melody,
 * plus a total-duration cache computed once in Buzzer_Init() (summed from
 * each melody's fixed Note table, so it never needs recomputing). */
static volatile uint32_t s_elapsed_ms = 0;
static uint32_t s_melody_total_ms[MELODY_COUNT];

/* Buzzer math @1 MHz counter clock (plan section 2.3): ARR = 1e6/f - 1;
 * continuous 0-100 volume: CCR = (ARR/2) * vol^2 / 10000 (perceptual-ish
 * taper). freqHz==0 (rest) special-cased to silence without touching ARR
 * (avoids a divide-by-zero; the previous note's ARR is harmless to leave
 * loaded since CCR=0 makes it inaudible regardless). */
static void buzzer_apply_freq_and_volume(uint16_t freqHz)
{
  if (freqHz == 0u) {
    __HAL_TIM_SET_COMPARE(&htim8, TIM_CHANNEL_1, 0u);
    return;
  }
  uint32_t arr = (1000000u / freqHz) - 1u;
  __HAL_TIM_SET_AUTORELOAD(&htim8, arr);
  uint32_t vol = s_volume;
  uint32_t ccr = ((arr / 2u) * vol * vol) / 10000u;
  __HAL_TIM_SET_COMPARE(&htim8, TIM_CHANNEL_1, ccr);
}

static void buzzer_apply_note(const Note *n)
{
  s_cur_note_freq = n->freqHz;
  s_remain_ms = n->ms;
  buzzer_apply_freq_and_volume(n->freqHz);
}

void Buzzer_Init(void)
{
  s_state = BUZZER_IDLE;
  s_melody_idx = 0;
  s_note_idx = 0;
  s_remain_ms = 0;
  s_cur_note_freq = 0;
  s_sfx_remain_ms = 0;
  s_elapsed_ms = 0;
  s_piano_freq = 0;
  s_active_melody = NULL;
  s_volume = BUZZER_DEFAULT_VOLUME_PCT;
  for (uint8_t i = 0; i < MELODY_COUNT; i++) {
    uint32_t total = 0;
    for (uint16_t n = 0; n < s_melodies[i].count; n++) {
      total += s_melodies[i].notes[n].ms;
    }
    s_melody_total_ms[i] = total;
  }
  HAL_TIM_PWM_Start(&htim8, TIM_CHANNEL_1); /* continuous; per-note ARR/CCR
                                              * pokes below modulate it --
                                              * AutoReloadPreload is DISABLE
                                              * (MX_TIM8_Init()), so writes
                                              * take effect immediately. */
  __HAL_TIM_SET_COMPARE(&htim8, TIM_CHANNEL_1, 0u); /* silent until a melody/SFX plays */
}

void Buzzer_TickISR(void)
{
  if (s_state == BUZZER_PIANO) {
    /* Phase 9: piano tone is a continuous, ISR-driven-timing-free hold --
     * Buzzer_PianoNoteOn()/Off() (app_task, or serial.c's ISR fast path)
     * own the PWM registers directly. No melody/SFX timing applies while
     * this state is active (mirrors the early-return SFX branch below,
     * which freezes melody timing for its own, much shorter, override). */
    return;
  }

  if (s_sfx_remain_ms > 0u) {
    s_sfx_remain_ms--;
    if (s_sfx_remain_ms == 0u) {
      /* Restore whatever should be sounding underneath the SFX. */
      if (s_state == BUZZER_PLAYING) {
        buzzer_apply_freq_and_volume(s_cur_note_freq);
      } else {
        __HAL_TIM_SET_COMPARE(&htim8, TIM_CHANNEL_1, 0u);
      }
    }
    return; /* melody timing is frozen while an SFX override is active */
  }

  if (s_state != BUZZER_PLAYING) {
    return;
  }
  s_elapsed_ms++; /* Phase 5: 1 kHz tick == 1 ms/call while actually playing */
  if (s_remain_ms > 0u) {
    s_remain_ms--;
    return;
  }

  const Melody *m = s_active_melody;
  s_note_idx++;
  if (s_note_idx >= m->count) {
    s_state = BUZZER_IDLE;
    __HAL_TIM_SET_COMPARE(&htim8, TIM_CHANNEL_1, 0u);
    /* Event_Post() is ISR-safe (0-timeout, never blocks -- events.h's own
     * contract), so posting straight from this ISR is fine. Plan section
     * 5.3: "at end: post EV_SONG_END (music app auto-nexts)." No Music
     * app exists yet to consume it (Phase 5); harmless to post regardless
     * -- app_task just drops it on the floor via the default event-switch
     * case, same as any other currently-unhandled EventType. */
    Event ev = { .type = (uint8_t)EV_SONG_END, .a = s_melody_idx, .b = 0, .c = 0 };
    Event_Post(&ev);
    return;
  }
  buzzer_apply_note(&m->notes[s_note_idx]);
}

void Buzzer_PlayMelody(uint8_t idx)
{
  if (idx >= buzzer_total_melody_count()) {
    return;
  }
  __disable_irq();
  if (idx < MELODY_COUNT) {
    s_active_melody = &s_melodies[idx];
  } else {
    /* Phase 9: uploaded song -- shadow-copy out of Storage's static blob
     * (see s_uploaded_notes' comment above for why a bare pointer into
     * Storage isn't used instead). */
    const StorageSong *song = Storage_GetSong((uint8_t)(idx - MELODY_COUNT));
    if (song == NULL) {
      __enable_irq();
      return; /* Storage_GetSongCount() raced down between the bounds check
               * above and here -- cannot happen from app_task (single
               * cooperative task owns both Storage_UploadSong() and this
               * call), defensive only. */
    }
    uint8_t n = song->note_count;
    if (n > STORAGE_SONG_MAX_NOTES) {
      n = STORAGE_SONG_MAX_NOTES;
    }
    for (uint8_t i = 0; i < n; i++) {
      s_uploaded_notes[i].freqHz = song->notes[i].freqHz;
      s_uploaded_notes[i].ms     = song->notes[i].ms;
    }
    memcpy(s_uploaded_name, song->name, STORAGE_SONG_NAME_LEN);
    s_uploaded_melody.notes = s_uploaded_notes;
    s_uploaded_melody.count = n;
    s_uploaded_melody.name  = s_uploaded_name;
    s_active_melody = &s_uploaded_melody;
  }
  s_melody_idx = idx;
  s_note_idx = 0;
  s_elapsed_ms = 0;
  s_state = BUZZER_PLAYING;
  buzzer_apply_note(&s_active_melody->notes[0]);
  __enable_irq();
}

void Buzzer_Stop(void)
{
  __disable_irq();
  s_state = BUZZER_IDLE;
  s_sfx_remain_ms = 0;
  s_remain_ms = 0;
  s_note_idx = 0;
  s_elapsed_ms = 0;
  s_cur_note_freq = 0;
  __HAL_TIM_SET_COMPARE(&htim8, TIM_CHANNEL_1, 0u);
  __enable_irq();
}

void Buzzer_Pause(void)
{
  __disable_irq();
  if (s_state == BUZZER_PLAYING) {
    s_state = BUZZER_PAUSED;
    __HAL_TIM_SET_COMPARE(&htim8, TIM_CHANNEL_1, 0u); /* freeze index (s_note_idx/s_remain_ms untouched), just go silent */
  }
  __enable_irq();
}

void Buzzer_Resume(void)
{
  __disable_irq();
  if (s_state == BUZZER_PAUSED) {
    s_state = BUZZER_PLAYING;
    buzzer_apply_freq_and_volume(s_cur_note_freq); /* restore the note that was frozen */
  }
  __enable_irq();
}

void Buzzer_TogglePause(void)
{
  if (s_state == BUZZER_PLAYING) {
    Buzzer_Pause();
  } else if (s_state == BUZZER_PAUSED) {
    Buzzer_Resume();
  }
  /* BUZZER_IDLE: no-op, nothing to toggle (caller should use
   * Buzzer_PlayMelody() to start one first). */
}

void Buzzer_Next(void)
{
  uint8_t total = buzzer_total_melody_count();
  uint8_t next = (uint8_t)((s_melody_idx + 1u) % total);
  Buzzer_PlayMelody(next);
}

void Buzzer_Prev(void)
{
  uint8_t total = buzzer_total_melody_count();
  uint8_t prev = (uint8_t)((s_melody_idx + total - 1u) % total);
  Buzzer_PlayMelody(prev);
}

void Buzzer_PlaySFX(BuzzerSfx sfx)
{
  uint16_t freq;
  uint16_t ms;
  switch (sfx) {
    case BUZZER_SFX_CLICK: freq = 4000u; ms = 20u;  break;
    case BUZZER_SFX_ERROR: freq = 200u;  ms = 100u; break;
    case BUZZER_SFX_HIT:   freq = 1200u; ms = 30u;  break;
    case BUZZER_SFX_KILL:  freq = 600u;  ms = 80u;  break;
    default: return;
  }
  __disable_irq();
  buzzer_apply_freq_and_volume(freq);
  s_sfx_remain_ms = ms;
  __enable_irq();
}

void Buzzer_SetVolume(uint8_t vol)
{
  if (vol > 100u) {
    vol = 100u;
  }
  __disable_irq();
  s_volume = vol;
  /* Re-apply immediately so the change is audible mid-note, using whatever
   * is currently supposed to be sounding (SFX takes priority if active,
   * matching Buzzer_TickISR()'s own priority rule). */
  if (s_sfx_remain_ms > 0u) {
    /* Leave the in-flight SFX's frequency as-is; only its CCR (loudness)
     * needs recomputing, which buzzer_apply_freq_and_volume() does via
     * ARR already loaded in hardware -- re-read it back instead of
     * re-deriving from a frequency we no longer have on hand here. */
    uint32_t arr = __HAL_TIM_GET_AUTORELOAD(&htim8);
    uint32_t volw = s_volume;
    __HAL_TIM_SET_COMPARE(&htim8, TIM_CHANNEL_1, ((arr / 2u) * volw * volw) / 10000u);
  } else if (s_state == BUZZER_PLAYING) {
    buzzer_apply_freq_and_volume(s_cur_note_freq);
  }
  /* BUZZER_IDLE / BUZZER_PAUSED: CCR is already 0 (silent); nothing to
   * re-apply until playback resumes, which re-derives CCR from s_volume
   * anyway (Buzzer_Resume()/Buzzer_PlayMelody()). */
  __enable_irq();
}

uint8_t Buzzer_GetVolume(void)
{
  return s_volume;
}

uint8_t Buzzer_IsPlaying(void)
{
  return (s_state == BUZZER_PLAYING) ? 1u : 0u;
}

uint8_t Buzzer_GetMelodyIndex(void)
{
  return s_melody_idx;
}

const char *Buzzer_GetMelodyName(uint8_t idx)
{
  if (idx < MELODY_COUNT) {
    return s_melodies[idx].name;
  }
  /* Phase 9: uploaded song -- read straight from Storage, not the shadow
   * copy (this is a display-only query for possibly-not-loaded idx, e.g.
   * app_music.c browsing without playing; the shadow only mirrors whatever
   * idx Buzzer_PlayMelody() last loaded). */
  const StorageSong *song = Storage_GetSong((uint8_t)(idx - MELODY_COUNT));
  return (song != NULL) ? song->name : "";
}

uint8_t Buzzer_GetMelodyCount(void)
{
  return buzzer_total_melody_count();
}

uint32_t Buzzer_GetElapsedMs(void)
{
  return s_elapsed_ms;
}

uint32_t Buzzer_GetMelodyDurationMs(uint8_t idx)
{
  if (idx < MELODY_COUNT) {
    return s_melody_total_ms[idx];
  }
  /* Phase 9: uploaded songs have no precomputed s_melody_total_ms[] slot
   * (their count/notes aren't fixed at compile time) -- sum on the fly.
   * STORAGE_SONG_MAX_NOTES notes is cheap enough to re-sum on every
   * call; this is a browse-time query (app_music.c's progress-bar/duration
   * display), not a per-tick ISR path. */
  const StorageSong *song = Storage_GetSong((uint8_t)(idx - MELODY_COUNT));
  if (song == NULL) {
    return 0u;
  }
  uint32_t total = 0;
  for (uint8_t i = 0; i < song->note_count; i++) {
    total += song->notes[i].ms;
  }
  return total;
}

void Buzzer_SeekPercent(uint8_t pct)
{
  if (s_state == BUZZER_IDLE || s_state == BUZZER_PIANO) {
    return; /* nothing loaded to seek within (BUZZER_PIANO has no melody
             * loaded either -- a continuous tone, not a note sequence) */
  }
  if (pct > 100u) {
    pct = 100u;
  }

  __disable_irq();
  const Melody *m = s_active_melody;
  uint32_t total_ms = (s_melody_idx < MELODY_COUNT)
                          ? s_melody_total_ms[s_melody_idx]
                          : Buzzer_GetMelodyDurationMs(s_melody_idx);
  uint32_t target_ms = (total_ms * pct) / 100u;
  uint32_t acc = 0;
  uint16_t idx = 0;
  for (; idx < m->count; idx++) {
    uint32_t note_ms = m->notes[idx].ms;
    uint8_t is_last = (idx == (uint16_t)(m->count - 1u)) ? 1u : 0u;
    if (target_ms < acc + note_ms || is_last) {
      /* Land inside this note (final note absorbs any pct==100 remainder). */
      uint32_t offset_into_note = is_last && (target_ms >= acc + note_ms)
                                       ? note_ms
                                       : (target_ms - acc);
      s_note_idx = idx;
      s_cur_note_freq = m->notes[idx].freqHz;
      /* Landing exactly on the last note's end (pct==100, or any pot value
       * the EMA/ADC ceiling rounds up to 100) makes offset_into_note ==
       * note_ms, i.e. s_remain_ms == 0. Buzzer_TickISR() treats
       * s_remain_ms==0 as "this note just finished" and advances on its
       * very next 1 ms tick -- since this is already the last note,
       * s_note_idx would overflow past m->count immediately, posting
       * EV_SONG_END and (via Music_HandleSongEnd()) restarting/advancing
       * the track a few ms after the user merely dragged the pot to its
       * end. That reset (elapsed time + progress bar snapping back to 0)
       * is what looked like the LCD "flashing" -- clamping to 1 ms keeps
       * the seek landing genuinely paused at the end instead of silently
       * completing the song. */
      uint16_t remain = (uint16_t)(note_ms - offset_into_note);
      s_remain_ms = (remain == 0u) ? 1u : remain;
      s_elapsed_ms = target_ms;
      if (s_state == BUZZER_PLAYING) {
        buzzer_apply_freq_and_volume(s_cur_note_freq);
      }
      __enable_irq();
      return;
    }
    acc += note_ms;
  }
  __enable_irq();
}

void Buzzer_PianoNoteOn(uint16_t freqHz)
{
  /* __disable_irq()-guarded: see buzzer.h's doc comment -- this can run
   * inside serial.c's UART RX ISR, which TIM3's Buzzer_TickISR() can
   * genuinely preempt (or vice versa), unlike every other mutator here
   * which only ever runs from app_task. */
  __disable_irq();
  s_state = BUZZER_PIANO;
  s_piano_freq = freqHz;
  buzzer_apply_freq_and_volume(freqHz);
  __enable_irq();
}

void Buzzer_PianoNoteOff(void)
{
  __disable_irq();
  if (s_state == BUZZER_PIANO) {
    s_state = BUZZER_IDLE;
    s_piano_freq = 0u;
    __HAL_TIM_SET_COMPARE(&htim8, TIM_CHANNEL_1, 0u);
  }
  __enable_irq();
}

uint16_t Buzzer_GetPianoFreq(void)
{
  return s_piano_freq;
}
