// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (Disc 2): a disc the game asks for (XamSwapDisc) that the player has not set up
// yet. The game keeps running: the request stays pending, as on the console while its "insert the disc" screen is up, and
// the launcher's own Disc setup panel (FM4 Recomp's setup app, --disc-panel: drop or choose the image, choose where its
// files go, the setup's progress, Cancel and Try Again) opens over the game window in its own process, so the window stays
// responsive. The panel runs the kit's setup tool itself and ends once the disc is set up (then it is mounted and the request
// completes: the completion event is set, the panel is gone, and the game shows its next screen) or when the player leaves
// it ("Back to the Game": the game keeps waiting). The game's own way out stays the only one: its Disc 2 screen's Back
// cancels through XamSwapCancel (FM4's CMultiDiscSwap), which closes the panel, ends the
// request and sets the event so its swap thread can finish (it waits on that event with no timeout). Nothing here ever
// completes a request without a disc in the drive.
//
// The host side (the panel process) is the Host interface: the window build implements it (runtime/disc_setup_sdl.cpp),
// the tests with a fake. SwapService is the state machine, CPU only:
//   Idle --Request--> Opening --Pump: panel opened--> Open --ok + mounted--> Idle (completed)
//   Open --left by the player (any other result)--> Waiting (the game's Back still cancels)
//   Open --ok, the mount refuses the disc--> Opening (the panel again, on that problem)
//   Opening / Waiting --Cancel--> Idle (event set);  Open --Cancel--> Cancelling (event set, panel closed) --result--> Idle
// A new request while one is pending stops (7) (not modelled; FM4 runs one swap thread at a time).
#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace runtime::disc {

struct InstallResult
{
    bool ok = false;
    std::string root, map;  // the extracted disc and its GDFX map (ok)
    std::string problem;    // why not (not ok), in words (the log; the panel showed it to the player)
};

class Host
{
public:
    virtual ~Host() = default;
    // Main thread, never waits for the player: opens the setup panel for `disc` over the game window; `problem` (not empty)
    // opens it on that failure, with Try Again. `gameText` is the game's own message. `done` is called once, from any thread,
    // with the panel's outcome.
    virtual void OpenPanel(uint32_t disc, const std::string& gameText, const std::string& problem, std::function<void(InstallResult)> done) = 0;
    // Any thread: closes the panel and stops its setup (its partial output stays; the tool resumes or refuses it next time).
    virtual void ClosePanel() = 0;
};

class SwapService
{
public:
    enum class State { Idle, Opening, Open, Waiting, Cancelling };
    explicit SwapService(Host& host) : host_(host) {}
    // XamSwapDisc for a disc not set up (guest thread). `mount` puts the installed disc in the drive and returns "" or a
    // problem (the identity check); `complete` sets the request's completion event. Stops (7) if a request is pending.
    void Request(uint32_t disc, const std::string& gameText, std::function<std::string(const InstallResult&)> mount, std::function<void()> complete);
    // XamSwapCancel (guest thread): ends a pending request and sets its event; false if none was pending.
    bool Cancel();
    // Main thread, every window loop turn, never waits: opens the panel when the state needs it and takes its result.
    void Pump();
    State Current() const;

private:
    Host& host_;
    mutable std::mutex lock_;
    State state_ = State::Idle;
    uint32_t disc_ = 0;
    std::string text_, problem_;
    std::function<std::string(const InstallResult&)> mount_;
    std::function<void()> complete_;
    bool hasResult_ = false;
    InstallResult result_;
};

// The setup panel's outcome (FM4 Recomp --disc-panel, app/setup/Panel.swift), in the kit's setup tool's terms (the data side's
// scripts/alpha/setup.sh --disc2 [--dest <dir>] <iso>, revision 2, whose lines the panel passes on). Its contract:
// stdout "progress <percent> <what>" lines, then
// "result <name>" lines (the panel's last one: "ok" once set up, "cancelled" when the player left), the exit status being its number (ok 0, failed 1, usage 2, wrong-disc 3, no-space 4,
// extract-failed 5, cancelled 6, not-set-up 7, unsupported 8); "setup: ..." lines on stderr explain; on ok it has recorded
// disc2_root and disc2_map in $FM4_RECOMP_HOME/locations.conf. One finished run, as the host saw it:
struct SetupRun
{
    int exitStatus = -1;    // -1: ended by a signal, or never started
    std::string result;     // its "result" line's name ("" if none)
    std::string detail;     // its last "setup: " line, without the prefix
    std::string locations;  // locations.conf's text after the run ("" if unreadable)
};
// The result name an exit status stands for ("" for any other).
std::string SetupResultName(int exitStatus);
// The run as an InstallResult: ok only for "ok" with exit status 0 and a usable Disc 2 record in `locations`; every other
// outcome a problem in words for the player (wrong disc, no space, extraction failed, stopped, unsupported build, Disc 1 not
// set up, a failure), with the tool's own detail appended. A result name and an exit status that disagree are a failure.
InstallResult InterpretSetup(const SetupRun& run);
// locations.conf (the data side's scripts/alpha/locations.sh format): UTF-8, one key=value per line, the value the rest of the
// line verbatim; '#' and blank lines are comments; lines without '=' and unknown keys are ignored; version, when present, must
// be 1; disc2_root and disc2_map at most once each, both or neither, absolute paths without CR. False with `problem` otherwise;
// true with empty root and map when Disc 2 is not recorded.
bool ReadDisc2Location(const std::string& text, std::string& root, std::string& map, std::string& problem);

// The process's service, when the window build provides a host (main.cpp: --renderer window with --disc-setup <the panel>);
// null otherwise, and XamSwapDisc then stops naming the options to set the disc up.
void SetService(SwapService* service);
SwapService* Service();
// Main thread, from the window loop: the service's Pump, when there is one.
void PumpMainThread();

}  // namespace runtime::disc
