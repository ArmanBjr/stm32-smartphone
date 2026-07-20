"""FastAPI web UI for the SmartPhone serial bridge."""

from __future__ import annotations

import asyncio
import tempfile
from pathlib import Path
from typing import Any, Dict, List, Optional

from fastapi import FastAPI, File, Form, HTTPException, Request, UploadFile
from fastapi.responses import FileResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from . import DEFAULT_HOST, DEFAULT_PORT, MAX_SONG_NOTES, PIANO_KEYS
from .serial_hub import JobCancelled, SerialHub
from .song_translator import audio_to_notes, parse_note_text

STATIC_DIR = Path(__file__).resolve().parent / "static"

hub = SerialHub()
app = FastAPI(title="SmartPhone Bridge", version="1.0")


class ConnectBody(BaseModel):
    port: str
    baud: int = 115200


class SendBody(BaseModel):
    line: str


class NoteBody(BaseModel):
    freq: int = Field(ge=0, le=65535)


class SongNotesBody(BaseModel):
    name: str = "Song"
    notes: List[List[int]]  # [[freq, ms], ...]


def _upload_exc(exc: Exception) -> HTTPException:
    if isinstance(exc, JobCancelled):
        return HTTPException(409, "cancelled")
    return HTTPException(400, str(exc))


@app.get("/")
async def index() -> FileResponse:
    return FileResponse(STATIC_DIR / "index.html")


app.mount("/static", StaticFiles(directory=str(STATIC_DIR)), name="static")


@app.get("/api/status")
async def status() -> Dict[str, Any]:
    return {
        "connected": hub.connected,
        "port": hub.port,
        "piano_armed": hub.piano_armed,
        "day_night": hub.day_night,
        "health": hub.health,
        "max_song_notes": MAX_SONG_NOTES,
        "piano_keys": PIANO_KEYS,
        "ports": hub.list_ports(),
    }


@app.get("/api/ports")
async def ports() -> Dict[str, List[str]]:
    return {"ports": hub.list_ports()}


@app.post("/api/connect")
async def connect(body: ConnectBody) -> Dict[str, Any]:
    try:
        hub.connect(body.port, body.baud)
    except Exception as exc:
        raise HTTPException(400, str(exc)) from exc
    return await status()


@app.post("/api/disconnect")
async def disconnect() -> Dict[str, Any]:
    hub.disconnect()
    return await status()


@app.post("/api/send")
async def send(body: SendBody) -> Dict[str, str]:
    try:
        hub.send_line(body.line)
    except Exception as exc:
        raise HTTPException(400, str(exc)) from exc
    return {"ok": "1"}


@app.post("/api/phone/start")
async def phone_start() -> Dict[str, str]:
    hub.send_line("/start")
    return {"ok": "1"}


@app.post("/api/phone/lock")
async def phone_lock() -> Dict[str, str]:
    hub.send_line("/lock")
    return {"ok": "1"}


@app.post("/api/phone/reset")
async def phone_reset() -> Dict[str, str]:
    hub.send_line("/reset")
    return {"ok": "1"}


@app.post("/api/phone/time")
async def phone_time() -> Dict[str, str]:
    hub.push_time_once()
    return {"ok": "1"}


@app.post("/api/phone/health")
async def phone_health() -> Dict[str, str]:
    try:
        hub.query_line("/health")
    except Exception as exc:
        raise HTTPException(409, str(exc)) from exc
    return {"ok": "1"}


@app.post("/api/phone/shot")
async def phone_shot() -> Dict[str, str]:
    hub.send_line("/shot")
    return {"ok": "1"}


@app.post("/api/piano/on")
async def piano_on() -> Dict[str, str]:
    hub.piano_on()
    return {"ok": "1"}


@app.post("/api/piano/off")
async def piano_off() -> Dict[str, str]:
    hub.piano_off()
    return {"ok": "1"}


@app.post("/api/piano/note-on")
async def piano_note_on(body: NoteBody) -> Dict[str, str]:
    try:
        hub.note_on(body.freq)
    except Exception as exc:
        raise HTTPException(400, str(exc)) from exc
    return {"ok": "1"}


@app.post("/api/piano/note-off")
async def piano_note_off() -> Dict[str, str]:
    hub.note_off()
    return {"ok": "1"}


@app.post("/api/cancel")
async def cancel_job() -> Dict[str, str]:
    hub.cancel_busy_work()
    return {"ok": "1"}


@app.post("/api/song/upload-notes")
async def song_upload_notes(body: SongNotesBody) -> Dict[str, Any]:
    notes = [(int(a[0]), int(a[1])) for a in body.notes if len(a) >= 2]
    try:
        n = await asyncio.to_thread(hub.song_upload, body.name, notes)
    except Exception as exc:
        raise _upload_exc(exc) from exc
    return {"uploaded": n, "name": body.name}


