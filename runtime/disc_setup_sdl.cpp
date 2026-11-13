// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The window build's disc setup host (disc_setup_sdl.h): the launcher's Disc setup panel as a child process.
#include "disc_setup_sdl.h"
#include "stops.h"
#include "video/window_sdl.h"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace runtime::disc {
namespace {

class PanelHost final : public Host
{
public:
    PanelHost(std::string panel, std::string home) : panel_(std::move(panel)), home_(std::move(home)) {}

    void OpenPanel(uint32_t disc, const std::string& gameText, const std::string& problem, std::function<void(InstallResult)> done) override
    {
        int out[2];
        if (pipe(out) != 0)
            return done({false, "", "", "the setup panel could not be started"});
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, out[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, out[0]);
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);  // its own group: ClosePanel ends it and its setup tool
        posix_spawnattr_setpgroup(&attributes, 0);
        // FM4 Recomp --disc-panel <disc> --frame <x,y,w,h> [--message <the game's text>] [--problem <text>] (app/setup/Panel.swift):
        // the game window's frame in points, top-left origin on the main display (SDL's), so the panel opens centred over it.
        const SDL_Rect frame = runtime::video::window::WindowFrame();
        const std::string discArg = std::to_string(disc);
        const std::string frameArg = std::to_string(frame.x) + "," + std::to_string(frame.y) + "," + std::to_string(frame.w) + "," + std::to_string(frame.h);
        std::vector<const char*> argv = {panel_.c_str(), "--disc-panel", discArg.c_str(), "--frame", frameArg.c_str()};
        if (!gameText.empty())
            argv.push_back("--message"), argv.push_back(gameText.c_str());
        if (!problem.empty())
            argv.push_back("--problem"), argv.push_back(problem.c_str());
        argv.push_back(nullptr);
        pid_t pid = 0;
        const int spawned = posix_spawn(&pid, panel_.c_str(), &actions, &attributes, const_cast<char* const*>(argv.data()), environ);
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        close(out[1]);
        if (spawned != 0)
        {
            close(out[0]);
            return done({false, "", "", "the setup panel " + panel_ + " could not be started: " + std::strerror(spawned)});
        }
        child_.store(pid);
        std::fprintf(stderr, "[disc] setup panel %s started for Disc %u (pid %d)\n", panel_.c_str(), disc, int(pid));
        std::thread([this, pid, fd = out[0], done = std::move(done)] {
            SetupRun run;
            FILE* in = fdopen(fd, "r");
            char buffer[1024];
            while (in != nullptr && std::fgets(buffer, sizeof buffer, in))
            {
                std::string line = buffer;
                if (!line.empty() && line.back() == '\n')
                    line.pop_back();
                if (line.compare(0, 7, "result ") == 0)
                    run.result = line.substr(7);
                else if (line.compare(0, 7, "setup: ") == 0)
                    run.detail = line.substr(7);
                std::fprintf(stderr, "[disc] panel: %s\n", line.c_str());
            }
            if (in != nullptr)
                std::fclose(in);
            int status = 0;
            waitpid(pid, &status, 0);
            child_.store(0);
            run.exitStatus = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            std::ifstream conf(home_ + "/locations.conf");
            std::stringstream text;
            text << conf.rdbuf();
            run.locations = text.str();
            done(InterpretSetup(run));
        }).detach();
    }

    void ClosePanel() override
    {
        if (const pid_t pid = child_.load(); pid > 0)
            kill(-pid, SIGTERM);  // the panel's group: the panel and its setup tool (setup.sh stops its extractor, "cancelled" (6))
    }

private:
    std::string panel_, home_;
    std::atomic<pid_t> child_{0};
};

}  // namespace

std::unique_ptr<Host> MakeSdlHost(const std::string& panel)
{
    if (access(panel.c_str(), X_OK) != 0)
        Stop(StopCode::Setup, "--disc-setup %s is not an executable (the kit's setup panel, bin/fm4recomp-setup)", panel.c_str());
    // locations.conf lives in FM4_RECOMP_HOME (play.sh exports it, and FM4_RECOMP_KIT, which the panel reads too).
    const char* home = std::getenv("FM4_RECOMP_HOME");
    if (home == nullptr || *home == 0)
        Stop(StopCode::Setup, "--disc-setup needs FM4_RECOMP_HOME (the player's data folder; play.sh sets it)");
    return std::make_unique<PanelHost>(panel, home);
}

}  // namespace runtime::disc
