# SmartPhone on STM32F303 Discovery — Master Implementation Plan (Source of Truth)

**Course:** Microprocessor Lab final project (پروژه پایانی آزمایشگاه ریزپردازنده)
**Board:** STM32F3DISCOVERY (STM32F303VCT6, Cortex-M4F, 72 MHz, 256 KB flash, 40+8 KB RAM)
**Toolchain:** STM32CubeIDE + CubeMX (.ioc), HAL allowed, C only.
**Deliverable:** `Core/` folder + `Project_name.ioc` zipped as `Name_StudentNumber_S#_T#.zip`.

**Scope:** Base project (smartphone with Menu, Note, Contact, Music Player, Plant-vs-Zombie,
Settings, Info, lock screen, UART/Termite commands + logging, LDR day/night, potentiometer
volume/seek, 4-digit 7-segment via 7448) **plus ALL bonus items**: pyserial live piano (10%),
pyserial song upload (10%), RTC live time sync + 7-seg time mode (15%), special zombie/plant
(5%), FreeRTOS implementation (15%), plus innovation features (up to +20).

---

## 0. NON-NEGOTIABLE RULES (from the project PDF — grading critical)

1. **Every module MUST be interrupt-driven.** (تمامی ماژول ها باید به صورت وقفه ای پیاده سازی شوند)
   No polling loops for keypad, ADC, UART, timing. All hardware events enter the system
   through ISRs / HAL callbacks.
2. **The `while(1)` loop may contain ONLY LCD-related code.** (در بخش while فقط کد های مربوط به lcd)
   Under FreeRTOS: `main()`'s `while(1)` is never reached after `osKernelStart()`; the rule is
   honored by making the **LCD render task's loop contain only LCD drawing code**. All other
   work happens in ISRs and non-LCD tasks. ⚠ **Team action: confirm this interpretation with
   the TA once, in writing.** Fallback if TA insists on bare-metal literalism: the architecture
   below is deliberately portable — remove FreeRTOS, move task bodies into ISR-driven state
   updates, keep `while(1)` = render loop only (see §9.1).
3. **Code style must match what was taught in class.** Project = Header files (declarations,
   `#ifndef` guards, `#define`s, enums/structs, globals as `extern`, function prototypes) +
   Implementation files (function bodies) + Application file (`main.c`). `const` tables go to
   ROM. Use `stdint.h` types (`uint8_t`, `uint32_t`, ...). No exotic constructs.
4. **Every user-visible action is logged to Termite over UART.** Invalid inputs (keypad or
   UART) are rejected AND announced.
5. Cheating rules / packaging rules per page 1 of the PDF (out of scope for agents, team handles).

---

## 1. HARDWARE MAP (verified — do not change without re-verifying)

### 1.1 Pin assignment

| Function | Pin(s) | Config | Notes |
|---|---|---|---|
| 7448 BCD A/B/C/D | PD0/PD1/PD2/PD3 | GPIO_Output PP, no pull, LOW speed | BCD LSB→MSB |
| 7-seg decimal point | PD7 | GPIO_Output | via 330 Ω |
| Digit selects 1–4 (cathodes) | PC0/PC1/PC2/PC3 | GPIO_Output | Active level per hardware: digit ON when its cathode line is enabled — **verify polarity on real board in Phase 1** (common-cathode + driver may invert; previous project code is the reference) |
| Green LED | PB0 | GPIO_Output | feedback / PvZ life OK |
| Red LED | PB1 | GPIO_Output | error / PvZ life lost blink |
| Push button | PB3 | GPIO_EXTI3, **pull-down, rising edge** | quick-lock button (our added role) |
| LCD 20×4 HD44780 4-bit | RS=PD8 RW=PD9 EN=PD10 D4..D7=PD11..PD14 | GPIO_Output | existing `LiquidCrystal` driver |
| Keypad rows 0–3 (inputs) | PE7/PE8/PE9/PE10 | GPIO_EXTI, **pull-down, rising edge** | EXTI9_5 covers PE7–9; EXTI15_10 covers PE10 |
| Keypad cols 0–3 (outputs) | PE11/PE12/PE13/PE14 | GPIO_Output PP, **idle HIGH** | |
| Buzzer (passive) | PC6 | **TIM8_CH1 PWM (AF4)** | pitch = PWM freq, loudness = duty |
| Potentiometer | PB2 | **ADC2_IN12**, single-ended | volume (everywhere) / seek (music) |
| **LDR (to be wired)** | **PA1** | **ADC1_IN2**, single-ended | day/night. Wire as divider: 3.3 V — LDR — PA1 — R(10 kΩ) — GND (or inverse; calibrate threshold in Settings) |
| UART to PC (Termite) | PC4=USART1_TX, PC5=USART1_RX | AF7, **115200 8N1** | ST-Link VCP (PCB rev C+, SB13/SB15) |
| RTC | internal | LSI clock source | F3-Disco has no guaranteed LSE crystal; LSI drift is fine because time is live-synced from PC (bonus 3) |

**Known cosmetic quirk (accepted):** PE8–PE15 are the Discovery's onboard LED ring; keypad
columns idling HIGH on PE11–PE14 keep 4 onboard LEDs lit. Harmless — push-pull columns are
strong enough; rows read solid HIGH through a pressed key. Do NOT "fix" this.

**Do-not-touch pins:** PA13/PA14 (SWD), PF0/PF1 (OSC via ST-Link MCO), PA0 (user button,
unused by us), PE15 (onboard LED, keep free).

### 1.2 Keypad logical layout (from project PDF, 4th column = our shortcut column)

```
col:      0          1          2          3 (extra)
row 0:  1 abc      2 def      3 ghi      A = Vol+
row 1:  4 jkl      5 mno      6 pqr      B = Vol-
row 2:  7 stu      8 vwx      9 yz       C = QuickLock
row 3:  back       0 space    LockType   D = Screenshot→UART
```
- **Letter mode (typing):** multi-tap — press `5` ×3 → `o`; tap window then commit.
  **Hold** a digit key ≥ HOLD_MS → prints the digit itself.
- **Nav mode (after LockType or in nav contexts):** 2=up 8=down 4=left 6=right, 5=select,
  0=delete, typing disabled.
- **back:** exits current app → Menu when on that app's **root** screen, **preserving app
  state** (re-entering resumes). Multi-screen apps (Note/Contact/PvZ/SMS/Settings PIN
  capture) consume BACK via `App.on_back()` to return to their parent screen first
  (e.g. PvZ Settings → PvZ menu; SMS compose → recipient list). A live PvZ game still
  exits to Menu so **Continue** works.
- Contact "Phone" field forces numeric mode; Note list starts in nav mode, editor in letter mode.

---

## 2. CubeMX (.ioc) CONFIGURATION — exact settings

### 2.1 System
- MCU: STM32F303VCTx (board: STM32F3DISCOVERY — start from *MCU* selector, not board
  selector, to avoid unwanted default pin locks; set pins manually per §1.1).
- **RCC:** HSE = **BYPASS Clock Source** (8 MHz from ST-Link MCO). LSI = ON (for RTC + IWDG).
- **Clock tree:** SYSCLK = 72 MHz (HSE 8 MHz → PLL ×9), AHB = 72, APB1 = 36 (timers ×2 = 72),
  APB2 = 72. ADC12 clock = PLL /1 (or AHB /1) — pick synchronous AHB/1 for simplicity.
- **SYS:** Debug = **Serial Wire** (frees PB3 for EXTI). Timebase source = **TIM1**
  (⚠ REQUIRED for FreeRTOS: SysTick belongs to the RTOS; HAL tick moves to TIM1).
- **IWDG:** Enabled, LSI/64, reload ≈ 2 s window (innovation/robustness; kicked from ui_task).
- **RTC:** Enabled (calendar), clock = LSI. No alarms needed.

### 2.2 GPIO / EXTI
Per §1.1. NVIC: enable EXTI3 (PB3), EXTI9_5 (PE7–9), EXTI15_10 (PE10).

### 2.3 Timers (all values for the given clock tree)

| Timer | Role | PSC | ARR | Rate | IRQ |
|---|---|---|---|---|---|
| TIM1 | HAL timebase (CubeMX auto) | auto | auto | 1 kHz | yes (HAL) |
| **TIM7** | **7-seg digit multiplexer** | 71 | 499 | 2 kHz (500 Hz/digit) | update IRQ |
| **TIM2** | **Seconds counter** (spec explicitly demands a dedicated interrupt-driven seconds timer for 7-seg content: game survival seconds, music M:SS, lock elapsed) | 7199 | 9999 | 1 Hz | update IRQ |
| **TIM6** | System soft-timer tick: keypad debounce/hold/multitap windows, LCD cursor blink, music progress bar, UI timeouts, auto-lock countdown | 71 | 999 | 1 kHz | update IRQ |
| **TIM3** | Melody sequencer (note durations; loads next note into TIM8) | 7199 | 9 | 1 kHz fixed (1 ms tick); ISR decrements a RAM `remain` counter per §5.3 — ARR itself is NOT reprogrammed at runtime | update IRQ |
| **TIM4** | ADC trigger: TRGO on update | 7199 | 499 | 20 Hz | no IRQ (TRGO only) |
| **TIM16** | PvZ game tick (zombie move/spawn, plant fire, collisions) | 7199 | 999 | 10 Hz | update IRQ |
| **TIM8_CH1** | Buzzer PWM, PC6 | 71 | `1e6/freq − 1` | audio | no IRQ |

Buzzer math @1 MHz counter clock: `ARR = 1000000/f − 1`; `CCR1 = (ARR+1) × duty`.
Loudness duty map: **0% → 0**, **50% → ≈ ARR/16**, **100% → ≈ ARR/2** (tune by ear; 50% duty
is max loudness for a square wave). Continuous 0–100 volume: `CCR1 = (ARR/2) × vol² / 10000`
(perceptual-ish taper).

### 2.4 ADC
- **ADC1 (LDR, PA1 = IN2)** and **ADC2 (pot, PB2 = IN12)**, both:
  Single-ended, 12-bit, right align, single conversion, sampling time ≥ 61.5 cycles
  (high-impedance sources), **External trigger = TIM4 TRGO, rising**, **EOC interrupt enabled**
  (End of single conversion). No DMA needed at 20 Hz. Run calibration
  (`HAL_ADCEx_Calibration_Start`) before start.
- ISR callback stores raw values; filtering in software (§5.4).

### 2.5 USART1
115200, 8N1, TX/RX on PC4/PC5 (AF7). **RX interrupt** (byte-wise via
`HAL_UART_Receive_IT` re-armed each byte, or LL RXNE). **TX by interrupt from ring buffer**
(never blocking-transmit inside ISRs; never `HAL_UART_Transmit` blocking anywhere except
the fatal-error handler).

### 2.6 FreeRTOS (CMSIS-RTOS v2)
- Middleware → FreeRTOS, CMSIS_V2. Heap_4, heap size ≥ 12 KB (tune; F303VC has 40 KB SRAM).
  **As configured in CubeMX Phase 0: `TOTAL_HEAP_SIZE` = 12288 Bytes.** (Tried 15360 during
  Heap Usage tab troubleshooting; the warning was a stale GUI display bug, not a real config
  issue — proven by direct inspection of generated `FreeRTOSConfig.h`. Reverted to the plan's
  original 12 KB minimum since no real headroom problem existed.)
- `configMAX_SYSCALL_INTERRUPT_PRIORITY` = 5 (NVIC group 4). Any ISR calling
  `...FromISR()` APIs must have NVIC priority **numerically between 5 and 15** (confirmed via
  CubeIDE NVIC validator tooltip — not just "≥5" with no upper bound).
