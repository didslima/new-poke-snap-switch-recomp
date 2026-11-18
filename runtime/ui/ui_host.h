// SPDX-License-Identifier: MIT
// Neutral platform host interface for runtime/ui (Stage 6F).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct ImGuiIO;

namespace snap::ui::host {

bool init_platform(void* native_window);
void new_frame(ImGuiIO& io, float width, float height);
void shutdown_platform();
void release_cursor_confine();

std::string system_font_path(const char* filename);
uint64_t current_process_id();
char path_sep();

void play_wav_async(const void* wav_bytes, size_t size);
std::string documents_dir();
std::string exe_dir();
bool beside_exe(const char* rel_path);
bool benchmark_launch_from_game();
bool tcp_port_open(const char* ipv4_addr, unsigned short port);
void sleep_ms(unsigned ms);
void open_folder(const std::string& dir);
void open_url(const std::string& url);
bool launch_gallery_server(const std::string& root, const std::string& profile, const std::string& port_file);

}  // namespace snap::ui::host
