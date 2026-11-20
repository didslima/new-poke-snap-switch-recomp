// Launcher mode of the game exe (host/launcher.cpp).
#pragma once
#include <functional>
#include <vector>
#include <filesystem>
#include <string>

namespace hle { struct Json; }

namespace snap_launcher {

struct Result {
  bool active = false;    // a config.json sits next to the exe: launcher mode
  bool proceed = false;   // start the game with the fields below; false = exit (quit, or a message was shown)
  int exit_code = 0;      // the process exit code when proceed is false (2 when the converted game files are empty or damaged)
  std::string main_elf, sdk_elf, config_path, trace_file;
  std::string notice;     // in: a one-time line for the player (the data copy of the first portable start); a message box, or the log for --play
  std::string action;     // in: one launcher action to run without the window (host/cli_flags.h action_names); exit_code is its result
  std::vector<std::string> action_args;   // in: that action's own arguments (host/cli_flags.h action_takes_args)
  bool play = false;      // in: --play (host/cli_flags.h): no launcher window, straight to the game-folder check and the start
};

// Call when the command line holds no command of its own (player flags only). Returns false when there is no config.json next to the exe (nothing happened).
bool prepare(Result& r);

// Offers the upscaler downloads (host/launcher_upscalers.cpp). first_start = only the pre-selected one (FSR). Returns the number installed.
int offer_upscalers(const std::filesystem::path& exe_dir, const hle::Json& data, bool first_start);

// --upscalers without a window. No arguments: lists each upscaler, whether it is installed and where its license is. `<name> --accept-license <name>`
// (both repeatable) downloads the named ones; a name without its --accept-license is refused, because nobody accepts a license for the player.
// Every line goes to `say`. Returns the process exit code: 0 done, 1 something was not installed, 2 the arguments are wrong.
int upscalers_cli(const std::filesystem::path& exe_dir, const hle::Json& data, const std::vector<std::string>& args,
                  const std::function<void(const std::string&)>& say);

}  // namespace snap_launcher
