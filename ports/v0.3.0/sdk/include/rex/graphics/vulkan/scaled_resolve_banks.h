#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace rex::graphics::vulkan {

// Each bank covers 16 MiB of guest addresses at any scale. A reservation is contiguous
// within a descriptor, and any assigned bank stays at its slot until promotion.
class ScaledResolveBanks {
 public:
  static constexpr uint32_t kCount = 32;
  static constexpr uint32_t kGuestSize = uint32_t{16} << 20;
  static constexpr uint64_t BankSize(uint32_t scale) { return uint64_t(kGuestSize) * scale * scale; }
  static constexpr uint64_t kSize = uint64_t(kGuestSize) * 4;  // QHD default
  static constexpr uint64_t kAddressSize = kSize * kCount;

  int slot(uint32_t bank) const { return int(slots_[bank]) - 1; }

  bool Translate(uint64_t start, uint64_t length, uint64_t& local,
                 uint64_t bank_size = kSize) const {
    if (!ValidRange(start, length, bank_size)) return false;
    const uint32_t first = uint32_t(start / bank_size);
    const uint32_t last = uint32_t((start + length - 1) / bank_size);
    const int base = slot(first);
    if (base < 0) return false;
    for (uint32_t bank = first + 1; bank <= last; ++bank) {
      if (slot(bank) != base + int(bank - first)) return false;
    }
    local = uint64_t(base) * bank_size + start % bank_size;
    return true;
  }

  bool Reserve(uint64_t start, uint64_t length, uint32_t capacity,
               uint64_t bank_size = kSize) {
    if (!length) return true;
    if (!capacity || capacity > kCount || !ValidRange(start, length, bank_size)) return false;
    const uint32_t first = uint32_t(start / bank_size);
    const uint32_t last = uint32_t((start + length - 1) / bank_size);
    const uint32_t count = last - first + 1;
    if (count > capacity) return false;

    int required = -1;
    for (uint32_t bank = first; bank <= last; ++bank) {
      if (slot(bank) < 0) continue;
      const int base = slot(bank) - int(bank - first);
      if (base < 0 || base + int(count) > int(capacity) ||
          (required >= 0 && base != required)) return false;
      required = base;
    }
    const auto install = [&](uint32_t base) {
      if (base + count > capacity) return false;
      for (uint32_t bank = first; bank <= last; ++bank) {
        const uint32_t destination = base + (bank - first);
        if ((used_ & (uint32_t{1} << destination)) &&
            slot(bank) != int(destination)) return false;
      }
      for (uint32_t bank = first; bank <= last; ++bank) {
        const uint32_t destination = base + (bank - first);
        used_ |= uint32_t{1} << destination;
        slots_[bank] = uint8_t(destination + 1);
      }
      return true;
    };
    if (required >= 0) return install(uint32_t(required));
    // Keep neighboring logical banks together whenever they fit, then try their
    // natural slots and finally any contiguous hole. Unrelated gaps cost nothing.
    if (first && slot(first - 1) >= 0 && install(uint32_t(slot(first - 1) + 1))) return true;
    if (last + 1 < kCount && slot(last + 1) >= int(count) &&
        install(uint32_t(slot(last + 1)) - count)) return true;
    const uint32_t preferred = std::min(first % capacity, capacity - count);
    for (uint32_t i = 0; i < capacity; ++i) {
      if (install((preferred + i) % capacity)) return true;
    }
    return false;
  }

 private:
  static bool ValidRange(uint64_t start, uint64_t length, uint64_t bank_size) {
    if (!bank_size || bank_size > UINT64_MAX / kCount) return false;
    const uint64_t space = bank_size * kCount;
    return length && start < space && length <= space - start;
  }

  uint32_t used_ = 0;
  std::array<uint8_t, kCount> slots_ = {};
};

}  // namespace rex::graphics::vulkan
