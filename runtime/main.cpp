// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/main.cpp (GPL-3.0-or-later, same owner), reviewed and re-tested here.
// FM4 Recomp changes: the binary is fm4recomp; --image, --xex and --deadline are required (no silent
// defaults); messages name our scripts. M3 (after ForzaRecomp main.cpp): the
// executable's module (XEX header copy and loader entry), data imports with documented values
// (kernel/module.cpp) while the rest keep their guard pages, and the title process with its main
// thread (PCR in r13, KTHREAD, TLS from the executable's template, stack).
//
// fm4recomp: load the image the translation was generated from, set up the guest address space
// and call the title entry. M2 implements no import and no thread layout: the run ends at the
// first strict stop (stops.h), which the launch harness records.
//
//   fm4recomp --image game/survey/image.bin --xex game/files/default.xex --disc game/files --modules game/modules
//             --deadline <seconds>
#include "fm4_image.h"
#include "fm4_modules.h"
#include "guest_memory.h"
#include "kernel/arena.h"
#include "kernel/file_system.h"
#include "kernel/content.h"
#include "kernel/audio.h"
#include "audio_sdl.h"
#include "xma_ffmpeg.h"
#include "ldi_mspack.h"
#include "kernel/xma.h"
#include "kernel/graphics.h"
#include "kernel/command_processor.h"
#include "video/renderer.h"
#include "video/guest_memory_vk.h"  // FM4 Recomp: GuestMemoryGpuAsked (FM4_GUEST_MEMORY_GPU)
#include "video/window_sdl.h"
#include "disc_setup_sdl.h"
#include "kernel/disc_swap.h"
#include "kernel/imports.h"
#include "kernel/input_live.h"
#include "kernel/loader.h"
#include "kernel/module.h"
#include "kernel/thread_state.h"
#include "kernel/threads.h"
#include "kernel/virtual_memory.h"
#include "layout.h"
#include "stops.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <pthread.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

static_assert(fm4::kImageBase == PPC_IMAGE_BASE && fm4::kImageSize == PPC_IMAGE_SIZE,
              "the generated tables and ppc/ describe different images");

// Title-specific hooks (M2-T03): generated from config/title_hooks.toml (FM4's own image, SHA-256-checked; each opt-in by a flag)
// into fm4_hooks.cpp; no other title's hook or address is ever ported.
namespace fm4 { int TitleHookCount(); void ReportTitleHooks(); }

namespace {

uint8_t* base = nullptr;
std::vector<const char*> dataImports;  // by guard index


const char* DescribeGuest(uint32_t guest)
{
    const long index = runtime::GuardIndex(guest, size_t(getpagesize()), dataImports.size());
    if (index >= 0)
        return dataImports[size_t(index)];  // a data import's guard: its value is undocumented
    if (guest >= runtime::kStacksBase && guest < runtime::kStacksEnd)
        return "a guest stack guard page (stack overflow or underflow)";
    if (guest - runtime::xma::kRegisterBase < runtime::xma::kRegisterBytes)
        return "the XMA decoder's registers: an access the translator did not mark as a device access (recomp/mmio.h)";
    return nullptr;
}

// The first `size` bytes of a file, which must have this CRC-32 (the image, or the XEX header the
// tables were generated from).
std::vector<uint8_t> ReadChecked(const char* path, uint32_t size, uint32_t crc, bool wholeFile)
{
    std::vector<uint8_t> data;
    if (FILE* file = std::fopen(path, "rb"))
    {
        data.resize(size + (wholeFile ? 1 : 0));
        data.resize(std::fread(data.data(), 1, data.size(), file));
        std::fclose(file);
    }
    if (data.size() != size || runtime::Crc32(data.data(), data.size()) != crc)
        Stop(StopCode::Setup, "%s is not the file the translation was generated from (re-run scripts/survey.sh and rebuild)", path);
    return data;
}

runtime::ThreadState mainThread;

void* RunGuest(void*)
{
    pthread_setname_np("fm4 guest main");  // FM4 Recomp: thread_cpu.h roles
    runtime::UseFaultStackOnThisThread();
    PPCContext ctx{};
    ctx.r1.u64 = mainThread.stackBase - 0x100;  // room for the entry's back chain and save area
    ctx.r13.u64 = mainThread.pcr;
    RegisterGuestContext(&ctx, mainThread.kthread);  // the hang report
    std::fprintf(stderr, "[runtime] calling the title entry %08X\n", fm4::kEntry);
    PPCFunc* entry = PPC_LOOKUP_FUNC(base, fm4::kEntry);
    if (entry == nullptr)
        Stop(StopCode::Setup, "the title entry %08X has no translated function", fm4::kEntry);
    entry(ctx, base);
    Stop(StopCode::EntryReturned, "the title entry returned (r3=%08X)", ctx.r3.u32);
}

}  // namespace

