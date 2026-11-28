// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The GPU keep-busy experiment (keep_busy.h). No part of the renderer's device, queue, slots or guard is touched.
#include "keep_busy.h"
#include "gpu_stop_write.h"
#include "shader_cache.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace runtime::video::keepbusy {
namespace {

// One fixed, bounded piece of work: kGroups x kGroups workgroups of 8x8 invocations, each a loop of kIterations steps, one store.
constexpr uint32_t kIterations = 32, kLocal = 8;
constexpr uint32_t kMaxDepth = 4;  // FM4_GPU_KEEP_BUSY_DEPTH: submissions kept queued (1: submit, then wait, as first built)

struct Unit
{
    bool asked = false, ready = false;
    uint32_t groups = 64, cycleSeconds = 0, depth = 1;
    uint64_t fenceTimeoutNs = 0;  // the renderer guard's bound
    bool (*rendererIdle)() = nullptr;  // FM4_GPU_KEEP_BUSY_GATED=1: submit only while the renderer has nothing in flight
    bool interactive = false;          // FM4_GPU_KEEP_BUSY_QOS=interactive (default: utility)
    std::string stopPath;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands[kMaxDepth] = {};  // one per queued submission, each with its own fence and buffer region
    VkFence fences[kMaxDepth] = {};
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    std::thread worker;
} unit;
std::atomic<bool> on{false};        // the worker submits while set
std::atomic<bool> quit{false};      // the worker ends after its submission in flight completes
std::atomic<bool> stranded{false};  // a fence did not signal in time: completion unknown, nothing destroyed
std::atomic<uint64_t> submissions{0};
std::atomic<bool> joined{false};
void Join()
{
    quit = true;
    if (std::this_thread::get_id() == unit.worker.get_id())
        return;  // a stop raised on the worker itself (a validation message from its own call): it submits nothing further
    if (!joined.exchange(true) && unit.worker.joinable())
        unit.worker.join();
}
std::chrono::steady_clock::time_point cycleStart;
bool cycleStarted = false;
int cyclePhase = -1;

std::string Glsl(uint32_t groups)
{
    const std::string edge = std::to_string(groups * kLocal);
    return "#version 450\n"
           "layout(local_size_x = 8, local_size_y = 8) in;\n"
           "layout(set = 0, binding = 0, std430) buffer Out { float value[]; } o;\n"
           "void main()\n"
           "{\n"
           "    uint index = gl_GlobalInvocationID.y * " + edge + "u + gl_GlobalInvocationID.x;\n"
           "    float x = float(index & 1023u) * (1.0 / 1024.0);\n"
           "    for (int i = 0; i < " + std::to_string(kIterations) + "; i++)\n"
           "        x = fract(x * 1.6180339 + 0.25);\n"
           "    o.value[index] = x;\n"
           "}\n";
}

bool GpuStopPresent() { return !unit.stopPath.empty() && ::access(unit.stopPath.c_str(), F_OK) == 0; }

void Work()
{
    pthread_set_qos_class_self_np(unit.interactive ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_UTILITY, 0);  // utility: below the game's threads
    bool inFlight[kMaxDepth] = {};
    // Waits for submission `i` if it is in flight (bounded); false when its completion is unknown (GPU_STOP written).
    const auto retire = [&](uint32_t i) {
        if (!inFlight[i])
            return true;
        if (vkWaitForFences(unit.device, 1, &unit.fences[i], VK_TRUE, unit.fenceTimeoutNs) != VK_SUCCESS)
        {
            stranded = true;  // completion unknown: nothing of this unit is destroyed, and the run's stop flag is written
            std::string error;
            if (!WriteGpuStop(unit.stopPath.c_str(), "fm4recomp keep-busy experiment (FM4_GPU_KEEP_BUSY): a fence did not signal within the fence bound; completion unknown", error))
                std::fprintf(stderr, "[keepbusy] GPU_STOP NOT WRITTEN: %s\n", error.c_str());
            std::fprintf(stderr, "[keepbusy] a fence did not signal within the fence bound: GPU_STOP written, no further keep-busy work\n");
            return false;
        }
        inFlight[i] = false;
        submissions++;
        return true;
    };
    const auto retireAll = [&] {
        for (uint32_t i = 0; i < unit.depth; i++)
            if (!retire(i))
                return false;
        return true;
    };
    uint32_t sinceStopCheck = 0, next = 0;
    while (!quit.load())
    {
        // Off, or gated while the game's own work is in flight: what is queued completes, nothing new is submitted.
        if (!on.load() || (unit.rendererIdle != nullptr && !unit.rendererIdle()))
        {
            if (!retireAll())
                return;
            std::this_thread::sleep_for(on.load() ? std::chrono::microseconds(100) : std::chrono::microseconds(1000));
            continue;
        }
        if (sinceStopCheck++ % 256 == 0 && GpuStopPresent())  // every GPU entry point honours GPU_STOP
        {
            std::fprintf(stderr, "[keepbusy] GPU_STOP is present: no further keep-busy work\n");
            retireAll();
            return;
        }
        // The oldest of the `depth` submissions: waited for, then submitted again (so up to `depth` stay queued).
        const uint32_t i = next;
        next = (next + 1) % unit.depth;
        if (!retire(i))
            return;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &unit.commands[i];
        if (vkResetFences(unit.device, 1, &unit.fences[i]) != VK_SUCCESS || vkQueueSubmit(unit.queue, 1, &submit, unit.fences[i]) != VK_SUCCESS)
        {
            std::fprintf(stderr, "[keepbusy] a submission was refused: no further keep-busy work\n");
            retireAll();
            return;
        }
        inFlight[i] = true;
    }
    retireAll();  // the thread ends with nothing in flight (or with GPU_STOP written)
}

}  // namespace

