// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// See shader_cache.h. The glslang calls mirror glslangValidator -V (StandAlone/StandAlone.cpp of
// glslang 16.6.0: setEnvInput(EShSourceGlsl, stage, EShClientVulkan, 100), setEnvClient(EShClientVulkan,
// EShTargetVulkan_1_0), setEnvTarget(EShTargetSpv, EShTargetSpv_1_0), parse(GetDefaultResources(), 100,
// false, EShMsgSpvRules | EShMsgVulkanRules), link, GlslangToSpv with default SpvOptions), which is
// what tests/shader_census/census.cpp runs; the output SPIR-V is therefore the census's. The
// SPIR-V's own validation (spirv-val in the census) happens at vkCreateShaderModule under the
// pinned validation layer, which stops the run at its first message (vulkan_device.cpp).
#include "shader_cache.h"
#include "diag_compile.h"  // FM4 Recomp: compile time beside a long swap (diagnostic)
#include "spirv_disk_cache.h"  // FM4 Recomp (M7 stability): FM4_SPIRV_CACHE
#include "render_scale.h"  // FM4 Recomp (M9): ScaleFragCoordReads
#include <algorithm>
#include <glslang/build_info.h>
#include <filesystem>
#include "kernel/shader_hash.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <glslang/SPIRV/Logger.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace runtime::video {

namespace {

std::string Hex(uint64_t v)
{
    char s[17];
    std::snprintf(s, sizeof s, "%016llx", static_cast<unsigned long long>(v));
    return s;
}

std::string Name(const CachedShader& s) { return std::string(s.pixel ? "ps_" : "vs_") + Hex(s.hash); }

// FM4 Recomp (opt-in FM4_SHADER_NO_PRECISE=1, a GPU-cost experiment): the translated GLSL is compiled without its `precise`
// qualifiers (shader_glsl.cpp: products and sums are precise so the host cannot fuse them, as Xenos does not). SPIRV-Cross emits
// NoContraction arithmetic for Metal as calls to [[clang::optnone]] helpers (spvFAdd, spvFMul, spvFSub: fma(1, a, b) and the
// like), which the Metal compiler cannot inline or schedule; without it a multiply-add may be fused (results differ by rounding
// only). FM4_SHADER_DUMP still writes the translated text.
bool NoPrecise()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_SHADER_NO_PRECISE");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[shader] FM4_SHADER_NO_PRECISE=1: shaders compiled without their precise qualifiers (multiply-adds may fuse)\n");
        return enabled;
    }();
    return on;
}

std::string WithoutPrecise(const std::string& glsl)
{
    std::string out;
    out.reserve(glsl.size());
    for (size_t i = 0; i < glsl.size();)
        if (glsl.compare(i, 8, "precise ") == 0 && (i == 0 || !(std::isalnum((unsigned char)glsl[i - 1]) || glsl[i - 1] == '_')))
            i += 8;
        else
            out += glsl[i++];
    return out;
}

