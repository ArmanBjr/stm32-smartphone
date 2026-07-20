/**
 * @file serial.c
 * @brief TX ring buffer drained by USART1 TX-complete interrupt; LOG(fmt,...)
 *        usable from any task context. Phase 7: RX line assembly (plan
 *        section 5.5) now wired -- see serial.h's header comment for the
 *        ping-pong buffer contract with cmdparse.c.
 */
#include "serial.h"
#include "app_config.h"
#include "events.h"
#include "buzzer.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#define TX_RING_SIZE 512u

static UART_HandleTypeDef *s_huart;
static uint8_t  tx_ring[TX_RING_SIZE];
static volatile uint16_t tx_head;      /* next free write index */
static volatile uint16_t tx_tail;      /* next byte to send */
static volatile uint8_t  tx_busy;
static volatile uint16_t tx_inflight;  /* length of the chunk currently in HAL_UART_Transmit_IT */
static volatile uint32_t s_tx_dropped; /* Phase 10: bytes not queued (ring full) */

/* RX line assembly (plan section 5.5). Two ping-pong buffers so the ISR
 * can keep filling the *next* line while app_task/cmdparse.c is still
 * reading the previous one -- see serial.h's header comment. */
static char             rx_line[2][SERIAL_RX_LINE_CAP + 1u];
static volatile uint8_t rx_active; /* which half the ISR is filling now */
static uint8_t          rx_len;    /* chars in rx_line[rx_active] so far */
static uint8_t          rx_byte;   /* single-byte HAL_UART_Receive_IT() target */

static void tx_kick(void)
{
  if (tx_busy || tx_head == tx_tail) {
    return;
  }

  uint16_t tail = tx_tail;
  uint16_t len = (tx_head > tail) ? (uint16_t)(tx_head - tail)
                                   : (uint16_t)(TX_RING_SIZE - tail);

  tx_busy = 1;
  tx_inflight = len;
  HAL_UART_Transmit_IT(s_huart, &tx_ring[tail], len);
}

void Serial_Init(UART_HandleTypeDef *huart)
{
  s_huart = huart;
  tx_head = 0;
  tx_tail = 0;
  tx_busy = 0;
  tx_inflight = 0;
  s_tx_dropped = 0;
}

void Serial_Send(const char *str)
{
  if (str == NULL || s_huart == NULL) {
    return;
  }

  size_t len = strlen(str);
  for (size_t i = 0; i < len; i++) {
    uint16_t next = (uint16_t)((tx_head + 1u) % TX_RING_SIZE);
    if (next == tx_tail) {
      /* ring full: drop remainder rather than block (never block). Count
       * every discarded byte for /health — do not LOG here. */
      s_tx_dropped += (uint32_t)(len - i);
      break;
    }
    tx_ring[tx_head] = (uint8_t)str[i];
    tx_head = next;
  }

  __disable_irq();
  tx_kick();
  __enable_irq();
}

uint32_t Serial_TxDroppedCount(void)
{
  return s_tx_dropped;
}

void LOG(const char *fmt, ...)
{
  char buf[192];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 2, fmt, args);
  va_end(args);

  if (n < 0) {
    return;
  }
  if ((size_t)n > sizeof(buf) - 3) {
    n = (int)sizeof(buf) - 3;
  }
  buf[n]     = '\r';
  buf[n + 1] = '\n';
  buf[n + 2] = '\0';

  Serial_Send(buf);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (s_huart == NULL || huart->Instance != s_huart->Instance) {
    return;
  }
  tx_tail = (uint16_t)((tx_tail + tx_inflight) % TX_RING_SIZE);
  tx_busy = 0;
  tx_kick();
}

void Serial_StartRx(void)
{
  rx_active = 0u;
  rx_len = 0u;
  if (s_huart != NULL) {
    HAL_UART_Receive_IT(s_huart, &rx_byte, 1u);
  }
}

/* Phase 9 (plan section 6: piano.py, "<=10ms latency" requirement) --
 * `/pn-{freq}` and `/pf` are intercepted here, in ISR context, rather than
 * posted as EV_UART_CMD and parsed by cmdparse.c from app_task: a full
 * post/dispatch/parse round trip adds tens of ms of jitter under FreeRTOS
 * scheduling (queue latency + app_task's own event-loop cadence), which the
 * plan's latency budget cannot absorb. Every other verb (including
 * `/piano-on` itself, which merely switches the current app and is not
 * latency-sensitive) still goes through the normal EV_UART_CMD path -- this
 * is a narrow, two-verb carve-out, not a parallel command parser. Returns 1
 * if it handled `line` (caller must NOT also post EV_UART_CMD for it), 0
 * otherwise (fall through to the normal path). `line` is NUL-terminated,
 * `len` is its length excluding the NUL (matches rx_len's own meaning). */
static uint8_t try_piano_fastpath(const char *line, uint8_t len)
{
  if (len == 3u && line[0] == '/' && line[1] == 'p' && line[2] == 'f') {
    Buzzer_PianoNoteOff();
    return 1u;
  }
  if (len > 4u && line[0] == '/' && line[1] == 'p' && line[2] == 'n' && line[3] == '-') {
    char *end;
    unsigned long freq = strtoul(&line[4], &end, 10);
    /* Require a clean integer (no trailing junk) -- otherwise `/pn-440x`
     * would still sound 440 Hz and look like success. */
    if (end != &line[4] && *end == '\0' && freq <= 0xFFFFu) {
      Buzzer_PianoNoteOn((uint16_t)freq);
    } /* malformed freq: silently ignored, same "drop rather than desync"
       * spirit as the RX-overflow branch below -- no ack is expected for
       * this fast-path verb anyway (piano.py doesn't wait for one, per the
       * latency requirement). */
    return 1u;
  }
  return 0u;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (s_huart == NULL || huart->Instance != s_huart->Instance) {
    return;
  }

  char c = (char)rx_byte;
  if (c == '\n' || c == '\r') {
    /* \r\n arrives as two terminators in a row -- only the first (with
     * rx_len > 0) closes a line; the second is silently absorbed instead
     * of posting a spurious empty line. */
    if (rx_len > 0u) {
      rx_line[rx_active][rx_len] = '\0';
      if (!try_piano_fastpath(rx_line[rx_active], rx_len)) {
        Event ev;
        ev.type = (uint8_t)EV_UART_CMD;
        ev.a    = rx_active;
        ev.b    = rx_len;
        ev.c    = (uint32_t)(uintptr_t)rx_line[rx_active];
        (void)Event_Post(&ev);
        rx_active = (uint8_t)(rx_active ^ 1u);
      }
      rx_len = 0u;
    }
  } else if (rx_len < SERIAL_RX_LINE_CAP) {
    rx_line[rx_active][rx_len] = c;
    rx_len++;
  } /* else: line too long -- drop the overflow byte, keep assembling so
     * the eventual \n still closes (and cmdparse.c) rejects) the
     * truncated line rather than desyncing on this and every future line */

  HAL_UART_Receive_IT(s_huart, &rx_byte, 1u);
}
