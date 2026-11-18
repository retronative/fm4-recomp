// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The live pad's keyboard bindings (key_bindings.h). Tested by tests/runtime/test_runtime.cpp.
#include "key_bindings.h"
#include "stops.h"
#include <algorithm>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <sys/stat.h>

namespace runtime::input {

const Action kActions[kActionCount] = {
    {"steer_left", &Keys::a},   {"steer_right", &Keys::d}, {"throttle", &Keys::w},     {"brake", &Keys::s},
    {"dpad_up", &Keys::up},     {"dpad_down", &Keys::down}, {"dpad_left", &Keys::left}, {"dpad_right", &Keys::right},
    {"a", &Keys::enter},        {"b", &Keys::backspace},    {"x", &Keys::x},            {"y", &Keys::y},
    {"start", &Keys::space},    {"back", &Keys::tab},       {"left_shoulder", &Keys::q}, {"right_shoulder", &Keys::e},
    {"camera", &Keys::c},
};

// SDL 3.4 src/events/SDL_keymap.c SDL_scancode_names (the strings) and SDL3/SDL_scancode.h (the values), for the keys
// app/setup/KeyBindings.swift can capture on a Mac keyboard.
const KeyName kKeyNames[] = {
    {"A", 4},  {"B", 5},  {"C", 6},  {"D", 7},  {"E", 8},  {"F", 9},  {"G", 10}, {"H", 11}, {"I", 12}, {"J", 13}, {"K", 14}, {"L", 15}, {"M", 16},
    {"N", 17}, {"O", 18}, {"P", 19}, {"Q", 20}, {"R", 21}, {"S", 22}, {"T", 23}, {"U", 24}, {"V", 25}, {"W", 26}, {"X", 27}, {"Y", 28}, {"Z", 29},
    {"1", 30}, {"2", 31}, {"3", 32}, {"4", 33}, {"5", 34}, {"6", 35}, {"7", 36}, {"8", 37}, {"9", 38}, {"0", 39},
    {"Return", 40}, {"Escape", 41}, {"Backspace", 42}, {"Tab", 43}, {"Space", 44},
    {"-", 45}, {"=", 46}, {"[", 47}, {"]", 48}, {"\\", 49}, {";", 51}, {"'", 52}, {",", 54}, {".", 55}, {"/", 56},
    {"F1", 58}, {"F2", 59}, {"F3", 60}, {"F4", 61}, {"F5", 62}, {"F6", 63}, {"F7", 64}, {"F8", 65}, {"F9", 66}, {"F10", 67}, {"F11", 68}, {"F12", 69},
    {"Home", 74}, {"PageUp", 75}, {"Delete", 76}, {"End", 77}, {"PageDown", 78}, {"Right", 79}, {"Left", 80}, {"Down", 81}, {"Up", 82},
    {"Keypad /", 84}, {"Keypad *", 85}, {"Keypad -", 86}, {"Keypad +", 87}, {"Keypad Enter", 88},
    {"Keypad 1", 89}, {"Keypad 2", 90}, {"Keypad 3", 91}, {"Keypad 4", 92}, {"Keypad 5", 93}, {"Keypad 6", 94}, {"Keypad 7", 95},
    {"Keypad 8", 96}, {"Keypad 9", 97}, {"Keypad 0", 98}, {"Keypad .", 99}, {"Keypad =", 103},
    {"Left Ctrl", 224}, {"Left Shift", 225}, {"Left Option", 226}, {"Right Ctrl", 228}, {"Right Shift", 229}, {"Right Option", 230},
};
const size_t kKeyNameCount = std::size(kKeyNames);

namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

std::string_view Trim(std::string_view s)
{
    while (!s.empty() && IsSpace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && IsSpace(s.back()))
        s.remove_suffix(1);
    return s;
}

int ActionIndex(std::string_view id)
{
    for (size_t i = 0; i < kActionCount; i++)
        if (id == kActions[i].id)
            return int(i);
    return -1;
}

bool Fail(std::string& error, unsigned line, const std::string& what)
{
    error = "line " + std::to_string(line) + ": " + what;
    return false;
}

// FM4 Recomp (in-game Keyboard Controls): swapped whole under the lock; readers keep their snapshot.
std::mutex activeLock;
std::shared_ptr<const KeyBindings> active = std::make_shared<const KeyBindings>(DefaultKeyBindings());
std::string activePath;

void SetActive(KeyBindings bindings, const std::string& path)
{
    auto next = std::make_shared<const KeyBindings>(std::move(bindings));
    std::lock_guard lock(activeLock);
    active = std::move(next);
    activePath = path;
}

// The file's text; false when it cannot be opened.
bool ReadText(const char* path, std::string& text)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::stringstream buffer;
    buffer << file.rdbuf();
    text = buffer.str();
    return true;
}

}  // namespace

int ScancodeFromName(std::string_view name)
{
    for (const KeyName& k : kKeyNames)
        if (name == k.name)
            return k.scancode;
    return -1;
}

const char* NameOfScancode(int scancode)
{
    for (const KeyName& k : kKeyNames)
        if (scancode == k.scancode)
            return k.name;
    return nullptr;
}

