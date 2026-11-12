// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The shader cache (execution model §2 "Shaders", unit 8): microcode -> hash -> GLSL (our translator,
// runtime/shader/shader_glsl.h) -> SPIR-V (the glslang library, the census's options) -> the host's
// shader module. Two levels, because the two inputs arrive at different times:
//   1. Load (every IM_LOAD / IM_LOAD_IMMEDIATE): the hash (kernel/shader_hash.h: the census recipe,
//      so logs, dumps and census rows name one shader the same way) is a map key; a miss translates
//      and tags the class (shader_class.h); a hit costs the hash alone (or, with `memo`, a memcmp). The ~18 loads per frame are
//      mostly re-loads of the same hash, so this is cheap, and a refused shader stops at its load,
//      before any draw, naming the hash (the census does the same offline). Translation needs only
//      the microcode, so nothing of the draw state is guessed here.
//   2. Variant (at a draw, from pipeline_cache.cpp): the host's `#define`s after line 1 (shader_glsl.h:
//      XE_RECT_LIST, XE_TEXTURE_<slot>_GAMMA/_SCALE/_LOD_BIAS, XE_COLOR0_GAMMA/_7E3/_SCALE,
//      XE_ALPHA_TEST + XE_ALPHA_REF) depend on the draw's fetch constants and render target, so the
//      SPIR-V is compiled per (hash, defines) at the first draw that needs it and kept. The variant id
//      is part of the pipeline key (pipeline_key.h; Xenia vulkan_pipeline_cache.h:147-150
//      vertex/pixel_shader_modification serve the same purpose).
// Nothing here calls Stop(): every refusal is returned with its reason and the renderer stops with
// it (the CPU test exercises the paths). No Vulkan type: the host module handle is opaque here and
// is created by pipeline_cache.cpp. One thread (the command processor's) uses the cache.
#pragma once
#include "shader/shader_glsl.h"
#include "shader_class.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace runtime::video {

// The GLSL -> SPIR-V compiler behind the cache: GlslangCompiler (below) in the runtime, a fake in the
// test's error-path checks.
class SpirvCompiler
{
public:
    virtual ~SpirvCompiler() = default;
    // `pixel`: the fragment stage, else the vertex stage. False with `error` holding the compiler's
    // log on any failure; true with `spirv` the module's words (the magic 0x07230203 first) and
    // `warnings` whatever the compiler reported on success (the census, glslangValidator exit 0,
    // accepts those).
    virtual bool Compile(bool pixel, const std::string& glsl, std::vector<uint32_t>& spirv, std::string& warnings, std::string& error) = 0;
};

// The glslang library (Homebrew glslang 16.6.0), with exactly the census's options
// (tests/shader_census/census.cpp: `glslangValidator -V`): GLSL source for the Vulkan client, client
// Vulkan 1.0, target SPIR-V 1.0, the default resource limits, messages EShMsgSpvRules |
// EShMsgVulkanRules, default SpvOptions (no optimizer, no debug info). glslang::InitializeProcess
// once per process (the constructor; its failure makes every Compile fail), FinalizeProcess in the
// destructor.
class GlslangCompiler final : public SpirvCompiler
{
public:
    GlslangCompiler();
    ~GlslangCompiler() override;
    GlslangCompiler(const GlslangCompiler&) = delete;
    GlslangCompiler& operator=(const GlslangCompiler&) = delete;
    bool Compile(bool pixel, const std::string& glsl, std::vector<uint32_t>& spirv, std::string& warnings, std::string& error) override;
    // FM4 Recomp (M7, FM4_TEXTURE_GPU_UNTILE): a compute shader with the same options (texture_vk.cpp's untile pipeline); the
    // graphics-only rewrites (FM4_SHADER_NO_PRECISE and the diagnostics) never apply.
    bool CompileCompute(const std::string& glsl, std::vector<uint32_t>& spirv, std::string& warnings, std::string& error);

private:
    bool CompileStage(int stage, bool pixel, bool rewrites, const std::string& glsl, std::vector<uint32_t>& spirv, std::string& warnings,
                      std::string& error);
    bool initialized_ = false;
};

// The host's `#define` lines for one variant, without "#define " and without newlines, e.g.
// "XE_RECT_LIST", "XE_ALPHA_TEST 4", "XE_ALPHA_REF 0.5". Kept sorted and distinct so that equal
// sets have one id whatever order the caller added them in.
struct ShaderVariantDefines
{
    std::vector<std::string> lines;
    // Inline (header-only) so pipeline_key.cpp's test links without the glslang-backed shader_cache.cpp.
    void Add(std::string line)
    {
        const auto at = std::lower_bound(lines.begin(), lines.end(), line);
        if (at == lines.end() || *at != line)
            lines.insert(at, std::move(line));
    }
    // FNV-1a-64 over the lines joined by '\n' (0 for no defines). Documented so a log can be matched.
    uint64_t Id() const
    {
        if (lines.empty())
            return 0;
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < lines.size(); i++)
        {
            for (const char c : lines[i])
                h = (h ^ uint8_t(c)) * 0x100000001b3ull;
            if (i + 1 < lines.size())
                h = (h ^ uint8_t('\n')) * 0x100000001b3ull;
        }
        return h;
    }
    std::string Joined() const  // "#define A\n#define B\n" (empty for no defines)
    {
        std::string s;
        for (const std::string& line : lines)
            s += "#define " + line + "\n";
        return s;
    }
};

