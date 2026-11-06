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
// changes: the misuse is a physical-device query with an invalid VkFormat (999: core formats end at
// 184, extension formats start at 1000000000), VUID-vkGetPhysicalDeviceFormatProperties-format-parameter
// (present in the installed layer 1.4.363). No VkDevice and no resource is created, nothing is
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
    uint32_t count = 1;
    VkPhysicalDevice gpu;
    if (vkEnumeratePhysicalDevices(instance, &count, &gpu) < 0 || count == 0)
        return 3;
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(gpu, VkFormat(999), &properties);  // invalid on purpose
    std::puts("validation control: misuse issued");
    vkDestroyInstance(instance, nullptr);
    return 0;
}
