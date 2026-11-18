// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// XamShowKeyboardUI and its dialog hosts (keyboard_ui.h).
#include "keyboard_ui.h"
#include "dispatcher.h"
#include "imports.h"
#include "notify.h"
#include "objects.h"
#include "overlapped.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace runtime::keyboard_ui {
namespace {

std::mutex hostLock;
Host host;
std::atomic<int> shown{0};

std::string ToUtf8(const std::u16string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (c < 0x80)
            out += char(c);
        else if (c < 0x800)
            out += char(0xC0 | c >> 6), out += char(0x80 | (c & 0x3F));
        else if (c < 0x10000)
            out += char(0xE0 | c >> 12), out += char(0x80 | (c >> 6 & 0x3F)), out += char(0x80 | (c & 0x3F));
        else
            out += char(0xF0 | c >> 18), out += char(0x80 | (c >> 12 & 0x3F)), out += char(0x80 | (c >> 6 & 0x3F)), out += char(0x80 | (c & 0x3F));
    }
    return out;
}

std::u16string ToUtf16(const std::string& s)
{
    std::u16string out;
    for (size_t i = 0; i < s.size();)
    {
        const uint8_t b = uint8_t(s[i]);
        const int n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size())
        {
            out += u'�', ++i;
            continue;
        }
        uint32_t c = n == 1 ? b : b & (0x7F >> n);
        for (int k = 1; k < n; ++k)
            c = c << 6 | (uint8_t(s[i + k]) & 0x3F);
        i += n;
        if (c >= 0x10000)
            out += char16_t(0xD800 + ((c - 0x10000) >> 10)), out += char16_t(0xDC00 + ((c - 0x10000) & 0x3FF));
        else
            out += char16_t(c);
    }
    return out;
}

// A guest UTF-16BE string (null-terminated); 0: empty.
std::u16string LoadString16(const uint8_t* base, uint32_t address)
{
    std::u16string out;
    if (address == 0)
        return out;
    for (uint32_t a = address; out.size() < 4096; a += 2)
    {
        const uint16_t c = Load16(base, a);
        if (c == 0)
            break;
        out += char16_t(c);
    }
    return out;
}

// The dialog with XN_SYS_UI around it (xeXamDispatchDialogEx pre / run / post).
Answer Show(uint8_t* base, const Request& request)
{
    Host h;
    {
        std::lock_guard<std::mutex> lock(hostLock);
        h = host;
    }
    BeginSystemUi(base);
    Answer answer;
    if (h)
        answer = h(request);
    else  // headless (xam_ui.cc:486-497): the default text is the answer
        answer.text = request.defaultText;
    EndSystemUi(base);
    std::fprintf(stderr, "[ui] XamShowKeyboardUI \"%s\": %s%s%s\n", ToUtf8(request.title).c_str(), answer.cancelled ? "cancelled" : "OK \"",
                 answer.cancelled ? "" : ToUtf8(answer.text).c_str(), answer.cancelled ? "" : "\"");
    return answer;
}

// AppleScript's `display dialog` with a text field; argv: title, description, default text. Cancel ends it with error -128.
const char* const kDialogScript[] = {
    "on run argv",
    "activate",
    ("set r to display dialog (item 2 of argv) with title (item 1 of argv) default answer (item 3 of argv) buttons {\"Cancel\", \"OK\"} "
     "default button \"OK\" cancel button \"Cancel\""),
    "return text returned of r",
    "end run",
};

}  // namespace

void SetHost(Host h)
{
    std::lock_guard<std::mutex> lock(hostLock);
    host = std::move(h);
}

// Reads a pipe to its end.
static std::string ReadAll(int fd)
{
    std::string text;
    char buffer[512];
    for (ssize_t n; (n = read(fd, buffer, sizeof buffer)) > 0;)
        text.append(buffer, size_t(n));
    close(fd);
    return text;
}

Host NativeDialogHost(const std::string& program)
{
    return [program](const Request& request) {
        const std::string title = ToUtf8(request.title), description = ToUtf8(request.description), text = ToUtf8(request.defaultText);
        std::vector<const char*> argv = {program.c_str()};
        for (const char* line : kDialogScript)
            argv.push_back("-e"), argv.push_back(line);
        argv.push_back(title.c_str()), argv.push_back(description.c_str()), argv.push_back(text.c_str()), argv.push_back(nullptr);
        // Two pipes: the answer (standard output) and the diagnostics (standard error). macOS's own warnings (e.g. "TSM
        // AdjustCapsLockLED...") go to standard error and must never become the typed name (2026-11-14 playtest).
        int out[2], err[2];
        if (pipe(out) != 0 || pipe(err) != 0)
            Stop(StopCode::Unimplemented, "XamShowKeyboardUI: cannot open the pipes for the dialog");
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, out[0]);
        posix_spawn_file_actions_addclose(&actions, err[0]);
        pid_t pid = 0;
        const int spawned = posix_spawn(&pid, argv[0], &actions, nullptr, const_cast<char* const*>(argv.data()), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(out[1]), close(err[1]);
        if (spawned != 0)
        {
            close(out[0]), close(err[0]);
            Stop(StopCode::Unimplemented, "XamShowKeyboardUI: %s could not be started: %s", program.c_str(), std::strerror(spawned));
        }
        // Standard error is read on a thread of its own so neither pipe can fill while the other is read.
        std::string diagnostics;
        std::thread errReader([&] { diagnostics = ReadAll(err[0]); });
        std::string output = ReadAll(out[0]);
        errReader.join();
        int status = 0;
        waitpid(pid, &status, 0);
        if (!output.empty() && output.back() == '\n')
            output.pop_back();
        Answer answer;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            answer.text = ToUtf16(output);
        else if (diagnostics.find("(-128)") != std::string::npos)
            answer.cancelled = true;
        else
            Stop(StopCode::Unimplemented, "XamShowKeyboardUI: the dialog failed (status %d): %s", status, diagnostics.c_str());
        return answer;
    };
}

