// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M7 stability, opt-in FM4_SHADER_CACHE=<directory>: files in <directory>/spirv): the GLSL -> SPIR-V results of shader variants kept on disk, so a
// variant compiled in an earlier session is read instead of compiled again on the command processor (first-use compiles made up
// roughly half of the 250-1900 ms race stalls in a profiled run). One file per variant:
//   name: the SHA-1 (hex) of the key material: a format line, the glslang version, the stage, the compile-affecting options and the
//         GLSL text exactly as compiled (so any change to the translator's output, the options or glslang is a different file);
//   body: "FM4SPV1\0", the word count (uint32, host order), the SPIR-V words (host order), the SHA-1 of those words (20 bytes).
// A file is used only when every part checks out (magic, size, SPIR-V magic 0x07230203, checksum); anything else is a miss and
// the variant is compiled as before. Files are written to a temporary name and renamed into place (a crash leaves no torn file).
// The pure parts (key, encode, decode) are tested in tests/runtime/test_shader_pipeline.cpp.
#pragma once
#include <TinySHA1.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace runtime::video::spirv_cache {

constexpr char kMagic[8] = {'F', 'M', '4', 'S', 'P', 'V', '1', '\0'};
constexpr uint32_t kSpirvMagic = 0x07230203u;

inline std::string Sha1Hex(const void* data, size_t bytes)
{
    sha1::SHA1 s;
    s.processBytes(data, bytes);
    sha1::SHA1::digest32_t digest;
    s.finalize(digest);
    char hex[41];
    for (int i = 0; i < 5; i++)
        std::snprintf(hex + 8 * i, 9, "%08x", digest[i]);
    return std::string(hex, 40);
}

inline void Sha1Bytes(const void* data, size_t bytes, uint8_t out[20])
{
    sha1::SHA1 s;
    s.processBytes(data, bytes);
    s.finalize(out);
}

// The file name (without directory) for a compile of `glsl` for `stage` ('v', 'f', 'c') with `options` (the compile-affecting
// flags as text) under `compiler` (e.g. "glslang 16.6.0").
inline std::string Key(const std::string& compiler, char stage, const std::string& options, const std::string& glsl)
{
    const std::string material = std::string("fm4 spirv cache 1\n") + compiler + "\n" + stage + "\n" + options + "\n" + glsl;
    return Sha1Hex(material.data(), material.size()) + ".spv";
}

inline std::vector<uint8_t> Encode(const std::vector<uint32_t>& spirv)
{
    std::vector<uint8_t> out(sizeof kMagic + 4 + spirv.size() * 4 + 20);
    std::memcpy(out.data(), kMagic, sizeof kMagic);
    const uint32_t words = uint32_t(spirv.size());
    std::memcpy(out.data() + sizeof kMagic, &words, 4);
    std::memcpy(out.data() + sizeof kMagic + 4, spirv.data(), spirv.size() * 4);
    Sha1Bytes(spirv.data(), spirv.size() * 4, out.data() + sizeof kMagic + 4 + spirv.size() * 4);
    return out;
}

// False (and `spirv` empty) unless `file` is a complete, intact entry holding a SPIR-V module.
inline bool Decode(const std::vector<uint8_t>& file, std::vector<uint32_t>& spirv)
{
    spirv.clear();
    if (file.size() < sizeof kMagic + 4 + 20 || std::memcmp(file.data(), kMagic, sizeof kMagic) != 0)
        return false;
    uint32_t words = 0;
    std::memcpy(&words, file.data() + sizeof kMagic, 4);
    if (words < 5 || file.size() != sizeof kMagic + 4 + uint64_t(words) * 4 + 20)
        return false;
    const uint8_t* body = file.data() + sizeof kMagic + 4;
    uint8_t digest[20];
    Sha1Bytes(body, size_t(words) * 4, digest);
    if (std::memcmp(digest, body + size_t(words) * 4, 20) != 0)
        return false;
    spirv.resize(words);
    std::memcpy(spirv.data(), body, size_t(words) * 4);
    if (spirv[0] != kSpirvMagic)
        return spirv.clear(), false;
    return true;
}

inline bool Load(const std::string& path, std::vector<uint32_t>& spirv)
{
    spirv.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;
    std::vector<uint8_t> file;
    uint8_t chunk[65536];
    for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;)
        file.insert(file.end(), chunk, chunk + n);
    std::fclose(f);
    return Decode(file, spirv);
}

// Writes the entry under a temporary name and renames it into place; false (nothing left behind) on any failure.
inline bool Store(const std::string& path, const std::vector<uint32_t>& spirv)
{
    const std::vector<uint8_t> bytes = Encode(spirv);
    const std::string temporary = path + ".tmp" + std::to_string(getpid());
    FILE* f = std::fopen(temporary.c_str(), "wb");
    if (f == nullptr)
        return false;
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (std::fclose(f) != 0 || !written || std::rename(temporary.c_str(), path.c_str()) != 0)
        return std::remove(temporary.c_str()), false;
    return true;
}

}  // namespace runtime::video::spirv_cache
