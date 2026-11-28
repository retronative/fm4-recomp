// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The in-game GRAPHICS submenu's logic (graphics_menu.h). Tested by tests/runtime/test_runtime.cpp.
// Portions copyright (c) 2026, i2kiselev (BSD 3-Clause; FM4 Static Recomp host/src/ui/graphics_page.cpp: the render
// scenario rows' labels and descriptions, kSceneryShadows ... kDepthOfField and kRows). FM4 Recomp changes: the game's upper case,
// shortened, with when each change shows; the file keys are ours.
#include "graphics_menu.h"
#include <algorithm>
#include <atomic>
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
// FM4 Recomp (render scenario settings): the labels are FM4 Static Recomp's (graphics_page.cpp kSceneryShadows ... kCarDirt), the
// file's spellings ours.
const Option kShadows[kShadowsCount] = {{"off", "OFF"}, {"on", "ON"}};
const Option kReflections[kReflectionsCount] = {{"console", "CONSOLE"}, {"every_race", "EVERY RACE"}, {"double", "DOUBLE"}};
const Option kMirrors[kMirrorCount] = {{"console", "CONSOLE"}, {"full", "FULL"}};
const Option kParticles[kParticlesCount] = {{"off", "OFF"}, {"on", "ON"}};
const Option kCrowds[kCrowdsCount] = {{"console", "CONSOLE"}, {"every_race", "EVERY RACE"}};
const Option kBlooms[kBloomCount] = {{"console", "CONSOLE"}, {"full_rate", "FULL RATE"}};
const Option kDetails[kDetailCount] = {{"console", "CONSOLE"}, {"high", "HIGH"}, {"ultra", "ULTRA"}};
const Option kDofs[kDofCount] = {{"on", "ON"}, {"off", "OFF"}};
const Option kMotionBlurs[kMotionBlurCount] = {{"on", "ON"}, {"off", "OFF"}};
const Option kCarDirts[kCarDirtCount] = {{"on", "ON"}, {"off", "OFF"}};

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
namespace {
std::atomic<bool> inGameMenuShown{false};
}  // namespace
void SetInGameMenuShown(bool shown) { inGameMenuShown.store(shown); }
bool InGameMenuShown() { return inGameMenuShown.load(); }
namespace {
int renderOffered = -1, dofOffered = -1, overridesOffered = -1;  // -1: not read yet
bool FlagOn(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && v[0] == '1' && v[1] == '\0';
}
}  // namespace
bool RenderOffered()
{
    if (renderOffered < 0)
        renderOffered = FlagOn("FM4_RENDER_SCENARIO") ? 1 : 0;
    return renderOffered == 1;
}
void SetRenderOffered(bool offered) { renderOffered = offered ? 1 : 0; }
bool DofOffered()
{
    if (dofOffered < 0)
        dofOffered = FlagOn("FM4_DEPTH_OF_FIELD_HOOK") ? 1 : 0;
    return dofOffered == 1;
}
void SetDofOffered(bool offered) { dofOffered = offered ? 1 : 0; }
bool OverridesOffered()
{
    if (overridesOffered < 0)
        overridesOffered = FlagOn("FM4_SHADER_OVERRIDES") ? 1 : 0;
    return overridesOffered == 1;
}
void SetOverridesOffered(bool offered) { overridesOffered = offered ? 1 : 0; }

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