@app.post("/api/song/translate")
async def song_translate(
    file: UploadFile = File(...),
    name: str = Form("Song"),
) -> Dict[str, Any]:
    hub.cancel_busy_work()
    suffix = Path(file.filename or "song.bin").suffix.lower()
    raw = await file.read()
    if not raw:
        raise HTTPException(400, "empty file")

    with tempfile.NamedTemporaryFile(delete=False, suffix=suffix or ".bin") as tmp:
        tmp.write(raw)
        tmp_path = tmp.name

    try:
        if suffix in (".txt", ".csv"):
            text = raw.decode("utf-8", errors="replace")
            result = parse_note_text(text)
        elif suffix in (".mp3", ".wav", ".ogg", ".flac", ".m4a"):
            result = await asyncio.to_thread(audio_to_notes, tmp_path)
        else:
            try:
                result = await asyncio.to_thread(audio_to_notes, tmp_path)
            except Exception:
                text = raw.decode("utf-8", errors="replace")
                result = parse_note_text(text)
    except Exception as exc:
        raise HTTPException(400, str(exc)) from exc
    finally:
        try:
            Path(tmp_path).unlink(missing_ok=True)
        except Exception:
            pass

    return {
        "name": (name or "Song")[:15],
        "notes": [[f, m] for f, m in result.notes],
        "note_count": result.note_count,
        "duration_ms": result.duration_ms,
        "truncated": result.truncated,
        "source_notes": result.source_notes,
        "message": result.message,
        "max_notes": MAX_SONG_NOTES,
    }


@app.post("/api/song/translate-and-upload")
async def song_translate_and_upload(
    file: UploadFile = File(...),
    name: str = Form("Song"),
) -> Dict[str, Any]:
    translated = await song_translate(file=file, name=name)
    notes = [(int(a[0]), int(a[1])) for a in translated["notes"]]
    try:
        n = await asyncio.to_thread(hub.song_upload, translated["name"], notes)
    except Exception as exc:
        raise _upload_exc(exc) from exc
    translated["uploaded"] = n
    return translated


@app.get("/api/sms/history")
async def sms_history() -> Dict[str, Any]:
    return {"items": hub.sms_history()}


@app.get("/api/logs")
async def logs(n: int = 100) -> Dict[str, List[str]]:
    return {"lines": hub.recent_logs(n)}


@app.get("/api/events")
async def events(request: Request) -> StreamingResponse:
    queue: asyncio.Queue[str] = asyncio.Queue(maxsize=100)
    loop = asyncio.get_running_loop()

    def _enqueue(line: str) -> None:
        try:
            queue.put_nowait(line)
        except asyncio.QueueFull:
            try:
                queue.get_nowait()  # drop oldest
            except asyncio.QueueEmpty:
                pass
            try:
                queue.put_nowait(line)
            except asyncio.QueueFull:
                pass

    def on_line(line: str) -> None:
        try:
            loop.call_soon_threadsafe(_enqueue, line)
        except Exception:
            pass

    hub.add_listener(on_line)

    async def gen():
        try:
            for line in hub.recent_logs(30):
                yield f"data: {line}\n\n"
            while True:
                if await request.is_disconnected():
                    break
                try:
                    line = await asyncio.wait_for(queue.get(), timeout=15.0)
                    yield f"data: {line}\n\n"
                except asyncio.TimeoutError:
                    yield ": keepalive\n\n"
        finally:
            hub.remove_listener(on_line)

    return StreamingResponse(gen(), media_type="text/event-stream")


def run_server(
    host: str = DEFAULT_HOST,
    port: int = DEFAULT_PORT,
    open_browser: bool = True,
    connect_port: Optional[str] = None,
    time_sync: bool = False,
) -> None:
    import threading
    import time
    import webbrowser

    import uvicorn

    if connect_port:
        def _boot() -> None:
            time.sleep(0.4)
            try:
                hub.connect(connect_port)
                if time_sync:
                    hub.push_time_once()
            except Exception as exc:
                hub.log(f"[hub] auto-connect failed: {exc}")

        threading.Thread(target=_boot, daemon=True).start()

    if open_browser:
        def _open() -> None:
            time.sleep(0.8)
            webbrowser.open(f"http://{host}:{port}/")

        threading.Thread(target=_open, daemon=True).start()

    print(f"[bridge] web UI → http://{host}:{port}/", flush=True)
    uvicorn.run(app, host=host, port=port, log_level="warning")
