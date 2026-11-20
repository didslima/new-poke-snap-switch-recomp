// SPDX-License-Identifier: MIT
// Stubs for host/build_launcher_ui_test.py only: the pieces of the game's input and window code that the launcher's Controls page and Mods page
// pull in through hle/input_core.cpp, which the preview exe does not need to run.
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>

#include "hle/btn_icons.h"
#include "hle/freecam.h"
#include "hle/hle.h"
#include "hle/hle_config.h"
#include "hle/input_core.h"
#include "benchmark.h"

namespace nvn { int g_presentCount = 0; int g_fdBurst = 0; }

namespace hle {
void log(const char* fmt, ...) { (void)fmt; }
Config& config() { static Config c; return c; }
bool load_config(const std::string&) { return false; }
namespace icons {
void set_mapping(const input::Mapping&) {}
void set_pad(Family, bool) {}
void set_last_input(bool) {}
}  // namespace icons
namespace freecam {
bool wants_mouse() { return false; }
void poll(const input::RawInput&, input::PadSample&, input::TouchSample&, bool, uint64_t) {}
bool script_valid_token(const std::string&) { return false; }
void script_begin() {}
void script_token(const std::string&) {}
void script_end() {}
}  // namespace freecam
namespace input { std::unique_ptr<Backend> make_default_backend() { return nullptr; } }
}  // namespace hle

namespace snap_benchmark {
bool make_scratch_profile(const std::string&, std::string&, std::string&, std::string& err) { err = "not in the preview"; return false; }
}  // namespace snap_benchmark
