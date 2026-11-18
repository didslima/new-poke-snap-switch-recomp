// SPDX-License-Identifier: MIT
// Explicit Linux stub for runtime/nvn/upscale/upscale_nvn.h (stage 6E-A Group 2C-3).
// Temporal upscalers (DLSS / FSR / XeSS) are currently Windows-only; on Linux they report unsupported explicitly.
#include "../upscale/upscale_nvn.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace nvn {

bool UpscaleApplyPolicy(float* /*sceneScale*/, float* /*outScale*/) {
  if (const char* e = std::getenv("SNAP_UPSCALER"); e && *e && std::strcmp(e, "0") != 0 && std::strcmp(e, "off") != 0) {
    static std::once_flag warned;
    std::call_once(warned, [e] {
      std::fprintf(stderr, "[upscale] SNAP_UPSCALER=%s is not supported on Linux yet; using native resolution\n", e);
    });
  }
  return false;
}

void UpscaleTagTexture(TextureImpl* /*t*/, const uint64_t* /*chainRel*/, int /*n*/, uint32_t /*tag*/) {}
void UpscaleForgetTexture(TextureImpl* /*t*/) {}

void UpscaleInstanceExtensions(std::vector<const char*>& /*ext*/) {}
void UpscaleDeviceExtensions(Host* /*h*/, std::vector<const char*>& /*ext*/, void** /*chainHead*/,
                             VkPhysicalDeviceVulkan11Features* /*f11*/,
                             VkPhysicalDeviceVulkan12Features* /*f12*/,
                             VkPhysicalDeviceVulkan13Features* /*f13*/) {}
void UpscaleDeviceCreated(Host* /*h*/) {}
void UpscaleDestroy(Host* /*h*/) {}

bool UpscaleOnSetRenderTargets(ReplayCtx& /*cx*/) { return false; }
void UpscaleViewportJitter(const ReplayCtx& /*cx*/, float* jx, float* jy) {
  if (jx) *jx = 0.0f;
  if (jy) *jy = 0.0f;
}
void UpscaleSceneJitter(float* jx, float* jy) {
  if (jx) *jx = 0.0f;
  if (jy) *jy = 0.0f;
}
TextureImpl* UpscaleSubstitute(const ReplayCtx& /*cx*/, TextureImpl* /*t*/) { return nullptr; }
void UpscaleBeforeSubmit(int /*n*/, CmdList* const* /*lists*/) {}
void UpscaleOnDraw(ReplayCtx& /*cx*/, const uint8_t* const* /*vsUbo*/, const uint64_t* /*vsRange*/, int /*n*/) {}
void UpscaleOnPresent() {}

}  // namespace nvn
