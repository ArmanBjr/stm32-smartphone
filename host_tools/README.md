# SmartPhone host tools

Local PC companion for the STM32F303 SmartPhone lab project.

## Web bridge (recommended)

```bash
pip install -r host_tools/requirements.txt
python host_tools/sms_bridge.py COM3
```

Opens **http://127.0.0.1:8765/** — buttons for `/start`, lock, reset, time sync,
live piano, song upload (`.txt` notes or `.mp3`/`.wav` via pitch translator), and SMS.
The page theme follows the board's LDR: covering the sensor (night) switches the UI
to the dark palette, light (day) switches to the sunlit one; a header pill shows the
current mode. The System health panel sends `/health` and updates live with MCU
uptime, event/TX drop counters, and UI/app/storage stack high-water marks.

- Close Termite first (one COM port only).
- After uploading a song, play it in the phone **Music** app (tracks after the built-ins).
- Device holds **2 songs × 80 notes** max (`STORAGE_SONG_MAX_NOTES`).
- Flash the firmware that includes the 80-note bump before uploading long songs.
- SMS (optional): copy `.env.example` → `.env` and set `MELIPAYAMAK_FROM` / `MELIPAYAMAK_TOKEN`.

Options:

```text
python host_tools/sms_bridge.py COM3 --time-sync
python host_tools/sms_bridge.py --no-browser          # pick port in the UI
python host_tools/sms_bridge.py COM3 --http-port 9000
```

## MP3 → notes

The translator (`bridge/song_translator.py`) uses librosa `pyin` pitch tracking,
quantizes to musical frequencies in the buzzer range, merges frames, and truncates
to 80 notes. This is **lab-quality monophonic approximation**, not sheet-music
transcription. Clear single-instrument melodies work best.

## Layout

```text
host_tools/
  sms_bridge.py           launcher
  requirements.txt
  sample_song.txt
  bridge/
    serial_hub.py         COM port + songup + piano arm + SMS_SEND handler
    song_translator.py    audio/text → (freq,ms)
    sms_client.py         melipayamak HTTP
    server.py             FastAPI + SSE
    static/               HTML UI
```
