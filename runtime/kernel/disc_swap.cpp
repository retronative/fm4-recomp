// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The pending disc swap (disc_swap.h).
#include "disc_swap.h"
#include "stops.h"
#include <atomic>
#include <cstdio>
#include <iterator>

namespace runtime::disc {

void SwapService::Request(uint32_t disc, const std::string& gameText, std::function<std::string(const InstallResult&)> mount,
                          std::function<void()> complete)
{
    std::lock_guard guard(lock_);
    if (state_ != State::Idle)
        Stop(StopCode::Unimplemented, "XamSwapDisc(%u): a swap to Disc %u is still pending (two at once are not modelled)", disc, disc_);
    state_ = State::Opening, disc_ = disc, text_ = gameText, problem_.clear(), mount_ = std::move(mount), complete_ = std::move(complete);
    hasResult_ = false;
    std::fprintf(stderr, "[disc] Disc %u is not set up: opening the setup panel over the game (the game keeps waiting)\n", disc);
}

bool SwapService::Cancel()
{
    std::function<void()> complete;
    bool open = false;
    uint32_t disc = 0;
    {
        std::lock_guard guard(lock_);
        if (state_ == State::Idle || state_ == State::Cancelling)
            return false;
        open = state_ == State::Open;
        state_ = open ? State::Cancelling : State::Idle;  // Cancelling: the panel's result is taken and dropped by Pump
        complete = std::move(complete_), disc = disc_;
    }
    if (open)
        host_.ClosePanel();  // outside the lock: a host may report the closed panel's result at once
    std::fprintf(stderr, "[disc] the game cancelled the swap to Disc %u: its request ends without the disc\n", disc);
    complete();  // the swap thread wakes, finds its own state "cancelled" and ends (it never sets "done")
    return true;
}

SwapService::State SwapService::Current() const
{
    std::lock_guard guard(lock_);
    return state_;
}

void SwapService::Pump()
{
    std::unique_lock guard(lock_);
    const uint32_t disc = disc_;
    if (state_ == State::Opening)
    {
        const std::string text = text_, problem = problem_;
        state_ = State::Open, hasResult_ = false;
        guard.unlock();  // the host may report at once (a panel that cannot start)
        host_.OpenPanel(disc, text, problem, [this](InstallResult r) {
            std::lock_guard g(lock_);
            result_ = std::move(r), hasResult_ = true;
        });
        return;
    }
    if ((state_ != State::Open && state_ != State::Cancelling) || !hasResult_)
        return;
    InstallResult r = std::move(result_);
    hasResult_ = false;
    if (state_ == State::Cancelling)
    {
        state_ = State::Idle;
        std::fprintf(stderr, "[disc] the closed panel for Disc %u ended (%s)\n", disc, r.ok ? "set up, not used" : r.problem.c_str());
        return;
    }
    if (!r.ok)
    {
        state_ = State::Waiting;
        std::fprintf(stderr, "[disc] the setup panel for Disc %u closed without the disc (%s); the game keeps waiting: its Back returns to "
                     "the menu, and choosing Disc %u again reopens the panel\n", disc, r.problem.c_str(), disc);
        return;
    }
    {
        const std::string problem = mount_(r);  // main thread; the guest's swap thread is waiting on the event
        if (problem.empty())
        {
            std::function<void()> complete = std::move(complete_);
            state_ = State::Idle;
            guard.unlock();
            std::fprintf(stderr, "[disc] Disc %u set up (%s) and in the drive\n", disc, r.root.c_str());
            complete();  // only now: the disc is in the drive
            return;
        }
        std::fprintf(stderr, "[disc] Disc %u was set up, but it cannot go in the drive: %s\n", disc, problem.c_str());
        problem_ = "Disc " + std::to_string(disc) + " was set up, but " + problem;
        state_ = State::Opening;  // the panel again, on the refusal
    }
}

std::string SetupResultName(int exitStatus)
{
    static const char* const kNames[] = {"ok", "failed", "usage", "wrong-disc", "no-space", "extract-failed", "cancelled", "not-set-up", "unsupported"};
    return exitStatus >= 0 && exitStatus < int(std::size(kNames)) ? kNames[exitStatus] : "";
}

bool ReadDisc2Location(const std::string& text, std::string& root, std::string& map, std::string& problem)
{
    root.clear(), map.clear();
    int roots = 0, maps = 0;
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        const size_t equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos)
            continue;
        const std::string key = line.substr(0, equals), value = line.substr(equals + 1);
        if (key == "version" && value != "1")
            return problem = "locations.conf has version " + value + " (this build reads version 1)", false;
        if (key != "disc2_root" && key != "disc2_map")
            continue;
        if ((key == "disc2_root" ? ++roots : ++maps) > 1)
            return problem = "locations.conf names " + key + " more than once", false;
        if (value.empty() || value[0] != '/' || value.find('\r') != std::string::npos)
            return problem = "locations.conf's " + key + " is not an absolute path", false;
        (key == "disc2_root" ? root : map) = value;
    }
    if (root.empty() != map.empty())
        return root.clear(), map.clear(), problem = "locations.conf names only one of disc2_root and disc2_map", false;
    return true;
}

InstallResult InterpretSetup(const SetupRun& run)
{
    const std::string name = run.result.empty() ? SetupResultName(run.exitStatus) : run.result;
    InstallResult r;
    auto fail = [&](const std::string& what) {
        r.problem = what + (run.detail.empty() ? "" : " (" + run.detail + ")");
        return r;
    };
    if (run.exitStatus < 0)
        return fail("the setup was ended before it finished");
    if (SetupResultName(run.exitStatus) != name)
        return fail("the setup tool reported \"" + name + "\" but ended with status " + std::to_string(run.exitStatus));
    if (name == "ok")
    {
        std::string problem;
        if (!ReadDisc2Location(run.locations, r.root, r.map, problem))
            return fail("the setup finished, but " + problem);
        if (r.root.empty())
            return fail("the setup finished, but locations.conf does not name Disc 2");
        r.ok = true;
        return r;
    }
    if (name == "wrong-disc")
        return fail("the image is not Forza Motorsport 4 (USA) Disc 2 (Content Install Disc), or a file on it is not the one this build needs");
    if (name == "no-space")
        return fail("there is not enough free space: Disc 2's files need about 3.2 GB in the chosen folder, and the game needs about 3.0 GB "
                    "more in your save folder to install its content; choose another folder or free some space");
    if (name == "extract-failed")
        return fail("the disc image could not be extracted (it may be damaged or unreadable)");
    if (name == "cancelled")
        return fail("the setup was stopped before it finished; the files copied so far are removed the next time");
    if (name == "unsupported")
        return fail("this build of FM4 Recomp cannot set up Disc 2 yet (it has no checksums for it); Disc 1 alone still plays");
    if (name == "not-set-up")
        return fail("Disc 1 is not set up for this build; run the kit's setup with your Disc 1 first");
    return fail("the setup failed");
}

namespace {
std::atomic<SwapService*> service{nullptr};
}  // namespace

void SetService(SwapService* s) { service.store(s); }
SwapService* Service() { return service.load(); }

void PumpMainThread()
{
    if (SwapService* s = service.load())
        s->Pump();
}

}  // namespace runtime::disc
