#pragma once

#include <cstdint>

namespace rex::graphics {

// Guest physical byte ranges read by one draw. An unrepresentable range
// makes the caller fall back to the exhaustive snapshot check for that draw.
class DrawPageRanges {
 public:
  bool Add(uint32_t address, uint32_t length) {
    if (!length) return true;
    constexpr uint32_t kGuestBytes = 1u << 29;
    if (address >= kGuestBytes || length > kGuestBytes - address) return false;
    uint32_t first = address >> 12;
    uint32_t last = (address + length - 1) >> 12;
    for (uint32_t i = 0; i < count_; ++i) {
      auto& range = ranges_[i];
      if (first > range.last + 1 || last + 1 < range.first) continue;
      if (first < range.first) range.first = first;
      if (last > range.last) range.last = last;
      MarkBuckets(first, last);
      return true;
    }
    if (count_ == kCapacity) return false;
    ranges_[count_++] = {first, last};
    MarkBuckets(first, last);
    return true;
  }

  bool ContainsPage(uint32_t page) const {
    if (page >= (1u << 17) || !(buckets_ & (uint64_t(1) << (page >> 11)))) return false;
    for (uint32_t i = 0; i < count_; ++i) {
      if (page >= ranges_[i].first && page <= ranges_[i].last) return true;
    }
    return false;
  }

 private:
  void MarkBuckets(uint32_t first, uint32_t last) {
    // Reject pages outside the draw's 8 MiB regions with a single bit test.
    buckets_ |= (~uint64_t(0) << (first >> 11)) & (~uint64_t(0) >> (63 - (last >> 11)));
  }

  struct PageInterval { uint32_t first, last; };
  static constexpr uint32_t kCapacity = 96;
  PageInterval ranges_[kCapacity];
  uint64_t buckets_ = 0;
  uint32_t count_ = 0;
};

}  // namespace rex::graphics
