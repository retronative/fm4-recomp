// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp: the in-game GRAPHICS submenu's logic (the Pause menu's GRAPHICS row; runtime/game/keyboard_screen.cpp shows it in one
// of FM4's own option screens, beside KEYBOARD CONTROLS). Pure: no SDL, no guest memory, no stop. Four rows, RESOLUTION, EDGE
// SMOOTHING, HDR and EXTENDED VIEW, each a list of values the player steps through. The file is $FM4_RECOMP_HOME/graphics.conf, the one FM4 Recomp.app's
// Graphics panel writes (app/setup/GraphicsSettings.swift owns the format: Serialize and Parse here are its serialized() and parse,
// byte for byte and rule for rule; tests/runtime/test_runtime.cpp and tests/app/test_graphics_settings.swift hold the same texts).
// play.sh reads the file when the game starts; every change is written at once, atomically, and the running game takes it at once
// where it can (Menu's applyLive: edge smoothing with FM4_GRAPHICS_LIVE), otherwise it applies at the next start.
#pragma once
#include <cstddef>
#include <functional>
#include <string>

namespace runtime::settings {

struct Option
{
    const char* file;   // the file's spelling (render_scale.h / post_aa.h own them)
    const char* label;  // the game's style: upper case ASCII
};
constexpr size_t kResolutionCount = 4, kSmoothingCount = 4, kHdrCount = 2, kExtendedCount = 2;
extern const Option kResolutions[kResolutionCount];  // 720p, fhd (the default), qhd, 4k
extern const Option kSmoothings[kSmoothingCount];    // off, soft, on (Standard, the default), strong
extern const Option kHdrs[kHdrCount];                // off (the default), auto (HDR output where it can be shown, runtime/video/hdr_output.h)
constexpr size_t kDefaultResolution = 1, kDefaultSmoothing = 2;  // Full HD with Standard smoothing (owner decision 2026-11-21)
constexpr size_t kDefaultHdr = 0;
extern const Option kExtendeds[kExtendedCount];      // off (the default), on (Extended View, runtime/kernel/extended_view.h)
constexpr size_t kDefaultExtended = 0;

// HDR: the file holds the preference only (Off or Auto). What the running game shows is its own state, kept here for the HDR row's
// description and the line after a change, and written to $FM4_RECOMP_HOME/hdr_status for the Settings panel (app/setup/
// GraphicsSettings.swift HdrStatus reads the same text). `reason`: why the picture is standard range, as a phrase ("HDR is off",
// "this display does not support HDR", "HDR is not available on this system", "restart required"); empty when HDR is active.
struct HdrStatus
{
    bool known = false, active = false;
    std::string reason;
    bool operator==(const HdrStatus& o) const { return known == o.known && active == o.active && reason == o.reason; }
};
void SetHdrStatus(const HdrStatus& status);  // any thread
HdrStatus GetHdrStatus();
// "HDR ACTIVE." or "SDR ACTIVE: <REASON>." (the game's upper case); empty while nothing is known.
std::string HdrStatusLine(const HdrStatus& status);
// The status file's text: "active = hdr\n", or "active = sdr\nreason = <reason>\n"; empty while nothing is known.
std::string HdrStatusText(const HdrStatus& status);
// SetHdrStatus, and when it changed: one log line and the status file beside graphics.conf (nothing without FM4_RECOMP_HOME; a
// failed write is named, never fatal). True when it changed.
bool PublishHdrStatus(const HdrStatus& status);

// Whether Extended View is offered in this run: FM4_EXTENDED_VIEW_OFFERED=1 (play.sh: 0 unless a playtest launcher sets it), read
// once; SetExtendedOffered replaces it (tests).
bool ExtendedOffered();
void SetExtendedOffered(bool offered);

struct Graphics
{
    size_t resolution = kDefaultResolution, smoothing = kDefaultSmoothing, hdr = kDefaultHdr, extended = kDefaultExtended;  // indices into the tables above
    bool operator==(const Graphics& o) const { return resolution == o.resolution && smoothing == o.smoothing && hdr == o.hdr && extended == o.extended; }
};

// The file's text: a comment, internal_resolution and anti_aliasing (a setting a file does not name is its default), and
// `hdr = auto` only when the HDR preference is Auto and `extended_view = on` only when Extended View is on (off is not written: a
// file without them is what it was before the settings existed).
std::string Serialize(const Graphics& g);
// `key = value` lines, # comments and blank lines; each setting at most once, only the spellings above. False with `error`.
bool Parse(const std::string& text, Graphics& out, std::string& error);
// No file (or one that cannot be opened): the defaults. An invalid file: the defaults and why in `error` (play.sh starts with them too).
Graphics Load(const std::string& path, std::string& error);
// Writes `g` to `path` (a temporary file renamed over it); false with `error`.
bool Save(const std::string& path, const Graphics& g, std::string& error);

// $FM4_RECOMP_HOME/graphics.conf, or empty without the variable (a developer run: no GRAPHICS row).
std::string GraphicsPath();
inline bool Available() { return !GraphicsPath().empty(); }

class Menu
{
public:
    static constexpr size_t kResolutionRow = 0, kSmoothingRow = 1, kHdrRow = 2, kExtendedRow = 3, kRows = 4;
    // The rows shown: EXTENDED VIEW only when it is offered (ExtendedOffered; not in 0.2.0, owner decision 2026-11-23). A file's
    // extended_view line is kept either way.
    static size_t Rows();
    // `path`: the file read now and written by every change. `applyLive` (optional): asked after a saved change whether the running
    // game took it at once (row, value); without it, or when it answers false, the change applies at the next start.
    explicit Menu(std::string path, std::function<bool(size_t, size_t)> applyLive = {});

    const Graphics& Current() const { return graphics_; }
    static const char* Label(size_t row);        // "RESOLUTION", "EDGE SMOOTHING", "HDR", "EXTENDED VIEW"
    static const char* Description(size_t row);  // what the row is (HDR: with what is active now, GetHdrStatus; valid until the next call)
    static size_t Count(size_t row);
    static const char* ValueLabel(size_t row, size_t value);
    size_t Value(size_t row) const;
    // The player chose `value` on `row` (out of range or unchanged: nothing, false). Saved at once; true when the value changed.
    bool Set(size_t row, size_t value);
    // The row's default (FULL HD, STANDARD, HDR OFF, EXTENDED VIEW OFF); true when it changed.
    bool Default(size_t row)
    {
        return Set(row, row == kResolutionRow ? kDefaultResolution : row == kSmoothingRow ? kDefaultSmoothing : row == kHdrRow ? kDefaultHdr : kDefaultExtended);
    }
    // The line under the list after a change: applied and saved, or saved and that it applies at the next start, or why it was not
    // saved; for HDR, saved and what is active now (HdrStatusLine); empty before any change.
    const std::string& Notice() const { return notice_; }

private:
    std::string path_;
    std::function<bool(size_t, size_t)> applyLive_;
    Graphics graphics_;
    std::string notice_;
};

}  // namespace runtime::settings
