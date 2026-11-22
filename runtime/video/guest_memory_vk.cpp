// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// guest_memory_vk.h. The import follows ReCarbon runtime/gpu/renderer.cpp:6531-6582 ImportSharedMemory (GPL-3.0-or-later):
// vkGetMemoryHostPointerPropertiesEXT, a buffer with VkExternalMemoryBufferCreateInfo HOST_ALLOCATION, VkImportMemoryHostPointerInfoEXT,
// the memory type from both type masks preferring HOST_VISIBLE | HOST_COHERENT (:6560). FM4 Recomp changes: a dedicated host view
// instead of the guest view itself, the alignment and range checks, storage use only, the fallback instead of a shadow copy.
#include "guest_memory_vk.h"
#include "mem_ledger.h"  // FM4 Recomp (M9, FM4_DIAG_MEMORY)
#include "vulkan_device.h"
#include "guest_memory.h"
#include "stops.h"
#include "kernel/command_processor.h"
#include "kernel/virtual_memory.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>

namespace runtime::video {
namespace {

GuestMemoryGpu view;
VkDeviceMemory memory = VK_NULL_HANDLE;
GpuReadGuard* guard = nullptr;  // allocated once, never freed (the fault hook may read it until the process ends)
bool on = false;
uint64_t waitBoundNs = 0;

uint64_t MonotonicNs()  // clock_gettime: async-signal-safe (POSIX.1-2008)
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
}
void NapInSignal()
{
    timespec t{0, 20000};  // 20 us, as ReCarbon's WaitForGpu (write_watch.cpp:741)
    nanosleep(&t, nullptr);
}

// The fault side (guest_memory.h GpuReadFaultHooks; signal handler, any thread). Only a store to a shared physical page the heap
// means writable can be the watch's protection, so only those wait (texture_vk.cpp WatchFault decides the same way).
bool FaultEnter(uint32_t guest)
{
    if (guest < 0xA0000000u || guest >= 0xE0000000u)
        return false;
    const uint32_t physical = guest & (runtime::kPhysicalSize - 1);
    if (!runtime::PhysicalHostWritable(physical))
        return false;
    GpuReadGuard* g = guard;
    if (runtime::gpu::OnCommandProcessorThread() && g->Busy(physical, 1))
        StopFromSignal(StopCode::Unimplemented,
                       "GPU read guard: the command processor stored to physical %08X, which unretired GPU part %llu reads (completed %llu); "
                       "a store site without command_processor.h beforeCpWrite",
                       physical, (unsigned long long)g->WriteReader(physical, 1), (unsigned long long)g->CompletedSerial());
    // FM4 Recomp (FM4_ASYNC_RETIRE): the completion worker's resolve write-back must not wait either: the part it would wait for is
    // one it retires itself (async_retire.h), so the wait could only end at the bound. Today's command-processor retire stops here.
    if (runtime::gpu::OnCompletionWorkerThread() && g->Busy(physical, 1))
        StopFromSignal(StopCode::Unimplemented,
                       "GPU read guard: a resolve write-back (completion worker) stored to physical %08X, which unretired GPU part %llu reads "
                       "(completed %llu)",
                       physical, (unsigned long long)g->WriteReader(physical, 1), (unsigned long long)g->CompletedSerial());
    switch (g->EnterWrite(physical, 1, waitBoundNs, MonotonicNs, NapInSignal))
    {
    case GpuReadGuard::WaitResult::Done: return true;
    case GpuReadGuard::WaitResult::Stranded:
        StopFromSignal(StopCode::GpuStranded, "GPU read guard: a store to physical %08X waits for GPU part %llu, but the GPU is stranded",
                       physical, (unsigned long long)g->WriteReader(physical, 1));
    case GpuReadGuard::WaitResult::TimedOut: break;
    }
    StopFromSignal(StopCode::Unimplemented,
                   "GPU read guard: a store to physical %08X waited %llu ms for GPU part %llu (submitted %llu, completed %llu): the command "
                   "processor did not retire it",
                   physical, (unsigned long long)(waitBoundNs / 1000000), (unsigned long long)g->WriteReader(physical, 1),
                   (unsigned long long)g->SubmittedSerial(), (unsigned long long)g->CompletedSerial());
}
void FaultLeave() { guard->LeaveWrite(); }
const runtime::GpuReadFaultHooks faultHooks{FaultEnter, FaultLeave};

// The heaps' side (virtual_memory.h SetGpuReadGuard): ordinary thread context, no heap lock held.
bool HeapBusy(uint32_t physical, uint32_t size) { return guard->Busy(physical, size); }
void HeapWait(uint32_t physical, uint32_t size)
{
    if (!guard->Busy(physical, size))
        return;
    if (runtime::gpu::OnCommandProcessorThread())  // never seen: the command processor neither frees memory nor reads files
        Stop(StopCode::Unimplemented, "GPU read guard: the command processor writes physical %08X+%X through the heap while GPU part %llu reads it",
             physical, size, (unsigned long long)guard->WriteReader(physical, size));
    const auto nap = [] { std::this_thread::sleep_for(std::chrono::microseconds(20)); };
    switch (guard->WaitForWrite(physical, size, waitBoundNs, MonotonicNs, nap))
    {
    case GpuReadGuard::WaitResult::Done: return;
    case GpuReadGuard::WaitResult::Stranded:
        Stop(StopCode::GpuStranded, "GPU read guard: a host write to physical %08X+%X waits for a stranded GPU", physical, size);
    case GpuReadGuard::WaitResult::TimedOut: break;
    }
    Stop(StopCode::Unimplemented, "GPU read guard: a host write to physical %08X+%X waited %llu ms for GPU part %llu (completed %llu)", physical, size,
         (unsigned long long)(waitBoundNs / 1000000), (unsigned long long)guard->WriteReader(physical, size), (unsigned long long)guard->CompletedSerial());
}

uint32_t ImportType(const VulkanDevice& dev, uint32_t bits)
{
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(dev.gpu, &properties);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & want) == want)
            return i;
    for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
        if (bits & (1u << i))
            return i;
    return UINT32_MAX;
}

