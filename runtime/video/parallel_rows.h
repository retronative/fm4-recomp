// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (M7): ParallelRows runs fn(first, last) over the rows [0, rows) in chunks on a small persistent pool, the calling
// thread taking part; it returns when every row is done. The resolve read-backs (edram_vk_plan.cpp) convert each pixel to its own
// tiled address, so rows are independent and the result is the serial one byte for byte. Below kParallelMinWork units of work
// (rows * work per row) it runs on the caller only. One caller at a time (the command-processor thread). The race profile:
// ResolveConvertToGuest was 13% of that thread.
#pragma once
#include <pthread.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace runtime::video {

constexpr uint64_t kParallelMinWork = 16384;
constexpr uint32_t kParallelWorkers = 6;

class RowPool
{
public:
    static RowPool& Get()
    {
        static RowPool* pool = new RowPool;  // never destroyed: its detached workers outlive static destruction
        return *pool;
    }

    void Run(uint32_t rows, const std::function<void(uint32_t, uint32_t)>& fn)
    {
        {
            std::lock_guard guard(lock_);
            job_ = &fn, rows_ = rows, chunk_ = std::max<uint32_t>(1, rows / ((kParallelWorkers + 1) * 4));
            next_.store(0), active_ = kParallelWorkers, generation_++;
        }
        wake_.notify_all();
        Work();
        std::unique_lock guard(lock_);
        done_.wait(guard, [&] { return active_ == 0; });
        job_ = nullptr;
    }

private:
    RowPool()
    {
        for (uint32_t i = 0; i < kParallelWorkers; i++)
            std::thread([this] {
                pthread_setname_np("fm4 resolve rows");
                uint64_t seen = 0;
                std::unique_lock guard(lock_);
                for (;;)
                {
                    wake_.wait(guard, [&] { return generation_ != seen; });
                    seen = generation_;
                    guard.unlock();
                    Work();
                    guard.lock();
                    if (--active_ == 0)
                        done_.notify_all();
                }
            }).detach();
    }

    void Work()
    {
        for (;;)
        {
            const uint32_t first = next_.fetch_add(chunk_);
            if (first >= rows_)
                return;
            (*job_)(first, std::min(rows_, first + chunk_));
        }
    }

    std::mutex lock_;
    std::condition_variable wake_, done_;
    const std::function<void(uint32_t, uint32_t)>* job_ = nullptr;
    uint32_t rows_ = 0, chunk_ = 1, active_ = 0;
    uint64_t generation_ = 0;
    std::atomic<uint32_t> next_{0};
};

inline void ParallelRows(uint32_t rows, uint64_t workPerRow, const std::function<void(uint32_t, uint32_t)>& fn)
{
    if (rows < 2 || uint64_t(rows) * workPerRow < kParallelMinWork)
        return fn(0, rows);
    RowPool::Get().Run(rows, fn);
}

}  // namespace runtime::video
