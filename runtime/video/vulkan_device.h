// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// The Vulkan instance and device on MoltenVK (macOS: a portability implementation), offscreen: no
// surface, no swapchain (the window is unit 4b). Every failure stops the run (Setup, 9):
//   - MoltenVK pinned (driverInfo "1.4.2", the Homebrew molten-vk 1.4.2);
//   - the Khronos validation layer pinned (spec version 1.4.363) and inserted by the instance itself
//     when validation is on (fm4recomp --vk-validation on: the M4 default in scripts/gpu_run.sh);
//     a VK_EXT_debug_utils messenger stops the run at the first warning or error it reports (the
//     launcher's log check is the second, independent check).
// After ForzaRecomp runtime/video/presenter.cpp:117-195 (instance with portability
// enumeration, VK_KHR_portability_subset when offered, the MoltenVK pin; GPL-3.0-or-later, same
// owner), reviewed and re-written here. FM4 Recomp changes: no SDL/surface; a graphics queue without
// present support; the validation layer pinned and enabled in-process; the messenger stops.
#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>

namespace runtime::video {

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
    bool imageViewFormatSwizzle = false;
};

// Creates the instance (validation as asked), picks the MoltenVK device with a graphics queue,
// creates the device and its queue. Stops (9) on any failure or pin mismatch.
VulkanDevice CreateVulkanDevice(bool validation);
// Only after GpuGuard::Shutdown kept nothing (no submission's completion unknown): destroying a
// device with work in flight is invalid GPU work (GPU safety). Destroys device, messenger,
// instance.
void DestroyVulkanDevice(VulkanDevice& d);
// A memory type with all of `wanted`; stops (9) if none.
uint32_t MemoryType(const VulkanDevice& d, uint32_t typeBits, VkMemoryPropertyFlags wanted);
// Validation messages seen so far (any is a stop; the count is for the report).
uint64_t ValidationMessageCount();

}  // namespace runtime::video
