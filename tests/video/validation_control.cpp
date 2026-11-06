// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Target fm4_validation_control.
//
// The validation layer's negative control, run by scripts/gpu_run.sh before every GPU run: a
// deliberate API misuse the pinned Khronos layer must report, else the layer set-up (not the run) is
// broken and nothing runs. After ForzaRecomp tests/video/validation_control.cpp (a
// size-0 buffer, VUID-VkBufferCreateInfo-size-00912; GPL-3.0-or-later, same owner). FM4 Recomp
// changes: the misuse is a physical-device query (vkGetPhysicalDeviceProperties2) whose structure has a
// wrong sType, VUID-VkPhysicalDeviceProperties2-sType-sType (reported by the installed layer 1.4.363,
// OBSERVED; an out-of-range VkFormat is not, see below), through a messenger of the instance. No VkDevice and no resource is created, nothing is
// submitted; the layer's stateless check skips the call, so the driver never sees it, and if the
// layer were missing the driver would only answer a format query. The layer is enabled by the
// program itself (as fm4recomp does), with its own messenger, so the log names the VUID either way.
//
// Exit: 0 when the misuse was issued (the launcher then greps the VUID); 2-4 when the set-up failed.
#include <vulkan/vulkan.h>
#include <cstdio>

namespace {
VKAPI_ATTR VkBool32 VKAPI_CALL Print(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                                     const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    std::printf("[validation control] %s: %s\n", data->pMessageIdName ? data->pMessageIdName : "-", data->pMessage ? data->pMessage : "-");
    return VK_FALSE;
}
}  // namespace

int main()
{
    VkDebugUtilsMessengerCreateInfoEXT messenger{};
    messenger.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    messenger.pfnUserCallback = Print;
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char* extensions[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pNext = &messenger;  // messages during this instance's calls reach Print
    info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    info.enabledLayerCount = 1;
    info.ppEnabledLayerNames = layers;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = extensions;
    VkInstance instance;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
        return 2;
    // The pNext messenger above covers only vkCreateInstance / vkDestroyInstance (VK_EXT_debug_utils: "only during
    // instance creation and destruction"); the misuse below needs a messenger of the instance (as the renderer makes
    // one, runtime/video/vulkan_device.cpp).
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (create == nullptr || destroy == nullptr)
        return 4;
    VkDebugUtilsMessengerEXT handle;
    if (create(instance, &messenger, nullptr, &handle) != VK_SUCCESS)
        return 5;
    uint32_t count = 1;
    VkPhysicalDevice gpu;
    if (vkEnumeratePhysicalDevices(instance, &count, &gpu) < 0 || count == 0)
        return 3;
    // The misuse: a wrong sType on a physical-device query (the stateless parameter check
    // VUID-VkPhysicalDeviceProperties2-sType-sType). OBSERVED with the pinned layer 1.4.363: an
    // out-of-range VkFormat in vkGetPhysicalDeviceFormatProperties(2) / ImageFormatProperties is NOT reported
    // (the local capture gpu_selfcheck2 (not distributed)), so that was not a usable control.
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;  // invalid on purpose
    vkGetPhysicalDeviceProperties2(gpu, &properties);
    std::puts("validation control: misuse issued");
    destroy(instance, handle, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}