struct ShaderVariant
{
    uint64_t id = 0;
    std::string defines;          // ShaderVariantDefines::Joined()
    std::vector<uint32_t> spirv;  // host order
    uint64_t module = 0;          // the host's VkShaderModule, set by pipeline_cache.cpp (0: not created yet)
};

struct CachedShader
{
    uint64_t hash = 0;
    bool pixel = false;
    uint32_t words = 0;
    runtime::shader::ShaderTranslation t;  // the GLSL and the reflection (texture slots, fetch slots, bools, loops)
    ShaderClass cls = kClassStraight;
    std::vector<std::unique_ptr<ShaderVariant>> variants;  // stable addresses: pipelines keep pointers
    // FM4 Recomp (M7 CPU cost): the program's words as loaded (host order), kept only with the load memo (ShaderCache `memo`):
    // a later load of the same (type, size) is answered by comparing its words with these, without hashing them.
    std::vector<uint32_t> microcode;
};

struct ShaderCacheStats
{
    // `hits`: loads answered by a cached shader (the memo's and the hash map's); `memoHits`: those the memo answered (no hash).
    uint64_t loads = 0, hits = 0, memoHits = 0, translations = 0, refusals = 0, compiles = 0, compileFailures = 0;
};

class ShaderCache
{
public:
    // `dumpDir`: FM4_SHADER_DUMP (opt-in; null or empty: nothing is written). When set, each
    // translation writes <dir>/<vs|ps>_<hash>.<vert|frag> and each variant
    // <dir>/<vs|ps>_<hash>_<variant id>.<vert|frag>, capped at kDumpCap files, next to the CP's
    // <vs|ps>_<hash>.bin (kernel/cp_trace.cpp), so a run's evidence dir can feed the census.
    // `maxVariants`: variants per shader before Variant refuses (XE_ALPHA_REF bakes a float into the
    // source: a per-frame reference would otherwise grow without bound).
    // `memo` (FM4 Recomp, M7 CPU cost; opt-in FM4_SHADER_MEMO=1 in the renderer): Load first compares the words with the
    // translated shaders of the same type and size (CachedShader::microcode, memcmp) and answers an exact match without hashing.
    // Exact, so never less safe than the hash path: a match is byte-identical to the program it names; a miss hashes as before.
    // The race loads ~2,600 shaders of ~150 words a frame, re-loads of ~300 programs, and the hash's
    // serial multiply chain was 2.6 % of the command processor's thread.
    ShaderCache(SpirvCompiler& compiler, const char* dumpDir, size_t maxVariants = kDefaultMaxVariants, bool memo = false);

    static constexpr size_t kDefaultMaxVariants = 64, kDumpCap = 4096;

    // IM_LOAD(_IMMEDIATE), `type` 0 vertex / 1 pixel, `words` host order. A hit or a translation; the
    // result becomes the stage's current shader. Null with `reason` (naming the hash) when the type is
    // not 0 or 1, the size is not whole 3-word slots, or the translator refuses. A refused shader is
    // remembered so a re-load reports the same reason without translating again.
    const CachedShader* Load(uint32_t type, const uint32_t* words, uint32_t count, std::string& reason);
    // The last shader loaded for the stage (null before the first load).
    const CachedShader* Current(bool pixel) const { return current_[pixel]; }
    const CachedShader* Find(uint64_t hash) const;

    // The variant of `shader` for `defines`, compiled at the first request. Null with `reason` on a
    // compile failure (the compiler's log, the hash and the defines) or when `maxVariants` is
    // reached; a failed compile is remembered and refused again without recompiling.
    ShaderVariant* Variant(const CachedShader& shader, const ShaderVariantDefines& defines, std::string& reason);

    const ShaderCacheStats& Stats() const { return stats_; }
    size_t Size() const { return shaders_.size(); }
    bool Memo() const { return memo_; }

private:
    bool Dump(const std::string& name, const std::string& text);

    SpirvCompiler& compiler_;
    std::string dumpDir_;
    size_t dumped_ = 0;
    const size_t maxVariants_;
    const bool memo_;
    std::unordered_map<uint64_t, std::unique_ptr<CachedShader>> shaders_;  // by hash
    std::unordered_map<uint64_t, std::vector<const CachedShader*>> bySize_;  // memo: type << 32 | words -> the translated shaders of that size
    std::unordered_map<uint64_t, std::string> refused_;                    // hash -> the translator's reason
    std::unordered_map<uint64_t, std::string> failedVariants_;             // hash ^ variant id -> the compiler's reason
    const CachedShader* current_[2] = {nullptr, nullptr};
    ShaderCacheStats stats_;
};

// The GLSL of a variant: the defines inserted after line 1 (`#version 450`), as shader_glsl.h requires.
std::string VariantGlsl(const std::string& glsl, const ShaderVariantDefines& defines);

}  // namespace runtime::video