// FM4 Recomp: the rows as a table (the four first rows' behaviour, file text and messages unchanged; the render scenario rows added).
// `noun` names the setting in an unknown-value message (the app's: "resolution" for internal_resolution); `always`: written even at
// its default.
namespace {
struct Row
{
    const char* label;
    const char* key;
    const char* noun;
    const Option* options;
    size_t count, fallback;
    size_t Graphics::*field;
    bool always;
    const char* description;
};
const Row kRowTable[Menu::kRows] = {
    {"RESOLUTION", "internal_resolution", "resolution", kResolutions, kResolutionCount, kDefaultResolution, &Graphics::resolution, true,
     "THE RENDERING RESOLUTION. HIGHER IS SHARPER AND NEEDS MORE GRAPHICS TIME."},
    {"EDGE SMOOTHING", "anti_aliasing", "anti_aliasing", kSmoothings, kSmoothingCount, kDefaultSmoothing, &Graphics::smoothing, true,
     "EXTRA SMOOTHING OF JAGGED EDGES, ON TOP OF THE ANTI-ALIASING OF THE GAME."},
    {"HDR", "hdr", "hdr", kHdrs, kHdrCount, kDefaultHdr, &Graphics::hdr, false, nullptr},  // Description: with what is active now
    {"EXTENDED VIEW", "extended_view", "extended_view", kExtendeds, kExtendedCount, kDefaultExtended, &Graphics::extended, false,
     "RACES FILL A WINDOW OR SCREEN THAT IS NOT 16:9: MORE VIEW AT THE SIDES WHEN IT IS WIDER, MORE ABOVE AND BELOW WHEN IT IS TALLER. THE HUD KEEPS ITS SHAPE."},
    // FM4 Recomp (render scenario settings): the descriptions follow FM4 Static Recomp's (graphics_page.cpp kRows), in the game's
    // upper case, with when each change shows.
    {"SCENERY SHADOWS", "scenery_shadows", "scenery_shadows", kShadows, kShadowsCount, 0, &Graphics::shadows, false,
     "SHADOWS OF CONES, BARRIERS AND OTHER SCENERY IN RACES, WHICH THE GAME DRAWS ONLY IN REPLAYS AND PHOTO MODE. EXPERIMENTAL: A SHADOW "
     "MAY FLICKER NEAR THE EDGE OF THE SCREEN. FROM THE NEXT RACE."},
    {"REFLECTION UPDATES", "reflection_updates", "reflection_updates", kReflections, kReflectionsCount, 0, &Graphics::reflections, false,
     "HOW OFTEN CAR REFLECTIONS ARE REDRAWN. THE CONSOLE FREEZES THEM IN 16-CAR RACES AND SPLIT SCREEN; EVERY RACE REDRAWS THEM THERE "
     "TOO; DOUBLE REDRAWS TWICE AS MANY FACES EACH FRAME."},
    {"MIRROR", "mirror", "mirror", kMirrors, kMirrorCount, 0, &Graphics::mirror, false,
     "FULL DRAWS THE MIRROR EVERY FRAME WITH UP TO 16 CARS AND THE CONES AND BARRIERS, WHERE THE CONSOLE HALVES ITS RATE AND DRAWS 6 "
     "CARS (12- AND 16-CAR RACES)."},
    {"SOFT PARTICLES", "soft_particles", "soft_particles", kParticles, kParticlesCount, 0, &Graphics::particles, false,
     "SMOKE AND DUST FADE WHERE THEY MEET THE GROUND AND CARS INSTEAD OF CUTTING INTO THEM, AS THE GAME DOES IN REPLAYS AND PHOTO MODE."},
    {"CROWDS", "crowds", "crowds", kCrowds, kCrowdsCount, 0, &Graphics::crowds, false,
     "EVERY RACE ADDS THE CROWDS THE CONSOLE LEAVES OUT OF 1- AND 16-CAR RACES. USES MORE GAME MEMORY. FROM THE NEXT RACE."},
    {"BLOOM", "bloom", "bloom", kBlooms, kBloomCount, 0, &Graphics::bloom, false,
     "FULL RATE UPDATES THE GLOW EVERY FRAME, WHERE THE CONSOLE SKIPS EVERY SECOND ONE (16-CAR RACES)."},
    {"LEVEL OF DETAIL", "level_of_detail", "level_of_detail", kDetails, kDetailCount, 0, &Graphics::detail, false,
     "HOW FAR CARS KEEP THEIR DETAILED MODELS. ULTRA ALSO KEEPS THE BEST MODEL FOR YOUR CAR, AT MORE GAME MEMORY PER CAR. FROM THE NEXT RACE."},
    {"DEPTH OF FIELD", "depth_of_field", "depth_of_field", kDofs, kDofCount, 0, &Graphics::dof, false,
     "BLURS WHAT IS OUT OF FOCUS IN THE SHOWROOM, PRE-RACE AND REPLAY CAMERAS AND PHOTO MODE. FROM THE NEXT SCREEN."},
    // FM4 Recomp (shader constant overrides; runtime/video/shader_overrides.h): FM4 Static Recomp's car_dirt.cpp effects.
    {"MOTION BLUR", "motion_blur", "motion_blur", kMotionBlurs, kMotionBlurCount, 0, &Graphics::motionBlur, false,
     "THE SPEED STREAKS OF THE ROAD SURFACE. OFF KEEPS THE ROAD'S GRAIN STILL. AT ONCE."},
    {"CAR DIRT", "car_dirt", "car_dirt", kCarDirts, kCarDirtCount, 0, &Graphics::carDirt, false,
     "THE DIRT THAT BUILDS UP ON CARS DURING A RACE. OFF KEEPS THEM CLEAN. AT ONCE."},
};

// "(off or on)", "(off, soft, on or strong)": the spellings, as the app lists them.
std::string Choices(const Row& r)
{
    std::string out = "(";
    for (size_t i = 0; i < r.count; i++)
        out += std::string(i == 0 ? "" : i + 1 == r.count ? " or " : ", ") + r.options[i].file;
    return out + ")";
}
}  // namespace

