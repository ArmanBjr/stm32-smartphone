/**
 * @file buzzer.h
 * @brief Phase 4: buzzer note/melody/SFX engine. Source of truth: master
 *        plan section 5.3 ("Buzzer & music engine") and section 2.3's TIM3
 *        (melody sequencer, 1 kHz tick) / TIM8_CH1 (PWM, PC6) rows.
 *
 *        Ownership split, mirroring ui.c/cgram.c's app_task-vs-ISR/ui_task
 *        split (rule 0.2): Buzzer_PlayMelody()/Pause()/Resume()/Next()/
 *        Prev()/PlaySFX()/SetVolume() are called from app_task (or, for
 *        Buzzer_TickISR(), from the TIM3 update ISR only). Unlike ui.c's
 *        relationship with hiwdg (deliberately kept out of ui.c via a
 *        callback -- see ui.h), buzzer.c *does* extern TIM8's handle
 *        directly: TIM8_CH1/PC6 is not a shared system resource, it is
 *        exclusively the buzzer's own dedicated peripheral (same
 *        reasoning LiquidCrystal.c already uses to own the LCD's GPIOs
 *        directly rather than going through a callback).
 */
#ifndef BUZZER_H
#define BUZZER_H

#include <stdint.h>

/** Short sound effects (plan section 5.3: "SFX (UI click 4 kHz/20 ms, error
 *  200 Hz/100 ms, jingles) briefly override the melody note then restore --
 *  single hardware channel, priority SFX>current music note"). Phase 8's
 *  game-over jingle is Buzzer_PlayMelody(PVZ_GAMEOVER_MELODY_IDX) rather
 *  than a third BuzzerSfx -- a multi-note phrase needs the melody
 *  sequencer; the single-tone SFX path cannot express it. */
typedef enum {
  BUZZER_SFX_CLICK = 0, /* 4000 Hz / 20 ms */
  BUZZER_SFX_ERROR,     /* 200 Hz / 100 ms -- also PvZ life-loss sad SFX */
  BUZZER_SFX_HIT,       /* Phase 8: pea impact ~1200 Hz / 30 ms */
  BUZZER_SFX_KILL,      /* Phase 8: zombie down ~600 Hz / 80 ms */
} BuzzerSfx;

void Buzzer_Init(void);

/** TIM3 update-ISR tick (1 kHz, plan section 2.3). Advances the current
 *  melody's note timing and/or an in-flight SFX override. Call ONLY from
 *  main.c's HAL_TIM_PeriodElapsedCallback() dispatch for TIM3 -- never from
 *  task context (mirrors keypad.h's Keypad_TickISR() contract). */
void Buzzer_TickISR(void);

/** Starts melody `idx` from its first note (idx wraps are the caller's
 *  responsibility -- out-of-range idx is a no-op). Posts nothing itself;
 *  natural end-of-melody posts EV_SONG_END (plan section 5.3) from
 *  Buzzer_TickISR(). */
void Buzzer_PlayMelody(uint8_t idx);
/** Stops playback and returns to IDLE (silent, no melody loaded for
 *  Resume). Used when leaving AppPvz with no phone-music to restore
 *  (PDF p.7 game BGM must not keep playing after the game exits). */
void Buzzer_Stop(void);
void Buzzer_Pause(void);
void Buzzer_Resume(void);
/** Convenience for a single key (e.g. a test app's SELECT): pause if
 *  playing, resume if paused, no-op if idle (no melody ever started). */
void Buzzer_TogglePause(void);
/** Jumps to the next/previous melody (wrapping) and starts it from note 0 --
 *  a user-driven skip, distinct from EV_SONG_END's natural-end auto-next
 *  (which is the *consumer's* job, e.g. Phase 5's Music app, per plan
 *  section 5.3: "post EV_SONG_END (music app auto-nexts)"). */
void Buzzer_Next(void);
void Buzzer_Prev(void);

/** Briefly overrides the current tone with `sfx`, then restores whatever
 *  was sounding before (silence if idle/paused, the current note if
 *  playing) -- priority SFX > music note, single hardware channel, per
 *  plan section 5.3. Safe to call from app_task. */
void Buzzer_PlaySFX(BuzzerSfx sfx);

/** vol clamped to [0,100]. Duty math per plan section 2.3's continuous
 *  volume formula (CCR = (ARR/2) * vol^2 / 10000). Safe to call from
 *  app_task; internally brief-critical-sectioned against Buzzer_TickISR(). */
void Buzzer_SetVolume(uint8_t vol);
uint8_t Buzzer_GetVolume(void);

uint8_t Buzzer_IsPlaying(void);
uint8_t Buzzer_GetMelodyIndex(void);
const char *Buzzer_GetMelodyName(uint8_t idx);
uint8_t Buzzer_GetMelodyCount(void);

/** Phase 5 (plan section 5.4: "pot seek...while pot_role=SEEK") additions.
 *  Elapsed/duration are both milliseconds into the *currently loaded*
 *  melody (s_melody_idx) -- meaningful even while BUZZER_PAUSED (frozen)
 *  or BUZZER_IDLE (0, nothing loaded). */
uint32_t Buzzer_GetElapsedMs(void);
uint32_t Buzzer_GetMelodyDurationMs(uint8_t idx);

/** Repositions playback within the *currently loaded* melody to `pct`
 *  (clamped 0-100) of its total duration, preserving play/pause state.
 *  No-op if BUZZER_IDLE (nothing loaded to seek within). Safe to call
 *  from app_task; internally critical-sectioned against Buzzer_TickISR(). */
void Buzzer_SeekPercent(uint8_t pct);

/** Phase 9 (plan section 6: piano.py live note streaming, plan section
 *  5.3-adjacent -- a single hardware channel, same one melodies/SFX use).
 *  Starts/retunes a continuous tone at `freqHz` (0 = silence, matches the
 *  melody engine's own rest convention). Overrides whatever melody state
 *  was active -- the caller (phone.c's AppPiano entry) is responsible for
 *  snapshotting/restoring any in-progress phone music, same as it already
 *  does around AppPvz. Called directly from serial.c's UART RX ISR for
 *  `/pn-{freq}`/`/pf` (plan section 6's "<=10ms latency" requirement rules
 *  out a post-to-app_task round trip -- see serial.c's try_piano_fastpath()).
 *  Internally __disable_irq()-guarded like every other buzzer.c state
 *  mutator, since the UART RX ISR and TIM3's Buzzer_TickISR() are separate
 *  interrupts that can genuinely preempt each other (unlike app_task, which
 *  TIM3 alone can already preempt -- the existing reason those mutators are
 *  guarded at all). */
void Buzzer_PianoNoteOn(uint16_t freqHz);

/** Silences the piano tone and returns the buzzer to BUZZER_IDLE (mirrors
 *  Buzzer_Stop()'s end state) -- does NOT restore any pre-Piano melody
 *  itself, that is phone.c's job on leaving AppPiano (same split as
 *  Buzzer_Stop() vs phone.c's AppPvz-exit restore logic). */
void Buzzer_PianoNoteOff(void);

/** Last frequency passed to Buzzer_PianoNoteOn() (0 if none/since
 *  Buzzer_PianoNoteOff()) -- for app_piano.c's render() to show what note is
 *  currently sounding without app_piano.c needing its own shadow copy. */
uint16_t Buzzer_GetPianoFreq(void);

#endif /* BUZZER_H */
