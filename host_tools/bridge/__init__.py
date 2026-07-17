"""SmartPhone host bridge package (web UI + serial + song translator)."""

MAX_SONG_NOTES = 80
BAUD = 115200
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8765

PIANO_KEYS = {
    "a": 262,
    "w": 277,
    "s": 294,
    "e": 311,
    "d": 330,
    "f": 349,
    "t": 370,
    "g": 392,
    "y": 415,
    "h": 440,
    "u": 466,
    "j": 494,
    "k": 523,
}