// FM4 Recomp (diagnostics, never in a kept stack; the WithoutPrecise pattern): text rewrites of the translated GLSL that answer
// where the vertex channel's GPU time goes. Each is opt-in by its own variable and
// logged once; a run with either is a measurement, not a candidate.
//  FM4_DIAG_VS_NO_ATOMICS=1: vertex shaders lose their `atomicOr(xe_error, ...)` stores (a fetch outside its slot no longer stops
//    the run: NOT shippable) to measure what a vertex function with device writes costs.
//  FM4_DIAG_FAST_MUL=1: xe_mul / xe_dp4 / xe_dp3 / xe_dp2add become plain products and dot() (breaks the D3D9 0 * x = 0 rule the
//    helpers implement: a semantics change not approved) to measure the ALU share of that rule.
bool DiagVsNoAtomics()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DIAG_VS_NO_ATOMICS");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[shader] FM4_DIAG_VS_NO_ATOMICS=1 (diagnostic): vertex shaders without their xe_error stores; a bad fetch no longer stops the run\n");
        return enabled;
    }();
    return on;
}
bool DiagFastMul()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DIAG_FAST_MUL");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[shader] FM4_DIAG_FAST_MUL=1 (diagnostic): xe_mul/xe_dp* as plain products (the D3D9 zero rule is off)\n");
        return enabled;
    }();
    return on;
}
std::string ReplaceAll(std::string text, const std::string& from, const std::string& to, size_t& count)
{
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to), count++;
    return text;
}
std::string DiagRewrite(std::string glsl, bool pixel)
{
    size_t n = 0;
    if (DiagVsNoAtomics() && !pixel)
        glsl = ReplaceAll(std::move(glsl), "atomicOr(xe_error, 1u);", "", n), glsl = ReplaceAll(std::move(glsl), "atomicOr(xe_error, 2u);", "", n);
    if (DiagFastMul())
    {
        glsl = ReplaceAll(std::move(glsl), "float xe_mul(float a, float b)\n{\n    precise float p = a * b;\n    return (a == 0.0 || b == 0.0) ? 0.0 : p;\n}",
                          "float xe_mul(float a, float b) { return a * b; }", n);
        glsl = ReplaceAll(std::move(glsl), "vec4 xe_mul(vec4 a, vec4 b)\n{\n    return vec4(xe_mul(a.x, b.x), xe_mul(a.y, b.y), xe_mul(a.z, b.z), xe_mul(a.w, b.w));\n}",
                          "vec4 xe_mul(vec4 a, vec4 b) { return a * b; }", n);
        glsl = ReplaceAll(std::move(glsl), "float xe_dp2add(vec4 a, vec4 b, vec4 c)\n{\n    precise float s = xe_mul(a.x, b.x) + xe_mul(a.y, b.y);\n    precise float r = s + c.x;\n    return r;\n}",
                          "float xe_dp2add(vec4 a, vec4 b, vec4 c) { return dot(a.xy, b.xy) + c.x; }", n);
        glsl = ReplaceAll(std::move(glsl), "float xe_dp4(vec4 a, vec4 b)\n{\n    precise float s = xe_mul(a.x, b.x) + xe_mul(a.y, b.y);\n    precise float t = s + xe_mul(a.z, b.z);\n    precise float r = t + xe_mul(a.w, b.w);\n    return r;\n}",
                          "float xe_dp4(vec4 a, vec4 b) { return dot(a, b); }", n);
        glsl = ReplaceAll(std::move(glsl), "float xe_dp3(vec4 a, vec4 b)\n{\n    precise float s = xe_mul(a.x, b.x) + xe_mul(a.y, b.y);\n    precise float r = s + xe_mul(a.z, b.z);\n    return r;\n}",
                          "float xe_dp3(vec4 a, vec4 b) { return dot(a.xyz, b.xyz); }", n);
    }
    return glsl;
}

}  // namespace

// ---- GlslangCompiler ---------------------------------------------------------------------------

GlslangCompiler::GlslangCompiler() : initialized_(glslang::InitializeProcess()) {}

GlslangCompiler::~GlslangCompiler()
{
    if (initialized_)
        glslang::FinalizeProcess();
}

// FM4 Recomp (M7 stability, opt-in FM4_SHADER_CACHE=<directory>; spirv_disk_cache.h): <directory>/spirv, created at the first use; empty
// when off or when it cannot be created (then every variant is compiled, as before). The pipeline cache is <directory>/pipelines.bin.
const std::string& SpirvCacheDir()
{
    static const std::string dir = [] {
        const char* v = std::getenv("FM4_SHADER_CACHE");
        if (v == nullptr || *v == '\0')
            return std::string();
        const std::string spirv = std::string(v) + "/spirv";
        std::error_code ec;
        std::filesystem::create_directories(spirv, ec);
        if (ec)
        {
            std::fprintf(stderr, "[shader] FM4_SHADER_CACHE: cannot create %s (%s): the SPIR-V disk cache is off\n", spirv.c_str(), ec.message().c_str());
            return std::string();
        }
        std::fprintf(stderr, "[shader] SPIR-V disk cache on (FM4_SHADER_CACHE): compiled variants are kept in %s and read back across sessions\n", spirv.c_str());
        return spirv;
    }();
    return dir;
}

