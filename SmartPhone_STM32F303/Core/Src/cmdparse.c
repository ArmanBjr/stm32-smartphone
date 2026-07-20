/**
 * @file cmdparse.c
 * @brief See cmdparse.h. Phase 11: SMS_RESULT| replies are recognized
 *        without a leading '/' (plan section 5.10) -- handled before the
 *        slash-only command gate below.
 */
#include "cmdparse.h"
#include "phone.h"
#include "storage.h"
#include "rtc_time.h"
#include "buzzer.h"
#include "analog.h"
#include "health.h"
#include "app_config.h"
#include "events.h"
#include "serial.h"
#include "stm32f3xx_hal.h"
#include "cmsis_os.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

/* Ping-pong payload for EV_SMS_RESULT (same two-buffer grace pattern as
 * serial.c's EV_UART_CMD line buffers -- app_task must finish reading
 * before the next SMS_RESULT reuses the same half). */
#define SMS_RESULT_BUF_CAP 80u
static char s_sms_result_buf[2][SMS_RESULT_BUF_CAP];
static uint8_t s_sms_result_idx = 0u;

/* Phase 9 (plan section 6: song_upload.py) -- `/songup-{name}-{count}`
 * session state. Plain (non-volatile) static, unlike serial.c's rx_line[]
 * ping-pong buffers: this whole session lives entirely in app_task context
 * (Cmdparse_HandleLine() is only ever called from Phone_Dispatch() on
 * EV_UART_CMD, ISR fast-path traffic for /pn-//pf never reaches this file --
 * see serial.c's try_piano_fastpath()), so there is no ISR/task race to
 * guard against, same reasoning s_sms_result_buf above already relies on. */
typedef struct {
  uint8_t         active;
  char            name[STORAGE_SONG_NAME_LEN];
  uint8_t         expected;
  uint8_t         received;
  StorageSongNote notes[STORAGE_SONG_MAX_NOTES];
} SongupSession;
static SongupSession s_songup;

static void reply_err(const char *line)
{
  LOG("ERR unknown/invalid: %s", line);
}

static uint8_t parse_u32(const char *s, uint32_t *out)
{
  if (s == NULL || *s == '\0') {
    return 0u;
  }
  char *end = NULL;
  unsigned long v = strtoul(s, &end, 10);
  if (end == s || *end != '\0') {
    return 0u; /* trailing garbage -- not a clean integer */
  }
  *out = (uint32_t)v;
  return 1u;
}

/** Phase 11 (plan section 5.10): PC bridge reply lines.
 *  SMS_RESULT|OK|<recId> or SMS_RESULT|ERR|<status-or-http-code> */
static uint8_t handle_sms_result(const char *line)
{
  static const char prefix[] = "SMS_RESULT|";
  if (strncmp(line, prefix, sizeof(prefix) - 1u) != 0) {
    return 0u;
  }

  const char *body = line + (sizeof(prefix) - 1u);
  uint8_t is_err = 0u;
  const char *payload = NULL;

  if (strncmp(body, "OK|", 3) == 0) {
    is_err = 0u;
    payload = body + 3;
  } else if (strncmp(body, "ERR|", 4) == 0) {
    is_err = 1u;
    payload = body + 4;
  } else {
    reply_err(line);
    return 1u; /* consumed as a (malformed) SMS_RESULT attempt */
  }

  s_sms_result_idx = (uint8_t)(1u - s_sms_result_idx);
  char *dst = s_sms_result_buf[s_sms_result_idx];
  size_t n = strlen(payload);
  if (n >= SMS_RESULT_BUF_CAP) {
    n = SMS_RESULT_BUF_CAP - 1u;
  }
  memcpy(dst, payload, n);
  dst[n] = '\0';

  /* Full raw line (incl. any Persian status) stays on the UART log -- LCD
   * only ever shows Sent / Send failed (plan section 5.10). */
  LOG("%s", line);

  Event ev = {
    .type = (uint8_t)EV_SMS_RESULT,
    .a = is_err,
    .b = 0,
    .c = (uint32_t)(uintptr_t)dst,
  };
  Event_Post(&ev);
  return 1u;
}

/** Handles `/setting-{NAME}-{VALUE}` (plan section 5.5). `body` is
 *  everything after the "setting-" prefix, e.g. "ldr-2200". */
