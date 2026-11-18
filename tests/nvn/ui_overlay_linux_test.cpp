// SPDX-License-Identifier: MIT
// Stage 6F: Linux UI overlay, ImGui 1.93.0 Vulkan dynamic rendering, SDL3 hotkey/event bridge,
// and clean teardown validation.
#include "host/linux/window_sdl3.h"
#include "runtime/ui/ui_host.h"
#include "runtime/ui/ui_internal.h"
#include "runtime/ui/ui_present.h"

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#ifdef HLE_WITH_SDL3
#include <SDL3/SDL.h>
#endif

// os_kernel.cpp calls rt_dispatch for guest execution; this UI test never runs guest code.
extern "C" void rt_dispatch(void*) {
  std::fprintf(stderr, "unexpected rt_dispatch call in ui_overlay_linux_test\n");
  std::abort();
}

extern "C" void snap_ui_shutdown();

namespace {

int g_checks = 0;
int g_fails = 0;

void check(bool ok, const char* msg, int line) {
  ++g_checks;
  if (!ok) {
    ++g_fails;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, msg);
  }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

void test_ui_host_basics() {
  CHECK(snap::ui::host::path_sep() == '/');
  CHECK(snap::ui::host::current_process_id() > 0);
  snap_ui_first_picture();
  CHECK(!snap_ui_start_pending());
}

void test_sdl3_hotkeys_and_vulkan_present() {
  snap_linux::HostWindow win;
  snap_linux::WindowConfig cfg;
  cfg.title = "snap_ui_overlay_linux_test";
  cfg.width = 640;
  cfg.height = 360;
  cfg.hidden = true;

  if (!win.create(cfg)) {
    std::printf("ui_overlay_linux_test: SDL video unavailable; skipping SDL3/Vulkan overlay frame test\n");
    return;
  }

#ifdef HLE_WITH_SDL3
  // Verify F1 toggles overlay_open() and F3 cycles fps_overlay_mode() through HostWindow::poll_events().
  CHECK(!snap::ui::overlay_open());
  auto push_key = [&](SDL_Keycode key) {
    SDL_Event ev{};
    ev.type = SDL_EVENT_KEY_DOWN;
    ev.key.windowID = win.window_id();
    ev.key.key = key;
    ev.key.down = true;
    ev.key.repeat = false;
    CHECK(SDL_PushEvent(&ev));
  };

  push_key(SDLK_F1);
  (void)win.poll_events();
  CHECK(snap::ui::overlay_open());

  push_key(SDLK_F1);
  (void)win.poll_events();
  CHECK(!snap::ui::overlay_open());

  const int m0 = snap::ui::fps_overlay_mode();
  push_key(SDLK_F3);
  (void)win.poll_events();
  CHECK(snap::ui::fps_overlay_mode() != m0);
  snap::ui::fps_overlay_set("default");
#endif

  // Exercise real ImGui + ImGui_ImplVulkan_Init + snap_ui_present() + snap_ui_shutdown() on Vulkan 1.3.
  VkApplicationInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  ai.pApplicationName = "ui_overlay_linux_test";
  ai.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &ai;

  VkInstance inst = VK_NULL_HANDLE;
  if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS || inst == VK_NULL_HANDLE) {
    std::printf("ui_overlay_linux_test: vkCreateInstance unavailable; skipping Vulkan render test\n");
    win.destroy();
    return;
  }

  uint32_t pd_count = 0;
  vkEnumeratePhysicalDevices(inst, &pd_count, nullptr);
  std::vector<VkPhysicalDevice> pds(pd_count);
  if (pd_count) vkEnumeratePhysicalDevices(inst, &pd_count, pds.data());