void Initialize(const VulkanDevice& renderer, SpirvCompiler& compiler, const char* gpuStopPath, uint64_t fenceTimeoutNs, bool (*rendererIdle)())
{
    const char* asked = std::getenv("FM4_GPU_KEEP_BUSY");
    if (asked == nullptr || std::strcmp(asked, "1") != 0)
        return;
    unit.asked = true;
    unit.stopPath = gpuStopPath != nullptr ? gpuStopPath : "";
    unit.fenceTimeoutNs = fenceTimeoutNs;
    const char* gated = std::getenv("FM4_GPU_KEEP_BUSY_GATED");
    unit.rendererIdle = gated != nullptr && std::strcmp(gated, "1") == 0 ? rendererIdle : nullptr;
    const char* qos = std::getenv("FM4_GPU_KEEP_BUSY_QOS");
    unit.interactive = qos != nullptr && std::strcmp(qos, "interactive") == 0;
    if (const char* v = std::getenv("FM4_GPU_KEEP_BUSY_GROUPS"))
    {
        const long n = std::strtol(v, nullptr, 10);
        if (n < 1 || n > 256)
        {
            std::fprintf(stderr, "[keepbusy] FM4_GPU_KEEP_BUSY_GROUPS must be 1..256: the experiment stays off\n");
            return;
        }
        unit.groups = uint32_t(n);
    }
    if (const char* v = std::getenv("FM4_GPU_KEEP_BUSY_DEPTH"))
    {
        const long n = std::strtol(v, nullptr, 10);
        if (n < 1 || n > long(kMaxDepth))
        {
            std::fprintf(stderr, "[keepbusy] FM4_GPU_KEEP_BUSY_DEPTH must be 1..%u: the experiment stays off\n", kMaxDepth);
            return;
        }
        unit.depth = uint32_t(n);
    }
    if (const char* v = std::getenv("FM4_GPU_KEEP_BUSY_CYCLE"))
    {
        const long n = std::strtol(v, nullptr, 10);
        if (n < 1 || n > 600)
        {
            std::fprintf(stderr, "[keepbusy] FM4_GPU_KEEP_BUSY_CYCLE must be 1..600 seconds: the experiment stays off\n");
            return;
        }
        unit.cycleSeconds = uint32_t(n);
    }
    std::string warnings, error;
    std::vector<uint32_t> spirv;
    if (unit.stopPath.empty() || !compiler.CompileCompute(Glsl(unit.groups), spirv, warnings, error))
    {
        std::fprintf(stderr, "[keepbusy] off: %s\n", unit.stopPath.empty() ? "no GPU_STOP path" : error.c_str());
        return;
    }
    // A device of its own on the renderer's GPU and queue family (which has compute: checked), nothing enabled.
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(renderer.gpu, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family(families);
    vkGetPhysicalDeviceQueueFamilyProperties(renderer.gpu, &families, family.data());
    if (renderer.queueFamily >= families || !(family[renderer.queueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT))
    {
        std::fprintf(stderr, "[keepbusy] off: the queue family has no compute\n");
        return;
    }
    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(renderer.gpu, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> offered(extensionCount);
    vkEnumerateDeviceExtensionProperties(renderer.gpu, nullptr, &extensionCount, offered.data());
    std::vector<const char*> extensions;
    for (const VkExtensionProperties& e : offered)
        if (std::strcmp(e.extensionName, "VK_KHR_portability_subset") == 0)
            extensions.push_back("VK_KHR_portability_subset");  // must be enabled when offered (VUID-VkDeviceCreateInfo-pProperties-04451)
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = renderer.queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = uint32_t(extensions.size());
    deviceInfo.ppEnabledExtensionNames = extensions.data();
    if (vkCreateDevice(renderer.gpu, &deviceInfo, nullptr, &unit.device) != VK_SUCCESS)
    {
        std::fprintf(stderr, "[keepbusy] off: no second device\n");
        return;
    }
    vkGetDeviceQueue(unit.device, renderer.queueFamily, 0, &unit.queue);
    const VkDeviceSize bytes = VkDeviceSize(unit.groups) * kLocal * unit.groups * kLocal * 4;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes * unit.depth;  // a region of `bytes` per queued submission
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 1;
    setInfo.pBindings = &binding;
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &unit.setLayout;
    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = spirv.size() * 4;
    shaderInfo.pCode = spirv.data();
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, unit.depth};
    VkDescriptorPoolCreateInfo descriptorInfo{};
    descriptorInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptorInfo.maxSets = unit.depth;
    descriptorInfo.poolSizeCount = 1;
    descriptorInfo.pPoolSizes = &size;
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = renderer.queueFamily;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSet sets[kMaxDepth] = {};
    VkDescriptorSetLayout layouts[kMaxDepth] = {};
    bool ok = vkCreateBuffer(unit.device, &bufferInfo, nullptr, &unit.buffer) == VK_SUCCESS;
    if (ok)
    {
        VkMemoryRequirements need;
        vkGetBufferMemoryRequirements(unit.device, unit.buffer, &need);
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = need.size;
        allocate.memoryTypeIndex = MemoryType(renderer, need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ok = vkAllocateMemory(unit.device, &allocate, nullptr, &unit.memory) == VK_SUCCESS &&
             vkBindBufferMemory(unit.device, unit.buffer, unit.memory, 0) == VK_SUCCESS;
    }
    ok = ok && vkCreateDescriptorSetLayout(unit.device, &setInfo, nullptr, &unit.setLayout) == VK_SUCCESS &&
         vkCreatePipelineLayout(unit.device, &layoutInfo, nullptr, &unit.layout) == VK_SUCCESS &&
         vkCreateShaderModule(unit.device, &shaderInfo, nullptr, &module) == VK_SUCCESS &&
         vkCreateDescriptorPool(unit.device, &descriptorInfo, nullptr, &unit.descriptors) == VK_SUCCESS &&
         vkCreateCommandPool(unit.device, &poolInfo, nullptr, &unit.pool) == VK_SUCCESS;
    for (uint32_t i = 0; ok && i < unit.depth; i++)
        ok = vkCreateFence(unit.device, &fenceInfo, nullptr, &unit.fences[i]) == VK_SUCCESS;
    if (ok)
    {
        VkComputePipelineCreateInfo pipeline{};
        pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline.stage.module = module;
        pipeline.stage.pName = "main";
        pipeline.layout = unit.layout;
        VkDescriptorSetAllocateInfo allocateSet{};
        allocateSet.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocateSet.descriptorPool = unit.descriptors;
        for (uint32_t i = 0; i < unit.depth; i++)
            layouts[i] = unit.setLayout;
        allocateSet.descriptorSetCount = unit.depth;
        allocateSet.pSetLayouts = layouts;
        VkCommandBufferAllocateInfo allocateCommands{};
        allocateCommands.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateCommands.commandPool = unit.pool;
        allocateCommands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateCommands.commandBufferCount = unit.depth;
        ok = vkCreateComputePipelines(unit.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &unit.pipeline) == VK_SUCCESS &&
             vkAllocateDescriptorSets(unit.device, &allocateSet, sets) == VK_SUCCESS &&
             vkAllocateCommandBuffers(unit.device, &allocateCommands, unit.commands) == VK_SUCCESS;
    }
    if (module != VK_NULL_HANDLE)
        vkDestroyShaderModule(unit.device, module, nullptr);
    if (ok)
    {
        for (uint32_t i = 0; ok && i < unit.depth; i++)
        {
            const VkDescriptorBufferInfo region{unit.buffer, bytes * i, bytes};
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = sets[i];
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &region;
            vkUpdateDescriptorSets(unit.device, 1, &write, 0, nullptr);
            VkCommandBufferBeginInfo begin{};  // recorded once, before any submission; submitted again only after completion
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            ok = vkBeginCommandBuffer(unit.commands[i], &begin) == VK_SUCCESS;
            if (ok)
            {
                vkCmdBindPipeline(unit.commands[i], VK_PIPELINE_BIND_POINT_COMPUTE, unit.pipeline);
                vkCmdBindDescriptorSets(unit.commands[i], VK_PIPELINE_BIND_POINT_COMPUTE, unit.layout, 0, 1, &sets[i], 0, nullptr);
                vkCmdDispatch(unit.commands[i], unit.groups, unit.groups, 1);
                ok = vkEndCommandBuffer(unit.commands[i]) == VK_SUCCESS;
            }
        }
    }
    if (!ok)
    {
        std::fprintf(stderr, "[keepbusy] off: a Vulkan object could not be created\n");
        Shutdown();  // nothing was submitted
        return;
    }
    unit.ready = true;
    on = unit.cycleSeconds == 0;  // with a cycle the first window is OFF (OnSwap)
    unit.worker = std::thread(Work);
    std::fprintf(stderr, "[keepbusy] GPU keep-busy experiment on (FM4_GPU_KEEP_BUSY=1): a second device submits one compute dispatch of %ux%u workgroups "
                         "(%u-step loop, %llu-byte buffer) after another, %u kept queued, each waited for (%llu ms bound, GPU_STOP on a miss)%s%s; %s\n",
                 unit.groups, unit.groups, kIterations, (unsigned long long)bytes, unit.depth, (unsigned long long)(fenceTimeoutNs / 1000000),
                 unit.rendererIdle != nullptr ? ", only while the renderer has nothing in flight (FM4_GPU_KEEP_BUSY_GATED=1)" : "",
                 unit.interactive ? ", interactive quality of service" : "",
                 unit.cycleSeconds == 0 ? "always" : ("alternating OFF/ON every " + std::to_string(unit.cycleSeconds) + " s from the first swap, OFF first").c_str());
}

void OnSwap(uint64_t swap)
{
    if (!unit.ready || unit.cycleSeconds == 0)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (!cycleStarted)
        cycleStart = now, cycleStarted = true;
    const int phase = int(std::chrono::duration_cast<std::chrono::seconds>(now - cycleStart).count() / unit.cycleSeconds);
    if (phase == cyclePhase)
        return;
    cyclePhase = phase;
    const bool next = phase % 2 == 1;
    on = next;
    const double unixSeconds = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::fprintf(stderr, "[keepbusy] %s from swap %llu at %.3f (window %d; %llu keep-busy submission(s) so far)\n", next ? "ON" : "OFF",
                 (unsigned long long)swap, unixSeconds, phase, (unsigned long long)submissions.load());
}

void Quiesce()
{
    if (!unit.ready)
        return;
    Join();  // the worker returns after the submission it waits for (at most the fence bound)
    std::fprintf(stderr, "[keepbusy] at stop: %llu keep-busy submission(s)%s\n", (unsigned long long)submissions.load(), stranded.load() ? "; one never completed" : "");
}

bool Shutdown()
{
    if (unit.device == VK_NULL_HANDLE)
        return true;
    Join();
    if (unit.ready)
        std::fprintf(stderr, "[keepbusy] %llu keep-busy submission(s)%s\n", (unsigned long long)submissions.load(),
                     stranded.load() ? "; one never completed: its device is left alone" : "");
    if (stranded.load())
        return false;  // completion unknown: destroying anything (the instance included) would be invalid GPU work
    vkDestroyPipeline(unit.device, unit.pipeline, nullptr);  // each null-safe; every submission completed (the worker waited)
    vkDestroyPipelineLayout(unit.device, unit.layout, nullptr);
    vkDestroyDescriptorSetLayout(unit.device, unit.setLayout, nullptr);
    vkDestroyDescriptorPool(unit.device, unit.descriptors, nullptr);
    for (uint32_t i = 0; i < unit.depth; i++)
        vkDestroyFence(unit.device, unit.fences[i], nullptr);
    vkDestroyCommandPool(unit.device, unit.pool, nullptr);
    vkDestroyBuffer(unit.device, unit.buffer, nullptr);
    vkFreeMemory(unit.device, unit.memory, nullptr);
    vkDestroyDevice(unit.device, nullptr);
    unit.device = VK_NULL_HANDLE;
    unit.ready = false;
    return true;
}

}  // namespace runtime::video::keepbusy