std::string Serialize(const Graphics& g)
{
    std::string text = "# FM4 Recomp graphics settings, written by FM4 Recomp's Graphics panel. Delete this file for the defaults.\n";
    for (const Row& r : kRowTable)
    {
        const size_t value = g.*r.field;
        if (r.always || value != r.fallback)
            text += std::string(r.key) + " = " + r.options[value].file + "\n";
    }
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
    bool seen[Menu::kRows] = {};
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
        size_t row = 0;
        while (row < Menu::kRows && key != kRowTable[row].key)
            row++;
        if (row == Menu::kRows)
            return error = where + "unknown setting '" + key + "'", false;
        const Row& r = kRowTable[row];
        if (seen[row])
            return error = where + r.key + " is set twice", false;
        if (!Find(r.options, r.count, value, g.*r.field))
            return error = where + "unknown " + r.noun + " '" + value + "' " + Choices(r), false;
        seen[row] = true;
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
            if (out.is_open())  // only a file this call made: std::remove would also take an empty folder in the way
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

// FM4 Recomp: the render scenario settings the running game applies.
namespace {
std::mutex renderLock;
bool renderKnown = false;
Graphics renderNow;
// Under renderLock: the first ask loads the file.
void LoadRenderOnce()
{
    if (renderKnown)
        return;
    std::string why;
    const std::string path = GraphicsPath();
    renderNow = path.empty() ? Graphics{} : Load(path, why);
    renderKnown = true;
    if (!why.empty())
        std::fprintf(stderr, "[graphics] render settings: %s: the game's own values\n", why.c_str());
}
}  // namespace
void PublishRender(const Graphics& g)
{
    std::lock_guard guard(renderLock);
    renderNow = g, renderKnown = true;
}
void PublishRenderValue(size_t row, size_t value)
{
    if (!Menu::IsRender(row) || value >= Menu::Count(row))
        return;
    std::lock_guard guard(renderLock);
    LoadRenderOnce();
    Menu::Field(renderNow, row) = value;
}
Graphics RenderNow()
{
    std::lock_guard guard(renderLock);
    LoadRenderOnce();
    return renderNow;
}

Menu::Menu(std::string path, const Graphics& inEffect, std::function<bool(size_t, size_t)> applyLive)
    : path_(std::move(path)), applyLive_(std::move(applyLive)), entry_(inEffect), effect_(inEffect)
{
    std::string why;
    saved_ = pending_ = Load(path_, why);  // an unreadable or invalid file: the defaults, as play.sh starts
}

Menu::Menu(std::string path) : path_(std::move(path))
{
    std::string why;
    saved_ = pending_ = entry_ = effect_ = Load(path_, why);
}

size_t& Menu::Field(Graphics& g, size_t row) { return g.*kRowTable[row < kRows ? row : kResolutionRow].field; }
size_t Menu::Field(const Graphics& g, size_t row) { return Field(const_cast<Graphics&>(g), row); }

bool Menu::Shown(size_t row)
{
    if (row == kExtendedRow)
        return ExtendedOffered();
    if (row == kDofRow)
        return DofOffered();
    if (row == kMotionBlurRow || row == kCarDirtRow)
        return OverridesOffered();
    if (IsRender(row))
        return RenderOffered();
    return row < kRows;
}

size_t Menu::Rows()
{
    size_t n = 0;
    for (size_t row = 0; row < kRows; row++)
        n += Shown(row) ? 1 : 0;
    return n;
}

size_t Menu::RowAt(size_t position)
{
    for (size_t row = 0; row < kRows; row++)
        if (Shown(row) && position-- == 0)
            return row;
    return kRows;
}

size_t Menu::DefaultValue(size_t row) { return row < kRows ? kRowTable[row].fallback : 0; }

const char* Menu::Label(size_t row) { return row < kRows ? kRowTable[row].label : ""; }

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
    return row < kRows ? kRowTable[row].description : "";
}

