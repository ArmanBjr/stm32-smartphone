# STM32 SmartPhone

Embedded “smartphone” on an **STM32F303 Discovery** board — FreeRTOS firmware with LCD/keypad apps, music & live piano, a Plants-vs-Zombies mini-game, SMS via UART, plus a Python **web host console** for control, song upload, and day/night theming.

> Course project: Microprocessor Lab · board: STM32F3DISCOVERY (STM32F303VCT6)

---

## Features

| Area | What’s included |
|------|-----------------|
| **Apps** | Menu, Lock, Note, Contact, Music, Settings, Info, SMS, Live Piano, Plants vs Zombies |
| **Hardware** | 20×4 LCD, 4×4 keypad, buzzer (PWM), pot (volume/seek), LDR (day/night), 7-seg + LEDs, UART @ 115200 |
| **RTOS** | FreeRTOS (CMSIS-RTOS v2) — interrupt-driven drivers, LCD render task |
| **PC bridge** | FastAPI web UI: connect COM port, start/lock/reset, time sync, piano keyboard, MP3→notes song upload, SMS history, live log (SSE) |
| **Bonus** | RTC time sync, live piano (`/pn-` / `/pf`), custom song flash slots (2 × 80 notes) |
| **Hardening** | IWDG, event/TX drop counters, `/health` (uptime + stack high-water marks) |

---

## Repository layout

```text
SmartPhone_STM32F303/     STM32CubeIDE firmware (open .ioc / .project)
  Core/Src, Core/Inc      Application + drivers (phone, apps, UART, …)
  Drivers/, Middlewares/  HAL + FreeRTOS
host_tools/               PC companion (web bridge)
  sms_bridge.py           Launcher → http://127.0.0.1:8765
  bridge/                 SerialHub, FastAPI, translator, static UI
ProjectExplanation/       Master plan (source of truth) + course PDF
PinAssigment/             Pin map notes
Songs/, tools/            Melody sources + conversion helper
```

---

## Firmware (STM32CubeIDE)

1. Open `SmartPhone_STM32F303` in **STM32CubeIDE**.
2. Build the **Debug** configuration and flash the F3 Discovery over ST-Link.
3. UART is on the ST-Link VCP: **115200 8N1** (close other serial tools before the bridge).

Useful host commands the firmware understands (among others):

```text
/start                  boot / unlock into menu
/lock                   lock screen
/reset                  soft reset
/time-{unix_epoch}      one-shot RTC sync
/health                 uptime, stack HWM, event/TX drop counters
/mode                   reply [LDR] state: day|night
/piano-on /piano-off
/pn-{freq}  /pf         live note on / off (ISR fast-path)
/songup-{name}-{n} … /end
/setting-{name}-{value}
```

Full protocol and architecture: `ProjectExplanation/SmartPhone_STM32F303_MasterPlan.md`.

---

## Course submission packaging

Zip for the course usually wants **`Core/`** plus the **`.ioc`** (see PDF page 1). Suggested contents:

```text
Name_StudentNumber_S#_T#.zip
  Core/                         (Inc + Src + Startup)
  SmartPhone_STM32F303.ioc
```

Exclude: `Debug/`, `Release/`, `.metadata/`, `host_tools/.env`, Python `venv/`. Rename the zip with your real student identifiers when submitting.

---

## Host web bridge

```bash
cd host_tools
pip install -r requirements.txt
python sms_bridge.py COM3          # Windows example
# python sms_bridge.py /dev/ttyACM0
```

Opens **http://127.0.0.1:8765/** — buttons for phone control, piano, settings, song upload (`.txt` `freq,ms` or `.mp3`/`.wav`), and a live UART log. The UI theme follows the board’s LDR (day ↔ night).

Options:

```text
python sms_bridge.py COM3 --time-sync
python sms_bridge.py --no-browser
python sms_bridge.py COM3 --http-port 9000
```

### SMS credentials (optional)

SMS uses Melipayamak. Copy the example env file and fill in your values — **do not commit** `.env`:

```bash
cp host_tools/.env.example host_tools/.env
# edit MELIPAYAMAK_FROM and MELIPAYAMAK_TOKEN
```

Without these, the rest of the bridge still works; SMS sends return an error.

### Song upload

- Text: one `freq,ms` per line (see `host_tools/sample_song.txt`).
- Audio: pitch tracking → quantized notes, max **80** notes per song, **2** flash slots.
- Play uploaded tracks in the phone **Music** app (after the built-ins).

More detail: [`host_tools/README.md`](host_tools/README.md).

---

## Hardware notes

Pin map is documented in `PinAssigment/PinAssignment.md` and the master plan §1. Highlights:

- LCD 20×4 on PD8–PD14  
- Keypad on PE7–PE14  
- Buzzer TIM8_CH1 → PC6  
- LDR → PA1 (ADC1), pot → PB2 (ADC2)  
- USART1 TX/RX → PC4/PC5 (ST-Link VCP)

---

## Status

Phases **0–11** are implemented: base phone apps, PvZ, PC bonuses (time / piano / song upload), SMS bridge, browser host console, and Phase 10 hardening (`/health`, drop counters, IWDG). Optional innovation items (I1–I12 beyond I9) remain available if more bonus marks are needed.

---

## License

University lab coursework. Use / fork for learning; check your course rules before redistributing graded materials (PDF, etc.).
