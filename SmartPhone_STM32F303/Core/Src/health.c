/**
 * @file health.c
 * @brief See health.h. Phase 10 hardening telemetry.
 */
#include "health.h"
#include "events.h"
#include "serial.h"
#include "stm32f3xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"

static osThreadId_t s_ui;
static osThreadId_t s_app;
static osThreadId_t s_storage;

void Health_RegisterTasks(osThreadId_t ui, osThreadId_t app, osThreadId_t storage)
{
  s_ui = ui;
  s_app = app;
  s_storage = storage;
}

static UBaseType_t hwm_words(osThreadId_t id)
{
  if (id == NULL) {
    return 0U;
  }
  /* CMSIS-RTOS v2 osThreadId_t is the FreeRTOS TaskHandle_t. */
  return uxTaskGetStackHighWaterMark((TaskHandle_t)id);
}

void Health_LogReport(void)
{
  uint32_t uptime_s = HAL_GetTick() / 1000u;
  uint32_t drops_evt = Event_DroppedCount();
  uint32_t drops_tx = Serial_TxDroppedCount();

  LOG("[HEALTH] uptime=%lus drops_evt=%lu drops_tx=%lu",
      (unsigned long)uptime_s,
      (unsigned long)drops_evt,
      (unsigned long)drops_tx);

  LOG("[HEALTH] hwm ui=%lu app=%lu storage=%lu (words free)",
      (unsigned long)hwm_words(s_ui),
      (unsigned long)hwm_words(s_app),
      (unsigned long)hwm_words(s_storage));
}
