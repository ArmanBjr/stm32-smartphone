"""SerialHub: owns the COM port, RX log fan-out, songup, piano state, SMS."""

from __future__ import annotations

import collections
import threading
import time
from typing import Callable, Deque, Dict, Iterable, List, Optional, Tuple

try:
    import serial
    from serial.tools import list_ports
except ImportError as exc:  # pragma: no cover
    raise ImportError("pip install pyserial") from exc

from . import BAUD, MAX_SONG_NOTES
from .sms_client import send_sms

LogListener = Callable[[str], None]
Note = Tuple[int, int]  # freqHz, ms


class JobCancelled(Exception):
    """Raised when a newer UI action cancels an in-flight song upload."""


class SerialHub:
    def __init__(self) -> None:
        self._ser: Optional[serial.Serial] = None
        self._lock = threading.RLock()
        self._rx_stop = threading.Event()
        self._rx_thread: Optional[threading.Thread] = None
        self._piano_armed = threading.Event()
        self._listeners: List[LogListener] = []
        self._log: Deque[str] = collections.deque(maxlen=400)
        self._port_name = ""
        self._baud = BAUD

        # One long job at a time (song upload). New work cancels the old one.
        self._job_lock = threading.Lock()
        self._job_cancel = threading.Event()

        self._sms_history: Deque[Dict[str, str]] = collections.deque(maxlen=100)

        # LDR day/night as reported by MCU [LDR] log lines; unknown until
        # the first sample after /start.
        self._day_night = "unknown"

    # ---- connection -------------------------------------------------

    @staticmethod
    def list_ports() -> List[str]:
        return [p.device for p in list_ports.comports()]

    @property
    def connected(self) -> bool:
        return self._ser is not None and self._ser.is_open

    @property
    def port(self) -> str:
        return self._port_name

    @property
    def piano_armed(self) -> bool:
        return self._piano_armed.is_set()

    @property
    def day_night(self) -> str:
        """'day' | 'night' | 'unknown' — from MCU [LDR] log lines."""
        return self._day_night

    def connect(self, port: str, baud: int = BAUD) -> None:
        self.cancel_busy_work()
        self.disconnect()
        ser = serial.Serial(port, baud, timeout=0.2)
        with self._lock:
            self._ser = ser
            self._port_name = port
            self._baud = baud
            self._rx_stop.clear()
            self._rx_thread = threading.Thread(
                target=self._rx_loop, name="serial-rx", daemon=True
            )
            self._rx_thread.start()
        self._emit(f"[hub] connected {port} @ {baud}")
        # Bootstrap the day/night theme: analog.c only logs [LDR] on boot
        # and on crossings, so ask explicitly (MCU replies `[LDR] state: …`).
        try:
            self._write_line("/mode", quiet=True)
        except Exception:
            pass

    def disconnect(self) -> None:
        self.cancel_busy_work()
        self._rx_stop.set()
        self._piano_armed.clear()
        self._day_night = "unknown"
        with self._lock:
            t = self._rx_thread
            ser = self._ser
            self._rx_thread = None
            self._ser = None
            self._port_name = ""
        if t is not None:
            t.join(timeout=1.0)
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass
            self._emit("[hub] disconnected")

    def add_listener(self, fn: LogListener) -> None:
        with self._lock:
            self._listeners.append(fn)

    def remove_listener(self, fn: LogListener) -> None:
        with self._lock:
            if fn in self._listeners:
                self._listeners.remove(fn)

    def recent_logs(self, n: int = 100) -> List[str]:
        with self._lock:
            return list(self._log)[-n:]

    def sms_history(self) -> List[Dict[str, str]]:
        with self._lock:
            return list(self._sms_history)

    def log(self, line: str) -> None:
        self._emit(line)

    def _emit(self, line: str) -> None:
        with self._lock:
            self._log.append(line)
            listeners = list(self._listeners)
        for fn in listeners:
            try:
                fn(line)
            except Exception:
                pass

    def cancel_busy_work(self) -> None:
        """Signal any in-flight song upload to stop immediately."""
        self._job_cancel.set()

    # ---- TX ---------------------------------------------------------

    def send_line(self, line: str, *, quiet: bool = False) -> None:
        """Cancel any song upload first (new UI action wins), then TX one line."""
        self.cancel_busy_work()
        self._write_line(line, quiet=quiet)

    def _write_line(self, line: str, *, quiet: bool = False) -> None:
        payload = (line.rstrip("\r\n") + "\n").encode("utf-8", errors="replace")
        with self._lock:
            if self._ser is None or not self._ser.is_open:
                raise RuntimeError("not connected")
            self._ser.write(payload)
            self._ser.flush()
        if quiet:
            with self._lock:
                self._log.append(f"!! {line}")
        else:
            self._emit(f"!! {line}")

    def push_time_once(self) -> None:
        epoch = int(time.time())
        self.send_line(f"/time-{epoch}")

    def piano_on(self) -> None:
        self.send_line("/piano-on")

    def piano_off(self) -> None:
        self.cancel_busy_work()
        try:
            self._write_line("/pf")
        except Exception:
            pass
        self._write_line("/piano-off")

    def note_on(self, freq_hz: int) -> None:
        if freq_hz < 0 or freq_hz > 65535:
            raise ValueError("freq out of range")
        # Piano notes must be low-latency: do not cancel via send_line's
        # cancel_busy_work in a way that fights itself — still cancel upload.
        self.cancel_busy_work()
        self._write_line(f"/pn-{int(freq_hz)}", quiet=True)

    def note_off(self) -> None:
        self.cancel_busy_work()
        self._write_line("/pf", quiet=True)

    def song_upload(self, name: str, notes: Iterable[Note]) -> int:
        """Exclusive upload. A newer cancel_busy_work() / song_upload aborts this."""
        note_list = list(notes)
        if not note_list:
            raise ValueError("empty song")
        if len(note_list) > MAX_SONG_NOTES:
            note_list = note_list[:MAX_SONG_NOTES]
        safe_name = name.replace("-", "_").strip()[:15] or "Song"
        for freq, ms in note_list:
            if freq < 0 or freq > 65535 or ms <= 0 or ms > 65535:
                raise ValueError(f"bad note {freq},{ms}")

        # Stop any previous upload, then take the exclusive job lock.
        self._job_cancel.set()
        with self._job_lock:
            self._job_cancel.clear()
            # Clear a half-open MCU songup session (incomplete /end aborts it).
            try:
                self._write_line("/end", quiet=True)
                time.sleep(0.03)
            except Exception:
                pass

            if self._job_cancel.is_set():
                raise JobCancelled("cancelled before start")

            self._emit(f"[hub] song upload start '{safe_name}' ({len(note_list)} notes)")
            self._write_line(f"/songup-{safe_name}-{len(note_list)}")
            time.sleep(0.05)  # let MCU enter songup session before note lines

            for i, (freq, ms) in enumerate(note_list):
                if self._job_cancel.is_set():
                    try:
                        self._write_line("/end", quiet=True)
                    except Exception:
                        pass
                    self._emit("[hub] song upload cancelled")
                    raise JobCancelled("cancelled")
                # quiet: avoid flooding SSE with 80 note lines
                self._write_line(f"{freq},{ms}", quiet=True)
                if (i + 1) % 10 == 0 or i + 1 == len(note_list):
                    self._emit(f"[hub] song upload {i + 1}/{len(note_list)}")
                time.sleep(0.015)

            if self._job_cancel.is_set():
                try:
                    self._write_line("/end", quiet=True)
                except Exception:
                    pass
                self._emit("[hub] song upload cancelled")
                raise JobCancelled("cancelled")

            self._write_line("/end")
            self._emit(f"[hub] song upload done '{safe_name}'")
            return len(note_list)

    # ---- RX ---------------------------------------------------------

    def _rx_loop(self) -> None:
        buf = bytearray()
        while not self._rx_stop.is_set():
            with self._lock:
                ser = self._ser
            if ser is None or not ser.is_open:
                break
            try:
                chunk = ser.read(256)
            except Exception as exc:
                self._emit(f"[hub] RX error: {exc}")
                break
            if not chunk:
                continue
            buf.extend(chunk)
            while True:
                nl = -1
                for i, b in enumerate(buf):
                    if b in (10, 13):
                        nl = i
                        break
                if nl < 0:
                    break
                raw = bytes(buf[:nl])
                drop = nl + 1
                if drop < len(buf) and {buf[nl], buf[drop]} == {10, 13}:
                    drop += 1
                del buf[:drop]
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip("\r\n")
                self._handle_mcu_line(line)

    def _handle_mcu_line(self, line: str) -> None:
        # During song upload, MCU ACKs are useful; note-echo ERRs are noisy —
        # still show them but listeners already drop when SSE is full.
        self._emit(f">> {line}")
        if "[LDR]" in line:
            # analog.c: "[LDR] initial state: day|night", "[LDR] day->night",
            # "[LDR] night->day" — the mode is always the trailing word.
            if line.rstrip().endswith("night"):
                self._set_day_night("night")
            elif line.rstrip().endswith("day"):
                self._set_day_night("day")
        if "[PIANO] enter" in line:
            if not self._piano_armed.is_set():
                self._piano_armed.set()
                self._emit("[piano] ARMED")
        elif "[PIANO] exit" in line:
            if self._piano_armed.is_set():
                self._piano_armed.clear()
                self._emit("[piano] DISARMED")
        if line.startswith("SMS_SEND|"):
            self._handle_sms_send(line)
        elif line.startswith("SMS_RESULT|"):
            self._record_sms_result(line)

    def _set_day_night(self, mode: str) -> None:
        if mode != self._day_night:
            self._day_night = mode
            self._emit(f"[mode] {mode}")

    def _record_sms(self, entry: Dict[str, str]) -> None:
        with self._lock:
            self._sms_history.appendleft(entry)

    def _record_sms_result(self, line: str) -> None:
        # Rare: if something else logs a result; keep history complete.
        parts = line.split("|", 2)
        status = parts[1] if len(parts) > 1 else "?"
        detail = parts[2] if len(parts) > 2 else ""
        self._record_sms(
            {
                "time": time.strftime("%H:%M:%S"),
                "direction": "result",
                "to": "",
                "text": detail,
                "status": status,
            }
        )

    def _handle_sms_send(self, line: str) -> None:
        parts = line.split("|", 2)
        if len(parts) < 3:
            self._reply_sms("SMS_RESULT|ERR|malformed SMS_SEND")
            return
        to = parts[1].strip()
        text = parts[2]
        self._record_sms(
            {
                "time": time.strftime("%H:%M:%S"),
                "direction": "send",
                "to": to,
                "text": text,
                "status": "pending",
            }
        )
        self._emit(f"[HTTP] POST to={to!r} text={text!r}")
        status, payload = send_sms(to, text)
        self._emit(f"[HTTP] result={status}|{payload}")
        self._record_sms(
            {
                "time": time.strftime("%H:%M:%S"),
                "direction": "result",
                "to": to,
                "text": text,
                "status": f"{status}|{payload}",
            }
        )
        self._reply_sms(f"SMS_RESULT|{status}|{payload}")

    def _reply_sms(self, line: str) -> None:
        try:
            self._write_line(line, quiet=True)
            self._emit(f"<< {line}")
        except Exception as exc:
            self._emit(f"[hub] SMS reply failed: {exc}")
