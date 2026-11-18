// SPDX-License-Identifier: MIT
// Explicit Linux stubs for Windows-only optional UI tabs/features (Stage 6F):
//   - ui_options.cpp (ShellExecuteW / Windows-only launcher options)
//   - ui_saves.cpp (ShellExecuteW / CreateProcessW restart)
//   - ui_extras.cpp (Winsock2 / ShellExecuteW)
//   - ui_bench.cpp (shlobj.h benchmark UI)
//   - ui_shiny.cpp (mmsystem PlaySoundW shiny chime/badges)
//   - ui_test.cpp (Win32 HWND/GDI scripted UI test)
#if !defined(__linux__)
#error "runtime/ui/linux/ui_linux_stubs.cpp is Linux-only"
#endif

#include "../ui_internal.h"
#include "hle/cheats.h"
#include "imgui.h"

namespace snap::ui {

void tab_graphics() {
  ImGui::TextDisabled("Graphics options tab is not yet ported on Linux.");
}
void tab_display() {
  ImGui::TextDisabled("Display options tab is not yet ported on Linux.");
}
void tab_photos() {
  ImGui::TextDisabled("Photos options tab is not yet ported on Linux.");
}
void tab_mods() {
  ImGui::TextDisabled("Mods options tab is not yet ported on Linux.");
}
void audio_output_section() {}
void access_rows() {}
void options_menu_opened() {}
int options_pending_restart(std::string* /*names*/) { return 0; }
void options_restart_bar() {}
bool options_test_set(const std::string& /*key*/, const std::string& /*value*/) { return false; }
std::string options_test_get(const std::string& /*key*/) { return {}; }
std::string options_note(const std::string& /*key*/) { return {}; }
void options_test_scroll(float /*fraction*/) {}
bool options_test_option_when(const std::string& /*key*/, std::string* /*when*/) { return false; }
bool options_test_mods(const std::string& /*op*/, const std::string& /*id*/, int /*arg*/, std::string* /*state*/) {
  return false;
}

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

void extras_controller() {}
void gallery_section() {}

void tab_benchmark() {
  ImGui::TextDisabled("Benchmark tab is not yet ported on Linux.");
}

bool shiny_fx_active() { return false; }
void draw_shiny_fx(ImVec2 /*size*/) {}
bool shiny_badges_active() { return false; }
void draw_shiny_badges(ImVec2 /*size*/) {}

void test_tick(double /*now*/) {}

}  // namespace snap::ui

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
