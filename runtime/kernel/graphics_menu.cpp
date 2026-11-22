// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The in-game GRAPHICS submenu's logic (graphics_menu.h). Tested by tests/runtime/test_runtime.cpp.
#include "graphics_menu.h"
#include <algorithm>
#include <mutex>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace runtime::settings {

const Option kResolutions[kResolutionCount] = {{"720p", "720P"}, {"fhd", "FULL HD"}, {"qhd", "QHD"}, {"4k", "4K"}};
const Option kSmoothings[kSmoothingCount] = {{"off", "OFF"}, {"soft", "SOFT"}, {"on", "STANDARD"}, {"strong", "STRONG"}};
const Option kHdrs[kHdrCount] = {{"off", "OFF"}, {"auto", "AUTO"}};
const Option kExtendeds[kExtendedCount] = {{"off", "OFF"}, {"on", "ON"}};

namespace {
std::mutex hdrLock;
HdrStatus hdrStatus;
std::string Upper(const std::string& text)
{
    std::string out;
    for (const char c : text)
        out += char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}
}  // namespace

namespace {
int extendedOffered = -1;  // -1: not read yet
}  // namespace
bool ExtendedOffered()
{
    if (extendedOffered < 0)
    {
        const char* v = std::getenv("FM4_EXTENDED_VIEW_OFFERED");
        extendedOffered = v != nullptr && v[0] == '1' && v[1] == '\0' ? 1 : 0;
    }
    return extendedOffered == 1;
}
void SetExtendedOffered(bool offered) { extendedOffered = offered ? 1 : 0; }
size_t Menu::Rows() { return ExtendedOffered() ? kRows : kRows - 1; }

void SetHdrStatus(const HdrStatus& status)
{
    std::lock_guard guard(hdrLock);
    hdrStatus = status;
}
HdrStatus GetHdrStatus()
{
    std::lock_guard guard(hdrLock);
    return hdrStatus;
}
std::string HdrStatusLine(const HdrStatus& status)
{
    return !status.known ? std::string() : status.active ? std::string("HDR ACTIVE.") : "SDR ACTIVE: " + Upper(status.reason) + ".";
}
std::string HdrStatusText(const HdrStatus& status)
{
    return !status.known ? std::string() : status.active ? std::string("active = hdr\n") : "active = sdr\nreason = " + status.reason + "\n";
}
bool PublishHdrStatus(const HdrStatus& status)
{
    {
        std::lock_guard guard(hdrLock);
        if (hdrStatus == status)
            return false;
        hdrStatus = status;
    }
    std::fprintf(stderr, "[graphics] %s\n", status.active ? "HDR active" : ("SDR active: " + status.reason).c_str());
    const char* home = std::getenv("FM4_RECOMP_HOME");
    if (home == nullptr || *home == 0)
        return true;
    const std::string path = std::string(home) + "/hdr_status", temporary = path + ".writing";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << HdrStatusText(status);
        out.flush();
        if (!out)
        {
            std::fprintf(stderr, "[graphics] hdr_status: cannot write %s\n", temporary.c_str());
            std::remove(temporary.c_str());
            return true;
        }
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0)
    {
        std::fprintf(stderr, "[graphics] hdr_status: cannot replace %s: %s\n", path.c_str(), std::strerror(errno));
        std::remove(temporary.c_str());
    }
    return true;
}

std::string Serialize(const Graphics& g)
{
    std::string text = "# FM4 Recomp graphics settings, written by FM4 Recomp's Graphics panel. Delete this file for the defaults.\n";
    text += std::string("internal_resolution = ") + kResolutions[g.resolution].file + "\n";
    text += std::string("anti_aliasing = ") + kSmoothings[g.smoothing].file + "\n";
    if (g.hdr != 0)
        text += std::string("hdr = ") + kHdrs[g.hdr].file + "\n";
    if (g.extended != 0)
        text += std::string("extended_view = ") + kExtendeds[g.extended].file + "\n";
    return text;
}

namespace {

std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool Find(const Option* options, size_t count, const std::string& value, size_t& index)
{
    for (size_t i = 0; i < count; i++)
        if (value == options[i].file)
            return index = i, true;
    return false;
}

}  // namespace

bool Parse(const std::string& text, Graphics& out, std::string& error)
{
    Graphics g;
    bool seenResolution = false, seenSmoothing = false, seenHdr = false, seenExtended = false;
    size_t number = 0, at = 0;
    while (at <= text.size())
    {
        const size_t end = std::min(text.find('\n', at), text.size());
        const std::string line = Trim(text.substr(at, end - at));
        at = end + 1;
        number++;
        if (line.empty() || line[0] == '#')
            continue;
        const std::string where = "line " + std::to_string(number) + ": ";
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            return error = where + "expected '<setting> = <value>', not '" + line + "'", false;
        const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
        if (key == "anti_aliasing")
        {
            if (seenSmoothing)
                return error = where + "anti_aliasing is set twice", false;
            if (!Find(kSmoothings, kSmoothingCount, value, g.smoothing))
                return error = where + "unknown anti_aliasing '" + value + "' (off, soft, on or strong)", false;
            seenSmoothing = true;
            continue;
        }
        if (key == "hdr")
        {
            if (seenHdr)
                return error = where + "hdr is set twice", false;
            if (!Find(kHdrs, kHdrCount, value, g.hdr))
                return error = where + "unknown hdr '" + value + "' (off or auto)", false;
            seenHdr = true;
            continue;
        }
        if (key == "extended_view")
        {
            if (seenExtended)
                return error = where + "extended_view is set twice", false;
            if (!Find(kExtendeds, kExtendedCount, value, g.extended))
                return error = where + "unknown extended_view '" + value + "' (off or on)", false;
            seenExtended = true;
            continue;
        }
        if (key != "internal_resolution")
            return error = where + "unknown setting '" + key + "'", false;
        if (seenResolution)
            return error = where + "internal_resolution is set twice", false;
        if (!Find(kResolutions, kResolutionCount, value, g.resolution))
            return error = where + "unknown resolution '" + value + "' (720p, fhd, qhd or 4k)", false;
        seenResolution = true;
    }
    out = g;
    return true;
}

