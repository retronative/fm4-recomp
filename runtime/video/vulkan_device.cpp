// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
#define VK_ENABLE_BETA_EXTENSIONS  // VkPhysicalDevicePortabilitySubsetFeaturesKHR (vulkan_beta.h)
#include "vulkan_device.h"
#include "stops.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace runtime::video {
namespace {

std::atomic<uint64_t> validationMessages{0};

void Check(VkResult result, const char* what)
{
    if (result != VK_SUCCESS)
        Stop(StopCode::Setup, "vulkan: %s failed (VkResult %d)", what, int(result));
}

// Any warning or error the validation layer reports ends the run (M4 Accept: validation clean).
// Info and verbose messages are not enabled. The message is printed whole first: it is the evidence.
VKAPI_ATTR VkBool32 VKAPI_CALL OnValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                                                   const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    validationMessages++;
    std::fprintf(stderr, "[vulkan validation] severity %X type %X %s: %s\n", unsigned(severity), unsigned(types),
                 data && data->pMessageIdName ? data->pMessageIdName : "-", data && data->pMessage ? data->pMessage : "-");
    Stop(StopCode::Setup, "vulkan: the validation layer reported a %s (see the line above)",
         (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "error" : "warning");
}

VkDebugUtilsMessengerCreateInfoEXT MessengerInfo()
{
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = OnValidationMessage;
    return info;
}

// The pinned validation layer must be visible to the loader (scripts/gpu_run.sh sets VK_LAYER_PATH).
void RequirePinnedLayer()
{
    uint32_t count = 0;
    Check(vkEnumerateInstanceLayerProperties(&count, nullptr), "vkEnumerateInstanceLayerProperties");
    std::vector<VkLayerProperties> layers(count);
    Check(vkEnumerateInstanceLayerProperties(&count, layers.data()), "vkEnumerateInstanceLayerProperties");
    for (const VkLayerProperties& l : layers)
        if (!std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation"))
        {
            if (l.specVersion != kValidationLayerSpec)
                Stop(StopCode::Setup, "vulkan: validation layer spec %u.%u.%u; pinned to 1.4.363", VK_API_VERSION_MAJOR(l.specVersion),
                     VK_API_VERSION_MINOR(l.specVersion), VK_API_VERSION_PATCH(l.specVersion));
            return;
        }
    Stop(StopCode::Setup, "vulkan: --vk-validation on, but VK_LAYER_KHRONOS_validation is not available (VK_LAYER_PATH)");
}

}  // namespace

uint64_t ValidationMessageCount() { return validationMessages.load(); }

VulkanDevice CreateVulkanDevice(bool validation)
{
    VulkanDevice d;
    d.validation = validation;
    if (validation)
        RequirePinnedLayer();
    std::vector<const char*> extensions{VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};  // MoltenVK is listed only when asked
    std::vector<const char*> layers;
    const VkDebugUtilsMessengerCreateInfoEXT messengerInfo = MessengerInfo();
    if (validation)
    {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "fm4recomp";
    application.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pNext = validation ? &messengerInfo : nullptr;  // also covers vkCreateInstance / vkDestroyInstance
    info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = uint32_t(layers.size());
    info.ppEnabledLayerNames = layers.data();
    Check(vkCreateInstance(&info, nullptr, &d.instance), "vkCreateInstance");
    if (validation)
    {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(d.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create == nullptr)
            Stop(StopCode::Setup, "vulkan: vkCreateDebugUtilsMessengerEXT is not available");
        Check(create(d.instance, &messengerInfo, nullptr, &d.messenger), "vkCreateDebugUtilsMessengerEXT");
    }

    uint32_t count = 0;
    Check(vkEnumeratePhysicalDevices(d.instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(d.instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
    for (VkPhysicalDevice candidate : devices)
    {
        VkPhysicalDeviceDriverProperties driver{};
        driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &driver;
        vkGetPhysicalDeviceProperties2(candidate, &properties);
        std::fprintf(stderr, "[gpu] device %s, driver %s %s, Vulkan %u.%u, per-stage samplers %u, sampled images %u, uniform buffers %u, storage buffers %u\n",
                     properties.properties.deviceName, driver.driverName, driver.driverInfo, VK_API_VERSION_MAJOR(properties.properties.apiVersion),
                     VK_API_VERSION_MINOR(properties.properties.apiVersion), properties.properties.limits.maxPerStageDescriptorSamplers,
                     properties.properties.limits.maxPerStageDescriptorSampledImages, properties.properties.limits.maxPerStageDescriptorUniformBuffers,
                     properties.properties.limits.maxPerStageDescriptorStorageBuffers);
        if (driver.driverID != VK_DRIVER_ID_MOLTENVK)
            continue;  // only the pinned MoltenVK is used (no other driver is approved)
        if (std::strcmp(driver.driverInfo, kMoltenVkVersion) != 0)
            Stop(StopCode::Setup, "vulkan: MoltenVK %s; pinned to %s", driver.driverInfo, kMoltenVkVersion);
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
        std::vector<VkQueueFamilyProperties> family(families);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, family.data());
        for (uint32_t i = 0; i < families && d.gpu == VK_NULL_HANDLE; i++)
            if (family[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                d.gpu = candidate, d.queueFamily = i;
        if (d.gpu != VK_NULL_HANDLE)
            break;
    }
    if (d.gpu == VK_NULL_HANDLE)
        Stop(StopCode::Setup, "vulkan: no MoltenVK device with a graphics queue");

    uint32_t available = 0;
    Check(vkEnumerateDeviceExtensionProperties(d.gpu, nullptr, &available, nullptr), "vkEnumerateDeviceExtensionProperties");
    std::vector<VkExtensionProperties> offered(available);
    Check(vkEnumerateDeviceExtensionProperties(d.gpu, nullptr, &available, offered.data()), "vkEnumerateDeviceExtensionProperties");
    std::vector<const char*> enabled;
    bool portabilityOffered = false;
    for (const VkExtensionProperties& e : offered)
        if (!std::strcmp(e.extensionName, "VK_KHR_portability_subset"))  // required when offered
            enabled.push_back("VK_KHR_portability_subset"), portabilityOffered = true;
    // Features the texture unit needs, enabled only when offered (Vulkan: enabling an unsupported feature is invalid):
    // textureCompressionBC (MoltenVK sets it from MTLDevice.supportsBCTextureCompression) and the portability subset's
    // imageViewFormatSwizzle (component mappings in image views; MoltenVK hard-codes it on Apple GPUs). Both logged.
    VkPhysicalDevicePortabilitySubsetFeaturesKHR portability{};
    portability.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = portabilityOffered ? &portability : nullptr;
    vkGetPhysicalDeviceFeatures2(d.gpu, &features);
    d.textureCompressionBC = features.features.textureCompressionBC == VK_TRUE;
    // Pipeline features the renderer uses when offered: a feature a pipeline relies on must be enabled here, not only
    // supported (VUID-VkPipelineRasterizationStateCreateInfo-depthClampEnable-00782).
    d.depthClamp = features.features.depthClamp == VK_TRUE;
    d.independentBlend = features.features.independentBlend == VK_TRUE;
    // Required: every translated vertex shader stores to binding 5 (XeErrors, shader_glsl.cpp kVertexHeader), which needs
    // vertexPipelineStoresAndAtomics (VUID-RuntimeSpirv-NonWritable-06341).
    if (features.features.vertexPipelineStoresAndAtomics != VK_TRUE)
        Stop(StopCode::Setup, "vulkan: the device lacks vertexPipelineStoresAndAtomics, which the translated vertex shaders need (XeErrors)");
    d.imageViewFormatSwizzle = portabilityOffered && portability.imageViewFormatSwizzle == VK_TRUE;
    VkPhysicalDeviceFeatures enabledFeatures{};
    enabledFeatures.textureCompressionBC = d.textureCompressionBC ? VK_TRUE : VK_FALSE;
    enabledFeatures.depthClamp = d.depthClamp ? VK_TRUE : VK_FALSE;
    enabledFeatures.independentBlend = d.independentBlend ? VK_TRUE : VK_FALSE;
    enabledFeatures.vertexPipelineStoresAndAtomics = VK_TRUE;
    VkPhysicalDevicePortabilitySubsetFeaturesKHR enabledPortability{};
    enabledPortability.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR;
    enabledPortability.imageViewFormatSwizzle = d.imageViewFormatSwizzle ? VK_TRUE : VK_FALSE;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = d.queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = uint32_t(enabled.size());
    deviceInfo.ppEnabledExtensionNames = enabled.data();
    deviceInfo.pEnabledFeatures = &enabledFeatures;
    deviceInfo.pNext = portabilityOffered ? &enabledPortability : nullptr;
    Check(vkCreateDevice(d.gpu, &deviceInfo, nullptr, &d.device), "vkCreateDevice");
    vkGetDeviceQueue(d.device, d.queueFamily, 0, &d.queue);
    // The formats the first screens need, as the device reports them:
    // S sampled, C colour attachment, D depth/stencil attachment, B blit destination, T transfer destination (optimal tiling).
    std::fprintf(stderr, "[gpu] features: textureCompressionBC %s, depthClamp %s, independentBlend %s, imageViewFormatSwizzle %s (portability subset %s)\n",
                 d.textureCompressionBC ? "yes" : "NO", d.depthClamp ? "yes" : "NO", d.independentBlend ? "yes" : "NO", d.imageViewFormatSwizzle ? "yes" : "NO", portabilityOffered ? "offered" : "absent");
    static constexpr struct { VkFormat format; const char* name; } kFormats[] = {
        {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1_RGBA_UNORM (Xenos DXT1)"}, {VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM (k_8_8_8_8)"},
        {VK_FORMAT_R8_UNORM, "R8_UNORM (k_8)"}, {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM (2_10_10_10 targets, format 54 front buffer)"},
        {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32_SFLOAT_S8_UINT (D24FS8 stand-in)"}, {VK_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT"}};
    for (const auto& f : kFormats)
    {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(d.gpu, f.format, &props);
        const VkFormatFeatureFlags o = props.optimalTilingFeatures;
        std::fprintf(stderr, "[gpu] format %s: %s%s%s%s%s(optimal %08X)\n", f.name, o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ? "S " : "",
                     o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT ? "C " : "", o & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? "D " : "",
                     o & VK_FORMAT_FEATURE_BLIT_DST_BIT ? "B " : "", o & VK_FORMAT_FEATURE_TRANSFER_DST_BIT ? "T " : "", o);
    }
    std::fprintf(stderr, "[gpu] MoltenVK %s, queue family %u, validation %s\n", kMoltenVkVersion, d.queueFamily,
                 validation ? "on (VK_LAYER_KHRONOS_validation 1.4.363, messenger stops at the first warning)" : "OFF");
    return d;
}

void DestroyVulkanDevice(VulkanDevice& d)
{
    if (d.device != VK_NULL_HANDLE)
        vkDestroyDevice(d.device, nullptr);
    if (d.messenger != VK_NULL_HANDLE)
        if (auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(d.instance, "vkDestroyDebugUtilsMessengerEXT")))
            destroy(d.instance, d.messenger, nullptr);
    if (d.instance != VK_NULL_HANDLE)
        vkDestroyInstance(d.instance, nullptr);
    d = VulkanDevice{};
}

uint32_t MemoryType(const VulkanDevice& d, uint32_t typeBits, VkMemoryPropertyFlags wanted)
{
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(d.gpu, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted)
            return i;
    Stop(StopCode::Setup, "vulkan: no memory type with properties %X", unsigned(wanted));
}

}  // namespace runtime::video
