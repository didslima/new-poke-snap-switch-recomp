// SPDX-License-Identifier: MIT
// Shared declarations inside runtime/ui (overlay core, controls menu, scripted test).
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

#include "hle/input_core.h"
#include "imgui.h"
#include "ui_theme.h"

namespace snap::ui {

// ---- controls menu (ui_controls.cpp)
// Draws the whole menu as a full-frame ImGui layer; `size` is the output size in pixels. Called between NewFrame and Render.
void draw_menu(ImVec2 size, bool* want_close);
// Called every frame while the menu is open or about to open: re-reads the mapping the hub holds.
void menu_opened();
void menu_closed();                 // saves pending edits
void menu_tick_save();              // debounced save of live edits
// Small notice at the bottom of the screen (invalid input file, migration, save errors); true while it is on screen.
bool draw_toast(ImVec2 size, double now);
// Shiny capture effect (runtime/ui/ui_shiny.cpp): badge, sparkles; active for ~2.6 s after snap_shiny_fx_trigger.
bool shiny_fx_active();
void draw_shiny_fx(ImVec2 size);
bool shiny_badges_active();             // a flagged photo thumbnail is on screen (nvn_shiny.cpp)
void draw_shiny_badges(ImVec2 size);
// Performance overlay (runtime/ui/ui_fps.cpp): settings key fps_overlay (default Off, "fps", "detailed"), corner fps_overlay_corner.
int fps_overlay_mode();                          // 0 off, 1 fps, 2 detailed
std::string fps_overlay_value();                 // "default" (off), "fps", "detailed"
std::string fps_overlay_corner_value();          // "tl", "tr", "bl", "br"
void fps_overlay_set(const std::string& v);      // live, stores the key
void fps_overlay_set_corner(const std::string& v);
void fps_overlay_cycle();                        // F3: Off -> FPS -> Detailed -> Off
bool fps_overlay_hotkey_free();                  // F3 is not bound to a game button
bool safe_banner_on();                           // safe mode run (SNAP_SAFE_MODE=1): the one-line banner is drawn every frame (ui_fps.cpp, docs/safe-mode.md)
void draw_safe_banner(ImVec2 size);
bool start_note_on();    // launcher Play: the game window has not shown its first picture yet (ui_fps.cpp)
void start_note_done();  // the first picture arrived
void draw_start_note(ImVec2 size);
bool fps_overlay_tick();                         // once per present; true when the overlay draws this frame
void draw_fps_overlay(ImVec2 size);
// Developer loop UI (runtime/ui/ui_tune.cpp; SNAP_DEV=1 only, see runtime/nvn/nvn_dev.h): F1 "Tuning" tab, GPU cost overlay (F4), shader compile errors.
bool tune_hotkey_free();                         // F4 is not bound to a game button
void tune_toggle_overlay();
bool tune_tick();                                // once per present; true when the dev layer draws this frame
void draw_tune_overlay(ImVec2 size);
void tab_tune();
const char* menu_current_tab();           // the page shown now (scripted test: expect_tab)
const char* menu_nav_debug();
bool menu_page_cursor();                  // the pad / keyboard cursor sits on an item of the page (scripted test: expect_focus)
void menu_select_tab(const char* name);   // scripted test: Bindings, Mouse, Controller, Accessibility, Test
// Rebinding as the UI performs it (a click on a slot); used by the scripted test.
bool menu_begin_rebind(int action, hle::input::Device dev, int slot);
bool menu_reset_binds(int dev);   // test hook (reset_binds): 0 keyboard, 1 mouse, 2 controller, 3 all
bool menu_rebinding();

// ---- saves tab (ui_saves.cpp): save profiles
void tab_saves();
void tab_savestate();   // ui_savestate.cpp: Save state and Load state buttons, the one-slot note (docs/save-states.md)
bool state_note_on();    // a save state was asked for or just finished: the note is on screen
void draw_state_note(ImVec2 size);
void tab_cheats();      // ui_cheats.cpp: the cheats master switch and one checkbox per cheat (runtime/hle/cheats.h)
void tab_benchmark();   // ui_bench.cpp: starts the ride benchmark (host/benchmark.cpp)
void saves_tab_opened();
bool saves_test_import(const std::string& fixture, std::string* name_out, std::string* err);   // scripted test
void saves_test_switch(const std::string& name);
bool saves_test_backups(const std::string& action, int n, std::string* what);   // scripted test: the F1 > Saves "Restore a backup" list

// ---- game-style drawing helpers (ui_style.cpp)
void style_shadow(ImDrawList* dl, ImVec2 pos, ImVec2 size, float round);
float style_prompt(const char* glyph, const char* label);   // "A"/"B" make the orange chevron, longer text ("LB") a grey pill; stays on the line
void style_footer_prompts(bool tabs, bool back);
// Button icons setting: with Xbox or PlayStation chosen the menus draw those pictures for controller buttons (ImGui primitives).
bool style_pad_glyphs();                                                          // Xbox or PlayStation pictures are on
float style_pad_glyph_width(int pad_button, float h);                             // 0 = no glyph for this button (or Switch style)
float style_pad_glyph(ImDrawList* dl, ImVec2 pos, float h, int pad_button);       // draws at pos (top-left), returns the width

// ---- audio tab (ui_audio.cpp)
void tab_audio();
void access_rows();   // the Accessibility rows of ui_options.cpp (vision, flashing, menu size, mono audio), appended to the Accessibility tab

// ---- shader precompile UI (ui_shaders.cpp)
bool shader_screen_active(double now);   // first-launch prompt or the progress screen is wanted this frame
void draw_shader_screen(ImVec2 size);
void shader_screen_closed();
void shader_cache_section();   // the shader cache controls, shown inside the Graphics page
// shared helpers (ui_saves.cpp)
bool hold_confirm(const char* label, float secs, const ImVec4& color);   // true once when held long enough (mouse, Enter or gamepad)
void restart_game();                                                      // relaunch with the same arguments and the active profile

// ---- extras (ui_extras.cpp): gyro and rumble settings and the photo gallery
void extras_controller();   // appended to the Controller tab
void gallery_section();     // inside the Photos page

// ---- option pages (ui_options.cpp): table-driven, values in hle/settings.h
void tab_graphics();
void tab_display();
void tab_photos();
void tab_mods();
void audio_output_section();   // appended to the Audio tab
void options_menu_opened();    // remembers the start-up values (what "needs a restart" is measured against)
int options_pending_restart(std::string* names);
void options_restart_bar();    // the "N changes need a restart / Apply and restart" strip
// scripted test (ui_test.cpp)
bool options_test_set(const std::string& key, const std::string& value);
std::string options_test_get(const std::string& key);
std::string options_test_default(const std::string& key);
std::string options_note(const std::string& key);   // one-line "which value is in effect" note of a picture row, "" if none
void options_test_scroll(float fraction);   // scroll the visible option list (0 top, 1 bottom)
bool options_test_option_when(const std::string& key, std::string* when);
bool options_test_mods(const std::string& op, const std::string& id, int arg, std::string* state);

// ---- overlay core (ui_overlay.cpp)
bool overlay_open();
void overlay_set_open(bool open);
void overlay_set_modal(bool on);   // the shader prompt / progress screen: input goes to ImGui and the game reads idle
void* overlay_hwnd();

// ---- scripted test (ui_test.cpp), enabled by SNAP_UI_TEST=<file>
void test_tick(double now);
bool test_active();

}  // namespace snap::ui
