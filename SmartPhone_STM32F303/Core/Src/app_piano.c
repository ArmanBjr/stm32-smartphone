/**
 * @file app_piano.c
 * @brief Phase 9: live piano screen for piano.py /sms_bridge :piano mode.
 *        Source of truth: master plan section 3 ("app_piano.c -- bonus:
 *        pyserial live piano mode screen") and section 5.5 (`/piano-on`,
 *        `/pn-{freq}`, `/pf`). Notes themselves arrive via serial.c's ISR
 *        fast-path into Buzzer_PianoNoteOn()/Off(); this app only owns the
 *        LCD status UI. Phone music arbitration lives in phone.c
 *        (Phone_SwitchApp), same pattern as AppPvz.
 */
#include "app.h"
#include "ui.h"
#include "keypad.h"
#include "buzzer.h"
#include "serial.h"
#include <stdio.h>

static uint16_t s_last_shown_freq = 0xFFFFu; /* force first on_tick redraw */

static void piano_on_enter(void)
{
  Keypad_SetInputMode(INPUT_MODE_NAV);
  /* Free the PWM channel until the first /pn- arrives -- phone.c already
   * snapshot the in-progress melody for restore on leave. */
  Buzzer_Stop();
  s_last_shown_freq = 0xFFFFu;
  /* Machine token for host_tools/sms_bridge.py — arms PC keyboard capture. */
  LOG("[PIANO] enter");
}

static void piano_on_event(const Event *ev)
{
  (void)ev; /* BACK handled globally by phone.c */
}

static void piano_render(void)
{
  UI_BeginFrame();
  UI_Print(0, 0, "-- Live Piano --");
  UI_Print(1, 0, "PC: :piano / keys");

  uint16_t freq = Buzzer_GetPianoFreq();
  char line[UI_COLS + 1];
  if (freq == 0u) {
    UI_Print(2, 0, "Note: (silent)");
  } else {
    snprintf(line, sizeof(line), "Note: %u Hz", (unsigned)freq);
    UI_Print(2, 0, line);
  }
  UI_Print(3, 0, "BACK=Menu");
  UI_EndFrame();
}

static void piano_on_tick(void)
{
  /* ISR may change s_piano_freq without posting an event -- poll so the
   * LCD tracks live notes without waiting for a keypress. */
  uint16_t freq = Buzzer_GetPianoFreq();
  if (freq != s_last_shown_freq) {
    s_last_shown_freq = freq;
    piano_render();
  }
}

static void piano_on_suspend(void)
{
  /* no-op by design (plan section 4.4). Buzzer_PianoNoteOff + music restore
   * are centralized in phone.c's Phone_SwitchApp(). */
}

const App AppPiano = {
  .on_enter   = piano_on_enter,
  .on_event   = piano_on_event,
  .on_tick    = piano_on_tick,
  .render     = piano_render,
  .on_suspend = piano_on_suspend,
};