bool ParseAnswers(const std::string& text, std::vector<Answer>& out, std::string& error)
{
    out.clear();
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find(';', start);
        if (end == std::string::npos)
            end = text.size();
        const std::string entry = text.substr(start, end - start);
        if (entry == "cancel")
            out.push_back({true, {}});
        else if (entry.compare(0, 3, "ok:") == 0)
            out.push_back({false, ToUtf16(entry.substr(3))});
        else
        {
            error = "entry " + std::to_string(out.size() + 1) + " \"" + entry + "\" is neither ok:<text> nor cancel";
            return false;
        }
        start = end + 1;
    }
    return true;
}

Host ScriptedHost(std::vector<Answer> answers)
{
    auto next = std::make_shared<size_t>(0);
    auto list = std::make_shared<std::vector<Answer>>(std::move(answers));
    return [next, list](const Request& request) {
        if (*next >= list->size())
            Stop(StopCode::Setup, "XamShowKeyboardUI \"%s\": dialog %zu, but FM4_KEYBOARD_UI_ANSWERS has %zu answer(s)", ToUtf8(request.title).c_str(),
                 *next + 1, list->size());
        return (*list)[(*next)++];
    };
}

uint32_t WriteAnswer(uint8_t* base, uint32_t buffer, uint32_t bufferLength, const Answer& answer)
{
    if (answer.cancelled)
        return kErrorCancelled;
    if (bufferLength == 0)  // copy_and_swap_truncating: nothing written
        return 0;
    const size_t chars = std::min<size_t>(answer.text.size(), bufferLength - 1);
    for (size_t i = 0; i < chars; ++i)
        Store16(base, buffer + uint32_t(i * 2), answer.text[i]);
    Store16(base, buffer + uint32_t(chars * 2), 0);
    return 0;
}

bool Active()
{
    return shown.load() > 0;
}

void BeginSystemUi(uint8_t* base)
{
    EnqueueNotification(base, kNotificationSystemUi, 1);
    ++shown;
}

void EndSystemUi(uint8_t* base)
{
    --shown;
    EnqueueNotification(base, kNotificationSystemUi, 0);
}

}  // namespace runtime::keyboard_ui

GUEST_IMPORT(XamShowKeyboardUI, "REFERENCE_DERIVED: Xenia 67f16c4 xam_ui.cc:468-523 (dialog, XN_SYS_UI 1/0, deferred completion; OK "
                                "copy_and_swap_truncating, Cancel X_ERROR_CANCELLED); argument layout VERIFIED from FM4's Photo Mode call")
{
    // (user index, flags, default text*, title*, description*, buffer*, buffer length (characters), XOVERLAPPED*).
    using namespace runtime;
    using namespace runtime::keyboard_ui;
    const uint32_t buffer = ctx.r8.u32, bufferLength = ctx.r9.u32, overlappedAddress = ctx.r10.u32;
    if (buffer == 0)
    {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    Request request{LoadString16(base, ctx.r6.u32), LoadString16(base, ctx.r7.u32), LoadString16(base, ctx.r5.u32), bufferLength};
    std::fprintf(stderr, "[ui] XamShowKeyboardUI(user %u, flags %08X, buffer %08X, %u characters, XOVERLAPPED %08X): \"%s\"\n", ctx.r3.u32,
                 ctx.r4.u32, buffer, bufferLength, overlappedAddress, ToUtf8(request.title).c_str());
    if (overlappedAddress == 0)
    {
        WriteAnswer(base, buffer, bufferLength, Show(base, request));
        ctx.r3.u64 = 0;
        return;
    }
    // CompleteOverlappedDeferredEx (kernel_state.cc:811-878): pending and the event reset now; the rest on a host thread.
    CheckOverlapped(ctx, base, overlappedAddress, "XamShowKeyboardUI");
    Store32(base, overlappedAddress + overlapped::kResult, overlapped::kErrorIoPending);
    const uint32_t event = Load32(base, overlappedAddress + overlapped::kEvent);
    uint32_t header = 0;
    if (event != 0)
    {
        header = LookupHandle(event)->header;  // CheckOverlapped: an event handle
        ResetEvent(base, header);
    }
    std::thread([base, buffer, bufferLength, overlappedAddress, header, request = std::move(request)] {
        const uint32_t extendedError = WriteAnswer(base, buffer, bufferLength, Show(base, request));
        Store32(base, overlappedAddress + overlapped::kResult, 0);
        Store32(base, overlappedAddress + overlapped::kExtendedError, extendedError);
        Store32(base, overlappedAddress + overlapped::kLength, 0);
        if (header != 0)
            SetEvent(base, header);  // after the fields, as CompleteOverlappedEx
    }).detach();
    ctx.r3.u64 = overlapped::kErrorIoPending;
}
