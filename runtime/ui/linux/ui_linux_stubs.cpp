// SPDX-License-Identifier: MIT
// Explicit Linux stubs for Windows-only optional UI tabs/features (Stage 6F/6G):
//   - ui_saves.cpp (ShellExecuteW / CreateProcessW restart)
//   - ui_test.cpp (Win32 HWND/GDI scripted UI test)
//   - host/window_opts.cpp (Win32 window mode/monitor options)
//   - host/mods/problem_report.cpp (Win32 problem report archive)
#if !defined(__linux__)
#error "runtime/ui/linux/ui_linux_stubs.cpp is Linux-only"
#endif

#include "../ui_internal.h"
#include "hle/cheats.h"
#include "window_opts_public.h"
#include "imgui.h"

#include <cstdio>

namespace snap::ui {

void tab_saves() {
  ImGui::TextDisabled("Saves management tab is not yet ported on Linux.");
}
void saves_tab_opened() {}
bool saves_test_import(const std::string& /*fixture*/, std::string* /*name_out*/, std::string* err) {
  if (err) *err = "not supported on Linux";
  return false;
}
void saves_test_switch(const std::string& /*name*/) {}
bool saves_test_backups(const std::string& /*action*/, int /*n*/, std::string* /*what*/) { return false; }
bool hold_confirm(const char* /*label*/, float /*secs*/, const ImVec4& /*color*/) { return false; }
void restart_game() {}

void test_tick(double /*now*/) {}

}  // namespace snap::ui

extern "C" {

int snap_problem_report(const char* /*who*/, char* path_out, int path_cap, char* why_out, int why_cap) {
  if (path_out && path_cap > 0) path_out[0] = '\0';
  if (why_out && why_cap > 0) {
    std::snprintf(why_out, static_cast<size_t>(why_cap), "Problem report export is not yet supported on Linux.");
  }
  return 0;
}

int snap_window_get_mode(void) { return SNAP_WINDOW_WINDOWED; }
void snap_window_set_mode(int /*mode*/) {}
void snap_window_toggle_fullscreen(void) {}
int snap_window_monitor_count(void) { return 0; }
const char* snap_window_monitor_name(int /*index*/) { return nullptr; }
int snap_window_get_monitor(void) { return -1; }
void snap_window_set_monitor(int /*index*/) {}
int snap_window_get_pause_on_focus_loss(void) { return 0; }
void snap_window_set_pause_on_focus_loss(int /*on*/) {}
int snap_window_get_mute_in_background(void) { return 0; }
void snap_window_set_mute_in_background(int /*on*/) {}
void snap_window_set_frame_cap(int /*fps*/) {}
int snap_window_focused(void) { return 1; }
int snap_window_paused(void) { return 0; }

}  // extern "C"

namespace hle::cheats {

bool master_on() { return false; }
void set_master(bool /*on*/) {}
std::vector<Entry> entries() { return {}; }
void set_on(size_t /*index*/, bool /*on*/) {}
void reload() {}
std::string file_path() { return {}; }
std::string status() { return "Cheats are not supported on Linux yet."; }
bool active() { return false; }
void tick() {}
bool test_load(const std::string& /*text*/, std::string* error) {
  if (error) *error = "not supported on Linux";
  return false;
}
void test_hooks(uint64_t /*heap_base*/, uint64_t /*keys*/) {}
bool test_run_once() { return false; }

}  // namespace hle::cheats
