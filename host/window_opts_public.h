// SPDX-License-Identifier: MIT
// OS-neutral public C API for window options (shared between Windows host/window_opts.cpp and Linux UI).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
enum { SNAP_WINDOW_WINDOWED = 0, SNAP_WINDOW_BORDERLESS = 1 };
int  snap_window_get_mode(void);
void snap_window_set_mode(int mode);
void snap_window_toggle_fullscreen(void);               // F11 and Alt+Enter call this
int  snap_window_monitor_count(void);
const char* snap_window_monitor_name(int index);        // "\\.\DISPLAY1 1920x1080" style; NULL when out of range
int  snap_window_get_monitor(void);                     // -1 = wherever the window is
void snap_window_set_monitor(int index);                // -1 or 0..count-1; moves a borderless window at once
int  snap_window_get_pause_on_focus_loss(void);
void snap_window_set_pause_on_focus_loss(int on);
int  snap_window_get_mute_in_background(void);
void snap_window_set_mute_in_background(int on);
int  snap_window_frame_cap(void);                       // fps, 0 = no extra cap (the game's own pacing); read by nvn Pace()
void snap_window_set_frame_cap(int fps);
int  snap_window_focused(void);
int  snap_window_paused(void);
#ifdef __cplusplus
}
#endif
