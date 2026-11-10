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
#include "kernel/shader_hash.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <glslang/SPIRV/Logger.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>

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

}  // namespace

// ---- GlslangCompiler ---------------------------------------------------------------------------

GlslangCompiler::GlslangCompiler() : initialized_(glslang::InitializeProcess()) {}

GlslangCompiler::~GlslangCompiler()
{
    if (initialized_)
        glslang::FinalizeProcess();
}

bool GlslangCompiler::Compile(bool pixel, const std::string& glsl, std::vector<uint32_t>& spirv, std::string& warnings, std::string& error)
{
    spirv.clear();
    warnings.clear();
    if (!initialized_)
        return error = "glslang::InitializeProcess failed", false;
    const EShLanguage stage = pixel ? EShLangFragment : EShLangVertex;
    const EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
    glslang::TShader shader(stage);
    const std::string text = NoPrecise() ? WithoutPrecise(glsl) : std::string();  // FM4_SHADER_NO_PRECISE
    const char* source = NoPrecise() ? text.c_str() : glsl.c_str();
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
    return text;
}

// ---- ShaderCache -------------------------------------------------------------------------------

ShaderCache::ShaderCache(SpirvCompiler& compiler, const char* dumpDir, size_t maxVariants)
    : compiler_(compiler), dumpDir_(dumpDir == nullptr ? "" : dumpDir), maxVariants_(maxVariants)
{
}

bool ShaderCache::Dump(const std::string& name, const std::string& text)
{
    if (dumpDir_.empty() || dumped_ >= kDumpCap)
        return false;
    FILE* f = std::fopen((dumpDir_ + "/" + name).c_str(), "wb");
    if (f == nullptr)
        return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    dumped_++;
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
    const uint64_t hash = runtime::gpu::ShaderHash(type, words, count);
    const bool pixel = type == 1;
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
    if (!runtime::shader::TranslateShader(words, count, pixel, shader->t, error))
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
    const CachedShader* result = shader.get();
    shaders_[hash] = std::move(shader);
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
