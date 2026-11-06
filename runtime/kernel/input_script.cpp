// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The scripted gamepad's parser and poller (input_script.h). Tested by tests/runtime/test_runtime.cpp.
#include "input_script.h"
#include <cstdio>

namespace runtime::input {
namespace {

struct Name { const char* text; uint16_t button; uint8_t trigger; };  // trigger: 1 left, 2 right, 0 a button
constexpr Name kNames[] = {
    {"A", kA, 0},           {"B", kB, 0},           {"X", kX, 0},           {"Y", kY, 0},
    {"START", kStart, 0},   {"BACK", kBack, 0},     {"LB", kLeftShoulder, 0}, {"RB", kRightShoulder, 0},
    {"LT", 0, 1},           {"RT", 0, 2},           {"DPAD_UP", kDpadUp, 0}, {"DPAD_DOWN", kDpadDown, 0},
    {"DPAD_LEFT", kDpadLeft, 0}, {"DPAD_RIGHT", kDpadRight, 0}, {"LS", kLeftThumb, 0}, {"RS", kRightThumb, 0},
};

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// The next whitespace-separated token of `line` from `at`, or empty at its end.
std::string_view Token(std::string_view line, size_t& at)
{
    while (at < line.size() && IsSpace(line[at]))
        at++;
    const size_t start = at;
    while (at < line.size() && !IsSpace(line[at]))
        at++;
    return line.substr(start, at - start);
}

bool ParseVblank(std::string_view token, uint64_t& value)
{
    if (token.empty() || token.size() > 19)  // 19 digits always fit a uint64
        return false;
    value = 0;
    for (const char c : token)
    {
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + uint64_t(c - '0');
    }
    return true;
}

bool Fail(std::string& error, unsigned line, const char* what, std::string_view token = {})
{
    error = "line " + std::to_string(line) + ": " + what;
    if (!token.empty())
        error += " '" + std::string(token) + "'";
    return false;
}

}  // namespace

bool ParseScript(std::string_view text, Script& out, std::string& error)
{
    out.events.clear();
    error.clear();
    unsigned number = 0;
    while (!text.empty())
    {
        number++;
        const size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (const size_t hash = line.find('#'); hash != std::string_view::npos)
            line = line.substr(0, hash);
        size_t at = 0;
        const std::string_view first = Token(line, at);
        if (first.empty())
            continue;
        Event event;
        if (!ParseVblank(first, event.vblank))
            return Fail(error, number, "the vblank must be a decimal number, not", first);
        if (!out.events.empty() && event.vblank <= out.events.back().vblank)
            return Fail(error, number, "vblank is not above the previous line's", first);
        for (std::string_view token = Token(line, at); !token.empty(); token = Token(line, at))
        {
            const Name* found = nullptr;
            for (const Name& name : kNames)
                if (token == name.text)
                    found = &name;
            if (found == nullptr)
                return Fail(error, number, "unknown button", token);
            event.buttons |= found->button;
            if (found->trigger == 1)
                event.leftTrigger = 0xFF;
            else if (found->trigger == 2)
                event.rightTrigger = 0xFF;
        }
        out.events.push_back(event);
    }
    return true;
}

long ActiveEvent(const Script& script, uint64_t vblank)
{
    long active = -1;
    for (size_t i = 0; i < script.events.size() && script.events[i].vblank <= vblank; i++)
        active = long(i);
    return active;
}

Report Poller::Poll(uint64_t vblank)
{
    Report report;
    report.event = ActiveEvent(script_, vblank);
    // Xenia's SDL driver (sdl_input_driver.cc:190-198): one increment per poll that sees a change, whatever the
    // number of updates in between; a freshly connected pad has a change pending so its first poll reports 1 (:477).
    if (packet_ == 0 || report.event != event_)
    {
        packet_++;
        event_ = report.event;
    }
    report.packet = packet_;
    if (report.event >= 0)
    {
        const Event& e = script_.events[size_t(report.event)];
        report.buttons = e.buttons, report.leftTrigger = e.leftTrigger, report.rightTrigger = e.rightTrigger;
    }
    return report;
}

std::string Describe(uint16_t buttons, uint8_t leftTrigger, uint8_t rightTrigger)
{
    std::string text;
    for (const Name& name : kNames)
    {
        const bool held = name.trigger == 0 ? (buttons & name.button) != 0 : (name.trigger == 1 ? leftTrigger : rightTrigger) != 0;
        if (held)
            text += (text.empty() ? "" : " ") + std::string(name.text);
    }
    return text;
}

}  // namespace runtime::input