  VkPhysicalDevice phys = VK_NULL_HANDLE;
  uint32_t family = 0;
  for (VkPhysicalDevice pd : pds) {
    VkPhysicalDeviceProperties pr{};
    vkGetPhysicalDeviceProperties(pd, &pr);
    if (pr.apiVersion < VK_API_VERSION_1_3) continue;
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
    for (uint32_t i = 0; i < qn; ++i) {
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        phys = pd;
        family = i;
        break;
      }
    }
    if (phys != VK_NULL_HANDLE) break;
  }

  if (phys == VK_NULL_HANDLE) {
    std::printf("ui_overlay_linux_test: no Vulkan 1.3 GPU available; skipping Vulkan render test\n");
    vkDestroyInstance(inst, nullptr);
    win.destroy();
    return;
  }

  float qprio = 1.0f;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &qprio;

  VkPhysicalDeviceVulkan13Features f13{};
  f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  f13.dynamicRendering = VK_TRUE;
  f13.synchronization2 = VK_TRUE;

  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.pNext = &f13;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;

  VkDevice dev = VK_NULL_HANDLE;
  CHECK(vkCreateDevice(phys, &dci, nullptr, &dev) == VK_SUCCESS && dev != VK_NULL_HANDLE);

  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(dev, family, 0, &queue);

  // Create a 640x360 RGBA8 offscreen target image mimicking the swapchain image in TRANSFER_DST_OPTIMAL.
  VkImageCreateInfo img_ci{};
  img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  img_ci.imageType = VK_IMAGE_TYPE_2D;
  img_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
  img_ci.extent = {640, 360, 1};
  img_ci.mipLevels = 1;
  img_ci.arrayLayers = 1;
  img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
  img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  img_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage img = VK_NULL_HANDLE;
  CHECK(vkCreateImage(dev, &img_ci, nullptr, &img) == VK_SUCCESS);

  VkMemoryRequirements mr{};
  vkGetImageMemoryRequirements(dev, img, &mr);
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(phys, &mp);
  uint32_t mem_idx = UINT32_MAX;
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      mem_idx = i;
      break;
    }
  }
  CHECK(mem_idx != UINT32_MAX);
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = mem_idx;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  CHECK(vkAllocateMemory(dev, &mai, nullptr, &mem) == VK_SUCCESS);
  CHECK(vkBindImageMemory(dev, img, mem, 0) == VK_SUCCESS);

  VkCommandPoolCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpci.queueFamilyIndex = family;
  cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkCommandPool cp = VK_NULL_HANDLE;
  CHECK(vkCreateCommandPool(dev, &cpci, nullptr, &cp) == VK_SUCCESS);

  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = cp;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb) == VK_SUCCESS);

  VkCommandBufferBeginInfo cbbi{};
  cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  CHECK(vkBeginCommandBuffer(cb, &cbbi) == VK_SUCCESS);

  VkImageMemoryBarrier2 pre{};
  pre.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  pre.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
  pre.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  pre.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  pre.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  pre.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  pre.image = img;
  pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo pre_di{};
  pre_di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  pre_di.imageMemoryBarrierCount = 1;
  pre_di.pImageMemoryBarriers = &pre;
  vkCmdPipelineBarrier2(cb, &pre_di);

  snap::ui::overlay_set_open(true);
  SnapUiFrame uf{inst, phys, dev, queue, family, cb, img, VK_FORMAT_B8G8R8A8_UNORM, {640, 360},
                 reinterpret_cast<VkSwapchainKHR>(0x1), 3, win.native_handle()};
  snap_ui_present(&uf);
  CHECK(snap::ui::overlay_hwnd() == win.native_handle());

  CHECK(vkEndCommandBuffer(cb) == VK_SUCCESS);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS);
  CHECK(vkQueueWaitIdle(queue) == VK_SUCCESS);

  snap::ui::overlay_set_open(false);
  snap_ui_shutdown();
  CHECK(snap::ui::overlay_hwnd() == nullptr);

  vkDestroyCommandPool(dev, cp, nullptr);
  vkDestroyImage(dev, img, nullptr);
  vkFreeMemory(dev, mem, nullptr);
  vkDestroyDevice(dev, nullptr);
  vkDestroyInstance(inst, nullptr);
  win.destroy();
}

}  // namespace

int main() {
  test_ui_host_basics();
  test_sdl3_hotkeys_and_vulkan_present();
  std::printf("ui_overlay_linux_test: %d checks, %d failed\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
