/**
 * @file cmdparse.h
 * @brief UART command dispatcher. Source of truth: master plan section 5.5
 *        ("Serial + commands") and section 6 Phase 7 / Phase 9 exits.
 *
 *        Implemented verbs:
 *          /start, /reset, /lock, /setting-{NAME}-{VALUE},
 *          /time-{unix_epoch},
 *          /health  (Phase 10 / I9: uptime, stack HWM, drop counters),
 *          /shot    (I4: dump 20x4 LCD shadow buffer as ASCII),
 *          /mode  (replies `[LDR] state: day|night` -- host web UI theme),
 *          /piano-on, /pn-{freq}, /pf  (pn/pf normally via serial ISR
 *            fast-path; see serial.c),
 *          /songup-{name}-{count} + `freq,ms` note lines + /end,
 *          SMS_RESULT|... (Phase 11, no leading '/').
 *
 *        Anything else: `ERR unknown/invalid: <line>`.
 */
#ifndef CMDPARSE_H
#define CMDPARSE_H

/** Parses and executes one already-line-assembled command (no trailing
 *  \r/\n -- serial.c's RX line assembler strips those). Call only from
 *  app_task context (phone.c's Phone_Dispatch() on EV_UART_CMD) -- this
 *  function calls into phone.c/storage.c/rtc_time.c, none of which are
 *  ISR-safe. Always replies over UART (OK/ERR/log line) so Termite / the
 *  host bridge give visible feedback for every command, matching the
 *  plan's "reject anything else: reply ERR unknown/invalid: <line>"
 *  wording. */
void Cmdparse_HandleLine(const char *line);

#endif /* CMDPARSE_H */
