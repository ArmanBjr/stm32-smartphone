"""Convert Songs/*.ino (HiBit Arduino format) to buzzer.c Note arrays."""
import re
import os
import glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PITCHES_H = os.path.join(ROOT, "Songs", "Pitches", "pitches.h")
SONGS_DIR = os.path.join(ROOT, "Songs")
OUT_SONGS = os.path.join(ROOT, "SmartPhone_STM32F303", "Core", "Src", "buzzer_new_songs.inc")
OUT_ENTRIES = os.path.join(ROOT, "SmartPhone_STM32F303", "Core", "Src", "buzzer_new_entries.inc")

PITCHES = {}
with open(PITCHES_H, encoding="utf-8") as f:
    for line in f:
        m = re.match(r"#define\s+(NOTE_\w+|REST)\s+(\d+)", line.strip())
        if m:
            PITCHES[m.group(1)] = int(m.group(2))

FREQ_TO_NAME = {}
for name, val in PITCHES.items():
    if name != "REST" and val not in FREQ_TO_NAME:
        FREQ_TO_NAME[val] = name

SONG_META = {
    "game_of_thrones.ino": ("s_melody_got", "GoT"),
    "harry_potter.ino": ("s_melody_harry", "Harry P."),
    "imagine_dragons_bones.ino": ("s_melody_bones", "Bones"),
    "pink_panther.ino": ("s_melody_pink", "Pink Pan."),
    "pirates_of_the_caribbean.ino": ("s_melody_pirates", "Pirates"),
    "shape_of_you.ino": ("s_melody_shape", "Shape of U"),
    "star_wars.ino": ("s_melody_starwars", "Star Wars"),
    "still_dre.ino": ("s_melody_dre", "Still Dre"),
    "the_godfather.ino": ("s_melody_godfather", "Godfather"),
}

ORDER = [
    "game_of_thrones.ino",
    "harry_potter.ino",
    "imagine_dragons_bones.ino",
    "pink_panther.ino",
    "pirates_of_the_caribbean.ino",
    "shape_of_you.ino",
    "star_wars.ino",
    "still_dre.ino",
    "the_godfather.ino",
]



# Tempo scale: none of these particular .ino sketches define their own
# TEMPO/wholenote constant (confirmed by grep -- they just list raw
# duration divisors), so there's no per-song source tempo to recover; one
# fixed whole-note length across all songs is the only option. 1.8x
# (previous value) came back reported as too slow overall; 1.45x is the
# adjustment -- still slower than the original too-fast 1.0x baseline, but
# not as heavy-handed as 1.8x. Same reasoning as before: a single global
# scale, not a per-song guess.
TEMPO_SCALE = 1.45
WHOLE_NOTE_MS = round(1300 * TEMPO_SCALE)


def ms_from_dur(d):
    return (WHOLE_NOTE_MS + d // 2) // d


def fmt_note(freq, ms):
    if freq == 0:
        return f"{{ NOTE_REST, {ms:3d} }}"
    name = FREQ_TO_NAME.get(freq)
    if name:
        return f"{{ {name}, {ms:3d} }}"
    return f"{{ {freq:4d}, {ms:3d} }}"


def parse_song(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    text = text.replace("NOTE_FS4|", "NOTE_FS4")
    mel_m = re.search(r"int\s+melody\[\]\s*=\s*\{(.*?)\};", text, re.S)
    dur_m = re.search(r"int\s+durations\[\]\s*=\s*\{(.*?)\};", text, re.S)
    melody = []
    for tok in re.findall(r"[A-Za-z0-9_]+", mel_m.group(1)):
        if tok in PITCHES:
            melody.append(PITCHES[tok])
    durations = [int(x) for x in re.findall(r"\d+", dur_m.group(1))]
    if len(melody) != len(durations):
        raise ValueError(f"{path}: melody {len(melody)} != durations {len(durations)}")
    return [(m, ms_from_dur(d)) for m, d in zip(melody, durations)]


def main():
    all_arrays = []
    melody_entries = []
    for fname in ORDER:
        path = os.path.join(SONGS_DIR, fname)
        var, disp = SONG_META[fname]
        notes = parse_song(path)
        lines = []
        for i in range(0, len(notes), 4):
            chunk = notes[i : i + 4]
            lines.append("  " + ", ".join(fmt_note(f, ms) for f, ms in chunk) + ",")
        arr = f"/* {disp} */\nstatic const Note {var}[] = {{\n" + "\n".join(lines) + "\n};"
        all_arrays.append(arr)
        melody_entries.append(
            f'  {{ {var}, sizeof({var}) / sizeof(Note), "{disp}" }},'
        )

    with open(OUT_SONGS, "w", encoding="utf-8") as f:
        f.write("\n\n".join(all_arrays))
    with open(OUT_ENTRIES, "w", encoding="utf-8") as f:
        f.write("\n".join(melody_entries))
    print(f"Wrote {len(ORDER)} songs to {OUT_SONGS}")


if __name__ == "__main__":
    main()
