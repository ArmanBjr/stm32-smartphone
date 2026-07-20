/**
 * @file health.h
 * @brief Phase 10 / innovation I9: fault telemetry exposed as `/health`.
 *        Reports uptime, FreeRTOS stack high-water marks for the three
 *        phone tasks, and queue/UART drop counters. Call
 *        Health_RegisterTasks() once after osThreadNew(); Health_LogReport()
 *        only from app_task (cmdparse) -- not from ISR.
 */
#ifndef HEALTH_H
#define HEALTH_H

#include "cmsis_os.h"

/** Store the three phone task handles for later HWM queries. */
void Health_RegisterTasks(osThreadId_t ui, osThreadId_t app, osThreadId_t storage);

/** Emit `[HEALTH] …` lines over UART (uptime, drops, stack HWM). */
void Health_LogReport(void);

#endif /* HEALTH_H */
