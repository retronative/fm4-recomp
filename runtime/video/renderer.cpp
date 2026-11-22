// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// The present path (upload a CPU-detiled front buffer, copy to a display image, read back) follows
// ForzaRecomp runtime/video/presenter.cpp:332-420 (Show; GPL-3.0-or-later, same owner),
// reviewed and re-written here. FM4 Recomp changes: offscreen (no swapchain); every submission goes
// through GpuGuard (ForzaRecomp's presenter waited with UINT64_MAX, unbounded); the staging buffer is
// a per-frame resource held by the slot's lifetime and destroyed only after a confirmed completion;
// captures go through the capped ring.
//
// The draw path (unit 8, Draw below) records every draw since the previous swap into the slot the swap
// then submits (one submission per frame in unit 4's single slot; unit 5 adds slots and submissions
// within a frame). Per draw, in this order and before anything is recorded: the gate's in-memory
// state; DecodeDrawState (draw_state.h, fail closed); a resolve to the EDRAM hooks, a NoWrite draw to
// the one policy switch (draw_record.h kNoWriteDraws); the host indices; GpuGuard::DrawLoopWork; the
// shader variants and the pipeline key (draw_record.h); PipelineCache::Get, whose first step is the
// shader-class allow-list (a never-seen class runs first in its own approved run); the
// render target from the EDRAM hooks; the uploads into the slot's ring; the descriptor set from the
// slot's pool; then one render pass with one draw. The ring and the pool are reset exactly in
// OpenSlot, after the slot's previous submission was confirmed complete (RetireSlot) and before the
// next one records anything; the xe_error dwords of the slot's draws are read in RetireSlot, after the
// confirmed completion and before that reset.
//
// Window mode (unit 4b): the same present path, then the display image is handed to the main thread's presenter
// (window_present.cpp) after the swap's submission retired and its capture was written; the next swap waits (bounded)
// only while the presenter is blitting from it. The one VkQueue is shared with the presenter: every submission here
// takes the queue lock first (bounded; uncontended in the other modes).
#include "thread_qos.h"
#include "renderer.h"
#include "diag_trigger.h"
#include "capture_store.h"
#include "capture_keep.h"
#include "draw_record.h"
#include "draw_state.h"
#include "edram_vk.h"
#include "front_buffer.h"
#include "render_scale.h"  // FM4 Recomp (M9, FM4_INTERNAL_RES)
#include "mem_ledger.h"    // FM4 Recomp (M9, FM4_DIAG_MEMORY)
#include "gpu_present.h"  // FM4 Recomp (M7): FM4_GPU_PRESENT
#include "post_aa.h"  // FM4 Recomp (M9): FM4_POST_AA
#include "hdr_output.h"  // FM4 Recomp (M9): FM4_HDR
#include "diag_hdr.h"    // FM4 Recomp (M9 diagnostic): FM4_DIAG_HDR_TOGGLE
#include "diag_res_switch.h"  // FM4 Recomp (M9 diagnostic): FM4_DIAG_RES_CYCLE
#include "diag_draw_list.h"  // FM4 Recomp (M9 diagnostic): FM4_DIAG_DRAW_LIST
#include "kernel/extended_view.h"  // FM4 Recomp (M9): Extended View
#include "zpd_report.h"  // FM4 Recomp (M9): FM4_OCCLUSION_QUERIES
#include "keep_busy.h"  // FM4 Recomp (M9 experiment): FM4_GPU_KEEP_BUSY
#include "gpu_guard.h"
#include "async_submit.h"
#include "async_retire.h"  // FM4 Recomp (M7): FM4_ASYNC_RETIRE
#include "guest_writes.h"
#include "pipeline_cache.h"
#include "push_template.h"  // FM4 Recomp (M7 CPU cost): FM4_PUSH_TEMPLATE
#include "png.h"
#include "diag_alpha.h"
#include "diag_depth.h"
#include "diag_blend.h"  // FM4 Recomp: FM4_DIAG_BLEND_LOG / FM4_DIAG_PS_SKIP (diagnostic)
#include "diag_slow_submit.h"  // FM4 Recomp: FM4_DIAG_SLOW_SUBMIT (diagnostic)
#include "diag_drain.h"  // FM4 Recomp: FM4_DIAG_DRAIN (diagnostic)
#include "diag_cp_delay.h"  // FM4 Recomp: FM4_DIAG_CP_DELAY_NS (diagnostic)
#include "diag_compile.h"  // FM4 Recomp: compile time beside a long swap (diagnostic)
#include "texture_dump.h"
#include "ps_trace.h"
#include "shader_cache.h"
#include "shader_class.h"
#include "texture_vk.h"
#include "vertex_cache.h"  // FM4 Recomp (M7, vertex reuse across submissions): FM4_VERTEX_CACHE_MB
#include "submit_policy.h"  // FM4 Recomp (M7): FM4_SUBMIT_SPLIT / FM4_SUBMIT_IF_IDLE
#include "guest_memory_vk.h"  // FM4 Recomp (M7, guest memory for the GPU): FM4_GUEST_MEMORY_GPU
#include "vulkan_device.h"
#include "window_present.h"
#include "kernel/command_processor.h"
#include "kernel/input_script.h"
#include "kernel/thread_cpu.h"
#include "kernel/cp_trace.h"
#include "kernel/graphics.h"
#include "kernel/av_latency.h"  // FM4 Recomp: FM4_DIAG_AV_LATENCY (diagnostic)
#include "kernel/virtual_memory.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <cstdarg>  // FM4_OCCLUSION_QUERIES: ZpdLog
#include <cstddef>  // offsetof (FM4_PUSH_TEMPLATE)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>  // FM4_ASYNC_RETIRE: the completion worker names itself
#include <optional>
#include <set>
#include <tuple>
#include <thread>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>

// Added to stops.h / stops.cpp by the stop drain.
void SetStopQuiesce(void (*quiesce)(), void (*signalReport)());

namespace runtime::video {
namespace {

constexpr uint32_t kDisplayWidth = 1280, kDisplayHeight = 720;  // the accepted video mode (imports_video.cpp)
// FM4 Recomp: slot 0 owns the ring, the descriptor pool and the timestamps; the others are command buffers and fences only, used
// by FM4_SUBMIT_SPLIT (SplitSubmit) to submit part of the open work without waiting (slots 1-7). Without it only slot 0 is used.
constexpr uint32_t kSlots = 8;
constexpr size_t kCaptureRecords = 16;
constexpr uint64_t kCaptureBytes = 64ull << 20;                    // 16 stored-deflate 1280x720 PNGs are ~44 MB
constexpr size_t kKeepRecords = 96;                               // FM4_CAPTURE_KEEP_EVERY (capture_keep.h)
constexpr uint64_t kKeepBytes = 320ull << 20;                      // 96 x ~2.8 MB
constexpr uint32_t kMaxVertexWindowDwords = 1u << 20;               // 4 MiB per vertex slot: a window past its fetch constant (VertexWindowDwords) stops above it
constexpr uint32_t kDrawsPerSlot = 4096;                           // descriptor sets per slot pool (a race frame is ~3,000 draws, L §3.4); a set
                                                                   // costs every binding of the layout (32 samplers), written or not

// A buffer the recorded commands use and that is destroyed only after their completion is confirmed.
struct Transient
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    uint64_t ledger = 0;  // FM4 Recomp (M9, FM4_DIAG_MEMORY): mem_ledger.h id
};

Options options;
bool enabled = false;
VulkanDevice dev;
struct Slot
{
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    // Unit 8: the slot's persistent upload ring (uniform, storage and index data of its draws), its
    // descriptor pool, and the xe_error dword of each draw recorded into the open submission.
    Transient ring;
    std::optional<UploadRing> allocator;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    bool open = false;  // recording (OpenSlot) and not yet submitted
    uint32_t sets = 0;  // FM4 Recomp: descriptor sets allocated from `descriptors` since OpenSlot (at most kDrawsPerSlot)
    // FM4 Recomp (FM4_GPU_TIMESTAMPS=1, diagnostic): a timestamp at the start and the end of the slot's submission.
    VkQueryPool timestamps = VK_NULL_HANDLE;
    bool timed = false, stamped = false;
    // FM4 Recomp (M9, FM4_OCCLUSION_QUERIES; the Zpd unit below): the slot's occlusion queries, the counting segments recorded
    // into its open command buffer, and how many queries the command buffer must reset before it uses any (all of them at first).
    struct ZpdSegment { uint64_t id; uint32_t query, num, den; };
    VkQueryPool zpdPool = VK_NULL_HANDLE;
    std::vector<ZpdSegment> zpdSegments;
    uint32_t zpdUsed = 0, zpdToReset = 0;
    struct ErrorDword
    {
        uint64_t offset, draw;
        // FM4 Recomp: the draw's vertex shader and windows, named (Text) only when its shader flags an error: kept as numbers,
        // since formatting them for every draw cost ~3 % of the command thread.
        uint64_t vsHash = 0;
        uint32_t highIndex = 0;
        struct Window { uint32_t slot, dwords, sizeWords, stride, end; bool computed; };
        // FM4 Recomp (M7 CPU cost): its windows are errorWindows[firstWindow, firstWindow + windowCount) of the slot (one vector a slot,
        // cleared with errorDwords; a vector per draw cost one allocation a draw).
        uint32_t firstWindow = 0, windowCount = 0;
        std::string Text(const Window* errorWindows) const
        {
            char hash[24];
            std::snprintf(hash, sizeof hash, "vs_%016llx", (unsigned long long)vsHash);
            std::string what = std::string(hash) + ", highest index " + std::to_string(highIndex);
            for (uint32_t k = 0; k < windowCount; k++)
            {
                const Window& w = errorWindows[firstWindow + k];
                what += "; slot " + std::to_string(w.slot) + ": window " + std::to_string(w.dwords) + " dwords, constant " + std::to_string(w.sizeWords) +
                        ", stride " + std::to_string(w.stride) + ", end " + std::to_string(w.end) + (w.computed ? ", computed index" : "");
            }
            return what;
        }
    };
    std::vector<ErrorDword> errorDwords;
    std::vector<ErrorDword::Window> errorWindows;  // every errorDwords entry's windows, in order (ErrorDword::firstWindow)
    // FM4 Recomp (FM4_VERTEX_REUSE): the vertex windows copied into this ring since OpenSlot, by physical address and endian:
    // where (dwords into the ring), how many dwords, and the watch sequence they were copied under.
    struct VertexCopy { uint32_t ringDword, dwords; uint64_t sequence, lap; };  // lap: UploadRing::Lap() when copied (Intact)
    std::unordered_map<uint64_t, VertexCopy> vertexCopies;
    // FM4 Recomp (FM4_DESCRIPTOR_CACHE): the sets allocated since the pool's reset, by DescriptorKey (layout, index range, and each
    // texture binding's view and sampler); with dynamic buffer bindings a set does not depend on the draw's ring offsets.
    std::unordered_map<std::string, VkDescriptorSet> descriptorCache;
} slots[kSlots];

// FM4 Recomp (M9, opt-in FM4_OCCLUSION_QUERIES=1): the game's occlusion queries (EVENT_WRITE_ZPD) counted on the GPU instead of
// answered with Xenia's fixed 1000 samples (OBSERVED, research/m9/glare: FM4 sizes the sun's flare by that count, so the fixed
// answer gives a large flare whether or not the sun is hidden). The design and the bookkeeping are ReCarbon's (runtime/gpu/
// renderer.cpp ZpassDone / ZpdBeginSegment / ZpdHarvest and zpd_report.h, GPL-3.0-or-later, commit 66401ea), fitted to this
// renderer's slots and parts:
//   - A query bracket is open from its BEGIN report's ZPD to its END report's. While one is open every draw is counted in a
//     segment: one precise occlusion query, begun right before the draw inside its render pass and ended at the pass's end, at any
//     ZPD, or when the weight of a host sample changes (the target's MSAA or internal scale), so no query spans a pass, a command
//     buffer or a bracket's edge. A draw that only tests depth is drawn for its count (draw_state.h countedOnly).
//   - A slot's queries are reset at the start of its command buffer and read once its submission is confirmed complete (after
//     GpuGuard::Retire; never a wait, never while in flight), into a ring of finished segments.
//   - An END's count is the sum of its bracket's segments in guest samples. The command processor stores the report through its
//     deferred queue, whose entries land only after every part recorded before them has retired: the count is read then
//     (ZpdCount), so the game never sees a count before the GPU finished the query. Nothing waits for a query.
// Valid GPU work only: every query is reset before its use, begun and ended inside one subpass, and its pool destroyed with the
// slot after every submission is confirmed complete. Command-processor thread only.
struct ZpdUnit
{
    static constexpr uint32_t kQueries = 256;  // a command buffer's segments; draws past them are not counted (named once)
    struct End { uint32_t token = 0; uint32_t address = 0; uint64_t s0 = 0, s1 = 0; bool known = false; uint32_t count = 0; };
    static constexpr uint32_t kEnds = 256;     // pending END reports (FM4: two a frame)
    bool on = false;
    int32_t query = -1;                 // the active segment's query in zpdSlot's pool, or -1
    uint32_t querySlot = 0, num = 0, den = 0;
    VkCommandBuffer commands = VK_NULL_HANDLE;  // the command buffer (and so the pass) the active segment was begun in
    uint64_t nextSegment = 1;
    uint32_t nextToken = 2;             // 0 and 1 are the hook's "not counted" and "not an END"
    zpd::BracketBook open;
    std::unique_ptr<zpd::ResultRing> results;
    End ends[kEnds];
    // The [zpd] line's tallies since the last one.
    uint64_t zpds = 0, begins = 0, endReports = 0, segments = 0, draws = 0, countedOnly = 0, untested = 0, unmatched = 0, dropped = 0, overflow = 0,
             unavailable = 0, lost = 0, stored = 0, nonZero = 0, lastCount = 0, maxCount = 0;
    long logLines = 0;                  // FM4_DIAG_ZPD_QUERY_LOG=<lines>: each BEGIN, counted draw, segment and END
} zpdUnit;
__attribute__((format(printf, 1, 2))) void ZpdLog(const char* format, ...)
{
    if (zpdUnit.logLines <= 0)
        return;
    zpdUnit.logLines--;
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    std::fprintf(stderr, "[zpd] %s\n", line);
}
// Ends the active segment, inside the pass it was begun in (callers: the pass's end, a ZPD, a change of weight).
void ZpdEndSegment()
{
    if (zpdUnit.query < 0)
        return;
    vkCmdEndQuery(zpdUnit.commands, slots[zpdUnit.querySlot].zpdPool, uint32_t(zpdUnit.query));
    zpdUnit.query = -1, zpdUnit.commands = VK_NULL_HANDLE;
}
// Before a counted draw, inside its pass on command buffer `c` of slot `s`: the active segment goes on while the weight is the same.
void ZpdBeginSegment(VkCommandBuffer c, uint32_t s, zpd::Weight weight)
{
    if (zpdUnit.query >= 0 && (weight.num != zpdUnit.num || weight.den != zpdUnit.den || c != zpdUnit.commands))
        ZpdEndSegment();
    if (zpdUnit.query >= 0)
        return;
    Slot& slot = slots[s];
    if (slot.zpdUsed >= ZpdUnit::kQueries)
    {
        if (zpdUnit.overflow++ == 0)
            std::fprintf(stderr, "[zpd] %u counting segments in one command buffer: draws past them are not counted (named once)\n", ZpdUnit::kQueries);
        return;
    }
    const uint32_t q = slot.zpdUsed++;
    vkCmdBeginQuery(c, slot.zpdPool, q, VK_QUERY_CONTROL_PRECISE_BIT);
    zpdUnit.query = int32_t(q), zpdUnit.querySlot = s, zpdUnit.commands = c, zpdUnit.num = weight.num, zpdUnit.den = weight.den;
    slot.zpdSegments.push_back({zpdUnit.nextSegment++, q, weight.num, weight.den});
    zpdUnit.segments++;
}
// Slot `s`'s submission is confirmed complete (or the slot was never submitted): its segments' counts into the ring. No wait.
void ZpdHarvest(uint32_t s)
{
    Slot& slot = slots[s];
    if (slot.zpdSegments.empty())
        return;
    static uint64_t results[ZpdUnit::kQueries * 2];  // value, availability
    const uint32_t n = slot.zpdUsed;
    const VkResult r = vkGetQueryPoolResults(dev.device, slot.zpdPool, 0, n, size_t(n) * 16, results, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    for (const Slot::ZpdSegment& g : slot.zpdSegments)
    {
        const bool available = (r == VK_SUCCESS || r == VK_NOT_READY) && results[2 * g.query + 1] != 0;
        const uint64_t raw = available ? results[2 * g.query] : 0;
        zpdUnit.unavailable += available ? 0 : 1;
        const uint64_t guest = zpd::Normalize(raw, g.num, g.den);
        zpdUnit.results->Put(g.id, guest, raw);
        ZpdLog("segment %llu: %s%llu host samples x %u/%u = %llu", (unsigned long long)g.id, available ? "" : "not available, ", (unsigned long long)raw, g.num,
               g.den, (unsigned long long)guest);
    }
    slot.zpdSegments.clear();
}
VkImage display = VK_NULL_HANDLE;
VkDeviceMemory displayMemory = VK_NULL_HANDLE;
// FM4 Recomp (M9, FM4_INTERNAL_RES; render_scale.h OutputScaled): the display image's size, the video mode's (kDisplayWidth x
// kDisplayHeight) or that times the internal scale (CreateResources), and then a second image of the video mode's size: a swap
// that has only 1x front-buffer words is uploaded there and enlarged into the display image (counted: upscaledPresents).
uint32_t displayWidth = 1280, displayHeight = 720;
VkImage display1x = VK_NULL_HANDLE;
VkDeviceMemory display1xMemory = VK_NULL_HANDLE;
uint64_t scaledPresents = 0, upscaledPresents = 0;
// FM4 Recomp (M9, opt-in FM4_POST_AA=1; post_aa.h, adapted from ReCarbon's FXAA-style pass): with the pass on, a swap's picture is
// uploaded into `source` (the display image's format and size) and one compute pass writes the display image from it, so the
// readback, the captures and the window's presenter read the display image exactly as without it. Lifetime: every object is made
// once (CreateResources: the image; InitPostAa: the rest, the descriptor set written once from its own pool, before any command
// uses it) and destroyed only in Shutdown, after every submission is confirmed complete. Off (the default): none of it exists and
// the display image's usage is unchanged.
// FM4_GRAPHICS_LIVE=1 (play.sh; the in-game GRAPHICS menu applies edge smoothing at once): the objects are made at the start whatever
// FM4_POST_AA says, and `level` (0 off, 1 soft, 2 standard, 3 strong; SetPostAaLive, any thread) is read once per presented swap, so
// turning the pass on or off or changing its strength creates and destroys nothing while the game runs.
// FM4_DIAG_POST_AA_PAIR=1 (diagnostic): a kept capture (FM4_CAPTURE_KEEP_EVERY) also writes the same swap before the pass (pre_N.png).
struct PostAaUnit
{
    bool asked = false, on = false, pair = false, live = false;
    const char* refused = nullptr;  // why the device cannot run it (CreateResources), or null
    runtime::video::postaa::Tuning tuning = runtime::video::postaa::kStandard;  // FM4_POST_AA=soft | on | strong
    VkImage source = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView sourceView = VK_NULL_HANDLE, displayView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint64_t passes = 0;
} postAa;
std::atomic<int> postAaLevel{0};  // the pass at the next swap: 0 off, 1 soft, 2 standard, 3 strong (fixed without FM4_GRAPHICS_LIVE)
Transient postAaPre;  // FM4_DIAG_POST_AA_PAIR: the pre-pass readback (persistent, as `readback`)

// FM4 Recomp (M9, opt-in FM4_HDR; hdr_output.h, adapted from SpeedBreaker): the presented picture in linear light with its highlights
// expanded into the display's headroom, for the window's HDR swapchain. One 16-bit float image of the display image's size and one
// small state buffer, created once (InitHdr, before any command buffer uses them) and destroyed at Shutdown; the pass runs in the
// swap's present part after the readback, so the display image, every capture and the standard-range path are untouched.
// Without a window the pass runs only with FM4_HDR_HEADROOM (an assumed headroom): a validated offscreen run of the same commands,
// whose result is read back and summarised at each kept capture.
struct HdrUnit
{
    bool asked = false, on = false, offscreen = false;
    bool live = false;                  // FM4_GRAPHICS_LIVE: the preference may change while the game runs
    float lastPeak = 1.0f;              // the last pass's base peak (1: the standard picture)
    double createMs = 0.0;
    float strength = runtime::video::hdr::kStrengthDefault, assumed = 0.0f, budget = runtime::video::hdr::kLocalBudget;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    Transient state, result;  // the limiter's state and cell field; offscreen: the image read back at a kept capture
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint64_t passes = 0;
} hdrUnit;
std::atomic<float> hdrHeadroom{1.0f};  // SetHdrHeadroom (the window's thread)
std::atomic<bool> hdrWanted{false};    // SetHdrPreference (any thread): Auto (HDR where the display and this run can show it) or Off
std::atomic<bool> hdrUsable{false};    // this run's checks passed: the pass's objects are made at the first swap that uses them
std::atomic<bool> hdrDisplay{false};   // SetHdrDisplay (the window's thread): the display the window is on can show HDR
std::atomic<const char*> hdrUnavailable{nullptr};  // why this run cannot show HDR at all (set once)
Transient readback;  // persistent: read on the CPU only after the slot's completion is confirmed
// FM4 Recomp (M9, live resolution, step 2; opt-in with FM4_GRAPHICS_LIVE=1): the internal resolution changed while the game runs.
// RequestInternalRes (any thread) only latches the scale asked for; Present serves it on the command processor's thread at a swap,
// when nothing is recorded or in flight (ServeResolutionRequest), by making everything of the new size first and only then
// destroying what the old size had (SwitchResolution). A request that cannot be served is refused with one line and the scale
// stays. Without a request nothing here runs: a run at one scale is as it was.
std::atomic<uint32_t> resRequested{0};  // the scale asked for, in halves (render_scale.h); 0: none
uint32_t resAttempts = 0;               // swaps at which the pending request found the renderer not idle (command processor's thread)
bool resClearsCarried = false;          // the EDRAM unit holds pending clears a switch kept aside (dropped at the next swap)
uint64_t resSwitches = 0, resRefusals = 0;
constexpr uint32_t kResAttempts = 120;  // then the request is refused (two seconds of swaps at 60 a second)
// mem_ledger.h ids of the display chain's images (FM4_DIAG_MEMORY), kept so a switch releases what it destroys.
uint64_t displayLedger = 0, postAaLedger = 0, hdrLedger = 0;
uint64_t transientsDestroyed = 0, drawsSinceSwap = 0, shadersSinceSwap = 0;
uint64_t noWriteSinceSwap = 0, resolvesSinceSwap = 0, ringFlushesSinceSwap = 0, drawNumber = 0;
uint64_t fetchUncheckedSinceSwap = 0, fetchCheckedSinceSwap = 0, fetchProofReasonSinceSwap[5] = {};
uint64_t nextPartNeed = 0;  // the ring bytes the draw about to open a part needs (UploadRing::NewPart's minFree)
uint64_t ringLapsSinceSwap = 0;  // FM4 Recomp (M7, vertex reuse across submissions): UploadRing::NewPart laps (each drops vertexCopies)
// The previous draw's constant blocks (M7 CPU cost: bound again by a draw with the same 2,088 words while they are in the ring).
struct LastConstants
{
    std::vector<uint32_t> alu;
    uint32_t bools[8] = {}, loops[32] = {};
    uint64_t vsAt = 0, psAt = 0, lap = 0;
    uint64_t vsLap = 0, psLap = 0;  // FM4 Recomp (FM4_CONSTANTS_PER_STAGE): each block's own lap (one stage may be older than the other)
    uint32_t vsWords = 0, psWords = 0;
} lastConstants;
bool haveLastConstants = false;
uint64_t stageConstantsReusedSinceSwap[2] = {};  // FM4 Recomp (FM4_CONSTANTS_PER_STAGE): draws that bound one stage's block again (VS, PS)
// FM4 Recomp (M7 CPU cost): opt-in FM4_CONSTANTS_PER_STAGE=1 (Draw step 7, draw_record.h SameStageConstants).
bool ConstantsPerStage()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_CONSTANTS_PER_STAGE");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] per-stage constants on (FM4_CONSTANTS_PER_STAGE=1): a stage whose constants equal the previous draw's binds its block again\n");
        return enabled;
    }();
    return on;
}
uint64_t constantsReusedSinceSwap = 0;  // FM4_FETCH_UNCHECKED: draws by vertex shader variant; why not provable (draw_state.h FetchProof)
// FM4 Recomp: per-swap timing (always on, a few clock reads): each FlushSlot (submit, then the bounded wait for completion) by
// cause, and the texture binds (hash, untile, staging) on the CPU.
// FM4 Recomp (M7, vertex reuse across submissions): VertexCache, a flush because no chunk of the vertex cache could be recycled.
// FM4 Recomp (M7, guest memory for the GPU): WriteWatch, a submit and retire a store waited for (gpu_read_guard.h G5).
enum class FlushCause : uint32_t { Resolve, Ring, Swap, Consumer, Sync, NextResolve, Idle, VertexCache, WriteWatch, Count };
constexpr const char* kFlushCauseNames[] = {"resolve", "ring", "swap", "resolve consumer", "CPU sync", "next resolve", "ring idle", "vertex cache",
                                            "write watch"};
static_assert(std::size(kFlushCauseNames) == size_t(FlushCause::Count));
uint64_t syncFlushesByPacketSinceSwap[128] = {};
// FM4 Recomp: what the draws ask of the slot's ring per swap (the ring check's `need`): texture staging, vertex windows, host
// indices, and the fixed system/constant/error blocks with their alignment slack (the timing line's "ring need").
uint64_t ringNeedSinceSwap[4] = {};  // "CPU sync" flushes per PM4 opcode (SyncGuestWrites)
uint64_t flushCountSinceSwap[size_t(FlushCause::Count)] = {}, flushNsSinceSwap[size_t(FlushCause::Count)] = {}, textureBindNsSinceSwap = 0;
uint64_t retireFenceNsSinceSwap = 0, retireWriteBackNsSinceSwap = 0;  // the retires' fence waits and resolve write-backs (inside the flush times)
std::chrono::steady_clock::time_point lastSwapEnd = std::chrono::steady_clock::now();
uint64_t NsSince(std::chrono::steady_clock::time_point t)
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count());
}

// FM4 Recomp (opt-in FM4_RESOLVE_DEFER=1): a resolve does not flush the slot. Its write-back to guest
// memory lands when its submission retires: before a later draw reads a range it writes (index buffer, vertex windows, textures:
// one flush, "resolve consumer"), before a command-processor packet the guest CPU can synchronise on (SyncGuestWrites: event
// writes, memory and register write-backs, interrupts, waits: "CPU sync"), and at the swap. So the guest CPU still sees every
// resolve in guest memory by the time it can know the GPU finished it, as on the hardware; draws no longer wait after each
// resolve. Xenia does not read resolves back to the CPU at all by default (d3d12_command_processor.cc:42-47 d3d12_readback_resolve
// false; vulkan_command_processor.cc:2611 "TODO(Triang3l): CPU readback").
bool ResolveDefer()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_RESOLVE_DEFER");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        std::fprintf(stderr, "[gpu] resolve deferral %s\n", enabled ? "on (FM4_RESOLVE_DEFER=1): a resolve's write-back waits for its first reader"
                                                                        : "off: every resolve flushes the slot");
        return enabled;
    }();
    return on;
}
}  // namespace

// FM4 Recomp (resolve_resident.h): opt-in, on top of FM4_RESOLVE_DEFER (without it every resolve flushes, so nothing is pending).
bool ResolveResident()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_RESOLVE_RESIDENT");
        const bool asked = v != nullptr && v[0] == '1' && v[1] == '\0', enabled = asked && ResolveDefer();
        std::fprintf(stderr, "[gpu] resident resolves %s\n",
                     enabled ? "on (FM4_RESOLVE_RESIDENT=1): a converting resolve also writes the textures that take its words, on the GPU"
                     : asked ? "off: FM4_RESOLVE_RESIDENT=1 needs FM4_RESOLVE_DEFER=1 (ignored)"
                             : "off");
        return enabled;
    }();
    return on;
}

bool ResolveResidentCheck()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_RESOLVE_RESIDENT_CHECK");
        const bool enabled = ResolveResident() && v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] resident resolve check on (FM4_RESOLVE_RESIDENT_CHECK=1): each converted resolve is compared with the CPU's at its retire\n");
        return enabled;
    }();
    return on;
}

// FM4 Recomp (opt-in FM4_RESOLVE_RESIDENT_DEPTH=1): depth resolves get resident words too (edram_vk.cpp kResolveResidentDepth).
bool ResolveResidentDepth()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_RESOLVE_RESIDENT_DEPTH");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[edram] resident depth resolves on (FM4_RESOLVE_RESIDENT_DEPTH=1): a depth resolve's selected sample as resident words\n");
        return enabled;
    }();
    return on;
}
bool ResolveResidentK88()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_RESOLVE_RESIDENT_K88");
        const bool enabled = ResolveResident() && v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] resident k_8_8 on (FM4_RESOLVE_RESIDENT_K88=1): 16 bpp k_8_8 resolves are converted on the GPU\n");
        return enabled;
    }();
    return on;
}

// FM4 Recomp (M7 CPU cost): opt-in FM4_RESOLVE_RESIDENT_1616=1 (resolve_resident.h ResolveResident1616).
bool ResolveResident1616()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_RESOLVE_RESIDENT_1616");
        const bool enabled = ResolveResident() && v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] resident 16_16 on (FM4_RESOLVE_RESIDENT_1616=1): 16_16 resolves into k_16_16 are converted on the GPU\n");
        return enabled;
    }();
    return on;
}

bool ResolveGpuWords()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_RESOLVE_GPU_WORDS");
        const bool asked = v != nullptr && v[0] == '1' && v[1] == '\0', enabled = asked && ResolveResident() && !ResolveResidentCheck();
        if (asked)
            std::fprintf(stderr, enabled ? "[gpu] resolve GPU words on (FM4_RESOLVE_GPU_WORDS=1): a GPU-converted resolve is written back from its words\n"
                                         : "[gpu] FM4_RESOLVE_GPU_WORDS=1 ignored: it needs FM4_RESOLVE_RESIDENT=1 and FM4_RESOLVE_RESIDENT_CHECK off\n");
        return enabled;
    }();
    return on;
}

namespace {
gpu::SubmissionGate gate;
// FM4 Recomp (FM4_GPU_TIMESTAMPS=1, diagnostic): GPU execution time of each submission from two timestamp queries, summed per
// swap, to split the flush waits into GPU work and submission round trips.
// FM4 Recomp (FM4_RING_MB=<16-1024>, opt-in; default 16): the slot's upload ring size. A race frame needs 100-240 MB of vertex
// data, so a 16 MB ring flushes (submit + wait) 7-14 times a frame; ShiftGlue grows its
// upload space instead of waiting (src/ui/graphics_upload_buffer_pool.cpp:93-145). The draws of one submission
// stay bounded by the descriptor pool (kDrawsPerSlot sets: a flush before it is exhausted).
uint64_t RingBytes()
{
    static const uint64_t bytes = [] {
        const char* v = std::getenv("FM4_RING_MB");
        if (v == nullptr)
            return uint64_t(16) << 20;
        char* end = nullptr;
        const unsigned long mb = std::strtoul(v, &end, 10);
        if (end == v || *end != '\0' || mb < 16 || mb > 1024)
            Stop(StopCode::Setup, "renderer: FM4_RING_MB=%s is not a size in MB from 16 to 1024", v);
        std::fprintf(stderr, "[gpu] upload ring %lu MB a slot (FM4_RING_MB)\n", mb);
        return uint64_t(mb) << 20;
    }();
    return bytes;
}

bool GpuTimestamps()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_GPU_TIMESTAMPS");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}
double timestampPeriodNs = 0;

// FM4 Recomp (FM4_PASS_MERGE=1, opt-in): consecutive draws on the same render pass and framebuffer share one render pass instance
// instead of one each. On a tile-based GPU (Apple, through MoltenVK) every instance loads and stores its whole attachments; one
// instance per draw (about 2,100 a race frame) moved them through memory each time. The pass stays open after a draw and is ended
// by the next draw on another framebuffer, by any recording outside a pass (render_hooks.h OutsideRenderPass) and before a
// submission. The technique, independently implemented: ShiftGlue (Xenia-derived, BSD) keeps its render pass while the next
// draw's pass and framebuffer match (src/graphics/vulkan/command_processor.cpp:5292-5311 at 72b7dd6f).
bool PassMerge()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_PASS_MERGE");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] render pass merge on (FM4_PASS_MERGE=1): consecutive draws on one framebuffer share a render pass\n");
        return enabled;
    }();
    return on;
}
struct OpenPass
{
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    int timing = -1;  // FM4_DIAG_PASS_TIMES: its entry in passTimings
    uint32_t areaRows = 0;  // FM4_PASS_AREA: the render area's rows from the top
    VkImageView color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;  // FM4_DIAG_SUBSET_PASSES: the attachments it holds (null: none)
    uint8_t depthKeyFormat = 0, colorKeyFormat = 0;  // FM4_SUBSET_MERGE: the pipeline key's formats of the draw that opened it
} openPass;
// FM4 Recomp (M7 GPU cost, opt-in FM4_SUBSET_MERGE=1; after SpeedBreaker 312ce69
// runtime/gpu/renderer.cpp:2451-2459 and :9080-9100 NFSMW_PASS_MERGE bit 2, GPL-3.0): a draw on the open pass's colour target with no
// depth/stencil (KeyFromDrawState's depthUsed 0: z and stencil disabled) is recorded in the open colour + depth pass instead of ending
// it, with a pipeline built for that pass's attachments whose depth test, depth write, stencil and depth bias are off (the same
// attachment formats and samples: render-pass compatible; the depth attachment is neither tested nor written, so the image and its
// EDRAM tiles are unchanged, as in the draw's own colour-only pass). FM4_DIAG_SUBSET_PASSES counted ~18 such pass ends a race frame,
// each followed by a reopening of the full pass (~17). Likewise a depth-only draw (no colour attachment) on the open pass's depth target,
// with the colour attachment declared and a write mask of 0 (draw_record.h SubsetMergeKeyDepthOnly). A draw whose merged pipeline is
// refused, or not compatible, ends the pass as before.
bool SubsetMerge()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_SUBSET_MERGE");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0' && PassMerge();
        if (v != nullptr && !enabled)
            std::fprintf(stderr, "[gpu] FM4_SUBSET_MERGE=%s ignored: it needs FM4_PASS_MERGE=1\n", v);
        else if (enabled)
            std::fprintf(stderr, "[gpu] subset merge on (FM4_SUBSET_MERGE=1): a colour-only or depth-only draw on the open pass's target stays in the open colour + depth pass\n");
        return enabled;
    }();
    return on;
}
uint64_t subsetMergesSinceSwap = 0;
// FM4 Recomp (diagnostic, FM4_DIAG_SUBSET_PASSES=1): each open draw pass ended for a draw
// on another target, classified by the two draws' attachments (colour, depth image views): the new draw's a strict subset of the open
// pass's (it could stay in it with the other target masked), the same set (ended for the render pass, framebuffer or rows), a
// superset, or another target. Reported every 300 swaps.
bool DiagSubsetPasses()
{
    static const bool on = [] { const char* v = std::getenv("FM4_DIAG_SUBSET_PASSES"); return v != nullptr && v[0] == '1' && v[1] == '\0'; }();
    return on;
}
uint64_t subsetEnds[4] = {0, 0, 0, 0};  // subset, same, superset, other
// FM4 Recomp (diagnostic, FM4_DIAG_VS_TALLY=1; profiles the vertex shaders of the main scene and shadow passes):
// per (target kind, vertex shader) the draws and the vertices they submit (indices, or the auto-indexed count), and the vertex shader's
// fetch slots and GLSL lines, reported every 300 swaps sorted by vertices: the geometry workload each shader carries.
bool DiagVsTally()
{
    static const bool on = [] { const char* v = std::getenv("FM4_DIAG_VS_TALLY"); return v != nullptr && v[0] == '1' && v[1] == '\0'; }();
    return on;
}
struct VsTally { uint64_t draws = 0, vertices = 0; size_t fetchSlots = 0; };
std::map<std::pair<std::string, uint64_t>, VsTally> vsTally;
// FM4 Recomp (opt-in FM4_PASS_AREA=1): a draw render pass's render area is the full width and only the rows the draw can touch
// (its scissor's bottom, rounded up to 32 and capped at the image), not the whole host target: the main scene's 1280x1024 2x
// target is 720 rows on screen and costs ~44 ms of GPU a race frame over ~22 passes, each loading and storing both attachments
// (FM4_DIAG_PASS_TIMES). Load and store act only inside the render area and the pixels
// outside it are not affected (Vulkan vkCmdBeginRenderPass, VkRenderPassBeginInfo::renderArea). A merged pass continues only for
// a draw whose rows fit its area.
bool PassArea()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_PASS_AREA");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] pass area on (FM4_PASS_AREA=1): a draw render pass covers only the rows its draws can touch\n");
        return enabled;
    }();
    return on;
}
// FM4 Recomp (FM4_DIAG_PASS_TIMES=1, diagnostic; needs FM4_GPU_TIMESTAMPS=1 and FM4_PASS_MERGE=1): a bottom-of-pipe timestamp
// before each draw render pass begins and after it ends (outside the pass), read at the slot's retire: GPU time per pass kind
// (target size, samples, formats), summed per swap and tallied every 300 swaps. The rest of GPU busy is the units' own work
// (EDRAM transfers, resolves, clears) and copies.
constexpr uint32_t kPassQueries = 2048;
VkQueryPool passQueries = VK_NULL_HANDLE;
// Also (render_hooks.h PassActivity): what ended each pass, the passes by draw count, and the GPU time between two consecutive
// passes of one command buffer, labelled by the jobs recorded between them.
struct PassTiming
{
    uint32_t query;
    uint32_t draws;
    std::string kind;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    std::string before;  // the jobs recorded since the previous pass of this command buffer ended
    const char* ended = "open";
    bool unit = false;  // a unit's own pass (render_hooks.h UnitPassBegin), tallied apart from the draw passes
};
int unitTiming = -1;  // the unit pass being recorded
uint64_t unitGpuNsSinceSwap = 0;
std::map<std::string, std::pair<uint64_t, uint64_t>> unitTally;  // kind -> (ns, passes)
std::vector<PassTiming> passTimings;
uint32_t passQueriesUsed = 0;
uint64_t passGpuNsSinceSwap = 0, passTimedSinceSwap = 0, gapGpuNsSinceSwap = 0;
std::map<std::string, std::pair<uint64_t, uint64_t>> passTally;  // kind -> (ns, passes)
std::map<std::string, std::pair<uint64_t, uint64_t>> passDrawTally, passGapTally, passEndTally;  // FM4_DIAG_PASS_TIMES (ns, count)
std::string passJobs;  // the jobs recorded outside a pass since the last pass ended
void NotePassJob(const char* job)
{
    if (("+" + passJobs + "+").find(std::string("+") + job + "+") == std::string::npos)
        passJobs += (passJobs.empty() ? "" : "+") + std::string(job);
}
const char* DrawBucket(uint32_t draws)
{
    return draws <= 1 ? "1 draw" : draws <= 7 ? "2-7 draws" : draws <= 31 ? "8-31 draws" : draws <= 127 ? "32-127 draws" : "128+ draws";
}
bool PassTimes()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DIAG_PASS_TIMES");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    return on && passQueries != VK_NULL_HANDLE;
}
uint64_t passesSinceSwap = 0;
// FM4 Recomp (opt-in FM4_VERTEX_REUSE=1, with FM4_WRITE_WATCH=1): a draw's vertex window that the open slot's ring already holds
// (the same physical start and endian, at least as many dwords) and that nothing wrote since it was copied (the write watch's
// sequences: guest stores fault, resolves and system-call writes mark) is read from that copy instead of copied again. The
// vertex binding then spans the whole ring and xe_vertex_base holds each window's dword in it.
// Ranges the watch cannot protect (the E0000000 view) are copied every time, as before.
bool wholeRingVertexBinding = false;  // the device's maxStorageBufferRange covers the ring (Init)
uint64_t vertexCopiedSinceSwap = 0, vertexReusedSinceSwap = 0;
bool VertexReuse()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_VERTEX_REUSE");
        const bool asked = v != nullptr && v[0] == '1' && v[1] == '\0', enabled = asked && wholeRingVertexBinding;
        if (asked)
            std::fprintf(stderr, enabled ? "[gpu] vertex reuse on (FM4_VERTEX_REUSE=1): a window the slot's ring holds unwritten is not copied again\n"
                                         : "[gpu] FM4_VERTEX_REUSE=1 ignored: the device's storage buffer range does not cover the ring\n");
        return enabled;
    }();
    return on;
}
// FM4 Recomp (M7, vertex reuse across submissions; opt-in FM4_VERTEX_CACHE_MB=<64-1024, a multiple of 8>, with FM4_VERTEX_REUSE=1
// and FM4_WRITE_WATCH=1): vertex windows go to a persistent buffer of their own (vertex_cache.h: chunks of kVertexCacheChunkBytes,
// copy-on-write, a chunk recycled only when every part that reads it is retired) instead of the slot's ring, so a window stays
// reusable across ring laps, parts and flushes while the write watch sees no write to it. Measured: ~79 % of the
// copied vertex bytes of the race were copied in the swap of a ring lap or the next.
constexpr uint64_t kVertexCacheChunkBytes = 8ull << 20;
static_assert(kVertexCacheChunkBytes >= uint64_t(kMaxVertexWindowDwords) * 4, "any vertex window must fit one cache chunk");
uint64_t VertexCacheBytes()  // 0: off
{
    static const uint64_t bytes = [] {
        const char* v = std::getenv("FM4_VERTEX_CACHE_MB");
        if (v == nullptr)
            return uint64_t(0);
        char* end = nullptr;
        const unsigned long mb = std::strtoul(v, &end, 10);
        if (end == v || *end != '\0' || mb < 64 || mb > 1024 || mb % 8 != 0)
            Stop(StopCode::Setup, "renderer: FM4_VERTEX_CACHE_MB=%s is not a size in MB from 64 to 1024, a multiple of 8", v);
        return uint64_t(mb) << 20;
    }();
    return bytes;
}
std::optional<VertexCache> vertexCache;  // set at device setup when FM4_VERTEX_CACHE_MB applies (CreateResources)
Transient vertexCacheBuffer;             // its buffer (vertex_cache.h L1: destroyed only in Shutdown)
uint64_t vertexCacheFlushesSinceSwap = 0, vertexCacheRecyclesAt = 0;
// FM4 Recomp (M7, guest memory for the GPU; opt-in FM4_GUEST_MEMORY_GPU=1 with FM4_WRITE_WATCH=1; guest_memory_vk.h, gpu_read_guard.h):
// a draw whose every vertex window the write watch guards is read by the GPU straight from guest memory (xe_vertex_base = physical
// / 4 in the imported buffer) instead of a copy; the guard makes a later store to those pages wait until the part that reads them
// has completed. Any other draw takes its snapshot (today's copy into the ring or the vertex cache). Per swap: the windows read
// directly and their bytes, the draws snapshotted (and those that fell back after their entry), the command processor's own waits.
uint64_t directWindowsSinceSwap = 0, directBytesSinceSwap = 0, snapshotDrawsSinceSwap = 0, lateSnapshotsSinceSwap = 0;
uint64_t hotSnapshotsSinceSwap = 0, hotLogged = 0, cpGuardWaitsTotal = 0;  // FM4 Recomp (M7, FM4_GUARD_HOT)
uint64_t guardWaitsAt = 0, guardForcedAt = 0, guardWaitNsAt = 0, cpGuardWaitsSinceSwap = 0, cpGuardForcedSinceSwap = 0, cpGuardNsSinceSwap = 0;
bool retiring = false;  // inside a retire's write-backs and deferred stores, where the guard must never need a flush
std::vector<uint64_t> directSequences;  // per window of the current draw: the write-watch sequence its guard was checked at
// FM4 Recomp (opt-in FM4_DESCRIPTOR_CACHE=1, with FM4_VERTEX_REUSE=1): the six buffer bindings are dynamic descriptors at base
// offset 0, so a draw's descriptor set depends only on its layout, its textures (view and sampler per binding) and, for the
// XE_RECT_LIST variant that reads XeIndices, the index range; a set the open slot already holds for the same key is bound again
// with the draw's ring offsets instead of being allocated and written (vkUpdateDescriptorSets,
// vkAllocateDescriptorSets and vkResetDescriptorPool were ~15 % of the command thread, mostly the validation layer). The vertex
// binding needs FM4_VERTEX_REUSE's whole-ring addressing (dynamic offset 0). Xenia and ShiftGlue bind their constant buffers the
// same way (dynamic offsets into an upload buffer). Decided in CreateResources, before any pool or layout exists.
bool descriptorCacheOn = false;
uint64_t descriptorHitsSinceSwap = 0, descriptorMissesSinceSwap = 0;
// FM4 Recomp (opt-in FM4_INDEX_REUSE=1, with FM4_WRITE_WATCH=1): a guest index buffer's host indices (BuildHostIndices) are kept
// with the write watch's sequence they were read under, keyed by every input of the conversion (range, size, endian, count,
// restart, offset, clamp, host primitive); while nothing wrote the range since (GuestRangeUnwritten), a later draw uses them
// instead of reading and converting the guest indices again (~5 % of the command thread). Across slots and
// frames: the indices live on the CPU, not in the ring. Bounded: cleared above kIndexCacheBytes.
bool IndexReuse()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_INDEX_REUSE");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] index reuse on (FM4_INDEX_REUSE=1): converted indices kept while the write watch sees no write\n");
        return enabled;
    }();
    return on;
}
// FM4 Recomp (FM4_VERTEX_REUSE, FM4_INDEX_REUSE): a range found written since it was last watched is hot (a buffer the game
// rewrites every frame); for kHotSwaps swaps it is copied or converted without being watched or cached, so it costs no
// mprotect on this thread and no write fault on the guest's (~1,000 faults, 16-23 ms of fault hook a race
// frame; WatchProtect 3 % of the command thread). Keyed by physical address << 2 | endian (vertices) or the index key.
constexpr uint64_t kHotSwaps = 30;
struct LastWatch { uint32_t physical, bytes; uint64_t sequence, hotUntil; };
std::unordered_map<uint64_t, LastWatch> vertexWatches;
// FM4 Recomp (M7 CPU cost): the index cache's key as a plain struct of the 12 fields BuildHostIndices depends on (it was those 12
// words in a 48-byte std::string: one heap allocation an indexed draw, copied into hotIndices and indexCache again on a miss).
// sizeof stays 48, so the indexCacheBytes accounting is unchanged.
struct IndexKey
{
    uint32_t fields[12];
    bool operator==(const IndexKey& o) const { return std::memcmp(fields, o.fields, sizeof fields) == 0; }
    struct Hasher
    {
        size_t operator()(const IndexKey& k) const
        {
            uint64_t h = 0xcbf29ce484222325ull;  // FNV-1a-64 over the words
            for (const uint32_t f : k.fields)
                h = (h ^ f) * 0x100000001b3ull;
            return size_t(h);
        }
    };
};
static_assert(sizeof(IndexKey) == 48, "the index key's size is counted against kIndexCacheBytes");
std::unordered_map<IndexKey, uint64_t, IndexKey::Hasher> hotIndices;  // index key -> the swap it stays hot until
uint64_t renderSwap = 0, hotSkipsSinceSwap = 0;
bool HotSkip()  // opt-in FM4_HOT_SKIP=1
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_HOT_SKIP");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] hot-range skip on (FM4_HOT_SKIP=1): a vertex or index range written since its last watch is not watched for %llu swaps\n",
                         (unsigned long long)kHotSwaps);
        return enabled;
    }();
    return on;
}
// True when the range should be watched now; marks it hot when it was written since its last watch.
bool WatchWorthIt(LastWatch* last, uint32_t physical, uint32_t bytes)
{
    if (last == nullptr || !HotSkip())
        return true;
    if (renderSwap < last->hotUntil)
        return false;
    if (last->physical == physical && last->bytes >= bytes && !GuestRangeUnwritten(last->physical, last->bytes, last->sequence))
    {
        last->hotUntil = renderSwap + kHotSwaps;
        return false;
    }
    return true;
}
struct CachedIndices { HostIndices host; uint64_t sequence; };
std::unordered_map<IndexKey, CachedIndices, IndexKey::Hasher> indexCache;
uint64_t indexCacheBytes = 0, indexHitsSinceSwap = 0, indexMissesSinceSwap = 0;
constexpr uint64_t kIndexCacheBytes = 128ull << 20;
bool VertexReuseVerify()  // FM4_VERTEX_REUSE_VERIFY=1 (diagnostic): every reused window compared with guest memory; a mismatch stops
{
    static const bool on = [] { const char* v = std::getenv("FM4_VERTEX_REUSE_VERIFY"); return v != nullptr && v[0] == '1' && v[1] == '\0'; }();
    return on;
}
uint64_t cpIdleAt = 0, selfWaitsAt = 0, immediateWaitsAt = 0, memWaitsAt = 0, syncedWaitsAt = 0, memWaitNsAt = 0, immediateInterruptsAt = 0, graceSyncsAt = 0;
uint64_t currentSwap = 0;  // the last swap Present saw (diagnostics)
// FM4 Recomp (M9, Extended View; kernel/extended_view.h), the command processor's thread: the factor of the frame being drawn (1:
// none), set at each swap for the draws that follow it, and how many of its draws were narrowed.
float extendedFrameFactor = 1.0f;
uint64_t extendedNarrowedSinceSwap = 0;
float hdrHudGain = 1.0f;  // FM4_HDR: the HUD draws' colour gain for the frames from the last swap (1: none; hdr_output.h HudGain)
float extendedShownFactor = 1.0f;  // the frame being presented: its stretch, given to the presenter while it is not presenting

// FM4 Recomp (FM4_DIAG_CONSUMER=1, diagnostic): what a "resolve consumer" flush waited for; each distinct one is logged once (at
// most 64): a vertex window or index buffer over a pending range, or a texture (its six fetch-constant words) that is not resident.
// FM4_DIAG_CONSUMER=2: every one tallied by kind (a texture by format, size and tiling; a buffer by size), the tally printed
// every 300 swaps (DiagConsumerReport).
int DiagConsumerMode()
{
    static const int mode = [] {
        const char* v = std::getenv("FM4_DIAG_CONSUMER");
        return v != nullptr && (v[0] == '1' || v[0] == '2') && v[1] == '\0' ? v[0] - '0' : 0;
    }();
    return mode;
}
std::map<std::string, uint64_t> consumerTally;
void DiagConsumerReport(uint64_t swap)
{
    if (DiagConsumerMode() != 2 || swap % 300 != 0 || consumerTally.empty())
        return;
    std::vector<std::pair<uint64_t, std::string>> sorted;
    for (const auto& [key, count] : consumerTally)
        sorted.push_back({count, key});
    std::sort(sorted.rbegin(), sorted.rend());
    std::fprintf(stderr, "[diag consumer] swaps %llu-%llu, waits by kind:", (unsigned long long)(swap - 299), (unsigned long long)swap);
    for (size_t i = 0; i < sorted.size() && i < 16; i++)
        std::fprintf(stderr, "%s %llu x %s", i ? ";" : "", (unsigned long long)sorted[i].first, sorted[i].second.c_str());
    std::fprintf(stderr, "\n");
    consumerTally.clear();
}
void DiagConsumer(const char* what, uint32_t physical, uint32_t bytes, const uint32_t* words)
{
    if (DiagConsumerMode() == 0)
        return;
    if (DiagConsumerMode() == 2)
    {
        char kind[96];
        if (words != nullptr)  // fetch constant fields as draw_state.cpp:494-512 decodes them
            std::snprintf(kind, sizeof kind, "%s format %u %ux%u%s dim %u", what, words[1] & 0x3F, (words[2] & 0x1FFF) + 1, ((words[2] >> 13) & 0x1FFF) + 1,
                          (words[0] >> 31) ? " tiled" : " linear", (words[5] >> 9) & 3);
        else
            std::snprintf(kind, sizeof kind, "%s of %u bytes", what, bytes);
        consumerTally[kind]++;
        return;
    }
    static std::set<std::string> seen;
    char key[160];
    if (words != nullptr)
        std::snprintf(key, sizeof key, "%s %08X %08X %08X %08X %08X %08X", what, words[0], words[1], words[2], words[3], words[4], words[5]);
    else
        std::snprintf(key, sizeof key, "%s at physical %08X, %u bytes", what, physical, bytes);
    if (seen.size() < 64 && seen.insert(key).second)
        std::fprintf(stderr, "[diag consumer] swap %llu: %s waits for a resolve write-back\n", (unsigned long long)currentSwap, key);
}  // FM4_DIAG_THREAD_CPU: runtime::gpu::CpIdleNs at the previous swap
bool ThreadCpuDiag()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DIAG_THREAD_CPU");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] diagnostic FM4_DIAG_THREAD_CPU=1: CPU time per swap by thread role (kernel/thread_cpu.h) and command-processor idle time\n");
        return enabled;
    }();
    return on;
}
void BeginUnitPass(VkCommandBuffer c, const char* kind)  // FM4_DIAG_PASS_TIMES (render_hooks.h beginUnitPass)
{
    unitTiming = -1;
    if (!PassTimes() || passQueriesUsed + 2 > kPassQueries)
        return;
    NotePassJob(passActivity);
    vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, passQueries, passQueriesUsed);
    unitTiming = int(passTimings.size());
    passTimings.push_back({passQueriesUsed, 0, kind, c, passJobs, "unit", true});
    passJobs.clear();
    passQueriesUsed += 2;
}
void EndUnitPass(VkCommandBuffer c)
{
    if (unitTiming < 0)
        return;
    vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, passQueries, passTimings[size_t(unitTiming)].query + 1);
    unitTiming = -1;
}
void EndOpenPass(VkCommandBuffer c)
{
    if (openPass.commands == VK_NULL_HANDLE)
    {
        if (!passTimings.empty())  // FM4_DIAG_PASS_TIMES: a job between passes
            NotePassJob(passActivity);
        return;
    }
    if (c != openPass.commands)
        Stop(StopCode::Setup, "renderer: a render pass is open on another command buffer than the one recorded into");
    ZpdEndSegment();  // FM4_OCCLUSION_QUERIES: a counting segment never outlives its pass
    vkCmdEndRenderPass(c);
    if (openPass.timing >= 0)  // FM4_DIAG_PASS_TIMES
    {
        vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, passQueries, passTimings[size_t(openPass.timing)].query + 1);
        passTimings[size_t(openPass.timing)].ended = passActivity;
        passJobs = passActivity;
    }
    openPass = OpenPass{};
}
uint64_t gpuBusyNsSinceSwap = 0, gpuTimedSinceSwap = 0;
std::timed_mutex queueLock;  // vkQueueSubmit / vkQueuePresentKHR on dev.queue (window mode: two threads)

// Unit 8.
std::optional<GlslangCompiler> compiler;
std::optional<ShaderCache> shaders;
// FM4 Recomp (M7 CPU cost; opt-in FM4_SHADER_MEMO=1): ShaderCache's load memo (shader_cache.h `memo`): a re-load of a translated
// program is answered by an exact comparison of its words instead of the FNV hash (~2,600 loads a race
// frame, ShaderCache::Load 2.6 % of the command processor's thread). Result-identical by construction: a match is byte-identical.
bool ShaderMemo()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_SHADER_MEMO");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[shader] load memo on (FM4_SHADER_MEMO=1): a re-loaded program is matched by its words, not hashed\n");
        return enabled;
    }();
    return on;
}
// FM4 Recomp (M7 CPU cost; opt-in FM4_VARIANT_MEMO): draw_record.h VariantMemo. 1: a held key's variants are reused;
// 2 (verify): every draw builds its defines as without the memo and a held entry that disagrees stops the run. Off (0) with the
// diagnostic FM4_DIAG_ALPHA_QUANT, which adds a define after VariantsFromDrawState that the key does not carry.
int VariantMemoMode()
{
    static const int mode = [] {
        const char* v = std::getenv("FM4_VARIANT_MEMO");
        int m = v != nullptr && (v[0] == '1' || v[0] == '2') && v[1] == '\0' ? v[0] - '0' : 0;
        const char* quant = std::getenv("FM4_DIAG_ALPHA_QUANT");
        if (m != 0 && quant != nullptr && quant[0] != '\0')
        {
            std::fprintf(stderr, "[gpu] variant memo off: FM4_DIAG_ALPHA_QUANT changes the defines after the memo's inputs\n");
            m = 0;
        }
        if (m != 0)
            std::fprintf(stderr, "[gpu] variant memo on (FM4_VARIANT_MEMO=%d): %s\n", m,
                         m == 1 ? "a draw with the inputs of an earlier one reuses its shader variants (no define text built)"
                                : "verify: every draw builds its defines and a held entry that disagrees stops the run");
        return m;
    }();
    return mode;
}
VariantMemo variantMemo{8192};
uint64_t variantMemoHitsSinceSwap = 0, variantMemoMissesSinceSwap = 0, variantMemoVerifiedSinceSwap = 0;
std::optional<PipelineCache> pipelines;
uint64_t pipelinesAtCacheSave = 0;  // FM4_SHADER_CACHE: the pipeline count at the last save
std::atomic<bool> pipelineCacheSaving{false};  // FM4_SHADER_CACHE: a background save holds the pipeline cache (Shutdown waits for it)
ShaderClass allowedClasses = kClassStraight;
SamplerLimits samplerLimits;  // the device's (CreateResources), for PlanDescriptors and the descriptor pools
const EdramHooks* edramHooks = nullptr;
const TextureHooks* textures = nullptr;
struct FramebufferKey
{
    VkRenderPass pass;
    VkImageView color, depth;
    uint32_t width, height;
    bool operator==(const FramebufferKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
    struct Hasher
    {
        size_t operator()(const FramebufferKey& k) const
        {
            const auto* b = reinterpret_cast<const unsigned char*>(&k);
            uint64_t h = 0xcbf29ce484222325ull;
            for (size_t i = 0; i < sizeof k; i++)
                h = (h ^ b[i]) * 0x100000001b3ull;
            return size_t(h);
        }
    };
};
std::unordered_map<FramebufferKey, VkFramebuffer, FramebufferKey::Hasher> framebuffers;

// FM4 Recomp (M9, memory; opt-in FM4_EDRAM_RELEASE_REPLACED=1): the framebuffers over an EDRAM view that is about to be destroyed
// (EdramHooks::releaseReplaced calls this for each replaced target's view) go first, so no cached framebuffer names a dead view (a
// later view could get the same handle). Only at the points where every submission is confirmed complete (EvictIdleTextures').
void ForgetFramebuffersOf(VkImageView view);
bool ReleaseReplacedTargets()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_EDRAM_RELEASE_REPLACED");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[edram] replaced host targets are destroyed once every submission is confirmed complete (FM4_EDRAM_RELEASE_REPLACED=1), not at shutdown\n");
        return enabled;
    }();
    return on;
}

void Check(VkResult result, const char* what)
{
    if (result != VK_SUCCESS)
        Stop(StopCode::Setup, "renderer: %s failed (VkResult %d)", what, int(result));
}

// The real device side of a slot.
class VulkanBackend final : public SubmissionBackend<Transient>
{
public:
    int Submit(uint32_t s) override
    {
        if (const VkResult r = vkResetFences(dev.device, 1, &slots[s].fence); r != VK_SUCCESS)
            return int(r);  // nothing submitted, but the guard treats it as a failed submit (stranded): fail closed
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &slots[s].commands;
        if (!drain::Enabled())
            return int(vkQueueSubmit(dev.queue, 1, &submit, slots[s].fence));
        drain::SubmitBegin(s);  // FM4_DIAG_DRAIN (diagnostic): the submit itself (MoltenVK's Metal encoding)
        const VkResult r = vkQueueSubmit(dev.queue, 1, &submit, slots[s].fence);
        drain::SubmitEnd(s);
        return int(r);
    }
    int WaitFence(uint32_t s, uint64_t timeoutNs) override
    {
        if (!drain::Enabled())
            return int(vkWaitForFences(dev.device, 1, &slots[s].fence, VK_TRUE, timeoutNs));
        drain::FenceWaitBegin(s);  // FM4_DIAG_DRAIN (diagnostic): when the fence was observed signalled
        const VkResult r = vkWaitForFences(dev.device, 1, &slots[s].fence, VK_TRUE, timeoutNs);
        if (r == VK_SUCCESS)
            drain::FenceObserved(s);
        return int(r);
    }
    int FenceStatus(uint32_t s) override { return int(vkGetFenceStatus(dev.device, slots[s].fence)); }
    void Destroy(Transient& t) override
    {
        vkDestroyBuffer(dev.device, t.buffer, nullptr);
        vkFreeMemory(dev.device, t.memory, nullptr);  // implicitly unmapped
        runtime::video::mem::Global().Release(t.ledger);
        t = Transient{};
        transientsDestroyed++;
    }
};

VulkanBackend backend;
// FM4 Recomp (opt-in FM4_ASYNC_SUBMIT=1, async_submit.h): vkQueueSubmit (and MoltenVK's Metal encoding inside it) on a worker thread.
bool AsyncSubmit()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_ASYNC_SUBMIT");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] asynchronous submission on (FM4_ASYNC_SUBMIT=1): vkQueueSubmit on a worker thread, in order; GpuGuard unchanged\n");
        return enabled;
    }();
    return on;
}
std::optional<AsyncSubmitBackend<Transient>> asyncBackend;
// FM4 Recomp (opt-in FM4_ASYNC_RETIRE=1, async_retire.h): a completion worker waits for the parts' fences in
// submission order and does their GPU-side retire work (the GPU read guard's completion, the resolve write-backs, a present part's
// window hand-over) while this thread records; GpuGuard::Retire then waits (bounded) for the worker's verdict. The command stream's
// deferred stores and interrupts stay on this thread (command_processor.h ApplyDeferredStoresThrough; ReapRetired). Refuses
// FM4_RESOLVE_RESIDENT_CHECK=1 (its check reads the EDRAM unit's state at the write-back). Independent of FM4_PARTIAL_RETIRE (a reap
// retires the parts in submission order by itself).
bool AsyncRetire()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_ASYNC_RETIRE");
        const bool asked = v != nullptr && v[0] == '1' && v[1] == '\0', enabled = asked && !ResolveResidentCheck();
        if (asked)
            std::fprintf(stderr, enabled ? "[gpu] asynchronous retire on (FM4_ASYNC_RETIRE=1): a completion worker waits the parts' fences in order and writes their "
                                           "resolves back; stores and interrupts land on the command processor after their part\n"
                                         : "[gpu] FM4_ASYNC_RETIRE=1 ignored: it refuses FM4_RESOLVE_RESIDENT_CHECK=1\n");
        return enabled;
    }();
    return on;
}
std::optional<AsyncRetireBackend<Transient>> retireBackend;
uint64_t presentSwapTag = 0;  // FM4_ASYNC_RETIRE: the swap the next SubmitSlot presents (0: an ordinary part)
uint64_t reapedPartsSinceSwap = 0, earlyStoresSinceSwap = 0, asyncPresentsSinceSwap = 0, workerBlockedNsSinceSwap[size_t(FlushCause::Count)] = {};
std::optional<GpuGuard<Transient, kSlots>> guard;
std::optional<CaptureStore<kCaptureRecords>> captures;
std::optional<CaptureStore<kKeepRecords>> kept;  // FM4_CAPTURE_KEEP_EVERY
std::optional<CaptureStore<kKeepRecords>> keptPre;  // FM4_DIAG_POST_AA_PAIR (diagnostic): the kept swaps before the post AA pass
// FM4 Recomp: PNG captures are off by default (the per-frame ring cost ~5 ms of command-processor time a
// swap: PWL gamma, RGB conversion, PNG). Opt-ins for rendering investigations: FM4_CAPTURE_RING=1 (the last kCaptureRecords
// swaps), FM4_CAPTURE_KEEP_EVERY=<n> (every n-th swap kept), FM4_CAPTURE_FRAMES=<swap>[,<swap>...] (exactly those). The
// self-check's capture is always written (scripts/gpu_run.sh verifies it).
bool captureRing = false;
std::set<uint64_t> captureFrames;
uint64_t keepEvery = 0;

// A refused verdict ends the run: a stranding with its own code (GPU_STOP was written), anything
// else as a setup failure.
void Require(const Verdict& v)
{
    if (!v.ok)
    {
        if (GpuReadGuard* g = GuestMemoryGpuGuard(); g != nullptr && v.stranded)  // FM4_GUEST_MEMORY_GPU: waiting stores stop too
            g->Strand();
        Stop(v.stranded ? StopCode::GpuStranded : StopCode::Setup, "renderer: %s", v.reason.c_str());
    }
}

// A refused draw-time result: the host cannot render this draw faithfully (unimplemented), named by its number.
void RequireDraw(uint64_t draw, const KeyResult& r)
{
    if (!r.ok)
        Stop(StopCode::Unimplemented, "renderer: draw %llu: %s", (unsigned long long)draw, r.reason.c_str());
}
void RequireDraw(uint64_t draw, const Verdict& v)
{
    if (!v.ok)
        Stop(v.stranded ? StopCode::GpuStranded : StopCode::Unimplemented, "renderer: draw %llu: %s", (unsigned long long)draw, v.reason.c_str());
}

// The slot's recorded commands to the queue, under the queue lock (window_present.h LockQueue: bounded).
// FM4 Recomp (FM4_SUBMIT_SPLIT): the slot whose command buffer records now, and the slots submitted since slot 0 opened, in
// submission order (RetireSlot retires them all, in that order).
uint32_t recording = 0;
std::vector<uint32_t> submitted;
std::vector<uint64_t> submittedSerials;  // FM4_PARTIAL_RETIRE: each submitted part's serial (render_hooks.h currentPartSerial)
uint64_t partialRetiresSinceSwap = 0, partialSplitsSinceSwap = 0, splitsAfterResolveSinceSwap = 0;
bool splitAfterResolvePending = false;  // FM4_SPLIT_AFTER_RESOLVE: consumed at the next draw's entry
// FM4 Recomp (opt-in FM4_PARTIAL_RETIRE=1, with FM4_SUBMIT_SPLIT): a draw that reads a pending resolve's guest range retires only
// the submitted parts up to the one that recorded it (and splits the open part only when the resolve is in it), then writes back
// just those parts' resolves, instead of ending the open part and retiring every part of the slot. The required completion point
// of a readback, as ReCarbon's completion thread harvests submissions one by one (runtime/gpu/renderer.cpp:600, :966,
// :9301). GpuGuard's per-part Retire is the same primitive; parts retire in submission order.
// FM4 Recomp (opt-in FM4_SPLIT_AFTER_RESOLVE=1, with FM4_PARTIAL_RETIRE): the open part is ended and submitted right after a
// read-back resolve is recorded (the resolve already ended the render pass, so nothing extra is reloaded), so the resolve reaches
// the GPU at once and its consumer finds its part complete instead of splitting the open part mid-pass (measured:
// the forced splits raised the render encoders a frame 177 -> 192, +2 ms of GPU). ReCarbon submits when a guest thread waits on
// recorded work (runtime/gpu/command_processor.cpp:1120-1123); here the known future reader is the next consumer draw.
bool SplitAfterResolve()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_SPLIT_AFTER_RESOLVE");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] split after resolve on (FM4_SPLIT_AFTER_RESOLVE=1): the open part is submitted right after a read-back resolve\n");
        return enabled;
    }();
    return on;
}
bool PartialRetire()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_PARTIAL_RETIRE");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] partial retire on (FM4_PARTIAL_RETIRE=1): a resolve consumer retires the parts up to its resolve's only\n");
        return enabled;
    }();
    return on;
}
uint64_t drawsInBuffer = 0, splitsSinceSwap = 0, idleSplitsSinceSwap = 0;
void SubmitSlot(uint32_t s)
{
    // FM4_ASYNC_SUBMIT: the worker takes the queue lock around the real submit; the guard's bookkeeping stays here.
    std::unique_lock<std::timed_mutex> queue = asyncBackend ? std::unique_lock<std::timed_mutex>()
                                                            : LockQueue(queueLock, PresenterWaitBoundNs(guard->FenceTimeoutNs()), "submit");
    if (retireBackend)  // FM4_ASYNC_RETIRE: what the completion worker does for this part, fixed before it can reach it
    {
        if (edramHooks != nullptr && edramHooks->handOff != nullptr)
            edramHooks->handOff(currentPartSerial);  // the read-back resolves recorded up to this part
        runtime::gpu::MarkDeferredStoresPart(currentPartSerial);  // the stores queued since the previous part follow this one
        retireBackend->Tag(s, currentPartSerial, presentSwapTag);
    }
    if (drain::Enabled())  // FM4_DIAG_DRAIN (diagnostic): the part handed to the submission (a present part has no draws)
        drain::PartHanded(s, currentPartSerial, presentSwapTag != 0 ? 0 : drawsInBuffer);
    Require(guard->Submit(s));
    submitted.push_back(s);
    submittedSerials.push_back(currentPartSerial);  // FM4_PARTIAL_RETIRE
    currentPartSerial++;
    if (GpuReadGuard* g = GuestMemoryGpuGuard())  // FM4_GUEST_MEMORY_GPU (gpu_read_guard.h G3): handed to the queue
        g->Submitted(currentPartSerial - 1);
}

// FM4_ASYNC_RETIRE (async_retire.h A5): after GpuGuard::Retire confirmed slot `s`, a failed write-back or present of the worker stops,
// named, as RetireSlot's own write-back failure does (not a stranding: the GPU completed).
void RequireRetireJob(uint32_t s)
{
    if (!retireBackend)
        return;
    if (const std::string error = retireBackend->Error(s); !error.empty())
        Stop(StopCode::Unimplemented, "renderer: %s", error.c_str());
}

Transient MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, runtime::video::mem::Kind kind = runtime::video::mem::Kind::Present, const char* label = "present buffer")
{
    Transient b;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    Check(vkCreateBuffer(dev.device, &info, nullptr, &b.buffer), "vkCreateBuffer");
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(dev.device, b.buffer, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        MemoryType(dev, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkAllocateMemory(dev.device, &allocate, nullptr, &b.memory), "vkAllocateMemory");
    b.ledger = runtime::video::mem::Global().Add(kind, allocate.allocationSize, label);
    Check(vkBindBufferMemory(dev.device, b.buffer, b.memory, 0), "vkBindBufferMemory");
    Check(vkMapMemory(dev.device, b.memory, 0, size, 0, &b.mapped), "vkMapMemory");
    return b;
}

void ImageBarrier(VkCommandBuffer c, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                  VkPipelineStageFlags dstStage, VkAccessFlags dstAccess, VkImage image = VK_NULL_HANDLE)  // `image`: the display image unless given
{
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image != VK_NULL_HANDLE ? image : display;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const PassActivity activity("present");  // FM4_DIAG_PASS_TIMES label
    EndOpenPass(c);  // FM4_PASS_MERGE
    vkCmdPipelineBarrier(c, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// FM4 Recomp (M7, opt-in FM4_GPU_PRESENT=1; gpu_present.h): the front buffer from the front-buffer resolve's GPU words instead of the
// CPU detile, red/blue exchange and gamma pass (Present was ~12 % of the command processor's time, "swap 0 x 4.8 ms"
// a frame). Needs FM4_RESOLVE_RESIDENT=1 (the words) and FM4_RESOLVE_GPU_WORDS=1 (the write-back is the words); a front buffer whose
// resolve already landed is presented from them only while the write watch (FM4_WRITE_WATCH=1) shows its range unwritten.
// FM4_GPU_PRESENT_CHECK=1 (diagnostic): every GPU-presented swap is also computed on the CPU after its retire and compared word for
// word with the display image's readback. Lifetime: every object below is created once (InitGpuPresent) and destroyed only in
// Shutdown, after every submission is confirmed complete; the descriptor sets are written once, before any command uses them.
bool GpuPresentAsked()
{
    static const bool on = [] { const char* v = std::getenv("FM4_GPU_PRESENT"); return v != nullptr && v[0] == '1' && v[1] == '\0'; }();
    return on;
}
bool GpuPresentCheck()
{
    static const bool on = [] { const char* v = std::getenv("FM4_GPU_PRESENT_CHECK"); return v != nullptr && v[0] == '1' && v[1] == '\0'; }();
    return on;
}
struct GpuPresentUnit
{
    bool on = false;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[FrontWordsTracker::kEntries] = {};
    Transient words[FrontWordsTracker::kEntries];  // one front buffer's resolve words each (tracker entry i)
    Transient lut, out;                             // BuildPwlLut's tables; the display words
    // FM4 Recomp (M9, FM4_INTERNAL_RES, the display image at the internal resolution): the same per host pixel (the resolves'
    // scaled words, EdramHooks::lastResolveHi), their sets, and per entry the front-buffer rows whose latest resolve had them: the
    // swap is presented at the internal resolution only when every row has (otherwise from the 1x words, enlarged).
    // The scaled words do not go through guest memory, so they need not wait for the tracker's rule that a set of bands retires
    // together (FM4's menus resolve the front buffer in three bands that retire apart: the tracker then presents on the CPU, which at
    // 1x is the same picture, but here would be 720p enlarged). Each band is followed on its own instead: until its part retires it
    // must be the newest pending write over its bytes; once it has, nothing may have written those bytes since (the write watch).
    struct HiBand
    {
        uint32_t row = 0, rows = 0;
        uint64_t serial = 0, sequence = 0;
        bool landed = false;
    };
    struct HiFront
    {
        FrontShape shape;
        uint32_t endian = 0;
        std::vector<HiBand> bands;
    };
    Transient hiWords[FrontWordsTracker::kEntries];
    VkDescriptorSet hiSets[FrontWordsTracker::kEntries] = {};
    HiFront hiFront[FrontWordsTracker::kEntries];
    uint64_t hiCopies = 0;
    FrontWordsTracker tracker;
    uint64_t presented = 0, cpu = 0, copies = 0, checked = 0, mismatches = 0;
    std::map<std::string, uint64_t> fallbacks;  // reason -> swaps presented on the CPU for it
} gpuPresent;

// One line: the swaps presented from the GPU and on the CPU (by reason), the resolve copies, the check's tally.
void ReportGpuPresent(const char* when)
{
    const GpuPresentUnit& u = gpuPresent;
    std::string reasons;
    for (const auto& [why, n] : u.fallbacks)
        reasons += (reasons.empty() ? "" : "; ") + std::to_string(n) + " " + why;
    if (runtime::video::scale::OutputScaled())  // FM4 Recomp (M9): how many display images were truly at the internal resolution
        std::fprintf(stderr, "[renderer] display image %ux%u %s: %llu swap(s) from the scaled front buffer, %llu from the 1x front buffer enlarged; "
                             "%llu scaled front-buffer word copies at resolves\n", displayWidth, displayHeight, when, (unsigned long long)scaledPresents,
                     (unsigned long long)upscaledPresents, (unsigned long long)u.hiCopies);
    std::fprintf(stderr, "[gpu] GPU present %s: %llu swap(s) from the GPU, %llu on the CPU (%s); %llu front-buffer word copies at resolves; check: %llu swap(s), "
                         "%llu word mismatch(es)%s\n", when, (unsigned long long)u.presented, (unsigned long long)u.cpu, reasons.empty() ? "none" : reasons.c_str(),
                 (unsigned long long)u.copies, (unsigned long long)u.checked, (unsigned long long)u.mismatches,
                 GpuPresentCheck() ? "" : " (FM4_GPU_PRESENT_CHECK off)");
}

void DestroyGpuPresent()
{
    if (!GpuPresentAsked())
        return;  // nothing was created
    GpuPresentUnit& u = gpuPresent;
    if (u.on)
        ReportGpuPresent("at shutdown");
    vkDestroyPipeline(dev.device, u.pipeline, nullptr);  // null-safe
    vkDestroyPipelineLayout(dev.device, u.layout, nullptr);
    vkDestroyDescriptorSetLayout(dev.device, u.setLayout, nullptr);
    vkDestroyDescriptorPool(dev.device, u.pool, nullptr);  // frees its sets
    for (Transient& t : u.words)
        if (t.buffer != VK_NULL_HANDLE)
            backend.Destroy(t);
    for (Transient& t : u.hiWords)  // FM4 Recomp (M9)
        if (t.buffer != VK_NULL_HANDLE)
            backend.Destroy(t);
    if (u.lut.buffer != VK_NULL_HANDLE)
        backend.Destroy(u.lut);
    if (u.out.buffer != VK_NULL_HANDLE)
        backend.Destroy(u.out);
    u = GpuPresentUnit{};
}

// FM4 Recomp (M9, memory; opt-in FM4_PRESENT_LAZY=1): a tracked front buffer's word buffers (1x, and scaled with the display image at
// the internal resolution) and its two descriptor sets are made when its first resolve band is adopted, not all three entries at
// start: FM4 presents one front buffer, so two entries' buffers (3.7 MB each at 1x, 14.7 MB more at QHD, 33 MB at 4K) were never
// used. A set is written before any command uses it (the entry's first copy and present come after this call).
bool PresentLazy()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_PRESENT_LAZY");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] GPU present: a front buffer's word buffers are made at its first resolve (FM4_PRESENT_LAZY=1)\n");
        return enabled;
    }();
    return on;
}
// A front buffer's set (the 1x one or the scaled one): its words, the ramp's tables, and the display words over the same byte count.
// Only while no recorded or pending command uses the set (its first writing, or a switch of the internal resolution).
void WriteFrontSet(VkDescriptorSet set, VkBuffer words, VkDeviceSize bytes)
{
    const GpuPresentUnit& u = gpuPresent;
    const VkDescriptorBufferInfo buffers[3] = {{words, 0, bytes}, {u.lut.buffer, 0, kGpuPresentLutWords * 4}, {u.out.buffer, 0, bytes}};
    VkWriteDescriptorSet writes[3] = {};
    for (uint32_t b = 0; b < 3; b++)
    {
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = set;
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &buffers[b];
    }
    vkUpdateDescriptorSets(dev.device, 3, writes, 0, nullptr);
}
// FM4 Recomp (M9, live resolution, step 2): the 1x words and the scaled words are each made when missing (they were made together,
// and only when the 1x words were missing): after a change of the internal resolution an entry can have its 1x words and no
// scaled ones. At one scale the two are still made by the same call, the 1x ones first.
void MakeFrontBuffers(size_t i)
{
    GpuPresentUnit& u = gpuPresent;
    const VkDeviceSize frameBytes = VkDeviceSize(kDisplayWidth) * kDisplayHeight * 4, displayBytes = VkDeviceSize(displayWidth) * displayHeight * 4;
    if (u.words[i].buffer == VK_NULL_HANDLE)
    {
        u.words[i] = MakeBuffer(frameBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);  // stops (9) on an allocation failure
        WriteFrontSet(u.sets[i], u.words[i].buffer, frameBytes);
    }
    if (runtime::video::scale::OutputScaled() && u.hiWords[i].buffer == VK_NULL_HANDLE)
    {
        u.hiWords[i] = MakeBuffer(displayBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        WriteFrontSet(u.hiSets[i], u.hiWords[i].buffer, displayBytes);
    }
}

// Initialize, after the compiler and the EDRAM unit: the pipeline and buffers, or one line naming why every swap stays on the CPU.
void InitGpuPresent()
{
    if (!GpuPresentAsked())
        return;
    GpuPresentUnit& u = gpuPresent;
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev.gpu, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family(families);
    vkGetPhysicalDeviceQueueFamilyProperties(dev.gpu, &families, family.data());
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(dev.gpu, &properties);
    const uint32_t groups = (displayWidth * displayHeight + kGpuPresentGroupSize - 1) / kGpuPresentGroupSize;  // the display image's (FM4 Recomp, M9)
    const char* why = options.mode != Mode::Present && options.mode != Mode::Window ? "not a present or window run"
                      : edramHooks == nullptr || edramHooks->lastResolve == nullptr ? "no EDRAM unit"
                      : !ResolveResident()                                           ? "it needs FM4_RESOLVE_RESIDENT=1 (with FM4_RESOLVE_DEFER=1): the resolve's GPU words"
                      : !ResolveGpuWords()                                           ? "it needs FM4_RESOLVE_GPU_WORDS=1: the write-back is then exactly those words"
                      : dev.queueFamily >= families || !(family[dev.queueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT) ? "the queue has no compute"
                      : properties.limits.maxComputeWorkGroupCount[0] < groups       ? "too many workgroups for one dispatch"
                      : !compiler                                                    ? "no shader compiler"
                                                                                     : nullptr;
    std::string error, warnings;
    std::vector<uint32_t> spirv;
    if (why == nullptr && !compiler->CompileCompute(GpuPresentGlsl(), spirv, warnings, error))
        why = "the present shader did not compile";
    VkShaderModule module = VK_NULL_HANDLE;
    if (why == nullptr)
    {
        VkDescriptorSetLayoutBinding bindings[3] = {};
        for (uint32_t b = 0; b < 3; b++)
            bindings[b] = {b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo set{};
        set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set.bindingCount = 3;
        set.pBindings = bindings;
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuPresentPush)};
        VkPipelineLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &u.setLayout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        VkShaderModuleCreateInfo shader{};
        shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader.codeSize = spirv.size() * 4;
        shader.pCode = spirv.data();
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 * uint32_t(FrontWordsTracker::kEntries)};  // FM4 Recomp (M9): and the scaled sets
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 2 * uint32_t(FrontWordsTracker::kEntries);
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &size;
        if (vkCreateDescriptorSetLayout(dev.device, &set, nullptr, &u.setLayout) != VK_SUCCESS ||
            vkCreatePipelineLayout(dev.device, &layout, nullptr, &u.layout) != VK_SUCCESS ||
            vkCreateShaderModule(dev.device, &shader, nullptr, &module) != VK_SUCCESS ||
            vkCreateDescriptorPool(dev.device, &poolInfo, nullptr, &u.pool) != VK_SUCCESS)
            why = "a Vulkan object could not be created";
        else
        {
            VkComputePipelineCreateInfo pipeline{};
            pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            pipeline.stage.module = module;
            pipeline.stage.pName = "main";
            pipeline.layout = u.layout;
            VkDescriptorSetLayout layouts[FrontWordsTracker::kEntries];
            for (VkDescriptorSetLayout& l : layouts)
                l = u.setLayout;
            VkDescriptorSetAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate.descriptorPool = u.pool;
            allocate.descriptorSetCount = uint32_t(FrontWordsTracker::kEntries);
            allocate.pSetLayouts = layouts;
            if (vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &u.pipeline) != VK_SUCCESS ||
                vkAllocateDescriptorSets(dev.device, &allocate, u.sets) != VK_SUCCESS ||
                (runtime::video::scale::OutputScaled() && vkAllocateDescriptorSets(dev.device, &allocate, u.hiSets) != VK_SUCCESS))
                why = "the compute pipeline or its descriptor sets could not be created";
        }
        vkDestroyShaderModule(dev.device, module, nullptr);  // null-safe; not needed once the pipeline exists
    }
    if (why != nullptr)
    {
        DestroyGpuPresent();  // nothing was used by a command
        std::fprintf(stderr, "[gpu] FM4_GPU_PRESENT=1 ignored: %s%s%s; every swap is presented on the CPU\n", why, error.empty() ? "" : ": ", error.c_str());
        return;
    }
    u.lut = MakeBuffer(kGpuPresentLutWords * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    // FM4 Recomp (M9): the display words at the display image's size; the 1x words fill their first frame's bytes.
    u.out = MakeBuffer(VkDeviceSize(displayWidth) * displayHeight * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    for (size_t i = 0; !PresentLazy() && i < FrontWordsTracker::kEntries; i++)
        MakeFrontBuffers(i);
    u.on = true;
    std::fprintf(stderr, "[gpu] GPU present on (FM4_GPU_PRESENT=1): a front buffer resolves wrote whole (or in bands of rows) is presented from their GPU words (one compute "
                         "pass with the gamma ramp, %zu front buffers tracked); any other swap on the CPU, named%s\n",
                 FrontWordsTracker::kEntries, GpuPresentCheck() ? "; FM4_GPU_PRESENT_CHECK=1: every GPU-presented swap compared with the CPU path" : "");
}

// FM4 Recomp (M9): the guest bytes of a band of rows of a tiled 32 bpp front buffer. A band that starts on a 32-row tile boundary is
// whole 32x32 tiles, row-major: (row / 32) tile rows in, each TilesPerRow x 4 KB (gpu_present.h FrontResolveBand's layout); any other
// band is taken as the whole front buffer (wider than needed, never narrower).
uint32_t HiBandBase(const FrontShape& s, const GpuPresentUnit::HiBand& b) { return b.row % 32 == 0 ? s.base + (b.row / 32) * TilesPerRow(s.pitch) * 4096 : s.base; }
uint32_t HiBandBytes(const FrontShape& s, const GpuPresentUnit::HiBand& b)
{
    return b.row % 32 == 0 ? std::min(FrontBytes(s) - (b.row / 32) * TilesPerRow(s.pitch) * 4096, ((b.rows + 31) / 32) * TilesPerRow(s.pitch) * 4096) : FrontBytes(s);
}

// After every confirmed retire (RetireSlot, RetireUpTo, RetireParts, after GuestWritesRetired*): the front buffers whose resolve
// landed are watched from now on.
void GpuPresentRetired(uint64_t through)
{
    if (!gpuPresent.on)
        return;
    gpuPresent.tracker.OnRetired(through, [](uint32_t physical, uint32_t bytes, uint64_t& sequence) { return WatchGuestRange(physical, bytes, sequence); });
    // FM4 Recomp (M9): a scaled band whose part retired is watched from now on; one that cannot be watched is forgotten.
    for (GpuPresentUnit::HiFront& f : gpuPresent.hiFront)
        for (size_t b = 0; b < f.bands.size();)
        {
            GpuPresentUnit::HiBand& band = f.bands[b];
            if (band.landed || band.serial > through)
                b++;
            else if (WatchGuestRange(HiBandBase(f.shape, band), HiBandBytes(f.shape, band), band.sequence))
                band.landed = true, b++;
            else
                f.bands.erase(f.bands.begin() + long(b));
        }
}

// FM4 Recomp (M9): the tracker entry holding front buffer `f`'s scaled words for every row, each band still the picture's last writer
// (pending and newest, or landed and unwritten since), or -1: the swap is its 1x image enlarged.
int ScaledFrontEntry(const FrontBuffer& fb)
{
    if (!gpuPresent.on || !runtime::video::scale::OutputScaled())
        return -1;
    const FrontShape s = FrontShapeOf(fb);
    for (size_t i = 0; i < FrontWordsTracker::kEntries; i++)
    {
        const GpuPresentUnit::HiFront& f = gpuPresent.hiFront[i];
        if (!(f.shape == s) || f.bands.empty())
            continue;
        uint32_t covered = 0;
        bool current = true;
        for (const GpuPresentUnit::HiBand& band : f.bands)
        {
            covered += band.rows;  // bands never overlap (GpuPresentAdopt removes the ones a new band covers)
            const uint32_t base = HiBandBase(f.shape, band), bytes = HiBandBytes(f.shape, band);
            current = current && (band.landed ? GuestRangeUnwritten(base, bytes, band.sequence) : GuestWritesPendingSerial(base, bytes) == band.serial);
        }
        return current && covered == s.height ? int(i) : -1;
    }
    return -1;
}

void ComputeBarrier(VkCommandBuffer c, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(c, srcStage, dstStage, 0, 1, &b, 0, nullptr, 0, nullptr);
}

// The resolve adoption site (Draw, DrawKind::Resolve; `c` is the command buffer being recorded, checked there): a resolve over a
// tracked front buffer drops its words or, when it writes a band of whole rows (or all of it), copies its words (the EDRAM unit's
// image, TRANSFER_SRC_OPTIMAL, the rectangle at 0,0) into the entry's buffer at the band's first row.
// FM4 Recomp (M9, FM4_INTERNAL_RES): `hiWords` (the same resolve per host pixel, or null) goes into the entry's scaled buffer at the
// band's scaled row, and the band's rows are marked as having scaled words (or not).
void GpuPresentAdopt(VkCommandBuffer c, const ResidentResolve& r, VkImage words, VkImage hiWords)
{
    if (!gpuPresent.on)
        return;
    const FrontWordsTracker::Take t = gpuPresent.tracker.OnResolve(r, currentPartSerial);
    if (t.entry < 0)
        return;
    EndOpenPass(c);  // FM4_PASS_MERGE (the resolve ended any pass; explicit)
    MakeFrontBuffers(size_t(t.entry));  // FM4_PRESENT_LAZY: the entry's buffers at its first band (nothing otherwise)
    // Write after read: a present pass earlier in submission order may still read this entry's buffer (an execution dependency).
    ComputeBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.bufferOffset = VkDeviceSize(t.row) * r.width * 4;  // row-major at the front buffer's width (FrontResolveBand: r.width == it)
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {r.width, r.height, 1};  // bufferRowLength 0: tightly packed rows; row + height <= the front buffer's height
    vkCmdCopyImageToBuffer(c, words, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gpuPresent.words[size_t(t.entry)].buffer, 1, &copy);
    gpuPresent.copies++;
    if (!runtime::video::scale::OutputScaled())
        return;
    using runtime::video::scale::HostEdge, runtime::video::scale::HostExact;
    const uint32_t h = r.hiHalves;
    const bool hi = hiWords != VK_NULL_HANDLE && h > runtime::video::scale::kHalves1x && r.width == kDisplayWidth && HostEdge(r.width, h) == displayWidth &&
                    HostExact(t.row, h) && HostExact(r.height, h) && t.row + r.height <= kDisplayHeight;
    // The bands this resolve covers are superseded (with or without scaled words); with them, it becomes the rows' band.
    GpuPresentUnit::HiFront& front = gpuPresent.hiFront[size_t(t.entry)];
    if (!(front.shape == gpuPresent.tracker.At(size_t(t.entry)).shape) || (!front.bands.empty() && front.endian != r.destEndian))
        front.bands.clear();
    front.shape = gpuPresent.tracker.At(size_t(t.entry)).shape;
    front.bands.erase(std::remove_if(front.bands.begin(), front.bands.end(),
                                     [&](const GpuPresentUnit::HiBand& b) { return b.row < t.row + r.height && t.row < b.row + b.rows; }),
                      front.bands.end());
    if (!hi)
        return;
    front.endian = r.destEndian;
    front.bands.push_back({t.row, r.height, currentPartSerial, 0, false});
    VkBufferImageCopy scaled{};
    scaled.bufferOffset = VkDeviceSize(HostEdge(t.row, h)) * displayWidth * 4;
    scaled.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    scaled.imageExtent = {displayWidth, HostEdge(r.height, h), 1};
    vkCmdCopyImageToBuffer(c, hiWords, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gpuPresent.hiWords[size_t(t.entry)].buffer, 1, &scaled);
    gpuPresent.hiCopies++;
}

// At a swap that produces the display image: the tracked entry whose words are the front buffer's bytes now, or -1 (named, counted).
int GpuPresentChoose(const FrontBuffer& f, uint64_t swap)
{
    if (!gpuPresent.on)
        return -1;
    const FrontShape s = FrontShapeOf(f);
    const size_t i = gpuPresent.tracker.Note(s, swap);
    if ((gpuPresent.presented + gpuPresent.cpu) % 600 == 599)
        ReportGpuPresent(("by swap " + std::to_string(swap)).c_str());
    const char* why = nullptr;
    if (gpuPresent.tracker.Current(i, GuestWritesPendingSerial(s.base, FrontBytes(s)),
                                   [](uint32_t physical, uint32_t bytes, uint64_t sequence) { return GuestRangeUnwritten(physical, bytes, sequence); }, why))
    {
        gpuPresent.presented++;
        return int(i);
    }
    gpuPresent.cpu++;
    if (gpuPresent.fallbacks[why]++ == 0)
        std::fprintf(stderr, "[gpu] GPU present: swap %llu (front buffer %08X) presented on the CPU: %s (first time; counted from now on)\n",
                     (unsigned long long)swap, f.physical, why);
    return -1;
}

// Entry `i`'s words -> the display words (gpuPresent.out), left visible to transfer reads. Outside any render pass.
// FM4 Recomp (M9): `scaled`: from the entry's scaled words, displayWidth x displayHeight of them.
void RecordGpuPresent(VkCommandBuffer c, size_t i, const uint32_t (&lut)[3][1024], bool swapRedBlue, bool scaled)
{
    const uint32_t endian = scaled ? gpuPresent.hiFront[i].endian : gpuPresent.tracker.At(i).endian;
    GpuPresentUnit& u = gpuPresent;
    EndOpenPass(c);  // FM4_PASS_MERGE
    // Write after read: the previous present's dispatch read the tables (an execution dependency).
    ComputeBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdUpdateBuffer(c, u.lut.buffer, 0, kGpuPresentLutWords * 4, &lut[0][0]);  // 12 KB: within vkCmdUpdateBuffer's 65,536 bytes
    // The tables and the entry's words (copied at its resolve, earlier in submission order) visible to the shader; the display words'
    // previous transfer read (the last present's copy into the display image) before this pass writes them.
    ComputeBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    const GpuPresentPush push{scaled ? displayWidth * displayHeight : kDisplayWidth * kDisplayHeight, endian, swapRedBlue ? 1u : 0u};
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, u.pipeline);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, u.layout, 0, 1, scaled ? &u.hiSets[i] : &u.sets[i], 0, nullptr);
    vkCmdPushConstants(c, u.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(c, (push.count + kGpuPresentGroupSize - 1) / kGpuPresentGroupSize, 1, 1);
    ComputeBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
}

// FM4_GPU_PRESENT_CHECK: after the swap's retire, the CPU path's display words from guest memory (the resolve's write-back landed)
// against the readback of the GPU-presented display image.
void GpuPresentCompare(const uint8_t* frontBuffer, const FrontBuffer& f, const runtime::gpu::gamma::Ramp& ramp, uint64_t swap)
{
    const size_t n = size_t(f.width) * f.height;
    std::vector<uint32_t> want(n);
    if (!Detile32(frontBuffer, TiledSize32(f.pitch, f.height), want.data(), f.width, f.height, f.pitch))
        Stop(StopCode::Unimplemented, "renderer: swap %llu: front buffer shorter than its tiled size (GPU present check)", (unsigned long long)swap);
    if (f.swapRedBlue)
        for (uint32_t& p : want)
            p = SwapRedBlue1010102(p);
    runtime::gpu::gamma::ApplyPwlToFrame(want.data(), n, ramp.pwl);
    const uint32_t* got = static_cast<const uint32_t*>(readback.mapped);
    uint64_t bad = 0;
    size_t first = n;
    for (size_t k = 0; k < n; k++)
        if (got[k] != want[k])
            bad++, first = std::min(first, k);
    gpuPresent.checked++, gpuPresent.mismatches += bad;
    if (bad != 0)
        std::fprintf(stderr, "[gpu] GPU present check: swap %llu: %llu of %zu display words differ from the CPU path; first at (%zu, %zu): GPU %08X, CPU %08X\n",
                     (unsigned long long)swap, (unsigned long long)bad, n, first % f.width, first / f.width, got[first], want[first]);
}

// Starts recording slot `s`: only while its previous submission is confirmed complete (idle).
VkCommandBuffer BeginSlot(uint32_t s)
{
    if (guard->SlotState(s) != gpu::SubmissionLifetime<Transient>::State::Idle)
        Stop(StopCode::Setup, "renderer: slot %u is recorded while not idle", s);
    Check(vkResetCommandBuffer(slots[s].commands, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(slots[s].commands, &begin), "vkBeginCommandBuffer");
    if (slots[s].zpdPool != VK_NULL_HANDLE)  // FM4_OCCLUSION_QUERIES: the slot is idle, so its last counts are in; its queries start reset
    {
        ZpdHarvest(s);
        // every query the previous command buffer used (all of them the first time), before this one uses any
        if (const uint32_t reset = std::max(slots[s].zpdToReset, slots[s].zpdUsed))
            vkCmdResetQueryPool(slots[s].commands, slots[s].zpdPool, 0, reset);
        slots[s].zpdUsed = 0, slots[s].zpdToReset = 0;
    }
    return slots[s].commands;
}

// The slot's open command buffer (draws and the swap record into the same submission). The first call
// after the slot's previous submission was retired opens it: this is the one reset point of the slot's
// upload ring and descriptor pool (pipeline_key.h UploadRing: never while the slot is in flight; the
// idle check is BeginSlot's).
VkCommandBuffer OpenSlot(uint32_t s)
{
    Slot& slot = slots[s];
    if (slot.open)
        return slots[recording].commands;  // FM4_SUBMIT_SPLIT: the slot's work continues in this command buffer
    const VkCommandBuffer c = BeginSlot(s);
    recording = s, drawsInBuffer = 0;
    if (slot.timestamps != VK_NULL_HANDLE)  // FM4_GPU_TIMESTAMPS (diagnostic)
    {
        vkCmdResetQueryPool(c, slot.timestamps, 0, 2);
        vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.timestamps, 0);
        slot.timed = true, slot.stamped = false;
    }
    if (PassTimes())  // FM4_DIAG_PASS_TIMES: the slot's pass queries, reset before any of its passes
        vkCmdResetQueryPool(c, passQueries, 0, kPassQueries), passTimings.clear(), passQueriesUsed = 0, passJobs.clear();
    // FM4 Recomp (M7 CPU cost): the ring is not emptied between parts (UploadRing::NewPart; a new lap starts at 0 only here, when
    // less than a quarter of the ring remains): the previous parts' vertex windows stay in the buffer and in vertexCopies for the
    // rest of the lap (Intact), instead of being copied and watched again after each of the ~9 flushes a frame (~120 MB of memcpy
    // and ~7,000 page protections a frame).
    if (slot.allocator->NewPart(std::max(slot.allocator->Capacity() / 4, nextPartNeed)))  // the draw that opens the part must fit
        ringLapsSinceSwap++;  // FM4 Recomp (M7, vertex reuse across submissions): a lap invalidates every vertexCopies entry (Intact)
    nextPartNeed = 0;
    Check(vkResetDescriptorPool(dev.device, slot.descriptors, 0), "vkResetDescriptorPool");
    slot.descriptorCache.clear();
    slot.sets = 0;
    slot.errorDwords.clear(), slot.errorWindows.clear();
    slot.open = true;
    return c;
}

// The slot's submission, confirmed complete (bounded wait), then the xe_error dword of each of its draws
// (shader_glsl.h binding 5: bit 0 a vertex fetch outside its slot's data, bit 1 a half float with
// exponent 31): a set bit stops naming the draw. Read before the next OpenSlot resets the ring.
void RetireSlot(uint32_t s)
{
    // FM4_PARTIAL_RETIRE or FM4_ASYNC_RETIRE's reap (in submission order) may have retired slot s's own part already
    if (!PartialRetire() && !retireBackend && (submitted.empty() || submitted.front() != s))
        Stop(StopCode::Setup, "renderer: slot %u is retired, but it is not the first submission of its work", s);
    const auto retireStart = std::chrono::steady_clock::now();
    for (const uint32_t k : submitted)  // FM4_SUBMIT_SPLIT: every part, in submission order (each a bounded wait)
        Require(guard->Retire(k)), RequireRetireJob(k), ZpdHarvest(k);  // FM4_ASYNC_RETIRE: the worker's verdict, its write-backs done; FM4_OCCLUSION_QUERIES: its counts, before the deferred stores land
    if (drain::Enabled())
        drain::RetireObserved();  // FM4_DIAG_DRAIN (diagnostic): the last part's retire seen by the command processor
    retireFenceNsSinceSwap += NsSince(retireStart);
    if (vertexCache)  // FM4 Recomp (M7, vertex reuse across submissions; vertex_cache.h L4): every part so far is confirmed complete
        vertexCache->Retired(currentPartSerial - 1);
    // FM4 Recomp (FM4_GUEST_MEMORY_GPU): published before the write-backs and deferred stores below (they land after the reads).
    if (GpuReadGuard* g = GuestMemoryGpuGuard())
        g->Completed(currentPartSerial - 1);
    submitted.clear();
    submittedSerials.clear();
    recording = s;
    // The EDRAM unit's read-back resolves of this submission into guest memory (render_hooks.h EdramHooks::retired),
    // before the pending ranges are declared complete.
    const auto writeBackStart = std::chrono::steady_clock::now();
    retiring = true;
    if (edramHooks != nullptr && retireBackend)  // FM4_ASYNC_RETIRE: written back by the worker; their staging is free now
        edramHooks->forget(UINT64_MAX);
    else if (edramHooks != nullptr)
        if (std::string error; !edramHooks->retired(UINT64_MAX, error))
            Stop(StopCode::Unimplemented, "renderer: resolve read-back: %s", error.c_str());
    retireWriteBackNsSinceSwap += NsSince(writeBackStart);
    runtime::gpu::ApplyDeferredStores();  // FM4_DEFER_FENCES: the stream's stores, after the write-backs they follow (none queued otherwise)
    retiring = false;
    GuestWritesRetired();  // the resolves recorded into it wrote guest memory (guest_writes.h; unit 4's one slot)
    GpuPresentRetired(UINT64_MAX);  // FM4_GPU_PRESENT: the front buffers whose resolve landed are watched from here
    Slot& slot = slots[s];
    slot.open = false;
    if (slot.timed && slot.stamped)  // FM4_GPU_TIMESTAMPS (diagnostic): the submission is complete, so both queries are available
    {
        uint64_t t[2] = {};
        if (vkGetQueryPoolResults(dev.device, slot.timestamps, 0, 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] >= t[0])
        {
            gpuBusyNsSinceSwap += uint64_t(double(t[1] - t[0]) * timestampPeriodNs), gpuTimedSinceSwap++;
            if (drain::Enabled())
                drain::GpuSpan(uint64_t(double(t[1] - t[0]) * timestampPeriodNs));  // FM4_DIAG_DRAIN (diagnostic)
        }
    }
    slot.timed = slot.stamped = false;
    if (PassTimes() && passQueriesUsed != 0)  // FM4_DIAG_PASS_TIMES: every part retired, so every query is available
    {
        std::vector<uint64_t> t(passQueriesUsed);
        if (vkGetQueryPoolResults(dev.device, passQueries, 0, passQueriesUsed, t.size() * 8, t.data(), 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            for (size_t i = 0; i < passTimings.size(); i++)
            {
                const PassTiming& p = passTimings[i];
                if (t[p.query + 1] >= t[p.query])
                {
                    const uint64_t ns = uint64_t(double(t[p.query + 1] - t[p.query]) * timestampPeriodNs);
                    if (p.unit)
                    {
                        unitGpuNsSinceSwap += ns;
                        auto& u = unitTally[p.kind];
                        u.first += ns, u.second++;
                    }
                    else
                    {
                        passGpuNsSinceSwap += ns, passTimedSinceSwap++;
                        auto& k = passTally[p.kind];
                        k.first += ns, k.second++;
                        auto& b = passDrawTally[p.kind + " " + DrawBucket(p.draws)];
                        b.first += ns, b.second++;
                    }
                }
                if (!p.unit)
                    passEndTally[p.ended].second++;
                // The GPU time between this pass and the previous one of the same command buffer: the jobs recorded between them.
                const PassTiming* q = i ? &passTimings[i - 1] : nullptr;
                if (q != nullptr && q->commands == p.commands && t[p.query] >= t[q->query + 1])
                {
                    const uint64_t ns = uint64_t(double(t[p.query] - t[q->query + 1]) * timestampPeriodNs);
                    gapGpuNsSinceSwap += ns;
                    auto& g = passGapTally[p.before.empty() ? "nothing" : p.before];
                    g.first += ns, g.second++;
                }
            }
        passTimings.clear(), passQueriesUsed = 0;
    }
    for (const Slot::ErrorDword& e : slot.errorDwords)
    {
        uint32_t error = 0;
        std::memcpy(&error, static_cast<const uint8_t*>(slot.ring.mapped) + e.offset, 4);
        if (error != 0)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: the vertex shader flagged xe_error %u (bit 0: a vertex fetch outside its slot's data; "
                                          "bit 1: a 16-bit float with exponent 31); %s", (unsigned long long)e.draw, error, e.Text(slot.errorWindows.data()).c_str());
    }
    slot.errorDwords.clear(), slot.errorWindows.clear();
}

// FM4_GPU_TIMESTAMPS (diagnostic): the end timestamp of the open slot, recorded last (outside any render pass).
void StampEnd(uint32_t s)
{
    const PassActivity activity("submit");  // FM4_DIAG_PASS_TIMES label
    EndOpenPass(slots[recording].commands);  // FM4_PASS_MERGE: nothing is submitted inside a render pass
    if (slots[s].timed)  // FM4_SUBMIT_SPLIT: into the last part (the start is in the first)
        vkCmdWriteTimestamp(slots[recording].commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slots[s].timestamps, 1), slots[s].stamped = true;
}

// The open slot's recorded work, submitted and confirmed complete (a bounded wait), so that guest memory
// holds what it wrote (resolves) before the CPU reads it. Nothing to do when the slot is not open.
void FlushSlot(uint32_t s, FlushCause cause)
{
    if (!slots[s].open)
        return;
    const auto start = std::chrono::steady_clock::now();
    StampEnd(s);
    Check(vkEndCommandBuffer(slots[recording].commands), "vkEndCommandBuffer");
    SubmitSlot(recording);
    const uint64_t blockedBefore = retireFenceNsSinceSwap;
    RetireSlot(s);  // confirmed completion of every part, then the draws' xe_error dwords
    workerBlockedNsSinceSwap[size_t(cause)] += retireFenceNsSinceSwap - blockedBefore;  // FM4_ASYNC_RETIRE: the wait for the worker
    flushCountSinceSwap[size_t(cause)]++, flushNsSinceSwap[size_t(cause)] += NsSince(start);
}

// FM4 Recomp (opt-in FM4_SUBMIT_SPLIT=<draws>, 16-4096): once the command buffer recording slot 0's work holds that many draws,
// it is ended and submitted without waiting, and the work continues in an idle slot's command buffer, so the GPU executes it
// while the CPU records the rest; the next flush waits only for what is left (ShiftGlue 72b7dd6f vulkan/command_processor.cpp
// :6047-6054 splits every vulkan_async_submission_split_draws draws without waiting; FM4's flushes come every ~200 draws, so the
// split is smaller). Every part uses slot 0's ring and descriptor pool, which are reset only after every part is retired
// (RetireSlot, in submission order); submission order on the one queue keeps the barriers' ordering across parts.
uint32_t SplitDraws()
{
    static const uint32_t n = [] {
        const char* v = std::getenv("FM4_SUBMIT_SPLIT");
        const unsigned long draws = v != nullptr ? std::strtoul(v, nullptr, 10) : 0;
        const uint32_t on = draws >= 16 && draws <= 4096 ? uint32_t(draws) : 0;
        if (v != nullptr && on != 0)
            std::fprintf(stderr, "[gpu] split submission on (FM4_SUBMIT_SPLIT=%u): every %u draws the open work is submitted without waiting\n", on, on);
        else if (v != nullptr)
            std::fprintf(stderr, "[gpu] FM4_SUBMIT_SPLIT=%s ignored: draws must be 16-4096\n", v);
        return on;
    }();
    return n;
}
// FM4 Recomp (opt-in FM4_SPLIT_AT_PASS_END=1, with FM4_SUBMIT_SPLIT): a split waits until no draw render pass is open (a resolve
// or a unit pass ended it), instead of ending the open pass itself: each pass end reloads and stores the attachments on a tile
// GPU ("split submit" ended 8 main-scene passes a frame). A part that reaches four times the
// split size splits anyway.
// FM4 Recomp (opt-in FM4_OVERWRITE_CLAIM=1): a draw that provably overwrites a rectangle of its targets (draw_record.h
// FindOverwriteRect: FM4's clears) claims the tiles inside it without an ownership transfer (edram::Bind cutouts).
bool OverwriteClaim()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_OVERWRITE_CLAIM");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] overwrite claims on (FM4_OVERWRITE_CLAIM=1): a one-rectangle overwriting draw claims its tiles without a transfer\n");
        return enabled;
    }();
    return on;
}
bool DiagOverwrite()
{
    static const bool on = [] { const char* v = std::getenv("FM4_DIAG_OVERWRITE"); return v != nullptr && v[0] == '1' && v[1] == '\0'; }();
    return on;
}
uint64_t overwriteClaimsSinceSwap = 0;
// FM4 Recomp (M7 GPU cost, FM4_CLEAR_DRAWS): opt-in FM4_CLEAR_DRAWS=1 (with FM4_OVERWRITE_CLAIM=1, whose rectangle proof it extends): a
// draw FindOverwriteRect proves and whose clear values the host can read without running its shaders (draw_record.h FindClearDraw:
// FM4's clears, vs b6cb6460159a82b1 + ps 277a2f4a3e7ec198) is not drawn. The EDRAM unit keeps it as a
// pending clear of its targets (render_hooks.h ClearRequest), recorded as vkCmdClearAttachments first in the next pass that touches
// them, as a deferred resolve clear is. Today such a draw costs a one-draw render pass and, when it sits on another framebuffer
// between two passes of the scene (the depth-only clears after the depth ping-pong transfers), ends and reopens them: race
// tallies show "1280x1024 x2 colour 0 depth 130 1 draw" 4-18 a swap and the two 640x1024 x4 one-draw kinds 6 a swap.
bool ClearDraws()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_CLEAR_DRAWS");
        const bool asked = v != nullptr && v[0] == '1' && v[1] == '\0', enabled = asked && OverwriteClaim();
        if (asked)
            std::fprintf(stderr, enabled ? "[gpu] clear draws on (FM4_CLEAR_DRAWS=1): a provable clear-class draw becomes a pending clear of its targets, recorded "
                                           "first in the next pass that touches them\n"
                                         : "[gpu] FM4_CLEAR_DRAWS=1 ignored: it needs FM4_OVERWRITE_CLAIM=1\n");
        return enabled;
    }();
    return on;
}
uint64_t clearDrawsDeferredSinceSwap = 0;
uint64_t pushedSetsSinceSwap = 0;  // FM4 Recomp (FM4_PUSH_DESCRIPTORS): draws whose descriptors were pushed
uint64_t pushTemplatesSinceSwap = 0;  // FM4 Recomp (FM4_PUSH_TEMPLATE): those pushed through the layout's update template
// FM4 Recomp (FM4_PUSH_TEMPLATE): the data block of push_template.h PlanPushTemplate: six buffer infos, then the images in slot order.
struct PushTemplateData
{
    VkDescriptorBufferInfo buffers[kPushBufferBindings];
    VkDescriptorImageInfo images[kTextureBindings];
};
static_assert(offsetof(PushTemplateData, images) == kPushBufferBindings * sizeof(VkDescriptorBufferInfo) &&
                  sizeof(VkDescriptorImageInfo) % alignof(VkDescriptorImageInfo) == 0,
              "PlanPushTemplate's offsets: the images follow the six buffer infos, each image info one stride after the previous");
// FM4 Recomp (FM4_DIAG_TRANSFERS=2, diagnostic): the renderer names each draw for the EDRAM unit's first-use tally (render_hooks.h
// diagDrawSignature); level 1 and off leave it empty.
bool DiagTransferConsumers()
{
    static const bool on = [] { const char* v = std::getenv("FM4_DIAG_TRANSFERS"); return v != nullptr && v[0] == '2' && v[1] == '\0'; }();
    return on;
}
edram::Ownership::SkipStats transferSkipsAtSwap;  // FM4_TRANSFER_SKIP_UNWRITTEN: the unit's counts at the last swap line
bool SplitAtPassEnd()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_SPLIT_AT_PASS_END");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}
// FM4 Recomp (M7, opt-in FM4_SUBMIT_IF_IDLE=<draws>, 4-4096; needs FM4_SUBMIT_SPLIT): SpeedBreaker's submit-when-idle policy
// (SpeedBreaker 312ce69 runtime/gpu/renderer.cpp:10848-10859 SubmitIfIdle, called after each command-ring batch,
// command_processor.cpp:1174; GPL-3.0, technique adapted, code independent): when no submitted part is still running, the open part
// goes out at the next draw entry once it holds <draws> draws (4 x <draws> inside a render pass: ending a pass early stores and
// reloads its targets on a tiled GPU), so the GPU starts while the next part is recorded. Measured: retire fence
// waits 14.4 ms against GPU busy 17.2 ms a race frame (the GPU ran mostly while the command processor waited). A split at a draw
// entry is the existing safe point: fences, interrupts and write-backs keep their part's retire order.
// FM4 Recomp (M7, opt-in FM4_SUBMIT_IF_IDLE_IN_PASS=<1-8>, default 4): submit_policy.h's in-pass factor for the idle split.
uint32_t SubmitIfIdleInPass()
{
    static const uint32_t n = [] {
        const char* v = std::getenv("FM4_SUBMIT_IF_IDLE_IN_PASS");
        const unsigned long f = v != nullptr ? std::strtoul(v, nullptr, 10) : 4;
        const uint32_t on = f >= 1 && f <= 8 ? uint32_t(f) : 4;
        if (v != nullptr)
            std::fprintf(stderr, "[gpu] FM4_SUBMIT_IF_IDLE_IN_PASS=%s: an idle split inside a render pass needs %u x the idle draws\n", v, on);
        return on;
    }();
    return n;
}
uint32_t SubmitIfIdleDraws()
{
    static const uint32_t n = [] {
        const char* v = std::getenv("FM4_SUBMIT_IF_IDLE");
        const unsigned long draws = v != nullptr ? std::strtoul(v, nullptr, 10) : 0;
        const uint32_t on = draws >= 4 && draws <= 4096 ? uint32_t(draws) : 0;
        if (v != nullptr)
            std::fprintf(stderr, on != 0 ? "[gpu] submit when idle on (FM4_SUBMIT_IF_IDLE=%s): the open part is submitted once it holds that many draws "
                                           "while no submitted part runs\n"
                                         : "[gpu] FM4_SUBMIT_IF_IDLE=%s ignored: draws must be 4-4096\n", v);
        return on;
    }();
    return n;
}
// FM4 Recomp (M7, opt-in FM4_SUBMIT_IF_IDLE_DEPTH=<1-4>, default 1): the idle split's queue depth: it fires while fewer than this many
// submitted parts still run (1: none, SpeedBreaker's SubmitIfIdle; 2: one, so the next part is already encoded and queued when the GPU
// finishes the running one, SpeedBreaker's depth with eager writes, renderer.cpp:10848-10859).
uint32_t SubmitIfIdleDepth()
{
    static const uint32_t n = [] {
        const char* v = std::getenv("FM4_SUBMIT_IF_IDLE_DEPTH");
        const unsigned long d = v != nullptr ? std::strtoul(v, nullptr, 10) : 1;
        const uint32_t on = d >= 1 && d <= 4 ? uint32_t(d) : 1;
        if (v != nullptr)
            std::fprintf(stderr, "[gpu] FM4_SUBMIT_IF_IDLE_DEPTH=%s: an idle split while fewer than %u submitted part(s) run\n", v, on);
        return on;
    }();
    return n;
}
// Fewer than SubmitIfIdleDepth() submitted parts still running: the slots but the one being recorded that are neither idle nor
// signalled (nothing retired here). Depth 1: no submitted part still running.
bool GpuQueueEmpty()
{
    uint32_t running = 0;
    for (uint32_t k = 0; k < kSlots; k++)
        if (k != recording && !guard->Idle(k) && !guard->Finished(k))
            running++;
    return running < SubmitIfIdleDepth();
}
void SplitSubmit()
{
    if (SplitDraws() == 0 || !slots[0].open)
        return;
    // FM4_SPLIT_AT_PASS_END: at the next pass boundary; FM4_SUBMIT_IF_IDLE: earlier while the GPU has nothing (submit_policy.h)
    const SplitDecision decision = DecideSplit(drawsInBuffer, SplitDraws(), SplitAtPassEnd(), openPass.commands != VK_NULL_HANDLE, SubmitIfIdleDraws(),
                                               [] { return GpuQueueEmpty(); }, SubmitIfIdleInPass());
    if (!decision.Split())
        return;
    const bool byIdle = decision.byIdle;
    uint32_t next = kSlots;
    for (uint32_t k = 1; k < kSlots && next == kSlots; k++)
        if (k != recording && guard->SlotState(k) == gpu::SubmissionLifetime<Transient>::State::Idle)
            next = k;
    if (next == kSlots)
        return;  // every part slot is in flight: keep recording into this one
    const PassActivity activity("split submit");  // FM4_DIAG_PASS_TIMES label
    EndOpenPass(slots[recording].commands);  // FM4_PASS_MERGE: nothing is submitted inside a render pass
    Check(vkEndCommandBuffer(slots[recording].commands), "vkEndCommandBuffer");
    SubmitSlot(recording);
    BeginSlot(next);
    recording = next, drawsInBuffer = 0;
    splitsSinceSwap++, idleSplitsSinceSwap += byIdle ? 1 : 0;
}

// FM4_PARTIAL_RETIRE: the open part ended and submitted (as SplitSubmit, without its draw-count conditions); false when every part
// slot is in flight (the caller falls back to the full flush).
// Fail closed: a unit records into `c` only while `c` is the command buffer being recorded (a split or flush may have ended and
// submitted an earlier handle: recording into it would be invalid GPU work).
void RequireRecording(VkCommandBuffer c, const char* what)
{
    if (!slots[0].open || c != slots[recording].commands)
        Stop(StopCode::Setup, "renderer: %s would record into a command buffer that is not the one being recorded (ended or submitted)", what);
}
bool ForceSplit()
{
    if (!slots[0].open)
        return false;
    uint32_t next = kSlots;
    for (uint32_t k = 1; k < kSlots && next == kSlots; k++)
        if (k != recording && guard->SlotState(k) == gpu::SubmissionLifetime<Transient>::State::Idle)
            next = k;
    if (next == kSlots)
        return false;
    const PassActivity activity("split submit");  // FM4_DIAG_PASS_TIMES label
    EndOpenPass(slots[recording].commands);
    Check(vkEndCommandBuffer(slots[recording].commands), "vkEndCommandBuffer");
    SubmitSlot(recording);
    BeginSlot(next);
    recording = next, drawsInBuffer = 0;
    splitsSinceSwap++, partialSplitsSinceSwap++;
    return true;
}
// FM4_PARTIAL_RETIRE: the parts up to `serial` confirmed complete and their resolves written back; false (nothing done) when the
// option is off, nothing is pending for that serial, or the open part holds it and cannot be split (the caller flushes fully).
bool RetireUpTo(uint64_t serial, FlushCause cause)
{
    if (!PartialRetire() || serial == 0 || !slots[0].open)
        return false;
    const auto start = std::chrono::steady_clock::now();
    if (serial >= currentPartSerial && !ForceSplit())  // the open part recorded it
        return false;
    const auto retireStart = std::chrono::steady_clock::now();
    while (!submitted.empty() && submittedSerials.front() <= serial)
    {
        Require(guard->Retire(submitted.front()));
        ZpdHarvest(submitted.front());  // FM4_OCCLUSION_QUERIES
        RequireRetireJob(submitted.front());  // FM4_ASYNC_RETIRE
        submitted.erase(submitted.begin());
        submittedSerials.erase(submittedSerials.begin());
    }
    retireFenceNsSinceSwap += NsSince(retireStart);
    workerBlockedNsSinceSwap[size_t(cause)] += NsSince(retireStart);  // FM4_ASYNC_RETIRE: the wait for the worker
    if (vertexCache)  // FM4 Recomp (M7, vertex reuse across submissions; vertex_cache.h L4): parts retire in order, so all up to `serial`
        vertexCache->Retired(std::min(serial, currentPartSerial - 1));
    if (GpuReadGuard* g = GuestMemoryGpuGuard())  // FM4_GUEST_MEMORY_GPU: before the write-backs, as RetireSlot
        g->Completed(std::min(serial, currentPartSerial - 1));
    const auto writeBackStart = std::chrono::steady_clock::now();
    retiring = true;
    if (edramHooks != nullptr && retireBackend)  // FM4_ASYNC_RETIRE: written back by the worker
        edramHooks->forget(serial);
    else if (edramHooks != nullptr)
        if (std::string error; !edramHooks->retired(serial, error))
            Stop(StopCode::Unimplemented, "renderer: resolve read-back (partial retire): %s", error.c_str());
    retiring = false;
    retireWriteBackNsSinceSwap += NsSince(writeBackStart);
    GuestWritesRetiredUpTo(serial);
    GpuPresentRetired(serial);  // FM4_GPU_PRESENT
    flushCountSinceSwap[size_t(cause)]++, flushNsSinceSwap[size_t(cause)] += NsSince(start);
    partialRetiresSinceSwap++;
    return true;
}

// FM4 Recomp (FM4_ASYNC_RETIRE): the submitted parts up to `serial`, retired on this thread in submission order: GpuGuard::Retire
// (the completion worker's verdict: fence, GPU read guard completion, write-backs, present hand-over; bounded), then this thread's
// bookkeeping (vertex cache, the EDRAM unit's pending list and staging, the texture unit's pending ranges). `wait` false: only the
// parts the worker has already retired (no wait at all). Returns the newest serial retired (0: none).
uint64_t RetireParts(uint64_t serial, bool wait)
{
    uint64_t through = 0;
    while (!submitted.empty() && submittedSerials.front() <= serial)
    {
        const uint32_t k = submitted.front();
        if (!wait && !retireBackend->Retired(k))
            break;
        Require(guard->Retire(k));
        RequireRetireJob(k);
        ZpdHarvest(k);  // FM4_OCCLUSION_QUERIES: before the stores of this part land (ReapRetired, RetireSlot)
        through = submittedSerials.front();
        submitted.erase(submitted.begin());
        submittedSerials.erase(submittedSerials.begin());
    }
    if (through == 0)
        return 0;
    if (vertexCache)  // vertex_cache.h L4: parts retire in order
        vertexCache->Retired(through);
    if (edramHooks != nullptr)
        edramHooks->forget(through);
    GuestWritesRetiredUpTo(through);
    GpuPresentRetired(through);  // FM4_GPU_PRESENT
    return through;
}

// FM4_ASYNC_RETIRE (deferred_store_parts.h P5): a store lands early only while no unretired GPU part reads its page (it then needs
// no flush, so nothing nests inside the reap); otherwise it and everything behind it wait for a later reap or the full drain.
bool StoreNeedsNoFlush(uint32_t physical)
{
    GpuReadGuard* const g = GuestMemoryGpuGuard();
    return g == nullptr || g->Plan(g->WriteReader(physical, 4)) == GpuReadGuard::Action::Proceed;
}
bool reaping = false;
}  // namespace

// FM4 Recomp (FM4_ASYNC_RETIRE; command_processor.h SetReapHook): between packets, in WAIT_REG_MEM polls and when the ring is idle (no
// command buffer handle is live at any of them), the parts the worker has retired are retired here without waiting (their slots
// become free for FM4_SUBMIT_SPLIT), then the stores of every part before the oldest unretired one land, in queue order, stopping at
// the first interrupt (deferred_store_parts.h P4: interrupts are raised by the full drain, RetireSlot, as today).
void ReapRetired()
{
    if (!enabled || !retireBackend || retiring || reaping)
        return;
    // Nothing can have changed since the last reap unless the worker retired a part (a drain retires and lands everything itself).
    static uint64_t reapedAt = UINT64_MAX;
    const uint64_t progress = retireBackend->RetiredParts();
    if (progress == reapedAt)
        return;
    reapedAt = progress;
    reaping = true;
    if (RetireParts(UINT64_MAX, false) != 0)
        reapedPartsSinceSwap++;
    const uint64_t oldestUnretired = submitted.empty() ? currentPartSerial : submittedSerials.front();
    retiring = true;  // a flush inside cannot happen (StoreNeedsNoFlush); were it to, FlushForGpuReader stops naming it
    earlyStoresSinceSwap += runtime::gpu::ApplyDeferredStoresThrough(oldestUnretired - 1, StoreNeedsNoFlush);
    retiring = false;
    reaping = false;
}
bool AsyncRetireOn() { return retireBackend.has_value(); }

namespace {

// FM4 Recomp (M9, FM4_POST_AA): everything but the source image (CreateResources). A refusal is named and every swap is presented
// without the pass; nothing made here was used by a command then.
void DestroyPostAa()
{
    PostAaUnit& u = postAa;
    vkDestroyPipeline(dev.device, u.pipeline, nullptr);  // each null-safe
    vkDestroyPipelineLayout(dev.device, u.layout, nullptr);
    vkDestroyDescriptorSetLayout(dev.device, u.setLayout, nullptr);
    vkDestroyDescriptorPool(dev.device, u.pool, nullptr);  // frees its set
    vkDestroySampler(dev.device, u.sampler, nullptr);
    vkDestroyImageView(dev.device, u.sourceView, nullptr);
    vkDestroyImageView(dev.device, u.displayView, nullptr);
    u.pipeline = VK_NULL_HANDLE, u.layout = VK_NULL_HANDLE, u.setLayout = VK_NULL_HANDLE, u.pool = VK_NULL_HANDLE, u.sampler = VK_NULL_HANDLE;
    u.sourceView = u.displayView = VK_NULL_HANDLE, u.set = VK_NULL_HANDLE;
    u.on = false;
}
void InitPostAa()
{
    PostAaUnit& u = postAa;
    if (!u.asked)
        return;
    namespace aa = runtime::video::postaa;
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev.gpu, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family(families);
    vkGetPhysicalDeviceQueueFamilyProperties(dev.gpu, &families, family.data());
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(dev.gpu, &properties);
    const char* why = u.refused != nullptr ? u.refused
                      : dev.queueFamily >= families || !(family[dev.queueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT) ? "the queue has no compute"
                      : properties.limits.maxComputeWorkGroupCount[0] < aa::Groups(displayWidth) ||
                                properties.limits.maxComputeWorkGroupCount[1] < aa::Groups(displayHeight) ? "too many workgroups for one dispatch"
                      : !compiler ? "no shader compiler"
                                  : nullptr;
    std::string error, warnings;
    std::vector<uint32_t> spirv;
    if (why == nullptr && !compiler->CompileCompute(aa::Glsl(), spirv, warnings, error))
        why = "the pass's shader did not compile";
    if (why == nullptr)
    {
        const VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                                          {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo set{};
        set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set.bindingCount = 2;
        set.pBindings = bindings;
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(aa::Push)};
        VkPipelineLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &u.setLayout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        VkShaderModuleCreateInfo shader{};
        shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader.codeSize = spirv.size() * 4;
        shader.pCode = spirv.data();
        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = sizes;
        VkSamplerCreateInfo sampler{};  // linear, clamped, the one mip level
        sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkShaderModule module = VK_NULL_HANDLE;
        view.image = u.source;
        bool ok = vkCreateImageView(dev.device, &view, nullptr, &u.sourceView) == VK_SUCCESS;
        view.image = display;
        ok = ok && vkCreateImageView(dev.device, &view, nullptr, &u.displayView) == VK_SUCCESS &&
             vkCreateSampler(dev.device, &sampler, nullptr, &u.sampler) == VK_SUCCESS &&
             vkCreateDescriptorSetLayout(dev.device, &set, nullptr, &u.setLayout) == VK_SUCCESS &&
             vkCreatePipelineLayout(dev.device, &layout, nullptr, &u.layout) == VK_SUCCESS &&
             vkCreateShaderModule(dev.device, &shader, nullptr, &module) == VK_SUCCESS &&
             vkCreateDescriptorPool(dev.device, &poolInfo, nullptr, &u.pool) == VK_SUCCESS;
        if (ok)
        {
            VkComputePipelineCreateInfo pipeline{};
            pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            pipeline.stage.module = module;
            pipeline.stage.pName = "main";
            pipeline.layout = u.layout;
            VkDescriptorSetAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate.descriptorPool = u.pool;
            allocate.descriptorSetCount = 1;
            allocate.pSetLayouts = &u.setLayout;
            ok = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &u.pipeline) == VK_SUCCESS &&
                 vkAllocateDescriptorSets(dev.device, &allocate, &u.set) == VK_SUCCESS;
        }
        vkDestroyShaderModule(dev.device, module, nullptr);  // null-safe; not needed once the pipeline exists
        if (!ok)
            why = "a Vulkan object of the pass could not be created";
    }
    if (why != nullptr)
    {
        DestroyPostAa();
        std::fprintf(stderr, "[renderer] FM4_POST_AA ignored: %s%s%s; every swap is presented without the pass\n", why, error.empty() ? "" : ": ", error.c_str());
        return;
    }
    const VkDescriptorImageInfo in{u.sampler, u.sourceView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, out{VK_NULL_HANDLE, u.displayView, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t b = 0; b < 2; b++)
    {
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = u.set;
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[b].pImageInfo = b == 0 ? &in : &out;
    }
    vkUpdateDescriptorSets(dev.device, 2, writes, 0, nullptr);
    u.on = true;
    if (u.live)
        std::fprintf(stderr, "[renderer] post-process anti-aliasing can be switched while the game runs (FM4_GRAPHICS_LIVE=1): now %s\n",
                     postAaLevel.load() == 0 ? "off" : u.tuning.name);
    if (postAaLevel.load() != 0)
    std::fprintf(stderr, "[renderer] post-process anti-aliasing on (FM4_POST_AA): one FXAA-style compute pass over the %ux%u display image at every "
                         "presented swap, after the game's own MSAA resolve (adapted from ReCarbon); strength %s (contrast minimum %.4f, relative %.3f, sub-pixel "
                         "weight %.2f)%s\n", displayWidth, displayHeight, u.tuning.name, u.tuning.edgeMin, u.tuning.edgeRelative, u.tuning.subpixel,
                 u.pair ? "; FM4_DIAG_POST_AA_PAIR=1: a kept capture also writes the swap before the pass (pre_N.png)" : "");
}

// The swap's picture is in the pass's source image (TRANSFER_DST_OPTIMAL, written by a transfer): the pass writes the display image
// from it and leaves the display image as an upload would (TRANSFER_DST_OPTIMAL, for RecordReadback). `pre`: also the source's
// readback (FM4_DIAG_POST_AA_PAIR, a kept swap). `timed`: recorded into slot 0 (FM4_DIAG_PASS_TIMES may time it).
void RecordPostAa(VkCommandBuffer c, bool pre, bool timed, const runtime::video::postaa::Tuning& tuning)
{
    PostAaUnit& u = postAa;
    const PassActivity activity("post aa");  // FM4_DIAG_PASS_TIMES label
    if (pre)
    {
        ImageBarrier(c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, u.source);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {displayWidth, displayHeight, 1};
        vkCmdCopyImageToBuffer(c, u.source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, postAaPre.buffer, 1, &copy);
        VkBufferMemoryBarrier host{};
        host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = postAaPre.buffer;
        host.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
        ImageBarrier(c, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, u.source);
    }
    else
        ImageBarrier(c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, u.source);
    // The display image's old contents are not kept: the pass writes every texel.
    ImageBarrier(c, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_SHADER_WRITE_BIT);
    const runtime::video::postaa::Push push = runtime::video::postaa::MakePush(displayWidth, displayHeight, tuning);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, u.pipeline);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, u.layout, 0, 1, &u.set, 0, nullptr);
    vkCmdPushConstants(c, u.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    if (timed)  // FM4_DIAG_PASS_TIMES (diagnostic): the pass's own GPU time, as a unit pass named "post aa" (slot 0's queries only)
        BeginUnitPass(c, "post aa");
    vkCmdDispatch(c, runtime::video::postaa::Groups(displayWidth), runtime::video::postaa::Groups(displayHeight), 1);
    if (timed)
        EndUnitPass(c);
    ImageBarrier(c, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    u.passes++;
}

// The display image (already in TRANSFER_DST_OPTIMAL after `written`) -> the readback buffer, made
// visible to the host. Leaves the display image in TRANSFER_SRC_OPTIMAL (the window's presenter blits from it).
void RecordReadback(VkCommandBuffer c)
{
    ImageBarrier(c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {displayWidth, displayHeight, 1};
    vkCmdCopyImageToBuffer(c, display, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
    VkBufferMemoryBarrier host{};
    host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = readback.buffer;
    host.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
}

// FM4 Recomp (M9, FM4_HDR): the unit's objects, each null-safe. Only with nothing of the unit in flight (Shutdown; a refusal in InitHdr).
void DestroyHdr()
{
    HdrUnit& u = hdrUnit;
    vkDestroyPipeline(dev.device, u.pipeline, nullptr);
    vkDestroyPipelineLayout(dev.device, u.layout, nullptr);
    vkDestroyDescriptorSetLayout(dev.device, u.setLayout, nullptr);
    vkDestroyDescriptorPool(dev.device, u.pool, nullptr);  // frees its set
    vkDestroyImageView(dev.device, u.view, nullptr);
    vkDestroyImage(dev.device, u.image, nullptr);
    vkFreeMemory(dev.device, u.memory, nullptr);
    if (u.state.buffer != VK_NULL_HANDLE)
        backend.Destroy(u.state);
    if (u.result.buffer != VK_NULL_HANDLE)
        backend.Destroy(u.result);
    const bool asked = u.asked;
    u = HdrUnit{};
    u.asked = asked;
}

void CreateHdr(uint64_t swap);

// After the shader compiler exists and before the presenter: FM4_HDR's switches (fail closed), then the unit, or a named refusal
// (the run goes on in standard range).
void InitHdr()
{
    HdrUnit& u = hdrUnit;
    namespace h = runtime::video::hdr;
    std::string error;
    float headroom = 0.0f;
    if (!h::Parse(std::getenv("FM4_HDR"), u.asked, error) ||
        !h::ParseNumber("FM4_HDR_STRENGTH", std::getenv("FM4_HDR_STRENGTH"), h::kStrengthMin, h::kStrengthMax, h::kStrengthDefault, u.strength, error) ||
        !h::ParseNumber("FM4_HDR_HEADROOM", std::getenv("FM4_HDR_HEADROOM"), 1.0f, 16.0f, 0.0f, headroom, error) ||
        !h::ParseNumber("FM4_HDR_LOCAL_BUDGET", std::getenv("FM4_HDR_LOCAL_BUDGET"), 0.0f, 4.0f, h::kLocalBudget, u.budget, error))
        Stop(StopCode::Setup, "renderer: %s", error.c_str());
    const char* liveAsked = std::getenv("FM4_GRAPHICS_LIVE");
    u.live = liveAsked != nullptr && liveAsked[0] == '1' && liveAsked[1] == '\0';
    u.assumed = headroom;
    u.offscreen = options.mode == Mode::Present;
    hdrWanted = u.asked;  // the preference, whatever this run can do (HdrOutputNow gives the reason when it cannot)
    if (!u.asked && !(u.live && (options.mode == Mode::Window || u.assumed != 0.0f)))
    {
        if (u.live && options.mode != Mode::Window)  // a run without a window can never present HDR: the menu then says so, not "restart"
            hdrUnavailable = "no window";
        return;
    }
    VkFormatProperties linearLight{}, encoded{};
    vkGetPhysicalDeviceFormatProperties(dev.gpu, VK_FORMAT_R16G16B16A16_SFLOAT, &linearLight);
    vkGetPhysicalDeviceFormatProperties(dev.gpu, VK_FORMAT_A2B10G10R10_UNORM_PACK32, &encoded);
    const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev.gpu, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family(families);
    vkGetPhysicalDeviceQueueFamilyProperties(dev.gpu, &families, family.data());
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(dev.gpu, &properties);
    const char* why = options.mode != Mode::Window && options.mode != Mode::Present ? "not a window or present run"
                      : u.offscreen && u.assumed == 0.0f ? "no window (an offscreen run of the pass needs FM4_HDR_HEADROOM)"
                      : options.mode == Mode::Window && !dev.swapchainColorspace ? "VK_EXT_swapchain_colorspace is not available"
                      : (linearLight.optimalTilingFeatures & needed) != needed ? "R16G16B16A16_SFLOAT cannot be blitted and stored to on this device"
                      : !(encoded.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ? "the display image's format cannot be a blit source"
                      : dev.queueFamily >= families || !(family[dev.queueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT) ? "the queue has no compute"
                      : properties.limits.maxComputeWorkGroupCount[0] < h::Groups(displayWidth) ||
                                properties.limits.maxComputeWorkGroupCount[1] < h::Groups(displayHeight) ? "too many workgroups for one dispatch"
                      : !compiler ? "no shader compiler"
                                  : nullptr;
    if (why != nullptr)
    {
        hdrUnavailable = why;
        std::fprintf(stderr, "[renderer] %s: %s; standard range\n", u.asked ? "FM4_HDR ignored" : "HDR output cannot be switched on in this run", why);
        return;
    }
    hdrUsable = true;
    std::fprintf(stderr, "[renderer] HDR output: the preference is %s%s; HDR is used where the display supports it, standard range otherwise\n",
                 u.asked ? "Auto" : "Off", u.live ? " and can change while the game runs (FM4_GRAPHICS_LIVE=1)" : "");
}

// Whether the picture is expanded now: the preference is Auto, this run can, and (in a window) the display can.
bool HdrEffective()
{
    return hdrWanted.load(std::memory_order_relaxed) && hdrUsable.load(std::memory_order_relaxed) &&
           (hdrUnit.offscreen || hdrDisplay.load(std::memory_order_relaxed));
}

// The pass's objects, once: at Initialize when HDR is on from the start, otherwise on the command processor's thread at the first
// swap that wants it (nothing of the unit exists before, so nothing is in flight). A failure leaves this run in standard range.
void CreateHdr(uint64_t swap)
{
    HdrUnit& u = hdrUnit;
    namespace h = runtime::video::hdr;
    const auto started = std::chrono::steady_clock::now();
    const char* why = nullptr;
    std::string error, warnings;
    std::vector<uint32_t> spirv;
    if (!compiler->CompileCompute(h::Glsl(), spirv, warnings, error))
        why = "the pass's shader did not compile";
    if (why == nullptr)
    {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        info.extent = {displayWidth, displayHeight, 1};
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        Check(vkCreateImage(dev.device, &info, nullptr, &u.image), "vkCreateImage (HDR)");
        VkMemoryRequirements need;
        vkGetImageMemoryRequirements(dev.device, u.image, &need);
        VkMemoryAllocateInfo memory{};
        memory.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memory.allocationSize = need.size;
        memory.memoryTypeIndex = MemoryType(dev, need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(dev.device, &memory, nullptr, &u.memory), "vkAllocateMemory (HDR)");
        Check(vkBindImageMemory(dev.device, u.image, u.memory, 0), "vkBindImageMemory (HDR)");
        hdrLedger = runtime::video::mem::Global().Add(runtime::video::mem::Kind::Display, need.size, "HDR image (FM4_HDR)");
        const VkDeviceSize stateBytes = 16 + VkDeviceSize(h::kCells) * 8 + VkDeviceSize(h::kCells) * 4;  // the shader's State block (std430)
        u.state = MakeBuffer(stateBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memset(u.state.mapped, 0, size_t(stateBytes));  // before any use: the limiter starts from a dark frame
        if (u.offscreen)
            u.result = MakeBuffer(VkDeviceSize(displayWidth) * displayHeight * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                                          {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo set{};
        set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set.bindingCount = 2;
        set.pBindings = bindings;
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(h::Push)};
        VkPipelineLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &u.setLayout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        VkShaderModuleCreateInfo shader{};
        shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader.codeSize = spirv.size() * 4;
        shader.pCode = spirv.data();
        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = sizes;
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        view.image = u.image;
        VkShaderModule module = VK_NULL_HANDLE;
        bool ok = vkCreateImageView(dev.device, &view, nullptr, &u.view) == VK_SUCCESS &&
                  vkCreateDescriptorSetLayout(dev.device, &set, nullptr, &u.setLayout) == VK_SUCCESS &&
                  vkCreatePipelineLayout(dev.device, &layout, nullptr, &u.layout) == VK_SUCCESS &&
                  vkCreateShaderModule(dev.device, &shader, nullptr, &module) == VK_SUCCESS &&
                  vkCreateDescriptorPool(dev.device, &poolInfo, nullptr, &u.pool) == VK_SUCCESS;
        if (ok)
        {
            VkComputePipelineCreateInfo pipeline{};
            pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            pipeline.stage.module = module;
            pipeline.stage.pName = "main";
            pipeline.layout = u.layout;
            VkDescriptorSetAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate.descriptorPool = u.pool;
            allocate.descriptorSetCount = 1;
            allocate.pSetLayouts = &u.setLayout;
            ok = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &u.pipeline) == VK_SUCCESS &&
                 vkAllocateDescriptorSets(dev.device, &allocate, &u.set) == VK_SUCCESS;
        }
        vkDestroyShaderModule(dev.device, module, nullptr);  // null-safe; not needed once the pipeline exists
        if (!ok)
            why = "a Vulkan object of the pass could not be created";
    }
    if (why != nullptr)
    {
        const HdrUnit kept = u;
        DestroyHdr();  // nothing of it was ever recorded
        u.live = kept.live, u.offscreen = kept.offscreen, u.assumed = kept.assumed, u.strength = kept.strength, u.budget = kept.budget;
        hdrUsable = false;
        hdrUnavailable = why;
        std::fprintf(stderr, "[renderer] FM4_HDR ignored: %s%s%s; standard range\n", why, error.empty() ? "" : ": ", error.c_str());
        return;
    }
    const VkDescriptorImageInfo image{VK_NULL_HANDLE, u.view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo state{u.state.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t b = 0; b < 2; b++)
    {
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = u.set;
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pImageInfo = b == 0 ? &image : nullptr;
        writes[b].pBufferInfo = b == 1 ? &state : nullptr;
    }
    vkUpdateDescriptorSets(dev.device, 2, writes, 0, nullptr);
    u.on = true;
    u.createMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (swap != 0)
        std::fprintf(stderr, "[renderer] HDR output switched on at swap %llu: the pass's objects created in %.1f ms\n", (unsigned long long)swap, u.createMs);
    std::fprintf(stderr, "[renderer] HDR output asked (FM4_HDR): the %ux%u picture decoded to linear light, highlights expanded up to %.2fx SDR white "
                         "(FM4_HDR_STRENGTH) within the display's headroom, local budget %.2f (adapted from SpeedBreaker)%s\n", displayWidth, displayHeight,
                 u.strength, u.budget, u.offscreen ? "; no window: an assumed headroom (FM4_HDR_HEADROOM), the result summarised at each kept capture" : "");
}

// The display image is in TRANSFER_SRC_OPTIMAL (RecordReadback): its encoded values are blitted into the unit's image, which the
// three steps of the pass turn into linear light in place; the image is left in TRANSFER_SRC_OPTIMAL for the window's presenter.
// `readBack` (offscreen): also into the result buffer, made visible to the host.
void RecordHdr(VkCommandBuffer c, bool readBack)
{
    HdrUnit& u = hdrUnit;
    namespace h = runtime::video::hdr;
    ImageBarrier(c, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, u.image);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = blit.dstOffsets[1] = {int32_t(displayWidth), int32_t(displayHeight), 1};
    vkCmdBlitImage(c, display, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    ImageBarrier(c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, u.image);
    // Not in effect (the preference Off, or the window on a display without HDR): a peak of 1, the standard picture in linear
    // light, through the same image and swapchain.
    const float headroom = u.assumed != 0.0f ? u.assumed : hdrHeadroom.load(std::memory_order_relaxed);
    u.lastPeak = HdrEffective() ? h::PeakForHeadroom(headroom, u.strength) : 1.0f;
    h::Push push{int32_t(displayWidth), int32_t(displayHeight), 0, u.lastPeak, u.budget};
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, u.pipeline);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, u.layout, 0, 1, &u.set, 0, nullptr);
    VkMemoryBarrier between{};  // a step reads what the step before wrote (the state buffer; the image only in the last step)
    between.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    between.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    between.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    const uint32_t groups[3][2] = {{h::Groups(h::kCellsX), h::Groups(h::kCellsY)}, {1, 1}, {h::Groups(displayWidth), h::Groups(displayHeight)}};
    for (int step = 0; step < 3; step++)
    {
        if (step != 0)
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &between, 0, nullptr, 0, nullptr);
        push.step = step;
        vkCmdPushConstants(c, u.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
        vkCmdDispatch(c, groups[step][0], groups[step][1], 1);
    }
    ImageBarrier(c, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, u.image);
    if (readBack)
    {
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {displayWidth, displayHeight, 1};
        vkCmdCopyImageToBuffer(c, u.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, u.result.buffer, 1, &copy);
        VkBufferMemoryBarrier host[2] = {};
        for (int i = 0; i < 2; i++)
        {
            host[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            host[i].srcAccessMask = i == 0 ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_WRITE_BIT;
            host[i].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            host[i].srcQueueFamilyIndex = host[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host[i].buffer = i == 0 ? u.result.buffer : u.state.buffer;
            host[i].size = VK_WHOLE_SIZE;
        }
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 2, host, 0, nullptr);
    }
    u.passes++;
}

// Offscreen (FM4_HDR_HEADROOM): the read-back result of a kept swap, after its submission retired, as one line: the limiter's
// state, and the picture's luma (Rec.709, linear, 1.0 = SDR white). Evidence for a comparison with the capture's own values.
void HdrSummary(uint64_t swap)
{
    const HdrUnit& u = hdrUnit;
    const auto half = [](uint16_t v) {  // IEEE 754 binary16
        const int exponent = (v >> 10) & 0x1F, mantissa = v & 0x3FF;
        const float magnitude = exponent == 0 ? std::ldexp(float(mantissa), -24) : exponent == 31 ? (mantissa ? NAN : INFINITY)
                                                                                                  : std::ldexp(float(mantissa | 0x400), exponent - 25);
        return (v & 0x8000) ? -magnitude : magnitude;
    };
    const uint16_t* texel = static_cast<const uint16_t*>(u.result.mapped);
    const float* state = static_cast<const float*>(u.state.mapped);
    const size_t pixels = size_t(displayWidth) * displayHeight;
    double sum = 0.0;
    float highest = 0.0f;
    size_t above = 0, bad = 0;
    for (size_t i = 0; i < pixels; i++)
    {
        const float luma = runtime::video::hdr::Luma709(half(texel[4 * i]), half(texel[4 * i + 1]), half(texel[4 * i + 2]));
        if (!std::isfinite(luma) || luma < 0.0f)
        {
            bad++;
            continue;
        }
        sum += luma, highest = std::max(highest, luma), above += luma > 1.0f ? 1 : 0;
    }
    std::fprintf(stderr, "[hdr] swap %llu: headroom %.2f (assumed), base peak %.3f, frame average %.4f, frame peak %.3f; luma mean %.5f, highest %.4f, "
                         "%.3f %% of the picture above SDR white, %zu invalid value(s)\n", (unsigned long long)swap, u.assumed,
                 u.lastPeak, state[0], state[1], sum / double(pixels), highest, 100.0 * double(above) / double(pixels), bad);
}

// RecordReadback, then ends the recording.
void EndWithReadback(VkCommandBuffer c)
{
    RecordReadback(c);
    Check(vkEndCommandBuffer(c), "vkEndCommandBuffer");
}

// The read-back display image (A2B10G10R10: R bits 0-9) -> a capture: 8-bit RGB keeps each channel's
// top 8 bits (ForzaRecomp presenter.cpp:396-405).
// Whether swap `id`'s front buffer is captured (the ring, a listed frame, or a kept every-Nth frame).
// FM4 Recomp (diagnostic): FM4_CAPTURE_KEEP_FROM_VBLANK=<n> keeps every-Nth frames only from that vblank on (with FM4_CP_TRACE_FROM_VBLANK:
// the 96 kept files cover the traced screen instead of the boot).
bool KeepSwap(uint64_t id)
{
    static const uint64_t from = [] {
        const char* v = std::getenv("FM4_CAPTURE_KEEP_FROM_VBLANK");
        return v != nullptr && *v != '\0' ? std::strtoull(v, nullptr, 10) : 0ull;
    }();
    return kept && id % keepEvery == 0 && (from == 0 || runtime::gpu::VblankCount() >= from);
}
bool FrontBufferCaptured(uint64_t id) { return captureRing || captureFrames.count(id) != 0 || KeepSwap(id); }
void Capture(uint64_t id, const char* what)
{
    const bool frontBuffer = std::strcmp(what, "front buffer") == 0;
    const bool toRing = !frontBuffer || captureRing || captureFrames.count(id) != 0;
    const bool toKept = frontBuffer && KeepSwap(id);
    if (!toRing && !toKept)
        return;  // captures off (the default): no conversion or PNG work for this swap (Present skipped the image as well)
    const uint32_t* pixels = static_cast<const uint32_t*>(readback.mapped);
    std::vector<uint8_t> rgb(size_t(displayWidth) * displayHeight * 3);  // the display image's size (FM4 Recomp, M9: the internal resolution)
    for (size_t i = 0; i < size_t(displayWidth) * displayHeight; i++)
    {
        const uint32_t v = pixels[i];
        rgb[3 * i] = uint8_t((v & 0x3FF) >> 2), rgb[3 * i + 1] = uint8_t(((v >> 10) & 0x3FF) >> 2), rgb[3 * i + 2] = uint8_t(((v >> 20) & 0x3FF) >> 2);
    }
    const std::vector<uint8_t> png = EncodePngRgb8(displayWidth, displayHeight, rgb);
    if (png.empty())  // the encoder refused its input (png.h: a size mismatch or an empty image); never a 0-byte capture
        Stop(StopCode::Setup, "renderer: capture %llu: the PNG encoder refused %zu RGB bytes for %ux%u", (unsigned long long)id, rgb.size(), displayWidth, displayHeight);
    std::string error;
    if (toRing)
    {
        const bool ok = captures->Add(id, png.size(), [&](const std::string& path) {
            FILE* f = std::fopen(path.c_str(), "wb");
            if (f == nullptr)
                return false;
            const bool written = std::fwrite(png.data(), 1, png.size(), f) == png.size();
            return std::fclose(f) == 0 && written;
        }, error);
        if (!ok)
            Stop(StopCode::Setup, "renderer: capture of %s: %s", what, error.c_str());
        std::fprintf(stderr, "[gpu] %s captured: %s (ring %zu files, %llu bytes)\n", what, captures->PathOf(id).c_str(), captures->Count(),
                     (unsigned long long)captures->Bytes());
    }
    if (toKept)
    {
        if (!kept->Add(id, png.size(), [&](const std::string& path) {
                FILE* f = std::fopen(path.c_str(), "wb");
                if (f == nullptr)
                    return false;
                const bool written = std::fwrite(png.data(), 1, png.size(), f) == png.size();
                return std::fclose(f) == 0 && written;
            }, error))
            Stop(StopCode::Setup, "renderer: kept capture of %s: %s", what, error.c_str());
        std::fprintf(stderr, "[gpu] %s kept: %s\n", what, kept->PathOf(id).c_str());
        if (postAa.on && postAa.pair)  // FM4_DIAG_POST_AA_PAIR (diagnostic): the same swap before the pass, beside it
        {
            if (!keptPre)
                keptPre.emplace(options.captureDir, "pre_", kKeepBytes);
            const uint32_t* before = static_cast<const uint32_t*>(postAaPre.mapped);
            for (size_t i = 0; i < size_t(displayWidth) * displayHeight; i++)
            {
                const uint32_t v = before[i];
                rgb[3 * i] = uint8_t((v & 0x3FF) >> 2), rgb[3 * i + 1] = uint8_t(((v >> 10) & 0x3FF) >> 2), rgb[3 * i + 2] = uint8_t(((v >> 20) & 0x3FF) >> 2);
            }
            const std::vector<uint8_t> pre = EncodePngRgb8(displayWidth, displayHeight, rgb);
            if (pre.empty() || !keptPre->Add(id, pre.size(), [&](const std::string& path) {
                    FILE* f = std::fopen(path.c_str(), "wb");
                    if (f == nullptr)
                        return false;
                    const bool written = std::fwrite(pre.data(), 1, pre.size(), f) == pre.size();
                    return std::fclose(f) == 0 && written;
                }, error))
                Stop(StopCode::Setup, "renderer: kept pre-pass capture of %s: %s", what, error.c_str());
        }
    }
}

// The display gamma ramp, applied on the CPU to the detiled front buffer before the upload, so the display image and the
// capture show what the TV would. Xenia applies the PWL ramp to a 2_10_10_10 front buffer at every swap, the guest's
// ramp or its default (vulkan/vulkan_command_processor.cc IssueSwap, use_pwl_gamma_ramp; shaders/apply_gamma_pwl.xesli;
// kernel/gamma_ramp.h), assuming an increment of 8 (apply_gamma_pwl.xesli:78-80, DC_LUTA_CONTROL 3; 0 before the guest
// writes it, register_table.inc:470-478): any other DC_LUTA_CONTROL stops. The ramp is logged when it changes.
uint64_t loggedRampDigest = 0;
bool rampLogged = false;
// `pixels` null: only the register check and the ramp log (M7: nothing displays or captures the frame, see Present).
// FM4 Recomp (FM4_GPU_PRESENT): `rampOut` (when not null) receives the ramp this call checked and logged (the GPU present's tables).
void ApplyDisplayGamma(const uint8_t* base, uint32_t* pixels, size_t count, uint64_t swap, runtime::gpu::gamma::Ramp* rampOut = nullptr)
{
    namespace gamma = runtime::gpu::gamma;
    const uint32_t control = runtime::gpu::ReadRegister(base, gamma::reg::kLutaControl);
    if (control != 0 && control != 3)
        Stop(StopCode::Unimplemented, "renderer: swap %llu: DC_LUTA_CONTROL %08X (only 0 or 3, the PWL increment 8 Xenia assumes)",
             (unsigned long long)swap, control);
    const runtime::gpu::GammaRampView v = runtime::gpu::CurrentGammaRamp();
    const uint64_t digest = gamma::PwlDigest(v.ramp.pwl);
    if (!rampLogged || digest != loggedRampDigest)
    {
        static const uint64_t defaultDigest = gamma::PwlDigest(gamma::DefaultRamp().pwl);
        const char* kind = gamma::IsD3dLinearPwl(v.ramp.pwl) ? "Direct3D 9 linear" : digest == defaultDigest ? "Xenia's default" : "title";
        const gamma::PwlEntry* r = &v.ramp.pwl[0][0];
        std::fprintf(stderr, "[gpu] display gamma ramp: PWL, 128 entries, digest %016llX (%s), from swap %llu; DC_LUTA_CONTROL %08X; "
                     "red base/delta at 00 %04X/%04X, 01 %04X/%04X, 40 %04X/%04X, 7E %04X/%04X, 7F %04X/%04X; ramp-state writes: %llu "
                     "by the command processor, %llu by CPU stores\n",(unsigned long long)digest, kind, (unsigned long long)swap, control,
                     r[0].base, r[0].delta, r[3].base, r[3].delta, r[0x40 * 3].base, r[0x40 * 3].delta, r[0x7E * 3].base, r[0x7E * 3].delta,
                     r[0x7F * 3].base, r[0x7F * 3].delta, (unsigned long long)v.cpWrites, (unsigned long long)v.cpuWrites);
        loggedRampDigest = digest, rampLogged = true;
    }
    if (pixels != nullptr)
        gamma::ApplyPwlToFrame(pixels, count, v.ramp.pwl);
    if (rampOut != nullptr)
        *rampOut = v.ramp;
}

// FM4 Recomp (M9, live resolution, step 1): the 1x display image (display1x: a swap without scaled words is uploaded there and
// enlarged into the display image), unless it exists. Its size is the video mode's at every scale, so one serves them all; this was
// CreateResources' text, where it is still called only when the display image is at the internal resolution (above 1x at start).
void CreateDisplay1x()
{
    if (display1x != VK_NULL_HANDLE)
        return;
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    info.extent = {kDisplayWidth, kDisplayHeight, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    Check(vkCreateImage(dev.device, &info, nullptr, &display1x), "vkCreateImage (1x display)");
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(dev.device, display1x, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(dev, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(dev.device, &allocate, nullptr, &display1xMemory), "vkAllocateMemory (1x display)");
    Check(vkBindImageMemory(dev.device, display1x, display1xMemory, 0), "vkBindImageMemory (1x display)");
    runtime::video::mem::Global().Add(runtime::video::mem::Kind::Display, requirements.size, "1x display image (a swap without scaled words)");
}

void CreateResources()
{
    VkFormatProperties format;
    vkGetPhysicalDeviceFormatProperties(dev.gpu, VK_FORMAT_A2B10G10R10_UNORM_PACK32, &format);
    const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((format.optimalTilingFeatures & needed) != needed)
        Stop(StopCode::Setup, "renderer: A2B10G10R10_UNORM images cannot be transfer sources and destinations");
    // FM4 Recomp (M9, FM4_INTERNAL_RES): the display image at the internal resolution (unless FM4_INTERNAL_OUTPUT=720p), and then the
    // 1x image a swap without scaled words is enlarged from (a LINEAR blit: the format must allow it, checked here, never assumed).
    const bool scaledOut = runtime::video::scale::InternalHalves() > runtime::video::scale::kHalves1x && runtime::video::scale::OutputScaled();
    if (scaledOut)
    {
        displayWidth = runtime::video::scale::HostEdge(kDisplayWidth, runtime::video::scale::InternalHalves());
        displayHeight = runtime::video::scale::HostEdge(kDisplayHeight, runtime::video::scale::InternalHalves());
        const VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((format.optimalTilingFeatures & blit) != blit)
            Stop(StopCode::Setup, "renderer: A2B10G10R10_UNORM images cannot be blitted with a linear filter (FM4_INTERNAL_RES needs it for a swap without "
                                  "scaled words; FM4_INTERNAL_OUTPUT=720p keeps the display image 1280x720)");
    }
    {  // FM4 Recomp (M9, FM4_POST_AA; post_aa.h): asked for, and whether this device and run can (never assumed; InitPostAa names a refusal)
        std::string error;
        if (!runtime::video::postaa::Parse(std::getenv("FM4_POST_AA"), postAa.asked, error, &postAa.tuning))
            Stop(StopCode::Setup, "renderer: %s", error.c_str());
        postAaLevel = !postAa.asked ? 0 : std::strcmp(postAa.tuning.name, "soft") == 0 ? 1 : std::strcmp(postAa.tuning.name, "strong") == 0 ? 3 : 2;
        const char* live = std::getenv("FM4_GRAPHICS_LIVE");
        const char* pairAsked = std::getenv("FM4_DIAG_POST_AA_PAIR");  // the diagnostic pairs every kept swap: not with a pass that can go off
        postAa.live = live != nullptr && live[0] == '1' && live[1] == '\0' && !(pairAsked != nullptr && pairAsked[0] == '1');
        postAa.asked = postAa.asked || postAa.live;
        const VkFormatFeatureFlags pass = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if (postAa.asked)
            postAa.refused = options.mode != Mode::Present && options.mode != Mode::Window ? "not a present or window run"
                             : !dev.storageImageExtendedFormats ? "the device lacks shaderStorageImageExtendedFormats (the pass stores rgb10_a2)"
                             : (format.optimalTilingFeatures & pass) != pass ? "A2B10G10R10_UNORM images cannot be storage images and sampled with a linear filter"
                                                                             : nullptr;
        const char* pair = std::getenv("FM4_DIAG_POST_AA_PAIR");
        postAa.pair = postAa.asked && postAa.refused == nullptr && pair != nullptr && pair[0] == '1' && pair[1] == '\0';
    }
    const bool postAaImages = postAa.asked && postAa.refused == nullptr;
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    info.extent = {displayWidth, displayHeight, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (postAaImages)  // FM4_POST_AA: the pass's source, then the display image as its destination
    {
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | (postAa.pair ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
        Check(vkCreateImage(dev.device, &info, nullptr, &postAa.source), "vkCreateImage (post AA source)");
        VkMemoryRequirements need;
        vkGetImageMemoryRequirements(dev.device, postAa.source, &need);
        VkMemoryAllocateInfo memory{};
        memory.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memory.allocationSize = need.size;
        memory.memoryTypeIndex = MemoryType(dev, need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(dev.device, &memory, nullptr, &postAa.memory), "vkAllocateMemory (post AA source)");
        Check(vkBindImageMemory(dev.device, postAa.source, postAa.memory, 0), "vkBindImageMemory (post AA source)");
        postAaLedger = runtime::video::mem::Global().Add(runtime::video::mem::Kind::Display, need.size, "post AA source image (FM4_POST_AA)");
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    }
    Check(vkCreateImage(dev.device, &info, nullptr, &display), "vkCreateImage");
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // the 1x display image below: as ever
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(dev.device, display, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(dev, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(dev.device, &allocate, nullptr, &displayMemory), "vkAllocateMemory");
    Check(vkBindImageMemory(dev.device, display, displayMemory, 0), "vkBindImageMemory");
    displayLedger = runtime::video::mem::Global().Add(runtime::video::mem::Kind::Display, requirements.size, "display image");
    if (scaledOut)
        CreateDisplay1x();  // FM4 Recomp (M9, live resolution, step 1): the same image, from its own function
    readback = MakeBuffer(VkDeviceSize(displayWidth) * displayHeight * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (postAa.pair)  // FM4_DIAG_POST_AA_PAIR (diagnostic)
        postAaPre = MakeBuffer(VkDeviceSize(displayWidth) * displayHeight * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    // The ring alignments are the device's (never assumed; MoltenVK reports Metal's).
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(dev.gpu, &properties);
    const uint64_t uniformAlign = properties.limits.minUniformBufferOffsetAlignment, storageAlign = properties.limits.minStorageBufferOffsetAlignment;
    wholeRingVertexBinding = uint64_t(properties.limits.maxStorageBufferRange) >= RingBytes();
    // FM4 Recomp (M7, vertex reuse across submissions): the vertex cache needs FM4_VERTEX_REUSE's whole-buffer vertex addressing
    // (xe_vertex_base in dwords from offset 0) and a storage binding that covers the cache. Blocks are 256-byte aligned (the
    // largest storage offset alignment Vulkan allows), as the ring's.
    if (const uint64_t bytes = VertexCacheBytes(); bytes != 0)
    {
        if (VertexReuse() && uint64_t(properties.limits.maxStorageBufferRange) >= bytes)
        {
            vertexCacheBuffer = MakeBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, runtime::video::mem::Kind::VertexCache, "vertex cache buffer");
            vertexCache.emplace(bytes, kVertexCacheChunkBytes, 256);
            if (!vertexCache->Valid())
                Stop(StopCode::Setup, "renderer: the vertex cache's layout (%llu bytes, %llu-byte chunks) is not valid", (unsigned long long)bytes,
                     (unsigned long long)kVertexCacheChunkBytes);
            std::fprintf(stderr, "[gpu] vertex cache on (FM4_VERTEX_CACHE_MB=%llu): %u chunks of %llu MB; vertex windows outlive ring laps and parts\n",
                         (unsigned long long)(bytes >> 20), vertexCache->Chunks(), (unsigned long long)(kVertexCacheChunkBytes >> 20));
        }
        else
            std::fprintf(stderr, "[gpu] FM4_VERTEX_CACHE_MB ignored: it needs FM4_VERTEX_REUSE=1 and a storage buffer range of %llu bytes (%u)\n",
                         (unsigned long long)bytes, properties.limits.maxStorageBufferRange);
    }
    InitGuestMemoryGpu(dev, guard->FenceTimeoutNs());  // FM4 Recomp (M7, FM4_GUEST_MEMORY_GPU): one line names the result; nothing otherwise
    if (const char* v = std::getenv("FM4_DESCRIPTOR_CACHE"); v != nullptr && v[0] == '1' && v[1] == '\0')
    {
        descriptorCacheOn = VertexReuse() && properties.limits.maxDescriptorSetUniformBuffersDynamic >= 3 && properties.limits.maxDescriptorSetStorageBuffersDynamic >= 3;
        std::fprintf(stderr, descriptorCacheOn ? "[gpu] descriptor cache on (FM4_DESCRIPTOR_CACHE=1): dynamic buffer offsets, a set reused per layout and textures\n"
                                               : "[gpu] FM4_DESCRIPTOR_CACHE=1 ignored: it needs FM4_VERTEX_REUSE=1 and 3 dynamic uniform and storage buffers a set (%u, %u)\n",
                     properties.limits.maxDescriptorSetUniformBuffersDynamic, properties.limits.maxDescriptorSetStorageBuffersDynamic);
    }
    // The sampler budgets of the per-pair layouts (pipeline_key.h SamplerLimits), queried, never assumed.
    samplerLimits.perStageSamplers = properties.limits.maxPerStageDescriptorSamplers;
    samplerLimits.perStageSampledImages = properties.limits.maxPerStageDescriptorSampledImages;
    samplerLimits.perStageResources = properties.limits.maxPerStageResources;
    samplerLimits.setSamplers = properties.limits.maxDescriptorSetSamplers;
    samplerLimits.setSampledImages = properties.limits.maxDescriptorSetSampledImages;
    std::fprintf(stderr, "[gpu] sampler limits: per stage %u samplers, %u sampled images, %u resources; per set %u samplers, %u sampled images\n",
                 samplerLimits.perStageSamplers, samplerLimits.perStageSampledImages, samplerLimits.perStageResources, samplerLimits.setSamplers,
                 samplerLimits.setSampledImages);
    if (uniformAlign == 0 || (uniformAlign & (uniformAlign - 1)) || storageAlign == 0 || (storageAlign & (storageAlign - 1)))
        Stop(StopCode::Setup, "renderer: buffer offset alignments %llu / %llu are not powers of two", (unsigned long long)uniformAlign,
             (unsigned long long)storageAlign);
    endOpenRenderPass = EndOpenPass;  // FM4_PASS_MERGE: the units end an open pass before recording outside one (render_hooks.h)
    beginUnitPass = BeginUnitPass, endUnitPass = EndUnitPass;  // FM4_DIAG_PASS_TIMES
    if (GpuTimestamps())  // FM4 Recomp (diagnostic): only with timestamp support on the queue family
    {
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev.gpu, &families, nullptr);
        std::vector<VkQueueFamilyProperties> family(families);
        vkGetPhysicalDeviceQueueFamilyProperties(dev.gpu, &families, family.data());
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(dev.gpu, &properties);
        if (dev.queueFamily < families && family[dev.queueFamily].timestampValidBits > 0 && properties.limits.timestampPeriod > 0)
        {
            timestampPeriodNs = properties.limits.timestampPeriod;
            std::fprintf(stderr, "[gpu] diagnostic FM4_GPU_TIMESTAMPS=1: GPU time per submission (period %.3f ns, %u valid bits)\n", timestampPeriodNs,
                         family[dev.queueFamily].timestampValidBits);
        }
        else
            std::fprintf(stderr, "[gpu] diagnostic FM4_GPU_TIMESTAMPS=1 refused: the queue family has no timestamps\n");
    }
    for (Slot& s : slots)
    {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = dev.queueFamily;
        Check(vkCreateCommandPool(dev.device, &poolInfo, nullptr, &s.pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cb.commandPool = s.pool;
        cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(dev.device, &cb, &s.commands), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        Check(vkCreateFence(dev.device, &fence, nullptr, &s.fence), "vkCreateFence");
        if (dev.occlusionQueryPrecise)  // FM4_OCCLUSION_QUERIES: every slot records draws or may (the part slots, the present slot)
        {
            VkQueryPoolCreateInfo occlusion{};
            occlusion.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            occlusion.queryType = VK_QUERY_TYPE_OCCLUSION;
            occlusion.queryCount = ZpdUnit::kQueries;
            Check(vkCreateQueryPool(dev.device, &occlusion, nullptr, &s.zpdPool), "vkCreateQueryPool (occlusion)");
            s.zpdToReset = ZpdUnit::kQueries, s.zpdUsed = 0, s.zpdSegments.clear();
        }
        if (&s != &slots[0])
            continue;  // FM4_SUBMIT_SPLIT: a part slot has a command buffer and a fence only
        if (timestampPeriodNs > 0)  // FM4_GPU_TIMESTAMPS (diagnostic)
        {
            VkQueryPoolCreateInfo query{};
            query.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            query.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query.queryCount = 2;
            Check(vkCreateQueryPool(dev.device, &query, nullptr, &s.timestamps), "vkCreateQueryPool");
            if (const char* v = std::getenv("FM4_DIAG_PASS_TIMES"); v != nullptr && v[0] == '1' && v[1] == '\0')  // FM4_DIAG_PASS_TIMES
            {
                query.queryCount = kPassQueries;
                Check(vkCreateQueryPool(dev.device, &query, nullptr, &passQueries), "vkCreateQueryPool");
                std::fprintf(stderr, "[gpu] diagnostic FM4_DIAG_PASS_TIMES=1: GPU time of each draw render pass (%u queries a slot)\n", kPassQueries);
            }
        }
        // Unit 8: the ring (uniform, storage and index data) and the descriptor pool, persistent per slot.
        // Unit 7: also the staging source of texture uploads (vkCmdCopyBufferToImage, texture_vk.cpp).
        s.ring = MakeBuffer(RingBytes(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, runtime::video::mem::Kind::Ring, "slot upload ring");
        s.allocator.emplace(RingBytes(), uniformAlign, storageAlign);
        const VkDescriptorPoolSize sizes[] = {
            {descriptorCacheOn ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 * kDrawsPerSlot},
            {descriptorCacheOn ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kDrawsPerSlot},
            // A set holds only its pair's samplers (per-pair layouts), at most min(32, the set budget) of them: the worst case per set.
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, std::min<uint32_t>(kTextureBindings, std::min(samplerLimits.setSamplers, samplerLimits.setSampledImages)) * kDrawsPerSlot},
        };
        VkDescriptorPoolCreateInfo dp{};
        dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dp.maxSets = kDrawsPerSlot;
        dp.poolSizeCount = uint32_t(std::size(sizes));
        dp.pPoolSizes = sizes;
        Check(vkCreateDescriptorPool(dev.device, &dp, nullptr, &s.descriptors), "vkCreateDescriptorPool");
    }
}

// Device teardown, only when nothing's completion is unknown. A kept resource keeps the device too
// (destroying it would free memory the GPU may still use): the run stops with it reported.
void Shutdown()
{
    if (runtime::video::mem::Wanted())  // FM4_DIAG_MEMORY (diagnostic): before anything is destroyed
        std::fputs(runtime::video::mem::Global().Report("at shutdown", 14).c_str(), stderr);
    if (const size_t kept = guard->Shutdown(); kept != 0)
        Stop(StopCode::GpuStranded, "renderer: %zu resource(s) kept at shutdown (completion unknown); the device is not destroyed", kept);
    if (retireBackend)  // FM4_ASYNC_RETIRE: every part retired above; join the worker before the submitter, the EDRAM unit and the device go
    {
        retireBackend->Stop();
        std::fprintf(stderr, "[gpu] asynchronous retire: %llu part(s) through the completion worker\n", (unsigned long long)retireBackend->Submits());
    }
    if (asyncBackend)  // FM4_ASYNC_SUBMIT: every submission confirmed above; the worker is idle: join it before anything is destroyed
    {
        asyncBackend->Stop();
        std::fprintf(stderr, "[gpu] asynchronous submission: %llu submission(s), queue depth at most %zu\n", (unsigned long long)asyncBackend->Submits(),
                     asyncBackend->MaxQueueDepth());
    }
    for (auto& [key, fb] : framebuffers)  // before the render passes and the EDRAM unit's views they reference
        vkDestroyFramebuffer(dev.device, fb, nullptr);
    framebuffers.clear();
    if (edramHooks != nullptr)  // its images, views and pipelines: after the framebuffers over its views, before the device
        edramHooks->shutdown();
    edramHooks = nullptr;
    endOpenRenderPass = nullptr, beginUnitPass = nullptr, endUnitPass = nullptr, openPass = OpenPass{};
    while (pipelineCacheSaving.load())  // FM4_SHADER_CACHE: a background save still reads the pipeline cache
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (pipelines && pipelines->PersistentCacheOn())
        pipelines->SavePersistentCache();  // the last pipelines of the run (ReCarbon saves at exit too)
    pipelines.reset();  // pipelines, render passes, modules, layouts
    shaders.reset();
    variantMemo.Clear();  // FM4_VARIANT_MEMO: its keys and entries point into the cache just destroyed
    compiler.reset();
    textures = nullptr;
    ShutdownTextureUnit();  // views, samplers, images: every submission that used them is confirmed complete (above)
    for (Slot& s : slots)
    {
        vkDestroyDescriptorPool(dev.device, s.descriptors, nullptr);
        backend.Destroy(s.ring);
        vkDestroyFence(dev.device, s.fence, nullptr);
        vkDestroyQueryPool(dev.device, s.timestamps, nullptr);  // FM4_GPU_TIMESTAMPS: every submission confirmed complete (above)
        vkDestroyQueryPool(dev.device, s.zpdPool, nullptr), s.zpdPool = VK_NULL_HANDLE, s.zpdSegments.clear();  // FM4_OCCLUSION_QUERIES: as above
        if (&s == &slots[0])
            vkDestroyQueryPool(dev.device, passQueries, nullptr), passQueries = VK_NULL_HANDLE;  // FM4_DIAG_PASS_TIMES
        s.timestamps = VK_NULL_HANDLE, s.timed = s.stamped = false;
        vkDestroyCommandPool(dev.device, s.pool, nullptr);  // frees its command buffer
        s.allocator.reset();
        s.descriptors = VK_NULL_HANDLE, s.fence = VK_NULL_HANDLE, s.commands = VK_NULL_HANDLE, s.pool = VK_NULL_HANDLE;
        s.open = false;
        s.errorDwords.clear(), s.errorWindows.clear();
    }
    backend.Destroy(readback);
    DestroyGpuPresent();  // FM4 Recomp (FM4_GPU_PRESENT): every submission confirmed complete above
    if (hdrUnit.asked || hdrUnit.live)  // FM4 Recomp (M9, FM4_HDR): the same guarantee
    {
        if (hdrUnit.passes != 0)
            std::fprintf(stderr, "[renderer] HDR output: %llu pass(es)\n", (unsigned long long)hdrUnit.passes);
        DestroyHdr();
    }
    if (postAa.asked)  // FM4 Recomp (M9, FM4_POST_AA): the same guarantee
    {
        if (postAa.on)
            std::fprintf(stderr, "[renderer] post-process anti-aliasing: %llu pass(es)\n", (unsigned long long)postAa.passes);
        DestroyPostAa();
        vkDestroyImage(dev.device, postAa.source, nullptr);  // null-safe
        vkFreeMemory(dev.device, postAa.memory, nullptr);
        if (postAaPre.buffer != VK_NULL_HANDLE)
            backend.Destroy(postAaPre);
        postAa = PostAaUnit{};
    }
    if (vertexCacheBuffer.buffer != VK_NULL_HANDLE)  // FM4 Recomp (M7, vertex reuse across submissions; L1): every submission confirmed above
        backend.Destroy(vertexCacheBuffer);
    vertexCache.reset();
    ShutdownGuestMemoryGpu(dev);  // FM4 Recomp (M7, FM4_GUEST_MEMORY_GPU): every submission confirmed above; the host view stays mapped
    vkDestroyImage(dev.device, display, nullptr);
    vkFreeMemory(dev.device, displayMemory, nullptr);
    vkDestroyImage(dev.device, display1x, nullptr);  // null-safe (FM4 Recomp, M9)
    vkFreeMemory(dev.device, display1xMemory, nullptr);
    display1x = VK_NULL_HANDLE, display1xMemory = VK_NULL_HANDLE;
    if (!runtime::video::keepbusy::Shutdown())  // FM4_GPU_KEEP_BUSY: its own device, before the instance goes
        Stop(StopCode::GpuStranded, "renderer: the keep-busy experiment's submission never completed (completion unknown); nothing is destroyed");
    DestroyVulkanDevice(dev);
    enabled = false;
}

uint64_t NowNs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// FM4_SELFCHECK_EDRAM: unset, or "depth4x2x" (RunSelfCheck step 6, the 4x -> 2x depth transfer); anything else stops.
bool EdramSelfCheckRequested()
{
    const char* v = std::getenv("FM4_SELFCHECK_EDRAM");
    if (v == nullptr || *v == 0)
        return false;
    if (std::strcmp(v, "depth4x2x") != 0)
        Stop(StopCode::Setup, "renderer: FM4_SELFCHECK_EDRAM=%s: only depth4x2x is defined", v);
    return true;
}

void Expect(bool ok, const char* what)
{
    std::fprintf(stderr, "[gpu self-check] %s %s\n", ok ? "pass" : "FAIL", what);
    if (!ok)
        Stop(StopCode::Setup, "renderer: self-check failed: %s", what);
}

// ---- Unit 8 helpers -------------------------------------------------------------------------------

uint32_t ReadRegisterHook(const void* context, uint32_t index) { return runtime::gpu::ReadRegister(static_cast<const uint8_t*>(context), index); }

// A guest index buffer element as the GPU loads it (little-endian, before the DMA endian swap in
// draw_record.cpp SwapIndex). The buffer's range was checked against physical memory by DrawState; the
// view that holds it is found per element (PhysicalToGuest).
struct IndexSource
{
    const uint8_t* base;
    const IndexBuffer* indices;
    uint64_t draw;
    const uint8_t* direct = nullptr;  // FM4 Recomp (M7): the whole buffer in one view (PhysicalRangeToGuest), element 0 here
};
uint32_t ReadGuestIndex(const void* context, uint32_t i)
{
    const IndexSource& s = *static_cast<const IndexSource*>(context);
    const uint32_t size = s.indices->is32Bit ? 4 : 2, physical = s.indices->physical + i * size;
    if (s.direct != nullptr && uint64_t(i + 1) * size <= s.indices->bytes)  // one lookup per draw instead of a locked one per element
    {
        uint32_t raw = 0;
        std::memcpy(&raw, s.direct + uint64_t(i) * size, size);
        return raw;
    }
    uint32_t guest = 0;
    if (!runtime::PhysicalToGuest(physical, guest))
        Stop(StopCode::Unimplemented, "renderer: draw %llu: index %u at physical %08X is in no view", (unsigned long long)s.draw, i, physical);
    uint32_t raw = 0;
    std::memcpy(&raw, s.base + guest, size);  // the GPU's little-endian element load
    return raw;
}

// The framebuffer of a render pass over the EDRAM unit's views (one per distinct set for the run; the
// views live for the run, render_hooks.h).
void ForgetFramebuffersOf(VkImageView view)
{
    for (auto it = framebuffers.begin(); it != framebuffers.end();)
        if (it->first.color == view || it->first.depth == view)
        {
            vkDestroyFramebuffer(dev.device, it->second, nullptr);
            it = framebuffers.erase(it);
        }
        else
            ++it;
}

VkFramebuffer Framebuffer(VkRenderPass pass, const HostTargets& t, bool color, bool depth)
{
    const FramebufferKey key{pass, color ? t.color : VK_NULL_HANDLE, depth ? t.depth : VK_NULL_HANDLE, t.width, t.height};
    if (const auto it = framebuffers.find(key); it != framebuffers.end())
        return it->second;
    VkImageView views[2];
    uint32_t count = 0;
    if (color)
        views[count++] = t.color;
    if (depth)
        views[count++] = t.depth;
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = pass;
    info.attachmentCount = count;
    info.pAttachments = views;
    info.width = t.width;
    info.height = t.height;
    info.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    Check(vkCreateFramebuffer(dev.device, &info, nullptr, &fb), "vkCreateFramebuffer");
    framebuffers[key] = fb;
    return fb;
}

// One block of the draw in the slot's ring; exhaustion stops (fail closed: no wrap, no silent drop).
uint64_t Allocate(Slot& s, uint64_t draw, uint64_t bytes, bool uniform, const char* what)
{
    uint64_t offset = 0;
    if (!s.allocator->Allocate(bytes, uniform, offset))
        Stop(StopCode::Unimplemented, "renderer: draw %llu: the slot's upload ring (%llu bytes) cannot hold %llu more bytes for %s after %llu allocations "
                                      "(unit 5 submits within a frame)", (unsigned long long)draw, (unsigned long long)RingBytes(), (unsigned long long)bytes,
             what, (unsigned long long)s.allocator->Allocations());
    return offset;
}

}  // namespace

gpu::SubmissionGate& Submissions() { return gate; }
// FM4 Recomp (FM4_GRAPHICS_LIVE, the in-game GRAPHICS menu): edge smoothing from the next presented swap (FM4_POST_AA's spellings).
// False when it cannot be switched in this run (no FM4_GRAPHICS_LIVE, the pass refused by the device, an unknown spelling): the
// caller then says the change applies at the next start.
void SetHdrHeadroom(float headroom)
{
    static float logged = -1.0f;
    static int lines = 0;
    hdrHeadroom.store(headroom, std::memory_order_relaxed);
    if (std::fabs(headroom - logged) >= 0.05f && lines < 20)
    {
        lines++, logged = headroom;
        std::fprintf(stderr, "[renderer] HDR output: the display's headroom is %.2fx SDR white, the picture's peak %.2fx%s\n", headroom,
                     runtime::video::hdr::PeakForHeadroom(headroom, hdrUnit.strength), lines == 20 ? " (no further lines)" : "");
    }
}

void SetHdrPreference(bool automatic)
{
    if (hdrWanted.exchange(automatic) != automatic)
        std::fprintf(stderr, "[renderer] HDR preference %s\n", automatic ? "Auto" : "Off");
}

void SetHdrDisplay(bool supported) { hdrDisplay.store(supported, std::memory_order_relaxed); }

HdrOutput HdrOutputNow()
{
    if (!hdrWanted.load())
        return {false, "HDR is off"};
    if (hdrUnavailable.load() != nullptr)
        return {false, "HDR is not available on this system"};
    if (!hdrUsable.load())  // this run was started without HDR and cannot change (no FM4_GRAPHICS_LIVE)
        return {false, "restart required"};
    if (!hdrUnit.offscreen && !hdrDisplay.load())
        return {false, "this display does not support HDR"};
    return {true, ""};
}

bool SetPostAaLive(const char* spelling)
{
    bool on = false;
    std::string error;
    runtime::video::postaa::Tuning tuning = runtime::video::postaa::kStandard;
    if (!postAa.live || !postAa.on || !runtime::video::postaa::Parse(spelling, on, error, &tuning))
        return false;
    const int level = !on ? 0 : std::strcmp(tuning.name, "soft") == 0 ? 1 : std::strcmp(tuning.name, "strong") == 0 ? 3 : 2;
    if (postAaLevel.exchange(level) != level)
        std::fprintf(stderr, "[renderer] post-process anti-aliasing: %s from the next presented swap (the GRAPHICS menu)\n", on ? tuning.name : "off");
    return true;
}

// FM4 Recomp (M9, live resolution, step 2; renderer.h). Only latches the request: Present serves it (ServeResolutionRequest).
bool RequestInternalRes(const char* spelling)
{
    uint32_t halves = 0;
    std::string error;
    if (!enabled || spelling == nullptr || *spelling == '\0' || !runtime::video::scale::ParseInternalRes(spelling, halves, error))
        return false;
    // Whether this run can switch at all; read once, after Initialize (`enabled`), and said once when it cannot.
    static const bool live = [] {
        const auto set = [](const char* name) { const char* v = std::getenv(name); return v != nullptr && v[0] == '1' && v[1] == '\0'; };
        const char* why = !set("FM4_GRAPHICS_LIVE") ? "FM4_GRAPHICS_LIVE is not 1"
                          : options.mode != Mode::Present && options.mode != Mode::Window ? "not a present or window run"
                          : !runtime::video::scale::OutputNative() ? "FM4_INTERNAL_OUTPUT=720p keeps the display image 1280x720"
                          : set("FM4_DIAG_POST_AA_PAIR") ? "FM4_DIAG_POST_AA_PAIR reads the picture before the pass at one size"
                          : set("FM4_GPU_PRESENT_CHECK") ? "FM4_GPU_PRESENT_CHECK compares 1280x720 words"
                                                         : nullptr;
        if (why != nullptr)
            std::fprintf(stderr, "[renderer] resolution: it cannot be changed while this run goes on (%s); a change applies at the next start\n", why);
        return why == nullptr;
    }();
    if (!live)
        return false;
    const uint32_t before = resRequested.exchange(halves, std::memory_order_relaxed);
    if (before != halves && halves != runtime::video::scale::InternalHalvesQuiet())
        std::fprintf(stderr, "[renderer] resolution: %s asked for (now %s): at the next swap with nothing in flight\n", runtime::video::scale::HalvesSpelling(halves),
                     runtime::video::scale::HalvesSpelling(runtime::video::scale::InternalHalvesQuiet()));
    return true;
}
const char* InternalResNow() { return runtime::video::scale::HalvesSpelling(runtime::video::scale::InternalHalvesQuiet()); }
bool InternalResPending()
{
    const uint32_t asked = resRequested.load(std::memory_order_relaxed);
    return asked != 0 && asked != runtime::video::scale::InternalHalvesQuiet();
}

bool Enabled() { return enabled; }
void SetEdramHooks(const EdramHooks* hooks) { edramHooks = hooks; }
void SetTextureHooks(const TextureHooks* hooks) { textures = hooks; }

bool Initialize(const Options& o)
{
    options = o;
    if (o.mode == Mode::Headless)
        return false;
    if (o.gpuStopPath == nullptr || o.captureDir == nullptr)
        Stop(StopCode::Setup, "renderer: --gpu-stop and --gpu-capture are required with a GPU renderer mode");
    std::string classError;
    if (!ParseShaderClassAllowList(o.shaderClasses, allowedClasses, classError))  // required, no silent default (shader_class.h)
        Stop(StopCode::Setup, "renderer: %s", classError.c_str());
    const gpu::FenceTimeout timeout = gpu::FenceTimeoutFromEnv(o.fenceTimeoutEnv);
    if (timeout.malformed)  // fail closed: a mistyped limit is not silently replaced
        Stop(StopCode::Setup, "renderer: FM4_GPU_FENCE_TIMEOUT_MS=%s is not a decimal number of milliseconds", o.fenceTimeoutEnv);
    if (AsyncSubmit())
    {
        const uint64_t boundNs = PresenterWaitBoundNs(gpu::FenceTimeoutNs(timeout.ms));
        asyncBackend.emplace(backend, kSlots, [boundNs] {
            const uint64_t lockStart = drain::Enabled() ? drain::NowNs() : 0;  // FM4_DIAG_DRAIN (diagnostic): the presenter's queue lock
            std::unique_lock<std::timed_mutex> lock = LockQueue(queueLock, boundNs, "submit (worker)");
            if (lockStart != 0)
                drain::QueueLockWaited(drain::NowNs() - lockStart);
            return lock;
        });
        if (diag::SlowSubmitEnabled())  // FM4_DIAG_SLOW_SUBMIT (diagnostic)
            asyncBackend->SetTimingHook(diag::ReportSubmit);
    }
    SubmissionBackend<Transient>& submitter =
        asyncBackend ? static_cast<SubmissionBackend<Transient>&>(*asyncBackend) : static_cast<SubmissionBackend<Transient>&>(backend);
    if (AsyncRetire())  // FM4 Recomp (FM4_ASYNC_RETIRE): the completion worker, between GpuGuard and the submitting backend
    {
        if (ResolveResidentCheck())
            Stop(StopCode::Setup, "renderer: FM4_ASYNC_RETIRE=1 with FM4_RESOLVE_RESIDENT_CHECK=1 (the check reads the EDRAM unit at the write-back)");
        AsyncRetireBackend<Transient>::Hooks hooks;
        hooks.completed = [](uint64_t serial) {
            if (GpuReadGuard* g = GuestMemoryGpuGuard())  // gpu_read_guard.h: after the fence wait, before any write-back
                g->Completed(serial);
        };
        hooks.writeBack = [](uint64_t serial, std::string& error) {
            if (!drain::Enabled())
                return edramHooks == nullptr || edramHooks->writeBackHandedOff == nullptr || edramHooks->writeBackHandedOff(serial, error);
            drain::WriteBackBegin(serial);  // FM4_DIAG_DRAIN (diagnostic)
            const bool ok = edramHooks == nullptr || edramHooks->writeBackHandedOff == nullptr || edramHooks->writeBackHandedOff(serial, error);
            drain::WriteBackEnd(serial);
            return ok;
        };
        hooks.present = [](uint64_t presentSwap, std::string& error) {
            if (HandOver().Offer(presentSwap))  // window_policy.h FrameHandOver: the command processor's BeginWrite came first
                return true;
            error = "swap " + std::to_string(presentSwap) + ": the window hand-over was not idle after BeginWrite (contract breach)";
            return false;
        };
        hooks.onWorkerStart = [] {
            pthread_setname_np("fm4 gpu retire");  // FM4 Recomp: thread_cpu.h roles
            runtime::ApplyThreadQos();  // FM4_THREAD_QOS (experiment, off by default)
            runtime::gpu::SetCompletionWorkerThread();
        };
        retireBackend.emplace(submitter, kSlots, gpu::FenceTimeoutNs(timeout.ms), PresenterWaitBoundNs(0), std::move(hooks));
    }
    guard.emplace(retireBackend ? static_cast<SubmissionBackend<Transient>&>(*retireBackend) : submitter, gate, o.gpuStopPath, gpu::FenceTimeoutNs(timeout.ms));
    Require(guard->Begin("renderer initialize"));  // GPU_STOP before any Vulkan call
    captures.emplace(o.captureDir, "frame_", kCaptureBytes);
    if (const char* v = std::getenv("FM4_CAPTURE_RING"); v != nullptr && v[0] == '1' && v[1] == '\0')
        captureRing = true;
    if (const char* v = std::getenv("FM4_CAPTURE_FRAMES"); v != nullptr && *v != '\0')
    {
        for (const char* p = v; *p != '\0';)
        {
            char* end = nullptr;
            const unsigned long long swap = std::strtoull(p, &end, 10);
            if (end == p || (*end != ',' && *end != '\0'))
                Stop(StopCode::Setup, "renderer: FM4_CAPTURE_FRAMES=%s is not a comma-separated list of swap numbers", v);
            captureFrames.insert(swap);
            p = *end == ',' ? end + 1 : end;
        }
    }
    const KeepEvery keep = KeepEveryFromEnv(std::getenv("FM4_CAPTURE_KEEP_EVERY"));
    if (keep.malformed)
        Stop(StopCode::Setup, "renderer: FM4_CAPTURE_KEEP_EVERY is not a decimal number from 1 to 1000000");
    if (keep.every)
    {
        keepEvery = keep.every;
        kept.emplace(o.captureDir, "keep_", kKeepBytes);
        std::fprintf(stderr, "[gpu] diagnostic FM4_CAPTURE_KEEP_EVERY=%llu: every %llu-th swap also kept (at most %zu files, %llu MB)\n",
                     (unsigned long long)keepEvery, (unsigned long long)keepEvery, kKeepRecords, (unsigned long long)(kKeepBytes >> 20));
    }
    std::fprintf(stderr, "[gpu] captures: ring %s, keep every %llu, frames %zu listed (PNG captures are off by default; FM4_CAPTURE_RING=1, "
                         "FM4_CAPTURE_KEEP_EVERY=<n>, FM4_CAPTURE_FRAMES=<swaps> opt in)\n",
                 captureRing ? "on" : "off", (unsigned long long)keepEvery, captureFrames.size());
    SetStopQuiesce(QuiesceForStop, ReportForSignalStop);
    if (o.mode == Mode::Window && o.surface == nullptr)
        Stop(StopCode::Setup, "renderer: window mode needs the window's surface request (window_sdl.h OpenWindow)");
    std::fprintf(stderr, "[gpu] renderer %s: fence timeout %u ms, captures %s (at most %zu files, %llu MB), GPU_STOP %s, shader classes allowed: %s\n",
                 o.mode == Mode::SelfCheck ? "self-check" : o.mode == Mode::Window ? "window" : "present", timeout.ms, o.captureDir, kCaptureRecords,
                 (unsigned long long)(kCaptureBytes >> 20), o.gpuStopPath, allowedClasses == kClassStraight ? "none (straight-line only)" : ShaderClassName(allowedClasses).c_str());
    dev = CreateVulkanDevice(o.validation, o.mode == Mode::Window ? o.surface : nullptr);  // surface and swapchain: window mode only
    CreateResources();
    // Unit 8: the shader and pipeline caches. The portability-subset features are not exposed by
    // vulkan_device.cpp yet: constant-alpha colour blend factors stay refused (conservative; FM4's first
    // screens blend with src alpha / inv src alpha only).
    compiler.emplace();
    shaders.emplace(*compiler, o.shaderDumpDir, ShaderCache::kDefaultMaxVariants, ShaderMemo());
    // The features enabled at vkCreateDevice (vulkan_device.cpp), never merely the supported ones.
    PipelineDeviceFeatures features;
    features.depthClamp = dev.depthClamp;
    features.constantAlphaColorBlendFactors = false;
    features.independentBlend = dev.independentBlend;
    features.dynamicBuffers = descriptorCacheOn;
    features.pushDescriptorLimit = dev.pushDescriptors && !descriptorCacheOn ? dev.maxPushDescriptors : 0;  // FM4_PUSH_DESCRIPTORS
    features.pushTemplates = features.pushDescriptorLimit != 0 && dev.cmdPushDescriptorSetWithTemplate != nullptr;  // FM4_PUSH_TEMPLATE
    KeyResult created;
    pipelines.emplace(dev.device, features, allowedClasses, created);
    if (!created)
        Stop(StopCode::Setup, "renderer: %s", created.reason.c_str());
    if (const char* dir = std::getenv("FM4_SHADER_CACHE"); dir != nullptr && *dir != '\0')  // FM4 Recomp (M7 stability): pipeline_cache.h
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev.gpu, &props);
        pipelines->UsePersistentCache(std::string(dir) + "/pipelines.bin", props.vendorID, props.deviceID, props.pipelineCacheUUID);
    }
    SetTextureDumpDir(o.captureDir);              // FM4_DUMP_TEXTURE (diagnostic, texture_dump.h): nothing unless set
    SetTextureHooks(InitializeTextureUnit(dev, &*compiler));  // unit 7 (texture_vk.h)
    // The EDRAM unit (edram_vk.h): its transfer shaders through the same compiler, its resolve writes through main.cpp's
    // writable guest base, each recorded resolve reported to the texture unit (guest_writes.h MarkGuestWritten). Present
    // mode only: the self-check draws nothing and keeps its device work unchanged, unless FM4_SELFCHECK_EDRAM asks for the
    // isolated EDRAM check (RunSelfCheck step 6).
    if (o.mode == Mode::Present || o.mode == Mode::Window || (o.mode == Mode::SelfCheck && EdramSelfCheckRequested()))
    {
        if (o.guestBase == nullptr)
            Stop(StopCode::Setup, "renderer: present mode needs the writable guest base (Options::guestBase)");
        std::string edramError;
        const EdramHooks* hooks = InitializeEdramVk(dev, *compiler, o.guestBase, MarkGuestWritten, edramError);
        if (hooks == nullptr)
            Stop(StopCode::Setup, "renderer: EDRAM unit: %s", edramError.c_str());
        SetEdramHooks(hooks);
    }
    InitGpuPresent();  // FM4 Recomp (FM4_GPU_PRESENT): after the compiler and the EDRAM unit; nothing unless asked
    InitPostAa();      // FM4 Recomp (M9, FM4_POST_AA): after the compiler; nothing unless asked
    InitHdr();         // FM4 Recomp (M9, FM4_HDR): the same
    {  // FM4 Recomp (M9, opt-in FM4_OCCLUSION_QUERIES=1): the outcome named once
        const char* v = std::getenv("FM4_OCCLUSION_QUERIES");
        const bool asked = v != nullptr && v[0] == '1' && v[1] == '\0';
        zpdUnit = ZpdUnit{};
        zpdUnit.on = asked && dev.occlusionQueryPrecise;
        if (zpdUnit.on)
        {
            zpdUnit.results = std::make_unique<zpd::ResultRing>();
            if (const char* lines = std::getenv("FM4_DIAG_ZPD_QUERY_LOG"))
                zpdUnit.logLines = std::min(std::strtol(lines, nullptr, 10), 1000000L);
            std::fprintf(stderr, "[zpd] occlusion queries counted on the GPU (FM4_OCCLUSION_QUERIES=1): precise Vulkan queries, %u a command buffer; an END's "
                                 "report is stored after the GPU finished its query\n", ZpdUnit::kQueries);
        }
        else if (asked)
            std::fprintf(stderr, "[zpd] FM4_OCCLUSION_QUERIES=1, but this device has no precise occlusion queries: not counted (the fixed result, as without it)\n");
    }
    runtime::video::keepbusy::Initialize(dev, *compiler, o.gpuStopPath, guard->FenceTimeoutNs(), [] { return gate.Load().inFlight == 0; });  // FM4 Recomp (M9 experiment, FM4_GPU_KEEP_BUSY): nothing unless asked
    if (o.mode == Mode::Window)  // on the main thread (main.cpp): the swapchain, the presenter's slot and guard
    {
        InitializePresenter({&dev, display, {displayWidth, displayHeight}, VK_NULL_HANDLE, &gate, o.gpuStopPath,
                             guard->FenceTimeoutNs(), &queueLock});
    }
    enabled = true;
    return true;
}

namespace {

// ---- FM4 Recomp (M9, live resolution, step 2): the switch ------------------------------------------------------------------
// The creators below return false where Initialize's stop (Check, MakeBuffer): a switch that cannot make something is refused and
// the run goes on at its scale. Whatever a failed call made was used by no command and is destroyed before it returns.
bool TryImage(VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage, const char* label, VkImage& image, VkDeviceMemory& memory,
              uint64_t& ledger)
{
    image = VK_NULL_HANDLE, memory = VK_NULL_HANDLE;
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    if (vkCreateImage(dev.device, &info, nullptr, &image) != VK_SUCCESS)
        return image = VK_NULL_HANDLE, false;
    VkMemoryRequirements need;
    vkGetImageMemoryRequirements(dev.device, image, &need);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = need.size;
    allocate.memoryTypeIndex = MemoryType(dev, need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev.device, &allocate, nullptr, &memory) != VK_SUCCESS)
        memory = VK_NULL_HANDLE;
    if (memory == VK_NULL_HANDLE || vkBindImageMemory(dev.device, image, memory, 0) != VK_SUCCESS)
    {
        vkDestroyImage(dev.device, image, nullptr);
        vkFreeMemory(dev.device, memory, nullptr);  // null-safe
        return image = VK_NULL_HANDLE, memory = VK_NULL_HANDLE, false;
    }
    ledger = runtime::video::mem::Global().Add(runtime::video::mem::Kind::Display, need.size, label);
    return true;
}
bool TryView(VkImage image, VkFormat format, VkImageView& view)
{
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    info.image = image;
    if (vkCreateImageView(dev.device, &info, nullptr, &view) != VK_SUCCESS)
        return view = VK_NULL_HANDLE, false;
    return true;
}
bool TryBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Transient& b)  // MakeBuffer's buffer (host-visible, mapped), without its stops
{
    b = Transient{};
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    if (vkCreateBuffer(dev.device, &info, nullptr, &b.buffer) != VK_SUCCESS)
        return b = Transient{}, false;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(dev.device, b.buffer, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(dev, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(dev.device, &allocate, nullptr, &b.memory) != VK_SUCCESS)
        b.memory = VK_NULL_HANDLE;
    if (b.memory == VK_NULL_HANDLE || vkBindBufferMemory(dev.device, b.buffer, b.memory, 0) != VK_SUCCESS ||
        vkMapMemory(dev.device, b.memory, 0, size, 0, &b.mapped) != VK_SUCCESS)
    {
        vkDestroyBuffer(dev.device, b.buffer, nullptr);
        vkFreeMemory(dev.device, b.memory, nullptr);  // null-safe; implicitly unmapped
        return b = Transient{}, false;
    }
    b.ledger = runtime::video::mem::Global().Add(runtime::video::mem::Kind::Present, allocate.allocationSize, "present buffer");
    return true;
}

// Everything of the display image's size, made for the new size before anything of the old size is touched.
struct ResChain
{
    VkImage display = VK_NULL_HANDLE, aaSource = VK_NULL_HANDLE, hdrImage = VK_NULL_HANDLE;
    VkDeviceMemory displayMemory = VK_NULL_HANDLE, aaMemory = VK_NULL_HANDLE, hdrMemory = VK_NULL_HANDLE;
    uint64_t displayLedger = 0, aaLedger = 0, hdrLedger = 0;
    VkImageView aaSourceView = VK_NULL_HANDLE, aaDisplayView = VK_NULL_HANDLE, hdrView = VK_NULL_HANDLE;
    Transient readback, out, hdrResult, hiWords[FrontWordsTracker::kEntries];
};
// A chain no command ever used (the switch was refused after part of it was made).
void DestroyResChain(ResChain& n)
{
    for (VkImageView v : {n.aaSourceView, n.aaDisplayView, n.hdrView})
        vkDestroyImageView(dev.device, v, nullptr);  // each null-safe
    for (VkImage i : {n.display, n.aaSource, n.hdrImage})
        vkDestroyImage(dev.device, i, nullptr);
    for (VkDeviceMemory m : {n.displayMemory, n.aaMemory, n.hdrMemory})
        vkFreeMemory(dev.device, m, nullptr);
    for (uint64_t id : {n.displayLedger, n.aaLedger, n.hdrLedger})
        runtime::video::mem::Global().Release(id);  // 0: nothing
    for (Transient* t : {&n.readback, &n.out, &n.hdrResult})
        if (t->buffer != VK_NULL_HANDLE)
            backend.Destroy(*t);
    for (Transient& t : n.hiWords)
        if (t.buffer != VK_NULL_HANDLE)
            backend.Destroy(t);
    n = ResChain{};
}

// The internal resolution from `from` to `to` (halves, render_scale.h), at swap `swap`. The caller (ServeResolutionRequest) has
// established the safe point: the command processor's thread; no command buffer open and no render pass; every submission of every
// slot confirmed complete (so no recorded or pending command names any object below); no guest write pending; in a window, the
// hand-over's write is held (the presenter is not reading the display image, its own submission is retired, and it cannot take a
// frame before this thread offers one).
//   1. Refuse what the device cannot do at the new size (the checks Initialize makes at the start size).
//   2. Make everything of the new size (the chain; the 1x display image and the scaled sets if this run never had them; the EDRAM
//      unit's modules and scaled resident image). Any failure: what was made is destroyed unused, false with `why`, nothing changed.
//   3. Destroy what has the old size, in the units' order: EDRAM targets (the renderer's framebuffers over them first), texture
//      twins, the scaled front-buffer words, then the display chain's old objects. Nothing can fail from here.
//   4. Install: the scale, the size, the new objects, the descriptor sets that named an old one, the presenter's image and extent.
// What is lost: the contents of the EDRAM host targets (zero until drawn; each key's pending clears are kept), the scaled twins and
// the scaled front-buffer words (this swap, and each texture until its next resolve, come from the 1x words, which are untouched).
bool SwitchResolution(uint64_t swap, uint32_t from, uint32_t to, bool windowed, std::string& why)
{
    namespace sc = runtime::video::scale;
    namespace aa = runtime::video::postaa;
    namespace h = runtime::video::hdr;
    const auto started = std::chrono::steady_clock::now();
    const bool scaled = sc::OutputScaledAt(to);
    const uint32_t width = scaled ? sc::HostEdge(kDisplayWidth, to) : kDisplayWidth, height = scaled ? sc::HostEdge(kDisplayHeight, to) : kDisplayHeight;
    const VkDeviceSize frameBytes = VkDeviceSize(kDisplayWidth) * kDisplayHeight * 4, displayBytes = VkDeviceSize(width) * height * 4;
    const VkFormat encoded = VK_FORMAT_A2B10G10R10_UNORM_PACK32, linear = VK_FORMAT_R16G16B16A16_SFLOAT;
    const bool aaImages = postAa.source != VK_NULL_HANDLE, hdrImages = hdrUnit.image != VK_NULL_HANDLE;

    // 1. The device at the new size.
    VkFormatProperties format;
    vkGetPhysicalDeviceFormatProperties(dev.gpu, encoded, &format);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(dev.gpu, &properties);
    const VkPhysicalDeviceLimits& limits = properties.limits;
    const VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    const char* refusal =
        scaled && (format.optimalTilingFeatures & blit) != blit ? "A2B10G10R10_UNORM images cannot be blitted with a linear filter (a swap without scaled words)"
        : width > limits.maxImageDimension2D || height > limits.maxImageDimension2D ? "the display image would exceed the device's image size"
        : gpuPresent.on && limits.maxComputeWorkGroupCount[0] < (width * height + kGpuPresentGroupSize - 1) / kGpuPresentGroupSize
            ? "too many workgroups for the GPU present's dispatch"
        : postAa.on && (limits.maxComputeWorkGroupCount[0] < aa::Groups(width) || limits.maxComputeWorkGroupCount[1] < aa::Groups(height))
            ? "too many workgroups for the anti-aliasing pass's dispatch"
        : (hdrImages || hdrUsable.load()) && (limits.maxComputeWorkGroupCount[0] < h::Groups(width) || limits.maxComputeWorkGroupCount[1] < h::Groups(height))
            ? "too many workgroups for the HDR pass's dispatch"
            : nullptr;
    if (refusal != nullptr)
        return why = refusal, false;

    // 2. Everything of the new size. The usages are CreateResources', InitGpuPresent's and CreateHdr's.
    ResChain n;
    const VkImageUsageFlags transfer = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    bool ok = TryImage(encoded, width, height, aaImages ? transfer | VK_IMAGE_USAGE_STORAGE_BIT : transfer, "display image", n.display, n.displayMemory, n.displayLedger) &&
              TryBuffer(displayBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, n.readback);
    if (aaImages)
    {
        ok = ok && TryImage(encoded, width, height, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | (postAa.pair ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0),
                            "post AA source image (FM4_POST_AA)", n.aaSource, n.aaMemory, n.aaLedger);
        if (postAa.on)  // the pass's views and set exist (InitPostAa did not refuse it)
            ok = ok && TryView(n.aaSource, encoded, n.aaSourceView) && TryView(n.display, encoded, n.aaDisplayView);
    }
    if (gpuPresent.on)
    {
        ok = ok && TryBuffer(displayBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, n.out);
        for (size_t i = 0; scaled && i < FrontWordsTracker::kEntries; i++)  // an entry that has its 1x words gets scaled ones (as MakeFrontBuffers pairs them)
            if (gpuPresent.words[i].buffer != VK_NULL_HANDLE)
                ok = ok && TryBuffer(displayBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, n.hiWords[i]);
        if (ok && scaled && gpuPresent.hiSets[0] == VK_NULL_HANDLE)  // a run started with the display image at 1280x720: the pool has their room (InitGpuPresent)
        {
            VkDescriptorSetLayout layouts[FrontWordsTracker::kEntries];
            for (VkDescriptorSetLayout& l : layouts)
                l = gpuPresent.setLayout;
            VkDescriptorSetAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate.descriptorPool = gpuPresent.pool;
            allocate.descriptorSetCount = uint32_t(FrontWordsTracker::kEntries);
            allocate.pSetLayouts = layouts;
            ok = vkAllocateDescriptorSets(dev.device, &allocate, gpuPresent.hiSets) == VK_SUCCESS;  // kept if the switch is refused below: unwritten, never bound
            if (!ok)
                for (VkDescriptorSet& s : gpuPresent.hiSets)
                    s = VK_NULL_HANDLE;
        }
    }
    if (hdrImages)
    {
        ok = ok && TryImage(linear, width, height, transfer | VK_IMAGE_USAGE_STORAGE_BIT, "HDR image (FM4_HDR)", n.hdrImage, n.hdrMemory, n.hdrLedger) &&
             TryView(n.hdrImage, linear, n.hdrView);
        if (hdrUnit.result.buffer != VK_NULL_HANDLE)  // offscreen: the image read back at a kept capture
            ok = ok && TryBuffer(VkDeviceSize(width) * height * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, n.hdrResult);
    }
    if (ok && scaled && display1x == VK_NULL_HANDLE)  // CreateDisplay1x's image, without its stops; kept once made, whatever follows
    {
        uint64_t ledger = 0;
        ok = TryImage(encoded, kDisplayWidth, kDisplayHeight, transfer, "1x display image (a swap without scaled words)", display1x, display1xMemory, ledger);
    }
    if (!ok)
        why = "an image, a buffer or a descriptor set of the new size could not be created";
    else if (std::string error; edramHooks != nullptr && !edramHooks->prepareScale(to, error))
        ok = false, why = error;
    if (!ok)
    {
        DestroyResChain(n);
        return false;
    }

    // 3. What has the old size. Every submission is confirmed complete and nothing is being recorded (the caller's checks).
    RescaleCounts counts;
    if (edramHooks != nullptr)
    {
        edramHooks->rescale(to, ForgetFramebuffersOf, counts);
        resClearsCarried = counts.carriedClears != 0;
    }
    const size_t twins = DestroyTextureTwins();
    size_t scaledWords = 0;
    if (gpuPresent.on)
    {
        for (size_t i = 0; i < FrontWordsTracker::kEntries; i++)
        {
            if (gpuPresent.hiWords[i].buffer != VK_NULL_HANDLE)
                backend.Destroy(gpuPresent.hiWords[i]), scaledWords++;
            gpuPresent.hiWords[i] = n.hiWords[i];
            gpuPresent.hiFront[i].bands.clear();  // no row has scaled words: this swap is its 1x words (ScaledFrontEntry)
        }
        backend.Destroy(gpuPresent.out);
        gpuPresent.out = n.out;
    }
    vkDestroyImage(dev.device, display, nullptr);
    vkFreeMemory(dev.device, displayMemory, nullptr);
    runtime::video::mem::Global().Release(displayLedger);
    display = n.display, displayMemory = n.displayMemory, displayLedger = n.displayLedger;
    backend.Destroy(readback);
    readback = n.readback;
    if (aaImages)
    {
        vkDestroyImageView(dev.device, postAa.sourceView, nullptr);  // null-safe (the pass refused: no views)
        vkDestroyImageView(dev.device, postAa.displayView, nullptr);
        vkDestroyImage(dev.device, postAa.source, nullptr);
        vkFreeMemory(dev.device, postAa.memory, nullptr);
        runtime::video::mem::Global().Release(postAaLedger);
        postAa.source = n.aaSource, postAa.memory = n.aaMemory, postAaLedger = n.aaLedger;
        postAa.sourceView = n.aaSourceView, postAa.displayView = n.aaDisplayView;
    }
    if (hdrImages)
    {
        vkDestroyImageView(dev.device, hdrUnit.view, nullptr);
        vkDestroyImage(dev.device, hdrUnit.image, nullptr);
        vkFreeMemory(dev.device, hdrUnit.memory, nullptr);
        runtime::video::mem::Global().Release(hdrLedger);
        if (hdrUnit.result.buffer != VK_NULL_HANDLE)
            backend.Destroy(hdrUnit.result);
        hdrUnit.image = n.hdrImage, hdrUnit.memory = n.hdrMemory, hdrUnit.view = n.hdrView, hdrLedger = n.hdrLedger;
        hdrUnit.result = n.hdrResult;
    }

    // 4. The new scale and size, and every descriptor set that named an object of the old size (none is in use by any command).
    sc::SetInternalHalves(to);
    displayWidth = width, displayHeight = height;
    if (gpuPresent.on)
        for (size_t i = 0; i < FrontWordsTracker::kEntries; i++)
        {
            if (gpuPresent.words[i].buffer != VK_NULL_HANDLE)
                WriteFrontSet(gpuPresent.sets[i], gpuPresent.words[i].buffer, frameBytes);  // it binds the display words (`out`)
            if (gpuPresent.hiWords[i].buffer != VK_NULL_HANDLE)
                WriteFrontSet(gpuPresent.hiSets[i], gpuPresent.hiWords[i].buffer, displayBytes);
        }
    if (aaImages && postAa.on)  // InitPostAa's writes, with the new views
    {
        const VkDescriptorImageInfo in{postAa.sampler, postAa.sourceView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, out{VK_NULL_HANDLE, postAa.displayView, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet writes[2] = {};
        for (uint32_t b = 0; b < 2; b++)
        {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = postAa.set;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[b].pImageInfo = b == 0 ? &in : &out;
        }
        vkUpdateDescriptorSets(dev.device, 2, writes, 0, nullptr);
    }
    if (hdrImages && hdrUnit.set != VK_NULL_HANDLE)  // CreateHdr's write of the image; the limiter's state buffer and its binding stay
    {
        const VkDescriptorImageInfo image{VK_NULL_HANDLE, hdrUnit.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = hdrUnit.set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(dev.device, 1, &write, 0, nullptr);
    }
    // The variant memo's key has no scale (draw_record.h VariantMemoKey) while the variants it names do (XE_HALVES): emptied, the
    // next draws choose again. The variants and pipelines themselves are keyed by their defines and stay.
    variantMemo.Clear();
    // FM4_OCCLUSION_QUERIES: a bracket open here would sum segments weighted for two scales. Present dropped the brackets of the
    // frame that ended (a bracket never spans a swap); kept explicit: the game gets no count for a query a switch interrupts.
    if (zpdUnit.on)
        zpdUnit.dropped += zpdUnit.open.DropBefore(UINT64_MAX);
    // The presenter is not presenting and cannot take a frame before this thread offers one (the caller's BeginWrite).
    if (windowed)
    {
        SetPresenterDisplay(display, {width, height});
        if (hdrImages)
            SetPresenterHdr(hdrUnit.image);  // it was given the old image when the unit was made (Present)
    }
    resSwitches++;
    std::fprintf(stderr, "[renderer] resolution switched at swap %llu: %s -> %s (%ux%u) in %.1f ms; EDRAM: %u host target(s), %u resolve shadow(s) and %u outgrown "
                         "image(s) destroyed, %u pending clear(s) carried, %u scaled resident image(s) destroyed; %zu texture twin(s) and %zu scaled front-buffer "
                         "word buffer(s) destroyed; made again: the display image%s%s%s, the readback%s; this swap is presented from its 1x words (switch %llu)\n",
                 (unsigned long long)swap, sc::HalvesSpelling(from), sc::HalvesSpelling(to), width, height,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(), counts.targets, counts.shadows, counts.outgrown,
                 counts.carriedClears, counts.residentImages, twins, scaledWords, aaImages ? ", the anti-aliasing pass's source" : "",
                 hdrImages ? ", the HDR image" : "", gpuPresent.on ? ", the display words" : "", windowed ? " (the window's presenter given the new image)" : "",
                 (unsigned long long)resSwitches);
    return true;
}

// Present, after the frame's FlushSlot: the pending request, if any. Not ready (something recorded, submitted or pending, or the
// window's presenter still reading the display image): tried again at the next swap, kResAttempts times, then refused.
void ServeResolutionRequest(uint64_t swap, bool windowed)
{
    namespace sc = runtime::video::scale;
    uint32_t want = resRequested.load(std::memory_order_relaxed);
    const uint32_t now = sc::InternalHalves();
    // A newer request (another thread, meanwhile) stays latched: only the one served is cleared.
    const auto done = [&] { resRequested.compare_exchange_strong(want, 0, std::memory_order_relaxed), resAttempts = 0; };
    if (want == 0 || want == now)
        return done();
    const auto refuse = [&](const char* reason) {
        resRefusals++;
        std::fprintf(stderr, "[renderer] resolution: the change to %s at swap %llu is refused (%s); it stays %s (refusal %llu)\n", sc::HalvesSpelling(want),
                     (unsigned long long)swap, reason, sc::HalvesSpelling(now), (unsigned long long)resRefusals);
        done();
    };
    // The safe point. The frame's parts were retired by Present's FlushSlot; an earlier swap's present part (FM4_ASYNC_RETIRE) here.
    if (retireBackend)
        RetireParts(UINT64_MAX, true);
    bool idle = submitted.empty() && openPass.commands == VK_NULL_HANDLE && !GuestWritesPending();
    for (uint32_t k = 0; k < kSlots; k++)
        idle = idle && !slots[k].open && guard->SlotState(k) == gpu::SubmissionLifetime<Transient>::State::Idle;
    const char* notReady = !idle ? "a command buffer is open, a submission is not confirmed complete or a guest write is pending"
                           : edramHooks != nullptr && !edramHooks->rescaleReady() ? "a resolve's read-back is still pending"
                                                                                  : nullptr;
    // In a window the display image is replaced: never while the presenter blits from it (the bounded wait Present makes anyway;
    // its own call later finds the hand-over idle). A frame the window had not taken is withdrawn, as at any swap.
    if (notReady == nullptr && windowed &&
        HandOver().BeginWrite(PresenterWaitBoundNs(guard->FenceTimeoutNs())) == window::FrameHandOver::Write::TimedOut)
        notReady = "the window's presenter still reads the display image";
    if (notReady != nullptr)
    {
        if (++resAttempts >= kResAttempts)
            refuse(notReady);
        return;
    }
    std::string why;
    if (SwitchResolution(swap, now, want, windowed, why))
        done();
    else
        refuse(why.c_str());
}

}  // namespace

void Present(const uint8_t* base, const uint32_t fetch[6], uint32_t physical, uint32_t width, uint32_t height, uint64_t swap)
{
    currentSwap = swap;
    {  // FM4 Recomp (M9, Extended View; kernel/extended_view.h): the factor for the frames from here, latched between frames
        static const float forced = [] {  // FM4_DIAG_ASPECT_FACTOR (diagnostic): a factor without a window, 1..2
            const char* v = std::getenv("FM4_DIAG_ASPECT_FACTOR");
            const float f = v != nullptr ? std::strtof(v, nullptr) : 0.0f;
            if (f >= runtime::view::kMinFactor && f <= runtime::view::kMaxFactor)
                runtime::view::Global().Force(f), std::fprintf(stderr, "[diag aspect] FM4_DIAG_ASPECT_FACTOR=%s: the main view is shaped by %.4f whatever the window (DIAGNOSTIC)\n", v, f);
            return f;
        }();
        (void)forced;
        // FM4_DIAG_ASPECT_CYCLE=<factor>[,<factor>...] (diagnostic; each 1..2) with FM4_DIAG_ASPECT_WINDOW=<swaps> (default 240): the forced
        // factor changes at every window, as a window resized or the setting switched would change it, in one bounded offscreen run.
        static const std::vector<float> cycle = [] {
            std::vector<float> factors;
            for (const char* at = std::getenv("FM4_DIAG_ASPECT_CYCLE"); at != nullptr && *at;)
            {
                char* end = nullptr;
                const float f = std::strtof(at, &end);
                if (end == at || f < runtime::view::kMinFactor || f > runtime::view::kMaxFactor)
                    break;
                factors.push_back(f);
                at = *end == ',' ? end + 1 : end;
            }
            if (!factors.empty())
                std::fprintf(stderr, "[diag aspect] FM4_DIAG_ASPECT_CYCLE: %zu factor(s) in turn (DIAGNOSTIC)\n", factors.size());
            return factors;
        }();
        static const uint64_t cycleWindow = [] { const char* v = std::getenv("FM4_DIAG_ASPECT_WINDOW"); const uint64_t n = v ? std::strtoull(v, nullptr, 10) : 0; return n ? n : uint64_t(240); }();
        if (!cycle.empty())
            runtime::view::Global().Force(cycle[size_t((swap / cycleWindow) % cycle.size())]);
        static const bool preference = [] {  // FM4_EXTENDED_VIEW=on (play.sh, from graphics.conf): the preference at the start; off otherwise
            const char* v = std::getenv("FM4_EXTENDED_VIEW");
            if (v != nullptr && *v != '\0' && std::strcmp(v, "off") != 0 && std::strcmp(v, "on") != 0)
                Stop(StopCode::Setup, "renderer: FM4_EXTENDED_VIEW '%s' is not off or on", v);
            const bool on = v != nullptr && std::strcmp(v, "on") == 0;
            if (on)
                runtime::view::Global().SetPreference(true);
            return on;
        }();
        (void)preference;
        if (runtime::view::Global().Latch())
            std::fprintf(stderr, "[renderer] Extended View: factor %.4f from swap %llu\n", runtime::view::Global().Factor(), (unsigned long long)swap);
        // The frame that ends here was drawn with extendedFrameFactor: it is shown stretched by it. The next frame's factor: the
        // latched one while the game widens a main view (its hook was called in the last half second), 1 otherwise.
        const float shown = extendedFrameFactor, next = runtime::view::Global().Effective(runtime::gpu::VblankCount());
        extendedShownFactor = shown;
        static uint64_t lines = 0;
        if (next != shown && lines++ < 40)
            std::fprintf(stderr, "[renderer] Extended View: frames from swap %llu are %s (the frame before: %llu HUD and menu draw(s) narrowed)\n",
                         (unsigned long long)(swap + 1), next > 1.0f ? "wider, their HUD and menus narrowed" : next < 1.0f ? "taller, their HUD and menus narrowed" : "16:9",
                         (unsigned long long)extendedNarrowedSinceSwap);
        extendedFrameFactor = next, extendedNarrowedSinceSwap = 0;
    }
    {  // FM4 Recomp (M9, FM4_HDR): the HUD's gain for the frames from here: only while the picture is expanded, by the base peak in steps
        const float headroom = hdrUnit.assumed != 0.0f ? hdrUnit.assumed : hdrHeadroom.load(std::memory_order_relaxed);
        const float gain = hdrUnit.on && HdrEffective()
                               ? runtime::video::hdr::HudGain(runtime::video::hdr::HudGainPeak(runtime::video::hdr::PeakForHeadroom(headroom, hdrUnit.strength)))
                               : 1.0f;
        static int lines = 0;
        if (gain != hdrHudGain && lines++ < 30)
            std::fprintf(stderr, "[renderer] HDR output: the HUD's and menus' draws are scaled by %.4f from swap %llu (their white is standard white after the expansion)\n",
                         double(gain), (unsigned long long)(swap + 1));
        hdrHudGain = gain;
    }
    renderSwap = swap;
    if (zpdUnit.on)  // FM4_OCCLUSION_QUERIES: a bracket never spans a swap (its END was not issued); the tallies every 600 swaps
    {
        if (swap != 0)
            zpdUnit.dropped += zpdUnit.open.DropBefore(swap);
        if (swap % 600 == 0)
        {
            ZpdUnit& z = zpdUnit;
            std::fprintf(stderr, "[zpd] swaps to %llu: %llu ZPD(s), %llu BEGIN(s), %llu END(s), %llu report(s) stored (%llu with samples; last %llu, highest %llu guest samples), "
                                 "%llu counted draw(s) (%llu drawn only for the count) in %llu segment(s); not counted: %llu untested draw(s); %llu END(s) without a BEGIN, "
                                 "%llu bracket(s) dropped, %llu segment(s) lost, %llu not available, %llu past a command buffer's queries\n",
                         (unsigned long long)swap, (unsigned long long)z.zpds, (unsigned long long)z.begins, (unsigned long long)z.endReports,
                         (unsigned long long)z.stored, (unsigned long long)z.nonZero, (unsigned long long)z.lastCount, (unsigned long long)z.maxCount,
                         (unsigned long long)z.draws, (unsigned long long)z.countedOnly, (unsigned long long)z.segments, (unsigned long long)z.untested,
                         (unsigned long long)z.unmatched, (unsigned long long)z.dropped, (unsigned long long)z.lost, (unsigned long long)z.unavailable,
                         (unsigned long long)z.overflow);
            z.zpds = z.begins = z.endReports = z.stored = z.nonZero = z.draws = z.countedOnly = z.segments = z.maxCount = 0;
            z.unmatched = z.dropped = z.lost = z.unavailable = 0;
        }
    }
    runtime::video::keepbusy::OnSwap(swap);  // FM4_GPU_KEEP_BUSY_CYCLE: the experiment's OFF/ON switches
    if (vertexWatches.size() > 65536)  // FM4_VERTEX_REUSE / FM4_INDEX_REUSE hot-range bookkeeping, bounded
        vertexWatches.clear();
    if (hotIndices.size() > 65536)
        hotIndices.clear();
    DiagConsumerReport(swap);  // FM4_DIAG_CONSUMER=2 (diagnostic)
    if (!enabled)
        return;
    Require(guard->Begin("present"));
    const FrontBuffer f = DecodeFrontBuffer(fetch, physical, width, height);
    if (!f.ok)
        Stop(StopCode::Unimplemented, "renderer: swap %llu: %s", (unsigned long long)swap, f.error.c_str());
    if (width != kDisplayWidth || height != kDisplayHeight)
        Stop(StopCode::Unimplemented, "renderer: swap %llu: a %ux%u front buffer (only %ux%u)", (unsigned long long)swap, width, height,
             kDisplayWidth, kDisplayHeight);
    uint32_t guest = 0;
    if (!runtime::PhysicalToGuest(f.physical, guest))
        Stop(StopCode::Unimplemented, "renderer: swap %llu: front buffer %08X is in no view", (unsigned long long)swap, f.physical);
    // The frame's recorded work (draws, and the resolves that write guest memory, render_hooks.h) must
    // complete before the front buffer is read: submit and retire the open slot first (FlushSlot; the
    // resolves already flushed their own work). Bounded waits only, in unit 4's one slot.
    FlushSlot(0, FlushCause::Swap);
    const bool windowed = options.mode == Mode::Window;
    // FM4 Recomp (M9, live resolution, step 2): a change of the internal resolution asked for while the game runs is made here, after
    // the frame's work completed and before anything below decides how this swap is presented (those decisions name buffers of the
    // display image's size). Nothing without a request.
    if (const char* next = runtime::video::diag::ResCycleAt(swap); next != nullptr)  // FM4_DIAG_RES_CYCLE (diagnostic)
        if (!RequestInternalRes(next))
            std::fprintf(stderr, "[diag res] swap %llu: the request for %s was not taken (this run cannot switch)\n", (unsigned long long)swap, next);
    if (resClearsCarried)  // the swap after a switch: a pending clear no target took back in a whole frame is dropped, never applied later
    {
        resClearsCarried = false;
        if (const size_t dropped = edramHooks != nullptr ? edramHooks->dropCarriedClears() : 0; dropped != 0)
            std::fprintf(stderr, "[renderer] resolution: %zu pending clear(s) carried over the switch were not taken back by swap %llu and are dropped\n",
                         dropped, (unsigned long long)swap);
    }
    if (resRequested.load(std::memory_order_relaxed) != 0)
        ServeResolutionRequest(swap, windowed);
    // FM4 Recomp (M7 CPU cost; the PWL gamma is capture-only work): the detiled,
    // gamma-corrected display image is consumed only by a capture or the window. Without either, Present keeps every check (the
    // GPU_STOP check, the front-buffer decode, FlushSlot's bounded completion of the frame, the DC_LUTA_CONTROL stop and the ramp
    // log) and skips the staging buffer, the 3.7 MB detile, the PWL pass over 921,600 pixels and the second submission with its
    // fence wait (~3.5 ms of command-processor time a swap).
    // FM4_DIAG_PRESENT_ALWAYS=1 (diagnostic): an offscreen run makes the display image at every swap, as a window does, so the
    // present path's GPU time shows in offscreen measurements (no PNG is written for it).
    static const bool presentAlways = [] { const char* v = std::getenv("FM4_DIAG_PRESENT_ALWAYS"); return v != nullptr && v[0] == '1' && v[1] == '\0'; }();
    const bool image = windowed || FrontBufferCaptured(swap) || presentAlways;
    // FM4 Recomp (FM4_ASYNC_RETIRE): a windowed swap that is not captured records its present part (upload, readback copy) into an
    // idle part slot and does not wait for it: the completion worker hands the frame to the window once the part completed
    // (async_retire.h A2). The frame's own parts were retired by the drain above (the front buffer is read on the CPU); the next
    // drain, at the latest the next swap's, retires the present part before the display image is written again.
    // FM4 Recomp (FM4_GPU_PRESENT, gpu_present.h): the tracked front-buffer entry whose resolve words are the bytes the CPU path would
    // detile now (after the drain above), or -1: today's CPU path. FM4_GPU_PRESENT_CHECK keeps a GPU-presented swap synchronous so its
    // readback can be compared after the retire.
    const int gpuFront = image ? GpuPresentChoose(f, swap) : -1;
    // FM4 Recomp (M9): not with the display image at the internal resolution (the check compares 1280x720 words).
    const bool gpuCheck = gpuFront >= 0 && GpuPresentCheck() && !runtime::video::scale::OutputScaled();
    // FM4 Recomp (M9): the entry whose scaled words are the whole picture (ScaledFrontEntry), whatever the 1x words' state.
    const int scaledEntry = image ? ScaledFrontEntry(f) : -1;
    const bool onGpu = gpuFront >= 0 || scaledEntry >= 0;
    const bool presentAsync = retireBackend && windowed && !FrontBufferCaptured(swap) && !gpuCheck;
    if (retireBackend && windowed)
        RetireParts(UINT64_MAX, true);  // an earlier present part when no draw opened the slot since: its hand-over happens before BeginWrite
    if (presentAsync)
    {
        if (ReleaseReplacedTargets() && edramHooks != nullptr && edramHooks->releaseReplaced != nullptr)
            edramHooks->releaseReplaced(ForgetFramebuffersOf);  // FM4 Recomp (M9, memory): the same completion guarantee as the eviction below
        EvictIdleTextures();  // every submission is confirmed complete; the present part samples no texture
    diag::TriggerAtRetire(swap, [](const std::string& c) {  // FM4_DIAG_TRIGGER (diagnostic): a live session's probe
        if (c == "stale-textures")
            std::fprintf(stderr, "[diag] stale-textures: %zu image(s) to re-hash\n", MarkTexturesStale());
        else if (c == "stale-vertices" && vertexCache)
            vertexCache->ForgetAll(), std::fprintf(stderr, "[diag] stale-vertices: the vertex cache forgot its windows\n");
        else
            std::fprintf(stderr, "[diag] trigger: unknown command '%s'\n", c.c_str());
    });
    }
    const auto presentStart = std::chrono::steady_clock::now();
    if (!image)
        ApplyDisplayGamma(base, nullptr, 0, swap);
    else
    {
    // A plain CPU read while the title may still write the buffer: the guarded read
    // (write watch, external host memory) is unit 7.
    // FM4 Recomp (FM4_GPU_PRESENT): a GPU-presented swap reads nothing on the CPU: the ramp's tables only (the same checks and log).
    Transient staging;
    runtime::gpu::gamma::Ramp ramp;
    uint32_t lut[3][1024];
    if (!onGpu)
    {
    staging = MakeBuffer(VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    uint32_t* linear = static_cast<uint32_t*>(staging.mapped);
    if (!Detile32(base + guest, TiledSize32(f.pitch, height), linear, width, height, f.pitch))
        Stop(StopCode::Unimplemented, "renderer: swap %llu: front buffer shorter than its tiled size", (unsigned long long)swap);
    if (f.swapRedBlue)
        for (size_t i = 0; i < size_t(width) * height; i++)
            linear[i] = SwapRedBlue1010102(linear[i]);
    ApplyDisplayGamma(base, linear, size_t(width) * height, swap);
    }
    else
    {
        ApplyDisplayGamma(base, nullptr, 0, swap, &ramp);
        runtime::gpu::gamma::BuildPwlLut(ramp.pwl, lut);
    }
    if (windowed)  // the display image is written below: never while the presenter blits from it (bounded wait)
        if (HandOver().BeginWrite(PresenterWaitBoundNs(guard->FenceTimeoutNs())) == window::FrameHandOver::Write::TimedOut)
            Stop(StopCode::Setup, "renderer: swap %llu: the window's presenter still reads the display image after %llu ms", (unsigned long long)swap,
                 (unsigned long long)(PresenterWaitBoundNs(guard->FenceTimeoutNs()) / 1000000));
    if (windowed)
        SetPresenterStretch(extendedShownFactor);  // Extended View: this frame's shape in the window
    uint32_t presentSlot = 0;
    if (presentAsync)  // FM4_ASYNC_RETIRE: an idle part slot (command buffer and fence only; slot 0's ring and pool stay untouched)
    {
        presentSlot = kSlots;
        for (uint32_t k = 1; k < kSlots && presentSlot == kSlots; k++)
            if (guard->SlotState(k) == gpu::SubmissionLifetime<Transient>::State::Idle)
                presentSlot = k;
        if (presentSlot == kSlots)
            Stop(StopCode::Setup, "renderer: swap %llu: no idle part slot for the present after the frame's drain", (unsigned long long)swap);
    }
    const VkCommandBuffer c = presentAsync ? BeginSlot(presentSlot) : OpenSlot(0);  // a fresh recording: the frame's draws completed above
    if (!onGpu && !guard->Hold(presentSlot, staging))
        Stop(StopCode::Setup, "renderer: the staging buffer cannot be held (slot not idle)");
    // FM4 Recomp (M9, FM4_INTERNAL_RES): with the display image at the internal resolution, the swap is the scaled front buffer when
    // every row's latest resolve gave scaled words; otherwise its 1x words (the GPU's or the CPU's) enlarged, counted and named.
    const bool scaledOut = runtime::video::scale::OutputScaled();
    const bool scaledFront = scaledOut && scaledEntry >= 0;
    if (onGpu)  // FM4_GPU_PRESENT: the display words on the GPU (gpu_present.h), copied into the display image as the staging is
        RecordGpuPresent(c, size_t(scaledFront ? scaledEntry : gpuFront), lut, f.swapRedBlue, scaledFront);
    if (scaledOut)
    {
        if ((scaledFront ? scaledPresents : upscaledPresents)++ == 0)
            std::fprintf(stderr, "[renderer] display image %ux%u: swap %llu %s (first time; counted from now on)\n", displayWidth, displayHeight,
                         (unsigned long long)swap,
                         scaledFront ? "from the scaled front buffer"
                         : gpuFront >= 0 ? "from the 1x front buffer enlarged: its resolves' scaled words do not cover it, or it was written since"
                                         : "from the 1x front buffer enlarged: no resolve words (presented on the CPU)");
    }
    // FM4 Recomp (M9, FM4_POST_AA): the picture goes into the pass's source image; the pass then writes the display image.
    const int aaLevel = postAa.on ? postAaLevel.load() : 0;
    const VkImage target = aaLevel != 0 ? postAa.source : display;
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    if (scaledOut && !scaledFront)
    {
        ImageBarrier(c, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, display1x);
        copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(c, !onGpu ? staging.buffer : gpuPresent.out.buffer, display1x, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        ImageBarrier(c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, display1x);
        ImageBarrier(c, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, target);
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(width), int32_t(height), 1};
        blit.dstOffsets[1] = {int32_t(displayWidth), int32_t(displayHeight), 1};
        vkCmdBlitImage(c, display1x, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    else
    {
    ImageBarrier(c, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, target);
    copy.imageExtent = {scaledFront ? displayWidth : width, scaledFront ? displayHeight : height, 1};
    vkCmdCopyBufferToImage(c, !onGpu ? staging.buffer : gpuPresent.out.buffer, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    if (aaLevel != 0)
        RecordPostAa(c, postAa.pair && KeepSwap(swap), !presentAsync,
                     aaLevel == 1 ? runtime::video::postaa::kSoft : aaLevel == 3 ? runtime::video::postaa::kStrong : runtime::video::postaa::kStandard);
    RecordReadback(c);
    if (const uint64_t every = runtime::video::diag::HdrToggleEvery(); every != 0 && swap % every == 0)  // FM4_DIAG_HDR_TOGGLE (diagnostic)
        SetHdrPreference(!hdrWanted.load());
    // FM4 Recomp (M9, FM4_HDR switched on while the game runs): the pass's objects at the first swap that wants them; in a window
    // the presenter then rebuilds its swapchain once (this thread holds the hand-over's write: it is not presenting).
    if (!hdrUnit.on && HdrEffective())
    {
        CreateHdr(swap);
        if (hdrUnit.on && windowed)
            SetPresenterHdr(hdrUnit.image);
    }
    if (hdrUnit.on && windowed && PresenterHdrRefused())  // no HDR swapchain for this window: standard range from here (objects kept)
    {
        hdrUnit.on = false, hdrUsable = false;
        hdrUnavailable = "the window's surface offers no HDR format";
        std::fprintf(stderr, "[renderer] HDR output off at swap %llu: %s; standard range\n", (unsigned long long)swap, hdrUnavailable.load());
    }
    const bool hdrRead = hdrUnit.on && hdrUnit.offscreen && KeepSwap(swap);
    if (hdrUnit.on)  // FM4 Recomp (M9, FM4_HDR): the linear-light picture for the HDR swapchain (offscreen: wherever the display image is made)
        RecordHdr(c, hdrRead);
    if (presentAsync)  // FM4_ASYNC_RETIRE: not timed (FM4_GPU_TIMESTAMPS measures slot 0's parts), not waited for
    {
        Check(vkEndCommandBuffer(c), "vkEndCommandBuffer");
        presentSwapTag = swap;
        SubmitSlot(presentSlot);
        presentSwapTag = 0;
        asyncPresentsSinceSwap++;
    }
    else
    {
    StampEnd(0);
    Check(vkEndCommandBuffer(c), "vkEndCommandBuffer");
    SubmitSlot(0);
    RetireSlot(0);  // one bounded wait per swap: the one-frame throttle (execution model §2); then the draws' xe_error dwords
    if (gpuCheck)  // FM4_GPU_PRESENT_CHECK (diagnostic): the readback is complete; the front buffer's write-back landed before the swap
        GpuPresentCompare(base + guest, f, ramp, swap);
    }
    }
    flushNsSinceSwap[size_t(FlushCause::Swap)] += NsSince(presentStart);
    if (!presentAsync)  // FM4_ASYNC_RETIRE: evicted before the present part was recorded
    {
        if (ReleaseReplacedTargets() && edramHooks != nullptr && edramHooks->releaseReplaced != nullptr)
            edramHooks->releaseReplaced(ForgetFramebuffersOf);  // FM4 Recomp (M9, memory): the same completion guarantee as the eviction below
        EvictIdleTextures();  // FM4 Recomp: every submission confirmed complete just above (GPU safety: no in-flight use)
    diag::TriggerAtRetire(swap, [](const std::string& c) {  // FM4_DIAG_TRIGGER (diagnostic): a live session's probe
        if (c == "stale-textures")
            std::fprintf(stderr, "[diag] stale-textures: %zu image(s) to re-hash\n", MarkTexturesStale());
        else if (c == "stale-vertices" && vertexCache)
            vertexCache->ForgetAll(), std::fprintf(stderr, "[diag] stale-vertices: the vertex cache forgot its windows\n");
        else
            std::fprintf(stderr, "[diag] trigger: unknown command '%s'\n", c.c_str());
    });
    }
    Capture(swap, "front buffer");  // FM4_ASYNC_RETIRE: never captured (presentAsync), so the readback buffer is not read
    if (hdrUnit.on && hdrUnit.offscreen && KeepSwap(swap))  // FM4_HDR without a window: the pass's result, read back above
        HdrSummary(swap);
    if (runtime::video::mem::Wanted() && KeepSwap(swap))  // FM4_DIAG_MEMORY (diagnostic): the allocations at each kept capture (a matched scene)
        std::fputs(runtime::video::mem::Global().Report("swap " + std::to_string(swap), 14).c_str(), stderr);
    if (windowed && !presentAsync && !HandOver().Offer(swap))  // the main thread blits it to the swapchain and presents (window_present.cpp)
        Stop(StopCode::Setup, "renderer: swap %llu: the window hand-over was not idle after BeginWrite (contract breach)", (unsigned long long)swap);
    std::fprintf(stderr, "[gpu] swap %llu presented %s; since the last swap: %llu draw(s) rendered, %llu no-write draw(s) %s, %llu resolve(s), "
                         "%llu shader load(s); %llu bytes of the slot's ring used, %llu ring flush(es); %s\n",
                 (unsigned long long)swap, windowed ? "offscreen and handed to the window" : "offscreen", (unsigned long long)drawsSinceSwap, (unsigned long long)noWriteSinceSwap,
                 kNoWriteDraws == NoWriteDraws::Skip ? "recorded as nothing (draw_record.h kNoWriteDraws)" : "(none allowed)",
                 (unsigned long long)resolvesSinceSwap, (unsigned long long)shadersSinceSwap, (unsigned long long)slots[0].allocator->Used(),
                 (unsigned long long)ringFlushesSinceSwap, TextureUnitSummary().c_str());
    runtime::av::Swap(swap, drawsSinceSwap);  // FM4_DIAG_AV_LATENCY (diagnostic): the UI transition after an A press
    {
        std::string timing;
        uint64_t waits = 0;
        for (size_t k = 0; k < size_t(FlushCause::Count); k++)
        {
            waits += flushNsSinceSwap[k];
            if (flushCountSinceSwap[k] || flushNsSinceSwap[k])
            {
                char part[96];
                std::snprintf(part, sizeof part, "; %s %llu x %.1f ms", kFlushCauseNames[k], (unsigned long long)flushCountSinceSwap[k],
                              flushNsSinceSwap[k] / 1e6);
                timing += part;
            }
            flushCountSinceSwap[k] = flushNsSinceSwap[k] = 0;
        }
        const char* separator = " (CPU sync by packet:";
        for (uint32_t op = 0; op < 128; op++)
            if (syncFlushesByPacketSinceSwap[op])
            {
                char part[48];
                std::snprintf(part, sizeof part, "%s %02X x %llu", separator, op, (unsigned long long)syncFlushesByPacketSinceSwap[op]);
                timing += part, separator = ",";
                syncFlushesByPacketSinceSwap[op] = 0;
            }
        if (*separator == ',')
            timing += ")";
        char gpuBusy[1200] = "";  // FM4 Recomp: room for the FM4_GUEST_MEMORY_GPU fields
        const double diagGpuBusyMs = gpuBusyNsSinceSwap / 1e6;  // FM4_DIAG_SLOW_SUBMIT (diagnostic)
        if (timestampPeriodNs > 0)  // FM4_GPU_TIMESTAMPS (diagnostic)
            std::snprintf(gpuBusy, sizeof gpuBusy, "; GPU busy %.1f ms in %llu submission(s)", gpuBusyNsSinceSwap / 1e6, (unsigned long long)gpuTimedSinceSwap);
        if (ThreadCpuDiag())  // FM4_DIAG_THREAD_CPU (diagnostic): CPU time by thread role and the command processor's idle time
        {
            const runtime::ThreadCpu cpu = runtime::ThreadCpuSince();
            const uint64_t idle = runtime::gpu::CpIdleNs();
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; CPU: guest %.1f ms (%u threads), cp %.1f ms, other %.1f ms; cp idle %.1f ms",
                          cpu.guestMs, cpu.guestThreads, cpu.cpMs, cpu.otherMs, (idle - cpIdleAt) / 1e6);
            cpIdleAt = idle;
        }
        if (DeferFences())  // FM4_DEFER_FENCES: waits a queued store satisfied (no flush)
        {
            const uint64_t self = runtime::gpu::SelfWaits();
            const uint64_t immediate = runtime::gpu::ImmediateWaits();
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; self-waits %llu, immediate waits %llu",
                          (unsigned long long)(self - selfWaitsAt), (unsigned long long)(immediate - immediateWaitsAt));
            const uint64_t mem = runtime::gpu::MemWaits(), synced = runtime::gpu::SyncedWaits(), memNs = runtime::gpu::MemWaitNs();
            const uint64_t interrupts = runtime::gpu::ImmediateInterrupts(), grace = runtime::gpu::GraceSyncs();
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy),
                          ", memory waits %llu (%llu synced, %llu after the grace), %.1f ms; immediate interrupts %llu",
                          (unsigned long long)(mem - memWaitsAt), (unsigned long long)(synced - syncedWaitsAt), (unsigned long long)(grace - graceSyncsAt),
                          (memNs - memWaitNsAt) / 1e6, (unsigned long long)(interrupts - immediateInterruptsAt));
            selfWaitsAt = self, immediateWaitsAt = immediate, memWaitsAt = mem, syncedWaitsAt = synced, memWaitNsAt = memNs;
            immediateInterruptsAt = interrupts, graceSyncsAt = grace;
        }
        if (VertexReuse())  // FM4_VERTEX_REUSE: vertex window bytes copied into the ring and read from a copy it already held
        {
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; vertices copied %.1f MB, reused %.1f MB",
                          vertexCopiedSinceSwap / 1048576.0, vertexReusedSinceSwap / 1048576.0);
            vertexCopiedSinceSwap = vertexReusedSinceSwap = 0;
            // FM4 Recomp (M7, vertex reuse across submissions): the ring's laps (each drops FM4_VERTEX_REUSE's ring copies), and with
            // FM4_VERTEX_CACHE_MB the cache's chunk recycles and the flushes it needed (FlushCause::VertexCache).
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; ring laps %llu", (unsigned long long)ringLapsSinceSwap);
            ringLapsSinceSwap = 0;
            if (vertexCache)
            {
                std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; vertex cache: %llu chunk recycle(s), %llu flush(es)",
                              (unsigned long long)(vertexCache->Recycles() - vertexCacheRecyclesAt), (unsigned long long)vertexCacheFlushesSinceSwap);
                vertexCacheRecyclesAt = vertexCache->Recycles(), vertexCacheFlushesSinceSwap = 0;
            }
        }
        // FM4 Recomp (M7, FM4_GUEST_MEMORY_GPU; gpu_read_guard.h): stores that waited for a GPU read (guest and host threads in the
        // fault handler or a heap or system-call write, plus the command processor's own stores), their total time, and how many
        // found the read's part still open (a forced submit); then the vertex windows read directly and the draws snapshotted. The
        // guard's submits and retires are the flush cause "write watch" above.
        if (GpuReadGuard* g = GuestMemoryGpuGuard())
        {
            const GpuReadGuard::Stats& st = g->GetStats();
            const uint64_t waitsNow = st.waits.load(), forcedNow = st.forcedSubmits.load(), nsNow = st.waitNs.load();
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy),
                          "; write-watch waits %llu x %.1f ms (%llu forced a submit); direct vertex reads %llu window(s) %.1f MB, %llu draw(s) "
                          "snapshot (%llu late)",
                          (unsigned long long)(waitsNow - guardWaitsAt + cpGuardWaitsSinceSwap), (nsNow - guardWaitNsAt + cpGuardNsSinceSwap) / 1e6,
                          (unsigned long long)(forcedNow - guardForcedAt + cpGuardForcedSinceSwap), (unsigned long long)directWindowsSinceSwap,
                          directBytesSinceSwap / 1048576.0, (unsigned long long)snapshotDrawsSinceSwap, (unsigned long long)lateSnapshotsSinceSwap);
            if (g->HotPages() != 0 || hotSnapshotsSinceSwap != 0)  // FM4 Recomp (M7, FM4_GUARD_HOT)
                std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy),
                              "; hot pages %llu (+%llu ahead), %llu draw(s) copied for them; command-processor store waits %llu in all",
                              (unsigned long long)g->HotPages(), (unsigned long long)g->RingAheadPages(), (unsigned long long)hotSnapshotsSinceSwap,
                              (unsigned long long)cpGuardWaitsTotal);
            uint32_t hotPage = 0;
            uint64_t hotReader = 0;
            while (g->HotLogEntry(hotLogged, hotPage, hotReader))
            {
                std::fprintf(stderr, "[gpu] write-watch hot page %llu: physical %08X (a store waited for GPU part %llu), swap %llu\n",
                             (unsigned long long)hotLogged + 1, hotPage, (unsigned long long)hotReader, (unsigned long long)swap);
                hotLogged++;
            }
            hotSnapshotsSinceSwap = 0;
            guardWaitsAt = waitsNow, guardForcedAt = forcedNow, guardWaitNsAt = nsNow;
            cpGuardWaitsSinceSwap = cpGuardForcedSinceSwap = cpGuardNsSinceSwap = 0;
            directWindowsSinceSwap = directBytesSinceSwap = snapshotDrawsSinceSwap = lateSnapshotsSinceSwap = 0;
        }
        if (IndexReuse())  // FM4_INDEX_REUSE: indexed draws whose host indices were kept, and conversions
        {
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; indices reused %llu, converted %llu; hot ranges not watched %llu",
                          (unsigned long long)indexHitsSinceSwap, (unsigned long long)indexMissesSinceSwap, (unsigned long long)hotSkipsSinceSwap);
            indexHitsSinceSwap = indexMissesSinceSwap = hotSkipsSinceSwap = 0;
        }
        if (descriptorCacheOn)  // FM4_DESCRIPTOR_CACHE: draws that reused a set the slot held, and sets written
        {
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; descriptor sets reused %llu, written %llu",
                          (unsigned long long)descriptorHitsSinceSwap, (unsigned long long)descriptorMissesSinceSwap);
            descriptorHitsSinceSwap = descriptorMissesSinceSwap = 0;
        }
        if (PartialRetire())  // FM4_PARTIAL_RETIRE
        {
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; %llu partial retire(s), %llu forced split(s), %llu after resolves",
                          (unsigned long long)partialRetiresSinceSwap, (unsigned long long)partialSplitsSinceSwap, (unsigned long long)splitsAfterResolveSinceSwap);
            partialRetiresSinceSwap = partialSplitsSinceSwap = splitsAfterResolveSinceSwap = 0;
        }
        if (OverwriteClaim())  // FM4_OVERWRITE_CLAIM
        {
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; %llu overwrite claim(s)", (unsigned long long)overwriteClaimsSinceSwap);
            overwriteClaimsSinceSwap = 0;
        }
        if (dev.pushDescriptors)  // FM4_PUSH_DESCRIPTORS
        {
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; %llu pushed descriptor set(s)", (unsigned long long)pushedSetsSinceSwap);
            if (pushTemplatesSinceSwap != 0)  // FM4_PUSH_TEMPLATE
                std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), " (%llu by template)", (unsigned long long)pushTemplatesSinceSwap);
            pushedSetsSinceSwap = 0, pushTemplatesSinceSwap = 0;
        }
        if (ClearDraws())  // FM4_CLEAR_DRAWS: clear-class draws kept as pending clears of their targets instead of drawn
        {
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; %llu clear draw(s) deferred", (unsigned long long)clearDrawsDeferredSinceSwap);
            clearDrawsDeferredSinceSwap = 0;
        }
        if (SubsetMerge())  // FM4_SUBSET_MERGE
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; %llu subset draw(s) kept in the open pass",
                          (unsigned long long)subsetMergesSinceSwap), subsetMergesSinceSwap = 0;
        if (transferSkipStats != nullptr)  // FM4_TRANSFER_SKIP_UNWRITTEN (render_hooks.h)
        {
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; %llu transfer(s) skipped (%llu tiles unwritten since lent)",
                          (unsigned long long)(transferSkipStats->transfers - transferSkipsAtSwap.transfers),
                          (unsigned long long)(transferSkipStats->tiles - transferSkipsAtSwap.tiles));
            transferSkipsAtSwap = *transferSkipStats;
        }
        if (ClearValueOwnership())  // FM4 Recomp (M7 GPU cost, FM4_CLEAR_VALUE_OWNERSHIP): transfers recorded as clears of the destination (edram_vk.h)
        {
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; %llu value transfer(s) of %llu tile(s)",
                          (unsigned long long)clearValueTransfersSinceSwap, (unsigned long long)clearValueTilesSinceSwap);
            clearValueTransfersSinceSwap = clearValueTilesSinceSwap = 0;
        }
        if (SplitDraws() != 0)  // FM4_SUBMIT_SPLIT: parts submitted without waiting
        {
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; %llu split submission(s)", (unsigned long long)splitsSinceSwap);
            if (SubmitIfIdleDraws() != 0)  // FM4_SUBMIT_IF_IDLE
                std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), " (%llu when the GPU was idle)", (unsigned long long)idleSplitsSinceSwap);
            splitsSinceSwap = idleSplitsSinceSwap = 0;
        }
        if (DiagVsTally() && swap % 300 == 0 && !vsTally.empty())  // FM4_DIAG_VS_TALLY (diagnostic): per swap over the last 300
        {
            std::vector<std::pair<uint64_t, std::pair<std::string, uint64_t>>> sorted;
            for (const auto& [k, t] : vsTally)
                sorted.push_back({t.vertices, k});
            std::sort(sorted.rbegin(), sorted.rend());
            std::fprintf(stderr, "[diag vs] swaps %llu-%llu, per swap, by vertices:", (unsigned long long)(swap - 299), (unsigned long long)swap);
            for (size_t i = 0; i < sorted.size() && i < 25; i++)
            {
                const VsTally& t = vsTally[sorted[i].second];
                std::fprintf(stderr, "%s %.0f vertices in %.1f draws vs_%016llx (%zu fetch slots) on %s", i ? ";" : "", t.vertices / 300.0, t.draws / 300.0,
                             (unsigned long long)sorted[i].second.second, t.fetchSlots, sorted[i].second.first.c_str());
            }
            std::fprintf(stderr, "\n");
            vsTally.clear();
        }
        if (DiagSubsetPasses() && swap % 300 == 0)  // FM4_DIAG_SUBSET_PASSES (diagnostic)
        {
            std::fprintf(stderr, "[diag subset] swaps %llu-%llu: draw passes ended for another target: %llu subset, %llu same set, %llu superset, %llu other\n",
                         (unsigned long long)(swap - 299), (unsigned long long)swap, (unsigned long long)subsetEnds[0], (unsigned long long)subsetEnds[1],
                         (unsigned long long)subsetEnds[2], (unsigned long long)subsetEnds[3]);
            subsetEnds[0] = subsetEnds[1] = subsetEnds[2] = subsetEnds[3] = 0;
        }
        if (PassTimes())  // FM4_DIAG_PASS_TIMES (diagnostic)
        {
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy),
                          "; draw passes GPU %.1f ms in %llu, unit passes %.1f ms, between passes %.1f ms", passGpuNsSinceSwap / 1e6,
                          (unsigned long long)passTimedSinceSwap, unitGpuNsSinceSwap / 1e6, gapGpuNsSinceSwap / 1e6);
            passGpuNsSinceSwap = passTimedSinceSwap = gapGpuNsSinceSwap = unitGpuNsSinceSwap = 0;
            // FM4_DIAG_PASS_TALLY=<swaps>: the tally interval (default 300; 60 separates the race from what precedes it).
            static const uint64_t tallyEvery = [] {
                const char* v = std::getenv("FM4_DIAG_PASS_TALLY");
                const long n = v != nullptr ? std::atol(v) : 0;
                return n >= 10 && n <= 10000 ? uint64_t(n) : 300ull;
            }();
            if (swap % tallyEvery == 0 && !passTally.empty())
            {
                std::vector<std::pair<uint64_t, std::string>> sorted;
                for (const auto& [kind, v] : passTally)
                    sorted.push_back({v.first, kind + " (" + std::to_string(v.second) + " passes)"});
                std::sort(sorted.rbegin(), sorted.rend());
                std::fprintf(stderr, "[diag passes] swaps %llu-%llu, GPU ms by draw pass kind:", (unsigned long long)(swap - tallyEvery + 1), (unsigned long long)swap);
                for (size_t i = 0; i < sorted.size() && i < 12; i++)
                    std::fprintf(stderr, "%s %.1f %s", i ? ";" : "", sorted[i].first / 1e6, sorted[i].second.c_str());
                std::fprintf(stderr, "\n");
                passTally.clear();
                const auto print = [&](const char* what, std::map<std::string, std::pair<uint64_t, uint64_t>>& tally, size_t top) {
                    std::vector<std::pair<std::pair<uint64_t, uint64_t>, std::string>> rows;
                    for (const auto& [name, v] : tally)
                        rows.push_back({v, name});
                    std::sort(rows.rbegin(), rows.rend());
                    std::fprintf(stderr, "[diag passes] swaps %llu-%llu, %s:", (unsigned long long)(swap - tallyEvery + 1), (unsigned long long)swap, what);
                    for (size_t i = 0; i < rows.size() && i < top; i++)
                        std::fprintf(stderr, "%s %.1f ms %s (%llu)", i ? ";" : "", rows[i].first.first / 1e6, rows[i].second.c_str(),
                                     (unsigned long long)rows[i].first.second);
                    std::fprintf(stderr, "\n");
                    tally.clear();
                };
                print("GPU ms by pass kind and draws in the pass", passDrawTally, 16);
                print("GPU ms by EDRAM-unit pass kind (transfers, clears, dumps, resident conversions)", unitTally, 20);
                if (edramHooks != nullptr && edramHooks->diagTally != nullptr)  // FM4_DIAG_TRANSFERS (edram_vk.cpp)
                    edramHooks->diagTally(swap - tallyEvery + 1, swap);
                print("GPU ms between two passes of one command buffer, by the jobs recorded there", passGapTally, 16);
                print("passes by what ended them (count)", passEndTally, 16);
            }
        }
        if (PassMerge())  // FM4_PASS_MERGE: render pass instances begun by draws
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; %llu draw render pass(es)", (unsigned long long)passesSinceSwap);
        std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; constants reused %llu draw(s)", (unsigned long long)constantsReusedSinceSwap);
        if (ConstantsPerStage())  // FM4_CONSTANTS_PER_STAGE
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), " (and one stage only: VS %llu, PS %llu)",
                          (unsigned long long)stageConstantsReusedSinceSwap[0], (unsigned long long)stageConstantsReusedSinceSwap[1]);
        constantsReusedSinceSwap = 0, stageConstantsReusedSinceSwap[0] = stageConstantsReusedSinceSwap[1] = 0;
        if (VariantMemoMode() != 0)  // FM4_VARIANT_MEMO
        {
            std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy), "; variant memo %llu hit(s), %llu miss(es), %llu verified, %zu held",
                          (unsigned long long)variantMemoHitsSinceSwap, (unsigned long long)variantMemoMissesSinceSwap,
                          (unsigned long long)variantMemoVerifiedSinceSwap, variantMemo.Size());
            variantMemoHitsSinceSwap = variantMemoMissesSinceSwap = variantMemoVerifiedSinceSwap = 0;
        }
        std::snprintf(gpuBusy + std::strlen(gpuBusy), sizeof gpuBusy - std::strlen(gpuBusy),
                      "; vertex fetch unchecked %llu / checked %llu draw(s) (inside %llu, no slots %llu, invalid %llu, computed %llu, past %llu)",
                      (unsigned long long)fetchUncheckedSinceSwap, (unsigned long long)fetchCheckedSinceSwap, (unsigned long long)fetchProofReasonSinceSwap[0],
                      (unsigned long long)fetchProofReasonSinceSwap[1], (unsigned long long)fetchProofReasonSinceSwap[2], (unsigned long long)fetchProofReasonSinceSwap[3],
                      (unsigned long long)fetchProofReasonSinceSwap[4]);  // FM4_FETCH_UNCHECKED
        fetchUncheckedSinceSwap = fetchCheckedSinceSwap = 0;
        for (uint64_t& n : fetchProofReasonSinceSwap)
            n = 0;
        std::fprintf(stderr, "[gpu] swap %llu timing: %.1f ms since the last swap; submit + wait %.1f ms%s (retire: fences %.1f ms, write-back %.1f ms); texture binds %.1f ms; ring need: textures %.1f MB, "
                             "vertices %.1f MB, indices %.1f MB, fixed blocks %.1f MB; vblank %llu%s\n",
                     (unsigned long long)swap, NsSince(lastSwapEnd) / 1e6, waits / 1e6, timing.c_str(), retireFenceNsSinceSwap / 1e6, retireWriteBackNsSinceSwap / 1e6, textureBindNsSinceSwap / 1e6,
                     ringNeedSinceSwap[0] / 1048576.0, ringNeedSinceSwap[1] / 1048576.0, ringNeedSinceSwap[2] / 1048576.0, ringNeedSinceSwap[3] / 1048576.0,
                     (unsigned long long)runtime::gpu::VblankCount(), gpuBusy);  // FM4 Recomp: guest time (the input script's clock) per swap
        // FM4 Recomp (M7 stability, FM4_SHADER_CACHE; after ReCarbon's every-600-frames saver): new pipelines are written out every
        // 600 swaps on a thread of its own (getting a large cache's data and writing it costs tens of ms: never on this thread).
        if (pipelines && pipelines->PersistentCacheOn() && swap % 600 == 0 && pipelines->GetStats().pipelines != pipelinesAtCacheSave)
        {
            pipelinesAtCacheSave = pipelines->GetStats().pipelines;
            if (!pipelineCacheSaving.exchange(true))
                std::thread([] {
                    pthread_setname_np("fm4 cache save");
                    if (!pipelines->SavePersistentCache())
                        std::fprintf(stderr, "[gpu] pipeline cache: the save failed (the next one retries)\n");
                    pipelineCacheSaving = false;
                }).detach();
        }
        if (diag::SlowSubmitEnabled())  // FM4_DIAG_SLOW_SUBMIT (diagnostic)
        {
            diag::ReportFence((unsigned long long)swap, retireFenceNsSinceSwap / 1e6, diagGpuBusyMs);
            diag::CompileTally& c = diag::Compiles();
            diag::ReportSwap((unsigned long long)swap, NsSince(lastSwapEnd) / 1e6, waits / 1e6, c.glslNs.exchange(0) / 1e6, c.glsl.exchange(0),
                             c.pipelineNs.exchange(0) / 1e6, c.pipelines.exchange(0), c.glslCached.exchange(0), c.translateNs.exchange(0) / 1e6,
                             c.translations.exchange(0), c.dumpNs.exchange(0) / 1e6, c.dumps.exchange(0));
        }
        if (retireBackend)  // FM4_ASYNC_RETIRE: "retire: fences" above is the command processor's wait for the worker
        {
            const AsyncRetireBackend<Transient>::Counters w = retireBackend->TakeCounters();
            std::string blocked;
            for (size_t k = 0; k < size_t(FlushCause::Count); k++)
                if (workerBlockedNsSinceSwap[k] != 0)
                {
                    char part[64];
                    std::snprintf(part, sizeof part, "%s %s %.1f ms", blocked.empty() ? "" : ",", kFlushCauseNames[k], workerBlockedNsSinceSwap[k] / 1e6);
                    blocked += part;
                    workerBlockedNsSinceSwap[k] = 0;
                }
            std::fprintf(stderr, "[gpu] swap %llu async retire: command processor blocked on the worker %.1f ms (%s); worker: %llu part(s), fences %.1f ms, "
                                 "write-backs and present %.1f ms; %llu reap(s), %llu store(s) landed early, %llu present part(s) not waited for\n",
                         (unsigned long long)swap, w.blockedNs / 1e6, blocked.empty() ? "none" : blocked.c_str(), (unsigned long long)w.retired, w.fenceNs / 1e6,
                         w.jobNs / 1e6, (unsigned long long)reapedPartsSinceSwap, (unsigned long long)earlyStoresSinceSwap, (unsigned long long)asyncPresentsSinceSwap);
            reapedPartsSinceSwap = earlyStoresSinceSwap = asyncPresentsSinceSwap = 0;
        }
        if (drain::Enabled())  // FM4_DIAG_DRAIN (diagnostic): this swap's drains
            drain::Swap(swap, drawsSinceSwap);
        gpuBusyNsSinceSwap = gpuTimedSinceSwap = passesSinceSwap = 0;
        retireFenceNsSinceSwap = retireWriteBackNsSinceSwap = 0;
        for (uint64_t& n : ringNeedSinceSwap)
            n = 0;
        textureBindNsSinceSwap = 0;
        lastSwapEnd = std::chrono::steady_clock::now();
    }
    // FM4 Recomp (M7 A/B harness, input_script.h @race): the first swap of 1,000 or more draws is the race start (the front end and
    // the loading screens draw about 100 to 400; a race frame 1,400 to 2,700). Nothing without a @race script.
    if (drawsSinceSwap >= 1000)
        runtime::input::NotifyRaceStart(runtime::gpu::VblankCount());
    runtime::input::NoteSwapDraws(drawsSinceSwap);
    drawsSinceSwap = shadersSinceSwap = noWriteSinceSwap = resolvesSinceSwap = ringFlushesSinceSwap = 0;
}

void Draw(const uint8_t* base, uint32_t opcode, uint32_t vizQueryToken)
{
    if (!enabled)
        return;
    diag::CpDelay();  // FM4_DIAG_CP_DELAY_NS (diagnostic): a CP-time sensitivity test
    const uint64_t draw = drawNumber++;
    // 1. The gate's in-memory state (no file access per draw; GPU_STOP is checked at every Submit, the
    //    single choke point to the queue, execution model §4).
    const gpu::SubmissionGate::View v = gate.Load();
    if (v.stopping || v.stranded)
        Stop(v.stranded ? StopCode::GpuStranded : StopCode::Setup, "renderer: draw %llu after the GPU stopped", (unsigned long long)draw);
    // 2. The draw state from the register file and the current shaders (fail closed).
    const CachedShader* vs = shaders->Current(false);
    const CachedShader* ps = shaders->Current(true);
    if (vs == nullptr || ps == nullptr)
        Stop(StopCode::Unimplemented, "renderer: draw %llu: no %s shader was loaded (IM_LOAD) before it", (unsigned long long)draw, vs ? "pixel" : "vertex");
    // FM4_CP_TRACE: the fetch constants of every texture slot the bound pair samples (the CP logs slots 0-2 of DRAW_INDX only).
    if (runtime::gpu::TraceEnabled())
    {
        uint32_t mask = 0;
        for (const CachedShader* stage : {vs, ps})
            for (const uint32_t s : stage->t.textureSlots)
                mask |= s < 32 ? 1u << s : 0u;
        runtime::gpu::TraceTextures(base, opcode, mask);
    }
    TracePsDraw(base, draw, vs->hash, ps->hash, vs->t);  // FM4_TRACE_PS (diagnostic, ps_trace.h): nothing unless set
    DrawInputs in;
    in.read = ReadRegisterHook;
    in.context = base;
    const bool zpdCounting = zpdUnit.on && !zpdUnit.open.Empty();  // FM4_OCCLUSION_QUERIES: an occlusion query counts this draw
    in.countNoWrite = zpdCounting;
    static const bool bulkConstants = [] {  // opt-in FM4_BULK_CONSTANTS=1 (draw_state.h DrawInputs::registers)
        const char* v = std::getenv("FM4_BULK_CONSTANTS");
        const bool on = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (on)
            std::fprintf(stderr, "[gpu] bulk constant read on (FM4_BULK_CONSTANTS=1): the 2,080 constant registers read byte-swapped in bulk\n");
        return on;
    }();
    if (bulkConstants)
        in.registers = base + runtime::gpu::RegisterAddress(0);  // what ReadRegisterHook reads (kernel/graphics.h Load32 of RegisterAddress)
    in.opcode = opcode;
    in.vizQueryToken = vizQueryToken;
    in.physicalBytes = runtime::kPhysicalSize;
    in.vs = &vs->t;
    in.ps = &ps->t;
    // FM4 Recomp (M7 CPU cost): the decoder takes aluScratch's 8 KB for aluConstants instead of allocating; it goes back at the end of
    // this draw (or into lastConstants, whose previous buffer then becomes the scratch).
    static std::vector<uint32_t> aluScratch;
    in.constantsScratch = &aluScratch;
    // FM4 Recomp (M7 CPU cost): one DrawState for the thread, decoded into (draw_state.h DecodeDrawState's `out` form) so its vectors
    // keep their storage from draw to draw (~5-8 allocations and frees a draw by value). One command-processor thread; Draw never
    // re-enters itself. The same holds for the other `static` per-draw scratch below (windows, the defines, the descriptor key).
    static DrawState d;
    DecodeDrawState(in, d);
    struct ScratchBack
    {
        std::vector<uint32_t>& scratch;
        std::vector<uint32_t>& alu;
        ~ScratchBack() { scratch.swap(alu); }
    } scratchBack{aluScratch, d.aluConstants};
    std::string error;
    switch (d.kind)
    {
    case DrawKind::Refused:
        Stop(StopCode::Unimplemented, "renderer: draw %llu: %s", (unsigned long long)draw, d.error.c_str());
    case DrawKind::Resolve:  // RB_MODECONTROL 6: the EDRAM unit's (render_hooks.h)
    {
        if (edramHooks == nullptr)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: a resolve (RB_MODECONTROL 6) without an EDRAM unit", (unsigned long long)draw);
        // FM4 Recomp: the EDRAM unit holds one pending resolve per submission (edram_vk.cpp Resolve: one staging buffer), so a
        // deferred one is written back before the next is recorded.
        // FM4_RESOLVE_RESIDENT: it holds several while its staging has room (EdramHooks::resolveRoom).
        if (ResolveDefer() && slots[0].open && GuestWritesPending() && !(ResolveResident() && edramHooks->resolveRoom()))
            FlushSlot(0, FlushCause::NextResolve);
        const VkCommandBuffer commands = OpenSlot(0);
        bool resolved;
        {
            const PassActivity activity("resolve");  // FM4_DIAG_PASS_TIMES label
            resolved = edramHooks->resolve(commands, base, d, draw, error);
        }
        if (!resolved)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: resolve: %s", (unsigned long long)draw, error.c_str());
        resolvesSinceSwap++;
        // FM4_SPLIT_AFTER_RESOLVE: the split is deferred to the next draw's entry (SplitSubmit's point, before anything of that draw
        // is recorded). NEVER here: the adoption below records into `commands`; a split here ended and submitted that buffer first
        // (recording into a submitted command buffer stranded a fence and wrote GPU_STOP).
        if (SplitAfterResolve() && PartialRetire() && GuestWritesPending())
            splitAfterResolvePending = true;
        // FM4 Recomp (resolve_resident.h): the textures that take the resolve's words get them now; the others over its range wait.
        if (ResolveResident() && textures != nullptr)
        {
            ResidentResolve last;
            VkImage words = VK_NULL_HANDLE;
            if (edramHooks->lastResolve(last, words))
            {
                RequireRecording(commands, "resolve adoption");
                // FM4 Recomp (M9, FM4_INTERNAL_RES): the same resolve per host pixel, when the EDRAM unit converted it (or null).
                const VkImage hiWords = edramHooks->lastResolveHi != nullptr ? edramHooks->lastResolveHi() : VK_NULL_HANDLE;
                textures->adoptResolve(commands, last, words, hiWords);
                GpuPresentAdopt(commands, last, words, hiWords);  // FM4_GPU_PRESENT: a front buffer's words (gpu_present.h), the same command buffer
            }
        }
        // Every resolve completes before the next draw records: that draw's texture loads read guest memory on
        // the CPU (trace11 samples the video resolve's destination in the same frame). Unconditional, so the
        // order does not depend on the EDRAM unit calling MarkGuestWritten (which only moves the watch's
        // sequences and arms texture_upload.h's pending refusal). Unit 4: one bounded wait per resolve.
        // FM4 Recomp: with FM4_RESOLVE_DEFER the write-back waits for its first reader instead (ResolveDefer).
        if (!ResolveDefer())
            FlushSlot(0, FlushCause::Resolve);
        return;
    }
    case DrawKind::NoWrite:  // the one policy switch (draw_record.h kNoWriteDraws; decision pending)
        if (kNoWriteDraws == NoWriteDraws::Stop)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: %s of %u indices writes neither colour nor depth nor stencil (kNoWriteDraws = Stop)",
                 (unsigned long long)draw, PrimitiveTypeName(d.primitiveType), d.indices.count);
        noWriteSinceSwap++;
        if (zpdCounting && zpdUnit.untested++ == 0)  // FM4_OCCLUSION_QUERIES: nothing to draw it into
            std::fprintf(stderr, "[zpd] draw %llu inside an occlusion query writes nothing and tests no depth: not counted (named once)\n",
                         (unsigned long long)draw);
        return;
    case DrawKind::Draw: case DrawKind::DepthOnly:
        break;
    }
    // 3. The host indices, then the loop-work cap (vertex invocations = what the host draws, pixel
    //    invocations = the scissor's area), before anything is recorded.
    // FM4 Recomp (FM4_RESOLVE_DEFER): an index buffer a pending resolve writes is read after its write-back.
    if (ResolveDefer() && GuestWritesPending() && d.indices.indexed && GuestWritesOverlap(d.indices.physical, d.indices.bytes) &&
        !RetireUpTo(GuestWritesPendingSerial(d.indices.physical, d.indices.bytes), FlushCause::Consumer))  // FM4_PARTIAL_RETIRE first
    {
        DiagConsumer("index buffer", d.indices.physical, d.indices.bytes, nullptr);
        FlushSlot(0, FlushCause::Consumer);
    }
    HostIndices built;
    const HostIndices* chosen = &built;
    IndexKey indexKey{};
    bool haveIndexKey = false, indexWatched = false;
    uint64_t indexSequence = 0;
    if (IndexReuse() && d.indices.indexed)  // FM4_INDEX_REUSE: every input of BuildHostIndices
    {
        const IndexBuffer& ib = d.indices;
        indexKey = IndexKey{{ib.physical, ib.bytes, ib.is32Bit, ib.endian, ib.count, ib.restart, ib.resetIndex, d.indexOffset, d.minIndex, d.maxIndex,
                             uint32_t(d.hostPrimitive), 0}};
        haveIndexKey = true;
        const auto it = indexCache.find(indexKey);
        if (it != indexCache.end() && GuestRangeUnwritten(ib.physical, ib.bytes, it->second.sequence))
            chosen = &it->second.host, indexHitsSinceSwap++;
        else
        {
            uint64_t& hotUntil = hotIndices[indexKey];
            if (it != indexCache.end() && HotSkip())  // written since it was watched: hot (see WatchWorthIt)
            {
                indexCacheBytes -= std::min<uint64_t>(indexCacheBytes, it->second.host.indices.size() * 4 + sizeof indexKey);
                hotUntil = renderSwap + kHotSwaps, indexCache.erase(it);
            }
            if (renderSwap >= hotUntil)
                indexWatched = WatchGuestRange(ib.physical, ib.bytes, indexSequence);  // protect, then the sequence, then the read
            else
                hotSkipsSinceSwap++;
        }
    }
    if (chosen == &built)
    {
        IndexSource source{base, &d.indices, draw};
        if (uint32_t guest = 0; d.indices.indexed && runtime::PhysicalRangeToGuest(d.indices.physical, d.indices.bytes, guest))
            source.direct = base + guest;
        RequireDraw(draw, BuildHostIndices(d, d.indices.indexed ? ReadGuestIndex : nullptr, &source, built));
        if (haveIndexKey)
        {
            indexMissesSinceSwap++;
            if (indexWatched)
            {
                if (indexCacheBytes > kIndexCacheBytes)
                    indexCache.clear(), indexCacheBytes = 0;
                indexCacheBytes += built.indices.size() * 4 + sizeof indexKey;
                // FM4 Recomp (M7 CPU cost): moved into the cache, not copied (the host indices were allocated and copied a second time at
                // every miss); the draw reads them from the entry (a node of the map: its address is stable).
                CachedIndices& cached = indexCache[indexKey];
                cached = CachedIndices{std::move(built), indexSequence};
                chosen = &cached.host;
            }
        }
    }
    const HostIndices& indices = *chosen;
    RequireDraw(draw, GpuGuard<Transient, kSlots>::DrawLoopWork(d.vsLoopRegisters, indices.vertexCount, d.psLoopRegisters, d.pixelInvocations));
    // 4. The shader variants, the pipeline key and the descriptor plan (CPU).
    // FM4 Recomp (M7 CPU cost): VariantsFromDrawState clears the lines and keeps their vectors' storage (3-4 allocations a draw before).
    static ShaderVariantDefines vsDefines, psDefines;
    // FM4 Recomp (opt-in FM4_FETCH_UNCHECKED, M7 GPU cost): the vertex shader's XE_FETCH_UNCHECKED variant has no device write in
    // the vertex stage (the xe_error store cost about a third of the vertex channel); its bounds test and 0 read stay.
    //  1: only a draw whose every vertex fetch is provably inside its slot (draw_state.h VertexFetchProof: no computed index, the
    //     index window within the constant's size) uses it; the flag could not have fired.
    //  2: every draw uses it (the XeErrors stores are a removable safeguard): an
    //     out-of-range fetch still reads 0 inside the buffer, but no longer stops the run. The proof tally in the swap line stays.
    static const int fetchUnchecked = [] {
        const char* v = std::getenv("FM4_FETCH_UNCHECKED");
        const int level = v != nullptr && (v[0] == '1' || v[0] == '2') && v[1] == '\0' ? v[0] - '0' : 0;
        if (level != 0)
            std::fprintf(stderr, "[gpu] fetch unchecked on (FM4_FETCH_UNCHECKED=%d): %s use the vertex shader variant without the error store\n", level,
                         level == 1 ? "draws with provably in-bounds vertex fetches" : "all draws (an out-of-range fetch reads 0 and no longer stops the run)");
        return level;
    }();
    // FM4 Recomp (M7 CPU cost, FM4_VARIANT_MEMO): the proof is pure (draw_state.cpp) and is decided before the defines are built, so the
    // memo key can carry the decision; the tallies are the same either way.
    const FetchProof proof = VertexFetchProof(d, indices.highIndex);
    const bool uncheckedDefine = fetchUnchecked == 2 || (fetchUnchecked == 1 && proof == FetchProof::Inside);
    if (uncheckedDefine)
        fetchUncheckedSinceSwap++;
    else
        fetchCheckedSinceSwap++, fetchProofReasonSinceSwap[uint8_t(proof)]++;
    // FM4 Recomp (M7 CPU cost, opt-in FM4_VARIANT_MEMO, draw_record.h VariantMemo): a draw whose key the memo holds takes the two
    // variants the full path chose for it, without building or hashing define text. A traced draw (FM4_TRACE_PS prints its defines)
    // always takes the full path; FM4_DIAG_ALPHA_QUANT turns the memo off (VariantMemoMode). =2 takes the full path every draw and
    // stops when a held entry disagrees with it.
    // FM4 Recomp (M9, Extended View; kernel/extended_view.h): while the game widens its main view, the HUD's and menus' draws take
    // the vertex shader's XE_OVERLAY_PIN variant (narrowed about the centre, edge vertices kept at the edge). Never from the memo.
    const bool overlay = runtime::view::IsOverlayDraw(d.target.colorFormat, d.target.pitch, d.primitiveType, d.blend.enabled);
    // (the variant only for a draw whose viewport is the whole 1280x720 frame: its vertices are then in the frame's own space)
    const bool overlayFullFrame = d.viewport.scale[0] == 640.0f && d.viewport.scale[1] == -360.0f && d.viewport.offset[0] == 640.0f && d.viewport.offset[1] == 360.0f;
    const bool overlayPin = extendedFrameFactor != 1.0f && overlay && overlayFullFrame;
    // FM4 Recomp (M9, FM4_HDR; hdr_output.h HudGain): while HDR is in effect the same draws take the pixel shader's XE_OVERLAY_GAIN
    // variant, so the HUD's white is standard white after the expansion.
    const bool overlayGain = hdrHudGain < 1.0f && overlay;
    static VariantMemoKey memoKey;  // one command-processor thread (the static scratch rule above)
    const int memoMode = VariantMemoMode();
    const bool memoKeyed = !overlayPin && !overlayGain && memoMode != 0 && !TracePsBindWanted(draw) && VariantMemoKeyFor(d, *vs, *ps, uncheckedDefine, memoKey);
    const VariantMemo::Entry* memoHit = memoKeyed ? variantMemo.Find(memoKey) : nullptr;
    ShaderVariant* vsVariant = nullptr;
    ShaderVariant* psVariant = nullptr;
    if (memoHit != nullptr && memoMode == 1)
        vsVariant = memoHit->vs, psVariant = memoHit->ps, variantMemoHitsSinceSwap++;
    else
    {
        RequireDraw(draw, VariantsFromDrawState(d, *vs, *ps, vsDefines, psDefines));
        if (uncheckedDefine)
            vsDefines.Add("XE_FETCH_UNCHECKED");
        if (overlayPin)
            vsDefines.Add("XE_OVERLAY_PIN");
        if (overlayGain)
        {
            char gain[32];
            std::snprintf(gain, sizeof gain, "XE_OVERLAY_GAIN %.4f", double(hdrHudGain));
            psDefines.Add(gain);
        }
        DiagAlphaQuantize(ps->hash, psDefines);  // FM4_DIAG_ALPHA_QUANT (diagnostic, diag_alpha.h): nothing unless set
        vsVariant = shaders->Variant(*vs, vsDefines, error);
        if (vsVariant == nullptr)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: %s", (unsigned long long)draw, error.c_str());
        psVariant = shaders->Variant(*ps, psDefines, error);
        if (psVariant == nullptr)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: %s", (unsigned long long)draw, error.c_str());
        if (memoHit != nullptr)  // FM4_VARIANT_MEMO=2
        {
            if (memoHit->vs != vsVariant || memoHit->ps != psVariant)
                Stop(StopCode::Unimplemented, "renderer: draw %llu: variant memo verify: vs_%016llx ps_%016llx: the memo holds variants %016llx / %016llx, the "
                                              "defines give %016llx / %016llx",
                     (unsigned long long)draw, (unsigned long long)vs->hash, (unsigned long long)ps->hash, (unsigned long long)memoHit->vs->id,
                     (unsigned long long)memoHit->ps->id, (unsigned long long)vsVariant->id, (unsigned long long)psVariant->id);
            variantMemoVerifiedSinceSwap++;
        }
        else if (memoKeyed)
            variantMemo.Insert(memoKey, vsVariant, psVariant), variantMemoMissesSinceSwap++;
    }
    PipelineKey key;
    RequireDraw(draw, KeyFromDrawState(d, *vs, *ps, vsVariant->id, psVariant->id, key));
    const bool noDepthDiagnostic = DiagNoDepth(ps->hash, key);  // FM4_DIAG_PS_NO_DEPTH (diagnostic, diag_depth.h): nothing unless set
    DiagBlendLog(draw, key, runtime::gpu::VblankCount());  // FM4_DIAG_BLEND_LOG (diagnostic, diag_blend.h): nothing unless set
    if (DiagSkipDraw(ps->hash, draw))                       // FM4_DIAG_PS_SKIP (diagnostic, diag_blend.h): nothing unless set
        return;
    DescriptorPlan plan;
    RequireDraw(draw, PlanDescriptors(*vs, *ps, samplerLimits, plan));
    key.samplerLayout = plan.layoutKey;  // the pipeline layout is the pair's (PipelineCache::Get refuses a key without it)
    // 5. The pipeline: the shader-class allow-list is PipelineCache::Get's first step, before any Vulkan call.
    PipelineCache::Entry pipeline;
    RequireDraw(draw, pipelines->Get(key, *vs, *vsVariant, *ps, *psVariant, pipeline));
    // 6. The render target (the EDRAM unit's images) and its framebuffer.
    if (edramHooks == nullptr)
        Stop(StopCode::Unimplemented, "renderer: draw %llu: a draw into a render target without an EDRAM unit", (unsigned long long)draw);
    Slot& slot = slots[0];
    // The vertex windows (their guest bytes are copied into the ring at step 7). One window must sit in one view.
    static std::vector<VertexWindow> windows;  // FM4 Recomp (M7 CPU cost): PlanVertexWindows clears it; the storage stays (1-2 allocations a draw before)
    uint32_t vertexDwords = 0;
    PlanVertexWindows(d, indices.highIndex, windows, vertexDwords);
    // FM4 Recomp (FM4_RESOLVE_DEFER): vertex windows and textures a pending resolve writes are read after its write-back (one
    // flush, before anything of this draw reads guest memory or is recorded).
    if (ResolveDefer() && GuestWritesPending())
    {
        bool needs = false;
        uint64_t neededSerial = 0;  // FM4_PARTIAL_RETIRE: the newest part any of them waits for
        for (const VertexWindow& w : windows)
            if (w.dwords != 0 && GuestWritesOverlap(w.physical, w.dwords * 4))
                needs = true, DiagConsumer("vertex window", w.physical, w.dwords * 4, nullptr),
                neededSerial = std::max(neededSerial, GuestWritesPendingSerial(w.physical, w.dwords * 4));
        if (textures != nullptr)
            for (const TextureSlot& t : d.textureSlots)
                if (textures->readsPendingWrite(t))
                    needs = true, DiagConsumer("texture", 0, 0, t.words), neededSerial = std::max(neededSerial, textures->pendingWriteSerial(t));
        if (needs && RetireUpTo(neededSerial, FlushCause::Consumer))
            needs = false;
        if (needs)
            FlushSlot(0, FlushCause::Consumer);
    }
    const auto windowBytes = [&](const VertexWindow& w) -> const uint8_t* {
        uint32_t guest = 0, guestEnd = 0;
        if (!runtime::PhysicalToGuest(w.physical, guest) || !runtime::PhysicalToGuest(w.physical + w.dwords * 4 - 1, guestEnd) || guestEnd - guest != w.dwords * 4 - 1)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: vertex fetch slot %u (%08X, %u dwords) is not in one view", (unsigned long long)draw, w.slot,
                 w.physical, w.dwords);
        if (const runtime::Heap* heap = runtime::LookupHeap(guest); heap == nullptr || !heap->IsCommitted(guest, w.dwords * 4))
            Stop(StopCode::Unimplemented, "renderer: draw %llu: vertex fetch slot %u (%08X, %u dwords) reaches uncommitted guest memory", (unsigned long long)draw,
                 w.slot, w.physical, w.dwords);
        return base + guest;
    };
    // A window past its fetch constant's size (VertexWindowDwords: read from guest memory, as Xenia): capped, and named once per
    // (vertex shader, slot) with the draw's index state and, when it reaches past twice the size, the first words past the constant.
    for (size_t k = 0; k < windows.size(); k++)
    {
        const VertexWindow& w = windows[k];
        const VertexSlot& v = d.vertexSlots[k];
        if (w.dwords <= v.sizeWords)
            continue;
        if (w.dwords > kMaxVertexWindowDwords)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: vertex fetch slot %u (%08X, %u dwords) needs a %u-dword window, more than the cap %u",
                 (unsigned long long)draw, w.slot, w.physical, v.sizeWords, w.dwords, kMaxVertexWindowDwords);
        static std::set<std::pair<uint64_t, uint32_t>> named;
        if (!named.insert({vs->hash, w.slot}).second)
            continue;
        std::string past;
        if (w.dwords > 2 * v.sizeWords)
        {
            const uint8_t* bytes = windowBytes(w);
            for (uint32_t i = v.sizeWords; i < std::min(w.dwords, v.sizeWords + 24); i++)
            {
                char word[16];
                std::snprintf(word, sizeof word, " %02X%02X%02X%02X", bytes[4 * i], bytes[4 * i + 1], bytes[4 * i + 2], bytes[4 * i + 3]);
                past += word;
            }
        }
        std::fprintf(stderr, "[draw] draw %llu: vs_%016llx vertex slot %u at %08X reaches past its fetch constant: indices up to %u x stride %u + end %u = %u dwords, "
                             "the constant holds %u (primitive %u, %s, count %u, offset %u, min %u, max %u)%s%s\n",
                     (unsigned long long)draw, (unsigned long long)vs->hash, w.slot, w.physical, indices.highIndex, v.stride, v.end, w.dwords, v.sizeWords,
                     d.primitiveType, d.indices.indexed ? "indexed" : "auto", d.indices.count, d.indexOffset, d.minIndex, d.maxIndex,
                     past.empty() ? "" : "; guest bytes past it:", past.c_str());
    }
    // FM4 Recomp: the rows the bind may claim (X render_target_cache.cc:757-767, EstimateMaxY).
    // A draw whose extent needs its vertices (clipping off, 8192 scissor: FM4's fast clears) is bounded by its exported
    // positions when its vertex shader's position is a plain vertex fetch (draw_record.h BoundTargetHeight); otherwise
    // the scissor bound stands and a bind refusal names why the vertices were not used.
    RenderTarget target = d.target;
    std::string extentNote;
    // FM4_OVERWRITE_CLAIM (draw_record.h FindOverwriteRect): a one-rectangle draw may overwrite its targets' tiles.
    const bool overwriteCandidate = OverwriteClaim() && d.primitiveType == 8 && !d.indices.indexed && d.indices.count == 3;
    OverwriteRect overwrite;
    ClearRequest clearRequest;  // FM4_CLEAR_DRAWS (draw_record.h FindClearDraw): the clear this draw stands for, when deferClear
    bool deferClear = false;
    if (d.extentNeedsVertices || overwriteCandidate)
    {
        const VertexWindow* positionWindow = nullptr;
        for (const VertexWindow& w : windows)
            if (vs->t.positionFetch.valid && w.slot == vs->t.positionFetch.slot && w.dwords != 0)
                positionWindow = &w;
        std::vector<uint32_t> loads;
        if (positionWindow != nullptr)
        {
            loads.resize(positionWindow->dwords);
            std::memcpy(loads.data(), windowBytes(*positionWindow), loads.size() * 4);  // the GPU's 32-bit loads (little-endian host)
        }
        if (d.extentNeedsVertices)
            BoundTargetHeight(d, vs->t, positionWindow ? loads.data() : nullptr, loads.size(), positionWindow ? positionWindow->endian : 0, indices,
                              target.targetHeight, extentNote);
        if (overwriteCandidate)
        {
            overwrite = FindOverwriteRect(d, vs->t, ps->t.kills, positionWindow ? loads.data() : nullptr, loads.size(), positionWindow ? positionWindow->endian : 0);
            if (overwrite.depth || overwrite.color)
                overwriteClaimsSinceSwap++;
            else if (DiagOverwrite())
            {
                static std::set<uint64_t> named;
                if (named.insert(vs->hash).second)
                    std::fprintf(stderr, "[diag overwrite] draw %llu: a rect-list draw of 3 vertices (vs %016llx) is no overwrite: %s\n", (unsigned long long)draw,
                                 (unsigned long long)vs->hash, overwrite.why.c_str());
            }
            // FM4 Recomp (M7 GPU cost, FM4_CLEAR_DRAWS): the overwriting draw's clear values; a refused candidate is named once per
            // signature (the shader pair, the depth/stencil state, the colour mask and blending), so the first layer-on run shows
            // which clear shapes the proof misses.
            if (ClearDraws() && (overwrite.depth || overwrite.color))
            {
                const ClearDraw found = FindClearDraw(d, overwrite, vs->t, ps->t, positionWindow ? loads.data() : nullptr, loads.size(),
                                                      positionWindow ? positionWindow->endian : 0, target.targetHeight);
                if (found.ok)
                {
                    clearRequest.rect = found.rect, clearRequest.color = found.color, clearRequest.depth = found.depth;
                    std::memcpy(clearRequest.rgba, found.rgba, sizeof clearRequest.rgba);
                    clearRequest.depthValue = found.depthValue, clearRequest.stencil = found.stencil;
                    deferClear = true;
                }
                else
                {
                    static std::set<std::tuple<uint64_t, uint64_t, uint32_t, uint32_t, bool>> named;
                    const DepthStencil& ds = d.depthStencil;
                    const uint32_t depthSig = (ds.zEnable ? 1u : 0) | (ds.zWriteEnable ? 2u : 0) | ds.zFunc << 2 | (ds.stencilEnable ? 32u : 0) | ds.front.func << 6 |
                                              ds.front.zPass << 9 | ds.front.writeMask << 12;
                    if (named.insert({vs->hash, ps->hash, depthSig, d.colorWriteMask, d.blend.enabled}).second)
                        std::fprintf(stderr, "[diag clear draws] draw %llu: an overwriting draw (vs %016llx ps %016llx, colour mask %X, blend %d, %s) is not deferred: %s\n",
                                     (unsigned long long)draw, (unsigned long long)vs->hash, (unsigned long long)ps->hash, d.colorWriteMask, int(d.blend.enabled),
                                     overwrite.depth ? (overwrite.color ? "depth and colour replaced" : "depth replaced") : "colour replaced", found.why.c_str());
                }
            }
        }
    }
    // FM4 Recomp (M7, guest memory for the GPU; FM4_GUEST_MEMORY_GPU, gpu_read_guard.h): all of the draw's windows read directly, or
    // none (one binding). For each window, in this order: the geometry (G7); the copy path's own checks and stops (windowBytes: one
    // view, committed); a pending write-back over any of its HOST pages (a write-back opens the whole host page, so a direct read
    // next to it would lose its guard); then, under the guard's gate, the read noted for the open part and "still protected since
    // its last watch" checked (G1, G8), else the range protected again (WatchGuestRange). A refusal leaves the noted reads in
    // place: a later store to those pages waits for this part for nothing, which is safe. The draw entry is before SplitSubmit
    // and the ring flush, so step 7 notes the reads again with the part that records the draw.
    GpuReadGuard* const readGuard = GuestMemoryGpuGuard();
    bool direct = false;
    if (readGuard != nullptr)
    {
        directSequences.assign(windows.size(), 0);
        bool any = false;
        direct = true;
        // FM4 Recomp (M7, FM4_GUARD_HOT; gpu_read_guard.h Hot): a draw with a window on a hot page takes the copy path, checked for
        // every window before any read is noted (a noted read would make the next store to the page wait for nothing).
        for (const VertexWindow& w : windows)
            if (w.dwords != 0 && readGuard->Hot(w.physical, w.dwords * 4))
            {
                direct = false;
                hotSnapshotsSinceSwap++;
                break;
            }
        for (size_t k = 0; k < windows.size() && direct; k++)
        {
            const VertexWindow& w = windows[k];
            if (w.dwords == 0)
                continue;
            any = true;
            const uint32_t bytes = w.dwords * 4;
            GuardFacts f{true, w.physical, bytes, runtime::kPhysicalSize, true, false};
            if (ChooseGuardPath(f) != GuardPath::Direct)  // outside physical memory or unaligned: nothing protected for it
            {
                direct = false;
                break;
            }
            (void)windowBytes(w);
            const uint64_t hostFirst = w.physical & ~uint64_t(runtime::kWatchHostPage - 1);
            const uint64_t hostEnd = (uint64_t(w.physical) + bytes + runtime::kWatchHostPage - 1) & ~uint64_t(runtime::kWatchHostPage - 1);
            f.pendingWrite = GuestWritesOverlap(uint32_t(hostFirst), uint32_t(hostEnd - hostFirst));
            const uint64_t key = uint64_t(w.physical) << 2 | (w.endian & 3);
            const auto lastIt = vertexWatches.find(key);
            LastWatch* const last = lastIt != vertexWatches.end() ? &lastIt->second : nullptr;
            const bool stillProtected = !f.pendingWrite && readGuard->NoteReadThen(w.physical, bytes, currentPartSerial, [&] {
                return last != nullptr && last->physical == w.physical && last->bytes >= bytes && GuestRangeUnwritten(last->physical, last->bytes, last->sequence);
            });
            uint64_t sequence = stillProtected ? last->sequence : 0;
            f.guarded = stillProtected || (!f.pendingWrite && WatchWorthIt(last, w.physical, bytes) && WatchGuestRange(w.physical, bytes, sequence));
            if (f.guarded && !stillProtected)
                vertexWatches[key] = {w.physical, bytes, sequence, 0};
            directSequences[k] = sequence;
            direct = ChooseGuardPath(f) == GuardPath::Direct;
        }
        direct = direct && any;
        if (!direct)
            snapshotDrawsSinceSwap++;
    }
    // The slot's ring must hold this whole draw (every texture reloads at every bind: texture_upload.h kTextureValidity).
    // Draws are separate render passes, so a full ring is flushed between draws (submit, confirmed retire, reset) rather
    // than refusing the draw (one observed draw had 14,971,540 of 16,777,216 bytes used, 1,869,120 more needed).
    {
        const uint64_t slack = 256;  // per block: the device's uniform/storage alignment is at most 256 (Vulkan limits)
        // FM4 Recomp (M7, vertex reuse across submissions): with the vertex cache the windows take no ring bytes. A direct draw keeps
        // its ring bytes for the late snapshot at step 7 (FM4_GUEST_MEMORY_GPU).
        const uint64_t vertexRing = vertexCache && !direct ? 0 : std::max<uint64_t>(uint64_t(vertexDwords) * 4, 4);
        uint64_t need = kSystemBytes + 2 * uint64_t(kConstantsBytes) + vertexRing + std::max<uint64_t>(indices.indices.size() * 4, 4) + kErrorsBytes + 6 * slack;
        const uint64_t fixed = need - vertexRing - std::max<uint64_t>(indices.indices.size() * 4, 4);
        ringNeedSinceSwap[1] += std::max<uint64_t>(uint64_t(vertexDwords) * 4, 4), ringNeedSinceSwap[2] += std::max<uint64_t>(indices.indices.size() * 4, 4);
        ringNeedSinceSwap[3] += fixed;
        for (uint32_t s = 0; s < kTextureBindings; s++)
            if (plan.dimension[s] != 0 && textures != nullptr)
                for (const TextureSlot& t : d.textureSlots)
                    if (t.slot == s)
                    {
                        const uint64_t bytes = textures->stagingBytes(t, base);
                        need += bytes, ringNeedSinceSwap[0] += bytes;
                    }
        if (need > slot.allocator->Capacity())
            Stop(StopCode::Unimplemented, "renderer: draw %llu: needs %llu ring bytes, more than the slot's %llu", (unsigned long long)draw,
                 (unsigned long long)need, (unsigned long long)slot.allocator->Capacity());
        nextPartNeed = need;  // read by OpenSlot (UploadRing::NewPart): a new lap when the rest of the ring cannot hold this draw
        if (slot.open && (slot.allocator->Available() < need || slot.sets >= kDrawsPerSlot))
        {
            FlushSlot(0, FlushCause::Ring);
            ringFlushesSinceSwap++;
        }
    }
    // FM4 Recomp (M7, vertex reuse across submissions; FM4_VERTEX_CACHE_MB): the windows into the vertex cache, here at the draw's
    // entry (after the resolve-consumer flush above, so a resolve into vertex memory is in guest memory and has marked the range
    // written; before SplitSubmit and OpenSlot, so the flush a full cache needs records nothing into a live command buffer). A
    // window the cache holds unwritten since its copy (GuestRangeUnwritten, as FM4_VERTEX_REUSE) is read there; any other is copied
    // into fresh space (vertex_cache.h L2) and remembered when the write watch protects it. Step 7 marks the chunks read by the
    // part that records the draw (L3).
    static std::vector<uint32_t> windowChunks;  // each window's cache chunk (UINT32_MAX: an empty window)
    if (vertexCache && !direct)
    {
        uint8_t* const cacheBytes = static_cast<uint8_t*>(vertexCacheBuffer.mapped);
        uint64_t copied = 0, reused = 0;
        const auto place = [&]() -> bool {  // false: no chunk can be recycled (L6)
            windowChunks.assign(windows.size(), UINT32_MAX);
            copied = reused = 0;
            for (size_t k = 0; k < windows.size(); k++)
            {
                VertexWindow& w = windows[k];
                if (w.dwords == 0)
                {
                    w.baseDword = 0;
                    continue;
                }
                const uint64_t key = uint64_t(w.physical) << 2 | (w.endian & 3), bytes = uint64_t(w.dwords) * 4;
                if (const VertexCache::Entry* e = vertexCache->Find(key);
                    e != nullptr && e->dwords >= w.dwords && GuestRangeUnwritten(w.physical, e->dwords * 4, e->sequence))
                {
                    vertexCache->MarkRead(e->chunk, currentPartSerial);
                    w.baseDword = uint32_t(e->offset / 4), windowChunks[k] = e->chunk;
                    reused += bytes;
                    if (VertexReuseVerify() && std::memcmp(cacheBytes + e->offset, windowBytes(w), size_t(bytes)) != 0)
                        Stop(StopCode::Unimplemented, "renderer: draw %llu: vertex cache verify: slot %u's window at %08X (%u dwords) differs from guest memory",
                             (unsigned long long)draw, w.slot, w.physical, w.dwords);
                    continue;
                }
                uint64_t at = 0;
                uint32_t chunk = 0;
                const VertexCache::Place p = vertexCache->Allocate(bytes, currentPartSerial, at, chunk);
                if (p == VertexCache::Place::NeedsRetire)
                    return false;
                if (p != VertexCache::Place::Ok)
                    Stop(StopCode::Unimplemented, "renderer: draw %llu: vertex fetch slot %u's window (%u dwords) does not fit a %llu-byte vertex cache chunk",
                         (unsigned long long)draw, w.slot, w.dwords, (unsigned long long)vertexCache->ChunkBytes());
                // Protect, then take the sequence, then copy (guest_writes.h WatchGuestRange): a store after the protection moves the
                // range's sequence past the one remembered, so the copy is not reused.
                uint64_t sequence = 0;
                const auto lastIt = vertexWatches.find(key);
                const bool worth = WatchWorthIt(lastIt != vertexWatches.end() ? &lastIt->second : nullptr, w.physical, w.dwords * 4);
                const bool watched = worth && WatchGuestRange(w.physical, w.dwords * 4, sequence);
                if (watched)
                    vertexWatches[key] = {w.physical, w.dwords * 4, sequence, 0};
                else if (!worth)
                    hotSkipsSinceSwap++;
                std::memcpy(cacheBytes + at, windowBytes(w), size_t(bytes));
                w.baseDword = uint32_t(at / 4), windowChunks[k] = chunk;
                copied += bytes;
                if (watched)
                    vertexCache->Remember(key, chunk, at, w.dwords, sequence);
                else
                    vertexCache->Forget(key);  // copied for this draw only; its space returns when its chunk is recycled
            }
            return true;
        };
        if (!place())
        {
            // Every chunk is read by a part not yet retired: flush (submit, confirmed retire; RetireSlot moves the watermark past
            // every part so far), then place the draw again from its first window, so its earlier windows are marked read by the
            // new part before any chunk is recycled.
            FlushSlot(0, FlushCause::VertexCache);
            vertexCacheFlushesSinceSwap++;
            if (!place())
                Stop(StopCode::Unimplemented, "renderer: draw %llu: the vertex cache (%u chunks of %llu bytes) cannot hold the draw's %zu windows after a flush",
                     (unsigned long long)draw, vertexCache->Chunks(), (unsigned long long)vertexCache->ChunkBytes(), windows.size());
        }
        vertexCopiedSinceSwap += copied, vertexReusedSinceSwap += reused;
    }
    SplitSubmit();  // FM4_SUBMIT_SPLIT: between draws, outside any render pass of this draw
    if (splitAfterResolvePending)  // FM4_SPLIT_AFTER_RESOLVE: here no command buffer handle of an earlier draw is live
    {
        splitAfterResolvePending = false;
        if (ForceSplit())
            splitsAfterResolveSinceSwap++;
    }
    const VkCommandBuffer c = OpenSlot(0);
    HostTargets targets;
    bool acquired;
    {
        const PassActivity activity("target acquire");  // FM4_DIAG_PASS_TIMES label
        RequireRecording(c, "target acquire");
        if (DiagTransferConsumers())  // FM4_DIAG_TRANSFERS=2 (diagnostic): the draw's signature for the EDRAM unit's first-use tally
        {
            const DepthStencil& ds = d.depthStencil;
            std::snprintf(diagDrawSignature, sizeof diagDrawSignature,
                          "vs %016llx ps %016llx prim %u, %ux%u x%u, z%d w%d func %u, stencil %d (zpass %u wmask %02X), colour mask %X blend %d",
                          (unsigned long long)vs->hash, (unsigned long long)ps->hash, d.primitiveType, d.scissor.width, d.scissor.height, d.target.samples,
                          int(ds.zEnable), int(ds.zWriteEnable), ds.zFunc, int(ds.stencilEnable), ds.front.zPass, ds.front.writeMask, d.colorWriteMask,
                          int(d.blend.enabled));
        }
        // FM4_CLEAR_DRAWS: a deferred clear draw binds with its (row-cut) rectangle as both cutouts and records no draw.
        acquired = edramHooks->acquireTargets(c, target, key.colorUsed != 0, key.depthUsed != 0, deferClear ? &clearRequest.rect : overwrite.depth ? &overwrite.rect : nullptr,
                                              deferClear ? &clearRequest.rect : overwrite.color ? &overwrite.rect : nullptr, deferClear ? &clearRequest : nullptr,
                                              targets, error);
    }
    if (acquired && lastBindTransferTiles != 0)  // FM4_DIAG_OVERWRITE (diagnostic): the draw that follows an ownership transfer
    {
        const bool diag = DiagOverwrite();
        static std::set<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, bool>> seen;
        const DepthStencil& ds = d.depthStencil;
        const uint32_t depthSig = (ds.zEnable ? 1u : 0) | (ds.zWriteEnable ? 2u : 0) | ds.zFunc << 2 | (ds.stencilEnable ? 32u : 0) | ds.front.func << 6 |
                                  ds.front.zPass << 9 | ds.front.writeMask << 12;
        if (diag && seen.insert({vs->hash, d.primitiveType, depthSig, d.colorWriteMask, d.blend.enabled}).second)
        {
            char verts[256] = "";
            if (!d.vertexSlots.empty() && d.vertexSlots[0].valid && d.vertexSlots[0].stride != 0)
            {
                const VertexSlot& s0 = d.vertexSlots[0];
                size_t n = 0;
                for (uint32_t v = 0; v < 3 && n + 48 < sizeof verts; v++)
                {
                    n += size_t(std::snprintf(verts + n, sizeof verts - n, " v%u(", v));
                    for (uint32_t k = 0; k < 4 && k < s0.stride && n + 16 < sizeof verts; k++)
                    {
                        // The dword's guest view (PhysicalToGuest, as the vertex copy below), within the slot's window: never a raw read.
                        const uint32_t dword = v * s0.stride + k;
                        uint32_t guest = 0;
                        if (dword >= s0.sizeWords || !runtime::PhysicalToGuest(s0.physical + dword * 4, guest))
                            break;
                        uint32_t w;
                        std::memcpy(&w, base + guest, 4);
                        if (s0.endian == 2) w = __builtin_bswap32(w);
                        else if (s0.endian == 1) w = (w & 0xFF00FF00u) >> 8 | (w & 0x00FF00FFu) << 8;
                        float f;
                        std::memcpy(&f, &w, 4);
                        n += size_t(std::snprintf(verts + n, sizeof verts - n, "%s%g", k ? "," : "", f));
                    }
                    n += size_t(std::snprintf(verts + n, sizeof verts - n, ")"));
                }
            }
            std::fprintf(stderr, "[diag overwrite] draw %llu after a transfer of %u tiles: vs %016llx ps %016llx prim %u count %u indexed %d; depth z%d w%d func %u, "
                                 "stencil %d (func %u zpass %u wmask %02X), colour mask %X blend %d; scissor %u,%u %ux%u rows %u/%u; vte xy/w %d scale %g,%g offset %g,%g; "
                                 "slot0 %08X stride %u endian %u:%s\n",
                         (unsigned long long)draw, lastBindTransferTiles, (unsigned long long)vs->hash, (unsigned long long)ps->hash, d.primitiveType, d.indices.count,
                         int(d.indices.indexed), int(ds.zEnable), int(ds.zWriteEnable), ds.zFunc, int(ds.stencilEnable), ds.front.func, ds.front.zPass,
                         ds.front.writeMask, d.colorWriteMask, int(d.blend.enabled), d.scissor.x, d.scissor.y, d.scissor.width, d.scissor.height, d.target.targetHeight,
                         d.target.edramRows, int(d.viewport.xyDividedByW), d.viewport.scale[0], d.viewport.scale[1], d.viewport.offset[0], d.viewport.offset[1],
                         d.vertexSlots.empty() ? 0u : d.vertexSlots[0].physical, d.vertexSlots.empty() ? 0u : d.vertexSlots[0].stride,
                         d.vertexSlots.empty() ? 0u : d.vertexSlots[0].endian, verts);
        }
    }
    if (!acquired)
        Stop(StopCode::Unimplemented, "renderer: draw %llu: render target: %s%s%s", (unsigned long long)draw, error.c_str(), extentNote.empty() ? "" : " (",
             extentNote.empty() ? "" : (extentNote + ")").c_str());
    if (deferClear)  // FM4_CLEAR_DRAWS: the clear is pending on its targets (the EDRAM unit); nothing of this draw is recorded or uploaded
    {
        clearDrawsDeferredSinceSwap++;
        return;
    }
    drawsInBuffer++;
    if (targets.samples != key.samples)
        Stop(StopCode::Unimplemented, "renderer: draw %llu: the EDRAM unit returned %u-sample images for a %u-sample target", (unsigned long long)draw,
             targets.samples, key.samples);
    if (key.colorUsed && (targets.color == VK_NULL_HANDLE || targets.format != HostColorFormat(key.color[0].format)))
        Stop(StopCode::Unimplemented, "renderer: draw %llu: the EDRAM unit's colour view is missing or of format %d (expected %d)", (unsigned long long)draw,
             int(targets.format), int(HostColorFormat(key.color[0].format)));
    if (key.depthUsed && (targets.depth == VK_NULL_HANDLE || targets.depthFormat != HostDepthFormat(key.depthFormat)))
        Stop(StopCode::Unimplemented, "renderer: draw %llu: the EDRAM unit's depth view is missing or of format %d (expected %d)", (unsigned long long)draw,
             int(targets.depthFormat), int(HostDepthFormat(key.depthFormat)));
    // The rows the draw can touch: the scissor's bottom within the surface's EDRAM rows (draw_state.h targetHeight), the
    // rows edram::Bind claimed (X render_target_cache.cc:757-767, :819-857). FM4's D3D fast clears have an 8192-row
    // scissor over a 360-row 4x surface whose host image is 1024 rows: the host scissor stops at
    // the claimed rows, never past the image.
    const uint32_t scissorBottom = std::max(target.targetHeight, d.scissor.y);
    // FM4 Recomp (M9, FM4_INTERNAL_RES): in guest pixels (the host images are targets.halves / 2 times the guest size).
    if (targets.guestWidth < d.scissor.x + d.scissor.width || targets.guestHeight < scissorBottom || targets.width == 0 || targets.height == 0)
        Stop(StopCode::Unimplemented, "renderer: draw %llu: the %ux%u host target does not cover the scissor (%u, %u, %ux%u; rows to %u)",
             (unsigned long long)draw, targets.width, targets.height, d.scissor.x, d.scissor.y, d.scissor.width, d.scissor.height, scissorBottom);
    const VkFramebuffer framebuffer = Framebuffer(pipeline.renderPass, targets, key.colorUsed != 0, key.depthUsed != 0);
    // 7. The uploads into the slot's ring (pipeline_key.h D9); the windows were planned before the bind.
    const uint64_t systemAt = Allocate(slot, draw, kSystemBytes, true, "XeSystem");
    // FM4 Recomp (M7 CPU cost): consecutive draws mostly share their 2,088 constant words; when they do and the previous draw's
    // two blocks are still in the ring (UploadRing::Intact), they are bound again instead of allocated and filled (2 x 4 KB a
    // draw). Compared by content (memcmp), not by a register-write count: the guest CPU writes registers by memory store too.
    // FM4 Recomp (M7 CPU cost, opt-in FM4_CONSTANTS_PER_STAGE=1): each stage's block is bound again on its own when FillConstants would
    // write the bytes its shader reads exactly as the previous draw's (draw_record.h SameStageConstants); the other stage is filled.
    // Invariant (each stage): the block at lastConstants.<stage>At holds FillConstants(previous draw, stage) in the range the shader
    // reads: a filled block is this draw's; a reused one equals this draw's by the comparison; lastConstants takes this draw's words
    // whenever a stage was filled (the alu swap below), and is already equal when both were reused.
    bool reuseVs = false, reusePs = false;
    if (!ConstantsPerStage())
        reuseVs = reusePs = haveLastConstants && d.aluConstants.size() == 2048 && lastConstants.alu.size() == 2048 &&
                            d.vsConstantWords == lastConstants.vsWords && d.psConstantWords == lastConstants.psWords &&
                            std::memcmp(d.aluConstants.data(), lastConstants.alu.data(), size_t(d.vsConstantWords) * 4) == 0 &&
                            std::memcmp(d.aluConstants.data() + 1024, lastConstants.alu.data() + 1024, size_t(d.psConstantWords) * 4) == 0 &&
                            std::memcmp(d.boolConstants, lastConstants.bools, sizeof lastConstants.bools) == 0 &&
                            std::memcmp(d.loopConstants, lastConstants.loops, sizeof lastConstants.loops) == 0 &&
                            slot.allocator->Intact(lastConstants.vsAt, kConstantsBytes, lastConstants.lap) &&
                            slot.allocator->Intact(lastConstants.psAt, kConstantsBytes, lastConstants.lap);
    else if (haveLastConstants)
    {
        reuseVs = SameStageConstants(d, false, lastConstants.vsWords, lastConstants.alu, lastConstants.bools, lastConstants.loops) &&
                  slot.allocator->Intact(lastConstants.vsAt, kConstantsBytes, lastConstants.vsLap);
        reusePs = SameStageConstants(d, true, lastConstants.psWords, lastConstants.alu, lastConstants.bools, lastConstants.loops) &&
                  slot.allocator->Intact(lastConstants.psAt, kConstantsBytes, lastConstants.psLap);
    }
    const bool sameConstants = reuseVs && reusePs;
    const uint64_t vsConstantsAt = reuseVs ? lastConstants.vsAt : Allocate(slot, draw, kConstantsBytes, true, "XeVsConstants");
    const uint64_t vsLap = slot.allocator->Lap();
    const uint64_t psConstantsAt = reusePs ? lastConstants.psAt : Allocate(slot, draw, kConstantsBytes, true, "XePsConstants");
    if (sameConstants)
        constantsReusedSinceSwap++;
    else
    {
        stageConstantsReusedSinceSwap[0] += reuseVs, stageConstantsReusedSinceSwap[1] += reusePs;  // FM4_CONSTANTS_PER_STAGE (0 when off)
        std::memcpy(lastConstants.bools, d.boolConstants, sizeof lastConstants.bools);
        std::memcpy(lastConstants.loops, d.loopConstants, sizeof lastConstants.loops);
        lastConstants.vsAt = vsConstantsAt, lastConstants.psAt = psConstantsAt, lastConstants.lap = slot.allocator->Lap();
        if (!reuseVs)
            lastConstants.vsLap = vsLap;
        if (!reusePs)
            lastConstants.psLap = slot.allocator->Lap();
        lastConstants.vsWords = d.vsConstantWords, lastConstants.psWords = d.psConstantWords;
        haveLastConstants = true;
    }
    uint64_t vertexBytes = std::max<uint64_t>(uint64_t(vertexDwords) * 4, 4);
    uint64_t vertexAt = 0;
    // FM4 Recomp (M7, vertex reuse across submissions): binding 0 is the whole vertex cache when it is on (one buffer for the run).
    VkBuffer vertexBuffer = vertexCache ? vertexCacheBuffer.buffer : slot.ring.buffer;
    const bool directAtEntry = direct;
    if (direct)
    {
        // FM4 Recomp (M7, FM4_GUEST_MEMORY_GPU; gpu_read_guard.h G1, G8): the reads noted again with the part that records the draw
        // (a split or the ring flush since the entry moved it; a store that waited for the entry's part may have landed since), and
        // each range still protected since the entry's check, or protected again. A range that cannot be protected now (the heap
        // released or revoked it meanwhile) turns the draw into a snapshot here: packed into the ring (whose need counted the
        // windows), the binding the whole ring from 0 with each window's absolute dword.
        for (size_t k = 0; k < windows.size() && direct; k++)
        {
            const VertexWindow& w = windows[k];
            if (w.dwords == 0)
                continue;
            const uint32_t bytes = w.dwords * 4;
            const bool still = readGuard->NoteReadThen(w.physical, bytes, currentPartSerial,
                                                       [&] { return GuestRangeUnwritten(w.physical, bytes, directSequences[k]); });
            uint64_t sequence = 0;
            if (!still && WatchGuestRange(w.physical, bytes, sequence))
                vertexWatches[uint64_t(w.physical) << 2 | (w.endian & 3)] = {w.physical, bytes, sequence, 0};
            else if (!still)
                direct = false;
        }
        if (direct)
        {
            for (VertexWindow& w : windows)
            {
                w.baseDword = w.dwords != 0 ? DirectBaseDword(w.physical) : 0;
                directWindowsSinceSwap += w.dwords != 0 ? 1 : 0, directBytesSinceSwap += uint64_t(w.dwords) * 4;
            }
            vertexBuffer = GuestMemoryGpuView()->buffer, vertexBytes = GuestMemoryGpuView()->bytes;
        }
        else
        {
            // The whole ring from 0 when the device's range covers it (FM4_DESCRIPTOR_CACHE's dynamic vertex offset is 0); else the
            // block alone, as the plain path (descriptor caching then needs FM4_VERTEX_REUSE, which needs the whole-ring range: off).
            const uint64_t at = Allocate(slot, draw, vertexBytes, false, "vertex data (late snapshot)");
            uint32_t packed = 0;
            for (VertexWindow& w : windows)
            {
                w.baseDword = (wholeRingVertexBinding ? uint32_t(at / 4) : 0) + packed;
                if (w.dwords != 0)
                    std::memcpy(static_cast<uint8_t*>(slot.ring.mapped) + at + uint64_t(packed) * 4, windowBytes(w), size_t(w.dwords) * 4);
                packed += w.dwords;
            }
            vertexBuffer = slot.ring.buffer;
            vertexCopiedSinceSwap += uint64_t(vertexDwords) * 4;  // a copy into the ring, as the snapshot paths count
            if (wholeRingVertexBinding)
                vertexBytes = slot.allocator->Capacity();
            else
                vertexAt = at;
            snapshotDrawsSinceSwap++, lateSnapshotsSinceSwap++;
        }
    }
    else if (vertexCache)
    {
        // FM4 Recomp (M7, vertex reuse across submissions; vertex_cache.h L3): placed at the draw's entry; a split since then moved
        // the draw to a later part, so its chunks are marked read by the part that records it. No chunk was recycled in between
        // (only the entry's placement allocates). The binding is the whole cache.
        for (const uint32_t chunk : windowChunks)
            if (chunk != UINT32_MAX)
                vertexCache->MarkRead(chunk, currentPartSerial);
        vertexBytes = vertexCache->Capacity();
    }
    else if (VertexReuse())
    {
        // Each window gets its own place in the ring (copied, or a copy the slot holds); the binding is the whole ring.
        for (VertexWindow& w : windows)
        {
            if (w.dwords == 0)
            {
                w.baseDword = 0;
                continue;
            }
            const uint64_t key = uint64_t(w.physical) << 2 | (w.endian & 3);
            const auto it = slot.vertexCopies.find(key);
            if (it != slot.vertexCopies.end() && it->second.dwords >= w.dwords &&
                slot.allocator->Intact(uint64_t(it->second.ringDword) * 4, uint64_t(it->second.dwords) * 4, it->second.lap) &&
                GuestRangeUnwritten(w.physical, it->second.dwords * 4, it->second.sequence))
            {
                w.baseDword = it->second.ringDword;
                vertexReusedSinceSwap += uint64_t(w.dwords) * 4;
                if (VertexReuseVerify() && std::memcmp(static_cast<uint8_t*>(slot.ring.mapped) + uint64_t(w.baseDword) * 4, windowBytes(w), size_t(w.dwords) * 4) != 0)
                    Stop(StopCode::Unimplemented, "renderer: draw %llu: vertex reuse verify: slot %u's window at %08X (%u dwords) differs from guest memory",
                         (unsigned long long)draw, w.slot, w.physical, w.dwords);
                continue;
            }
            uint64_t sequence = 0;
            const auto lastIt = vertexWatches.find(key);
            const bool worth = WatchWorthIt(lastIt != vertexWatches.end() ? &lastIt->second : nullptr, w.physical, w.dwords * 4);
            const bool watched = worth && WatchGuestRange(w.physical, w.dwords * 4, sequence);
            if (watched)
                vertexWatches[key] = {w.physical, w.dwords * 4, sequence, 0};
            else if (!worth)
                hotSkipsSinceSwap++;
            const uint64_t at = Allocate(slot, draw, uint64_t(w.dwords) * 4, false, "vertex window");
            std::memcpy(static_cast<uint8_t*>(slot.ring.mapped) + at, windowBytes(w), size_t(w.dwords) * 4);
            w.baseDword = uint32_t(at / 4);
            vertexCopiedSinceSwap += uint64_t(w.dwords) * 4;
            if (watched)
                slot.vertexCopies[key] = {w.baseDword, w.dwords, sequence, slot.allocator->Lap()};
            else
                slot.vertexCopies.erase(key);
        }
        vertexBytes = slot.allocator->Capacity();
    }
    else
        vertexAt = Allocate(slot, draw, vertexBytes, false, "vertex data");
    const uint64_t indexBytes = std::max<uint64_t>(indices.indices.size() * 4, 4);
    const uint64_t indexAt = Allocate(slot, draw, indexBytes, false, "indices");
    const uint64_t errorAt = Allocate(slot, draw, kErrorsBytes, false, "XeErrors");
    uint8_t* ring = static_cast<uint8_t*>(slot.ring.mapped);
    // FM4 Recomp (M9): clip space from guest pixels, so the window offset and the +0.5 pixel centre scale with the geometry (ReCarbon
    // renderer.cpp:8214-8233, :8966 at 66401ea); the VkViewport below covers the host image.
    HostViewport hv = FoldViewport(d.viewport, targets.guestWidth, targets.viewRows);  // FM4 Recomp (M7 CPU cost): once; the VkViewport below reads it too
    // FM4 Recomp (M9, Extended View; kernel/extended_view.h): the HUD's and menus' draws stay a centred 16:9 block once the picture
    // is stretched. A draw over the whole frame gives the frame's place in the target (its centre in clip space, its first column
    // and row) and takes the pinned variant chosen above with the two stretches; a draw in a viewport of its own (the track map)
    // is moved and narrowed about the frame's centre; scissors follow the same rule.
    uint32_t scissorX = d.scissor.x, scissorWidth = d.scissor.width, scissorY = d.scissor.y, scissorHeight = scissorBottom - d.scissor.y;
    if (extendedFrameFactor != 1.0f && d.target.colorFormat == runtime::view::kFinalColourFormat && d.target.pitch == runtime::view::kFrameWidth)
    {
        static float centreX = 0.0f, centreY = 0.0f;  // the frame in this target, from its last whole-frame draw (the scene's copy at least)
        static uint32_t frameX = 0, frameY = 0;
        if (overlayFullFrame)
        {
            centreX = hv.offset[0], centreY = hv.offset[1];
            frameX = uint32_t(std::max(0.0f, std::round((hv.offset[0] + 1.0f) * 0.5f * float(targets.guestWidth) - 640.0f)));
            frameY = uint32_t(std::max(0.0f, std::round((hv.offset[1] + 1.0f) * 0.5f * float(targets.viewRows) - 360.0f)));
        }
        if (overlay)
        {
            const float wide = runtime::view::Wide(extendedFrameFactor), tall = runtime::view::Tall(extendedFrameFactor);
            if (overlayPin)
                hv.offset[3] = wide, hv.scale[3] = tall;  // xe_viewport_offset.w / _scale.w: the XE_OVERLAY_PIN variant's stretches
            else
                runtime::view::NarrowAbout(hv.scale[0], hv.offset[0], centreX, wide), runtime::view::NarrowAbout(hv.scale[1], hv.offset[1], centreY, tall);
            runtime::view::NarrowScissor(scissorX, scissorWidth, frameX, runtime::view::kFrameWidth, wide);
            runtime::view::NarrowScissor(scissorY, scissorHeight, frameY, 720, tall);
            extendedNarrowedSinceSwap++;
        }
    }
    if (runtime::video::diag::DrawListSwap(currentSwap + 1))  // FM4_DIAG_DRAW_LIST (diagnostic): the draws issued since the last swap
        std::fprintf(stderr, "[diag draws] swap %llu draw %llu: vs %016llx ps %016llx prim %u count %u; target format %u (raw %u) msaa %u, %ux%u; "
                             "viewport xy/w %d scale %g,%g offset %g,%g -> clip scale %g,%g offset %g,%g; scissor %u,%u %ux%u; colour mask %X blend %d\n",
                     (unsigned long long)(currentSwap + 1), (unsigned long long)draw, (unsigned long long)vs->hash, (unsigned long long)ps->hash, d.primitiveType,
                     d.indices.count, d.target.colorFormat, d.target.colorFormatRaw, d.target.msaa, targets.guestWidth, targets.viewRows,
                     int(d.viewport.xyDividedByW), d.viewport.scale[0], d.viewport.scale[1], d.viewport.offset[0], d.viewport.offset[1], hv.scale[0], hv.scale[1],
                     hv.offset[0], hv.offset[1], d.scissor.x, d.scissor.y, d.scissor.width, d.scissor.height, d.colorWriteMask, int(d.blend.enabled));
    if (runtime::video::diag::DrawListSwap(currentSwap + 1) && d.target.colorFormat == runtime::view::kFinalColourFormat && !d.vertexSlots.empty() &&
        d.vertexSlots[0].valid && d.vertexSlots[0].stride != 0)  // the same diagnostic: a final-target draw's first vertices (slot 0's first two floats)
    {
        const VertexSlot& s0 = d.vertexSlots[0];
        char verts[320] = "";
        size_t n = 0;
        for (uint32_t v = 0; v < 6 && v < d.indices.count && n + 40 < sizeof verts; v++)
        {
            float xy[2] = {0, 0};
            for (uint32_t k = 0; k < 2 && k < s0.stride; k++)
            {
                uint32_t guest = 0, w = 0;
                if (v * s0.stride + k >= s0.sizeWords || !runtime::PhysicalToGuest(s0.physical + (v * s0.stride + k) * 4, guest))
                    break;
                std::memcpy(&w, base + guest, 4);
                if (s0.endian == 2) w = __builtin_bswap32(w);
                else if (s0.endian == 1) w = (w & 0xFF00FF00u) >> 8 | (w & 0x00FF00FFu) << 8;
                std::memcpy(&xy[k], &w, 4);
            }
            n += size_t(std::snprintf(verts + n, sizeof verts - n, " (%g,%g)", xy[0], xy[1]));
        }
        std::fprintf(stderr, "[diag draws] swap %llu draw %llu vertices: indexed %d, %u slot(s), slot 0 stride %u words endian %u, %u words:%s\n",
                     (unsigned long long)(currentSwap + 1), (unsigned long long)draw, int(d.indices.indexed), unsigned(d.vertexSlots.size()), s0.stride, s0.endian,
                     s0.sizeWords, verts);
    }
    FillXeSystem(windows, hv, d.systemFlags, ring + systemAt);
    if (!sameConstants)
    {
        if (!reuseVs)  // FM4_CONSTANTS_PER_STAGE: one stage may be bound again (off: reuseVs == reusePs == sameConstants)
            FillConstants(d, false, ring + vsConstantsAt);
        if (!reusePs)
            FillConstants(d, true, ring + psConstantsAt);
        lastConstants.alu.swap(d.aluConstants);  // this draw's words become the previous draw's; nothing below reads d.aluConstants
    }
    for (const VertexWindow& w : windows)
    {
        if (w.dwords == 0 || VertexReuse() || directAtEntry)  // FM4_GUEST_MEMORY_GPU: read directly, or copied by the late snapshot
            continue;
        // A plain CPU read of guest memory, as the present path (the guarded read is unit 7).
        std::memcpy(ring + vertexAt + uint64_t(w.baseDword) * 4, windowBytes(w), size_t(w.dwords) * 4);
    }
    if (!indices.indices.empty())
        std::memcpy(ring + indexAt, indices.indices.data(), indices.indices.size() * 4);
    std::memset(ring + errorAt, 0, kErrorsBytes);
    {
        // FM4 Recomp (M7 CPU cost): the windows go to the slot's flat errorWindows (no vector per draw).
        const Slot::ErrorDword e{errorAt, draw, vs->hash, indices.highIndex, uint32_t(slot.errorWindows.size()), uint32_t(windows.size())};
        for (size_t k = 0; k < windows.size(); k++)
            slot.errorWindows.push_back({windows[k].slot, windows[k].dwords, d.vertexSlots[k].sizeWords, d.vertexSlots[k].stride, d.vertexSlots[k].end,
                                         d.vertexSlots[k].computedIndex});
        slot.errorDwords.push_back(e);
    }
    // 8. The descriptor set: the six buffer bindings and the textures the pair samples. The textures are bound first (their views
    //    and samplers decide FM4_DESCRIPTOR_CACHE's key); then a set is allocated and written, or one the slot holds is reused.
    VkDescriptorSet set = VK_NULL_HANDLE;
    const VkDescriptorSetLayout layout = pipeline.layouts.set;
    // FM4 Recomp (M7 CPU cost): fixed arrays, one entry a texture slot at most (two vectors reserved to 32 cost two allocations a draw).
    VkDescriptorImageInfo images[kTextureBindings];
    uint32_t imageBindings[kTextureBindings];
    uint32_t imageCount = 0;
    // FM4 Recomp (M7 CPU cost): the texture binds are timed once a draw, not once a texture (two clock reads a texture; mach_continuous_time
    // was 2.7 % of the command thread); the swap line's bind time now also covers the slot lookups between the binds (sub-microsecond).
    const bool anyTexture = (plan.vsSlots | plan.psSlots) != 0;
    const auto bindStart = anyTexture ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
    for (uint32_t s = 0; s < kTextureBindings; s++)
    {
        if (plan.dimension[s] == 0)
            continue;
        if (textures == nullptr)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: the shaders sample texture slot %u but no texture unit is registered", (unsigned long long)draw, s);
        const TextureSlot* t = nullptr;
        for (const TextureSlot& candidate : d.textureSlots)
            if (candidate.slot == s)
                t = &candidate;
        if (t != nullptr)
            DiagTextureUser(draw, vs->hash, ps->hash, s, t->words);  // FM4_DIAG_TEX_USERS (diagnostic, diag_blend.h): nothing unless set
        if (t == nullptr)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: texture slot %u is sampled but was not decoded", (unsigned long long)draw, s);
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        const TextureBindContext context{c, base, slot.ring.buffer, static_cast<uint8_t*>(slot.ring.mapped), &*slot.allocator};
        RequireRecording(c, "texture bind");
        bool bound;
        {
            const PassActivity activity("texture");  // FM4_DIAG_PASS_TIMES label
            bound = textures->bind(context, *t, view, sampler, error);
        }
        if (!bound || view == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: texture slot %u (%08X %08X %08X %08X %08X %08X): %s", (unsigned long long)draw, s,
                 t->words[0], t->words[1], t->words[2], t->words[3], t->words[4], t->words[5], error.empty() ? "no view or sampler" : error.c_str());
        images[imageCount] = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        imageBindings[imageCount++] = kBindingTexture0 + s;
    }
    if (anyTexture)
        textureBindNsSinceSwap += NsSince(bindStart);
    // FM4_DESCRIPTOR_CACHE: XeIndices is read only by the XE_RECT_LIST variant; every other draw binds 4 bytes at its offset.
    const uint64_t indexRange = indices.kind == HostIndices::Kind::RectList ? indexBytes : 4;
    // FM4 Recomp (M7 CPU cost): the key's storage stays from draw to draw (its 16 + 20 x textures bytes cost one allocation a draw); a
    // miss copies it into the cache (one allocation, as the moved key's node was).
    static std::string descriptorKey;
    descriptorKey.clear();
    if (descriptorCacheOn)
    {
        const auto put = [&](uint64_t v) { descriptorKey.append(reinterpret_cast<const char*>(&v), sizeof v); };
        put(uint64_t(reinterpret_cast<uintptr_t>(layout))), put(indexRange);
        for (uint32_t k = 0; k < imageCount; k++)
            put(imageBindings[k]), put(uint64_t(reinterpret_cast<uintptr_t>(images[k].imageView))), put(uint64_t(reinterpret_cast<uintptr_t>(images[k].sampler)));
        if (readGuard != nullptr)  // FM4_GUEST_MEMORY_GPU: binding 0 changes from draw to draw (guest memory, the cache or the ring)
            put(uint64_t(reinterpret_cast<uintptr_t>(vertexBuffer))), put(vertexBytes);
        if (const auto it = slot.descriptorCache.find(descriptorKey); it != slot.descriptorCache.end())
            set = it->second, descriptorHitsSinceSwap++;
    }
    // FM4 Recomp (M7 CPU cost, FM4_PUSH_DESCRIPTORS): a push-descriptor layout gets the same six buffers and textures as writes
    // recorded with vkCmdPushDescriptorSetKHR at the bind below: no set allocated, written or bound, no pool to reset.
    const bool push = pipeline.layouts.push;
    VkDescriptorBufferInfo pushBuffers[6];
    VkWriteDescriptorSet pushWrites[6 + kTextureBindings];
    uint32_t pushCount = 0;
    // FM4 Recomp (M7 CPU cost, FM4_PUSH_TEMPLATE, push_template.h): the same six buffers and images as one data block for the layout's
    // update template: the buffers in binding order (b[] below is bindings 0-5), then images[] (filled in ascending slot order for
    // exactly the slots of plan.layoutKey: PlanDescriptors sets dimension[s] for those slots only, pipeline_key.cpp).
    PushTemplateData pushData;
    if (push && pipeline.layouts.pushTemplate != VK_NULL_HANDLE)
    {
        // PlanPushTemplate's image count is the popcount of the key's slots (push_template.h); with the keys equal (checked first) those
        // are plan.vsSlots | plan.psSlots (SamplerLayoutKey), so no plan is built per draw.
        const uint32_t templateImages = uint32_t(__builtin_popcount(plan.vsSlots | plan.psSlots));
        if (pipeline.layouts.layoutKey != plan.layoutKey || templateImages != imageCount)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: push template of layout %llx for %u images, the draw binds %u (layout %llx)",
                 (unsigned long long)draw, (unsigned long long)pipeline.layouts.layoutKey, templateImages, imageCount, (unsigned long long)plan.layoutKey);
        pushData.buffers[0] = {vertexBuffer, vertexAt, vertexBytes}, pushData.buffers[1] = {slot.ring.buffer, systemAt, kSystemBytes};
        pushData.buffers[2] = {slot.ring.buffer, vsConstantsAt, kConstantsBytes}, pushData.buffers[3] = {slot.ring.buffer, psConstantsAt, kConstantsBytes};
        pushData.buffers[4] = {slot.ring.buffer, indexAt, indexBytes}, pushData.buffers[5] = {slot.ring.buffer, errorAt, kErrorsBytes};
        for (uint32_t k = 0; k < imageCount; k++)
            pushData.images[k] = images[k];
        pushedSetsSinceSwap++, pushTemplatesSinceSwap++;
    }
    else if (push)
    {
        const VkDescriptorBufferInfo b[6] = {{vertexBuffer, vertexAt, vertexBytes}, {slot.ring.buffer, systemAt, kSystemBytes},
                                             {slot.ring.buffer, vsConstantsAt, kConstantsBytes}, {slot.ring.buffer, psConstantsAt, kConstantsBytes},
                                             {slot.ring.buffer, indexAt, indexBytes}, {slot.ring.buffer, errorAt, kErrorsBytes}};
        const VkDescriptorType types[6] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                           VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        for (uint32_t k = 0; k < 6; k++)
        {
            pushBuffers[k] = b[k];
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstBinding = k;
            w.descriptorCount = 1;
            w.descriptorType = types[k];
            w.pBufferInfo = &pushBuffers[k];
            pushWrites[pushCount++] = w;
        }
        for (uint32_t k = 0; k < imageCount; k++)
        {
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstBinding = imageBindings[k];
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &images[k];
            pushWrites[pushCount++] = w;
        }
        pushedSetsSinceSwap++;
    }
    else if (set == VK_NULL_HANDLE)
    {
        VkDescriptorSetAllocateInfo setInfo{};
        setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setInfo.descriptorPool = slot.descriptors;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &layout;
        if (const VkResult r = vkAllocateDescriptorSets(dev.device, &setInfo, &set); r != VK_SUCCESS)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: vkAllocateDescriptorSets failed (VkResult %d): the slot's pool (%u sets) is exhausted "
                 "(this set: 6 buffers, %d samplers)", (unsigned long long)draw, int(r), kDrawsPerSlot, __builtin_popcount(plan.vsSlots | plan.psSlots));
        slot.sets++;
        // Dynamic (FM4_DESCRIPTOR_CACHE): base offset 0 and the fixed range; the draw's offsets go to vkCmdBindDescriptorSets.
        const VkDescriptorBufferInfo buffers[6] = {
            {vertexBuffer, descriptorCacheOn ? 0 : vertexAt, vertexBytes},                     // 0 XeVertexData
            {slot.ring.buffer, descriptorCacheOn ? 0 : systemAt, kSystemBytes},                // 1 XeSystem
            {slot.ring.buffer, descriptorCacheOn ? 0 : vsConstantsAt, kConstantsBytes},        // 2 XeVsConstants
            {slot.ring.buffer, descriptorCacheOn ? 0 : psConstantsAt, kConstantsBytes},        // 3 XePsConstants
            {slot.ring.buffer, descriptorCacheOn ? 0 : indexAt, descriptorCacheOn ? indexRange : indexBytes},  // 4 XeIndices (XE_RECT_LIST only)
            {slot.ring.buffer, descriptorCacheOn ? 0 : errorAt, kErrorsBytes},                 // 5 XeErrors
        };
        const VkDescriptorType uniform = descriptorCacheOn ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        const VkDescriptorType storage = descriptorCacheOn ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        const VkDescriptorType bufferTypes[6] = {storage, uniform, uniform, uniform, storage, storage};
        VkWriteDescriptorSet writes[6 + kTextureBindings];  // FM4 Recomp (M7 CPU cost): a fixed array (a vector allocation a miss before)
        uint32_t writeCount = 0;
        for (uint32_t b = 0; b < 6; b++)
        {
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = set;
            w.dstBinding = b;
            w.descriptorCount = 1;
            w.descriptorType = bufferTypes[b];
            w.pBufferInfo = &buffers[b];
            writes[writeCount++] = w;
        }
        for (uint32_t k = 0; k < imageCount; k++)
        {
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = set;
            w.dstBinding = imageBindings[k];
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &images[k];
            writes[writeCount++] = w;
        }
        vkUpdateDescriptorSets(dev.device, writeCount, writes, 0, nullptr);
        if (descriptorCacheOn)
            slot.descriptorCache.emplace(descriptorKey, set), descriptorMissesSinceSwap++;
    }
    // 9. One render pass, one draw. The render area is the whole target (every attachment loads and
    //    stores); the scissor is the guest's. Dynamic state in the pipeline's declared set.
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = pipeline.renderPass;
    pass.framebuffer = framebuffer;
    pass.renderArea = {{0, 0}, {targets.width, targets.height}};
    // FM4 Recomp (M7 GPU cost): the targets' pending resolve clears (render_hooks.h PendingClear) are recorded first in this pass;
    // the render area (FM4_PASS_AREA) must cover their rows as well as the draw's.
    uint32_t touchedBottom = runtime::video::scale::HostEdge(scissorBottom, targets.halves);  // host rows (FM4 Recomp, M9; the pending clears are host rectangles)
    for (const PendingClear& pc : targets.clears)
        touchedBottom = std::max(touchedBottom, uint32_t(std::max(0, pc.rect.offset.y)) + pc.rect.extent.height);
    const uint32_t areaRows = PassArea() ? std::min(targets.height, (touchedBottom + 31) & ~31u) : targets.height;
    pass.renderArea.extent.height = areaRows;
    // FM4_SUBSET_MERGE: the colour-only draw's pipeline for the open colour + depth pass (above); the draw then stays in that pass.
    if (DiagVsTally())  // FM4_DIAG_VS_TALLY (diagnostic)
    {
        char kind[96];
        std::snprintf(kind, sizeof kind, "%ux%u x%u colour %d depth %d", targets.width, targets.height, targets.samples,
                      key.colorUsed ? int(targets.format) : 0, key.depthUsed ? int(targets.depthFormat) : 0);
        VsTally& t = vsTally[{kind, vs->hash}];
        t.draws++, t.vertices += d.indices.count, t.fetchSlots = vs->t.vertexFetchSlots.size();
    }
    bool subset = false;
    const bool colourOnly = key.colorUsed && !key.depthUsed, depthOnly = !key.colorUsed && key.depthUsed;
    if (SubsetMerge() && openPass.commands == c && openPass.pass != VK_NULL_HANDLE && openPass.color != VK_NULL_HANDLE && openPass.depth != VK_NULL_HANDLE &&
        (colourOnly || depthOnly))
    {
        const char* why = colourOnly && targets.color != openPass.color ? "another colour target"
                          : depthOnly && targets.depth != openPass.depth ? "another depth target"
                          : touchedBottom > openPass.areaRows        ? "rows past the open area"
                                                                     : nullptr;
        PipelineCache::Entry inPass;
        KeyResult got{true, ""};
        if (why == nullptr)
        {
            const PipelineKey merged = colourOnly ? SubsetMergeKey(key, openPass.depthKeyFormat) : SubsetMergeKeyDepthOnly(key, openPass.colorKeyFormat);
            got = pipelines->Get(merged, *vs, *vsVariant, *ps, *psVariant, inPass);
            why = !got.ok ? "merged pipeline refused" : inPass.renderPass != openPass.pass ? "merged pipeline's render pass differs" : nullptr;
        }
        if (why == nullptr)
            pipeline = inPass, subset = true, subsetMergesSinceSwap++;
        else if (DiagSubsetPasses())  // FM4_DIAG_SUBSET_PASSES (diagnostic): the first candidates not merged, and why
        {
            static int lines = 0;
            if (lines++ < 20)
                std::fprintf(stderr, "[diag subset] draw %llu not merged: %s (colour format %u, open depth key format %u%s%s)\n", (unsigned long long)draw, why,
                             key.color[0].format, openPass.depthKeyFormat, got.ok ? "" : "; ", got.ok ? "" : got.reason.c_str());
        }
    }
    if (!subset && (openPass.commands != c || openPass.pass != pipeline.renderPass || openPass.framebuffer != framebuffer || touchedBottom > openPass.areaRows))
    {
        {
            const PassActivity activity("draw on another target");  // FM4_DIAG_PASS_TIMES label
            if (DiagSubsetPasses() && openPass.commands == c && openPass.pass != VK_NULL_HANDLE)
            {
                const VkImageView nc = key.colorUsed ? targets.color : VK_NULL_HANDLE, nd = key.depthUsed ? targets.depth : VK_NULL_HANDLE;
                const bool colorIn = nc == VK_NULL_HANDLE || nc == openPass.color, depthIn = nd == VK_NULL_HANDLE || nd == openPass.depth;
                const bool same = nc == openPass.color && nd == openPass.depth;
                const bool super = (openPass.color == VK_NULL_HANDLE || openPass.color == nc) && (openPass.depth == VK_NULL_HANDLE || openPass.depth == nd);
                subsetEnds[same ? 1 : colorIn && depthIn ? 0 : super ? 2 : 3]++;
            }
            EndOpenPass(openPass.commands);
        }
        int timing = -1;
        if (PassMerge() && PassTimes() && passQueriesUsed + 2 <= kPassQueries)  // FM4_DIAG_PASS_TIMES: after the work before it
        {
            char kind[96];
            std::snprintf(kind, sizeof kind, "%ux%u x%u colour %d depth %d", targets.width, targets.height, targets.samples,
                          key.colorUsed ? int(targets.format) : 0, key.depthUsed ? int(targets.depthFormat) : 0);
            vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, passQueries, passQueriesUsed);
            timing = int(passTimings.size());
            passTimings.push_back({passQueriesUsed, 0, kind, c, passJobs});
            passJobs.clear();
            passQueriesUsed += 2;
        }
        vkCmdBeginRenderPass(c, &pass, VK_SUBPASS_CONTENTS_INLINE);
        passesSinceSwap++;
        if (PassMerge())
            openPass = {c, pipeline.renderPass, framebuffer, timing, areaRows, key.colorUsed ? targets.color : VK_NULL_HANDLE,
                        key.depthUsed ? targets.depth : VK_NULL_HANDLE, key.depthFormat, key.color[0].format};
    }
    // The pending resolve clears of the acquired targets, in guest order before this draw (inside the pass: a new one, or the open one).
    for (const PendingClear& pc : targets.clears)
        {
            const VkClearRect r{pc.rect, 0, 1};
            vkCmdClearAttachments(c, 1, &pc.attachment, 1, &r);
        }
    if (openPass.timing >= 0)
        passTimings[size_t(openPass.timing)].draws++;
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
    if (push && pipeline.layouts.pushTemplate != VK_NULL_HANDLE)  // FM4_PUSH_TEMPLATE
    {
        if (pipeline.layouts.layoutKey != plan.layoutKey)  // FM4_SUBSET_MERGE swaps the pipeline: its key keeps samplerLayout (one Layouts entry)
            Stop(StopCode::Unimplemented, "renderer: draw %llu: the bound pipeline's layout %llx is not the draw's %llx", (unsigned long long)draw,
                 (unsigned long long)pipeline.layouts.layoutKey, (unsigned long long)plan.layoutKey);
        dev.cmdPushDescriptorSetWithTemplate(c, pipeline.layouts.pushTemplate, pipeline.layouts.pipeline, 0, &pushData);
    }
    else if (push)  // FM4_PUSH_DESCRIPTORS
        dev.cmdPushDescriptorSet(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layouts.pipeline, 0, pushCount, pushWrites);
    else if (descriptorCacheOn)  // the dynamic offsets in binding order (FM4_DESCRIPTOR_CACHE); the vertex binding spans the ring from 0
    {
        const uint32_t offsets[6] = {0, uint32_t(systemAt), uint32_t(vsConstantsAt), uint32_t(psConstantsAt), uint32_t(indexAt), uint32_t(errorAt)};
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layouts.pipeline, 0, 1, &set, 6, offsets);
    }
    else
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layouts.pipeline, 0, 1, &set, 0, nullptr);
    const VkViewport viewport{0.0f, 0.0f, float(targets.width), float(targets.viewHeight), hv.zMin, hv.zMax};  // the key's full extent (FM4 Recomp, M9, memory)
    vkCmdSetViewport(c, 0, 1, &viewport);
    // FM4 Recomp (M9): the guest scissor in host pixels, edge by edge (render_scale.h HostRect).
    const runtime::video::scale::Rect hs =
        runtime::video::scale::HostRect({scissorX, scissorY, scissorWidth, scissorHeight}, targets.halves);
    const VkRect2D scissor{{int32_t(hs.x), int32_t(hs.y)}, {hs.width, hs.height}};
    if (hs.height != 0 && hs.y + hs.height > targets.height)  // FM4 Recomp (M9, memory): the image holds the surface's rows (acquireTargets)
        Stop(StopCode::Unimplemented, "renderer: draw %llu: the scissor's host rows %u-%u reach past the %u rows the host target holds",
             (unsigned long long)draw, hs.y, hs.y + hs.height, targets.height);
    vkCmdSetScissor(c, 0, 1, &scissor);
    if (TracePsBindWanted(draw))  // FM4 Recomp (M7 CPU cost): the 12 register reads and the defines text (1-3 allocations) only for a traced draw
    {
        // FM4_TRACE_PS (diagnostic, ps_trace.h): the bind and dynamic state of a traced draw.
        PsBindTrace t;
        t.depthControl = runtime::gpu::ReadRegister(base, 0x2200), t.suScModeCntl = runtime::gpu::ReadRegister(base, 0x2205);
        t.pitch = target.pitch, t.msaa = target.msaa, t.colorBase = target.colorBase, t.colorFormat = target.colorFormat;
        t.depthBase = target.depthBase, t.depthFormat = target.depthFormat, t.pitchTiles = target.pitchTiles, t.edramRows = target.edramRows;
        t.targetHeight = target.targetHeight, t.colorWritten = target.colorWritten, t.depthWritten = target.depthWritten;
        t.keyDepthUsed = key.depthUsed, t.keyDepthWrite = key.depthWrite, t.keyDepthFunc = key.depthFunc, t.keyStencil = key.stencilEnable;
        t.keySamples = key.samples, t.hostWidth = targets.width, t.hostHeight = targets.height, t.hostSamples = targets.samples;
        t.scissorX = scissor.offset.x, t.scissorY = scissor.offset.y, t.scissorWidth = scissor.extent.width, t.scissorHeight = scissor.extent.height;
        t.viewportWidth = viewport.width, t.viewportHeight = viewport.height, t.minDepth = viewport.minDepth, t.maxDepth = viewport.maxDepth;
        t.noDepthDiagnostic = noDepthDiagnostic;
        t.colorControl = runtime::gpu::ReadRegister(base, 0x2202);
        for (uint32_t i = 0; i < 6; i++)
            t.vport[i] = runtime::gpu::ReadRegister(base, 0x210F + i);
        t.clipCntl = runtime::gpu::ReadRegister(base, 0x2204), t.vteCntl = runtime::gpu::ReadRegister(base, 0x2206);
        t.vtxCntl = runtime::gpu::ReadRegister(base, 0x2302), t.systemFlags = d.systemFlags;
        t.foldZScale = hv.scale[2], t.foldZOffset = hv.offset[2];
        const std::string psDefineText = psDefines.Joined();
        t.psDefines = psDefineText.c_str();
        TracePsBind(draw, t);
    }
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, d.depthStencil.front.ref);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, d.depthStencil.back.ref);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, d.depthStencil.front.compareMask);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, d.depthStencil.back.compareMask);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, d.depthStencil.front.writeMask);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, d.depthStencil.back.writeMask);
    vkCmdSetBlendConstants(c, d.blend.constant);
    const HostDepthBias bias = DepthBiasFor(d);  // the pipeline enables depth bias only with a depth attachment and an offset
    vkCmdSetDepthBias(c, bias.constant, 0.0f, bias.slope);
    if (zpdCounting)  // FM4_OCCLUSION_QUERIES: counted in a segment of this pass, weighted by the target's MSAA and internal scale
    {
        const uint64_t before = zpdUnit.nextSegment;
        ZpdBeginSegment(c, recording, zpd::HostSampleWeight(target.samples, targets.samples, targets.halves));
        zpdUnit.draws++, zpdUnit.countedOnly += d.countedOnly ? 1 : 0;
        ZpdLog("swap %llu draw %llu vs_%016llx ps_%016llx%s: %s %llu, x %u/%u, %ux%u x%u host target, colour mask %X, depth control %08X, scissor %ux%u@%d,%d",
               (unsigned long long)currentSwap, (unsigned long long)draw, (unsigned long long)vs->hash, (unsigned long long)ps->hash,
               d.countedOnly ? " (drawn for the count only)" : "", zpdUnit.query < 0 ? "NOT counted, after segment" : zpdUnit.nextSegment != before ? "new segment" : "segment",
               (unsigned long long)(zpdUnit.nextSegment - 1), zpdUnit.num, zpdUnit.den, targets.width, targets.height, targets.samples, d.colorWriteMask,
               runtime::gpu::ReadRegister(base, 0x2200), scissor.extent.width, scissor.extent.height, scissor.offset.x, scissor.offset.y);
    }
    switch (indices.kind)
    {
    case HostIndices::Kind::Direct: case HostIndices::Kind::RectList:
        vkCmdDraw(c, indices.vertexCount, 1, 0, 0);
        break;
    case HostIndices::Kind::Indexed:
        vkCmdBindIndexBuffer(c, slot.ring.buffer, indexAt, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(c, indices.vertexCount, 1, 0, 0, 0);
        break;
    }
    if (!PassMerge())
        ZpdEndSegment(), vkCmdEndRenderPass(c);
    drawsSinceSwap++;
}

bool DeferFences()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DEFER_FENCES");
        const bool asked = v != nullptr && v[0] == '1' && v[1] == '\0', enabled = asked && ResolveDefer();
        if (asked)
            std::fprintf(stderr, enabled ? "[gpu] deferred stream stores on (FM4_DEFER_FENCES=1): fences and memory writes land at GPU completion\n"
                                         : "[gpu] FM4_DEFER_FENCES=1 ignored: it needs FM4_RESOLVE_DEFER=1\n");
        return enabled;
    }();
    return on;
}

// FM4 Recomp (M9, FM4_OCCLUSION_QUERIES; command_processor.h RendererHooks::zpassDone): an EVENT_WRITE_ZPD at `physical`.
uint32_t ZpassDone(uint32_t physical)
{
    if (!enabled || !zpdUnit.on)
        return 0;
    ZpdEndSegment();  // inside the open pass: no segment spans a bracket's edge
    zpdUnit.zpds++;
    switch (zpd::Classify(physical))
    {
    case zpd::Kind::Begin:
    {
        zpd::Bracket old;
        const auto opened = zpdUnit.open.Open(physical, zpdUnit.nextSegment, currentSwap, &old);
        zpdUnit.begins++;
        if (opened != zpd::BracketBook::Opened::New)
            zpdUnit.dropped++;
        ZpdLog("swap %llu BEGIN %08X: segments from %llu, %u open%s", (unsigned long long)currentSwap, physical, (unsigned long long)zpdUnit.nextSegment,
               zpdUnit.open.Size(), opened == zpd::BracketBook::Opened::New ? "" : " (an open bracket was replaced or dropped)");
        return 1;
    }
    case zpd::Kind::End:
    {
        ZpdUnit::End end{zpdUnit.nextToken, physical, zpdUnit.nextSegment, zpdUnit.nextSegment, false, 0};
        zpdUnit.nextToken = zpdUnit.nextToken == UINT32_MAX ? 2 : zpdUnit.nextToken + 1;
        zpd::Bracket closed;
        if (zpdUnit.open.Close(physical, &closed))
            end.s0 = closed.s0;
        else
            zpdUnit.unmatched++;  // an END without its BEGIN: zero samples
        zpdUnit.ends[end.token % ZpdUnit::kEnds] = end;
        zpdUnit.endReports++;
        ZpdLog("swap %llu END %08X: segments %llu..%llu, token %u", (unsigned long long)currentSwap, physical, (unsigned long long)end.s0,
               (unsigned long long)end.s1, end.token);
        return end.token;
    }
    default:
        Stop(StopCode::Unimplemented, "renderer: EVENT_WRITE_ZPD with its report at %08X, neither a BEGIN (+0x20) nor an END (+0x00) of a 64-byte block (FM4_OCCLUSION_QUERIES)",
             physical);
    }
}

// The count of the END `token` (RendererHooks::zpdCount): called when the report's store lands, after every part recorded before
// the END has retired (their segments are in the ring; ZpdHarvest). A segment the ring no longer holds counts 0 and is tallied.
uint32_t ZpdCount(uint32_t token)
{
    ZpdUnit::End& end = zpdUnit.ends[token % ZpdUnit::kEnds];
    if (end.token != token)
    {
        zpdUnit.lost++;
        return 0;
    }
    if (end.known)  // the report's second dword of the count (Total_A, then ZPass_A): the same value
        return end.count;
    uint32_t lost = 0;
    uint64_t raw = 0;
    const uint32_t count = zpd::EndCount(zpdUnit.results->Sum(end.s0, end.s1, lost, &raw));
    zpdUnit.lost += lost, zpdUnit.stored++, zpdUnit.nonZero += count != 0 ? 1 : 0, zpdUnit.lastCount = count;
    zpdUnit.maxCount = std::max<uint64_t>(zpdUnit.maxCount, count);
    ZpdLog("END %08X token %u: segments %llu..%llu, %llu host samples: %u guest samples stored%s", end.address, token, (unsigned long long)end.s0,
           (unsigned long long)end.s1, (unsigned long long)raw, count, lost ? " (segments lost)" : "");
    end.known = true, end.count = count;
    return count;
}

bool DeferStore(uint32_t physical)
{
    if (!DeferFences() || !slots[0].open)
        return false;
    MarkGuestWritten(physical, 4);  // pending: a draw reading these bytes, or a CPU sync point, waits for the retire
    return true;
}

bool WorkOpen() { return DeferFences() && slots[0].open; }

bool PendingAt(uint32_t physical, uint32_t bytes)
{
    return ResolveDefer() && slots[0].open && GuestWritesPending() && GuestWritesOverlap(physical, bytes);
}

void RingIdle()
{
    if (slots[0].open)
        FlushSlot(0, FlushCause::Idle);
    else
        runtime::gpu::ApplyDeferredStores();  // nothing open: every submission they followed is already complete
}

// FM4 Recomp (M7, FM4_GUEST_MEMORY_GPU; gpu_read_guard.h G5, PlanGuardFlush): the parts up to `reader` submitted and confirmed
// complete, at a packet boundary (no command buffer handle is live). With FM4_PARTIAL_RETIRE only up to `reader` (RetireUpTo splits
// the open part off when it holds the read); otherwise, or when no idle slot can take the rest of the work (ForceSplit), the whole
// slot. Never inside a retire: its write-backs and deferred stores land after every read of the parts it retires (the guard's
// completion is published first), and a direct read is refused over a host page with a pending write-back, so they cannot need it.
void FlushForGpuReader(uint64_t reader)
{
    GpuReadGuard* const g = GuestMemoryGpuGuard();
    const GuardFlushPlan plan = PlanGuardFlush(reader, g->CompletedSerial(), currentPartSerial, PartialRetire());
    if (!plan.flush || !slots[0].open)
        return;
    if (retiring)
        Stop(StopCode::Unimplemented, "renderer: GPU read guard: a write during a retire needs GPU part %llu (completed %llu)", (unsigned long long)reader,
             (unsigned long long)g->CompletedSerial());
    if (plan.wholeSlot || !RetireUpTo(plan.retireUpTo, FlushCause::WriteWatch))
        FlushSlot(0, FlushCause::WriteWatch);
}

void HonourGpuReadGuard()
{
    GpuReadGuard* const g = GuestMemoryGpuGuard();
    if (g == nullptr)
        return;
    if (const uint64_t reader = g->TakeRequest(); reader != 0)
        FlushForGpuReader(reader);
}

void BeforeCpWrite(uint32_t physical, uint32_t bytes)
{
    GpuReadGuard* const g = GuestMemoryGpuGuard();
    if (g == nullptr)
        return;
    const uint64_t reader = g->WriteReader(physical, bytes);
    const GpuReadGuard::Action action = g->Plan(reader);
    if (action == GpuReadGuard::Action::Proceed)
        return;
    const auto start = std::chrono::steady_clock::now();
    cpGuardWaitsSinceSwap++, cpGuardForcedSinceSwap += action == GpuReadGuard::Action::SubmitThenWait ? 1 : 0;
    cpGuardWaitsTotal++;
    g->NoteWaitedStore(physical, bytes, reader);  // FM4 Recomp (M7, FM4_GUARD_HOT): as a guest store that waited
    FlushForGpuReader(reader);
    cpGuardNsSinceSwap += NsSince(start);
    if (reader > g->CompletedSerial())
        Stop(StopCode::Unimplemented, "renderer: GPU read guard: the command processor's store to %08X+%u still waits for GPU part %llu after a flush "
                                      "(completed %llu)", physical, bytes, (unsigned long long)reader, (unsigned long long)g->CompletedSerial());
}

void SyncGuestWrites(uint32_t opcode)
{
    if (ResolveDefer() && slots[0].open && GuestWritesPending())
    {
        if (drain::Enabled())  // FM4_DIAG_DRAIN (diagnostic): the drain's stages (diag_drain.h)
            drain::FlushBegin(opcode, recording, drawsInBuffer, submitted);
        FlushSlot(0, FlushCause::Sync);
        if (drain::Enabled())
            drain::FlushEnd();
        syncFlushesByPacketSinceSwap[opcode & 0x7F]++;
    }
}

void LoadShader(uint32_t type, const uint32_t* words, uint32_t count)
{
    if (!enabled)
        return;
    shadersSinceSwap++;
    const uint64_t before = shaders->Stats().translations;
    std::string reason;
    const CachedShader* shader = shaders->Load(type, words, count, reason);
    if (shader == nullptr)
        Stop(StopCode::Unimplemented, "renderer: %s", reason.c_str());  // the hash is in the reason (shader_cache.cpp)
    if (shaders->Stats().translations != before)  // the first load of this hash: name its class for the allow-list decision
        std::fprintf(stderr, "[shader] %s_%016llx: %u words, class %s, %zu vertex fetch slot(s), %zu texture slot(s), %zu bool(s), %zu loop(s)%s\n",
                     shader->pixel ? "ps" : "vs", (unsigned long long)shader->hash, shader->words, ShaderClassName(shader->cls).c_str(),
                     shader->t.vertexFetchSlots.size(), shader->t.textureSlots.size(), shader->t.boolConstants.size(), shader->t.loopConstants.size(),
                     !shader->t.pathZeroReads.empty() ? (", " + std::to_string(shader->t.pathZeroReads.size()) +
                                                " path-dependent read(s) of an unwritten register (zero, as Xenia)").c_str() : "");
}

void QuiesceForStop()
{
    if (!gate.BeginStop())
        return;  // another stop drains
    runtime::video::keepbusy::Quiesce();  // FM4_GPU_KEEP_BUSY: its submission in flight completes, no further one
    const uint64_t limit = (guard ? guard->FenceTimeoutNs() : gpu::FenceTimeoutNs(gpu::kFenceTimeoutDefaultMs)) + 1000000000ull;
    const auto outcome = gate.AwaitIdle(limit, NowNs, [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
    const gpu::SubmissionGate::View v = gate.Load();
    static const char* const names[] = {"idle", "completed", "STRANDED (completion unknown)", "TIMED OUT", "own thread in flight"};
    std::fprintf(stderr, "[gpu at stop] %s; %u submission(s) counted in flight\n", names[int(outcome)], v.inFlight);
}

void ReportForSignalStop()
{
    // Signal-safe only: an atomic or and load, write(2). Not waited for: completion UNKNOWN at exit.
    gate.BeginStop();
    const gpu::SubmissionGate::View v = gate.Load();
    if (v.inFlight == 0)
        return;
    char text[] = "[gpu at stop] signal stop: 0000000000 submission(s) in flight, not waited for; GPU completion UNKNOWN at exit\n";
    uint32_t n = v.inFlight;
    for (int i = 0; i < 10; i++, n /= 10)
        text[36 - i] = char('0' + n % 10);
    (void)!::write(STDERR_FILENO, text, sizeof text - 1);
}

void RunSelfCheck()
{
    if (!enabled || options.mode != Mode::SelfCheck)
        Stop(StopCode::Setup, "renderer: the self-check needs --renderer self-check");
    const std::string dir = options.captureDir;

    // 1. One clear and one upload into the display image, read back: the device works and the
    //    lifetime defers the staging buffer's destruction until the completion is confirmed.
    //    Expected words: A2B10G10R10 packs R in bits 0-9, G 10-19, B 20-29, A 30-31 (Vulkan
    //    VK_FORMAT_A2B10G10R10_UNORM_PACK32). Clear (1, 0, 1, 1): exact in UNORM (0 and 1 map to 0
    //    and 1023) -> 3 << 30 | 1023 << 20 | 0 << 10 | 1023 = 0xFFF003FF. The pattern is copied
    //    bit for bit: texel (x, y) of a 32x32 block at (64, 64) = 3 << 30 | 0x155 << 20 | (32 y) << 10 | 32 x.
    Require(guard->Begin("self-check clear"));
    Transient staging = MakeBuffer(32 * 32 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    for (uint32_t y = 0; y < 32; y++)
        for (uint32_t x = 0; x < 32; x++)
            static_cast<uint32_t*>(staging.mapped)[y * 32 + x] = 3u << 30 | 0x155u << 20 | (32 * y) << 10 | (32 * x);
    const VkCommandBuffer c = BeginSlot(0);
    Expect(guard->Hold(0, staging), "the staging buffer is held by idle slot 0");
    ImageBarrier(c, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkClearColorValue magenta{{1.0f, 0.0f, 1.0f, 1.0f}};
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(c, display, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &magenta, 1, &all);
    ImageBarrier(c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = {64, 64, 0};
    copy.imageExtent = {32, 32, 1};
    vkCmdCopyBufferToImage(c, staging.buffer, display, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    EndWithReadback(c);
    const uint64_t destroyedBefore = transientsDestroyed;
    Require(guard->Submit(0));
    // Observed while in flight, judged after the retire: a failing Expect stops, and a stop with this
    // thread's own submission in flight would not wait for it (SubmissionGate::AwaitIdle OwnThread).
    const bool heldInFlight = guard->Held(0) == 1 && transientsDestroyed == destroyedBefore;
    Require(guard->Retire(0));
    Expect(heldInFlight, "in flight: the staging buffer is held, not destroyed");
    Expect(guard->Held(0) == 0 && transientsDestroyed == destroyedBefore + 1, "confirmed: the staging buffer destroyed after the fence");
    const uint32_t* px = static_cast<const uint32_t*>(readback.mapped);
    Expect(px[0] == 0xFFF003FFu && px[size_t(719) * kDisplayWidth + 1279] == 0xFFF003FFu, "clear read back as 0xFFF003FF");
    Expect(px[size_t(64) * kDisplayWidth + 64] == (3u << 30 | 0x155u << 20) &&
               px[size_t(64 + 31) * kDisplayWidth + 64 + 31] == (3u << 30 | 0x155u << 20 | 992u << 10 | 992u) &&
               px[size_t(63) * kDisplayWidth + 64] == 0xFFF003FFu,
           "upload read back bit for bit, the clear around it");
    Capture(0, "self-check clear");

    // 2. The bounded fence wait on the real driver, on a scratch gate, guard and flag: a fence that is
    //    created and never submitted (valid Vulkan: waiting on it simply times out) behind a test seam
    //    whose Submit submits nothing. The minimum limit (100 ms) must time out, strand, keep the
    //    held resource and write the scratch GPU_STOP.
    {
        VkFence never = VK_NULL_HANDLE;
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        Check(vkCreateFence(dev.device, &fi, nullptr, &never), "vkCreateFence");
        struct NeverSubmitted final : SubmissionBackend<int>
        {
            VkFence fence;
            int destroyed = 0, submits = 0;
            int Submit(uint32_t) override { return submits++, 0; }  // the seam: nothing reaches the queue
            int WaitFence(uint32_t, uint64_t ns) override { return int(vkWaitForFences(dev.device, 1, &fence, VK_TRUE, ns)); }
            int FenceStatus(uint32_t) override { return int(vkGetFenceStatus(dev.device, fence)); }
            void Destroy(int&) override { destroyed++; }
        } seam;
        seam.fence = never;
        gpu::SubmissionGate scratch;
        const std::string flag = dir + "/selfcheck_stranded_GPU_STOP";
        GpuGuard<int, 1> g(seam, scratch, flag, gpu::FenceTimeoutNs(gpu::kFenceTimeoutMinMs));
        Expect(g.Hold(0, 7) && g.Submit(0).ok && seam.submits == 1, "scratch: held and 'submitted' through the seam");
        const uint64_t t0 = NowNs();
        const Verdict r = g.Retire(0);
        const uint64_t waited = NowNs() - t0;
        std::fprintf(stderr, "[gpu self-check] scratch retire: %s (waited %llu ms)\n", r.reason.c_str(), (unsigned long long)(waited / 1000000));
        Expect(!r.ok && r.stranded && waited >= 100000000ull && waited < 2000000000ull, "a never-signalled fence times out within the bound and strands");
        Expect(seam.destroyed == 0 && g.Held(0) == 1 && scratch.Load().stranded, "stranded: the resource is kept and the gate refuses");
        Expect(gpu::CheckGpuStop(flag.c_str()).stopped, "stranded: GPU_STOP written (scratch path)");
        Expect(!g.Submit(0).ok && seam.submits == 1, "after the stranding nothing more is submitted");
        Expect(g.Shutdown() == 1 && seam.destroyed == 0, "shutdown keeps the stranded resource");
        vkDestroyFence(dev.device, never, nullptr);  // never submitted: not in use
    }

    // 3. GPU_STOP honoured in-process: a planted scratch flag refuses before the backend is reached.
    {
        const std::string flag = dir + "/selfcheck_planted_GPU_STOP";
        std::string error;
        Expect(WriteGpuStop(flag.c_str(), "planted by the self-check", error), "scratch GPU_STOP planted");
        struct Counting final : SubmissionBackend<int>
        {
            int calls = 0;
            int Submit(uint32_t) override { return calls++, 0; }
            int WaitFence(uint32_t, uint64_t) override { return calls++, 0; }
            int FenceStatus(uint32_t) override { return calls++, 0; }
            void Destroy(int&) override { calls++; }
        } counting;
        gpu::SubmissionGate scratch;
        GpuGuard<int, 1> g(counting, scratch, flag, gpu::FenceTimeoutNs(gpu::kFenceTimeoutMinMs));
        const Verdict v = g.Submit(0);
        // CheckGpuStop names the flag's first line: the writer's provenance line ("GPU_STOP written <when> by
        // fm4recomp (in-process); only the owner clears it."), the reason being its second line (gpu_stop_write.h,
        // the same layout as scripts/gpu_run.sh writes).
        Expect(!v.ok && !v.stranded && v.reason.find("GPU_STOP written ") != std::string::npos &&
                   v.reason.find("by fm4recomp (in-process)") != std::string::npos && counting.calls == 0 && scratch.Load().inFlight == 0,
               "a planted GPU_STOP refuses the submission, naming the flag's first line; the backend is not called");
    }

    // 4. The loop-work cap (2^26): two loops of 64 (low bytes of 0x01020340 and 0x00000040) are
    //    4096 = 2^12 per invocation, so 2^14 = 16384 pixel invocations reach 2^26 exactly (allowed)
    //    and 16385 exceed it (refused).
    {
        const uint32_t loops[] = {0x01020340u, 0x00000040u};
        using G = GpuGuard<Transient, kSlots>;
        Expect(G::DrawLoopWork({}, 1u << 30, loops, 16384).ok, "loop work 2^26 exactly is allowed (a stage without loops is unbounded)");
        Expect(!G::DrawLoopWork({}, 0, loops, 16385).ok, "loop work 2^26 + 2^12 is refused");
    }

    // 5. The capped capture ring with real files: two records at most, so the third capture deletes
    //    the first; a capture larger than the byte cap is refused and nothing is written.
    {
        const std::vector<uint8_t> tiny = EncodePngRgb8(1, 1, {0, 0, 0});
        CaptureStore<2> ring(dir, "selfcheck_ring_", tiny.size() * 2);
        auto write = [&](const std::string& path) {
            FILE* f = std::fopen(path.c_str(), "wb");
            if (f == nullptr)
                return false;
            const bool ok = std::fwrite(tiny.data(), 1, tiny.size(), f) == tiny.size();
            return std::fclose(f) == 0 && ok;
        };
        std::string error;
        for (uint64_t id = 1; id <= 3; id++)
            Expect(ring.Add(id, tiny.size(), write, error), "a tiny capture is accepted");
        Expect(::access(ring.PathOf(1).c_str(), F_OK) != 0 && ::access(ring.PathOf(2).c_str(), F_OK) == 0 &&
                   ::access(ring.PathOf(3).c_str(), F_OK) == 0,
               "the oldest capture's file was deleted on eviction, the newest two kept");
        CaptureStore<2> small(dir, "selfcheck_toolarge_", tiny.size() - 1);
        Expect(!small.Add(9, tiny.size(), write, error) && ::access(small.PathOf(9).c_str(), F_OK) != 0, "an oversized capture is refused, no file");
    }

    // 6. (FM4_SELFCHECK_EDRAM=depth4x2x only) The EDRAM depth transfer 4x -> 2x on the device, isolated: known depth and
    //    stencil uploaded into the 1x key, transferred 1x -> 4x and 4x -> 2x by the unit's own transfer passes, every guest
    //    sample of both dumped and compared with the EDRAM sample layout's expectation (edram_vk.h RecordDepthSelfCheck).
    const bool edramCheck = EdramSelfCheckRequested();
    if (edramCheck)
    {
        Require(guard->Begin("self-check EDRAM depth 4x -> 2x"));
        const VkCommandBuffer e = BeginSlot(0);
        std::string error;
        const bool recorded = RecordDepthSelfCheck(e, error);
        if (!recorded)
            std::fprintf(stderr, "[gpu self-check] EDRAM depth 4x -> 2x: %s\n", error.c_str());
        // Ended even after a refusal, so the command buffer is well formed (nothing below submits it then). No display
        // readback: step 1 left the display image in TRANSFER_SRC, and this step reads its results from the EDRAM staging buffer.
        Check(vkEndCommandBuffer(e), "vkEndCommandBuffer");
        Expect(recorded, "EDRAM depth 4x -> 2x recorded (1x upload, transfers 1x -> 4x and 4x -> 2x, depth dumps)");
        Require(guard->Submit(0));
        Require(guard->Retire(0));
        const DepthSelfCheckResult r = VerifyDepthSelfCheck();
        std::fprintf(stderr, "[gpu self-check] EDRAM depth: 4x %llu sample(s), %llu mismatch(es); 2x %llu sample(s), %llu mismatch(es)%s%s\n",
                     (unsigned long long)r.checked4x, (unsigned long long)r.mismatches4x, (unsigned long long)r.checked2x,
                     (unsigned long long)r.mismatches2x, r.first.empty() ? "" : "; first: ", r.first.c_str());
        Expect(r.checked4x == 640ull * 16 * 4 && r.mismatches4x == 0, "EDRAM 1x -> 4x depth: every guest sample's depth and stencil word as the layout places it");
        Expect(r.checked2x == 1280ull * 16 * 2 && r.mismatches2x == 0, "EDRAM 4x -> 2x depth: every guest sample's depth and stencil word as the layout places it");
        std::fprintf(stderr, "[gpu self-check] EDRAM depth formats: kD24FS8 -> kD24S8 %llu pixel(s), %llu mismatch(es); back %llu pixel(s), %llu mismatch(es)\n",
                     (unsigned long long)r.checkedS8, (unsigned long long)r.mismatchesS8, (unsigned long long)r.checkedRoundTrip,
                     (unsigned long long)r.mismatchesRoundTrip);
        Expect(r.checkedS8 == 1280ull * 32 && r.mismatchesS8 == 0, "EDRAM depth kD24FS8 -> kD24S8: every pixel's word as ConvertDepthFormat gives it");
        Expect(r.checkedRoundTrip == 1280ull * 32 && r.mismatchesRoundTrip == 0, "EDRAM depth kD24S8 -> kD24FS8 (back): every pixel's word as ConvertDepthFormat gives it");
    }

    Shutdown();
    Stop(StopCode::GpuSelfCheck, "GPU self-check passed: device, clear and upload read back, deferred destruction, bounded fence wait (stranding "
                            "on a scratch flag), GPU_STOP refusal, loop-work cap, capped captures%s; %llu validation message(s)",
         edramCheck ? ", EDRAM depth transfers 4x -> 2x and between kD24FS8 and kD24S8" : "", (unsigned long long)ValidationMessageCount());
}

}  // namespace runtime::video
