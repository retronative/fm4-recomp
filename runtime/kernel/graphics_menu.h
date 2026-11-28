// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp: the in-game GRAPHICS submenu's logic (the Pause menu's GRAPHICS row; runtime/game/keyboard_screen.cpp shows it in one
// of FM4's own option screens, beside KEYBOARD CONTROLS). Pure: no SDL, no guest memory, no stop. Rows RESOLUTION, EDGE
// SMOOTHING, HDR and EXTENDED VIEW, then (FM4 Recomp, the game's render scenario settings, runtime/game/render_scenario.h) SCENERY
// SHADOWS, REFLECTION UPDATES, MIRROR, SOFT PARTICLES, CROWDS, BLOOM, LEVEL OF DETAIL and DEPTH OF FIELD, each a list of values the
// player steps through; a row the run does not offer is not shown (Shown, RowAt). The file is $FM4_RECOMP_HOME/graphics.conf, the one FM4 Recomp.app's
// Graphics panel writes (app/setup/GraphicsSettings.swift owns the format: Serialize and Parse here are its serialized() and parse,
// byte for byte and rule for rule; tests/runtime/test_runtime.cpp and tests/app/test_graphics_settings.swift hold the same texts).
// play.sh reads the file when the game starts. In the menu a change is a preview: the running game takes it at once where it can
// (Menu's applyLive, FM4_GRAPHICS_LIVE), and nothing is written until A saves (atomically). B, or the screen closing any other way,
// discards what is not saved and brings the running game back to what it showed when the menu opened (Discard).
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
constexpr size_t kDefaultResolution = 1, kDefaultSmoothing = 2;  // Full HD with Standard smoothing
constexpr size_t kDefaultHdr = 0;
extern const Option kExtendeds[kExtendedCount];      // off (the default), on (Extended View, runtime/kernel/extended_view.h)
constexpr size_t kDefaultExtended = 0;
// FM4 Recomp (render scenario settings): each list's first value is the game's own (the default; not written to the file).
constexpr size_t kShadowsCount = 2, kReflectionsCount = 3, kMirrorCount = 2, kParticlesCount = 2, kCrowdsCount = 2, kBloomCount = 2,
                 kDetailCount = 3, kDofCount = 2, kMotionBlurCount = 2, kCarDirtCount = 2;
extern const Option kShadows[kShadowsCount];          // off, on
extern const Option kReflections[kReflectionsCount];  // console, every_race, double
extern const Option kMirrors[kMirrorCount];           // console, full
extern const Option kParticles[kParticlesCount];      // off, on
extern const Option kCrowds[kCrowdsCount];            // console, every_race
extern const Option kBlooms[kBloomCount];             // console, full_rate
extern const Option kDetails[kDetailCount];           // console, high, ultra
extern const Option kDofs[kDofCount];                 // on, off
// FM4 Recomp (shader constant overrides, runtime/video/shader_overrides.h): on (the game's own), off.
extern const Option kMotionBlurs[kMotionBlurCount];
extern const Option kCarDirts[kCarDirtCount];

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
// FM4 Recomp: whether the render scenario rows are offered: FM4_RENDER_SCENARIO=1 (their title hook armed; play.sh), and DEPTH OF FIELD:
// FM4_DEPTH_OF_FIELD_HOOK=1. Read once; the setters replace them (tests).
bool RenderOffered();
void SetRenderOffered(bool offered);
bool DofOffered();
void SetDofOffered(bool offered);
// FM4_SHADER_OVERRIDES: the MOTION BLUR and CAR DIRT rows (runtime/video/shader_overrides.h).
bool OverridesOffered();
void SetOverridesOffered(bool offered);

// Whether the in-game GRAPHICS submenu is shown (its previews are not saved): the Settings panel does not open meanwhile, since
// closing it applies the saved file over them. Any thread.
void SetInGameMenuShown(bool shown);
bool InGameMenuShown();

struct Graphics
{
    size_t resolution = kDefaultResolution, smoothing = kDefaultSmoothing, hdr = kDefaultHdr, extended = kDefaultExtended;  // indices into the tables above
    // FM4 Recomp (render scenario settings): indices too, 0 the game's own.
    size_t shadows = 0, reflections = 0, mirror = 0, particles = 0, crowds = 0, bloom = 0, detail = 0, dof = 0, motionBlur = 0, carDirt = 0;
    bool operator==(const Graphics& o) const
    {
        return resolution == o.resolution && smoothing == o.smoothing && hdr == o.hdr && extended == o.extended && shadows == o.shadows &&
               reflections == o.reflections && mirror == o.mirror && particles == o.particles && crowds == o.crowds && bloom == o.bloom &&
               detail == o.detail && dof == o.dof && motionBlur == o.motionBlur && carDirt == o.carDirt;
    }
};

// FM4 Recomp: the render scenario settings the running game applies (runtime/game/render_scenario.cpp reads them every main-loop
// tick and when the game switches depth of field; only the render fields of a Graphics count). The first ask loads graphics.conf (no
// file, no FM4_RECOMP_HOME or an invalid file: the defaults). PublishRender replaces them all (the Settings panel when it closes);
// PublishRenderValue one row's (the GRAPHICS menu's preview and rollback: Menu's applyLive). Any thread.
void PublishRender(const Graphics& g);
void PublishRenderValue(size_t row, size_t value);
Graphics RenderNow();