- Tasks/queues in §4.3.
- **`USE_NEWLIB_REENTRANT` = Disabled.** CubeMX flags this as a code-gen warning ("must be set
  to make sure newlib is fully reentrant"), but enabling it costs a `struct _reent` per task
  (~500+ bytes each), which conflicts with the tight §9 RAM budget (~20 KB for
  tasks+heap+buffers+game arrays out of 40 KB total). Decision: leave disabled since
  string/LCD formatting is centralized in `ui_task` (rule §0.2 — LCD-only task) rather than
  spread across many concurrently-running tasks, minimizing the real-world risk of newlib
  `_reent` state corruption. Revisit if a build/runtime issue traces back to this later.

### 2.7 NVIC priorities (grouping 4; lower number = more urgent)

| ISR | Prio | Calls RTOS API? |
|---|---|---|
| TIM7 (7-seg mux) | 1 | NO (pure GPIO writes — must never jitter) |
| ~~TIM3 (melody) \| 2 \| NO (pure timer regs)~~ — **SUPERSEDED, see Phase 4 deviation note (section 9.6)** |
| ~~TIM1 (HAL tick) \| 4 \| no~~ — **SUPERSEDED, see deviation note below** |
| EXTI3/9_5/15_10 (keys) | 5 | yes (queue post) |
| TIM6 (1 kHz tick) | 5 | yes |
| **TIM1_UP_TIM16 (shared vector: HAL tick + game tick)** | **15** | **yes (TIM16 side posts EV_GAME_TICK)** |
| TIM2 (1 Hz) | 6 | yes |
| USART1 | 6 | yes |
| ADC1_2 | 7 | yes |

**⚠ DEVIATION (found during Phase 4 hardware test — TIM3 melody-end reset, see section 9.6 for
full root cause): TIM3 changed from priority 2 to priority 5.** The original assumption above
("TIM3: NO RTOS calls, pure timer regs") held for `buzzer.c`'s per-tick note-timing logic, but
`Buzzer_TickISR()` also calls `Event_Post()` (an RTOS queue-post wrapper) exactly once, at
natural melody end — a case the original design table did not anticipate. Per the
`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5` rule already established for the TIM1/TIM16 row
below, any ISR calling an RTOS API must sit at priority >= 5, so TIM3 moved from 2 to 5 (matching
TIM6's own priority) in both `SmartPhone_STM32F303.ioc` and `stm32f3xx_hal_msp.c`. No functional
regression: TIM3's actual note-timing behavior (ARR/PSC, 1 ms tick) is unchanged, only its
NVIC urgency relative to other interrupts.

**⚠ DEVIATION (found during Phase 0 CubeMX config, confirmed in CubeIDE NVIC panel):**
On STM32F303, **TIM1 update and TIM16 share a single NVIC vector** (`TIM1_UP_TIM16_IRQn`) — they
are NOT independently configurable, contrary to the original table above which listed them as
separate priorities (TIM1=4, TIM16=6). Since TIM16's ISR posts `EV_GAME_TICK` via a
`...FromISR()` RTOS call, the shared vector must respect the §2.6 rule
(`configMAX_SYSCALL_INTERRUPT_PRIORITY` = 5 ⇒ priority must be numerically **between 5 and 15**,
per the STM32CubeIDE NVIC validator tooltip: "Value must be between 5 and 15. Priority must be
>= FREERTOS priority min if interrupt handler calls system functions."). **Resolution: once
FreeRTOS middleware (CMSIS_V2) was enabled in CubeMX, CubeMX auto-designated TIM1 as the HAL
timebase source (since FreeRTOS claims SysTick) and force-locked the shared
`TIM1_UP_TIM16_IRQn` row to priority 15, greyed out and non-editable in the NVIC table.** This
supersedes the earlier "set to priority 5" resolution — priority 15 is still fully RTOS-safe
(same lock tier as `SysTick`/`PendSV`, both also fixed at 15), just less preemptive-urgent than
originally chosen. No functional impact expected. Note: the "Uses FreeRTOS functions" checkbox
for this row is also locked/unchecked by CubeMX and cannot be manually toggled — this is only a
CubeMX code-generation metadata hint and does NOT prevent manually adding
`osMessageQueuePut(...FromISR)` inside the handler body in Phase 8. In `stm32f3xx_it.c`, the
single `TIM1_UP_TIM16_IRQHandler()` must dispatch to both `HAL_TIM_IRQHandler(&htim1)` and
`HAL_TIM_IRQHandler(&htim16)` (CubeMX auto-generates this correctly once both timers are
enabled — verify at Phase 8 when TIM16/game tick is wired up).

---

## 3. SOFTWARE ENGINEERING — files and responsibilities

All app code lives in `Core/` (submission requires it). Layout:

```
Core/
  Inc/  (all headers)             Src/  (all implementations)
  ── config ──────────────────────────────────────────────
  pinmap.h          — every pin/port + timer/channel macro in ONE place
  app_config.h      — tunables: HOLD_MS, TAP_WINDOW_MS, DEBOUNCE_MS, LDR default
                      threshold, auto-lock seconds, max notes/contacts/songs, etc.
  ── kernel ──────────────────────────────────────────────
  events.h/.c       — event types (enum + payload struct), ISR→task queue plumbing
  softtimer.h/.c    — 1 kHz-tick software timers (countdowns, periodic flags)
  ── drivers (each = header + impl, no app logic inside) ─
  LiquidCrystal.h/.c— (existing, reused) HD44780 4-bit driver
  cgram.h/.c        — CGRAM manager: per-screen banks of ≤8 custom chars, load-on-demand
  seg7.h/.c         — 4-digit display buffer + TIM7 mux ISR body + modes
                      (OFF / MUSIC "M.SS n" / GAME "SS PP" / TIME "HH.MM")
  keypad.h/.c       — EXTI scan, debounce, hold, multi-tap engine, input modes,
                      emits high-level KeyEvents (CHAR, DIGIT, NAV_UP/DOWN/L/R,
                      SELECT, BACK, DELETE, LOCKTYPE, SHORTCUT_A..D)
  buzzer.h/.c       — note-on/off (freq,duty), melody player (TIM3-sequenced),
                      SFX one-shots, background/game channel arbitration
  analog.h/.c       — ADC EOC handling, median/EMA filter, pot→0..100 volume,
                      pot→seek position, LDR→day/night with hysteresis
  serial.h/.c       — TX ring buffer + IT drain, RX line assembler, LOG(fmt,...)
  cmdparse.h/.c     — command dispatcher: /start /reset /lock /setting-N-V,
                      /time-, /piano, /songup protocol; invalid-input rejection
  storage.h/.c      — flash persistence (last 2 pages @0x0803F000, 2 KB pages):
                      magic + version + CRC blob {settings, notes, contacts,
                      uploaded songs, high scores}. save()/load()/wipe_game_only()
  rtc_time.h/.c     — RTC init (LSI), set-from-epoch, get HH:MM:SS
  leds.h/.c         — green/red LED patterns (solid, blink-n, tick-driven)
  ── UI framework ────────────────────────────────────────
  ui.h/.c           — screen abstraction: dirty-flag render manager; ONLY module
                      the LCD task talks to; owns 20×4 shadow framebuffer and
                      diff-based redraw (minimizes slow HD44780 writes)
  widgets.h/.c      — scroll list, blinking cursor cell, progress bar, volume icon,
                      text field w/ insertion cursor
  ── applications (state machines; NEVER touch hardware directly) ─
  app.h             — the App interface: {on_enter, on_event, on_tick, render,
                      on_suspend} function-pointer struct
  phone.h/.c        — top-level shell: boot logo, menu, lock, app switching with
                      state preservation, settings store, log tagging
  app_menu.c        — icon grid (6×1 / 3×2 / 2×3), blinking selection
  app_note.c        — scrollable note list, +, editor (title/body), auto-scroll
  app_contact.c     — same engine as note; Phone field numeric-only
  app_music.c       — playlist (≥4 built-in + uploaded), play/pause/prev/next,
                      live progress bar, 3-state volume icon, pot seek↔volume
                      via LockType, background playback, auto-next, shuffle/repeat
  app_pvz.c         — game: 17×4 grid, entities, lives, score, day/night plants,
                      settings (damage, zombie speed, difficulty, start plants),
                      special zombie/plant, continue-state, 7-seg score mode
  app_settings.c    — LDR threshold, icon layout, UART-settings enable, volume,
                      PIN set/change, auto-lock time, 7-seg time-mode toggle
  app_info.c        — team names + student numbers
  app_lock.c        — lock screen: live clock, PIN entry / any-key unlock,
                      exact-state resume
  app_piano.c       — bonus: pyserial live piano mode screen
  app_sms.c         — Phase 11: compose+send SMS via PC bridge (§5.10); reuses
                      app_contact.c's storage-backed list to pick a recipient
  ── main ────────────────────────────────────────────────
  main.c            — CubeMX init + task creation + osKernelStart(); USER CODE
                      while(1) stays empty/unreachable (rule §0.2)
  stm32f3xx_it.c    — ISRs: thin, call driver hooks only
host_tools/ (NOT in Core/, PC side, Python + pyserial)
  piano.py          — keyboard→note streaming
  song_upload.py    — send named melody to device
  time_sync.py      — push live epoch to device every second
  sms_bridge.py     — Phase 11: listens for SMS_SEND lines, POSTs to the
                      melipayamak API, replies SMS_RESULT (§5.10); requires
                      the `requests` package (not in stdlib)
```

**Header discipline (class style):** every `.h` has `#ifndef X_H / #define X_H / #endif`;
constants as `#define` or `const`; shared globals declared `extern` in header, defined once
in the `.c`; function prototypes for every public function; `volatile` on every variable
shared between ISR and task context.

---

## 4. ARCHITECTURE

### 4.1 Data flow (the one diagram to remember)

```
 EXTI keys ─┐
 TIM6 1kHz ─┤ ISRs (thin) ──► eventQueue ──► app_task (state machines,
 TIM2 1Hz  ─┤                                 phone shell, game logic*)
 TIM16 game─┤                                     │ sets state + dirty flags
 ADC EOC   ─┤                                     ▼
 UART RX   ─┘                              ui_task: while(1){ LCD only }
 TIM7 mux ISR ──► seg7 framebuffer (no RTOS)      │
 TIM3 melody ISR ─► TIM8 PWM regs (no RTOS)       ▼
 UART TX IT ◄── logQueue ◄── LOG() from anywhere  HD44780
```
\* PvZ per-tick entity updates run in app_task on a `EV_GAME_TICK` event from TIM16 —
keeps heavy logic out of ISRs while remaining interrupt-*driven* (the tick originates
in an interrupt; nothing is polled).

### 4.2 Event system
`events.h`: single `Event {uint8_t type; uint8_t a; uint16_t b; uint32_t c;}`.
Types: `EV_KEY` (KeyEvent in a/b), `EV_TICK_1S`, `EV_GAME_TICK`, `EV_ADC_POT`,
`EV_ADC_LDR_EDGE` (day↔night crossed), `EV_UART_CMD` (index into parsed cmd slot),
`EV_SONG_END`, `EV_AUTOLOCK`, `EV_BTN_LOCK`.
Queue length 16. ISR side: `osMessageQueuePut(..., 0 timeout)`; on overflow, drop + count
(never block an ISR).

### 4.3 FreeRTOS tasks

| Task | Prio | Stack | Loop contents |
|---|---|---|---|
| `ui_task` | low (osPriorityLow) | 512 w | **only** `ui_render_dirty(); cgram_apply(); IWDG kick; osDelay(30)` — LCD code only (rule §0.2) |
| `app_task` | normal | 1024 w | `osMessageQueueGet(eventQueue)` → `phone_dispatch(ev)` |
| `log_task` | below normal | 256 w | drain `logQueue` → UART TX ring (or TX IT directly) |
| `storage_task` | low | 256 w | waits on save-request flag; performs flash erase/write (§5.6) |

Everything else is ISR context. No other tasks. Mutex `lcdStateMutex` guards app-state
snapshots read by `ui_task` (or better: apps write into a render-model struct guarded by
the mutex; ui_task copies then draws).

### 4.4 State preservation (spec: back→menu→re-enter resumes; lock resumes exactly)
Each app owns a static context struct (cursor positions, scroll offsets, editor buffers,
game grid...). `on_suspend()` is a no-op by design — state simply persists in the static
struct. `phone.c` never zeroes app contexts except on `/reset` (full `NVIC_SystemReset()`)
or explicit "new game". Lock screen is an overlay: it snapshots which app was active and
returns to it without calling `on_enter` re-init.

---

## 5. SUBSYSTEM SPECS (implementation notes for agents)

### 5.1 Keypad engine (keypad.c) — the trickiest module; follow exactly
State machine per keypress, all timing from TIM6 1 kHz tick:
1. **IDLE:** all 4 columns HIGH. Any key press pulls a row HIGH → EXTI rising fires.
2. **EXTI ISR:** mask further row EXTIs (NVIC or per-line), record timestamp, state=SCAN,
   request scan on next tick (do NOT scan inside EXTI — bounce).
3. **SCAN (in TIM6 ISR after DEBOUNCE_MS ≈ 25 ms):** drive all columns LOW; then for
   col = 0..3: col HIGH, tiny settle (few NOPs), read rows, col LOW. First (row,col) hit =
   key. If none (bounce/ghost), restore & re-arm EXTI, state=IDLE.
4. **HELD:** columns restored except keep resolving release: keep the pressed key's column
   HIGH and watch its row. Row LOW ≥ DEBOUNCE_MS → release. If held ≥ HOLD_MS (900 ms) in
   letter mode → emit `DIGIT` event immediately, mark consumed.
5. **RELEASE:** if not consumed by hold → feed to **multi-tap engine**: same key within
   TAP_WINDOW_MS (1000 ms) cycles `1→a→b→c→1...`; different key or window expiry commits
   the pending char (emit `CHAR`). In nav mode, keys map directly (2/4/6/8/5/0/back/locktype)
   with no multi-tap. Restore all columns HIGH, clear EXTI pending flags
   (`__HAL_GPIO_EXTI_CLEAR_IT`) **before** unmasking, state=IDLE.
- **Ghosting:** 4×4 with no diodes — with ≥3 simultaneous keys phantom keys appear. Policy:
  ignore multi-key scans (treat as invalid, log `[KEY] ghost/multi ignored`).
- LockType toggles global `input_mode` (TYPE↔NAV) and is also the pot-function toggle inside
  Music (spec). 0 = space (TYPE) / delete (NAV). Every emitted event is logged.

### 5.2 7-segment (seg7.c) — two timers per spec
- TIM7 ISR @2 kHz: turn all digits off → write BCD of `frame[i]` to PD0–3 (+ DP on PD7 if
  `dpMask&`), enable digit i, `i=(i+1)&3`. Off-before-switch kills ghosting.
- TIM2 ISR @1 Hz: increments the active seconds source and posts `EV_TICK_1S`.
- Modes (owned by phone shell): `SEG_OFF` (default — spec: "خاموش در تمام مدت"),
  `SEG_MUSIC`: `[min][DP][sec10][sec1] … rightmost digit = song number` → layout per spec:
  digit3(right)=song#, digits2..1=seconds, DP, digit0(left)=minutes → shows `M.SS n`.
  `SEG_GAME`: left two = survival seconds (wraps at 60 → +1 score), right two = score.
  `SEG_TIME` (bonus 3, toggled in Settings): `HH.MM` from RTC.
- Blank leading zeros where the spec's examples do; 7448 shows garbage >9 — clamp.
- ⚠ Flash writes (§5.6) stall the bus → mux freezes ~20 ms per erased page. Acceptable;
  storage_task saves rarely (explicit save / lock / dirty+idle).

### 5.3 Buzzer & music engine (buzzer.c)
- Note = `{uint16_t freqHz; uint16_t ms;}` ; melody = `const Note[]` in ROM + name.
  ≥4 built-in melodies + up to N uploaded ones in RAM (mirrored to flash blob).
- TIM3 1 ms ISR: `if(--remain==0)` load next note: set TIM8 ARR/CCR (or CCR=0 for rest);
  at end: post `EV_SONG_END` (music app auto-nexts). Pause = freeze index, CCR=0.
- Channel arbitration: `BG_MUSIC` vs `GAME_MUSIC` vs `SFX`. PvZ music **pauses** phone
  music (spec). SFX (UI click 4 kHz/20 ms, error 200 Hz/100 ms, jingles) briefly override
  the melody note then restore — single hardware channel, priority SFX>current music note.
- Volume: global 0–100 (settings/pot/keys), rendered to LCD as nearest of 3 states
  (0 / 50 / 100 icon) per spec; duty math in §2.3.

### 5.4 Analog (analog.c)
- TIM4 TRGO 20 Hz → both ADCs convert → EOC ISRs stash raw.
- Filter: 5-sample median then EMA (α≈0.3). Pot deadband ±2/100 to stop volume jitter
  (log/apply only on change). Music mode: pot value maps to seek % while `pot_role=SEEK`;
  LockType flips to VOLUME (spec) — pot writes 0..100, LCD shows nearest 3-state.
- LDR: compare vs `settings.ldr_threshold` with hysteresis ±5%; on crossing post
  `EV_ADC_LDR_EDGE` (PvZ swaps day/night plant behavior; log `[LDR] day→night`).

### 5.5 Serial + commands (serial.c / cmdparse.c)
- TX: 512-B ring, TXE-interrupt drain; `LOG("[MENU] enter Note")` from any context
  (from ISR: copy into ring with IRQs briefly masked, or route via log_task queue).
- RX: byte ISR appends to line buffer until `\n`/`\r` → hand line to cmdparse in app_task
  context (post event).
- Commands (reject anything else: reply `ERR unknown/invalid: <line>`):
  - `/start` — boot the phone UI: show custom-character logo (animation, §7-I12) then Menu.
    Before `/start` the device idles on a "waiting" screen.
  - `/reset` — log, small delay to flush TX, `NVIC_SystemReset()`. Notes/contacts/settings/
    uploaded songs survive (flash); game continue-state and runtime volume do not (RAM).
  - `/lock` — same path as auto-lock/PB3: overlay lock screen, live time; PIN if set else
    any key unlocks; resume exact prior state.
  - `/setting-{NAME}-{VALUE}` — only accepted when Settings→"UART settings"=ON (spec);
    NAME ∈ {ldr, icons, volume, uartset, autolock, segtime}; validate range, apply, log,
    reply `OK`.
  - Bonus protocol: `/time-{unix_epoch}` (time_sync / bridge `:time` or `--time-sync`,
    one-shot) → RTC set; re-send manually if LSI drifts during a long demo;
    `/piano-on`, `/pn-{freq}`, `/pf` (note on/off, ≤10 ms latency path: parsed in RX ISR
    fast-path for these two verbs only);
    `/songup-{name}-{count}` then `count` lines `freq,ms` then `/end` → validate, store,
    ack `OK stored as #k` / `ERR`.

### 5.6 Storage (storage.c)
- Region: last two 2-KB pages of 256 KB flash (0x0803F000, 0x0803F800), linker untouched
  (app ≪ 252 KB). Blob: `{magic 0x50484F4E, version, len, payload{settings, notes[],
  contacts[], songs[], highscores}, CRC32}` — write to alternate page each save
  (simple wear/atomicity: newest valid CRC wins on boot).
- Save triggers: leaving Note/Contact editor with changes, Settings change, song upload,
  new high score, and on `/lock`. Executed only in `storage_task` (never ISR), scheduler
  suspended around `HAL_FLASH` erase/program of the region.
- Budget (fits easily in one 2 KB page): 10 notes × (16 title + 80 body), 10 contacts ×
  (16 + 12), 2 uploaded songs × 64 notes, settings ≈ 32 B, scores 16 B ≈ 1.6 KB.

### 5.7 UI framework (ui.c / widgets.c / cgram.c)
- 20×4 shadow buffer + per-cell dirty diffing → `ui_render_dirty()` writes only changed
  cells (HD44780 ≈ 40 µs/char + our delays; full redraw is visible, diffing is not).
- **CGRAM budget is 8 custom characters TOTAL** — hard HD44780 limit. `cgram.c` defines
  named banks: `BANK_LOGO`, `BANK_MENU_ICONS_DAY`, `BANK_MENU_ICONS_NIGHT`, `BANK_MUSIC`
  (volume states, progress fills), `BANK_PVZ` (plant day, plant night, zombie, special,
  heart, bullet). App's `on_enter` requests its bank; cgram loads lazily. Never assume a
  glyph is resident across apps.
- Widgets: `list_view` (scroll window, cursor row, "+" tail per spec screenshots),
  `blink_cursor` (block char toggled by TIM6-derived 500 ms soft timer — menu icon blink
  and text insertion cursor both use it), `progress_bar` (custom-char partial fills),
  `volume_icon` (3 states), `text_field` (insert/delete at cursor, auto-scroll when full —
  spec: long note bodies scroll and follow the cursor).

### 5.8 PvZ (app_pvz.c)
- Model: `grid[4][17]` cells ∈ {empty, plant(type,hp), zombie handled separately};
  `zombies[] {row, x_fixedpoint, hp, type}`, `bullets[] {row, x}`, lives=2, score,
  survival seconds. LCD col 0–2 = HUD (hearts, score, plant inventory day/night counts,
  `++` add), cols 3–19 = 17-wide field (matches spec screenshot).
- TIM16 10 Hz → `EV_GAME_TICK` → in app_task: move zombies (speed setting = fixed-point
  step), plants fire (rate scaled by day/night: day-plants shoot in day, night-plants at
  night per LDR), bullets travel, collisions (bullet hits front zombie: hp−damage; zombie
  adjacent to plant eats it over time), spawn per difficulty, zombie reaches col 3 → −1
  life (red LED blink, sad SFX) and clears; 0 lives → game over jingle, high-score check.
- +1 score per zombie kill; TIM2 seconds: at 60 → wrap + bonus point (spec). Every 10
  points → +1 plant inventory (type per current day/night). Start: pick 4 plants
  (mix day/night) on a pre-screen (spec).
- New game / **Continue** (greyed when no saved run — RAM-only per user decision) / Setting
  (damage, zombie speed, difficulty=spawn rate, starting plants).
- **Special units (bonus 4):** "Tank zombie" (3× HP, distinct glyph, spawns every Nth) and
  "Ice plant" (hit slows zombie 50% for 3 s). Keep both; costs one extra CGRAM glyph each
  (fits BANK_PVZ if day/night plant glyphs share with tint-by-context… if over 8, drop
  bullet glyph → use `-`).
- Game music via buzzer GAME channel; entering game pauses phone music, leaving resumes.

### 5.9 Lock / boot / shell (phone.c, app_lock.c)
- Boot: idle "press /start" screen → `/start` → animated cactus/custom logo (spec demands
  custom-character logo) + boot jingle → Menu.
- Lock sources: `/lock`, PB3 button, auto-lock timeout (innovation, Settings). Lock shows
  **live time** — RTC HH:MM:SS (bonus 3 gives real time; before first `/time` sync it shows
  RTC's default epoch — acceptable, log a `[RTC] unsynced` note). PIN set in Settings →
  numeric entry to unlock; no PIN → any key. Unlock resumes the exact pre-lock screen/state.

### 5.10 SMS (app_sms.c / host_tools/sms_bridge.py) — Phase 11, added post-hoc after
user request; this subsystem is NOT in the original project PDF spec — it is an
explicitly user-requested addition, using the melipayamak SMS REST API
(`https://console.melipayamak.com/api/send/simple/{token}`, POST JSON
`{"from","to","text"}` → JSON `{"recId","status"}`).

**Why a PC bridge is required (verified, not guessed):** the STM32F303 Discovery board has
no WiFi/GSM/Ethernet peripheral in §1's hardware map — the only external link is USART1 on
PC4/PC5 to the ST-Link virtual COM port (a wired link to whatever PC is on the other end of
the USB cable). The MCU cannot itself open an HTTPS connection. `sms_bridge.py` is a new
PC-side pyserial script (same family as `piano.py`/`song_upload.py`/`time_sync.py`) that
performs the actual HTTP POST on the MCU's behalf.

**UART protocol (new verb family, alongside `/setting-` and the bonus-protocol verbs in
§5.5) — pipe-delimited, not dash-delimited, because SMS text may contain spaces, dashes,
and punctuation that would break the existing `/name-value` parsing:**
- MCU → PC: `SMS_SEND|<to>|<text>\n` — `<to>` is the recipient's raw digits (from a
  Contact's Phone field or manual entry, §5.10's UI below); `<text>` is everything after the
  second `|` to end of line, unescaped.
- PC → MCU: `sms_bridge.py` performs `requests.post(url, json={"from": FROM_CONST, "to":
  to, "text": text})` and replies over the same serial port:
  - Success: `SMS_RESULT|OK|<recId>`
  - Failure (HTTP error, non-2xx, or a non-empty `"status"` field in the JSON body):
    `SMS_RESULT|ERR|<status-or-http-code>`
- `cmdparse.c` gets a new `SMS_RESULT|` branch (recognized without a leading `/`, since it's
  a reply, not a command) that posts an event so `app_sms.c` can update its "Sending…"
  screen to "Sent" or "Failed" instead of waiting indefinitely. A local ~10 s deadline
  checked from `app_task` via `HAL_GetTick()` (same established stand-in for the still-
  unbuilt `softtimer.h` as `Widgets_BlinkOn()`, §9.5 / §9.11) covers the case where the
  bridge script isn't running or the network call itself times out.

**Fixed sender number.** `"from": "50004001980947"` is a constant baked into
`sms_bridge.py` only — the MCU never sends or stores it, so it isn't retyped or exposed by
the on-device UI (this was an explicit user-confirmed decision, not a guess).

**On-device UI (app_sms.c), per user confirmation "it should use app_contact as well":**
1. **Recipient step** — reuses `app_contact.c`'s storage-backed list/scroll engine (same
   `widgets.h` list view) to show existing contacts; a pinned "Manual number" row at the top
   of the list (mirroring Note's "+" tail convention) drops into a numeric-only text field
   (same field type as Contact's Phone field) for a one-off number. Selecting a contact
   copies its stored Phone field as `<to>` and advances to the text step.
2. **Text step** — a `text_field` widget (§5.7, same insert/delete/auto-scroll engine as
   Note's body editor) for the SMS text; SELECT sends it.
3. **Sending / result screen** — shows "Sending…" while awaiting `SMS_RESULT|...`, then
   either "Sent" (+ `recId`) or a **generic English** "Send failed" message on error, with
   BACK returning to Menu. Auto-lock/back-navigation follow the same state-preservation
   rules as every other app (§4.4).

**Persian error text cannot be shown on the LCD (design constraint, not guessed away).**
melipayamak's `"status"` field on failure is Persian free text
(`"شرح خطا در صورت بروز"` = "error description if one occurs"), and the HD44780's built-in
character ROM has no Farsi/Arabic glyphs — rendering it would require a custom font table
far beyond the 8-glyph CGRAM budget (§5.7, already near its ceiling from PvZ). Resolution:
the LCD only ever shows a generic "Sent"/"Send failed" line; the *full* raw `status` string
(whatever script/language it's in) is still emitted PC-side via `sms_bridge.py`'s own stdout
and, redundantly, echoed back to the MCU's `LOG()` (visible on Termite, not the LCD) inside
the `SMS_RESULT|ERR|...` line itself — so nothing is silently lost, it's just not rendered
on-device.

**No persistent SMS history/outbox in this phase** (YAGNI — neither the melipayamak spec
nor the user's request mentions saving sent messages; storage.c's flash budget, §5.6, is not
extended for this). Sent/failed status is RAM-only and does not survive `/reset` — consistent
with PvZ's continue-state precedent (§5.8) for "session-only, not persisted" data. Could
become a future innovation-menu item (§7) if requested later.

**Menu integration.** `app_menu.c`'s `s_items[]` grows from `MENU_ITEM_COUNT=6` to 7 (new
`AppSms` entry, new `CGRAM_ICON_SMS` glyph — 7th glyph in `BANK_MENU_ICONS_DAY`/`_NIGHT`,
still ≤8 per bank, §5.7's ceiling). The three existing menu grid layouts (`6×1`/`3×2`/`2×3`)
are all exactly 6 cells and can no longer hold 7 icons without redesign — replace them with
three 7-capable layouts that still respect the LCD's 4-row physical limit: `7×1` (1 row, 7
cells, tightest — same "icon only, no label" tradeoff the current 6×1 already has),
`4×2` (2 rows, 8 cells, 1 unused — generous label width), `3×3` (3 rows, 9 cells, 2 unused,
most generous label width). `icon_layout`'s valid range stays 0–2 (same
`/setting-icons-{0,1,2}` validation in `cmdparse.c`), just remapped to the new dimensions.
`menu_move()`'s existing `idx < MENU_ITEM_COUNT` guard already tolerates the trailing unused
cells in `4×2`/`3×3` with no further logic change needed.

**Single-COM-port constraint (inherited from §6 Phase 9's own note):** `sms_bridge.py` must
hold the same serial port Termite would otherwise use — it cannot run at the same time as a
Termite session, same sequential-demo constraint already documented for `piano.py`/
`song_upload.py`/`time_sync.py`.

---

## 6. PHASED PLAN (each phase ends demo-able; agents work top-to-bottom)

**Phase 0 — Project skeleton (½ day).** CubeMX per §2, generate, add file tree §3 as empty
modules, FreeRTOS tasks stubbed, build clean, LED blink from ui_task + `hello` on Termite.
*Exit:* .ioc matches §2 exactly; 72 MHz verified (MCO or timing LED).

**Phase 1 — Driver bring-up (1–2 days).** LiquidCrystal ported & diff-renderer working;
seg7 with TIM7+TIM2 counting seconds on display; keypad raw scan printing (row,col) to
Termite; verify digit-select polarity, keypad EXTI lines. *Exit:* all peripherals proven
in isolation, interrupt-driven.

**Phase 2 — Input engine + events (1–2 days).** Full §5.1 machine: debounce, hold-digit,
multi-tap, modes, ghost rejection; event queue; logging of every key. *Exit:* typing
`hello world` into a debug text field on LCD works; hold-5 prints `5`.

**Phase 3 — UI framework + shell (2 days).** cgram banks, widgets, phone shell, boot logo,
Menu with 3 icon layouts + blinking selection, Info app, back-navigation with state
preservation. *Exit:* navigate Menu↔Info, layouts switchable in code.

**Phase 4 — Sound + analog (2 days).** Buzzer note/melody/SFX engine (§5.3), 4 melodies,
ADC pipeline (§5.4), volume by keys+pot with 3-state icon, LDR day/night events logged.
*Exit:* melody plays with pause/next, volume audibly changes, covering LDR flips day/night log.

**Phase 5 — Music Player app (1–2 days).** Full spec UI: name, controls (5/4/6/2/8),
live progress bar, background playback + auto-next, pot seek↔volume via LockType,
7-seg `M.SS n` mode. *Exit:* every music-spec sentence demonstrably true.

**Phase 6 — Note + Contact + storage (2–3 days).** List/editor engines, auto-scroll, mode
switching per spec, numeric-only Phone field, flash persistence surviving power cycle,
`/reset` semantics (saves survive, game doesn't). *Exit:* create/edit/delete notes &
contacts; power-cycle keeps them.

**Phase 7 — Shell completion (1–2 days).** Lock screen (PIN, live time, exact resume),
Settings app (all spec params + PIN + auto-lock + segtime), UART command set complete with
invalid-input rejection, logging audit over every app. *Exit:* full PDF command table passes.

**Phase 8 — PvZ (3–4 days).** §5.8 completely, incl. special units, game settings, continue,
7-seg game mode, LDR-driven plant behavior, LEDs. *Exit:* full game loop, lose/survive,
scores persist.

**Phase 9 — PC bonuses (2 days).** `time_sync.py` + RTC + Settings 7-seg time mode;
`piano.py` low-latency live notes; `song_upload.py` + storage of uploaded songs appearing
in Music. *Exit:* all three demoable from one PC session alongside Termite (note: one COM
port — scripts and Termite can't be open simultaneously; demo sequentially).

**Phase 10 — Hardening + polish (2 days).** IWDG, stack high-water checks, queue-overflow
counters, CGRAM audit per screen, log-format pass `[APP] action`, README, final .ioc/code
consistency, packaging per page 1.

**Phase 11 — SMS via PC bridge (1 day).** Not in the original PDF spec — added post-hoc by
explicit user request (§5.10 has the full design). `app_sms.c` (recipient picker reusing
Contact's list engine + manual-entry fallback, text compose, send/result screen),
`cmdparse.c`'s new `SMS_SEND|`/`SMS_RESULT|` pipe-delimited verb pair, `host_tools/
sms_bridge.py` (pyserial ↔ melipayamak HTTP bridge, fixed `from` constant), `app_menu.c`
7-icon grid + new `4×2`/`3×3`/`7×1` layouts, new `CGRAM_ICON_SMS` glyph. *Exit:* select a
saved contact (or type a number manually), compose text, send, and see "Sent"/"Send failed"
on the LCD while `sms_bridge.py` shows the real melipayamak response (including any Persian
`status` text) on the PC side; confirm the recipient phone actually receives the message.

Dependencies: 2←1, 3←2, 4←1, 5←{3,4}, 6←3, 7←{3,6}, 8←{3,4}, 9←{5,7}, 11←{3,6}. Phases 4/6
can run parallel to 5 if two people split (one owns drivers/sound, one owns UI/apps).

---

## 7. INNOVATION MENU (pick for the +20; ranked by impact ÷ effort)

| # | Feature | Effort | Why it lands |
|---|---|---|---|
| I1 | **Snake mini-game** as a 7th app (reuses PvZ grid/tick engine, nav keys) | Low | "Two games" demos beautifully; ~1 day on existing engine |
| I2 | **PIN + auto-lock + sleep**: settable PIN, auto-lock after N s idle, `__WFI()` while locked (wake on EXTI) | Low | Fills a spec gap (lock mentions a PIN but never how it's set) + real low-power engineering talking point |
| I3 | **Sound design**: key click, error buzz, boot/game-over jingles, per-app enter chirp | Low | Instantly perceptible polish |
| I4 | **LCD screenshot to Termite** (`D` key / `/shot`): dumps 20×4 framebuffer as ASCII | Low | Unique, great for the report; trivial since we own a shadow buffer |
| I5 | **High scores + achievements** in PvZ, flash-persisted ("Survive 3 min", "10 kills") | Low-Med | Uses storage we already built |
| I6 | **Melody composer app**: enter notes on keypad, preview, save as playable song | Med | Closes the loop with song storage; very demo-friendly |
| I7 | **Day/night themed menu icons** (CGRAM bank swap on LDR edge) | Low | Makes the LDR visible outside the game |
| I8 | **Music shuffle/repeat + animated equalizer** custom-char animation while playing | Low-Med | Music app looks "real" |
| I9 | **IWDG watchdog + fault telemetry** (`/health`: uptime, stack HWM, queue drops) | Low | Engineering-rigor marks; mostly free from Phase 10 |
| I10 | **T9-lite autocomplete** in Note (small const dictionary, suggestion on line 4, `6`=accept) | Med-High | Flashiest, but real work — only if time remains |
| I11 | **4th keypad column shortcuts** (Vol±, QuickLock, Screenshot) | Low | Uses otherwise-dead keys |
| I12 | **Animated boot logo** (3–4 CGRAM frames, growing cactus) | Low | Spec wants a custom logo anyway; animation is +ε effort |

**Recommended bundle for +20:** I1, I2, I3, I4, I5, I7, I11, I12 (all low effort) + I6 or I8.

**Shipped (innovation bundle, restore `8bc5efe` → this work):** I4 → I3 → I2 → I12 → I5 → I8 → I10.
I9 already landed with Phase 10 (`/health`). Not shipped: I1 Snake, I6 composer.
See §9.14 for implementation notes.

---

## 8. RISK REGISTER (agents: read before coding)

1. **FreeRTOS vs while-only-LCD rule** — verify with TA (see §0.2). Mitigation baked in: §9.1.
2. **CGRAM 8-glyph ceiling** — every screen design must name its bank up front; PvZ is at
   the limit. Any new glyph request goes through cgram bank audit.
3. **RAM** (40 KB): tasks+heap+buffers+game arrays budget ≈ 20 KB — track
   `uxTaskGetStackHighWaterMark` in Phase 10.
4. **Flash-write display freeze** (§5.6) — save policy already minimizes; never save mid-game.
5. **One COM port** for Termite AND pyserial tools (now including `sms_bridge.py`, §5.10) —
   demo sequentially; scripts echo device logs so nothing is lost.
6. **Keypad ghosting** without diodes — multi-key scans rejected (§5.1); document in report.
7. **LSI RTC drift** (~±5%) — irrelevant while time_sync pushes every second; log
   `[RTC] unsynced` otherwise.
8. **HAL timebase must be TIM1, not SysTick** (FreeRTOS owns SysTick) — set in CubeMX Phase 0
   or HAL_Delay deadlocks inside RTOS.
9. **Class-style compliance** — no dynamic allocation outside FreeRTOS heap, no VLAs, no
   function pointers beyond the documented App interface if TA objects (fallback: switch on
   app id — 1-hour refactor).

## 9. FALLBACKS

### 9.1 Bare-metal fallback (if TA rejects FreeRTOS reading of the while rule)
The design degrades gracefully: delete tasks; `app_task` body becomes `phone_dispatch()`
called from a `EV_*`-setting ISR chain — i.e., dispatch runs at the tail of TIM6/EXTI/UART
ISRs (bounded work); `ui_task` loop becomes `main()`'s `while(1){ ui_render_dirty(); }`;
queues become lock-free ring buffers with IRQ masking. Submit FreeRTOS build as the bonus
variant and bare-metal as base if the TA wants both — the module code (drivers, apps,
widgets) is identical in either skeleton. **Decide after the TA answer; before Phase 3.**

### 9.2 If TIM8 PWM misbehaves on PC6
TIM8 is an advanced timer — must set `MOE` (HAL does via `HAL_TIM_PWM_Start`), and BDTR
defaults matter. If blocked >2 h: buzzer falls back to TIM3-toggled GPIO square wave
(pitch via ARR, volume via skipping pulses) — audible, less clean; escalate to team lead.

---

### 9.3 Phase 1 implementation notes (driver bring-up, confirmed in CubeIDE)

**Files added**, matching section 3's file tree: `Core/Inc/pinmap.h`, `Core/Inc/app_config.h`,
`Core/{Inc,Src}/LiquidCrystal.{h,c}`, `Core/{Inc,Src}/serial.{h,c}`, `Core/{Inc,Src}/seg7.{h,c}`,
`Core/{Inc,Src}/keypad.{h,c}`. `main.c` wired: includes, `Serial_Init`/`LiquidCrystal()`/
`Seg7_Init`/`Keypad_Init` + `HAL_TIM_Base_Start_IT(&htim7/&htim2)` in `USER CODE BEGIN 2`;
`StartDefaultTask` loop body does `Keypad_Process()` + `HAL_IWDG_Refresh(&hiwdg)` +
`osDelay(5)` (temporary — see below); shared `HAL_TIM_PeriodElapsedCallback` extended for
TIM7 (`Seg7_MuxISR`) and TIM2 (`Seg7_SecondsTickISR`) alongside the existing TIM1 HAL-tick
branch. `stm32f3xx_it.c` required no edits — `EXTI9_5`, `EXTI15_10`, `TIM2`, `TIM7`,
`USART1` handlers already dispatch to the correct `HAL_*_IRQHandler()` calls from Phase 0
codegen. Build: **0 errors, 0 warnings** (arm-none-eabi-gcc, `.text=45972 .data=128 .bss=18816`).

**⚠ DEVIATION (bug found during Phase 1, not present in the plan's design):** `MX_IWDG_Init()`
(Phase 0 codegen, Prescaler=64/Reload=1250 → ~2.0 s timeout) starts the independent watchdog
immediately with **no refresh call anywhere** in the generated code — the board would have
hard-reset every ~2 s with zero application code running. Fixed by adding
`HAL_IWDG_Refresh(&hiwdg)` to the Phase 1 bring-up loop (`StartDefaultTask`, every ~5 ms).
This is a stand-in for the IWDG kick section 4.3 assigns to `ui_task`'s loop; move it there
once `ui_task` exists (Phase 3) and delete it from `StartDefaultTask`.

**Rule 0.2 note:** `StartDefaultTask`'s loop currently does non-LCD work (`Keypad_Process`,
IWDG kick) because `ui_task`/`app_task` don't exist yet — only CubeMX's single `defaultTask`
exists at this point. This temporarily violates the "while(1)/render loop = LCD-only" rule;
correct this in Phase 3 when the task split happens.

**Digit-select polarity (section 1.1 caveat):** implemented as **active-LOW** (`RESET` = digit
enabled, `SET` = digit disabled, off-before-switch) in `seg7.c`, ported from the previous-project
reference driver (`ToDo/3/HangmanGame/Core/Src/seg7_display.c`) which uses the same polarity
convention on the same board family. **Confirmed correct on physical hardware** during Phase 1
bring-up (after the TIM1/TIM16 HardFault fix below) — no flip needed.

**DP not lighting, then wrong polarity (found on hardware, not anticipated in the original
plan):** the seg7 decimal point (`PIN_SEG_DP`, PD7) initially never lit under any software
configuration. Debugged by elimination: tried active-HIGH (initial code), then active-LOW (by
symmetry with the digit selects), then a constant non-multiplexed drive to rule out a duty-cycle
visibility problem — all three failed identically, while the BCD pins (PD0-3, same `GPIOD` port,
same `HAL_GPIO_Init()` call, same ISR) worked correctly throughout. That ruled out
software/GPIO-config entirely. Root cause turned out to be physical: the DP lead was never
actually connected to PD7 on the breadboard. Once wired up, the dot lit during every mux slot
EXCEPT the intended one (`i==1`) under the active-LOW write — proving the DP LED is wired
**active HIGH**, opposite of the digit selects (it's a standalone LED via its own 330Ω resistor,
not sharing the decoder's cathode circuit). Fixed in `seg7.c`'s `Seg7_Init()` and `Seg7_MuxISR()`
to `SET` = lit, `RESET` = off. LCD and all 16 keypad positions confirmed working correctly on
hardware with no other issues.

**Not yet built (explicitly deferred, per phased plan):** LCD diff-renderer (Phase 3's
`ui.c`/`widgets.c`), full keypad multi-tap/hold/event-queue engine (Phase 2), full seg7
mode set `SEG_OFF/MUSIC/GAME/TIME` (later phases), UART RX line assembly + `cmdparse`
dispatch (Phase 7). Phase 1's `keypad.c` only does raw scan + debounce + `[KEY] row=.. col=..
key=..` UART report; `seg7.c` only does the 2 kHz mux + free-running MM.SS seconds counter.

**Pending (cannot be verified from the IDE build alone — requires physical hardware):**
LCD boot text renders correctly; seg7 digits count MM.SS with no ghosting and correct
polarity; keypad EXTI fires cleanly for all 16 keys with correct (row,col,key) on Termite
and no false triggers.

### 9.4 Phase 2 implementation notes (input engine + events)

**Files added**, matching section 3's file tree: `Core/Inc/events.h`, `Core/Src/events.c`
(new — `Event` struct, `eventQueue` create/post per section 4.2, exactly as specified: no
`input_engine.c` split was introduced, since section 3's file tree assigns the entire
"EXTI scan, debounce, hold, multi-tap engine, input modes, emits high-level KeyEvents"
responsibility to `keypad.h/.c` alone). `Core/{Inc,Src}/keypad.{h,c}` fully rewritten:
Phase 1's `HAL_GetTick()`-polled raw scan (`Keypad_Process()`) is gone, replaced by
`Keypad_TickISR()` driven from TIM6 @1 kHz, implementing the full section 5.1 state machine
(IDLE → EXTI-masked DEBOUNCE → column-strobe SCAN with ghost rejection → HELD, watching for
either HOLD_MS-timeout digit-emit or debounced release → multi-tap/action dispatch → IDLE).
`app_config.h` gained `KEYPAD_HOLD_MS` (900), `KEYPAD_TAP_WINDOW_MS` (1000),
`EVENT_QUEUE_LEN` (16). `main.c`: added `#include "events.h"`; `Event_Init()` in
`USER CODE BEGIN RTOS_QUEUES`; `HAL_TIM_Base_Start_IT(&htim6)` alongside the existing
TIM7/TIM2 starts in `USER CODE BEGIN 2`; `HAL_TIM_PeriodElapsedCallback` gained a
`htim->Instance == TIM6` branch calling `Keypad_TickISR()`; boot LOG/LCD text updated from
"Phase 1 bring-up" to "Phase 2" messaging. `StartDefaultTask`'s loop body replaced: it now
drains `eventQueue` (`osMessageQueueGet(..., 0)`) and, for `EV_KEY` events, updates a 20-char
debug text field on LCD row 1 (CHAR/DIGIT append, DELETE backspaces, BACK clears) — this is
the phase's literal exit-criteria harness ("typing `hello world` ... works; hold-5 prints
`5`"), not a permanent UI. `stm32f3xx_it.c` required no edits — `TIM6_DAC_IRQHandler` already
dispatched to `HAL_TIM_IRQHandler(&htim6)` from Phase 0 codegen; the timer itself just wasn't
started yet, and its NVIC priority (5) was already correctly pre-configured in
`HAL_TIM_Base_MspInit` (`stm32f3xx_hal_msp.c`), matching section 2.7's table.

**⚠ CLARIFICATION (ambiguity found in this document, resolved by evidence, not guessed):**
section 5.1's shorthand "`1→a→b→c→1...`" for the multi-tap cycle, read literally as a
tap-order (tap1→digit, tap2→a, tap3→b, tap4→c), **contradicts** section 1.2's own worked
example — "press `5` ×3 → `o`" — since key 5's letters are "mno" (3 letters): a digit-first
cycle would land tap3 on `n`, not `o`. Resolved by trusting the concrete worked example over
the ambiguous shorthand: the cycle is **letters-first, digit-last**
(`m→n→o→5→m...` for key 5; generally `letters[0..n-1]→digit→letters[0]...`). Implemented in
`keypad.c`'s `key_table[]` + `multitap_char_at()`. Also clarified while building the per-key
table: key 9 has only 2 letters ("yz", not 3 like keys 1-8), and key 0 has no letters at all
(space only in TYPE mode, no multi-tap) — both directly from section 1.2's layout diagram,
not assumptions. **If this cycle order is ever found wrong on hardware, the fix is entirely
inside `multitap_char_at()`/`key_table[]` in `keypad.c` — nowhere else.**

**Design decisions not fully spelled out in section 5.1, made explicitly here (not guessed,
derived from adjacent plan text):** (1) `back`, `LockType`, and the 4th-column shortcuts
(A–D) are treated as **mode-independent action keys** — no hold, no multi-tap — because
section 1.2 describes `back` as "always exits current app" and `LockType` as toggling the
global mode, neither qualified as TYPE-only; only the digit keys 0–9 branch on
TYPE-vs-NAV. (2) HOLD_MS is timed from `HELD`-state entry (i.e., from the resolved,
debounced press), since the plan states the figure but not its exact start point. (3) SCAN
performs a full 4-column sweep (not an early exit on first hit) so the section 5.1
ghosting policy ("≥3 simultaneous keys… ignore multi-key scans") can actually be
enforced — the "first (row,col) hit = key" phrasing in step 3 and the ghosting paragraph in
the same section are reconciled by scanning fully and using hit-count, which degrades to
"first/only hit" in the normal single-key case anyway. Ghost threshold implemented as
`hit_count >= 2` (stricter than the ">=3" wording, i.e. any 2-key ambiguity is already
rejected) since the 4×4 has no diodes and 2-key ambiguity is already unresolvable without
them; documented here as a conservative interpretation, revisit if legitimate 2-key chords
turn out to be needed.

**Rule 0.2 note (continued from 9.3):** `StartDefaultTask`'s loop still does non-LCD work
(`osMessageQueueGet` + LCD debug-field update + IWDG kick) — same accepted, documented
temporary violation as Phase 1, same fix point (Phase 3's `ui_task`/`app_task` split).

**Not yet built (explicitly deferred, per phased plan):** the shortcut actions (A–D) only
*emit* `KEY_EV_SHORTCUT_A..D` events per the section 3 file-tree contract — actually wiring
them to volume/QuickLock/Screenshot behavior is improvement item I11 (section 8), explicitly
"Low priority... uses otherwise-dead keys", deferred until those subsystems (buzzer volume,
lock screen, UART screenshot) exist in later phases. `app_task`/`phone_dispatch` (Phase 3+)
is the eventual consumer of `eventQueue` for everything beyond this phase's debug harness.

**Build:** initially could not be verified in-session (no `arm-none-eabi-gcc` on this
session's environment) — pre-verified by manual re-read against HAL/CMSIS-RTOS v2 API
signatures and NVIC priority cross-check. **Confirmed clean in STM32CubeIDE by the user:**
`0 errors, 0 warnings` (`text 49612, data 132, bss 18876, dec 68620`), all Phase 2 sources
(`events.c`, `keypad.c`, `main.c` changes) compiled and linked successfully.

**Pending (requires physical hardware — build is done):** typing
`hello world` via multi-tap into the LCD debug row; hold-5 prints `5` within ~900 ms; a
different key or the 1 s window commits a pending letter; LockType toggles TYPE↔NAV and NAV
keys (2/8/4/6/5/0) map correctly with typing disabled; `back` always clears the debug field;
≥2 simultaneous keys are rejected and logged `[KEY] ghost/multi ignored`; no event-queue
overflow under fast typing; IWDG does not reset the board during normal use.

### 9.5 Phase 3 implementation notes (UI framework + shell)

**Files added**, matching section 3's file tree: `Core/{Inc,Src}/ui.{h,c}` (20x4 shadow
framebuffer split into a `s_back`/`s_shown` pair — writes go to `s_back`, `UI_RenderDirty()`
diffs `s_back` against `s_shown` cell-by-cell and only issues LCD writes for cells that
changed, then updates `s_shown` — functionally equivalent to the plan's "dirty-flag"
language, mutex-guarded since `s_back` is written from `app_task` and read from `ui_task`);
`Core/{Inc,Src}/cgram.{h,c}` (named banks `CGRAM_BANK_LOGO`/`_MENU_ICONS_DAY`/
`_MENU_ICONS_NIGHT`/`_MUSIC`/`_PVZ` per section 3; split into `Cgram_RequestBank()`, callable
from `app_task`, which only records which bank is wanted, and `Cgram_Apply()`, callable only
from `ui_task`, which does the actual `createChar()` HW writes — this makes rule 0.2
compliance mechanical rather than merely documented); `Core/{Inc,Src}/widgets.{h,c}`
(`Widgets_BlinkOn()` only — see YAGNI note below); `Core/Inc/app.h` (the `App` interface
struct verbatim from section 4.3: `{on_enter, on_event, on_tick, render, on_suspend}`);
`Core/{Inc,Src}/phone.{h,c}` (`Phone_Init()` boot sequence, `Phone_Dispatch()` global BACK
handling + per-app event forwarding, `Phone_Tick()`, `Phone_SwitchApp()` with
suspend/enter/render sequencing); `Core/Src/app_menu.c` (icon grid, layout selectable via
`MENU_LAYOUT` in `app_config.h`); `Core/Src/app_info.c` (team info screen). `keypad.h/.c`
gained `Keypad_SetInputMode()` (not in section 3's named-function list, but required by
section 1.2's own text — "Contact 'Phone' field forces numeric mode; Note list starts in nav
mode, editor in letter mode" — apps need a way to set the mode on entry; `s_input_mode` made
`volatile` since it's now written from both ISR and `app_task` context). `app_config.h`
gained `UI_RENDER_TICK_MS` (30, taken verbatim from section 4.3's task table
`osDelay(30)`), `APP_TASK_TICK_MS` (50, bounds `app_task`'s `osMessageQueueGet()` so blink
state gets re-evaluated periodically regardless of key traffic), `UI_BLINK_MS` (500, section
5.7's blink figure), `MENU_LAYOUT_6X1`/`_3X2`/`_2X3` + `MENU_LAYOUT` (currently `_3X2`),
`BOOT_LOGO_MS` (1500). `main.c`: added `appTask` (normal priority, 1024-word stack, per
section 4.3's table) running `Phone_Init()` then the `osMessageQueueGet(eventQueue) ->
Phone_Dispatch()` / `Phone_Tick()` loop; `StartDefaultTask` (CubeMX-generated identity kept
unchanged — see deviation note below) now conceptually *is* section 4.3's `ui_task`: its loop
body was rewritten to `UI_RenderDirty(); Cgram_Apply(); HAL_IWDG_Refresh(&hiwdg);
osDelay(UI_RENDER_TICK_MS);` — LCD-only, finally resolving the rule 0.2 deviation carried
forward from Phase 1/2. `UI_Init()`/`Cgram_Init()` added in `USER CODE BEGIN RTOS_MUTEX`.

**Design decisions not fully spelled out in section 4/5, made explicitly here (not guessed,
derived from adjacent plan text or hard constraints):** (1) CGRAM codes 0-7 cannot pass
through `LiquidCrystal.c`'s `print(const char[])` (code 0 would be misread as a C-string
terminator) — `UI_RenderDirty()` calls `write(uint8_t)` (raw single-byte write) per cell
instead, which is the only correct way to emit custom glyphs. (2) `defaultTask`'s
CubeMX-generated name/handle/priority were left unchanged rather than renamed to `uiTask`,
since those declarations sit outside `USER CODE` markers and would be overwritten by any
future `.ioc` regeneration regardless — only the already-`USER CODE`-owned function body
needed to change to become section 4.3's `ui_task` in function; `appTask` (new, fully inside
`USER CODE`) was named to match the plan exactly since it has no CubeMX-generated identity to
collide with. (3) Menu's 6 grid slots: only "Info" has a built target `App` this phase (per
section 6, only `app_menu.c` + `app_info.c` are in scope); the other 5 slots (Note, Contact,
Music, PvZ, Settings) are real, visible, navigable grid cells — `target == NULL` — so all 3
layouts are genuinely demoable per the exit criterion, but `SELECT` on them logs "not yet
implemented" instead of dereferencing a null `App`. (4) CGRAM icon and boot-logo pixel
content (the `s_logo_glyphs`/`s_menu_icons_day` bitmap arrays in `cgram.c`) is original
placeholder art — the plan specifies bank *names* and *timing* (section 3, section 5.9), not
exact pixel content, so this is a legitimate cosmetic design choice, not a guessed spec
value; freely replaceable without touching any other module.

**YAGNI deferrals (explicit, not oversights):** `widgets.c` implements only `Widgets_BlinkOn()`
(reads `HAL_GetTick()` directly, gated `/ UI_BLINK_MS & 1`, called from `app_task` context
only, never an ISR) — `list_view`, `progress_bar`, `volume_icon`, `text_field` (section 5.7's
full widget list) are not stubbed, since no app needing them exists until Note/Contact/Music
(Phases 5-6). `softtimer.c` (section 3, kernel group) is likewise not built — section 5.7
literally says "blink_cursor ... toggled by TIM6-derived 500 ms soft timer," but a single
500 ms toggle read from non-ISR `app_task` context is functionally identical without a
generic soft-timer abstraction that (so far) has only one consumer; revisit if/when a second
consumer needs it. `CGRAM_BANK_MENU_ICONS_NIGHT`/`_MUSIC`/`_PVZ` are named in `cgram.h`
(section 3's bank list) but `load_bank()` just logs "not yet implemented" for them, since the
LDR day/night switch (Phase 4), Music (Phase 5), and PvZ (Phase 8) subsystems that would
request them don't exist yet.

**DEVIATION (documented, not yet reconciled into section 5.9's literal text):** section 5.9
specifies "Boot: idle 'press /start' screen -> `/start` -> animated logo + boot jingle ->
Menu," gated on UART command parsing. `cmdparse.c` (section 3, Phase 7) does not exist yet, so
`Phone_Init()` instead shows the boot logo for a fixed `BOOT_LOGO_MS` (1500 ms) and
auto-advances to Menu — same pattern as Phase 1/2's already-documented temporary deviations
for features whose prerequisite subsystem is a later phase. No boot jingle (buzzer/sound
subsystem is Phase 4). Revisit this note once `cmdparse.c` lands.

**Team data (confirmed by the user, no longer placeholder):** `app_info.c` shows
Amin Hasanzadeh and Arman Bijari.

**Build:** not attempted in-session per explicit user instruction ("i will build the project
myself do not build it"). Verified by manual code review only: include dependencies, `App`
struct field/initializer types, menu-index arithmetic for all 3 `MENU_LAYOUT` values (6x1,
3x2, 2x3), FreeRTOS heap budget (`configTOTAL_HEAP_SIZE` 12288 B) against the new `appTask`
1024-word/4096-byte stack alongside existing tasks, CMSIS-RTOS v2 mutex API usage in `ui.c`,
and `setCursor(col,row)` parameter ordering consistency with existing Phase 1/2 call sites.

**DEVIATION/fix (post-build, user-reported boot loop):** first hardware run after the Phase 3
build showed the boot logo repeating forever (board resetting continuously). Root cause not
yet confirmed on hardware, but the leading hypothesis by inspection: `defaultTask`/`ui_task`
kept its CubeMX-original 128-word/512-byte stack (see 9.5's design-decisions note above),
sized for the trivial pre-Phase-3 loop -- Phase 3 is the first time this task calls into the
bit-banged HD44780 driver (`LiquidCrystal.c`) and `cgram.c`'s `createChar()` loop from *task*
context, and `configCHECK_FOR_STACK_OVERFLOW` was 0 (off), so a stack overflow there would
silently corrupt heap_4 memory instead of being reported -- consistent with an unexplained
hang -> `HAL_IWDG_Refresh()` never runs -> IWDG reset -> repeat. Two changes made pending
hardware confirmation: (1) `configCHECK_FOR_STACK_OVERFLOW` set to 2 and
`configUSE_MALLOC_FAILED_HOOK` set to 1 in `FreeRTOSConfig.h`, with hook bodies added in
`freertos.c` that `LOG()` the offending task name over UART before halting -- this makes the
hypothesis verifiable on the next boot instead of guessed; (2) `defaultTask`'s `stack_size`
bumped from `128*4` to `384*4` bytes defensively (heap budget has ample room). **If the UART
log shows `[STACK OVERFLOW] defaultTask` (or `appTask`) or `[MALLOC FAILED]` on the next
board reset, this confirms the hypothesis. If the log stays silent through a reset, this is
ruled out and the next diagnostic step is checking `RCC->CSR`'s `*RSTF` flags in the debugger
to confirm IWDG is even the reset source, per the same debugger-based method that found the
Phase 0 TIM1/TIM16 bug (section 9.4).**

**DEVIATION/fix round 2 (post-build, boot loop persists after round 1):** user confirmed via
UART screenshot that the round-1 hooks did NOT fire (no `[STACK OVERFLOW]` or
`[MALLOC FAILED]` line before the reset repeats) -- only the normal `[BOOT]`/`[PHONE] switch
app` lines, so the stack-overflow hypothesis is either wrong or the corruption is severe enough
to HardFault directly before FreeRTOS's periodic (context-switch-time) stack-canary check can
catch it. By inspection, every fault handler in `stm32f3xx_it.c`
(`HardFault_Handler`/`MemManage_Handler`/`BusFault_Handler`/`UsageFault_Handler`) and
`Error_Handler()` in `main.c` were bare `while(1){}`/`__disable_irq(); while(1){}` loops with
zero diagnostics -- any of these being hit would starve the IWDG (~2s timeout) exactly as
observed, regardless of root cause, and `SCB->SHCSR`'s `MEMFAULTENA`/`BUSFAULTENA`/
`USGFAULTENA` are never enabled in this project so MemManage/Bus/Usage faults all escalate to
`HardFault_Handler` by default -- instrumenting that one handler covers the large majority of
possible CPU-fault scenarios. `LOG()` (interrupt-driven, drained by `USART1_IRQHandler`) cannot
be used inside a fault handler (highest exception priority, no maskable interrupt can preempt
it) or inside `Error_Handler()` (explicitly calls `__disable_irq()`), so both were instrumented
with **polled** `HAL_UART_Transmit()` instead (busy-waits on hardware flags directly, needs no
interrupt). Changes made: (1) `HardFault_Handler` in `stm32f3xx_it.c` converted to a `naked`
function (required so its inline-asm trampoline reads the CPU's stack pointer before any
compiler-generated prologue could disturb it) that determines MSP-vs-PSP from `LR` bit 2 per
the Cortex-M `EXC_RETURN` convention and tail-jumps into a new `HardFault_Handler_C()`, which
prints the stacked exception frame (`PC`, `LR`, `PSR`, `R0`-`R3`, `R12`) plus `SCB->CFSR` /
`HFSR` / `MMFAR` / `BFAR` over polled UART, then parks in an IWDG-starving loop so the board
resets after the dump is visible; `stm32f3xx_it.h`'s prototype was updated to match with
`__attribute__((naked))` (this line sits outside a `USER CODE` marker, so a future CubeMX
`.ioc` regeneration would silently drop the attribute -- must be re-added by hand if that
happens). (2) `Error_Handler()` in `main.c` now sends a `[ERROR_HANDLER]` polled-UART line
before its existing `__disable_irq(); while(1){}` body, distinguishing "a `HAL_xxx_Init()` call
failed" from "the CPU HardFaulted" in the UART log, though it cannot identify *which* init call
without a debugger (the function takes no arguments at its HAL-generated call sites). **Next
hardware test: rebuild, reflash, reset, and watch the UART log for a `[HARDFAULT] PC=...
LR=... PSR=... R0=... ... CFSR=... HFSR=... MMFAR=... BFAR=...` block (or an
`[ERROR_HANDLER]` line) appearing right before the reset repeats. A `[HARDFAULT]` block's `PC`
value can be cross-referenced against `Debug/SmartPhone_STM32F303.list` to pinpoint the exact
faulting instruction/function, continuing the same evidence-based method used for the Phase 0
TIM1/TIM16 bug (section 9.4). If neither line appears, a CPU fault and a failed HAL init are
both ruled out, and the next step is checking `RCC->CSR`'s `*RSTF` flags in the debugger to
confirm what's actually driving the reset.**

**DEVIATION/fix round 3 (post-build, round-2 instrumentation confirms no fault/error path):**
user rebuilt with round-2's HardFault/Error_Handler instrumentation and reset the board; the
UART log shows a clean, regular `[BOOT] Phase 3: UI framework + shell` / `[PHONE] switch app`
alternation with **no** `[HARDFAULT]`, `[ERROR_HANDLER]`, `[STACK OVERFLOW]`, or
`[MALLOC FAILED]` line ever appearing before each reset. This rules out a CPU fault, a failed
`HAL_xxx_Init()` call, and a caught stack overflow/malloc failure as the cause -- the board is
silently hanging somewhere with no exception raised, which starves `HAL_IWDG_Refresh()` and
reproduces the observed reset exactly as before. By inspection, every UART line seen so far
(`[BOOT]`, `[PHONE] switch app`) is logged from **appTask** (`Phone_Init()` /
`Phone_SwitchApp()`); nothing in the codebase ever logs from **ui_task**
(`StartDefaultTask()`, main.c), which is the *only* place `HAL_IWDG_Refresh()` is called. There
is therefore zero direct evidence ui_task is even alive -- if `osThreadNew()` for it silently
returned `NULL`, or it started but stalled before reaching its refresh line, IWDG would starve
with no fault and no hook firing, exactly matching what's observed. Manual review of
`ui.c`/`cgram.c`/`app_menu.c`/`keypad.c`/`widgets.c` (the only code appTask's boot path touches
via `Phone_Init()` -> `Phone_SwitchApp(&AppMenu)` -> `menu_on_enter()`/`menu_render()`) found no
nested/self-deadlocking use of `ui.c`'s `s_mutex` (every `UI_Clear()`/`UI_PutChar()`/
`UI_Print()`/`UI_RenderDirty()` call acquires-then-releases it individually, never nested) and
no other blocking call, so this is not a confirmed root cause yet -- just the leading
uninstrumented gap. Two verifiable (not guessed) additions made: (1) in `main.c`, right after
both `osThreadNew()` calls, a `[RTOS] defaultTaskHandle=%p appTaskHandle=%p` LOG line -- a
`NULL` handle there directly proves thread creation failed (e.g. heap exhaustion, despite the
~4 KB headroom estimated in the stack-size comment above, since that was arithmetic, not a
measurement); (2) in `StartDefaultTask()`, a one-shot `[UI] ui_task alive` LOG as the very
first statement, before any LCD/CGRAM work, so ui_task announces itself the instant it starts
running, independent of whether it later stalls. **Next hardware test: rebuild, reflash,
reset, and check the UART log for both `[RTOS] defaultTaskHandle=0x... appTaskHandle=0x...`
(neither should be `0x0`/NULL) and `[UI] ui_task alive` appearing once at boot, before the
`[BOOT]`/`[PHONE] switch app` lines repeat. If `[UI] ui_task alive` is missing, ui_task never
started or never reached its first line -- next step is inspecting heap allocation failure
paths or thread creation order. If `[UI] ui_task alive` appears every single boot but the reset
still happens, ui_task is starting but stalling inside its own loop (`UI_RenderDirty()` /
`Cgram_Apply()` / the LCD driver itself) after that point -- next step would be instrumenting
inside that loop specifically (e.g. a LOG after each of `UI_RenderDirty()`/`Cgram_Apply()`) to
narrow down which call it stalls in.**

**DEVIATION/fix round 4 (post-build, round 3 confirms ui_task starts but still resets):** user
rebuilt with round 3's instrumentation; the UART log shows `[RTOS] defaultTaskHandle=0x20001be0
appTaskHandle=0x20002c50` (both non-NULL -- thread creation succeeded) and `[UI] ui_task alive`
appearing reliably every single boot, right after `[BOOT] Phase 3: UI framework + shell` and
before `[PHONE] switch app`, then the reset repeats. This rules out failed thread creation and
confirms ui_task does start and does reach its loop -- but we still don't know if it keeps
looping (and keeps feeding the IWDG) past that first announcement, since `[UI] ui_task alive`
only logs once, before the `for(;;)`. The next untested code path, by inspection: this is the
*first time on real hardware* `Cgram_Apply()` loads any CGRAM bank other than
`CGRAM_BANK_LOGO` -- `menu_on_enter()` (`app_menu.c`) calls `Cgram_RequestBank(
CGRAM_BANK_MENU_ICONS_DAY)`, and the next ui_task tick's `Cgram_Apply()` sees
`want != s_loaded` and calls `load_bank()` -> six `createChar()` calls into the bit-banged
`LiquidCrystal.c` driver -- a call sequence and CGRAM bank that has never actually run on this
board before now, unlike the logo bank (2 `createChar()` calls) which already worked earlier in
the same boot. Also unconfirmed: whether `Phone_SwitchApp()`'s `on_enter()`/`render()` calls
(both running in *appTask*, contending for `ui.c`'s `s_mutex` against ui_task's
`UI_RenderDirty()`) complete at all. Two more verifiable (not guessed) additions made, both
temporary and meant to be removed once root-caused: (1) `phone.c`'s `Phone_SwitchApp()` now
logs `[PHONE] on_enter done` and `[PHONE] render done` immediately after each of those two
calls, bracketing them so the log shows exactly how far execution gets; (2)
`StartDefaultTask()` in `main.c` now logs `[UI] tick=N enter` / `[UI] tick=N after_render` /
`[UI] tick=N after_cgram` around `UI_RenderDirty()` and `Cgram_Apply()` for the first 8 loop
iterations only (avoids flooding the UART ring buffer past that), so the exact iteration and
exact call the loop stalls in -- if it stalls -- will be visible. **Next hardware test:
rebuild, reflash, reset, and send the full UART log from this boot. Specifically check: does
`[PHONE] on_enter done` appear (rules `menu_on_enter()`/`Cgram_RequestBank()` itself in, since
that call alone can't stall -- it's a single volatile write)? Does `[PHONE] render done` appear
(if `on_enter done` shows but `render done` doesn't, the hang is inside `menu_render()` itself,
most likely mutex contention or the loop over `s_items`)? Do any `[UI] tick=N ...` lines appear
at all after `[UI] ui_task alive` (if none do, ui_task's own loop is stalling on its very first
iteration, most likely inside `UI_RenderDirty()`'s mutex acquire or `Cgram_Apply()`'s first
`CGRAM_BANK_MENU_ICONS_DAY` load); and if some appear, which `tick=N` and which of
`enter`/`after_render`/`after_cgram` is the last one printed before the log goes silent and the
reset happens -- that pinpoints the exact stalling call.**

**DEVIATION/fix round 5 (post-build, root cause found):** user rebuilt with round 4's
instrumentation; UART log shows `[BOOT] Phase 3: UI framework + shell` -> `[UI] ui_task alive`
-> `[UI] tick=0 enter` -> `[PHONE] switch app` -> `[PHONE] on_enter done`, then the reset
repeats (next boot's `[RTOS] defaultTaskHandle=...` begins). Critically, `[UI] tick=0
after_render` and `[PHONE] render done` never appear. Per round 4's own diagnostic questions:
`on_enter done` appearing but `render done` not appearing pinpoints the hang inside
`menu_render()` -- but `menu_render()` itself only calls `UI_Clear()`/`UI_PutChar()`, which
only touch the RAM `s_back` buffer under a mutex; none of that can itself take ~2s. The
`[UI] tick=0 enter` appearing but `after_render` never appearing, in ui_task's own loop,
narrows this to *inside* `UI_RenderDirty()` (or its mutex acquire). Both point at the same
mutex: `UI_RenderDirty()` holds `s_mutex` for its *entire* diff/write loop, and appTask's
`menu_render()` (via `UI_Clear()`/`UI_PutChar()`) blocks trying to acquire the same mutex --
so `render()` never finishing is a symptom of `UI_RenderDirty()` not finishing, not a separate
bug.

Root cause, confirmed by reading `LiquidCrystal.c` in full (not guessed): the bit-banged
HD44780 driver's `pulseEnable()` does 3x `HAL_Delay(1)`; `write4bits()` does 1x
`pulseEnable()`; `send()` (used by both `command()` and `write()`) does 2x `write4bits()` in
4-bit mode; so one `command()` or `write()` call is already up to ~3-6ms (tick-rounding can
push `HAL_Delay(1)` up to ~1ms over, so realistically higher). Each LCD cell write is
`setCursor()` (1x `command()`) + `write()` (1x `write()`) = 2 driver calls = 4
`write4bits()` = 4 `pulseEnable()` = up to ~12-24ms/cell worst case. `ui.c`'s `UI_Init()`
originally force-initialized `s_shown` (the "what's currently on the LCD" shadow buffer) to
all-zero specifically so the very first `UI_RenderDirty()` treats all `UI_ROWS*UI_COLS` = 80
cells as dirty and redraws the whole screen. That reasoning was correct about *why* (`s_shown`
must start as a value that can never accidentally equal a real back-buffer cell, or a cell
that's genuinely blank at boot would wrongly be skipped) but wrong about the *consequence*:
`LiquidCrystal.c`'s own `begin()` already ends with `clear()`, so the physical LCD genuinely
*is* all-spaces by the time ui_task's first `UI_RenderDirty()` runs -- forcing all 80 cells to
redraw (vs. the ~12 cells the boot screen actually populates: 2 logo glyphs + "SmartPhone") was
therefore both unnecessary and, at up to ~12-24ms/cell x 80 cells, potentially ~1-2s all by
itself. That is enough to blow the ~2.0s IWDG window on top of `main()`'s own pre-kernel init
overhead (dominated by `LiquidCrystal()`'s `begin()`, itself ~60ms+ of `HAL_Delay()` calls),
**before ui_task's single post-render `HAL_IWDG_Refresh()` call ever executes** -- a
legitimately slow first render, not a fault, deadlock, or failed thread creation (all
independently ruled out in rounds 1-4).

**Fix 1 (the actual bug):** `ui.c`'s `UI_Init()` now initializes `s_shown` to `' '` (space)
instead of `0x00`, matching the LCD's real known-blank post-`begin()` state. Frame 1 now only
redraws the ~12 cells the boot screen actually populates instead of all 80 -- both correct
(same semantics: `s_shown` still starts as a value no accidental real content collides with,
since UI text never happens to already equal a space in the cells it's about to write) and
fast.

**Fix 2 (defense-in-depth, since future full-screen transitions -- e.g. Menu<->Info -- can
also dirty up to 80 cells at once and hit the same wall even with fix 1 in place):**
`UI_RenderDirty()`'s signature changed from `void UI_RenderDirty(void)` to
`void UI_RenderDirty(void (*on_row_done)(void))`. It now invokes the callback once per row
(up to `UI_ROWS` = 4 times per call, not per cell) inside its mutex-held loop, so a worst-case
full-screen redraw feeds the IWDG several times during its own execution instead of only after.
`ui.c` intentionally has no knowledge of `hiwdg` (confirmed via grep: not exposed through
`main.h`), so `main.c` passes a new small static wrapper, `ui_iwdg_row_kick()`, which calls
`HAL_IWDG_Refresh(&hiwdg)`. `StartDefaultTask()`'s call site is now
`UI_RenderDirty(ui_iwdg_row_kick)`.

**Cleanup:** all round 2/3/4 temporary diagnostic `LOG()` calls (`[RTOS] defaultTaskHandle=...`
in `main()`, `[UI] ui_task alive` and the `dbg_tick`-gated `[UI] tick=N ...` lines in
`StartDefaultTask()`, and `[PHONE] on_enter done`/`[PHONE] render done` in
`Phone_SwitchApp()`) have been removed now that the boot loop is root-caused -- their purpose
(bisecting an unknown hang) is fulfilled and they were explicitly documented as temporary in
rounds 2-4. `[BOOT]` and `[PHONE] switch app` remain as permanent, low-volume lifecycle logs.

**Next hardware test (supersedes round 4's pending list): rebuild, reflash, reset, and confirm
the boot loop is gone (board reaches Menu and stays there, no repeating `[BOOT]` line).** If
confirmed, proceed to the original Phase 3 hardware test checklist: boot logo displays then
auto-advances to Menu after ~1.5 s; Menu navigation (2/8/4/6 = up/down/left/right, 5 =
select) works and wraps at grid edges in all 3 layouts; selecting a non-Info item logs
"not yet implemented" and does not crash; selecting Info switches to the Info screen;
`back` from Info returns to Menu; Menu's cursor position is preserved across the
Menu->Info->Menu round trip (state preservation per section 4.4); the blinking `>` selection
cursor blinks visibly at ~500 ms half-period; no LCD flicker/full-redraw artifacts (diff
rendering should only touch changed cells); IWDG does not reset the board during normal use.

### 9.6 Phase 4 implementation notes (sound + analog)

**User confirmation before starting:** Phase 3's round-5 boot-loop fix was confirmed working
on hardware ("everything fine. the uart behavior is also fine."); the user separately noted
"i can't scroll which i think is not implemented yet" — checked against section 5.7/section 6
and confirmed correct: `list_view`/`text_field` auto-scroll widgets are Phase 6 scope, not yet
built, so this is expected, not a bug. The user then asked to proceed to Phase 4.

**Files added**, matching section 3's file tree: `Core/{Inc,Src}/buzzer.{h,c}` (note/melody/SFX
engine — 4 built-in melodies as `Note{freqHz,ms}` ROM arrays wrapped in a `Melody` struct,
`BUZZER_IDLE`/`PLAYING`/`PAUSED` state machine, `Buzzer_TickISR()` driven by TIM3 at 1 kHz per
section 2.3, SFX-overrides-then-restores-melody priority per section 5.3); `Core/{Inc,Src}/analog.{h,c}`
(ADC pipeline — 5-sample median feeding an EMA(alpha=0.3) filter per channel, pot -> volume with
deadband, LDR -> hysteresis-gated `EV_ADC_LDR_EDGE` + `Analog_IsNight()` accessor, per section
5.4). `Core/Src/app_music_test.c` (temporary placeholder `App`, see below).

**Design decisions not fully spelled out in section 5.3/5.4, made explicitly here (not guessed):**
(1) **TIM8-extern architecture:** `buzzer.c` directly `extern`s `htim8` (mirroring
`stm32f3xx_it.c`'s existing `extern UART_HandleTypeDef huart1;` precedent), unlike `ui.c`'s
IWDG-callback pattern — justified because TIM8/PC6 is exclusively the buzzer's own dedicated
hardware (same reasoning `LiquidCrystal.c` already uses to own the LCD's GPIOs directly), not a
shared system resource like IWDG that many modules touch. `analog.c`, by contrast, needs *no*
ADC handle externs at all, since `HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)` already
receives the firing handle as a parameter — `main.c`'s dispatcher passes it straight through to
`Analog_ConvCpltCallback(hadc)`.
(2) **ADC re-arming:** `MX_ADC1_Init()`/`MX_ADC2_Init()` configure single (non-continuous)
conversion mode, TIM4-TRGO-triggered. Per the F3 reference manual, `ADSTART` is cleared by
hardware at end-of-conversion in this mode, so each `HAL_ADC_Start_IT()` only arms *one*
upcoming trigger. `Analog_ConvCpltCallback()` therefore calls `HAL_ADC_Start_IT(hadc)` again at
the end of its own body to re-arm for the next 20 Hz TRGO edge — this lives in `analog.c` (not
`main.c`) so the main.c dispatcher stays a pure pass-through and all ADC-triggering knowledge
stays in the one module that owns the filter pipeline consuming it.
(3) **Melody content:** 4 short recognizable fragments ("Twinkle Twinkle", an ascending/
descending scale run, "Happy Birthday", a 2-tone alert jingle) — section 5.3 mandates ">=4
built-in melodies" but not which songs; same design-choice footing as `cgram.c`'s hand-drawn
icon bitmaps (Phase 3, section 9.5 note 4).
(4) **Vol+/Vol- (keypad shortcuts A/B) now wired**, per section 1.2 ("A = Vol+, B = Vol-") and
risk register item I11's own text ("deferred until those subsystems ... exist"): the
buzzer/volume subsystem exists as of this phase, so A/B become live now in `Phone_Dispatch()`
(global handling, like `BACK`, so it works from any app — and deliberately does not consume the
event, so the current app's own `on_event()`/`render()` still runs afterward). QuickLock/
Screenshot (C/D) remain deferred — their subsystems (lock screen/Phase 7, UART screenshot)
still don't exist.
(5) **Temporary `AppMusicTest` app**, wired into Menu's previously-`NULL` "Music" slot: Phase
4's own exit criterion ("melody plays with pause/next, volume audibly changes") needs to be
demoable via the UI, and no other trigger point exists yet (`cmdparse.c`, which could otherwise
drive the buzzer from a UART command, is Phase 7). `app.h` gained `extern const App
AppMusicTest;`; `app_music_test.c`'s own header comment marks it for deletion once Phase 5's
real `app_music.c` lands. Volume is shown as plain text (`UI_Print` "Vol:NN%"), not a custom
CGRAM icon, since `cgram.h` itself already annotates `CGRAM_BANK_MUSIC` (needed for the 3-state
volume icon) as Phase 5 scope — deferring icon rendering follows the codebase's own existing
authoritative comment, not a fresh guess.
(6) **`CGRAM_BANK_MENU_ICONS_NIGHT` now implemented** (unlike `_MUSIC`/`_PVZ`, still correctly
deferred): `cgram.h`'s own comment already marked this bank as Phase-4 scope. `cgram.c` gained a
`s_menu_icons_night[6][8]` table (same 6 slots/order as the day set, redrawn with a small
filled-corner accent so the swap is visibly obvious on hardware) and `load_bank()`'s
`CGRAM_BANK_MENU_ICONS_NIGHT` case now actually loads it. `app_menu.c` gained
`menu_request_icon_bank()` (chooses day/night bank via `Analog_IsNight()`), called both from
`menu_on_enter()` and from a new `EV_ADC_LDR_EDGE` handler in `menu_on_event()` so a day/night
crossing that happens while Menu is already on screen is reflected immediately, not just on
next entry.
(7) **LDR/pot tunables are temporary Settings-less stand-ins** (`app_config.h`:
`ANALOG_LDR_THRESHOLD_RAW`, `ANALOG_LDR_HYSTERESIS_RAW`, `ANALOG_POT_DEADBAND_PCT`,
`BUZZER_DEFAULT_VOLUME_PCT`, `BUZZER_VOLUME_STEP_PCT`) — `settings.c`/`storage.c` (Phase 6)
don't exist yet to back a user-tunable `settings.ldr_threshold`, so these are compile-time
placeholders, same deferred-config pattern already used for `MENU_LAYOUT` in Phase 3. Revisit
once Settings exposes real values and/or the LDR divider is characterized on real hardware.

**`main.c` changes:** `USER CODE BEGIN 2` gained `HAL_ADCEx_Calibration_Start()` for both
ADC1/ADC2 (required before first use on F3), `HAL_ADC_Start_IT()` for both, `HAL_TIM_Base_Start()`
(plain, not `_IT` — TIM4 only needs its counter running to trigger ADC1/ADC2 via TRGO; no IRQ
is enabled for it, per section 2.3's own TIM4 row), `HAL_TIM_Base_Start_IT(&htim3)`,
`Buzzer_Init()`, `Analog_Init()`. `HAL_TIM_PeriodElapsedCallback()`'s dispatch chain gained an
`else if (htim->Instance == TIM3) { Buzzer_TickISR(); }` branch. A new
`HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)` was added, calling
`Analog_ConvCpltCallback(hadc)` — confirmed this is not a duplicate definition (no prior
`HAL_ADC_ConvCpltCallback` existed in the project) and that NVIC (`ADC1_2_IRQn`, `TIM3_IRQn`)
and the IRQ handlers (`ADC1_2_IRQHandler` calling `HAL_ADC_IRQHandler()` for both `hadc1`/`hadc2`,
`TIM3_IRQHandler` calling `HAL_TIM_IRQHandler(&htim3)`) are already wired by CubeMX-generated
`stm32f3xx_hal_msp.c`/`stm32f3xx_it.c` — no NVIC/handler changes needed.

**Build:** not attempted in-session per the standing user instruction ("i will build the
project myself do not build it"). Verified by manual code review only: header/include
dependencies across all new and edited files, `Event`/`App`/`ADC_HandleTypeDef` field usage,
`extern` declarations resolving to the correct CubeMX-generated globals (`hadc1`/`hadc2`/
`htim3`/`htim4`/`htim8`), and that no existing `HAL_ADC_ConvCpltCallback`/TIM3 dispatch branch
already existed to collide with.

**Next hardware test (Phase 4 exit criteria, section 6): melody plays with audible pause/next
(SELECT toggles pause/resume, LEFT/RIGHT cycle prev/next melody in the Music placeholder app);
volume audibly changes via both A/B keypad shortcuts (from any app) and the potentiometer;
LDR flips log `[LDR] day->night`/`night->day` over UART and Menu's icons visibly swap between
the day and night glyph sets (both at Menu on-enter and live while sitting in Menu); IWDG does
not reset during normal use of any of the above.**

---

**DEVIATION (Phase 4 post-build bug report -- melody-end reset, found on first physical
Phase-4 hardware test):** User reported "when i play a music after being played one time,
the system reset itself." Root cause verified (not guessed) by systematic elimination, not
speculation: ruled out FreeRTOS task-stack overflow (no `[STACK OVERFLOW]` log line -- that
diagnostic hook already exists from Phase 3's round-5 fix, section 9.5); ruled out a clean
HardFault/BusFault/UsageFault/MemManage fault (`stm32f3xx_it.c`'s `HardFault_Handler_C()` does
a *blocking* `HAL_UART_Transmit()` fault-register dump before spinning forever without feeding
IWDG, so a genuine fault would reliably have produced a `[HARDFAULT]` UART line -- none
appeared in the user's log). Then found the actual root cause by cross-referencing every
configured NVIC preempt priority in `stm32f3xx_hal_msp.c` against
`FreeRTOSConfig.h`'s `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5`: any ISR that calls a
FreeRTOS/CMSIS-RTOS API (e.g. `Event_Post()` -> `osMessageQueuePut()`) must run at NVIC preempt
priority numerically >= 5, since the kernel's BASEPRI critical-section masking cannot block an
interrupt at a numerically-lower (higher-urgency) priority than itself -- calling a kernel API
from such an interrupt is undefined behavior, not a guaranteed-safe path. `TIM3_IRQn` (the
vector `buzzer.c`'s `Buzzer_TickISR()` runs from, per section 9.6's design) was configured at
priority `2` -- an unsafe value, inherited unmodified from CubeMX's original `.ioc` generation
and never revisited when Phase 4 added a FreeRTOS API call (`Event_Post()`) inside
`Buzzer_TickISR()`, which fires exactly once per melody: at natural melody end. This precisely
explains both symptoms the user observed: the correlation with "after playing ... one time"
(melody's natural end is the *only* place in `Buzzer_TickISR()` that calls `Event_Post()`), and
the absence of any diagnostic UART message (kernel state corruption from the priority violation
is undefined behavior, not a clean detectable fault). Every other ISR was individually checked
against the same rule and confirmed already safe: `ADC1_2_IRQn=7`, `TIM2_IRQn=6`,
`TIM6_DAC_IRQn=5` (Keypad_TickISR, also calls `Event_Post()`, already at the safe boundary),
`USART1_IRQn=6` all satisfy `>= 5`; `TIM7_IRQn=1` (Seg7_MuxISR) and `TIM1_UP_TIM16_IRQn=15`
were grepped for FreeRTOS API calls and confirmed to make none, so their priorities are safe
regardless of numeric value. **Fix applied in two places** so a future CubeMX regeneration
does not reintroduce the bug: `SmartPhone_STM32F303.ioc`'s `NVIC.TIM3_IRQn` line changed from
priority `2` to `5` (matching `TIM6_DAC_IRQn`'s already-safe value), and
`stm32f3xx_hal_msp.c`'s `HAL_NVIC_SetPriority(TIM3_IRQn, 2, 0);` changed to
`HAL_NVIC_SetPriority(TIM3_IRQn, 5, 0);` with an inline comment recording this root cause (so
the fix is self-documenting even if this plan section is not read first). No functional
impact: TIM3 still ticks the buzzer sequencer at the same 1 kHz rate (section 2.3) -- only its
interrupt's relative urgency against other ISRs changes, and buzzer timing has no sub-millisecond
real-time deadline that priority 5 vs. 2 would violate.

**Hardware retest needed:** let a melody play all the way through to its natural end (not just
pause/skip) without any other interaction, and confirm no reset occurs.

---

**DEVIATION (Phase 0 latent bug, found via debugger on first physical flash during
Phase 1 — not a Phase 1 regression):** First hardware run showed LCD/seg7/keypad/UART all
completely dead. Debugger pause + call stack traced a HardFault occurring inside
`HAL_Init()`, before `SystemClock_Config()` or any application code runs:
`HAL_Init() -> HAL_InitTick() -> HAL_NVIC_EnableIRQ() -> [TIM1 update IRQ fires immediately]
-> TIM1_UP_TIM16_IRQHandler() -> HAL_TIM_IRQHandler(&htim16) -> NULL Instance deref ->
HardFault`. Root cause: TIM1 and TIM16 share one NVIC vector on the F303
(`TIM1_UP_TIM16_IRQn`, see section 2.7's earlier deviation note); CubeMX's auto-generated
`TIM1_UP_TIM16_IRQHandler()` unconditionally calls `HAL_TIM_IRQHandler(&htim16)` every time
it fires, but `HAL_InitTick()` (inside `HAL_Init()`, the very first line of `main()`) starts
TIM1 and enables this shared IRQ long before `MX_TIM16_Init()` runs later in `main()` — so
the first TIM1 tick interrupt hits with `htim16.Instance` still NULL (zero-initialized
`.bss`), and the HAL dereferences it. This fault would have fired on every build back to
Phase 0; it was never caught earlier because Phase 0's hardware exit criterion (LED blink +
`hello` on Termite) was apparently never actually flashed and run on the board before now.
**Fix (applied in `stm32f3xx_it.c`, inside the USER CODE markers so it survives CubeMX
regen):** guard `TIM1_UP_TIM16_IRQHandler()` to skip the `htim16` call and return early
while `htim16.Instance == NULL`. No functional impact once TIM16 is initialized (Phase 8's
game tick will use it normally after `MX_TIM16_Init()` runs).

---

### 9.7 Phase 5 implementation notes (Music Player app)

**User confirmation before starting:** Phase 4 was confirmed fully working on hardware
("done. everything works fine even our ldr detects the light. this is for now. for later
phases we will add other musics as well. let's go to the next phase if this phase is
done."), authorizing the move to Phase 5 per section 6's phase order.

**Files added**, matching section 3's file tree: `Core/{Inc,Src}/app_music.c` +
`Core/Inc/app_music.h` (the real Music Player app). `Core/Src/app_music_test.c` (Phase 4's
temporary placeholder, see section 9.6 note 5) **deleted**; its Menu slot in `app_menu.c`
repointed from `&AppMusicTest` to `&AppMusic`; `app.h`'s `extern const App AppMusicTest;`
replaced with `extern const App AppMusic;`.

**Files extended** (Music depends on all of these; each addition is additive, no existing
API signature changed): `buzzer.h/.c` gained `Buzzer_GetElapsedMs()`,
`Buzzer_GetMelodyDurationMs(uint8_t idx)`, and `Buzzer_SeekPercent(uint8_t pct)`, backed by a
new `s_elapsed_ms` (incremented once per `Buzzer_TickISR()` call while `BUZZER_PLAYING`,
reset in `Buzzer_PlayMelody()`) and a `s_melody_total_ms[MELODY_COUNT]` cache computed once
in `Buzzer_Init()` by summing each melody's fixed `Note` table. `analog.h/.c` gained an
`AnalogPotRole` enum (`VOLUME`/`SEEK`) with `Analog_SetPotRole()`/`Analog_GetPotRole()`;
`handle_pot()` was refactored into a shared `apply_pot_pct()` helper that branches on the
role (`Buzzer_SetVolume()` vs `Buzzer_SeekPercent()`) while keeping the existing deadband
gate and `EV_ADC_POT` posting untouched. `seg7.h/.c` gained a `Seg7Mode` enum
(`SEG_OFF`/`SEG_MUSIC` — GAME/TIME deliberately deferred, YAGNI, until Phase 8/9's owning
apps exist) with `Seg7_SetMode()`/`Seg7_GetMode()`/`Seg7_SetMusicInfo()`; `Seg7_MuxISR()`
now blanks the whole display (still advancing `s_mux_index` so a later mode switch resumes
mid-cycle cleanly) whenever `s_mode==SEG_OFF`, and `Seg7_SecondsTickISR()` was simplified to
just `Event_Post()` an `EV_TICK_1S` event — **this finally supersedes the Phase-1 free-running
MM:SS placeholder** that had lived directly in `seg7.c` since Phase 1, whose own header
comment had marked it "Superseded by EV_TICK_1S event posting once the event queue exists
(Phase 2...)" — an outstanding TODO that Phase 2/3/4 never actually closed out, since no
consumer needed it until now. `cgram.h/.c` gained `CGRAM_ICON_VOL_MUTE/LOW/HIGH` (indices
0/1/2 within `CGRAM_BANK_MUSIC`) and a 3-glyph `s_music_icons[3][8]` table (hand-drawn, same
design-choice footing as the existing menu icons, section 9.5 note 4); `load_bank()`'s
`CGRAM_BANK_MUSIC` case was split out of the shared "not yet implemented" stub it previously
shared with `CGRAM_BANK_PVZ` (which remains correctly deferred to Phase 8).
`app_config.h` gained `MUSIC_PROGRESS_BAR_COLS` (18 -- interior width of the `[####...]`
progress bar, chosen so bar+brackets+margin stay within `UI_COLS==20`, same "no figure given,
pick something round and legible" policy as `BUZZER_VOLUME_STEP_PCT`).

**Design decisions not fully spelled out in section 3/5.1/5.2/5.4, made explicitly here (not
guessed):**

(1) **"controls (5/4/6/2/8)" key mapping:** section 3's file tree names exactly 5 distinct
Music behaviors (play/pause, prev, next, shuffle, repeat) — a 1:1 count match with the 5
keys section 3 literally lists. Mapped: `5`=SELECT=play/pause toggle, `4`=LEFT=prev,
`6`=RIGHT=next, `2`=UP=toggle shuffle, `8`=DOWN=toggle repeat (NAV mode's existing
2/4/5/6/8 layout, `keypad.h`'s `InputMode`). `LockType` (section 5.1: "also the pot-function
toggle inside Music") toggles the pot between `VOLUME` and `SEEK` — consumed by
`music_on_event()`'s `KEY_EV_LOCKTYPE` case, the event's first-ever consumer (it had existed,
unconsumed, in `keypad.c` since Phase 2).

(2) **Pot-role default while in Music:** section 5.4's literal wording is "Music mode: pot
value maps to seek % while pot_role=SEEK" — read as SEEK being Music's own default (not
VOLUME), with LockType toggling it to VOLUME and back while Music stays the active app
(section 5.1's more general "toggle" framing, read together with 5.4's specific default).
`music_on_enter()` therefore calls `Analog_SetPotRole(ANALOG_POT_ROLE_SEEK)` every time Music
becomes current, regardless of what the role was left at previously.

(3) **Background playback + auto-next architecture:** `phone.c`'s `Phone_Dispatch()` only
routes events to the *currently active* app's `on_event()` via the `App` vtable, but Music
must keep auto-advancing tracks even when the user has navigated back to Menu (section 3:
"background playback, auto-next"). Solved with a new `Music_HandleSongEnd(const Event *ev)`
(declared in the new `app_music.h`, defined in `app_music.c`, NOT part of the `App` vtable)
called unconditionally from `Phone_Dispatch()` for every `EV_SONG_END`, regardless of
`s_current` — mirroring the existing global `KEY_EV_SHORTCUT_A`/`_B` (Vol+/Vol-) handling
already in `Phone_Dispatch()` (section 9.6 note 4), including the same "don't `return`, let
it still fall through" pattern (harmless here since Music's own `on_event()` doesn't handle
`EV_SONG_END` at all).

(4) **Pot-role / 7-seg-mode reset on leaving Music:** since pot role and 7-seg mode are
cross-cutting global states that should only be non-default while Music is actually active,
and `on_suspend()` is deliberately a no-op by design (section 4.4, not meant for cleanup —
same reasoning already documented for every other app's `on_suspend()`), the reset is
centralized in `phone.c`'s `Phone_SwitchApp()`: when switching *away from* `&AppMusic`
specifically (`s_current == &AppMusic && app != &AppMusic`), it forces
`Analog_SetPotRole(ANALOG_POT_ROLE_VOLUME)` and `Seg7_SetMode(SEG_OFF)` — mirroring the
existing `s_current != &AppMenu` pointer-comparison precedent already used for the global
BACK handler in the same function.

(5) **Manual next/prev vs. shuffle/repeat:** shuffle governs *forward* navigation only —
both manual `NEXT` (RIGHT key) and auto-advance-on-`EV_SONG_END` consult it (picking a
different random track via a small `xorshift32` PRNG, seeded once from `HAL_GetTick()`).
Manual `PREV` (LEFT key) always stays sequential (`Buzzer_Prev()`), since there is no
shuffle-history stack (YAGNI) — this lets the user reliably back up to the actual previous
track instead of bouncing to another random one mid-shuffle. Repeat only affects the
*natural-end* auto-advance (`Music_HandleSongEnd()` replays the same track when `s_repeat` is
set, checked before shuffle) — manual skip keys intentionally ignore it, matching
`buzzer.h`'s own existing `Buzzer_Next()`/`Buzzer_Prev()` comment ("a user-driven skip,
distinct from `EV_SONG_END`'s natural-end auto-next").

(6) **CGRAM / progress-bar budget:** the live progress bar (section 3) uses the LCD
controller's built-in (non-CGRAM) solid-block character `0xFF` for filled cells and `'.'`
for empty cells, rather than a custom glyph — deliberately, to stay well under the 8-glyph
CGRAM ceiling (section 8 risk register item 2: "every screen design must name its bank up
front"). Only the 3-state volume icon consumes CGRAM (`CGRAM_BANK_MUSIC`'s 3 glyphs,
mute/low/high selected by `Buzzer_GetVolume()`: 0 -> mute, 1-50 -> low, 51-100 -> high).

(7) **Shuffle PRNG:** `xorshift32`, hand-rolled, to avoid pulling in `<stdlib.h>`'s
`rand()`/`srand()` for a single-use shuffle pick — not a guess about a spec requirement,
since section 3 names no algorithm, same footing as the melody content/icon-bitmap design
choices already documented in sections 9.6/9.5.

(8) **`EV_TICK_1S` intentionally still has zero real consumers:** posting it from
`Seg7_SecondsTickISR()` (see "Files extended" above) fulfills `events.h`'s own long-standing
declared contract, but `app_music.c` deliberately does NOT consume it for its own elapsed-time
display — it polls `Buzzer_GetElapsedMs()` directly from `music_on_tick()` (driven by
`APP_TASK_TICK_MS`, section 4.3), which stays correct even across a background auto-next that
happens while some other app is on screen and `EV_TICK_1S` would otherwise need Music-specific
routing logic it doesn't need. Unconsumed events are silently dropped by `app_task`'s default
event-switch case today, same as every other posted-but-unconsumed `EventType` — this is not
a regression, just an event nobody needs yet.

**Build:** not attempted in-session per the standing user instruction. Verified by manual
code review only: header/include dependencies across all new and edited files (`app_music.h`
only pulls in `events.h`, avoiding a circular `app.h`<->`app_music.h` include since `app.h`
itself declares `extern const App AppMusic;`, following the existing no-per-app-header
pattern), `Event`/`App`/`AnalogPotRole`/`Seg7Mode` field and enum usage, and that
`Phone_Dispatch()`'s new `EV_SONG_END` branch and `Phone_SwitchApp()`'s new pot-role/seg7-mode
reset branch don't collide with any existing logic in those functions.

**Next hardware test (Phase 5 exit criteria, section 6: "every music-spec sentence
demonstrably true"):** open Music from Menu and confirm the 7-seg display turns on showing
`M.SS n` (previously off at Menu, per section 5.2's default) with digit0=minutes,
digit3(rightmost)=song number; SELECT toggles play/pause; LEFT/RIGHT skip prev/next (RIGHT
should pick a random *different* track once shuffle (UP) is toggled on — repeated presses
should visibly vary); DOWN toggles repeat, and letting a track play to its natural end with
repeat ON should audibly restart the same track; with repeat OFF and shuffle OFF, natural end
should sequentially advance; with shuffle ON, natural end should jump to a random different
track; navigating BACK to Menu mid-playback should keep the melody audibly playing in the
background and still auto-advance at natural end even though Music isn't on screen (and the
7-seg display should go OFF once back in Menu, confirming the `Phone_SwitchApp()` reset);
re-opening Music should resume showing accurate live progress-bar/elapsed-time state; the
progress bar (`[####......]`) should visibly fill left-to-right as the track plays and the
volume icon should visibly change between mute/low/high glyphs as volume changes; pressing
LockType while in Music should flip the on-screen "Pot:VOL"/"Pot:SEEK" label, and turning the
pot should correspondingly change either audible volume or seek position (confirmed by the
progress bar jumping and the 7-seg seconds updating); IWDG must not reset during any of the
above.

---

### 9.8 Phase 6 implementation notes (Note + Contact + storage)

**User confirmation before starting:** "ok since this phase is sompleted let's go to the
next phase of the plan remember DO NOT ASSUME, VERIFY," authorizing the move to Phase 6 per
section 6's phase order, following the same "stick to plan, verify don't guess" discipline
used every prior phase.

**Files added**, matching section 3's file tree: `Core/{Inc,Src}/storage.h/.c` (flash
persistence), `Core/Src/app_note.c`, `Core/Src/app_contact.c` (both declared `extern` in
`app.h` per the existing no-per-app-header convention `app_music.c` already established).

**Files extended:** `widgets.h/.c` gained `Widgets_ListEnsureVisible()` (section 5.7's
`list_view`: scroll-window clamping so the selected row stays within a `visible_rows`
window) and a new `TextField` type + `Widgets_TextFieldBind/Insert/Backspace/MoveCursor/
ScrollOffset()` (section 5.7's `text_field`: insert/delete at cursor, auto-scroll when full,
cursor-follow) — both were named in section 5.7 but had no implementation until a real
consumer (Note/Contact) needed them, same lazy-build-when-needed policy as `progress_bar`/
`volume_icon` landing inline in `app_music.c` for Phase 5 (section 9.7). `app_config.h`
gained `NOTE_LIST_VISIBLE_ROWS` (4 — matches `ui.h`'s `UI_ROWS`, kept as a standalone macro
rather than importing `ui.h` into the dependency-free tunables file, same style as every
other entry in that file). `app.h` gained `extern const App AppNote;`/`AppContact;`.
`app_menu.c`'s `s_items[]` table repointed the `"Note"`/`"Contact"` slots from `NULL` to
`&AppNote`/`&AppContact`. `main.c` gained `#include "storage.h"`, a `Storage_Init()` call in
the pre-kernel `USER CODE BEGIN 2` block (alongside `Buzzer_Init()`/`Analog_Init()`, plain
flash reads, no `HAL_FLASH` unlock needed), `storageTaskHandle`/`storageTask_attributes`
(256-word stack, `osPriorityLow`, matching section 4.3's table exactly) plus the
`osThreadNew(StartStorageTask, ...)` call, and `StartStorageTask()`'s body — a thin
`for(;;) { Storage_WaitAndSave(); }` wrapper, same convention as `StartAppTask`/
`StartDefaultTask`.

**Design decisions not fully spelled out in section 5.6/5.7/1.2, made explicitly here (not
guessed):**

(1) **Blob header gained a `seq` field not literally named in section 5.6.** Section 5.6
says "write to alternate page each save ... newest valid CRC wins on boot." CRC alone proves
a page's *integrity*, not which of two independently-valid pages is more *recent* — there is
no way to answer "newest" from CRC bytes alone. `StorageBlob` therefore adds a monotonically
incrementing `uint32_t seq`, bumped on every save; `Storage_Init()` picks the higher-`seq`
page among those that pass `storage_page_valid()` (magic+version+len+CRC all check out).
This is an explicit, documented interpretation of the plan's shorthand, not a scope
addition — the plan's own wording requires *some* recency signal beyond CRC to be
implementable at all.

(2) **`StorageBlob` also carries a `len` field (`sizeof(StoragePayload)`)** as a cheap sanity
check ahead of trusting a page's CRC (protects against a version-format mismatch slipping
past magic/version alone in a future phase that changes `StoragePayload`'s layout without
bumping `STORAGE_VERSION`) — belt-and-suspenders, not required by section 5.6's literal text.

(3) **Reserved placeholder byte regions for not-yet-built subsystems.** Section 5.6's blob
lists `{settings, notes, contacts, uploaded songs, high scores}` as one unit, but only
notes/contacts have an owning subsystem as of Phase 6 (settings is Phase 7's
`app_settings.c`, songs is Phase 9's `song_upload.py`/`cmdparse.c`, high scores is Phase 8's
`app_pvz.c`). Rather than deferring those fields entirely (which would force a
`STORAGE_VERSION` bump and an on-flash format migration once those phases land),
`StoragePayload` reserves their exact plan-specified byte budgets NOW as zeroed placeholder
arrays (`settings_reserved[32]`, `songs_reserved[2*64*4=512]`, `highscores_reserved[16]`,
taken verbatim from section 5.6's own figures: "settings ~= 32 B", "2 uploaded songs x 64
notes" where a `buzzer.h` `Note` is `{uint16_t freqHz; uint16_t ms;}` = 4 bytes, "scores
16 B"). Only `storage.c`'s internals change in later phases to start populating those
regions instead of leaving them zeroed — the on-flash format itself doesn't change.

(4) **`s_blob` is a file-static variable, not a `storage_task` stack local.** Section 4.3's
table budgets `storage_task` at 256 words = 1024 bytes, but `StorageBlob` is ~1.8 KB
(10 notes x (16+80) + 10 contacts x (16+12) + reserved regions + header/CRC fields) —
larger than the entire task stack budget on its own. Making it `static` (RAM working copy,
also the exact bytes flashed on save — no separate snapshot/copy step) keeps
`Storage_WaitAndSave()`'s actual stack usage small regardless of blob size.

(5) **CRC32 is a hand-rolled bit-by-bit software routine** (standard IEEE 802.3 polynomial
`0xEDB88320`), not the STM32F3's HAL CRC peripheral — the peripheral would require a
CubeMX/.ioc regeneration, out of this phase's scope, and the blob is small (~1.8 KB) and
saved rarely (only on explicit save triggers), so software CRC's performance cost is
negligible against that access pattern.

(6) **Atomicity window (`vTaskSuspendAll()`/`xTaskResumeAll()`) covers the `seq++`/CRC-stamp
step too, not just the `HAL_FLASH` erase/program calls.** Section 5.6 says "scheduler
suspended around HAL_FLASH erase/program of the region," but `app_task` (normal priority,
section 4.3) is higher priority than `storage_task` (low priority) and could preempt
mid-CRC-computation if only the flash calls were covered, corrupting the CRC's view of
`s_blob.payload` mid-snapshot via a concurrent `Storage_Set*/Add*/Delete*` call. Extending the
suspended window to include the `seq`/`crc32` stamping makes the whole
snapshot-then-write sequence atomic against `app_task`, closing a race the plan's literal
wording didn't explicitly call out but which is required for the plan's own "newest valid
CRC wins" guarantee to actually hold.

(7) **Editor cursor-move/backspace/confirm actions reuse the LockType key as an
editor-local TYPE<->NAV toggle**, directly following `app_music.c`'s already-established
precedent (section 9.7 note 1: LockType repurposed for pot-role toggling inside Music,
not treated as a literal universal action in every app). This was not guessed —
`keypad.c`'s `dispatch_digit_key_release()` was read in full and confirmed to emit
`KEY_EV_NAV_UP/DOWN/LEFT/RIGHT/SELECT/DELETE` ONLY when the global `s_input_mode ==
INPUT_MODE_NAV`; in `INPUT_MODE_TYPE` those same digit keys emit only `KEY_EV_CHAR`
(multi-tap) or `KEY_EV_DIGIT` (hold >= `KEYPAD_HOLD_MS`). There is no third mode, so an
in-editor cursor-move/backspace/confirm action can only be reached by actually flipping the
global mode via LockType — `note_edit_on_key()`/`contact_edit_on_key()` cache the resulting
mode in a per-app `s_edit_mode` static (section 4.4: state persists in the app's own struct)
so the editor's own TYPE/NAV sub-state survives suspend/resume just like everything else.

(8) **"Contact 'Phone' field forces numeric mode" (section 1.2) implemented as an
event-level filter, not a third keypad mode.** Since keypad.h only has TYPE/NAV (see note 7
above — confirmed by the same full `keypad.c` read, not assumed), there is no keypad-engine
concept of "numeric-only TYPE mode" to invent. Instead, `contact_on_event()` discards
`KEY_EV_CHAR` (multi-tap letters) while `s_edit_field == 1` (Phone) and TYPE sub-mode is
active, accepting only `KEY_EV_DIGIT` (hold-digit literal) for that field — the Name field
(`s_edit_field == 0`) accepts both CHAR and DIGIT freely, same as both of Note's fields. This
is the smallest change that satisfies the plan sentence without adding new keypad engine
behavior.

(9) **"Note list starts in nav mode, editor in letter mode" (section 1.2) implemented via
explicit `Keypad_SetInputMode()` calls at each screen-entry boundary**: `note_enter_list()`/
`contact_enter_list()` call `Keypad_SetInputMode(INPUT_MODE_NAV)`; `note_enter_edit()`/
`contact_enter_edit()` set `s_edit_mode = INPUT_MODE_TYPE` and call
`Keypad_SetInputMode(s_edit_mode)`. `on_enter()` (called every time the app becomes current,
not just first-open, per `app.h`'s own contract) re-applies whichever mode the current screen
expects, so navigating away mid-edit and back via Menu resumes in the correct sub-mode
without needing any extra bookkeeping.

(10) **List screens force `Keypad_SetInputMode(INPUT_MODE_NAV)` on their own
`KEY_EV_LOCKTYPE` case**, discarding the toggle rather than acting on it. `keypad.c`
auto-toggles its global mode on every LockType press regardless of which app/screen is
active (confirmed by reading `dispatch_action_key()`) — left unhandled, a stray LockType
press while sitting on the LIST screen would silently flip the global mode to TYPE and break
NAV_UP/DOWN/SELECT navigation with no on-screen indication why. Forcing it back to NAV is a
defensive no-op for a key that has nothing meaningful to toggle on that screen (no typing
happens there).

(11) **`Storage_RequestSave()` triggers, scoped to what Phase 6 actually owns:** section
5.6 lists five save triggers total ("leaving Note/Contact editor with changes, Settings
change, song upload, new high score, and on `/lock`"); only the first ("leaving Note/Contact
editor") and note/contact deletion (an implied "changed" case, not literally listed but
clearly a persisted-state change) are wired in this phase, on `KEY_EV_SELECT` (commit) and
`KEY_EV_DELETE` respectively in each app's LIST/EDIT key handlers. The remaining four
triggers (Settings/song-upload/high-score/`/lock`) are Phase 7/8/9-owned and will call
`Storage_RequestSave()` from their own future code once those subsystems exist — no
speculative wiring added here (YAGNI), consistent with section 9's reserved-byte-region
policy (note 3 above) of deferring subsystem logic without deferring on-flash layout.

**Build:** not attempted in-session per the standing user instruction. Verified by manual
code review only: `storage.c`'s `_Static_assert(sizeof(StorageBlob) <= STORAGE_PAGE_SIZE, ...)`
confirms the ~1.82 KB blob fits one 2 KB flash page at compile time; `app_note.c`/
`app_contact.c`'s `Event`/`App`/`TextField`/`KeyEventType`/`InputMode` field and enum usage
checked against `events.h`/`app.h`/`widgets.h`/`keypad.h`'s actual declarations; confirmed
`main.c`'s new `storageTask_attributes` uses `osPriorityLow` (a real `cmsis_os2.h` enumerator,
grepped directly, not assumed) matching section 4.3's "low" priority figure for
`storage_task`; confirmed `FreeRTOS.h`/`task.h` are valid direct includes in this codebase
(already used the same way in `freertos.c` for `TaskHandle_t`) before relying on them in
`storage.c` for `vTaskSuspendAll()`/`xTaskResumeAll()`.

**Next hardware test (Phase 6 exit criteria, section 6: "create/edit/delete notes &
contacts; power-cycle keeps them"):** open Note from Menu, confirm the list starts in NAV
mode (cursor blinks, 2/8 move selection, no typing); SELECT the "+ New Note" tail, confirm
the editor opens in TYPE mode (multi-tap letters build the title, hold-digit inserts a
literal digit); press LockType, confirm the on-screen mode label flips to NAV and 2/8 now
switch between title/body fields, 4/6 move the cursor, 0 backspaces; press 5 (SELECT) to
commit, confirm it returns to the LIST screen with the new title visible and a flash save is
logged; power-cycle the board and reopen Note, confirming the note survived; select an
existing note and press 0 (DELETE) from the LIST screen, confirm it's removed and a save is
logged; repeat the equivalent flow for Contact, additionally confirming the Phone field
only accepts hold-digit input (multi-tap letter presses on that field visibly do nothing)
while the Name field accepts normal multi-tap letters; confirm both apps' state (selected
index, scroll position, in-progress edit) survives navigating away to Menu and back via
`Phone_SwitchApp()`, per section 4.4's "`on_suspend()` is a no-op, state persists" contract;
IWDG must not reset during any of the above, including during the flash erase/program window.

---

### 9.9 Phase 7 implementation notes (shell completion: lock, Settings, UART commands)

**User confirmation before starting:** "ok, this phase is also done. one note worth knowing,
that in Info app the scroll doesn't work and Arman Bijari student ID is not properly shown.
now let's go for the next phase of the plan." — authorizing the move to Phase 7 per section
6's phase order (the two Info-app cosmetic notes are logged here for a future pass; they are
not this phase's scope). Same "stick to plan, verify don't guess" discipline as every prior
phase.

**Files added**, matching section 3's file tree: `Core/{Inc,Src}/rtc_time.h/.c` (RTC epoch
sync + HH:MM:SS string, not itself named in section 3 but required to implement bonus 3
`/time-{unix_epoch}` and the lock screen's live clock — both explicitly spec'd), `Core/{Inc,
Src}/leds.h/.c` ("green/red LED patterns: solid, blink-n, tick-driven"), `Core/{Inc,Src}/
cmdparse.h/.c` (UART command dispatcher), `Core/Src/app_lock.c`, `Core/Src/app_settings.c`
(both declared `extern` in `app.h` per the existing no-per-app-header convention).

**Files extended:** `serial.h/.c` gained `Serial_StartRx()` + a `HAL_UART_RxCpltCallback()`
byte-ISR line assembler (double-buffered `rx_line[2][SERIAL_RX_LINE_CAP+1]`, posts
`EV_UART_CMD` with `Event.c` pointing at the completed line). `storage.h/.c` replaced the
Phase 6 zeroed `settings_reserved[32]` placeholder with a real `StorageSettings` struct
(exactly 32 B, `_Static_assert`-verified, `STORAGE_VERSION` deliberately not bumped per the
reserved-byte policy from section 9.8 note 3) plus `Storage_GetSettings()`/
`Storage_GetSettingsMutable()` accessors. `app_config.h` gained `LED_BLINK_HALF_MS`,
`SERIAL_RX_LINE_CAP`, `SETTINGS_DEFAULT_AUTOLOCK_S`, `SETTINGS_DEFAULT_LDR_THRESHOLD`,
`LOCK_WRONG_PIN_FLASH_TICKS`. `app.h` gained `extern const App AppLock;`/`AppSettings;`.
`app_menu.c`'s `s_items[]` table repointed the `"Settings"` slot from `NULL` to
`&AppSettings`. `keypad.c`'s single global `HAL_GPIO_EXTI_Callback()` gained a `PIN_BTN_LOCK`
(PB3) branch, posting `EV_BTN_LOCK` directly — dispatched *before* the existing
`kp_state != KP_IDLE` guard, since PB3 is a single button on its own EXTI3 line, independent
of the row/col matrix debounce state machine, and must not be dropped just because a keypad
press happens to be mid-debounce. `phone.h`/`phone.c` gained `Phone_Boot()`, `Phone_Lock()`,
`Phone_UnlockToSaved()` and restructured `Phone_Init()`/`Phone_Dispatch()`/`Phone_Tick()` (see
decisions 1-3 below). `main.c` gained `#include "rtc_time.h"`/`"leds.h"` and
`RTC_Time_Init()`/`Leds_Init()`/`Serial_StartRx()` calls in the pre-kernel `USER CODE BEGIN 2`
block, same timing convention as `Storage_Init()`/`Buzzer_Init()`.

**Design decisions not fully spelled out in section 5.5/5.6/5.9, made explicitly here (not
guessed):**

(1) **`/start` boot-gate restructuring closes the deviation `phone.h` had flagged since
Phase 3.** Section 5.9 says boot is `idle "press /start" screen -> /start -> animated logo +
jingle -> Menu`, but `cmdparse.c` (the only thing that can receive `/start`) didn't exist
before this phase, so `Phone_Init()` had been showing the logo and auto-advancing to Menu on a
fixed timer instead. `Phone_Init()` now only draws the idle screen; a new `Phone_Boot()` —
called exclusively from `cmdparse.c`'s `/start` handler — does the logo-then-Menu transition,
guarded by a `static uint8_t s_booted` flag so a stray repeated `/start` after boot is an
explicit no-op rather than re-triggering the boot animation.

(2) **Lock overlay remembers the pre-lock app via a single `static const App *s_locked_from`
in `phone.c`, reusing the existing `Phone_SwitchApp()` machinery unchanged.** `Phone_Lock()`
stores `s_current` into `s_locked_from` before switching to `&AppLock`; `Phone_UnlockToSaved()`
switches back to `s_locked_from` (or `&AppMenu` as a defensive fallback if somehow NULL) and
clears it. No special-casing was needed in `Phone_SwitchApp()` itself — `AppLock`'s own
`on_enter()`/`on_event()`/`render()` does all lock-specific work, and every other app's
`on_suspend()` staying a no-op (section 4.4) means whatever screen/state the user was on is
still sitting there in that app's own static struct when `Phone_UnlockToSaved()` switches back
to it, satisfying section 5.9's "unlock resumes the exact pre-lock screen/state" literally,
for free.

(3) **`Phone_Dispatch()` routes `EV_UART_CMD` to `Cmdparse_HandleLine()` *before* the
`s_current == NULL` early-return guard**, the one deliberate exception to "no event is
processed until an app is current." The very first `/start` command must be processable while
`s_current` is still `NULL` (pre-boot, before `Phone_Boot()` has ever run) — routing it after
the guard would make `/start` itself unreachable. All other event types (`EV_KEY`,
`EV_SONG_END`, `EV_BTN_LOCK`) still early-return on `s_current == NULL`, since none of them
are meaningful before boot.

(4) **`KEY_EV_BACK`'s existing global handler (section 1.2: "back always returns to Menu") now
additionally excludes `s_current == &AppLock`**, alongside its pre-existing
`s_current != &AppMenu` exclusion — otherwise BACK would let the user bypass the lock screen
entirely, which section 5.9's PIN-gate intent clearly forbids even though the plan text
doesn't spell out this specific interaction. `KEY_EV_SHORTCUT_C` (QuickLock) goes live in this
phase for the same reason A/B (Vol+/-) went live in Phase 4 (section 9.7): shortcut wiring was
deferred only "until those subsystems ... exist" per the risk register, and the lock-screen
subsystem now exists. Shortcut D (Screenshot) remains deferred — its subsystem (a UART
screenshot-dump command) is not part of this phase's `cmdparse.c` scope (see decision 6 below)
and stays unbuilt.

(5) **Auto-lock is polled from `Phone_Tick()`, not driven by the pre-existing `EV_AUTOLOCK`
event type.** `events.h` already had `EV_AUTOLOCK` defined (anticipated in an earlier phase),
but nothing produces it, and inventing a producer (e.g. a periodic 1 Hz idle-check event) would
just be a slower, more complex path to the same result `Phone_Tick()` already gives for free:
it already runs every `APP_TASK_TICK_MS` regardless of event traffic (section 4.3's own
rationale for `Phone_Tick()` existing at all — see section 9.5). `Phone_Tick()` now tracks
`s_last_activity_tick` (updated on every `EV_KEY` in `Phone_Dispatch()`) and, when
`s_current != &AppLock`, compares elapsed idle time against `Storage_GetSettings()->
autolock_s * 1000` (0 = disabled), calling `Phone_Lock()` and returning early (skipping that
tick's `on_tick()`/`render()` to avoid rendering the just-suspended app one extra frame) once
the threshold is crossed. `EV_AUTOLOCK` itself stays defined-but-unused — removing it would be
an unrelated cleanup outside this phase's scope, not a Phase 7 decision.

(6) **`cmdparse.c` fully implements the base command set (`/start`, `/reset`, `/lock`,
`/setting-{NAME}-{VALUE}`) plus bonus 3 (`/time-{unix_epoch}`), and explicitly rejects the
remaining bonus verbs (`/piano-on`, `/pn-{freq}`, `/pf`, `/songup-{name}-{count}` + its
follow-up lines, `/end`) with `ERR not yet implemented: <line>` rather than routing them into
the generic `ERR unknown/invalid: <line>` bucket.** Section 6 lists Phase 7's exit criterion as
"UART command set complete with invalid-input rejection" and separately lists "Phase 9 — PC
bonuses: time_sync.py + RTC + Settings 7-seg time mode; piano.py ...; song_upload.py ..." —
i.e. the plan itself scopes `/time-` to this phase (trivial anyway, `rtc_time.c` already
exists) but scopes the piano/song-upload verbs' *owning subsystems* (a live piano app,
uploaded-song storage) to Phase 9. Building those subsystems now would be guessing ahead of
Phase 9's own design work; recognizing-and-rejecting them now (rather than only wiring them in
Phase 9 with zero mention here) still satisfies "invalid-input rejection" literally — rejection
is rejection, the reason just differs and is now visible over UART instead of silently falling
into the generic bucket.

(7) **PIN entry (both `app_lock.c`'s unlock gate and `app_settings.c`'s PIN-set flow) reuses
TYPE mode's hold-digit `KEY_EV_DIGIT` mechanism, not a dedicated numeric input mode.**
`keypad.h` only has `INPUT_MODE_TYPE`/`INPUT_MODE_NAV` (confirmed by the same full `keypad.c`
read section 9.8 notes 7/8 already cite — no third mode exists), so PIN entry follows
`app_contact.c`'s already-established Phone-field precedent exactly: force `INPUT_MODE_TYPE`,
accept only `KEY_EV_DIGIT`, discard everything else relevant to typing. `app_lock.c` compares
the 4-digit buffer against `Storage_GetSettings()->pin` and calls `Phone_UnlockToSaved()` on
match, resetting to empty and showing a timed "Wrong PIN" message on mismatch (never lets a
wrong attempt or partial entry linger — reset happens both on wrong-PIN and on
`on_enter()`/re-entry). `app_settings.c`'s PIN-set flow is independent (writes+saves rather
than compares) but shares the same 4-hold-digit shape; a stale in-progress capture is
discarded in `settings_on_enter()` since `phone.c`'s global BACK handler exits straight to
Menu without ever routing through `app_settings.c`'s own event handler, so leftover partial
capture state must be swept on the way back in rather than on the way out.

(8) **`app_settings.c` is a single always-NAV row editor (LEFT/RIGHT steps the selected row's
value in place, SELECT toggles booleans / opens PIN capture), not a separate LIST/EDIT
two-screen flow like `app_note.c`/`app_contact.c`.** There is no variable-length collection
here — the plan's Settings params (LDR threshold, icon layout, volume, uartset, autolock,
segtime, plus PIN enable/value) are a fixed 8-row set — so a second "editor" screen per row
would add a navigation layer the spec never asks for (YAGNI, matching this codebase's
established policy against speculative structure). Every change applies immediately to the
live RAM `StorageSettings` and calls `Storage_RequestSave()` (section 5.6: "Settings change" is
a save trigger), rather than requiring a separate commit gesture, since there is no meaningful
"discard changes" concept for a single-value row the way there is for a half-typed note title.

(9) **`Storage_RequestSave()` triggers completed this phase:** section 5.6's "Settings change"
and "on `/lock`" triggers (the two of the original five left un-wired after Phase 6 note 11
that belong to this phase's own subsystems) are now both wired — every `app_settings.c` row
change and every `Phone_Lock()` call requests a save. "Song upload" and "new high score"
remain Phase 9/8-owned respectively, per the same reserved-scope reasoning as before.

(10) **Volume changes made from `app_settings.c`'s Volume row call `Buzzer_SetVolume()`
immediately, mirroring `cmdparse.c`'s `/setting-volume-*` handler**, rather than only writing
`StorageSettings.volume` and waiting for some later sync point — there is no such sync point
in this codebase (the live buzzer volume and the persisted setting are two separate pieces of
state kept in lockstep by every writer, same pattern the Vol+/- shortcuts in `phone.c` already
established in Phase 4), so both write paths must apply the change live to stay consistent with
each other.

(11) **PB3's `EV_BTN_LOCK` path has no software debounce**, unlike the row/col matrix's
explicit `KEYPAD_DEBOUNCE_MS` state machine. Section 1.1 wires PB3 with a pull-down for a clean
rising edge (a single dedicated button, not a multiplexed matrix prone to ghosting), and
`Phone_Lock()` is already idempotent (no-ops if already locked or not yet booted) — so a stray
extra `EV_BTN_LOCK` from switch bounce is a harmless no-op rather than a bug needing its own
debounce state machine, the same reasoning that keeps `keypad.c`'s existing row/col debounce
scoped to only the matrix pins.

**Build:** not attempted in-session per the standing user instruction. Verified by manual code
review only: confirmed `EXTI3_IRQHandler`/`USART1_IRQHandler` (both NVIC-level dispatch) and
`MX_RTC_Init()` (constructing `hrtc`) already existed pre-phase in `stm32f3xx_it.c`/`main.c`,
anticipated by earlier phases, so this phase only needed to write the HAL callback bodies
(`HAL_GPIO_EXTI_Callback()`'s new PB3 branch, `HAL_UART_RxCpltCallback()`) and RTC-facing logic
on top of already-initialized hardware; confirmed `PIN_BTN_LOCK`/`PIN_BTN_LOCK_PORT` and
`PIN_LED_GREEN/_PORT`/`PIN_LED_RED/_PORT` already existed in `pinmap.h`; confirmed
`StorageSettings`'s field order (both `uint16_t` fields declared first and contiguous, then all
`uint8_t`/`char` fields) produces zero compiler alignment padding by construction, verified
against the `_Static_assert(sizeof(StorageSettings) == 32)` reasoning rather than assumed;
confirmed every new `Event`/`KeyEventType`/`App` field and enum usage against `events.h`/
`keypad.h`/`app.h`'s actual declarations; checked every new/edited file's `/*`...`*/` comment
delimiter counts balance (a recurring bug class in earlier phases per this codebase's own
history) via a direct grep sweep rather than assuming correctness.

**Next hardware test (Phase 7 exit criteria, section 6: "full PDF command table passes"):**
power-cycle the board, confirm the idle "press /start" screen shows and stays (no auto-advance
to Menu); send `/start` over Termite, confirm the animated logo shows for `BOOT_LOGO_MS` then
Menu appears; send a stray repeated `/start`, confirm it's a silent no-op (still on Menu, no
re-animated logo); send `/lock`, confirm the lock screen shows a live HH:MM:SS clock (ticking
every second) and, before any `/time-` sync, the "(unsynced -- /time)" hint; send
`/time-{current_unix_epoch}`, confirm the clock jumps to the correct time and the hint
disappears; with no PIN set, press any key, confirm it unlocks back to Menu; open Settings,
toggle "PIN lock" On, SELECT "Set PIN", enter 4 digits, confirm it saves; press PB3 (or send
`/lock`, or press shortcut C), confirm the lock screen now requires the PIN — enter it wrong,
confirm "Wrong PIN" flashes then clears for retry; enter it correctly, confirm it resumes
exactly the pre-lock app/screen (not Menu) if that app wasn't Menu; set Settings' Autolock to a
short nonzero value (e.g. 10s via LEFT/RIGHT), leave the phone idle, confirm it auto-locks at
roughly that interval and that any keypress elsewhere resets the idle timer; confirm BACK does
nothing on the lock screen (cannot bypass it); exercise every `/setting-{NAME}-{VALUE}` name
(`ldr`, `icons`, `volume`, `uartset`, `autolock`, `segtime`) with both valid and out-of-range
values, confirming `OK`/`ERR unknown/invalid` respectively, and confirm settings changes made
via UART are visible when Settings is next opened; toggle `uartset` off via Settings, then
attempt a `/setting-*` command, confirm it's rejected with the UART-settings-OFF message; send
`/reset`, confirm the board reboots; send each bonus-protocol verb (`/piano-on`, `/pn-440`,
`/pf`, `/songup-test-1`, `/end`). **Superseded by Phase 9 (§9.12):** those verbs are now
fully implemented (Phase 7 historically replied `ERR not yet implemented`); do not expect
the Phase 7 rejection strings after flashing Phase 9 firmware.
distinctly from a genuinely garbled line (which must still reply the generic `ERR
unknown/invalid: <line>`); power-cycle after all of the above, confirm every setting (including
the PIN and its enabled flag) survived; IWDG must not reset during any of the above.

**Post-Phase-7 hardware fix (bug report: "Settings screen keeps reloading on its own"):**
after the first hardware build, user reported the Settings screen appearing to spontaneously
reload with no physical key press. Root cause, confirmed by code reading (not guessed):
`Phone_Dispatch()`'s `EV_UART_CMD` branch (section 9.9 decision 3) returned immediately after
calling `Cmdparse_HandleLine()` without ever touching `s_last_activity_tick` -- only `EV_KEY`
updated it. A user actively driving the phone over UART (e.g. testing `/setting-*` commands
while sitting in Settings, with no physical keypad presses in between) was therefore still
subject to the `autolock_s` idle timer expiring in the background; once it fired,
`Phone_Tick()` silently switched to `AppLock`, and since `pin_enabled` defaults to 0 (any key
unlocks), the very next physical keypress instantly unlocked back to the exact same Settings
screen -- visually indistinguishable from the screen "reloading" itself. Fix: `Phone_Dispatch()`
now stamps `s_last_activity_tick = HAL_GetTick()` in the `EV_UART_CMD` branch too, so UART
traffic counts as activity exactly like keypad traffic. Harmless to also do this pre-boot
(`s_current` is still `NULL` then, so `Phone_Tick()`'s autolock check stays a no-op until
`Phone_Boot()` runs). Re-test: repeat the autolock hardware-test step above but this time issue
`/setting-*` UART commands (no physical key presses) throughout a period longer than
`autolock_s` while sitting in Settings, and confirm the screen no longer auto-locks/instantly
unlocks during that UART-only activity.

### 9.10 Phase 8 implementation notes (PvZ game)

**User confirmation before starting:** user approved moving on to Phase 8 ("ok done. now
let's go for the next phase. i know that plan covers everything but i will also send the
project spec so you wouldn't have any problem with it"), followed by a Persian-language
project-spec excerpt describing the PvZ requirements: grid lanes, day/night plants, an ice
plant with a slow effect (bonus), a tank zombie variant (bonus), lives, score, and a
score-driven inventory bonus. That spec text is the concrete source for the design decisions
below, alongside section 5.8's existing scope description.

**Files added:** `pvz_engine.h` / `pvz_engine.c` (pure simulation core -- grid cells,
zombies, bullets, spawn/fire/tick/score logic, zero hardware/UI calls, own xorshift32 PRNG
seeded by a caller-supplied salt rather than `HAL_GetTick()`) and `app_pvz.c` (the `App`
target: screens, input, rendering, and all hardware-facing calls). Split into engine/app per
this project's "many small files" convention -- the same split app_music.c/app_note.c etc.
already follow between pure logic and hardware glue.

**Files extended:** `app_config.h` (all `PVZ_*` tunables: grid/HUD dimensions, pool sizes,
lives, plant-select count, score constants, fixed-point field width/bullet speed, default
damage/zombie-speed/difficulty/start-plants, tank HP multiplier/spawn cadence, ice slow
percent/duration, zombie base HP/attack cadence, plant start HP, and
`PVZ_PLANT_FIRE_TICKS` added this phase for the auto-fire cadence); `storage.h`/`storage.c`
(`StorageSettings.pvz_damage`/`pvz_zombie_speed`/`pvz_difficulty`/`pvz_start_plants` rows plus
a `StorageHighscore` struct and `Storage_GetHighscore()`/`Storage_SetHighscoreIfBetter()`,
added in the segment immediately preceding this implementation); `cgram.h`/`cgram.c` (a PvZ
icon/glyph bank for plants/zombies/bullets/hearts on the 5x8 CGRAM cells); `app.h` (`extern
const App AppPvz;`); `seg7.h`/`seg7.c` (`SEG_GAME` mode driving the `SS.CC`
survival-seconds/score display); `main.c` (TIM16 started at 10 Hz producing
`EV_GAME_TICK`, alongside the pre-existing TIM2 1 Hz `EV_TICK_1S`); `app_menu.c` (`s_items[]`
PvZ row repointed from a NULL "not yet implemented" target to `&AppPvz`); `phone.c`
(`Phone_SwitchApp()` extended with phone-music snapshot/restore around AppPvz so game BGM
can own the single buzzer channel, plus defensive `Seg7_SetMode(SEG_OFF)` / LED off on
leaving AppPvz, mirroring the pre-existing AppMusic pot-role/Seg7 reset pattern in the same
function); `buzzer.h`/`buzzer.c` (`Buzzer_Stop()` for leave-without-restore).

**Design decisions not fully spelled out in section 5.8, made explicitly here (not guessed):**

1. **Engine/app split.** `pvz_engine.c` never touches `UI_*`, `Cgram_*`, `Buzzer_*`,
   `Leds_*`, `Analog_*`, or `Storage_*` -- it only knows grid/zombie/bullet state and pure
   tick logic, taking `is_night` as a plain `uint8_t` parameter rather than calling
   `Analog_IsNight()` itself. `app_pvz.c` is the only file that touches hardware/UI APIs.
   This mirrors the codebase's existing convention (see pvz_engine.h's own header comment)
   and keeps the simulation reasoned-about/testable independent of the LCD/CGRAM state.
2. **Zombie-vs-plant blocking behavior.** A zombie that reaches a live plant's cell stops
   advancing and attacks that plant every `PVZ_ZOMBIE_ATTACK_TICKS` ticks (classic
   lane-blocking) rather than pushing through or detouring around it. Chosen because the
   spec only says plants "block/kill" zombies, not how contact resolves when a plant
   survives a hit; stop-and-chew is the standard genre behavior and the simplest rule that
   still lets a zombie eventually break through a plant with enough HP loss.
3. **Day/night/ice plant firing eligibility.** Day plants fire only while `!is_night`,
   night plants only while `is_night` (direct mapping of the spec's day/night plant split);
   ice plants fire in both states, since the ice plant is described as a bonus utility unit
   independent of day/night rather than a third day/night-gated type.
4. **Ice slow-on-hit mechanic.** A bullet fired from an ice-plant cell carries a
   `from_ice` flag; on hitting a zombie it applies `PVZ_ICE_SLOW_PCT`/`PVZ_ICE_SLOW_TICKS`
   slow on top of normal damage, rather than the ice plant having a separate no-damage
   "freeze aura" mechanic. Chosen because the spec's ice-plant bonus is described purely as
   a slow effect on hit, and reusing the existing bullet/damage pipeline avoids a second
   parallel effect system for a single bonus unit.
5. **Tank zombie.** Every `PVZ_TANK_EVERY_N`th spawn is a tank with
   `PVZ_TANK_HP_MULT`x normal HP and otherwise-identical movement/attack behavior --
   deterministic cadence rather than random-chance spawning, so play-testing/tuning the
   spawn rhythm is reproducible.
6. **Score-driven inventory bonus.** Every `PVZ_SCORE_PER_BONUS_PLANT` points earned
   (kills *and* the minute-wrap bonus point -- PDF p.8) grants +1 plant to whichever of
   day/night inventory matches the *current* `is_night` state at the moment the threshold
   is crossed (never the ice inventory). PDF p.7 only defines two plant types (day/night);
   ice is bonus-4 only.
7. **Ice plant starting grant.** `PvzEngine_NewGame()` calls
   `PvzEngine_GrantIcePlant(st, PVZ_START_ICE_PLANTS)` (`=1`) so bonus-4's ice unit is
   placeable. Score bonuses never grant ice (note 6). PDF bonus item 4 only requires a
   special zombie *or* plant; the plan keeps both (tank + ice).
8. **In-game plant-type selection.** Placement-type cycling (day/night/ice) reuses
   `KEY_EV_LOCKTYPE` while AppPvz stays in NAV input mode, following the exact precedent
   app_music.c (pot SEEK/VOLUME toggle) and app_settings.c (forced-NAV LOCKTYPE handler)
   already established for repurposing LOCKTYPE as an in-app secondary toggle instead of
   its literal keypad-lock meaning, since PvZ has no use for keypad-lock during a game.
9. **Game-tick scope.** `EV_GAME_TICK` (10 Hz) only drives `PvzEngine_Tick()` while
   `s_screen == PVZ_SCREEN_GAME` is actually showing -- not during AppPvz's
   Menu/PlantSelect/Settings/GameOver screens. This makes "Continue" a true pause-in-place
   (no simulation progresses while the player is off the game screen) rather than a
   silently-still-running background game.
10. **Game BGM + phone-music restore (PDF p.7 / §5.8).** PDF requires background game
    music on every PvZ screen and that phone music stop while the game is open; §5.8 also
    asks leaving to resume phone music. Single TIM8 channel: `Phone_SwitchApp()` snapshots
    `(melody_idx, seek_pct)` of any playing phone track before AppPvz starts
    `PVZ_BGM_MELODY_IDX`, restores via `PlayMelody`+`SeekPercent` on leave (or
    `Buzzer_Stop()` if nothing was playing). `Music_HandleSongEnd` is skipped while
    `s_current == &AppPvz`; AppPvz loops BGM on `EV_SONG_END` (except on the game-over
    screen, where `PVZ_GAMEOVER_MELODY_IDX` / "Alert" is the §5.8 game-over jingle).
11. **Life-loss clear + LED.** §5.8 "−1 life (red LED blink, sad SFX) and clears": on
    breach, at most one life per tick is lost, all active zombies/bullets are deactivated
    (planted cells stay), red blinks, non-fatal plays `BUZZER_SFX_ERROR`. Green LED
    (§1.1 "PvZ life OK") is solid on while `lives > 0` during an active run. Fatal breach
    returns `PVZ_TICK_LIFE_LOST` with `game_over` set so the app still does the blink
    before the game-over jingle.
12. **`pvz_start_plants` wired.** Plant-select pick count is
    `StorageSettings.pvz_start_plants` (PDF p.8 setting; ceiling
    `PVZ_PLANT_SELECT_COUNT` / PDF p.7 "چهار"), not a hardcoded 4.
13. **Front-most bullet hit.** A bullet hits the overlapping zombie with the smallest `x`
    in its lane (nearest the plants), not the first active pool slot.
14. **HUD/field column layout.** The 20-column LCD is split into a fixed
    `PVZ_HUD_COLS`-wide left HUD (hearts/score/current placement-type inventory count) and a
    `PVZ_GRID_COLS`-wide field to its right -- 3 columns cannot show day+night+ice counts
    at once; LOCKTYPE cycles the displayed type. Matches the PDF's left-inventory / right-
    field layout without inventing a second HUD page.
15. **Fixed-point position units.** Zombie/bullet x-position and speeds are tracked in
    "hundredths of a cell" integers (`PVZ_FIELD_WIDTH_HUNDREDTHS`,
    `PVZ_BULLET_SPEED_HUNDREDTHS`) rather than floats, consistent with this project's
    no-floating-point convention elsewhere (see app_config.h's own comment on the constant).
16. **High score persistence.** `Storage_SetHighscoreIfBetter()` is called on every game
    over, with `Storage_RequestSave()` only issued when the call actually improved the
    stored high score -- avoiding a flash write on every single game over, only on ones
    that matter.

**Build:** verified by code review only (no on-device build attempted this session, per the
user's own build/flash process) -- checked that `pvz_engine.h`'s struct definitions match
their usages in `pvz_engine.c`/`app_pvz.c` exactly (`PvzState`/`PvzCell`/`PvzZombie`/
`PvzBullet` field names/types), that every `PVZ_*` constant referenced in `pvz_engine.c` and
`app_pvz.c` is defined in `app_config.h`, that `app_menu.c`'s `s_items[]` table and
`phone.c`'s `Phone_SwitchApp()` both reference `AppPvz` consistently with its `extern`
declaration in `app.h` and its definition in `app_pvz.c`, and that comment delimiters and
brace/paren nesting in all three new/changed files are balanced.

**Next hardware test (Phase 8 exit criteria, section 6):** flash and open Menu, select PvZ,
confirm game BGM starts on the PvZ menu and loops; confirm the plant-select screen lets the
user split `pvz_start_plants` (Settings) between day/night inventory; start a new game and
confirm 1 ice plant is in inventory (LOCKTYPE cycle), HUD/field render, green LED on;
place day/night/ice plants and confirm day/night LDR gating + ice slow-on-hit; confirm
score bonuses (kills and minute wrap) grant the correct day/night type; breach → life lost,
field threats clear, red blink, sad SFX; both lives out → game-over jingle + high score;
Continue / music restore / Settings snapshot / 7-seg `SS.CC` as before; IWDG stays happy.

### 9.10a Phase 8 hotfix (post-implementation bug report, after user hardware testing)

User re-tested Phase 8 on hardware after implementing Phase 11 themselves and reported 5
issues. Root-cause findings and fixes (code review only, no on-device build this session, per
the user's own build/flash process):

1. **"I want more specific lcd characters for zombies and plants."** `cgram.c`'s
   `s_pvz_icons[8][8]` redrawn: the previous glyphs for plant day/night/ice shared the same
   peashooter-head *silhouette* with only 1-2 detail pixels changed, and zombie/tank shared the
   same body outline with only eye pixels changed -- readable as a bitmap diff, not readable as
   a 5x8 dot-matrix cell during play. New glyphs give each icon a distinct silhouette
   (open-mouth peashooter / domed spotted mushroom / crowned peashooter; narrow arms-out zombie
   / wide full-block armored tank; round pea / taller cross-marked snowflake pea).
2. **"When a zombie moves for a second something like its shadow is seen."** Reviewed the full
   render pipeline (`app_pvz.c`'s full-back-buffer rebuild every `render()` call, `ui.c`'s
   dirty-diff writer, `LiquidCrystal.c`'s 4-bit write timing, `main.c`'s TIM16 wiring) and found
   no software logic bug -- every frame is rebuilt from scratch and the diff-render correctly
   detects and rewrites any cell whose content changed. The most plausible remaining explanation
   is that `ui.c`'s `UI_RenderDirty()` writes are fire-and-forget (`LiquidCrystal.c`'s `write()`
   comment: "raw byte write ... assume success", no HD44780 busy-flag readback), so if one
   individual hardware write is ever dropped (marginal/noisy 4-bit data line, common on
   breadboarded HD44780 wiring), `s_shown[][]` still records it as delivered and that exact cell
   is never revisited until its *content* changes again -- producing a stale "ghost" glyph. Since
   true write verification needs the R/W pin wired for busy-flag polling (a hardware change, not
   in scope here), added a cheap self-healing mitigation instead: `UI_RenderDirty()` now forces a
   full-screen rewrite (bypassing the dirty diff) once every `UI_FORCE_REDRAW_EVERY_N_RENDERS`
   (33) calls, i.e. roughly once per second at `UI_RENDER_TICK_MS`=30 ms, silently correcting any
   single missed write. **Needs hardware confirmation** -- if the shadow persists after this fix,
   it points more strongly at a wiring/breadboard-contact issue than software.
3. **"Plant do not fire those bullets."** `pvz_engine.c`'s `plants_fire()`/`bullets_tick()` logic
   was reviewed line-by-line and found internally consistent with the documented design (plants
   only fire once a zombie is active in their row; day plants gated `!is_night`, night plants
   gated `is_night`, ice plants always). The confirmed bug is in `spawn_tick()`'s row RNG: it
   *reseeded* a local `xorshift32_next()` state fresh from a small monotonically-increasing salt
   (`zombies_spawned+1`) on every single spawn and took only one step off that fresh seed --
   xorshift's avalanche is weak after one step from small sequential seeds, so the resulting row
   sequence was a near-deterministic rotation (hand-verified for seeds 1..5: rows came out
   1,2,3,0,1,2,3,0,...) rather than anything random. In practice some lanes could go a long time
   without a zombie, so any plant placed there would look like it "never fires" even though the
   firing logic itself is correct. Fixed by adding a persistent `PvzState.rng_state` field,
   seeded once in `PvzEngine_NewGame()` (mixed from the settings snapshot) and advanced -- never
   reseeded -- by every `spawn_tick()` call, giving a real evolving PRNG stream while staying
   `HAL_GetTick()`-free (`pvz_engine.c` stays hardware-free per its own header comment). **Needs
   hardware confirmation** -- if plants still appear not to fire after this fix, check whether
   `Analog_IsNight()`'s LDR reading matches the physical room lighting (a day plant will
   correctly refuse to fire while the LDR reads "night", by design in section 5.8/PDF p.7).
4. **"Zombie rate should change shouldn't it?"** Confirmed: `spawn_tick()` always reset
   `spawn_timer` to the flat Settings-configured `st->difficulty`, with no escalation anywhere in
   `pvz_engine.c` -- a game 5 minutes in spawned at exactly the same rate as second 1. Added a
   ramp: `app_config.h`'s new `PVZ_DIFFICULTY_RAMP_EVERY_N` (5 zombies) /
   `PVZ_DIFFICULTY_RAMP_STEP` (3 ticks) / `PVZ_DIFFICULTY_MIN` (12 ticks, ~1.2 s floor) --
   `spawn_tick()` now computes `effective_difficulty = max(PVZ_DIFFICULTY_MIN, difficulty - (zombies_spawned / RAMP_EVERY_N) * RAMP_STEP)`
   each spawn, so the field gets steadily harder over a run without ever becoming literally
   unplayable. Numeric values are this file's own design choice (not a plan/PDF figure), same
   footing as the other Phase 8 tunables in `app_config.h`.
5. **"Music is not plants vs. zombies. Make it more accurate."** Confirmed:
   `PVZ_BGM_MELODY_IDX` pointed at `buzzer.c`'s "Bones" track (a generic melody, index 6), with
   no PvZ-style tune anywhere in the codebase. Added a new original melody, `s_melody_pvz` in
   `buzzer.c` (`MELODY_COUNT` 13 -> 14, new "PvZ Theme" table entry at index 13) -- a short
   bouncy, whimsical garden-march loop in a major key (not a transcription of any copyrighted
   score, an original composition matching the genre's character) -- and repointed
   `PVZ_BGM_MELODY_IDX` to it. `PVZ_GAMEOVER_MELODY_IDX` (Alert, index 3) is unaffected since the
   new melody was appended at the end of the table.

**Build:** verified by code review only -- checked `PvzState.rng_state`'s new field is
initialized in every `PvzEngine_NewGame()` path and referenced consistently in `spawn_tick()`
and `xorshift32_next()`'s call site; checked `buzzer.c`'s `MELODY_COUNT` bump to 14 matches the
new `s_melodies[]` row count and that no other file hardcodes `13` as the melody count; checked
`app_config.h`'s new `PVZ_DIFFICULTY_*`/`PVZ_BGM_MELODY_IDX` constants are referenced correctly
in `pvz_engine.c`; checked `cgram.c`'s redrawn `s_pvz_icons[8][8]` bytes all stay within the
5-bit (0x00-0x1F) CGRAM row range; checked `ui.c`'s new forced-full-redraw counter doesn't
change `UI_RenderDirty()`'s per-row IWDG-kick call pattern.

**Next hardware test:** flash, replay a PvZ game, and report back on all 5 items above --
items 1/4/5 should be immediately and unambiguously confirmable; items 2/3 carry a "needs
hardware confirmation" note above since no on-device root cause could be pinned down purely
from source review.

### 9.10b Phase 8 hotfix ROUND 2 -- regression from item 2's mitigation (critical, hardware-confirmed)

User re-tested on real hardware and reported a severe regression directly caused by item 2's
fix above: plants stopped firing entirely, zombies were never killed, and the UART log showed a
continuous flood of `[EVT] queue full, dropped type=2 (total dropped=N)` (N climbing past 200
within one short session) starting the instant PvZ opened and continuing throughout play. User
was explicit: "it didn't happen before your edits."

**Root cause (confirmed by code review, no guessing):** `type=2` is `EV_GAME_TICK` (`events.h`'s
`EventType` enum: `EV_KEY=0, EV_TICK_1S=1, EV_GAME_TICK=2, ...`), posted by TIM16 at 10 Hz
(`main.c`) into `eventQueue`, a fixed `EVENT_QUEUE_LEN`=16-deep `osMessageQueueNew()`
(`app_config.h`/`events.c`) drained only by `app_task` (`phone.c`'s `Phone_Dispatch()` /
`Phone_Tick()`). Both of those unconditionally call `s_current->render()` synchronously right
after every dispatched event/tick (`phone.c` lines ~279-281 and ~309-311) -- so `app_task`'s
ability to keep draining `eventQueue` at 10 Hz depends on `render()` returning quickly every
time. `render()` for any app (via `ui.c`'s `UI_BeginFrame`/`UI_Print`/`UI_EndFrame`) calls
`ui_lock()` -> `osMutexAcquire(s_mutex, osWaitForever)` -- the *same* `s_mutex` that item 2's
fix made `UI_RenderDirty()` (running in `ui_task`, not `app_task`) hold for its *entire* redraw
loop, once every `UI_FORCE_REDRAW_EVERY_N_RENDERS` (33) calls. A full forced 80-cell HD44780
redraw costs roughly cells x ~12 ms (`LiquidCrystal.c`'s `pulseEnable()`: 3x `HAL_Delay(1)` per
`write4bits()`, 2x per `write()`/`command()`) -- up to roughly 1 s, once per second, entirely
inside that mutex. Every time this coincided with an `EV_GAME_TICK` needing to render, `app_task`
stalled on the mutex for up to ~1 s; with TIM16 posting a new `EV_GAME_TICK` every 100 ms, the
16-deep queue overflowed almost immediately whenever `app_task` was blocked this way -- exactly
the reported drop flood. `PvzEngine_Tick()` (only invoked from `app_pvz.c`'s handling of a
delivered `EV_GAME_TICK`) was therefore starved of ticks almost entirely, which is what actually
produced "plants don't shoot" / "zombies don't get killed" -- **not** a bug in `plants_fire()`/
`bullets_tick()` or the round-1 RNG fix, both of which were never given enough ticks to run.

**Fix:** reverted `ui.c`'s forced-full-redraw mechanism entirely -- removed
`UI_FORCE_REDRAW_EVERY_N_RENDERS`/`s_force_redraw_ctr` and the `force_full` branch;
`UI_RenderDirty()` is back to pure dirty-diff (`s_back[r][c] != s_shown[r][c]`), matching its
pre-hotfix behavior. This directly un-does item 2's speculative mitigation, which was already
flagged above as "needs hardware confirmation" and is now confirmed actively harmful rather than
merely unconfirmed. Item 2's original "shadow" symptom's root cause is therefore still
unresolved at the software level; if it recurs, any future mitigation must not hold `s_mutex` for
anywhere near this long in one call (e.g. force at most one extra cell per `UI_RenderDirty()`
call, spread across many ticks, instead of all 80 cells in a single blocking pass) -- or it will
reproduce this exact regression.

**Build:** verified by code review only -- confirmed `ui.c` no longer defines
`UI_FORCE_REDRAW_EVERY_N_RENDERS`/`s_force_redraw_ctr`, confirmed `UI_RenderDirty()`'s per-row
IWDG-kick (`on_row_done()`) call pattern is unchanged, confirmed no other file references the
removed constant/counter, confirmed `events.h`'s `EventType` ordering (`type=2` = `EV_GAME_TICK`)
and `events.c`'s `EVENT_QUEUE_LEN`=16 / synchronous `render()` call sites in `phone.c` support the
starvation diagnosis above.

**Next hardware test:** flash and replay a PvZ game -- expect the `[EVT] queue full, dropped
type=2` flood to disappear (occasional isolated drops under a genuine burst are fine; a sustained
climbing flood is not), and plants/zombies should behave per items 3/4 above once
`PvzEngine_Tick()` is actually running at ~10 Hz again. Item 2 (shadow) is back to its original,
still-unconfirmed status -- report separately whether it's still observed now that ticks aren't
being starved (a starved game loop could itself have produced shadow-like stale-frame artifacts,
so this may turn out to be the same bug).

### 9.10c Phase 8 hotfix ROUND 3 (BACK navigation, fire-rate consistency, spawn pacing)

User raised 3 more issues while retesting: BACK during a live PvZ game exits to the smartphone
Menu instead of the PvZ menu; plant fire rate feels a little high and inconsistent; two zombies
sometimes appear to spawn at once.

1. **"If i'm in a PvZ game and i press back, it has to get back to the menu of PvZ not the menu
   of smartphone itself."** Confirmed: `app_pvz.c`'s `pvz_on_back()` explicitly handled
   `PVZ_SCREEN_GAME` via the `default: return 0u` fallthrough, which per `phone.c`'s
   `Phone_Dispatch()` (`if (s_current->on_back != NULL && s_current->on_back() != 0u) { ... }
   else { Phone_SwitchApp(&AppMenu); }`) hands BACK straight to the phone shell's root Menu --
   this was a deliberate original design choice ("Continue relies on exiting to phone Menu", per
   the old comment) but the user wants BACK during play to pause into the *PvZ* menu instead.
   Fixed by moving `PVZ_SCREEN_GAME` into the same case as the other nested screens
   (`PVZ_SCREEN_SETTINGS`/`PVZ_SCREEN_PLANT_SELECT`/`PVZ_SCREEN_GAME_OVER`): switch
   `s_screen = PVZ_SCREEN_MENU` and return `1u` (consumed). This is a pause, not a reset --
   `pvz_on_event()`'s `EV_GAME_TICK` handler already no-ops whenever `s_screen != PVZ_SCREEN_GAME`
   ("simulation only advances while the live game screen is showing"), so `s_state` simply stops
   advancing while parked on the PvZ menu, and the existing "Continue" menu item
   (`s_game_active`-gated) resumes exactly where the player left off -- no new state was needed,
   this was purely a one-line BACK-routing fix.
2. **"The rate of shooting from those plants are a little bit high and cause some
   inconsistency."** Found a real bug in `pvz_engine.c`'s `plants_fire()`: `c->fire_cooldown =
   PVZ_PLANT_FIRE_TICKS` was only set *inside* the "found a free bullet slot" branch of the
   `PVZ_MAX_BULLETS` (8, shared across every plant on the field) search. Whenever that shared
   pool was briefly exhausted (easy with several plants all eligible to fire in the same 10 Hz
   window), the plant's cooldown stayed at 0 and it re-attempted *every single tick* with no
   wait at all, then fired instantly the moment any bullet (from any plant) expired -- so
   cadence wasn't a steady ~1 shot per `PVZ_PLANT_FIRE_TICKS`, it was bursty: silence while the
   pool was full, then an immediate volley the instant a slot freed. This is the "inconsistency"
   the user observed. Fixed by starting the cooldown as soon as a plant is eligible to fire,
   regardless of whether a bullet slot was actually available -- every plant now has a strict,
   consistent per-shot cadence; a shot lost to a genuinely full pool is simply lost (unchanged)
   rather than turning into an instant retry. Also raised `app_config.h`'s
   `PVZ_PLANT_FIRE_TICKS` 10 -> 16 (~1.0 s -> ~1.6 s) per the "a little bit high" part of the
   report -- a plain tuning number, this file's own design choice like the other Phase 8
   constants.
3. **"Why do two zombies come at once, that's not right."** Reviewed `spawn_tick()`: it spawns
   at most one zombie per call and the ramp (§9.10a item 4) already floors the interval well
   above 0, so a genuine same-tick double-spawn isn't reachable from this file's logic as
   written. The far more likely cause is the now-reverted §9.10b regression: while `ui.c`'s
   forced-full-redraw was blocking `s_mutex` for up to ~1 s at a time, `app_task` could fall
   behind and then drain several backlogged `EV_GAME_TICK` events back-to-back once unblocked --
   faster than `ui_task`'s independent 30 ms render cadence could visibly flush each intermediate
   frame to the physical LCD -- so two spawn-timer expiries landing inside that catch-up burst
   could appear on-screen as if they happened "at once" even though they were sequential
   internally. With §9.10b's fix in place, `EV_GAME_TICK` is delivered at its real 10 Hz cadence
   and this shouldn't reproduce. As an extra pacing safety margin on top of that fix (not a
   guess at a second root cause), also raised `PVZ_DIFFICULTY_MIN` 12 -> 20 ticks (~1.2 s ->
   ~2.0 s) so even late-game the spawn floor stays comfortably above a single render tick's worth
   of slack.

**Build:** verified by code review only -- checked `pvz_on_back()`'s `PVZ_SCREEN_GAME` case
matches the same `s_screen = PVZ_SCREEN_MENU; Seg7_SetMode(SEG_OFF); pvz_ensure_bgm(); return 1u;`
pattern already proven correct for the other nested screens; checked `plants_fire()`'s moved
`fire_cooldown` assignment still only fires for cells that passed the `may_fire` gate, and that
the inner bullet-slot loop's behavior (fire if a slot is free, silently skip if not) is otherwise
unchanged; checked the `PVZ_PLANT_FIRE_TICKS`/`PVZ_DIFFICULTY_MIN` constant changes aren't
referenced anywhere that assumed the old values.

**Next hardware test:** flash and replay a PvZ game -- BACK from a live game should land on the
PvZ menu (with "Continue" resuming play), plant fire cadence should feel steady rather than
bursty, and zombie spawns should read as one-at-a-time rather than paired. Report back on all 3.

### 9.11 Phase 11 implementation notes (SMS via PC bridge)

**User confirmation before starting:** user confirmed the PC companion script is acceptable,
that SMS must reuse Contacts, that Phase 11 runs before Phases 9/10, and that the send
timeout may use `HAL_GetTick()` if it does not violate project rules (confirmed: §0.1
forbids polling hardware loops, not reading the TIM1-driven HAL tick from `app_task` --
same precedent as `Widgets_BlinkOn()`, §9.5).

**Files added:** `Core/Src/app_sms.c` (`AppSms`: recipient list with pinned "Manual number"
row + contact rows from `Storage_GetContact*`, numeric manual field, text compose via
`TextField`, sending/result screens); `host_tools/sms_bridge.py` (pyserial listener,
`requests.post` to melipayamak, fixed `FROM`/`URL` constants, replies `SMS_RESULT|...`).

**Files extended:** `events.h` (`EV_SMS_RESULT`: `a`=0 OK / 1 ERR, `c`=pointer to
cmdparse-owned ping-pong payload string); `cmdparse.c` (`SMS_RESULT|` handled *before*
the leading-`/` gate; posts `EV_SMS_RESULT` + `LOG`s the raw line so Persian `status` is
visible on the UART side); `app.h` (`extern const App AppSms`); `app_config.h`
(`SMS_SEND_TIMEOUT_MS=10000`, menu layout macros remapped to `7x1`/`4x2`/`3x3` with old
`6x1`/`3x2`/`2x3` names kept as aliases); `app_menu.c` (`MENU_ITEM_COUNT=7`, SMS after
Contact, `menu_layout_dims` updated); `cgram.h`/`cgram.c` (`CGRAM_ICON_SMS=6`, day/night
tables expanded to 7 glyphs); `Debug/Core/Src/subdir.mk` + `objects.list` (`app_sms.c`
linked).

**Design decisions (not guessed):**
1. **Timeout via HAL_GetTick.** §5.10 originally said "reuse softtimer.h"; that module
   still does not exist. Deviation (written back into §5.10 as well): AppSms stores
   `deadline = HAL_GetTick() + SMS_SEND_TIMEOUT_MS` at send time and fails in
   `sms_on_tick()` on expiry -- identical rule footing to blink's documented softtimer
   deferral.
2. **No `app_sms.h`.** Result is consumed in `AppSms.on_event` when SMS is `s_current`
   (Phone_Dispatch already forwards non-special events to the current app). Late results
   while another app is foreground are ignored by that app; AppSms logs "late SMS_RESULT
   ignored" only if it is current but not waiting.
3. **Manual number / text buffer sizes.** Phone field uses `STORAGE_CONTACT_PHONE_LEN`;
   SMS body uses `STORAGE_NOTE_BODY_LEN` -- matching §5.10's "same field type as Contact
   Phone" / "same engine as Note's body editor" wording exactly.
4. **`from` / API token.** Baked only into `sms_bridge.py` as confirmed in §5.10; MCU
   emits only `SMS_SEND|<to>|<text>` via `LOG()` (which already appends `\r\n`).
5. **Menu glyph indices.** `CGRAM_ICON_*` values are independent of menu list order; SMS
   is the 7th glyph at index 6; Music/PvZ/etc. keep their existing indices so day/night
   banks stay consistent with the constants in `cgram.h`.

**Build:** not attempted in-session (user builds/flashes). `app_sms.c` added to the
Debug `subdir.mk` / `objects.list` so a CubeIDE rebuild will compile it.

**Next hardware / PC test (Phase 11 exit criteria, section 6):** close Termite; run
`python host_tools/sms_bridge.py COMx`; `/start` the phone; open Menu -> SMS; pick a
Contact with a real phone (or Manual number); compose text; SELECT send; confirm LCD
shows "Sending..." then "Sent" (with recId) or "Send failed"; confirm bridge stdout
shows the full HTTP body (including any Persian status); confirm the handset receives
the SMS; confirm leaving mid-send and having no bridge yields "Send failed" after ~10 s
via the HAL_GetTick deadline; confirm Settings icon layout 0/1/2 still switches grids
(now 7x1 / 4x2 / 3x3) and all 7 icons are reachable.

### 9.12 Phase 9 implementation notes (PC bonuses: time / piano / song upload)

**Scope verified against** master plan §5.2 / §5.5 / §5.6 / §6 Phase 9 exit, plus the
user rule that all pyserial tools must live in the existing `host_tools/sms_bridge.py`
(single COM port — do not require a second Termite session).

**Already present before this close-out (prior agent, verified on disk):**
`SEG_TIME` + `Seg7_SetTimeInfo`, `Phone_ApplySeg7Default` + `EV_TICK_1S` RTC refresh,
Settings Segtime row live apply, `StorageSong` / `Storage_UploadSong`, buzzer
`BUZZER_PIANO` + uploaded-melody index space, serial ISR `try_piano_fastpath()` for
`/pn-`/`/pf`, menu 8th icon + `CGRAM_ICON_PIANO`, `AppPiano` extern in `app.h`.

**Completed in this close-out:**
1. **`app_piano.c`** — LCD status UI; `Buzzer_Stop()` on enter; polls
   `Buzzer_GetPianoFreq()` in `on_tick` (ISR changes freq with no event).
2. **`cmdparse.c`** — full `/piano-on`, defensive `/pn-`/`/pf`, `/songup-{name}-{count}`
   session + plain `freq,ms` note lines + `/end` → `OK stored as #k` /
   `ERR songup aborted: ...`; `/setting-segtime-*` now calls `Phone_ApplySeg7Default()`.
3. **`host_tools/sms_bridge.py`** — kept SMS; added one-shot `:time` /
   `--time-sync` (not a 1 Hz loop — user preference / normal demo use), `:piano`
   (key→`/pn-`/`/pf`), `:songup <name> <file>`; sample
   `host_tools/sample_song.txt`.
4. **Build list** — `app_piano.c` in `Debug/Core/Src/subdir.mk` + `objects.list`.

**Design decisions (not guessed):**
1. **Note-line format is `freq,ms`**, not `/pn-{freq}-{ms}`. §5.5 literally says
   "`count` lines `freq,ms` then `/end`". An earlier `app_config.h` comment claiming
   `/pn-{freq}-{ms}` for songup was wrong and was corrected.
2. **ISR fast-path stays narrow** — only `/pn-` and `/pf`. `/piano-on` and songup stay
   on the normal `EV_UART_CMD` → `Cmdparse_HandleLine` path (not latency-sensitive).
3. **Music playlist** needs no `app_music.c` change — `Buzzer_GetMelodyCount()` /
   `GetMelodyName()` already include `Storage_GetSongCount()` uploaded slots.
4. **Host tools merged into `sms_bridge.py`** per user rule (one COM session keeps SMS +
   Termite-like stdin + Phase 9 demos). Standalone `time_sync.py` / `piano.py` /
   `song_upload.py` files were **not** added as separate scripts.
5. **Song name hyphens** — count is parsed after the *last* `-` in the songup header so
   names may contain hyphens; the bridge still sanitizes `-`→`_` when uploading from
   `:songup` for simplicity.

**Build:** not attempted in-session (user builds/flashes in CubeIDE).

**Phase 9 exit-criteria tests (hardware + PC):** see the checklist delivered with the
implementation response (time sync + Segtime 7-seg; live piano ≤10 ms feel; song upload
appears in Music; SMS still works from the same bridge).

**Host web UI (post–Phase 9, user request):** `host_tools/sms_bridge.py` now launches a
FastAPI browser console (`bridge/` package) at `http://127.0.0.1:8765` with buttons for
UART commands, on-screen piano, SMS, and MP3/WAV→note translation. Uploaded song capacity
raised to **`STORAGE_SONG_MAX_NOTES = 80`** (still 2 slots; fits one 2 KB flash page).
The UI also mirrors the board's LDR day/night state: a new `/mode` verb (cmdparse.c)
replies `[LDR] state: day|night` on demand (bridge sends it on connect), and the bridge
parses all `[LDR] …day/night` log lines to live-switch the web theme (dark ↔ sunlit).
Expect a one-time flash blob wipe on first boot after that firmware change (payload size
mismatch → `storage_zero_init()`).

### 9.13 Phase 10 implementation notes (hardening + polish)

**Shipped:**
- **IWDG** — already kicked from `ui_task` / per-row LCD render (unchanged).
- **Event drop counter** — `Event_DroppedCount()` + `[EVT] queue full…` (unchanged).
- **`Serial_TxDroppedCount()`** — counts bytes silently dropped when the TX ring is full
  (no per-drop LOG; would worsen overflow).
- **`/health`** (`health.c` / `cmdparse.c`, innovation I9) — logs:
  ```
  [HEALTH] uptime=Ns drops_evt=A drops_tx=B
  [HEALTH] hwm ui=X app=Y storage=Z (words free)
  ```
  Task handles registered once after `osThreadNew` in `main.c`.
- **CGRAM audit** — screen→bank→slots matrix documented in `cgram.h` header
  (Logo 8 / 4 frames, Menu day/night 8, Music 8 = vol+EQ, PvZ 8; all ≤ 8).
- **Log format** — module tags (`[PHONE]`, `[MUSIC]`, …) already in place; a few human
  cmdparse lines tagged `[CMD]`. Protocol `OK` / `ERR unknown/invalid:` / `SMS_SEND|`
  left unchanged for the host bridge.

**`.ioc` / task stack note (intentional, do not CubeMX-regenerate just for this):**
- CubeMX `.ioc` may still list defaultTask stack as 128 words; runtime
  `defaultTask_attributes` in `main.c` uses **384 words** (UI). App (1024) and
  storage (256) tasks are created manually in USER CODE and are absent from the `.ioc`.

**Packaging:** course zip = `Core/` + `.ioc`; exclude `Debug/`, `.metadata/`, `.env`.
Filename `Name_StudentNumber_S#_T#.zip` is filled in by the student at submit time.

**Hardware check:** `/start` then `/health` → three HWM numbers + counters; normal use
must not IWDG-reset; Menu LDR day/night icons still swap.

### 9.14 Innovation bundle notes (I4, I3, I2, I12, I5, I8, I10)

Shipped in that order under `SmartPhone_STM32F303/Core/` (no `.ioc` regen, flash layout
unchanged — `StorageHighscore` still 16 B via reserved carve).

| ID | What landed |
|----|-------------|
| **I4** | `UI_CopyFrame` / `UI_DumpShot` — `KEY_EV_SHORTCUT_D` and `/shot` dump `[SHOT]`…`[/SHOT]` (CGRAM → `#`) |
| **I3** | `Buzzer_PlaySFX`: key click (skip Piano), wrong-PIN + `reply_err` ERROR, boot CLICK, app-enter CLICK (skip Music/Piano/PvZ) |
| **I2** | `Phone_IsLocked`; `LOCK_IDLE_MS` (300) queue wait; lock dirty-render; `__WFI()` after empty wait (no STOP) |
| **I12** | `BANK_LOGO` 8 glyphs = 4 frames; `Phone_Boot` swaps indices over `BOOT_LOGO_MS` |
| **I5** | `PvzState.survival_total_s` / `kills`; highscore `best_*` + `achievements` bits; game-over toast |
| **I8** | Music CGRAM slots 3–7 bar glyphs; row-2 EQ while playing |
| **I10** | `t9_dict.c` (~80 words); Note EDIT row 2 `>suggest`; hold-6 accepts |

**Hardware checks:** `/shot` + D key; clicks/chirps; lock idle no IWDG reset; boot animation;
PvZ unlock flags; Music bars; Note hold-6 accept.

---

*End of source-of-truth plan. Any deviation during implementation must be written back
into this document.*
