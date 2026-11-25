/**
 * @file        tests/unit/graphics/fm4_scaled_resolve_test.cpp
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/vulkan/scaled_resolve_banks.h>

#include "test_memory.h"

namespace {

using namespace rex::graphics;

// A copy of texture_cache_test.cpp's harness: that file is upstream's.
class TestSharedMemory final : public SharedMemory {
 public:
  TestSharedMemory() : SharedMemory(rex::testing::GetTestMemory()) { InitializeCommon(); }

 private:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& ranges) override {
    for (const auto& [first, count] : ranges) {
      MakeRangeValid(first << page_size_log2(), count << page_size_log2(), false);
    }
    return true;
  }
};

class TestTextureCache final : public TextureCache {
 public:
  TestTextureCache(const RegisterFile& regs, SharedMemory& memory)
      : TextureCache(regs, memory, 2, 2) {}

  const Texture* bound(uint32_t index = 0) const {
    const auto* binding = GetValidTextureBinding(index);
    return binding ? binding->texture : nullptr;
  }

  bool EnsureScaledResolveMemoryCommitted(uint32_t, uint32_t, uint32_t) override {
    return true;
  }

 private:
  class TestTexture final : public Texture {
   public:
    TestTexture(TextureCache& cache, TextureKey key) : Texture(cache, key, false) {}
  };

  uint32_t GetHostFormatSwizzle(TextureKey) const override {
    return xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  }
  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension) const override { return 16384; }
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension) const override { return 2048; }
  bool IsScaledResolveSupportedForFormat(TextureKey) const override { return true; }
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override {
    return std::make_unique<TestTexture>(*this, key);
  }
  bool LoadTextureDataFromResidentMemoryImpl(Texture&, bool, bool) override { return true; }
};

constexpr uint32_t kBase = 0x300000;
constexpr uint32_t kMips = 0x310000;

// A 64x64 tiled k_8_8_8_8 with packed mips.
void SetMippedFetch(RegisterFile& regs) {
  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.type = xenos::FetchConstantType::kTexture;
  fetch.dimension = xenos::DataDimension::k2DOrStacked;
  fetch.format = xenos::TextureFormat::k_8_8_8_8;
  fetch.tiled = 1;
  fetch.packed_mips = 1;
  fetch.pitch = 2;
  fetch.size_2d.width = 63;
  fetch.size_2d.height = 63;
  fetch.base_address = kBase >> 12;
  fetch.mip_address = kMips >> 12;
  fetch.mip_max_level = 6;
  std::memcpy(&regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], &fetch, sizeof(fetch));
}

using Runs = std::vector<std::pair<uint32_t, uint32_t>>;

}  // namespace

TEST_CASE("a CPU write releases only the scaled pages it covers",
          "[graphics][texture_cache][scaled]") {
  RegisterFile regs;
  TestSharedMemory memory;
  TestTextureCache cache(regs, memory);
  cache.MarkRangeAsResolved(0x100000, 0x10000);
  memory.MemoryInvalidationCallback(0x104000, 0x2000, true);
  CHECK(cache.GetScaledResolvedPageRuns() == Runs{{0x100000, 0x4000}, {0x106000, 0xA000}});
}

TEST_CASE("scaled runs split where the bytes per texel change",
          "[graphics][texture_cache][scaled]") {
  RegisterFile regs;
  TestSharedMemory memory;
  TestTextureCache cache(regs, memory);
  cache.MarkRangeAsResolved(0x200000, 0x2000, 2);
  cache.MarkRangeAsResolved(0x202000, 0x2000, 2);
  cache.MarkRangeAsResolved(0x204000, 0x1000, 3);
  cache.MarkRangeAsResolved(0x205000, 0x1000);
  const auto runs = cache.GetScaledResolveRuns();
  REQUIRE(runs.size() == 2);
  CHECK(runs[0].start == 0x200000);
  CHECK(runs[0].length == 0x4000);
  CHECK(runs[0].bpb_log2 == 2);
  CHECK(runs[1].start == 0x204000);
  CHECK(runs[1].length == 0x1000);
  CHECK(runs[1].bpb_log2 == 3);
  CHECK(cache.GetScaledResolvedPageRuns() == Runs{{0x200000, 0x6000}});
}

TEST_CASE("an uncompressed texture's base decides over a stale mip page",
          "[graphics][texture_cache][scaled]") {
  RegisterFile regs;
  TestSharedMemory memory;
  TestTextureCache cache(regs, memory);
  SetMippedFetch(regs);
  cache.RequestTextures(1);
  const auto* cpu = cache.bound();
  REQUIRE(cpu);
  REQUIRE(cpu->GetGuestMipsSize());
  const uint32_t base_size = cpu->GetGuestBaseSize();

  cache.MarkRangeAsResolved(kBase, base_size);
  cache.MarkRangeAsResolved(kMips, cpu->GetGuestMipsSize());
  cache.RequestTextures(1);
  REQUIRE(cache.bound());
  CHECK(cache.bound()->key().scaled_resolve);

  memory.MemoryInvalidationCallback(kBase, base_size, true);
  cache.RequestTextures(1);
  REQUIRE(cache.bound());
  CHECK_FALSE(cache.bound()->key().scaled_resolve);
}

TEST_CASE("a texture with its base resolved stays scaled without its mips",
          "[graphics][texture_cache][scaled]") {
  RegisterFile regs;
  TestSharedMemory memory;
  TestTextureCache cache(regs, memory);
  SetMippedFetch(regs);
  cache.RequestTextures(1);
  const auto* cpu = cache.bound();
  REQUIRE(cpu);
  REQUIRE(cpu->GetGuestMipsSize());

  cache.MarkRangeAsResolved(kBase, cpu->GetGuestBaseSize());
  cache.RequestTextures(1);
  REQUIRE(cache.bound());
  CHECK(cache.bound()->key().scaled_resolve);
}

TEST_CASE("compact resolve bank assignments and dense promotion offsets", "[graphics][scaled]") {
  using rex::graphics::vulkan::ScaledResolveBanks;
  constexpr uint64_t bank = ScaledResolveBanks::kSize;
  {
    ScaledResolveBanks map;
    uint64_t address = 0;
    REQUIRE(map.Reserve(9 * bank, 1, 4));
    REQUIRE(map.Reserve(9 * bank - 256, bank + 512, 4));
    REQUIRE(map.Translate(9 * bank - 256, bank + 512, address));
    REQUIRE(address == bank - 256);
    REQUIRE(map.slot(8) == 0);
    REQUIRE(map.slot(9) == 1);
    REQUIRE(map.slot(10) == 2);
  }
  {
    ScaledResolveBanks map;
    uint64_t local = 0;
    REQUIRE(map.Reserve(0, 1, 3));
    REQUIRE(map.Reserve(6 * bank, 1, 3));
    REQUIRE(!map.Reserve(0, bank + 1, 3));
    REQUIRE(!map.Translate(0, bank + 1, local));
    REQUIRE(map.slot(1) == -1);
    REQUIRE(map.Translate(6 * bank + 3, 4, local));
    REQUIRE(local == bank + 3);
    REQUIRE(map.Reserve(31 * bank, bank, 3));
    REQUIRE(!map.Reserve(12 * bank, 1, 3));
    REQUIRE(!map.Reserve(31 * bank, bank + 1, 32));
    REQUIRE(!map.Reserve(0, 1, 33));
  }
  {
    ScaledResolveBanks map;
    uint64_t local;
    REQUIRE(map.Reserve(4 * bank, 3 * bank, 12));
    REQUIRE(map.Reserve(26 * bank, 3 * bank, 12));
    REQUIRE(map.Translate(26 * bank, 3 * bank, local));
    for (uint32_t logical = 0; logical < ScaledResolveBanks::kCount; ++logical) {
      const int slot = map.slot(logical);
      if (slot < 0) continue;
      REQUIRE(map.Translate(logical * bank + 64, 4, local));
      REQUIRE(local == uint64_t(slot) * bank + 64);
      // Copying this physical bank to its canonical destination preserves the byte.
      REQUIRE(local - uint64_t(slot) * bank + logical * bank == logical * bank + 64);
    }
  }
}

TEST_CASE("4K resolves retain 64-bit guest-to-physical offsets and promote all live banks",
          "[graphics][scaled]") {
  using rex::graphics::vulkan::ScaledResolveBanks;
  constexpr uint64_t bank = ScaledResolveBanks::BankSize(3);
  constexpr uint64_t space = bank * ScaledResolveBanks::kCount;
  constexpr uint64_t four_gib = uint64_t{1} << 32;
  STATIC_REQUIRE(bank == uint64_t{144} * 1024 * 1024);
  STATIC_REQUIRE(space == uint64_t{4608} * 1024 * 1024);
  STATIC_REQUIRE(ScaledResolveBanks::BankSize(2) == ScaledResolveBanks::kSize);
  ScaledResolveBanks map;
  uint64_t local = 0;
  REQUIRE(map.Reserve(four_gib - 16, 64, 12, bank));
  REQUIRE(map.Translate(four_gib + 8, 1, local, bank));
  REQUIRE(local == uint64_t(map.slot(28)) * bank + (four_gib + 8) % bank);
  REQUIRE(map.Reserve(space - bank, bank, 12, bank));
  REQUIRE(map.Translate(space - 1, 1, local, bank));
  REQUIRE(local == uint64_t(map.slot(31) + 1) * bank - 1);
  REQUIRE_FALSE(map.Reserve(space - 1, 2, 12, bank));
  REQUIRE_FALSE(map.Translate(space, 1, local, bank));
  REQUIRE_FALSE(map.Translate(UINT64_MAX - 4, 16, local, bank));
  // Occupy all twelve compact banks, then require the next range to use dense storage.
  for (uint32_t i = 0; i < 10; ++i) REQUIRE(map.Reserve(i * bank, 4, 12, bank));
  REQUIRE_FALSE(map.Reserve(uint64_t{14} * bank, 4, 12, bank));
  // Verify the byte each whole-bank promotion copy places at its canonical address.
  for (uint32_t i = 0; i < ScaledResolveBanks::kCount; ++i) {
    const int slot = map.slot(i);
    if (slot < 0) continue;
    const uint64_t logical = uint64_t(i + 1) * bank - 1;
    REQUIRE(map.Translate(logical, 1, local, bank));
    REQUIRE(local == uint64_t(slot + 1) * bank - 1);
    REQUIRE(uint64_t(i) * bank + (local - uint64_t(slot) * bank) == logical);
  }
}