static void handle_setting(const char *line, const char *body)
{
  const StorageSettings *ro = Storage_GetSettings();
  if (!ro->uart_settings_enable) {
    LOG("[CMD] ERR setting rejected (UART settings OFF): %s", line);
    return;
  }

  const char *dash = strchr(body, '-');
  if (dash == NULL) {
    reply_err(line);
    return;
  }
  char name[16];
  size_t name_len = (size_t)(dash - body);
  if (name_len == 0u || name_len >= sizeof(name)) {
    reply_err(line);
    return;
  }
  memcpy(name, body, name_len);
  name[name_len] = '\0';
  const char *value_str = dash + 1;

  uint32_t value;
  if (!parse_u32(value_str, &value)) {
    reply_err(line);
    return;
  }

  StorageSettings *s = Storage_GetSettingsMutable();

  if (strcmp(name, "ldr") == 0 && value <= 4095u) {
    s->ldr_threshold = (uint16_t)value;
  } else if (strcmp(name, "icons") == 0 && value <= 2u) {
    s->icon_layout = (uint8_t)value;
  } else if (strcmp(name, "volume") == 0 && value <= 100u) {
    s->volume = (uint8_t)value;
    Buzzer_SetVolume(s->volume);
  } else if (strcmp(name, "uartset") == 0 && value <= 1u) {
    s->uart_settings_enable = (uint8_t)value;
  } else if (strcmp(name, "autolock") == 0 && value <= 65535u) {
    s->autolock_s = (uint16_t)value;
  } else if (strcmp(name, "segtime") == 0 && value <= 1u) {
    s->segtime_enable = (uint8_t)value;
    /* Same live apply as app_settings.c's ROW_SEGTIME -- UART path must
     * not leave the 7-seg stuck until the next app switch. */
    Phone_ApplySeg7Default();
  } else {
    reply_err(line);
    return;
  }

  Storage_RequestSave();
  LOG("OK");
}

static void songup_abort(const char *why)
{
  s_songup.active = 0u;
  s_songup.expected = 0u;
  s_songup.received = 0u;
  LOG("[CMD] ERR songup aborted: %s", why);
}

/** Phase 9 (plan section 5.5): `/songup-{name}-{count}` then `count` lines
 *  of `freq,ms`, then `/end`. Returns 1 if `line` was consumed by the
 *  songup state machine (including malformed mid-session lines that abort
 *  it), 0 if the line is unrelated and the normal command path should run. */
static uint8_t handle_songup_traffic(const char *line)
{
  if (!s_songup.active) {
    return 0u;
  }

  if (strcmp(line, "/end") == 0) {
    if (s_songup.received != s_songup.expected) {
      char why[40];
      snprintf(why, sizeof(why), "got %u/%u notes",
               (unsigned)s_songup.received, (unsigned)s_songup.expected);
      songup_abort(why);
      return 1u;
    }
    if (s_songup.received == 0u) {
      songup_abort("empty song");
      return 1u;
    }
    uint8_t slot = Storage_UploadSong(s_songup.name, s_songup.notes,
                                      s_songup.received);
    Storage_RequestSave();
    s_songup.active = 0u;
    LOG("OK stored as #%u", (unsigned)slot);
    return 1u;
  }

  /* Mid-session note line: plain `freq,ms` (plan section 5.5 literal).
   * A stray slash-command aborts the session and falls through so the
   * command itself can still run (e.g. `/start` / `/lock` mid-upload). */
  if (line[0] == '/') {
    songup_abort(line);
    return 0u;
  }

  const char *comma = strchr(line, ',');
  if (comma == NULL || comma == line) {
    songup_abort("bad note line");
    return 1u;
  }

  char freq_buf[12];
  size_t freq_len = (size_t)(comma - line);
  if (freq_len == 0u || freq_len >= sizeof(freq_buf)) {
    songup_abort("bad note freq");
    return 1u;
  }
  memcpy(freq_buf, line, freq_len);
  freq_buf[freq_len] = '\0';

  uint32_t freq = 0u;
  uint32_t ms = 0u;
  if (!parse_u32(freq_buf, &freq) || !parse_u32(comma + 1, &ms)) {
    songup_abort("bad note ints");
    return 1u;
  }
  if (freq > 0xFFFFu || ms > 0xFFFFu || ms == 0u) {
    songup_abort("note out of range");
    return 1u;
  }
  if (s_songup.received >= s_songup.expected ||
      s_songup.received >= STORAGE_SONG_MAX_NOTES) {
    songup_abort("too many notes");
    return 1u;
  }

  s_songup.notes[s_songup.received].freqHz = (uint16_t)freq;
  s_songup.notes[s_songup.received].ms     = (uint16_t)ms;
  s_songup.received++;
  return 1u;
}