// The import; on any refusal its reason (nothing kept but the host view, which the GPU never saw).
const char* Import(const VulkanDevice& dev, uint8_t*& host, uint32_t& type)
{
    if (!dev.externalMemoryHost || dev.getMemoryHostPointerProperties == nullptr)
        return "the device does not offer VK_EXT_external_memory_host";
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(dev.gpu, &properties);
    if (uint64_t(properties.limits.maxStorageBufferRange) < runtime::kPhysicalSize)
        return "the device's maxStorageBufferRange is below 512 MB";
    host = runtime::MapPhysicalHostView();
    if (host == nullptr)
        return "no host view of the physical memory (vm_map)";
    const VkDeviceSize align = dev.minImportedHostPointerAlignment;
    if (align == 0 || reinterpret_cast<uintptr_t>(host) % align != 0 || runtime::kPhysicalSize % align != 0)
        return "the host view or its size is not a multiple of minImportedHostPointerAlignment";
    VkMemoryHostPointerPropertiesEXT pointer{};
    pointer.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
    if (dev.getMemoryHostPointerProperties(dev.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &pointer) != VK_SUCCESS)
        return "vkGetMemoryHostPointerPropertiesEXT refused the host view";
    VkExternalMemoryBufferCreateInfo external{};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.pNext = &external;
    info.size = runtime::kPhysicalSize;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;  // read by shaders only: nothing writes guest memory through it
    if (vkCreateBuffer(dev.device, &info, nullptr, &view.buffer) != VK_SUCCESS)
    {
        view.buffer = VK_NULL_HANDLE;
        return "vkCreateBuffer refused the import buffer";
    }
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(dev.device, view.buffer, &requirements);
    type = requirements.size <= runtime::kPhysicalSize ? ImportType(dev, requirements.memoryTypeBits & pointer.memoryTypeBits) : UINT32_MAX;
    VkImportMemoryHostPointerInfoEXT import{};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import.pHostPointer = host;
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.pNext = &import;
    allocate.allocationSize = runtime::kPhysicalSize;
    allocate.memoryTypeIndex = type;
    const char* why = type == UINT32_MAX                                                          ? "no memory type both the buffer and the host pointer accept"
                      : vkAllocateMemory(dev.device, &allocate, nullptr, &memory) != VK_SUCCESS ? "vkAllocateMemory refused the import"
                      : vkBindBufferMemory(dev.device, view.buffer, memory, 0) != VK_SUCCESS     ? "vkBindBufferMemory refused the import"
                                                                                                 : nullptr;
    if (why != nullptr)  // nothing was submitted that uses them: freed at once
    {
        if (memory != VK_NULL_HANDLE)
            vkFreeMemory(dev.device, memory, nullptr), memory = VK_NULL_HANDLE;
        vkDestroyBuffer(dev.device, view.buffer, nullptr), view.buffer = VK_NULL_HANDLE;
    }
    if (why == nullptr)  // the guest's own pages, seen by the GPU: shared, never added to the renderer's total (mem_ledger.h)
        runtime::video::mem::Global().Add(runtime::video::mem::Kind::GuestShared, runtime::kPhysicalSize, "guest physical memory (host pointer import)");
    return why;
}

}  // namespace