// The file's text: a comment, internal_resolution and anti_aliasing (a setting a file does not name is its default), and
// `hdr = auto` only when the HDR preference is Auto and `extended_view = on` only when Extended View is on (off is not written: a
// file without them is what it was before the settings existed). FM4 Recomp: then each render scenario setting that is not the
// game's own, in row order (scenery_shadows, reflection_updates, mirror, soft_particles, crowds, bloom, level_of_detail,
// depth_of_field).
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
    static constexpr size_t kResolutionRow = 0, kSmoothingRow = 1, kHdrRow = 2, kExtendedRow = 3;
    // FM4 Recomp (render scenario settings; runtime/game/render_scenario.h).
    static constexpr size_t kShadowsRow = 4, kReflectionsRow = 5, kMirrorRow = 6, kParticlesRow = 7, kCrowdsRow = 8, kBloomRow = 9,
                            kDetailRow = 10, kDofRow = 11, kMotionBlurRow = 12, kCarDirtRow = 13, kRows = 14;
    // The rows shown: EXTENDED VIEW only when it is offered (ExtendedOffered; not in this release), the render scenario rows only with
    // their hooks (RenderOffered, DofOffered). A file's lines are kept either way. Rows() counts the rows shown; RowAt(position) is
    // the row shown at that position of the list (kRows past the end).
    static bool Shown(size_t row);
    static size_t Rows();
    static size_t RowAt(size_t position);
    // True for a render scenario row (applied by the running game at its next tick: render_scenario.cpp).
    static bool IsRender(size_t row) { return row >= kShadowsRow && row < kRows; }
    // The row's default (FULL HD, STANDARD, HDR OFF, EXTENDED VIEW OFF; the game's own for a render scenario row).
    static size_t DefaultValue(size_t row);
    // `path`: the saved settings, read now and written only by Confirm. `inEffect`: what the running game shows now (the entry
    // snapshot: the Pause menu or the Pre-Race screen cannot change graphics before GRAPHICS opens, so it is also what they showed
    // when they opened); Discard brings the game back to it. `applyLive` (optional): makes `value` of `row` take effect now (a
    // preview, or a rollback); false when this run cannot (the value then applies at the next start, once saved).
    Menu(std::string path, const Graphics& inEffect, std::function<bool(size_t, size_t)> applyLive = {});
    // The saved settings are what is in effect (a menu without a running game).
    explicit Menu(std::string path);

    const Graphics& Current() const { return pending_; }  // what the rows show
    const Graphics& Saved() const { return saved_; }
    const Graphics& InEffect() const { return effect_; }
    const Graphics& Entry() const { return entry_; }
    // Something is shown that is not saved, or the running game shows something other than at the entry.
    bool Dirty() const { return !(pending_ == saved_) || !(effect_ == entry_); }
    static const char* Label(size_t row);        // "RESOLUTION", "EDGE SMOOTHING", "HDR", "EXTENDED VIEW", "SCENERY SHADOWS", ...
    static const char* Description(size_t row);  // what the row is (HDR: with what is active now, GetHdrStatus; valid until the next call)
    static size_t Count(size_t row);
    static const char* ValueLabel(size_t row, size_t value);
    size_t Value(size_t row) const;
    // The player chose `value` on `row` (out of range or unchanged: nothing, false): shown, previewed where the game can take it
    // now, not saved. True when the value changed.
    bool Set(size_t row, size_t value);
    // X: every row shown to its default (FULL HD, STANDARD, HDR OFF; EXTENDED VIEW OFF only when it is offered; each render scenario
    // row offered to the game's own), previewed as Set, not saved. True when a row changed.
    bool Defaults();
    // A: what the rows show is saved; on success it is the new baseline (a later Discard goes back to it). False when nothing
    // needed saving or the file could not be written (the rows and the preview stay; Notice says why).
    bool Confirm();
    // B, or the screen closing: what is not saved is dropped (the rows show the saved settings) and every row whose effect moved
    // since the entry (or the last Confirm) is set back to it with applyLive. False when one could not be (it stays as previewed
    // until the next start; Failures names those rows, as "RESOLUTION QHD").
    bool Discard();
    const std::string& Failures() const { return failures_; }
    // The line under the list after a change: previewed (with A: SAVE, B: DISCARD) or that it applies once saved and restarted;
    // saved; or why it was not saved. For HDR, also what is active now (HdrStatusLine). Empty before any change.
    const std::string& Notice() const { return notice_; }

    // The member of `g` that holds `row` (also PublishRenderValue's).
    static size_t& Field(Graphics& g, size_t row);
    static size_t Field(const Graphics& g, size_t row);

private:
    bool Preview(size_t row, size_t value);  // applyLive for `value` unless already in effect; true when it is in effect now
    std::string path_;
    std::function<bool(size_t, size_t)> applyLive_;
    Graphics saved_, entry_, effect_, pending_;
    std::string notice_, failures_;
};

}  // namespace runtime::settings
