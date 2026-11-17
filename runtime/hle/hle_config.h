// SPDX-License-Identifier: MIT
// HLE configuration: paths, user identity, language, operation mode. Loaded from a JSON file
// (default %APPDATA%\SnapRecomp\config\hle.json; env SNAP_HLE_CONFIG overrides the path) plus env overrides.
// Every key is optional; defaults are listed in docs/hle-services.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hle {

struct Config {
  // ---- file system
  std::string romfs_root = "extracted/romfs-v262144";  // env SNAP_ROMFS overrides. Read only.
  std::vector<std::string> rom_layers;                 // mod overlay directories, first match wins, before romfs_root
  std::string data_root;                               // default %APPDATA%\SnapRecomp
  std::string save_root;                               // default <data_root>\save
  int save_backups = 5;                                // generations kept per user
  int save_backup_keep = 10;                           // whole-save backups kept (runtime/hle/save_backup.h); 0 = off; hle.json "save_backup_keep", env SNAP_SAVE_BACKUP_KEEP
  int64_t save_data_size = 128ll << 20;                // reported by GetSaveDataSize (not seen in nacp: assumed)
  int64_t save_journal_size = 32ll << 20;
  // ---- user
  std::string user_name = "Default";
  uint64_t uid[2] = {0x534e415052454331ull, 0x0000000000000001ull};  // stable default; "uid": "<32 hex digits>"
  // ---- system
  int language = 1;                                    // nn::settings::Language: 1 = AmericanEnglish (en-US)
  bool language_explicit = false;                      // hle.json has a numeric "language" (then Auto leaves it alone)
  std::string region = "US";
  bool docked = true;                                  // nn::oe::GetOperationMode: Console
  std::string display_version = "2.0.1";
  std::string player_name = "Ash";                     // answer to the name prompt (nn::swkbd); hle.json "player_name", env SNAP_PLAYER_NAME
  bool headless = false;                               // nn::err::Show* only logs (tests, CI); env SNAP_HLE_HEADLESS=1
  std::string input_map_path;                          // default %SNAP_CONFIG_DIR% or <data_root>, file input.json
  std::string screenshot_dir;                          // nn::album::SaveScreenshot; default <data_root>\screenshots
  int controller_style = 0;                            // 0 Pro Controller (FullKey, id No1), 1 Handheld (id 0x20)
};

Config& config();                                      // process-wide, filled by load_config / defaults
// Reads the JSON file at `path` (empty: the default path). Returns false and logs if the file exists but is invalid;
// a missing file is fine (defaults). Applies env overrides afterwards and fills the derived paths.
bool load_config(const std::string& path = "");
// The language the game is told to use (nn::settings::Language index): env SNAP_GAME_LANGUAGE (the F1 / launcher "Game language" row; auto | ja | en | fr | de | it | es | ko |
// zh-hans | zh-hant), else hle.json "language" when set, else Auto = the Windows display language mapped to the closest language the game ships, else English.
const char* game_text_suffix(int lang);   // TextResource_<suffix>.drp of a game language index (0 ja, 1 en, 2 fr, 3 de, 4 it, 5 es, 6 zh-hans, 7 ko, 11 zh-hant); "en" for the rest
int resolve_game_language();
int game_language_from_tag(const std::string& tag);    // "fr" -> 2 ...; -1 when the tag is not one of the game's languages
std::string windows_ui_language_tag();                 // the Windows display language mapped to a game tag ("en", "zh-hans" ...), "en" when none fits
// Picks the save profile for this run and points save_root at it (profiles.h). Called once by the host after load_config; does nothing
// when SNAP_SAVE_DIR or save_root is set (unless SNAP_PROFILES_DIR is set too).
void activate_profiles();
std::string tool_save_root(const std::string& profile, std::string* label);   // import/export target (see hle_config.cpp)
void apply_defaults();                                 // derived paths from data_root; called by load_config
std::string appdata_utf8();                            // %APPDATA% as UTF-8 on Windows (hle_host_win32.cpp)
bool migrate_legacy_data_root(const std::string& old_root, const std::string& new_root);   // copies old_root/save to new_root/save once (never moves)
std::string default_data_root();                       // %APPDATA%\SnapRecomp (HOME/.local/share/SnapRecomp elsewhere)
std::string default_input_path();                      // %SNAP_CONFIG_DIR%\input.json, else <data_root>\input.json
std::string legacy_input_path();                       // <data_root>\config\input_map.json (version 1, migrated on first load)
std::string uid_dir_name(const uint64_t uid[2]);       // 32 lowercase hex digits

}  // namespace hle
