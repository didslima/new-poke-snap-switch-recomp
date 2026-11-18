// SPDX-License-Identifier: MIT
// Neutral platform host interface for runtime/ui (Stage 6F).
#pragma once

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

}  // namespace snap::ui::host
