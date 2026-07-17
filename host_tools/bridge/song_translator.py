"""Translate audio files (mp3/wav) or note text into buzzer (freqHz, ms) lists."""

from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path
from typing import List, Sequence, Tuple

from . import MAX_SONG_NOTES

Note = Tuple[int, int]  # freqHz, ms

# Usable piezo/buzzer range for this project
FREQ_MIN = 130
FREQ_MAX = 1000
MIN_NOTE_MS = 60
FRAME_MS = 50  # analysis hop target


@dataclass
class TranslateResult:
    notes: List[Note]
    note_count: int
    duration_ms: int
    truncated: bool
    source_notes: int
    message: str


def parse_note_text(text: str, max_notes: int = MAX_SONG_NOTES) -> TranslateResult:
    notes: List[Note] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(",")
        if len(parts) != 2:
            raise ValueError(f"bad line (want freq,ms): {line!r}")
        freq = int(parts[0].strip())
        ms = int(parts[1].strip())
        if freq < 0 or freq > 65535 or ms <= 0 or ms > 65535:
            raise ValueError(f"out of range: {line!r}")
        notes.append((freq, ms))
    return _cap_notes(notes, max_notes, "text")


def audio_to_notes(
    path: str | Path,
    max_notes: int = MAX_SONG_NOTES,
) -> TranslateResult:
    """Lab-quality monophonic pitch track → quantized note list."""
    try:
        import librosa
        import numpy as np
    except ImportError as exc:  # pragma: no cover
        raise ImportError(
            "Audio translation needs librosa + numpy. "
            "pip install -r host_tools/requirements.txt"
        ) from exc

    path = Path(path)
    y, sr = librosa.load(str(path), sr=22050, mono=True)
    if y.size == 0:
        raise ValueError("empty audio")

    hop = int(sr * FRAME_MS / 1000)
    hop = max(hop, 256)
    frame_ms = int(round(1000.0 * hop / sr))

    fmin = FREQ_MIN
    fmax = min(FREQ_MAX, sr // 2 - 1)
    try:
        f0, voiced_flag, _ = librosa.pyin(
            y,
            fmin=float(fmin),
            fmax=float(fmax),
            sr=sr,
            hop_length=hop,
        )
    except Exception:
        # Fallback: piptrack peak
        pitches, mags = librosa.piptrack(y=y, sr=sr, hop_length=hop, fmin=fmin, fmax=fmax)
        n_frames = pitches.shape[1]
        f0 = np.full(n_frames, np.nan, dtype=float)
        voiced_flag = np.zeros(n_frames, dtype=bool)
        for i in range(n_frames):
            idx = mags[:, i].argmax()
            p = pitches[idx, i]
            if p > 0:
                f0[i] = float(p)
                voiced_flag[i] = True

    raw: List[Note] = []
    cur_freq = 0
    cur_ms = 0

    def flush() -> None:
        nonlocal cur_freq, cur_ms
        if cur_ms <= 0:
            cur_freq = 0
            cur_ms = 0
            return
        if cur_ms < MIN_NOTE_MS and cur_freq != 0:
            # absorb short blips into silence or previous
            if raw and raw[-1][0] != 0:
                pf, pm = raw[-1]
                raw[-1] = (pf, pm + cur_ms)
            else:
                raw.append((0, cur_ms))
        else:
            raw.append((cur_freq, cur_ms))
        cur_freq = 0
        cur_ms = 0

    for i, hz in enumerate(f0):
        voiced = bool(voiced_flag[i]) if voiced_flag is not None else not (
            hz != hz  # NaN
        )
        if not voiced or hz != hz or hz <= 0:
            q = 0
        else:
            q = _quantize_hz(float(hz))
        if q == cur_freq:
            cur_ms += frame_ms
        else:
            flush()
            cur_freq = q
            cur_ms = frame_ms
    flush()

    # Merge adjacent identical pitches (after blip absorb)
    merged: List[Note] = []
    for freq, ms in raw:
        if merged and merged[-1][0] == freq:
            merged[-1] = (freq, merged[-1][1] + ms)
        else:
            merged.append((freq, ms))

    # Drop leading/trailing pure rests if everything else exists
    while merged and merged[0][0] == 0:
        merged.pop(0)
    while merged and merged[-1][0] == 0:
        merged.pop()

    if not merged:
        raise ValueError("no pitched notes detected (try a clearer melody / wav)")

    return _cap_notes(merged, max_notes, path.name)


def _quantize_hz(hz: float) -> int:
    """Nearest MIDI note, clamped to buzzer range."""
    midi = int(round(69 + 12 * math.log2(hz / 440.0)))
    midi = max(48, min(84, midi))  # C3..C6 approx
    q = int(round(440.0 * (2.0 ** ((midi - 69) / 12.0))))
    return max(FREQ_MIN, min(FREQ_MAX, q))


def _cap_notes(
    notes: Sequence[Note],
    max_notes: int,
    label: str,
) -> TranslateResult:
    source = len(notes)
    truncated = source > max_notes
    if truncated:
        # Prefer keeping the start of the melody (intro) for demos
        kept = list(notes[:max_notes])
        msg = f"truncated {source} → {max_notes} notes ({label})"
    else:
        kept = list(notes)
        msg = f"{source} notes from {label}"
    dur = sum(ms for _, ms in kept)
    return TranslateResult(
        notes=kept,
        note_count=len(kept),
        duration_ms=dur,
        truncated=truncated,
        source_notes=source,
        message=msg,
    )