bool GlslangCompiler::Compile(bool pixel, const std::string& glsl, std::vector<uint32_t>& spirv, std::string& warnings, std::string& error)
{
    const uint64_t start = diag::CompileClockNs();  // diag_compile.h (counted always; printed beside a long swap)
    std::string cachePath;
    if (const std::string& dir = SpirvCacheDir(); !dir.empty())
    {
        char options[64];  // the options that change the compiled text (CompileStage's rewrites)
        std::snprintf(options, sizeof options, "noprecise %d vs_no_atomics %d fast_mul %d", int(NoPrecise()), int(DiagVsNoAtomics()), int(DiagFastMul()));
        char compiler[48];
        std::snprintf(compiler, sizeof compiler, "glslang %d.%d.%d", GLSLANG_VERSION_MAJOR, GLSLANG_VERSION_MINOR, GLSLANG_VERSION_PATCH);
        cachePath = dir + "/" + spirv_cache::Key(compiler, pixel ? 'f' : 'v', options, glsl);
        if (spirv_cache::Load(cachePath, spirv))
        {
            warnings.clear();
            diag::Compiles().glslNs.fetch_add(diag::CompileClockNs() - start, std::memory_order_relaxed);
            diag::Compiles().glslCached.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    const bool ok = CompileStage(pixel ? EShLangFragment : EShLangVertex, pixel, true, glsl, spirv, warnings, error);
    if (ok && !cachePath.empty())
        spirv_cache::Store(cachePath, spirv);  // a failed store only means the next session compiles it again
    diag::Compiles().glslNs.fetch_add(diag::CompileClockNs() - start, std::memory_order_relaxed);
    diag::Compiles().glsl.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

bool GlslangCompiler::CompileCompute(const std::string& glsl, std::vector<uint32_t>& spirv, std::string& warnings, std::string& error)
{
    return CompileStage(EShLangCompute, false, false, glsl, spirv, warnings, error);
}

bool GlslangCompiler::CompileStage(int stageIndex, bool pixel, bool rewrites, const std::string& glsl, std::vector<uint32_t>& spirv, std::string& warnings,
                                   std::string& error)
{
    spirv.clear();
    warnings.clear();
    if (!initialized_)
        return error = "glslang::InitializeProcess failed", false;
    const EShLanguage stage = EShLanguage(stageIndex);
    const EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
    glslang::TShader shader(stage);
    const bool rewrite = rewrites && (NoPrecise() || DiagVsNoAtomics() || DiagFastMul());  // FM4_SHADER_NO_PRECISE; the diagnostics above
    const std::string text = rewrite ? DiagRewrite(NoPrecise() ? WithoutPrecise(glsl) : glsl, pixel) : std::string();
    const char* source = rewrite ? text.c_str() : glsl.c_str();
    shader.setStrings(&source, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    if (!shader.parse(GetDefaultResources(), 100, false, messages))
        return error = std::string("glslang parse: ") + shader.getInfoLog(), false;
    warnings = shader.getInfoLog();
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages))
        return error = std::string("glslang link: ") + program.getInfoLog(), false;
    if (const char* log = program.getInfoLog(); log != nullptr && *log != '\0')
        warnings += log;
    spv::SpvBuildLogger logger;
    glslang::SpvOptions options;  // the defaults: no optimizer, no debug info, as glslangValidator -V without -Od/-Os/-g
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &logger, &options);
    if (const std::string log = logger.getAllMessages(); !log.empty())
        return error = "glslang SPIR-V generation: " + log, false;  // strict: a generation message is a failure
    if (spirv.size() < 5 || spirv[0] != 0x07230203u)
        return error = "glslang produced no SPIR-V module", spirv.clear(), false;
    return true;
}

// ---- Variants --------------------------------------------------------------------------------

std::string VariantGlsl(const std::string& glsl, const ShaderVariantDefines& defines)
{
    if (defines.lines.empty())
        return glsl;
    std::string text = glsl;
    const size_t eol = text.find('\n');
    text.insert(eol == std::string::npos ? text.size() : eol + 1, defines.Joined());
    // FM4 Recomp (M9, FM4_INTERNAL_RES): render_scale.h ScaleFragCoordReads for an XE_HALVES variant (never at 1x).
    if (std::find_if(defines.lines.begin(), defines.lines.end(), [](const std::string& l) { return l.rfind("XE_HALVES ", 0) == 0; }) != defines.lines.end())
        runtime::video::scale::ScaleFragCoordReads(text);
    return text;
}

// ---- ShaderCache -------------------------------------------------------------------------------

ShaderCache::ShaderCache(SpirvCompiler& compiler, const char* dumpDir, size_t maxVariants, bool memo)
    : compiler_(compiler), dumpDir_(dumpDir == nullptr ? "" : dumpDir), maxVariants_(maxVariants), memo_(memo)
{
}

bool ShaderCache::Dump(const std::string& name, const std::string& text)
{
    if (dumpDir_.empty() || dumped_ >= kDumpCap)
        return false;
    const uint64_t start = diag::CompileClockNs();  // diag_compile.h
    FILE* f = std::fopen((dumpDir_ + "/" + name).c_str(), "wb");
    if (f == nullptr)
        return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    dumped_++;
    diag::Compiles().dumpNs.fetch_add(diag::CompileClockNs() - start, std::memory_order_relaxed);
    diag::Compiles().dumps.fetch_add(1, std::memory_order_relaxed);
    return ok && closed;
}

const CachedShader* ShaderCache::Find(uint64_t hash) const
{
    const auto it = shaders_.find(hash);
    return it == shaders_.end() ? nullptr : it->second.get();
}

const CachedShader* ShaderCache::Load(uint32_t type, const uint32_t* words, uint32_t count, std::string& reason)
{
    stats_.loads++;
    if (type > 1)
        return reason = "IM_LOAD type " + std::to_string(type) + " is neither vertex (0) nor pixel (1)", nullptr;
    if (count == 0 || count % 3 != 0)
        return reason = std::string(type ? "ps" : "vs") + " of " + std::to_string(count) + " words is not whole 3-word slots", nullptr;
    const bool pixel = type == 1;
    // FM4 Recomp (M7 CPU cost): the memo (shader_cache.h `memo`): an exact comparison with every translated shader of this type and
    // size answers a re-load without the hash. A refused program is not memoised (it has no CachedShader) and takes the hash path
    // to its remembered reason; a candidate that differs is rejected at the first differing byte.
    const uint64_t sizeKey = uint64_t(type) << 32 | count;
    if (memo_)
        if (const auto bucket = bySize_.find(sizeKey); bucket != bySize_.end())
            for (const CachedShader* candidate : bucket->second)
                if (std::memcmp(candidate->microcode.data(), words, size_t(count) * 4) == 0)
                {
                    stats_.hits++, stats_.memoHits++;
                    current_[pixel] = candidate;
                    return candidate;
                }
    const uint64_t hash = runtime::gpu::ShaderHash(type, words, count);
    if (const CachedShader* hit = Find(hash))
    {
        stats_.hits++;
        current_[pixel] = hit;
        return hit;
    }
    if (const auto refused = refused_.find(hash); refused != refused_.end())
        return stats_.refusals++, reason = refused->second, nullptr;
    auto shader = std::make_unique<CachedShader>();
    shader->hash = hash;
    shader->pixel = pixel;
    shader->words = count;
    shader->cls = ScanShaderClass(words, count);
    std::string error;
    stats_.translations++;
    const uint64_t translateStart = diag::CompileClockNs();  // diag_compile.h
    const bool translated = runtime::shader::TranslateShader(words, count, pixel, shader->t, error);
    diag::Compiles().translateNs.fetch_add(diag::CompileClockNs() - translateStart, std::memory_order_relaxed);
    diag::Compiles().translations.fetch_add(1, std::memory_order_relaxed);
    if (!translated)
    {
        stats_.refusals++;
        reason = Name(*shader) + " (" + std::to_string(count) + " words, class " + ShaderClassName(shader->cls) + ") refused: " + error;
        refused_[hash] = reason;
        // FM4_SHADER_DUMP: a refused shader's microcode as the census reads it (<vs|ps>_<hash>.bin, big-endian words, cp_trace.cpp's
        // format), so the translator can be worked on offline without the CP trace.
        std::string be(size_t(count) * 4, '\0');
        for (uint32_t i = 0; i < count; i++)
            for (uint32_t b = 0; b < 4; b++)
                be[4 * i + b] = char(uint8_t(words[i] >> (24 - 8 * b)));
        Dump(Name(*shader) + ".bin", be);
        return nullptr;
    }
    Dump(Name(*shader) + (pixel ? ".frag" : ".vert"), shader->t.glsl);
    if (memo_)
        shader->microcode.assign(words, words + count);
    const CachedShader* result = shader.get();
    shaders_[hash] = std::move(shader);
    if (memo_)
        bySize_[sizeKey].push_back(result);
    current_[pixel] = result;
    return result;
}

ShaderVariant* ShaderCache::Variant(const CachedShader& shader, const ShaderVariantDefines& defines, std::string& reason)
{
    const uint64_t id = defines.Id();
    for (const auto& v : shader.variants)
        if (v->id == id)
            return v.get();
    if (const auto failed = failedVariants_.find(shader.hash ^ id); failed != failedVariants_.end())
        return reason = failed->second, nullptr;
    const std::string where = Name(shader) + " variant " + Hex(id) + (defines.lines.empty() ? " (no defines)" : ":\n" + defines.Joined());
    if (shader.variants.size() >= maxVariants_)
        return reason = where + "more than " + std::to_string(maxVariants_) + " variants of one shader (XE_ALPHA_REF or texture defines changing per draw?)", nullptr;
    auto variant = std::make_unique<ShaderVariant>();
    variant->id = id;
    variant->defines = defines.Joined();
    const std::string glsl = VariantGlsl(shader.t.glsl, defines);
    std::string warnings, error;
    stats_.compiles++;
    if (!compiler_.Compile(shader.pixel, glsl, variant->spirv, warnings, error))
    {
        stats_.compileFailures++;
        Dump(Name(shader) + "_" + Hex(id) + (shader.pixel ? ".frag" : ".vert"), glsl);  // the failing source stays for diagnosis
        reason = where + " failed to compile: " + error;
        failedVariants_[shader.hash ^ id] = reason;
        return nullptr;
    }
    if (!warnings.empty())
        std::fprintf(stderr, "[shader] %s: glslang: %s\n", where.c_str(), warnings.c_str());
    if (!defines.lines.empty())
        Dump(Name(shader) + "_" + Hex(id) + (shader.pixel ? ".frag" : ".vert"), glsl);
    ShaderVariant* result = variant.get();
    shaders_.at(shader.hash)->variants.push_back(std::move(variant));  // the cache owns the shader; callers hold const views
    return result;
}

}  // namespace runtime::video
