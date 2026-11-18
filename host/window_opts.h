// SPDX-License-Identifier: MIT
// Window options for the game window (borderless fullscreen, monitor, remembered size and position, pause and mute on focus
// loss, frame cap). Settings: %APPDATA%\SnapRecomp\window.json (SNAP_CONFIG_DIR moves the folder) plus SNAP_WINDOW_* env overrides.
// Documented in host/README.md ("Window options"). boot.cpp calls only snap_window_initial / attach / proc / on_present.
#pragma once
#ifdef _WIN32
#include <windows.h>
// boot.cpp call sites. initial: client size, style and outer rect for CreateWindowW. attach: after creation. proc: first thing in
// the window procedure (returns true when it handled the message; *res is the result). on_present: from the NVN present hook
// (blocks the game thread while the window is paused).
void snap_window_initial(DWORD* style, RECT* outer);
void snap_window_attach(HWND h);
bool snap_window_proc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT* res);
void snap_window_on_present();
#endif

// Public C API (callable from any thread; changes are applied on the window thread and saved to window.json).
#include "window_opts_public.h"
