// SPDX-License-Identifier: MIT
// Win32 host helpers for the shared NVN->Vulkan layer (stage 6E-A).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "nvn_os_host.h"

namespace nvn::os_host {

int primary_screen_height() {
  return GetSystemMetrics(SM_CYSCREEN);
}

}  // namespace nvn::os_host