KeyBindings DefaultKeyBindings()
{
    // The defaults, in kActions order: A on Space, B on Backspace, X / Y on Left Shift / Left Ctrl, START on Return; the rest as the
    // map window_sdl.cpp hard-coded before the bindings (input_live.h Keys).
    static const char* const kDefaults[kActionCount][2] = {
        {"A", nullptr},      {"D", nullptr},     {"W", nullptr},    {"S", nullptr},     {"Up", nullptr}, {"Down", nullptr},
        {"Left", nullptr},   {"Right", nullptr}, {"Space", nullptr}, {"Backspace", nullptr}, {"Left Shift", nullptr}, {"Left Ctrl", nullptr},
        {"Return", nullptr}, {"Tab", nullptr},   {"Q", nullptr},    {"E", nullptr},     {"C", nullptr},
    };
    KeyBindings b;
    for (size_t i = 0; i < kActionCount; i++)
        for (const char* name : kDefaults[i])
            if (name != nullptr)
                b.keys[i].push_back(uint16_t(ScancodeFromName(name)));
    return b;
}

bool ParseKeyBindings(std::string_view text, KeyBindings& out, std::string& error)
{
    out = KeyBindings{};
    error.clear();
    int owner[256];  // the action of each scancode seen, -1 none
    unsigned ownerLine[256] = {};
    std::fill(std::begin(owner), std::end(owner), -1);
    bool named[kActionCount] = {};  // an action with a line (FM4 Recomp: `<action> = none` leaves it unbound)
    unsigned number = 0;
    while (!text.empty())
    {
        const size_t end = text.find('\n');
        const std::string_view raw = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        number++;
        const std::string_view line = Trim(raw);
        if (line.empty() || line.front() == '#')
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            return Fail(error, number, "expected '<action> = <key>', not '" + std::string(line) + "'");
        const std::string_view id = Trim(line.substr(0, eq)), name = Trim(line.substr(eq + 1));
        const int action = ActionIndex(id);
        if (action < 0)
            return Fail(error, number, "unknown action '" + std::string(id) + "'");
        named[action] = true;
        if (name == "none")
            continue;
        const int scancode = ScancodeFromName(name);
        if (scancode < 0)
            return Fail(error, number, "unknown key '" + std::string(name) + "'");
        if (owner[scancode] >= 0)
            return Fail(error, number, "key '" + std::string(name) + "' is already bound to '" + kActions[owner[scancode]].id + "' (line " +
                                           std::to_string(ownerLine[scancode]) + ")");
        owner[scancode] = action, ownerLine[scancode] = number;
        out.keys[action].push_back(uint16_t(scancode));
    }
    for (size_t i = 0; i < kActionCount; i++)
        if (!named[i])
        {
            error = std::string("no key for '") + kActions[i].id + "'";
            return false;
        }
    return true;
}

Keys KeysFromState(const KeyBindings& bindings, const bool* state, size_t count)
{
    Keys keys;
    for (size_t i = 0; i < kActionCount; i++)
        for (const uint16_t scancode : bindings.keys[i])
            if (scancode < count && state[scancode])
                keys.*kActions[i].field = true;
    return keys;
}

std::string DescribeKeyBindings(const KeyBindings& bindings)
{
    std::string text;
    for (size_t i = 0; i < kActionCount; i++)
    {
        text += (i ? ", " : "") + std::string(kActions[i].id) + " ";
        for (size_t k = 0; k < bindings.keys[i].size(); k++)
        {
            const char* name = NameOfScancode(bindings.keys[i][k]);
            text += (k ? "/" : "") + std::string(name != nullptr ? name : "?");
        }
    }
    return text;
}

void LoadKeyBindings(const char* path)
{
    std::string text;
    if (!ReadText(path, text))
        Stop(StopCode::Setup, "--keybindings %s: cannot be read", path);
    KeyBindings parsed;
    std::string error;
    if (!ParseKeyBindings(text, parsed, error))
        Stop(StopCode::Setup, "--keybindings %s %s", path, error.c_str());
    SetActive(std::move(parsed), path);
}

std::shared_ptr<const KeyBindings> ActiveKeyBindings()
{
    std::lock_guard lock(activeLock);
    return active;
}

std::string KeyBindingsPath()
{
    std::lock_guard lock(activeLock);
    return activePath;
}

ReloadResult ReloadKeyBindings(const std::string& path)
{
    ReloadResult result;
    const std::shared_ptr<const KeyBindings> before = ActiveKeyBindings();
    struct stat info;
    if (stat(path.c_str(), &info) != 0 && errno == ENOENT)
    {
        // Reset to Defaults removed the file: the defaults, as a launch without --keybindings.
        KeyBindings defaults = DefaultKeyBindings();
        result.changed = !(defaults == *before);
        SetActive(std::move(defaults), "");
        result.applied = true;
        result.message = "no " + path + ": the default bindings";
        return result;
    }
    std::string text, error;
    KeyBindings parsed;
    if (!ReadText(path.c_str(), text))
        result.message = path + " cannot be read: the bindings stay as they were";
    else if (!ParseKeyBindings(text, parsed, error))
        result.message = path + " " + error + ": the bindings stay as they were";
    else
    {
        result.changed = !(parsed == *before);
        SetActive(std::move(parsed), path);
        result.applied = true;
        result.message = "bindings from " + path;
    }
    return result;
}

}  // namespace runtime::input
