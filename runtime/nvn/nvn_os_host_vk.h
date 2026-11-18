// SPDX-License-Identifier: MIT
// Neutral Vulkan surface and instance-extension host helpers for the shared NVN layer (stage 6E-A).
#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace nvn::os_host {

void append_vulkan_surface_instance_extensions(std::vector<const char*>& out);
bool physical_device_presentation_support(VkInstance instance, VkPhysicalDevice phys, uint32_t queue_family);
bool create_vulkan_surface(VkInstance instance,
                           void* native_window,
                           const VkAllocationCallbacks* allocator,
                           VkSurfaceKHR* out_surface);

}  // namespace nvn::os_host