static void handle_songup_begin(const char *line)
{
  /* body after "/songup-" is "{name}-{count}" -- name may itself contain
   * hyphens; the LAST '-' separates name from count (same "last dash"
   * convention as a path/basename split, not a guess: count is always a
   * trailing unsigned integer). */
  const char *body = line + 8; /* strlen("/songup-") */
  const char *last_dash = strrchr(body, '-');
  if (last_dash == NULL || last_dash == body) {
    reply_err(line);
    return;
  }

  uint32_t count = 0u;
  if (!parse_u32(last_dash + 1, &count) || count == 0u ||
      count > STORAGE_SONG_MAX_NOTES) {
    reply_err(line);
    return;
  }

  size_t name_len = (size_t)(last_dash - body);
  if (name_len == 0u) {
    reply_err(line);
    return;
  }
  if (name_len >= STORAGE_SONG_NAME_LEN) {
    name_len = STORAGE_SONG_NAME_LEN - 1u;
  }

  memset(&s_songup, 0, sizeof(s_songup));
  memcpy(s_songup.name, body, name_len);
  s_songup.name[name_len] = '\0';
  s_songup.expected = (uint8_t)count;
  s_songup.received = 0u;
  s_songup.active = 1u;
  LOG("OK songup ready %u notes", (unsigned)s_songup.expected);
}

void Cmdparse_HandleLine(const char *line)
{
  if (line == NULL) {
    reply_err("");
    return;
  }

  /* Phase 11 (plan section 5.10): bridge replies have no leading '/'. */
  if (handle_sms_result(line)) {
    return;
  }

  /* Phase 9: songup session may be consuming bare `freq,ms` lines (no
   * leading '/') -- must run before the slash-only gate below. */
  if (handle_songup_traffic(line)) {
    return;
  }

  if (line[0] != '/') {
    reply_err(line);
    return;
  }

  if (strcmp(line, "/start") == 0) {
    Phone_Boot();
    return;
  }

  if (strcmp(line, "/reset") == 0) {
    LOG("[CMD] /reset");
    osDelay(50); /* plan section 5.5: "small delay to flush TX" before
                   * NVIC_SystemReset() -- app_task blocking briefly here
                   * is deliberate and matches Phone_Init()'s existing
                   * osDelay(BOOT_LOGO_MS) precedent for a one-shot,
                   * non-looping delay (rule 0.1 unaffected). */
    NVIC_SystemReset();
    return; /* unreachable */
  }

  if (strcmp(line, "/lock") == 0) {
    Phone_Lock();
    return;
  }

  /* Phase 10 / I9: uptime, stack HWM, event + UART TX drop counters. */
  if (strcmp(line, "/health") == 0) {
    Health_LogReport();
    return;
  }

  /* Host web UI day/night theme: query the LDR state on demand. analog.c
   * only logs "[LDR] ..." at boot and on crossings, so a bridge that
   * connects later needs this to bootstrap its theme. Same trailing-word
   * format ("day"/"night") the bridge already parses from edge logs. */
  if (strcmp(line, "/mode") == 0) {
    LOG("[LDR] state: %s", Analog_IsNight() ? "night" : "day");
    return;
  }

  if (strncmp(line, "/setting-", 9) == 0) {
    handle_setting(line, line + 9);
    return;
  }

  if (strncmp(line, "/time-", 6) == 0) {
    uint32_t epoch;
    if (parse_u32(line + 6, &epoch)) {
      RTC_Time_SetFromEpoch(epoch);
      LOG("OK");
    } else {
      reply_err(line);
    }
    return;
  }

  /* Phase 9 (plan section 5.5 bonus protocol). `/pn-` and `/pf` are
   * normally consumed in serial.c's UART RX ISR (latency path) and never
   * reach here; the handlers below are defensive for any non-ISR path that
   * might still deliver them (e.g. a future soft-inject). */
  if (strcmp(line, "/piano-on") == 0) {
    Phone_SwitchApp(&AppPiano);
    LOG("OK");
    return;
  }
  if (strcmp(line, "/piano-off") == 0) {
    /* Host bridge :piano / q — leave Piano back to Menu (same as BACK). */
    Phone_SwitchApp(&AppMenu);
    LOG("OK");
    return;
  }
  if (strncmp(line, "/pn-", 4) == 0) {
    uint32_t freq;
    if (parse_u32(line + 4, &freq) && freq <= 0xFFFFu) {
      Buzzer_PianoNoteOn((uint16_t)freq);
    } else {
      reply_err(line);
    }
    return;
  }
  if (strcmp(line, "/pf") == 0) {
    Buzzer_PianoNoteOff();
    return;
  }
  if (strncmp(line, "/songup-", 8) == 0) {
    if (s_songup.active) {
      songup_abort("nested songup");
    }
    handle_songup_begin(line);
    return;
  }
  if (strcmp(line, "/end") == 0) {
    /* /end with no active session -- not a silent success. */
    reply_err(line);
    return;
  }

  reply_err(line);
}
