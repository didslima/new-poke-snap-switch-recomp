// SPDX-License-Identifier: MIT
// SDL3 + Dear ImGui graphical launcher for double-click AppImage startup.
#pragma once

#include <string>

namespace snap_linux {

// Opens the graphical launcher window, allows configuring required files/ROMFS/video,
// and launches the game when Play is pressed. Returns process exit code.
int run_launcher_gui(const std::string &argv0, const std::string &initial_error = "");

} // namespace snap_linux
