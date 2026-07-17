/**
 * @file events.h
 * @brief Cross-module event queue. Source of truth: master plan section 4.2
 *        ("Event system"). Single flat `Event` struct posted from thin ISRs,
 *        drained by `app_task` (Phase 3) / a temporary scratch handler
 *        (Phase 2, see `StartDefaultTask` in main.c). Never block an ISR:
 *        posting always uses a 0 timeout and drops + counts on overflow.
 */
#ifndef EVENTS_H
#define EVENTS_H

#include <stdint.h>
#include "cmsis_os.h"

/** Event.type values. Payload interpretation of `a`/`b`/`c` is per-type and
 *  owned by the producing module (e.g. EV_KEY's KeyEventType/char live in
 *  a/b -- see keypad.h). */
typedef enum {
  EV_KEY = 0,
  EV_TICK_1S,
  EV_GAME_TICK,
  EV_ADC_POT,
  EV_ADC_LDR_EDGE,
  EV_UART_CMD,
  EV_SONG_END,
  EV_AUTOLOCK,
  EV_BTN_LOCK,
  EV_SMS_RESULT, /* Phase 11 (§5.10): a=0 OK / a=1 ERR; c = ptr to status/recId string */
} EventType;

/** Exact shape from plan section 4.2: `Event {uint8_t type; uint8_t a;
 *  uint16_t b; uint32_t c;}`. Kept flat/fixed-size so it can sit in a
 *  fixed-size osMessageQueue item slot with no allocation. */
typedef struct {
  uint8_t  type; /* EventType */
  uint8_t  a;
  uint16_t b;
  uint32_t c;
} Event;

extern osMessageQueueId_t eventQueue;

/** Create eventQueue. Call once from main(), before osKernelStart() (the
 *  "USER CODE BEGIN RTOS_QUEUES" section), task/ISR-safe either way. */
void Event_Init(void);

/** Non-blocking post (0 timeout) -- safe from ISR or task context.
 *  Returns 1 if enqueued, 0 if the queue was full (dropped + counted;
 *  never blocks the caller, per plan section 4.2). */
uint8_t Event_Post(const Event *ev);

/** Total events dropped so far because the queue was full (diagnostics). */
uint32_t Event_DroppedCount(void);

#endif /* EVENTS_H */
