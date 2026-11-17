// SPDX-License-Identifier: MIT
// POSIX (Linux) host helpers for the shared NVN->Vulkan layer (stage 6E-A).
#if !defined(__linux__)
#error "runtime/nvn/linux/nvn_os_host_posix.cpp is the Linux implementation of nvn_os_host.h"
#endif

#include "../nvn_os_host.h"

namespace nvn::os_host {

int primary_screen_height() {
  return 0;
}

}  // namespace nvn::os_host
