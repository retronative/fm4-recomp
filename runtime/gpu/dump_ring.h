// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// A capped ring of GPU dump records (run safety: dumps capped; an uncapped
// dump in ForzaRecomp once filled ~50 GB). At most `Capacity` records and `byteCap` bytes are kept:
// a new record evicts the oldest ones until both limits hold, and the caller deletes what was
// evicted. A record larger than the whole byte cap is refused and evicts nothing. Pure bookkeeping:
// no file is written or removed here.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace runtime::gpu {

template <size_t Capacity>
class DumpRing
{
    static_assert(Capacity > 0);

public:
    struct Record
    {
        uint64_t id = 0, bytes = 0;
    };
    struct Pushed
    {
        bool accepted = false;
        std::vector<Record> evicted;  // oldest first: the caller deletes their files
    };

    explicit DumpRing(uint64_t byteCap) : byteCap_(byteCap) {}

    Pushed Push(Record r)
    {
        Pushed result;
        if (r.bytes > byteCap_)
            return result;
        while (count_ == Capacity || bytes_ + r.bytes > byteCap_)
        {
            result.evicted.push_back(records_[head_]);
            bytes_ -= records_[head_].bytes;
            head_ = (head_ + 1) % Capacity;
            count_--;
        }
        records_[(head_ + count_) % Capacity] = r;
        count_++;
        bytes_ += r.bytes;
        result.accepted = true;
        return result;
    }

    size_t Count() const { return count_; }
    uint64_t Bytes() const { return bytes_; }
    Record At(size_t i) const { return records_[(head_ + i) % Capacity]; }  // 0 is the oldest

private:
    std::array<Record, Capacity> records_{};
    size_t head_ = 0, count_ = 0;
    uint64_t bytes_ = 0;
    const uint64_t byteCap_;
};

}  // namespace runtime::gpu
