/**
 * @file events.c
 * @brief eventQueue creation + non-blocking post. Per plan section 4.2:
 *        queue length EVENT_QUEUE_LEN (16), ISR side always uses a 0
 *        timeout and never blocks; overflow is dropped and counted.
 */
#include "events.h"
#include "app_config.h"
#include "serial.h"

osMessageQueueId_t eventQueue;

static volatile uint32_t s_dropped;

void Event_Init(void)
{
  eventQueue = osMessageQueueNew(EVENT_QUEUE_LEN, sizeof(Event), NULL);
}

uint8_t Event_Post(const Event *ev)
{
  osStatus_t status = osMessageQueuePut(eventQueue, ev, 0U, 0U);
  if (status != osOK) {
    s_dropped++;
    /* LOG() is ring-buffer + TX-IT based, callable "from anywhere" per
     * serial.h -- safe here even though Event_Post() runs in ISR context
     * for EV_KEY/EV_BTN_LOCK etc. */
    LOG("[EVT] queue full, dropped type=%u (total dropped=%lu)",
        (unsigned)ev->type, (unsigned long)s_dropped);
    return 0U;
  }
  return 1U;
}

uint32_t Event_DroppedCount(void)
{
  return s_dropped;
}
