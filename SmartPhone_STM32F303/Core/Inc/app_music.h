/**
 * @file app_music.h
 * @brief Phase 5: real Music Player app. Source of truth: master plan
 *        section 3 ("app_music.c -- playlist (>=4 built-in + uploaded),
 *        play/pause/prev/next, live progress bar, 3-state volume icon, pot
 *        seek<->volume via LockType, background playback, auto-next,
 *        shuffle/repeat") and section 6 Phase 5 exit criterion ("every
 *        music-spec sentence demonstrably true"). Uploaded playlist entries
 *        are cmdparse.c/storage.c territory (Phase 6/7, not built yet) --
 *        deliberately out of scope here; see master plan section 9.7.
 *
 *        `AppMusic` itself is declared extern in app.h (matching every
 *        other app's existing precedent -- section 3's file tree has no
 *        per-app header). This header exists solely for
 *        Music_HandleSongEnd(), which phone.c needs to call directly (see
 *        below) and has nowhere else to be declared.
 */
#ifndef APP_MUSIC_H
#define APP_MUSIC_H

#include "events.h"

/** Background playback + auto-next (plan section 3): buzzer.c posts
 *  EV_SONG_END on natural melody end regardless of which app is currently
 *  on screen, so this cannot be routed through AppMusic's own on_event()
 *  (phone.c's Phone_Dispatch() only calls the *current* app's on_event()).
 *  phone.c calls this unconditionally for every EV_SONG_END, mirroring the
 *  existing global KEY_EV_SHORTCUT_A/_B (Vol+/-) handling already in
 *  Phone_Dispatch() -- see master plan section 9.7 for the full rationale. */
void Music_HandleSongEnd(const Event *ev);

#endif /* APP_MUSIC_H */