bool GuestMemoryGpuAsked()
{
    static const bool asked = [] { const char* v = std::getenv("FM4_GUEST_MEMORY_GPU"); return v != nullptr && v[0] == '1' && v[1] == '\0'; }();
    return asked;
}
const GuestMemoryGpu* GuestMemoryGpuView() { return on ? &view : nullptr; }
GpuReadGuard* GuestMemoryGpuGuard() { return on ? guard : nullptr; }

void InitGuestMemoryGpu(const VulkanDevice& dev, uint64_t fenceTimeoutNs)
{
    if (!GuestMemoryGpuAsked())
        return;
    const char* watch = std::getenv("FM4_WRITE_WATCH");
    uint8_t* host = nullptr;
    uint32_t type = UINT32_MAX;
    const char* why = !(watch != nullptr && watch[0] == '1' && watch[1] == '\0') || !runtime::WatchAvailable()
                          ? "it needs FM4_WRITE_WATCH=1 (16 KB host pages and the heaps)"
                          : Import(dev, host, type);
    if (why != nullptr)
    {
        std::fprintf(stderr, "[gpu] FM4_GUEST_MEMORY_GPU=1 ignored: %s; vertex windows keep their copy paths\n", why);
        return;
    }
    guard = new GpuReadGuard(runtime::kPhysicalSize, 4096, runtime::kWatchHostPage);
    if (!guard->Valid())
        Stop(StopCode::Setup, "GPU read guard: the page layout is not valid");
    waitBoundNs = 2 * fenceTimeoutNs + 1000000000ull;
    if (const char* h = std::getenv("FM4_GUARD_HOT"); h != nullptr && (h[0] == '1' || h[0] == '2') && h[1] == '\0')  // FM4 Recomp (M7): gpu_read_guard.h Hot
    {
        guard->SetHotTracking(true, h[0] == '2');
        std::fprintf(stderr, "[gpu] write-watch hot pages on (FM4_GUARD_HOT=%c): a 16 KB page a store waited for is read through the copy paths from then on%s\n",
                     h[0], h[0] == '2' ? "; a page hot just after a hot page also makes the next 4 MB hot (a streaming ring)" : "");
    }
    view.bytes = runtime::kPhysicalSize;
    on = true;
    runtime::SetGpuReadGuard(HeapBusy, HeapWait);
    runtime::SetGpuReadFaultHooks(&faultHooks);
    std::fprintf(stderr, "[gpu] guest memory for the GPU on (FM4_GUEST_MEMORY_GPU=1): 512 MB imported from a host view at %p (memory type %u, import "
                         "alignment %llu); vertex windows read directly where the write watch guards them, stores to them wait for the GPU "
                         "(bound %llu ms)\n",
                 static_cast<void*>(host), type, (unsigned long long)dev.minImportedHostPointerAlignment, (unsigned long long)(waitBoundNs / 1000000));
}

void ShutdownGuestMemoryGpu(const VulkanDevice& dev)
{
    if (!on)
        return;
    runtime::SetGpuReadFaultHooks(nullptr);
    runtime::SetGpuReadGuard(nullptr, nullptr);
    on = false;
    vkDestroyBuffer(dev.device, view.buffer, nullptr);
    vkFreeMemory(dev.device, memory, nullptr);
    view = GuestMemoryGpu{}, memory = VK_NULL_HANDLE;
}

}  // namespace runtime::video