size_t Menu::Count(size_t row) { return row < kRows ? kRowTable[row].count : 0; }

const char* Menu::ValueLabel(size_t row, size_t value) { return value >= Count(row) ? "" : kRowTable[row].options[value].label; }

size_t Menu::Value(size_t row) const { return row < kRows ? Field(pending_, row) : 0; }

bool Menu::Preview(size_t row, size_t value)
{
    if (Field(effect_, row) == value)
        return true;
    if (!applyLive_ || !applyLive_(row, value))
        return false;
    Field(effect_, row) = value;
    return true;
}

bool Menu::Set(size_t row, size_t value)
{
    if (value >= Count(row) || value == Value(row))
        return false;
    Field(pending_, row) = value;
    const bool live = Preview(row, value);
    notice_ = std::string(Label(row)) + " " + ValueLabel(row, value);
    // HDR: what is active is the running game's to say (applyLive published it). Nothing known: as any row.
    const std::string now = row == kHdrRow && live ? HdrStatusLine(GetHdrStatus()) : std::string();
    notice_ += live ? ": PREVIEW, NOT SAVED." + (now.empty() ? std::string() : " " + now) + " A: SAVE. B: DISCARD."
                    : ": NOT SAVED. A: SAVE (IT APPLIES THE NEXT TIME THE GAME STARTS). B: DISCARD.";
    return true;
}

bool Menu::Defaults()
{
    bool changed = false, later = false;
    for (size_t row = 0; row < kRows; row++)
    {
        if (!Shown(row))
            continue;
        const size_t value = DefaultValue(row);
        if (Field(pending_, row) == value)
            continue;
        Field(pending_, row) = value;
        changed = true;
        later = !Preview(row, value) || later;
    }
    notice_ = !changed ? "THESE ARE THE DEFAULTS." : later ? "DEFAULTS: NOT SAVED. A: SAVE (SOME APPLY THE NEXT TIME THE GAME STARTS). B: DISCARD."
                                                           : "DEFAULTS: PREVIEW, NOT SAVED. A: SAVE. B: DISCARD.";
    return changed;
}

bool Menu::Confirm()
{
    if (pending_ == saved_)
    {
        if (!(effect_ == entry_))
            entry_ = effect_;  // a preview undone by hand: nothing to write, and nothing to roll back
        return false;
    }
    std::string error;
    if (!Save(path_, pending_, error))
    {
        notice_ = "NOT SAVED: " + Upper(error) + ". B: DISCARD.";
        return false;
    }
    saved_ = pending_, entry_ = effect_;
    std::string later;
    for (size_t row = 0; row < kRows; row++)
        if (Shown(row) && Field(effect_, row) != Field(saved_, row))
            later += std::string(later.empty() ? "" : ", ") + Label(row);
    notice_ = later.empty() ? "SAVED." : "SAVED. RESTART REQUIRED: " + later + " APPLIES THE NEXT TIME THE GAME STARTS.";
    return true;
}

bool Menu::Discard()
{
    failures_.clear();
    for (size_t row = 0; row < kRows; row++)
    {
        const size_t back = Field(entry_, row);
        if (Field(effect_, row) == back)
            continue;
        if (applyLive_ && applyLive_(row, back))
            Field(effect_, row) = back;
        else
            failures_ += std::string(failures_.empty() ? "" : ", ") + Label(row) + " " + ValueLabel(row, Field(effect_, row));
    }
    pending_ = saved_;
    notice_.clear();
    return failures_.empty();
}

}  // namespace runtime::settings
