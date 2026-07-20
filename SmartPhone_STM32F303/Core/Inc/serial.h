/**
 * @file serial.h
 * @brief TX ring buffer + IT drain, RX byte capture; LOG(fmt,...) from
 *        anywhere. Per plan section 5.5.
 *
 *        Phase 7 update: RX line assembly is now wired (plan section 5.5:
 *        "RX: byte ISR appends to line buffer until \n/\r -> hand line to
 *        cmdparse in app_task context (post event)"). Serial_StartRx()
 *        arms the first `HAL_UART_Receive_IT()`; each completed line is
 *        posted as an `EV_UART_CMD` event whose `Event.c` is a pointer
 *        into one of two static ping-pong line buffers (`Event.a` selects
 *        which one) -- cmdparse.c (app_task context) must finish reading
 *        it before the *next* line completes and the ISR starts reusing
 *        that same buffer half again; two buffers give app_task exactly
 *        one full line's worth of grace, which is generous since
 *        app_task's queue-drain loop runs every event with no blocking
 *        work in between.
 */
#ifndef SERIAL_H
#define SERIAL_H

#include "stm32f3xx_hal.h"
#include <stdint.h>

void Serial_Init(UART_HandleTypeDef *huart);

/** Queue a raw string for transmission (copied into the ring buffer). */
void Serial_Send(const char *str);

/** printf-style logging: formats into a stack buffer, then Serial_Send(). */
void LOG(const char *fmt, ...);

/** Arms the first single-byte HAL_UART_Receive_IT() so RX line assembly
 *  starts. Call once from main(), after Serial_Init(), before
 *  osKernelStart() -- HAL_UART_RxCpltCallback() re-arms itself for every
 *  subsequent byte, so this is a one-time kickoff, not a poll. */
void Serial_StartRx(void);

/** Phase 10: bytes silently dropped because the TX ring was full.
 *  Reported by `/health`; no per-drop LOG (that would worsen overflow). */
uint32_t Serial_TxDroppedCount(void);

#endif /* SERIAL_H */