int main(int argc, char** argv)
{
    const char* imagePath = nullptr;
    const char* xexPath = nullptr;
    const char* discPath = nullptr;
    const char* discMapPath = nullptr;
    const char* disc2Path = nullptr;     // FM4 Recomp (Disc 2): optional, with its map (XamSwapDisc)
    const char* disc2MapPath = nullptr;
    const char* discSetupTool = nullptr;  // FM4 Recomp (Disc 2): the kit's setup tool, for a disc set up from the game (window only)
    const char* gpuStopPath = nullptr;
    const char* userDataPath = nullptr;
    const char* audioMode = nullptr;
    const char* modulesPath = nullptr;
    const char* rendererMode = nullptr;
    const char* gpuCaptureDir = nullptr;
    const char* vkValidation = nullptr;
    const char* shaderClasses = nullptr;
    int stopAtSwap = 0;
    int deadline = 0;
    for (int i = 1; i < argc; i += 2)
    {
        if (i + 1 == argc)
            Stop(StopCode::Setup, "option %s needs a value", argv[i]);
        if (!std::strcmp(argv[i], "--image")) imagePath = argv[i + 1];
        else if (!std::strcmp(argv[i], "--xex")) xexPath = argv[i + 1];
        else if (!std::strcmp(argv[i], "--disc")) discPath = argv[i + 1];
        else if (!std::strcmp(argv[i], "--disc-map")) discMapPath = argv[i + 1];
        else if (!std::strcmp(argv[i], "--disc2")) disc2Path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--disc2-map")) disc2MapPath = argv[i + 1];
        else if (!std::strcmp(argv[i], "--disc-setup")) discSetupTool = argv[i + 1];
        else if (!std::strcmp(argv[i], "--gpu-stop")) gpuStopPath = argv[i + 1];
        else if (!std::strcmp(argv[i], "--user-data")) userDataPath = argv[i + 1];
        else if (!std::strcmp(argv[i], "--audio")) audioMode = argv[i + 1];
        else if (!std::strcmp(argv[i], "--modules")) modulesPath = argv[i + 1];
        else if (!std::strcmp(argv[i], "--renderer")) rendererMode = argv[i + 1];
        else if (!std::strcmp(argv[i], "--gpu-capture")) gpuCaptureDir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--vk-validation")) vkValidation = argv[i + 1];
        else if (!std::strcmp(argv[i], "--shader-classes")) shaderClasses = argv[i + 1];
        else if (!std::strcmp(argv[i], "--stop-at-swap")) stopAtSwap = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--deadline")) deadline = std::atoi(argv[i + 1]);
        else Stop(StopCode::Setup, "unknown option %s", argv[i]);
    }
    if (imagePath == nullptr || xexPath == nullptr || discPath == nullptr || discMapPath == nullptr || gpuStopPath == nullptr || userDataPath == nullptr || modulesPath == nullptr || audioMode == nullptr || rendererMode == nullptr || deadline <= 0 || stopAtSwap <= 0)
        Stop(StopCode::Setup, "usage: fm4recomp --image <image.bin> --xex <default.xex> --disc <extracted disc> --disc-map <disc1_gdfx.tsv> "
             "--modules <game/modules> --gpu-stop <research/GPU_STOP> --user-data <game/user-data> --audio <audible|mute> --renderer <headless|self-check|present|window> --stop-at-swap <n> --deadline <seconds> (all required; "
             "a renderer other than headless also requires --gpu-capture <evidence dir> --vk-validation <on|off> --shader-classes <none|branch,predicate,loop>; "
             "optional: --disc2 <extracted Disc 2> --disc2-map <disc2_gdfx.tsv>, both or neither; with --renderer window, --disc-setup <the kit's setup panel, bin/fm4recomp-setup> "
             "sets up a disc the game asks for that is not given)");
    if ((disc2Path == nullptr) != (disc2MapPath == nullptr))
        Stop(StopCode::Setup, "--disc2 and --disc2-map go together (the extracted Disc 2 and its GDFX map)");
    // Audio (audio policy): audible plays through the default device (SDL3); mute opens no device and keeps
    // the same 48 kHz pacing, frame conversion and callbacks (kernel/audio.h).
    if (!std::strcmp(audioMode, "audible"))
        runtime::audio::StartSdlAudio();
    else if (!std::strcmp(audioMode, "mute"))
    {
        runtime::audio::SetSink(runtime::audio::Sink::Clock);
        std::fprintf(stderr, "[audio] mute: no audio device opened; the 48 kHz clock pulls one frame every 256 samples (same pacing, conversion and callbacks as audible)\n");
    }
    else
        Stop(StopCode::Setup, "--audio must be audible or mute, not %s", audioMode);
    // The scripted gamepad (test only, kernel/input_script.h): FM4_INPUT_SCRIPT=<file> connects a pad to user 0 that
    // presses the file's buttons at its vblanks; a malformed file stops (9) here, before the title runs.
    if (const char* script = std::getenv("FM4_INPUT_SCRIPT"); script != nullptr && *script != '\0')
        runtime::input::LoadScript(script);
    // FM4 Recomp: FM4_INPUT_LIVE=1 with --renderer window connects the live pad (input_live.h) instead.
    const bool liveInput = std::getenv("FM4_INPUT_LIVE") != nullptr && !std::strcmp(std::getenv("FM4_INPUT_LIVE"), "1");
    if (liveInput && (std::strcmp(rendererMode, "window") || runtime::input::ScriptLoaded()))
        Stop(StopCode::Setup, "FM4_INPUT_LIVE=1 needs --renderer window and no FM4_INPUT_SCRIPT");
    if (liveInput)
        runtime::input::EnableLive();
    runtime::xma::SetDecoderFactory(runtime::xma::FfmpegDecoderFactory());  // XMA decoding (kernel/xma.h)
    runtime::ldi::SetFactory(runtime::ldi::MspackDecoderFactory());  // LZX decompression (kernel/ldi.h)
    runtime::gpu::SetStopAtSwap(uint32_t(stopAtSwap));
    runtime::gpu::SetGpuStopPath(gpuStopPath);
    // The renderer (M4; runtime/video/renderer.h). Only scripts/gpu_run.sh passes a mode other than headless.
    runtime::video::Options video;
    video.gpuStopPath = gpuStopPath;
    video.captureDir = gpuCaptureDir;
    video.fenceTimeoutEnv = std::getenv("FM4_GPU_FENCE_TIMEOUT_MS");
    if (!std::strcmp(rendererMode, "headless")) video.mode = runtime::video::Mode::Headless;
    else if (!std::strcmp(rendererMode, "self-check")) video.mode = runtime::video::Mode::SelfCheck;
    else if (!std::strcmp(rendererMode, "present")) video.mode = runtime::video::Mode::Present;
    else if (!std::strcmp(rendererMode, "window")) video.mode = runtime::video::Mode::Window;  // present + a visible window (unit 4b)
    else Stop(StopCode::Setup, "--renderer must be headless, self-check, present or window, not %s", rendererMode);
    if (video.mode != runtime::video::Mode::Headless)
    {
        if (gpuCaptureDir == nullptr || vkValidation == nullptr || (std::strcmp(vkValidation, "on") && std::strcmp(vkValidation, "off")))
            Stop(StopCode::Setup, "--renderer %s requires --gpu-capture <dir> and --vk-validation <on|off>", rendererMode);
        video.validation = !std::strcmp(vkValidation, "on");
        // The shader-class allow-list (video/shader_class.h): required, no silent default; parsed and refused by the renderer.
        if (shaderClasses == nullptr)
            Stop(StopCode::Setup, "--renderer %s requires --shader-classes <none|branch,predicate,loop> (a never-seen shader class runs first in its own approved run)",
                 rendererMode);
        video.shaderClasses = shaderClasses;
        video.shaderDumpDir = std::getenv("FM4_SHADER_DUMP");  // the translated GLSL beside the CP's .bin dumps (video/shader_cache.h)
    }
    runtime::SetModuleImages(fm4::kModules, std::size(fm4::kModules), modulesPath);  // XexLoadImage (kernel/loader.h)
    runtime::MountDisc(discPath, discMapPath);
    if (disc2Path != nullptr)
        runtime::AddDisc(2, disc2Path, disc2MapPath);  // put in the drive when the game asks (XamSwapDisc, kernel/file_system.h)
    if (discSetupTool != nullptr)
    {
        // A disc the game asks for and the launcher did not give is set up from the game (kernel/disc_swap.h): the launcher's
        // setup panel over the game window, so only with --renderer window. Its setup tool records where it put the disc in
        // $FM4_RECOMP_HOME/locations.conf (the data side's contract), which the host reads after it.
        if (std::strcmp(rendererMode, "window"))
            Stop(StopCode::Setup, "--disc-setup needs --renderer window (its panel opens over the game window)");
        static const std::unique_ptr<runtime::disc::Host> discHost = runtime::disc::MakeSdlHost(discSetupTool);
        static runtime::disc::SwapService discService(*discHost);
        runtime::disc::SetService(&discService);
    }
    runtime::MountUserData(userDataPath);  // saved content, writable and isolated (kernel/content.h)  // read-only: game:\ and d:\ (kernel/file_system.h)
    const auto image = ReadChecked(imagePath, fm4::kImageSize, fm4::kImageCrc32, true);
    const auto header = ReadChecked(xexPath, fm4::kHeaderSize, fm4::kHeaderCrc32, false);

    base = runtime::ReserveGuestMemory();
    video.guestBase = base;  // the EDRAM unit's resolve writes (video/renderer.h Options::guestBase)
    runtime::InitializeHeaps(base);  // the guest's virtual and physical heaps (kernel/virtual_memory.h)
    runtime::gpu::InitializeRegisters(base);  // the GPU register page (kernel/graphics.h)
    runtime::CommitGuestMemory(base, fm4::kImageBase, fm4::kImageSize);
    std::memcpy(base + fm4::kImageBase, image.data(), image.size());

    // One host pointer per guest code word after the image (PPC_LOOKUP_FUNC); null = no function.
    runtime::CommitGuestMemory(base, PPC_IMAGE_BASE + PPC_IMAGE_SIZE, PPC_CODE_SIZE * 2);
    size_t functions = 0;
    for (const PPCFuncMapping* m = PPCFuncMappings; m->host != nullptr; m++, functions++)
        PPC_LOOKUP_FUNC(base, m->guest) = m->host;
    runtime::RegisterCode(PPC_CODE_BASE, PPC_CODE_SIZE, PPC_IMAGE_BASE, PPC_IMAGE_SIZE);  // modules map around it

    // The executable as the kernel sees it: its header copy and loader entry (kernel/module.h).
    const runtime::Module module = runtime::CreateModule(base, header, fm4::kImageBase, fm4::kEntry);
    runtime::SetExecutableModule(module);

    // Import slots: a function's slot holds its thunk, so a pointer to it reaches the strict stub
    // (or the runtime's implementation, GUEST_IMPORT). A data import with a documented value
    // (kernel/module.cpp) points to a kernel block holding it; any other points to its own
    // unmapped guard page, which names it on first access.
    const size_t page = size_t(getpagesize());
    size_t documented = 0;
    for (const auto& import : fm4::kImports)
    {
        if (import.kind == fm4::ImportKind::FunctionSlot)
            runtime::StoreBigEndian32(base, import.address, import.thunk);
        else if (import.kind == fm4::ImportKind::Variable)
        {
            const char* name = import.name + 7;  // without "__imp__"
            uint32_t value;
            if (const runtime::DataExport* e = runtime::FindDataExport(name))
            {
                value = runtime::ArenaAlloc(base, e->size);
                e->init(base, value, module);
                documented++;
            }
            else
            {
                value = runtime::GuardAddress(dataImports.size(), page);
                dataImports.push_back(import.name);
            }
            runtime::StoreBigEndian32(base, import.address, value);
            runtime::RecordDataImport(name, value);
        }
    }
    if (!runtime::GuardsFit(dataImports.size(), page))
        Stop(StopCode::Setup, "%zu data-import guards do not fit below 80000000", dataImports.size());

    // Code pages and the filled function table are read-only from here on. (Guest reads of the
    // table, which upstream's PPC_LOOKUP_FUNC places in guest memory, still succeed: a known gap.)
    for (uint64_t at = fm4::kImageBase; at < uint64_t(fm4::kImageBase) + fm4::kImageSize; at += page)
        if (!runtime::PageWritable(uint32_t(at), page, fm4::kSections, std::size(fm4::kSections)))
            runtime::ProtectGuestMemoryReadOnly(base, at, page);
    const uint64_t table = PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
    runtime::ProtectGuestMemoryReadOnly(base, table, ((table + PPC_CODE_SIZE * 2 + page - 1) & ~uint64_t(page - 1)) - table);
    // The title process and its main thread: the entry runs on thread 1 with the executable's
    // default stack size (Xenia XThread for the main thread; ForzaRecomp main.cpp).
    const uint32_t process = runtime::CreateProcessBlock(base, fm4::kTls);
    mainThread = runtime::CreateThreadState(base, fm4::kTls, process, fm4::kDefaultStackSize, 1, fm4::kEntry, 0, 0);
    runtime::SetTitleProcess(process, fm4::kTls, fm4::kDefaultStackSize);  // threads the title creates (kernel/threads.h)
    std::fprintf(stderr, "[runtime] %zu functions, %zu imports (%zu implemented), %zu data imports documented, %zu guarded, "
                 "%d title hooks\n", functions, fm4::kImportCount, runtime::ImplementedImportCount(), documented,
                 dataImports.size(), fm4::TitleHookCount());
    fm4::ReportTitleHooks();
    std::fprintf(stderr, "[runtime] main thread: PCR %08X, KTHREAD %08X, TLS %08X, stack %08X-%08X\n",
                 mainThread.pcr, mainThread.kthread, mainThread.tls, mainThread.stackLimit, mainThread.stackBase);

    runtime::InstallGuestFaultHandler(base, DescribeGuest);
    std::thread([deadline] {
        std::this_thread::sleep_for(std::chrono::seconds(deadline));
        ReportGuestContexts();
        runtime::gpu::ReportCommandProcessor();
        runtime::audio::Report("deadline");
        runtime::xma::Report("deadline");
        Stop(StopCode::Deadline, "no stop within %d s", deadline);
    }).detach();

    // The renderer starts under the deadline (a hang in device creation is bounded in-process too), before
    // the title can start the ring.
    // Window mode: SDL video and the window on this, the main thread (macOS: AppKit windows and events belong to the main
    // thread), before the renderer creates the instance and the surface (also here, in Initialize).
    if (video.mode == runtime::video::Mode::Window)
        video.surface = runtime::video::window::OpenWindow();
    // FM4_DEFER_FENCES (renderer.h DeferFences): the store and idle hooks only when on (the command processor then skips the
    // sync before a store-only packet, which is right only when that store is deferred).
    static const runtime::gpu::RendererHooks hooks{runtime::video::Present, runtime::video::Draw, runtime::video::LoadShader,
                                                   runtime::video::SyncGuestWrites,
                                                   runtime::video::DeferFences() ? runtime::video::DeferStore : nullptr,
                                                   runtime::video::DeferFences() ? runtime::video::RingIdle : nullptr,
                                                   runtime::video::DeferFences() ? runtime::video::PendingAt : nullptr,
                                                   runtime::video::DeferFences() ? runtime::video::WorkOpen : nullptr,
                                                   // FM4 Recomp (M7, FM4_GUEST_MEMORY_GPU=1): the GPU read guard's hooks only when asked
                                                   runtime::video::GuestMemoryGpuAsked() ? runtime::video::HonourGpuReadGuard : nullptr,
                                                   runtime::video::GuestMemoryGpuAsked() ? runtime::video::BeforeCpWrite : nullptr};
    if (runtime::video::Initialize(video))
    {
        if (video.mode == runtime::video::Mode::SelfCheck)
            runtime::video::RunSelfCheck();  // never returns: stops 12 when every check passed
        runtime::gpu::SetRendererHooks(&hooks);
        if (runtime::video::AsyncRetireOn())  // FM4 Recomp (FM4_ASYNC_RETIRE): nothing is called between packets otherwise
            runtime::gpu::SetReapHook(runtime::video::ReapRetired);
    }

    // The translated code nests host calls deeply: run it on a large host stack.
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, size_t(512) << 20);
    pthread_t thread;
    if (pthread_create(&thread, &attributes, RunGuest, nullptr) != 0)
        Stop(StopCode::Setup, "cannot start the guest thread");
    // Window mode: this thread runs the window's events and presents the frames the command processor hands over. It
    // never returns; the run still ends only by a stop (the window closed: 14; the deadline thread: 124; any other stop
    // on its own thread), each of which ends the process with _exit without waiting for this loop.
    if (video.mode == runtime::video::Mode::Window)
        runtime::video::window::RunWindowLoop();
    pthread_join(thread, nullptr);
    return 0;
}