Graphics Load(const std::string& path, std::string& error)
{
    error.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};  // no file: the defaults
    std::ostringstream text;
    text << in.rdbuf();
    Graphics g;
    std::string why;
    if (!Parse(text.str(), g, why))
        return error = "graphics.conf " + why, Graphics{};
    return g;
}

bool Save(const std::string& path, const Graphics& g, std::string& error)
{
    const std::string temporary = path + ".writing";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << Serialize(g);
        out.flush();
        if (!out)
        {
            error = "cannot write " + temporary;
            std::remove(temporary.c_str());
            return false;
        }
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0)
    {
        error = "cannot replace " + path + ": " + std::strerror(errno);
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

std::string GraphicsPath()
{
    const char* home = std::getenv("FM4_RECOMP_HOME");
    return home != nullptr && *home != 0 ? std::string(home) + "/graphics.conf" : std::string();
}

Menu::Menu(std::string path, std::function<bool(size_t, size_t)> applyLive) : path_(std::move(path)), applyLive_(std::move(applyLive))
{
    std::string why;
    graphics_ = Load(path_, why);  // an unreadable or invalid file: the defaults, as play.sh starts
}

const char* Menu::Label(size_t row)
{
    return row == kResolutionRow ? "RESOLUTION" : row == kSmoothingRow ? "EDGE SMOOTHING" : row == kHdrRow ? "HDR" : "EXTENDED VIEW";
}

const char* Menu::Description(size_t row)
{
    if (row == kHdrRow)
    {
        static thread_local std::string text;
        const std::string now = HdrStatusLine(GetHdrStatus());
        text = "BRIGHTER HIGHLIGHTS ON AN HDR DISPLAY. AUTO USES HDR ONLY WHERE THE DISPLAY SUPPORTS IT; OTHERWISE THE PICTURE IS STANDARD (SDR)." +
               (now.empty() ? std::string() : " NOW: " + now);
        return text.c_str();
    }
    if (row == kExtendedRow)
        return "RACES FILL A WINDOW OR SCREEN THAT IS NOT 16:9: MORE VIEW AT THE SIDES WHEN IT IS WIDER, MORE ABOVE AND BELOW WHEN IT IS TALLER. THE HUD KEEPS ITS SHAPE.";
    return row == kResolutionRow ? "THE RENDERING RESOLUTION. HIGHER IS SHARPER AND NEEDS MORE GRAPHICS TIME."
                                 : "EXTRA SMOOTHING OF JAGGED EDGES, ON TOP OF THE ANTI-ALIASING OF THE GAME.";
}

size_t Menu::Count(size_t row)
{
    return row == kResolutionRow ? kResolutionCount : row == kSmoothingRow ? kSmoothingCount : row == kHdrRow ? kHdrCount : row == kExtendedRow ? kExtendedCount : 0;
}

const char* Menu::ValueLabel(size_t row, size_t value)
{
    return value >= Count(row) ? "" : row == kResolutionRow ? kResolutions[value].label : row == kSmoothingRow ? kSmoothings[value].label : row == kHdrRow ? kHdrs[value].label : kExtendeds[value].label;
}

size_t Menu::Value(size_t row) const
{
    return row == kResolutionRow ? graphics_.resolution : row == kSmoothingRow ? graphics_.smoothing : row == kHdrRow ? graphics_.hdr : row == kExtendedRow ? graphics_.extended : 0;
}

bool Menu::Set(size_t row, size_t value)
{
    if (value >= Count(row) || value == Value(row))
        return false;
    Graphics next = graphics_;
    (row == kResolutionRow ? next.resolution : row == kSmoothingRow ? next.smoothing : row == kHdrRow ? next.hdr : next.extended) = value;
    std::string error;
    if (!Save(path_, next, error))
    {
        notice_ = "NOT SAVED: ";
        for (const char c : error)
            notice_ += char(std::toupper(static_cast<unsigned char>(c)));
        return false;
    }
    graphics_ = next;
    const bool applied = applyLive_ && applyLive_(row, value);
    notice_ = std::string(Label(row)) + " " + ValueLabel(row, value);
    // HDR: the preference is saved; what is active is the running game's to say (applyLive published it). Nothing known: as any row.
    const std::string now = row == kHdrRow && applied ? HdrStatusLine(GetHdrStatus()) : std::string();
    notice_ += !now.empty() ? " SAVED. " + now : applied ? " APPLIED AND SAVED." : " SAVED. RESTART REQUIRED: IT APPLIES THE NEXT TIME THE GAME STARTS.";
    return true;
}

}  // namespace runtime::settings
