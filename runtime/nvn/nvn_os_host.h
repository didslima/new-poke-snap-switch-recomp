// SPDX-License-Identifier: MIT
// Neutral OS host interface for the shared NVN->Vulkan layer (stage 6E-A).
// Windows: runtime/nvn/nvn_os_host_win32.cpp; Linux: runtime/nvn/linux/nvn_os_host_posix.cpp.
#pragma once

#include <cctype>

namespace nvn::os_host {

int primary_screen_height();

inline bool ascii_ieq(const char* a, const char* b) {
  if (!a || !b) return a == b;
  while (*a && *b) {
    if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

}  // namespace nvn::os_host
