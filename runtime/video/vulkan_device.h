// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// The Vulkan instance and device on MoltenVK (macOS: a portability implementation). Offscreen unless a
// SurfaceRequest is given (fm4recomp --renderer window, unit 4b): then the
// instance also enables the window system's surface extensions (SDL_Vulkan_GetInstanceExtensions), the surface is
// created right after the instance, the queue family must support presenting to it, and the device enables
// VK_KHR_swapchain. Every failure stops the run (Setup, 9):
//   - MoltenVK pinned (driverInfo "1.4.2", the Homebrew molten-vk 1.4.2);
//   - the Khronos validation layer pinned (spec version 1.4.363) and inserted by the instance itself
//     when validation is on (fm4recomp --vk-validation on: the M4 default in scripts/gpu_run.sh);
//     a VK_EXT_debug_utils messenger stops the run at the first warning or error it reports (the
//     launcher's log check is the second, independent check).
// After ForzaRecomp runtime/video/presenter.cpp:117-195 (instance with portability
// enumeration, VK_KHR_portability_subset when offered, the MoltenVK pin; GPL-3.0-or-later, same
// owner), reviewed and re-written here. FM4 Recomp changes: SDL stays outside (the surface comes through
// SurfaceRequest, window mode only); offscreen modes need no present support; the validation layer pinned and enabled
// in-process; the messenger stops.
#pragma once
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

namespace runtime::video {

// The window's side of instance and device creation (window mode only; window_sdl.cpp fills it on the main thread).
struct SurfaceRequest
{
    std::vector<const char*> instanceExtensions;  // SDL_Vulkan_GetInstanceExtensions: added (deduplicated) at vkCreateInstance
    VkSurfaceKHR (*create)(VkInstance instance) = nullptr;               // SDL_Vulkan_CreateSurface, main thread; stops on failure
    void (*destroy)(VkInstance instance, VkSurfaceKHR surface) = nullptr;  // SDL_Vulkan_DestroySurface, before the instance
};

constexpr const char* kMoltenVkVersion = "1.4.2";                        // driverInfo, the whole string
constexpr uint32_t kValidationLayerSpec = VK_MAKE_API_VERSION(0, 1, 4, 363);  // VkLayer_khronos_validation.json api_version

struct VulkanDevice
{
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    bool validation = false;
    // Features enabled at vkCreateDevice when the device offers them (logged at creation; the texture unit refuses
    // what is absent): BC1-3 sampling (MoltenVK: MTLDevice.supportsBCTextureCompression) and image-view component
    // swizzles (VK_KHR_portability_subset imageViewFormatSwizzle).
    bool textureCompressionBC = false;
    bool depthClamp = false;        // PA_CL_CLIP_CNTL clip_disable pipelines (pipeline_cache.cpp)
    bool independentBlend = false;
    bool imageViewFormatSwizzle = false;
    bool samplerAnisotropy = false;  // anisotropic samplers (X vulkan_texture_cache.cc:2591-2598)
    // FM4 Recomp (FM4_TRANSFER_STENCIL_EXPORT): VK_EXT_shader_stencil_export enabled when offered (MoltenVK on Apple GPUs:
    // gl_FragStencilRefARB -> Metal [[stencil]]), and sampleRateShading (per-sample fragment shading, gl_SampleID), for the
    // EDRAM unit's depth transfers in one draw instead of nine per sample (edram_vk.cpp ExecuteTransfers).
    bool stencilExport = false;
    bool sampleRateShading = false;
    // Window mode only: the surface (created right after the instance) and VK_KHR_swapchain enabled on the device.
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    void (*destroySurface)(VkInstance, VkSurfaceKHR) = nullptr;
    bool swapchain = false;
};

// Creates the instance (validation as asked), picks the MoltenVK device with a graphics queue,
// creates the device and its queue. Stops (9) on any failure or pin mismatch. With `surface` (window
// mode): its instance extensions, the surface, a queue family that can present to it (both the graphics
// and the present queue: one queue, so no queue-family ownership transfer), and VK_KHR_swapchain.
VulkanDevice CreateVulkanDevice(bool validation, const SurfaceRequest* surface = nullptr);
// Only after GpuGuard::Shutdown kept nothing (no submission's completion unknown): destroying a
// device with work in flight is invalid GPU work (GPU safety). Destroys device, surface,
// messenger, instance (every swapchain must already be destroyed: VUID-vkDestroySurfaceKHR-surface-01266).
void DestroyVulkanDevice(VulkanDevice& d);
// A memory type with all of `wanted`; stops (9) if none.
uint32_t MemoryType(const VulkanDevice& d, uint32_t typeBits, VkMemoryPropertyFlags wanted);
// Validation messages seen so far (any is a stop; the count is for the report).
uint64_t ValidationMessageCount();

}  // namespace runtime::video
