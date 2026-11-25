// Portions copyright (c) 2026, Pinyon Shift contributors (BSD 3-Clause: see THIRD_PARTY_NOTICES.md).
// Copyright (c) 2026, i2kiselev (BSD 3-Clause: see LICENSE in the repository root).
#include "ui/graphics_page.h"

#include "game_hooks.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/upscaler.h>

#if defined(__APPLE__)
#include <dlfcn.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fm4_static_recomp::ui {
#if defined(__APPLE__)
uint32_t CurrentMacWindowRefreshHz();
#endif
namespace {

using hostui::MenuRow;
using hostui::MenuScreen;

struct Choice {
  const char* label;
  const char* value;
};

struct Setting {
  const char* name;
  bool string;  // written quoted
  std::vector<Choice> choices;  // none for a whole number in [low, high]
  int low = 0;
  int high = 0;
};

const Setting kFrameRate = {"fm4_frame_rate", false, {}, 60, 240};
const Setting kVsync = {"vulkan_present_mode", true, {{"OFF", "immediate"}, {"ON", "fifo"}}};
const Setting kDetail = {"fm4_level_of_detail",
                         true,
                         {{"CONSOLE", "console"},
                          {"HIGH", "high"},
                          {"ULTRA", "ultra"}}};
// Values 1–3 are existing profile scales. The Full HD profile choice has its
// own identifier because its render dimensions sit between 1x and 2x.
const Setting kScale = {"resolution_scale", false,
                        {{"720p", "1"},
#if defined(__APPLE__)
                         {"1080p (Full HD)", "5"},
#endif
                         {"1440p (QHD)", "2"}, {"4K (2160p)", "3"},
#if !defined(__APPLE__)
                         {"5K (2880p)", "4"},
#endif
                        }};
const Setting kExtendedView = {"fm4_extended_view", false, {{"OFF", "false"}, {"ON", "true"}}};
const Setting kHdr = {"present_hdr", false, {{"OFF", "false"}, {"AUTO", "true"}}};
const Setting kDisplayMode = {"fullscreen", false, {{"WINDOWED", "false"}, {"FULLSCREEN", "true"}}};
const Setting kUpscaler = {"present_effect",
                           true,
                           {{"OFF", "bilinear"},
                            {"FSR 1", "fsr"},
                            {"FSR 2", "fsr2"},
                            {"FSR 3", "fsr3"},
                            {"FSR 4", "fsr4"},
                            {"DLSS", "dlss"}}};
const Setting kDlssPreset = {
    "present_dlss_preset",
    true,
    {{"AUTO", "auto"}, {"K", "k"}, {"M", "m"}, {"J", "j"}, {"L", "l"}, {"DEFAULT", "default"}}};
const Setting kSharpness = {"present_upscaler_sharpness",
                            false,
                            {{"OFF", "0"},
                             {"10 %", "10"},
                             {"20 %", "20"},
                             {"30 %", "30"},
                             {"40 %", "40"},
                             {"50 %", "50"},
                             {"60 %", "60"},
                             {"70 %", "70"},
                             {"80 %", "80"},
                             {"90 %", "90"},
                             {"100 %", "100"}}};
const Setting kPostAa = {
    "swap_post_effect", true, {{"OFF", "none"}, {"FXAA", "fxaa"}, {"FXAA EXTREME", "fxaa_extreme"}}};
const Setting kShowroomMsaa = {
    "fm4_showroom_msaa", true, {{"4X", "4x"}, {"2X", "2x"}, {"OFF", "off"}}};
const Setting kRaceMsaa = {"fm4_race_msaa", true, {{"4X", "4x"}, {"2X", "2x"}, {"OFF", "off"}}};
const Setting kAnisotropy = {"anisotropic_override",
                             false,
                             {{"GAME", "-1"}, {"2X", "2"}, {"4X", "3"}, {"8X", "4"}, {"16X", "5"}}};

const Setting kMotionBlur = {"fm4_road_detail_blur", false, {{"ON", "true"}, {"OFF", "false"}}};
const Setting kDepthOfField = {"fm4_depth_of_field", false, {{"ON", "true"}, {"OFF", "false"}}};
const Setting kCarDirt = {"fm4_car_dirt", false, {{"ON", "true"}, {"OFF", "false"}}};

const Setting kSceneryShadows = {"fm4_scenery_shadows", false, {{"OFF", "false"}, {"ON", "true"}}};
const Setting kReflectionUpdates = {
    "fm4_reflection_updates",
    true,
    {{"CONSOLE", "console"}, {"EVERY RACE", "every_race"}, {"DOUBLE", "double"}}};
const Setting kMirror = {"fm4_full_rate_mirror", false, {{"CONSOLE", "false"}, {"FULL", "true"}}};
const Setting kBloom = {"fm4_full_rate_bloom", false, {{"CONSOLE", "false"}, {"FULL RATE", "true"}}};
const Setting kSoftParticles = {"fm4_soft_particles", false, {{"OFF", "false"}, {"ON", "true"}}};
const Setting kCrowds = {
    "fm4_crowds_every_race", false, {{"CONSOLE", "false"}, {"EVERY RACE", "true"}}};

// Every setting but the frame rate, the resolution scale and the upscaler's.
const Setting* const kPresetSettings[] = {
    &kPostAa, &kShowroomMsaa, &kRaceMsaa, &kAnisotropy, &kSceneryShadows, &kReflectionUpdates,
    &kDetail, &kMirror, &kBloom, &kSoftParticles, &kCrowds, &kMotionBlur, &kDepthOfField,
    &kCarDirt};
constexpr size_t kPresetSettingCount = std::size(kPresetSettings);

const Setting* const kSettings[] = {
    &kFrameRate, &kVsync, &kScale, &kExtendedView, &kUpscaler, &kDlssPreset, &kSharpness, &kPostAa,
    &kShowroomMsaa, &kRaceMsaa, &kAnisotropy,
    &kSceneryShadows, &kReflectionUpdates, &kDetail, &kMirror, &kBloom, &kSoftParticles, &kCrowds,
    &kMotionBlur, &kDepthOfField, &kCarDirt,
#if defined(__APPLE__)
    &kHdr, &kDisplayMode,
#endif
};

struct Preset {
  const char* label;
  std::array<const char*, kPresetSettingCount> values;  // in kPresetSettings' order
};

// The page shows the preset all settings match, or CUSTOM.
const Preset kPresets[] = {
    {"CONSOLE",
     {"none", "4x", "2x", "-1", "false", "console", "console", "false", "false", "false",
      "false", "true", "true", "true"}},
    {"HIGH",
     {"fxaa", "2x", "2x", "5", "false", "console", "high", "false", "true", "false",
      "false", "true", "true", "true"}},
    {"ULTRA",
     {"fxaa_extreme", "2x", "2x", "5", "false", "every_race", "ultra", "true", "true",
      "true", "false", "true", "true", "true"}},
    {"EXPERIMENTAL",
     {"fxaa_extreme", "4x", "4x", "5", "true", "double", "ultra", "true", "true", "true",
      "true", "true", "true", "true"}},
};
constexpr int kPresetCount = int(std::size(kPresets));

struct RowText {
  const char* label;
  const Setting* setting;  // null for the preset row
  const char* description;
  bool divider_above = false;
  // The launcher shows the row only while this setting has this value.
  const Setting* shown_with = nullptr;
  const char* shown_value = nullptr;
};

// The page's rows in order; the frame rate, scale, upscaler and preset rows
// add behaviour of their own in CreateGraphicsPage. The DLSS preset is
// the launcher's alone: DLSS takes it only as it starts.
const RowText kRows[] = {
    {"DISPLAY MODE", &kDisplayMode, "Use the full display or play in a window."},
    {"HDR", &kHdr, "Auto uses HDR when your display has highlight headroom. Off uses standard brightness."},
    {"FRAME RATE", &kFrameRate,
     "Target frames per second in races, menus and the showroom. Lower the target or the "
     "resolution if the picture stutters."},
    {"VSYNC", &kVsync,
     "On stops tearing on a display without G-Sync or FreeSync. With either, leave it off: "
     "on, an Nvidia GPU shows frames through the desktop, without variable refresh and at a "
     "lower frame rate."},
    {"RESOLUTION", &kScale,
     "Choose the resolution the game renders at. Higher resolutions give sharper edges and use more GPU time."},
    {"EXTENDED VIEW", &kExtendedView,
     "Show more of the track at the sides of a wide window or above and below in a tall window. "
     "Applies when you save."},
    {"UPSCALER", &kUpscaler,
     "Upscales the picture from the selected resolution to your display. DLSS (NVIDIA RTX GPUs), "
     "FSR 4 (AMD Radeon RX 6000 or later and recent Ryzen APUs), FSR 2 and FSR 3 (any GPU) use "
     "the game's depth and motion and smooth edges themselves: "
     "while one runs, MSAA and post AA are off, the MSAA change from the next screen. FSR 1 "
     "(any GPU, light) upscales and sharpens the finished picture, after MSAA and post AA. Off "
     "stretches the picture."},
    {"DLSS PRESET", &kDlssPreset,
     "The DLSS model. Auto takes M where DLSS upscales by more than 1.8x on an RTX 40 or later "
     "GPU, else K; Default leaves the choice to the DLSS runtime and the NVIDIA app's override.",
     false, &kUpscaler, "dlss"},
    {"SHARPNESS", &kSharpness,
     "Sharpens the picture after the upscaler. Off leaves the picture of DLSS, FSR 2, FSR 3 "
     "or FSR 4 as it is; FSR 1 always sharpens a little."},
    {"GRAPHICS PRESET", nullptr,
     "Sets every setting below. Console is the game as on the console; "
     "High raises most, adds FXAA and uses 2x MSAA everywhere; Ultra also raises the level of "
     "detail, the mirror and particles and uses FXAA Extreme; Experimental adds 4x MSAA "
     "everywhere, scenery shadows, double reflection updates and crowds in every race. A "
     "change to one setting makes the preset Custom."},
    {"SHOWROOM MSAA", &kShowroomMsaa,
     "The game's multisampling in the showroom, menus, pre-race, replays and photo mode. 4x is "
     "the console's and draws the scene in three tiles, 2x in two, Off in one: fewer samples "
     "are faster and leave edges to the selected resolution. Applies from the next screen. Off "
     "while DLSS or FSR 2 / 3 upscales: the upscaler smooths edges itself.",
     true},
    {"RACE MSAA", &kRaceMsaa,
     "The game's multisampling in races, the countdown and the results. 2x is the console's "
     "(4x in races alone on track); 4x has twice the samples and three tiles instead of two, "
     "heavy on the GPU; Off draws one tile and leaves edges to the selected resolution. Applies "
     "from the next race. Off while DLSS or FSR 2 / 3 upscales."},
    {"ANISOTROPIC FILTERING", &kAnisotropy,
     "Keeps textures sharp at grazing angles. Game leaves each texture the filtering the game "
     "asks for."},
    {"LEVEL OF DETAIL", &kDetail,
     "How far cars keep their detailed models. Ultra also keeps the best model loaded for your "
     "car, at more guest memory per car. Applies from the next race."},
    {"SCENERY SHADOWS", &kSceneryShadows,
     "Experimental, so in no other preset: shadows of the cones, barriers and other scenery in "
     "races, which the game draws only in replays and photo mode. Known bug: a shadow vanishes "
     "or flickers as its object nears the edge of the screen, most visibly on large objects."},
    {"REFLECTION UPDATES", &kReflectionUpdates,
     "How often car reflections are redrawn. The console leaves them frozen in 16-car races "
     "and split screen; Every Race redraws them there too; Double redraws twice as many "
     "faces each frame."},
    {"MIRROR", &kMirror,
     "Full draws the mirror every frame with up to 16 cars and the cones and barriers, where "
     "the console halves its rate and draws 6 cars (12- and 16-car races)."},
    {"SOFT PARTICLES", &kSoftParticles,
     "Smoke and dust fade where they meet the ground and cars instead of cutting into them, "
     "as the game does in replays and photo mode."},
    {"CROWDS", &kCrowds,
     "Every Race adds the crowds the console leaves out of 1- and 16-car races. Uses more "
     "guest memory."},
    {"POST AA", &kPostAa,
     "Smooths edges in the finished picture. FXAA Extreme smooths more and softens more. Off "
     "while DLSS or FSR 2 / 3 upscales: the upscaler smooths edges itself.",
     true},
    {"BLOOM", &kBloom,
     "Full Rate updates the glow every frame, where the console skips every second one "
     "(16-car races)."},
    {"MOTION BLUR", &kMotionBlur,
     "Blurs the road surface with speed. Off keeps the asphalt as sharp as when standing "
     "still."},
    {"DEPTH OF FIELD", &kDepthOfField,
     "Blurs what is out of focus in the showroom, pre-race and replay cameras and photo mode. "
     "Applies from the next screen."},
    {"CAR DIRT", &kCarDirt,
     "The dirt cars gather in a race. Off leaves cars clean."},
};

const RowText& Text(const char* label) {
  for (const RowText& row : kRows) {
    if (std::string_view(row.label) == label) {
      return row;
    }
  }
  std::abort();
}

std::string JsonString(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      default:
        out += c;
    }
  }
  return out + "\"";
}

std::string EffectiveDefault(const Setting& setting) {
#if defined(__APPLE__)
  // The macOS player starts with native Full HD and standard FXAA.
  if (&setting == &kScale) return "5";
  if (&setting == &kPostAa) return "fxaa";
  if (&setting == &kHdr) return "true";
#endif
  // A built-in default (built_in_defaults.cpp), over the SDK's auto.
  if (&setting == &kVsync) {
    return "immediate";
  }
  const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(setting.name);
  return info ? info->default_value : std::string();
}

std::string RowJson(const RowText& row) {
  std::string out = "{\"label\":" + JsonString(row.label);
  if (!row.setting) {
    return out + ",\"kind\":\"preset\",\"description\":" + JsonString(row.description) + "}";
  }
  const Setting& setting = *row.setting;
  out += ",\"key\":" + JsonString(setting.name);
  out += std::string(",\"string\":") + (setting.string ? "true" : "false");
  if (setting.choices.empty()) {
    out += ",\"kind\":\"range\",\"low\":" + std::to_string(setting.low) +
           ",\"high\":" + std::to_string(setting.high);
  } else {
    out += ",\"kind\":\"choice\",\"choices\":[";
    for (size_t i = 0; i < setting.choices.size(); ++i) {
      out += std::string(i ? "," : "") + "{\"label\":" + JsonString(setting.choices[i].label) +
             ",\"value\":" + JsonString(setting.choices[i].value) + "}";
    }
    out += "]";
  }
  out += ",\"default\":" + JsonString(EffectiveDefault(setting));
  if (row.shown_with) {
    out += ",\"shown_when\":{" + JsonString(row.shown_with->name) + ":" +
           JsonString(row.shown_value) + "}";
  }
  return out + ",\"description\":" + JsonString(row.description) + "}";
}

// SAVE & APPLY sets the drafted settings one per frame. After a setting that
// rebuilds the renderer or the presenter, the next waits until it is done:
// the resolution scale until the renderer draws at it (at most
// kRebuildTimeout), the upscaler and vsync for kPresenterSettle. A scale the
// driver's pipeline cache already holds switches in about 2 s; one not used
// before builds all its pipelines first (11-12 s on an RTX 4070 Ti), which
// the page says once the switch outlasts kShaderBuildShown.
constexpr auto kPresenterSettle = std::chrono::milliseconds(500);
constexpr auto kRebuildTimeout = std::chrono::seconds(60);
constexpr auto kShaderBuildShown = std::chrono::seconds(3);

#if defined(__APPLE__)
constexpr char kNativeFhd[] = "fm4_native_fhd";
#endif

std::string Live(const char* name) {
#if defined(__APPLE__)
  if (std::string_view(name) == kScale.name && rex::cvar::GetFlagByName(kNativeFhd) == "true") {
    return "5";
  }
#endif
  return rex::cvar::GetFlagByName(name);
}

// The profile selection is separate from the SDK's integer render-target scale.
std::string_view PhysicalScale(std::string_view selection) {
#if defined(__APPLE__)
  if (selection == "5") return "2";
#endif
  return selection;
}

bool SetRegisteredValue(const Setting& setting, const std::string& value) {
#if defined(__APPLE__)
  if (&setting == &kScale) {
    const bool has_fhd = !rex::cvar::GetFlagByName(kNativeFhd).empty();
    if (value == "5" && !has_fhd) return false;
    if (!rex::cvar::SetFlagByName(kScale.name, PhysicalScale(value))) return false;
    return !has_fhd || rex::cvar::SetFlagByName(kNativeFhd, value == "5" ? "true" : "false");
  }
#endif
  return rex::cvar::SetFlagByName(setting.name, value);
}

int LiveNumber(const char* name) { return std::atoi(Live(name).c_str()); }

// The highest frame rate the page offers: the display's refresh rate, within
// the setting's range.
int FrameRateCeiling(uint32_t refresh_hz) {
  return refresh_hz ? std::clamp(int(refresh_hz), kFrameRate.low, kFrameRate.high)
                    : kFrameRate.high;
}

int Find(const Setting& setting, std::string_view value) {
  if (setting.choices.empty()) {
    const int number = std::atoi(std::string(value).c_str());
    return std::to_string(number) == value && number >= setting.low && number <= setting.high
               ? 0
               : -1;
  }
  for (size_t i = 0; i < setting.choices.size(); ++i) {
    if (value == setting.choices[i].value) {
      return int(i);
    }
  }
  return -1;
}

int Step(const Setting& setting, int current, int direction) {
  const int count = int(setting.choices.size());
  return current < 0 ? 0 : (current + direction + count) % count;
}

std::string Label(const Setting& setting, int index) {
  return index >= 0 ? std::string(setting.choices[size_t(index)].label) : std::string("CUSTOM");
}

bool FromCommandLine(const char* name) {
  return rex::cvar::GetFlagSource(name) == rex::cvar::Source::kCommandLine;
}

// The per-axis scales override resolution_scale when set.
bool SetByLaunchOption(const Setting& setting) {
  if (&setting == &kScale) {
#if defined(__APPLE__)
    if (FromCommandLine(kNativeFhd)) return true;
#endif
    return FromCommandLine(kScale.name) || FromCommandLine("draw_resolution_scale_x") ||
           FromCommandLine("draw_resolution_scale_y");
  }
  return FromCommandLine(setting.name);
}

// The page's changes: drafted by the rows, then saved and applied together.
struct Draft {
  std::function<bool()> is_fullscreen;
  std::string Current(const Setting& setting) const {
    if (&setting == &kDisplayMode && is_fullscreen) return is_fullscreen() ? "true" : "false";
    return Live(setting.name);
  }

  // The changed settings' values; a setting changed back is no longer here.
  std::map<const Setting*, std::string> values;
  // Applying: the settings still to set, in order, and the one set last while
  // the renderer or presenter takes it in.
  std::vector<const Setting*> queue;
  const Setting* settling = nullptr;
  std::chrono::steady_clock::time_point set_at;
  // Leaving was refused once over these changes; the next leave discards them.
  bool leave_warned = false;
  // Applied, with nothing changed since.
  bool applied = false;
  bool save_failed = false;
  uint32_t loading_resolution = 0;
  uint64_t frame_before_resolution = 0;
  bool resolution_unconfirmed = false;
  // The scale the UPSCALER row lowered the drafted scale to, until the next
  // change; 0 when it did not.
  uint32_t lowered_scale = 0;

  bool busy() const { return !queue.empty() || settling; }

  std::string Value(const Setting& setting) const {
    const auto it = values.find(&setting);
    return it != values.end() ? it->second : Current(setting);
  }

  void Change(const Setting& setting, std::string value) {
    if (value == Current(setting)) {
      values.erase(&setting);
    } else {
      values[&setting] = std::move(value);
    }
    leave_warned = false;
    applied = false;
    save_failed = false;
    lowered_scale = 0;
  }
};

uint32_t LiveScale(const GraphicsPageServices& services) {
#if defined(__APPLE__)
  // The optional renderer publishes completion separately from the requested mode.
  static const auto fhd_is_active =
      reinterpret_cast<bool (*)()>(dlsym(RTLD_DEFAULT, "fm4_native_fhd_is_active"));
  if (fhd_is_active && fhd_is_active()) return 5;
#endif
  uint32_t scale = services.draw_resolution_scale ? services.draw_resolution_scale() : 0;
  if (!scale) {
    scale = uint32_t(std::max(1, std::atoi(Live(kScale.name).c_str())));
  }
  return scale;
}

// Why the upscaler `value` names cannot run on this GPU, or nullopt when it can.
std::optional<std::string> UpscalerProblem(const GraphicsPageServices& services,
                                           std::string_view value) {
  if (value == "bilinear" || !services.upscalers) {
    return std::nullopt;
  }
  for (const rex::ui::UpscalerInfo& info : services.upscalers()) {
    if (info.id == value) {
      return info.available ? std::nullopt : std::optional(info.unavailable_reason);
    }
  }
  return std::string("its runtime is missing");
}

uint32_t DraftedScale(const Draft& draft, const GraphicsPageServices& services) {
  return draft.values.contains(&kScale) ? uint32_t(std::atoi(draft.Value(kScale).c_str()))
                                        : LiveScale(services);
}

uint32_t RenderHeight(uint32_t selection) {
#if defined(__APPLE__)
  if (selection == 5) {
    return 1080;
  }
#endif
  return 720 * selection;
}

// Choices are ordered by dimensions. Their persisted IDs need not be.
std::optional<uint32_t> LargestFittingScale(const GraphicsPageServices& services) {
  const auto output = services.output_size ? services.output_size() : std::nullopt;
  if (!output || !output->first || !output->second) {
    return std::nullopt;
  }
  for (auto it = kScale.choices.rbegin(); it != kScale.choices.rend(); ++it) {
    const uint32_t selection = uint32_t(std::atoi(it->value));
    const uint32_t height = RenderHeight(selection);
    if (height <= output->second && height * 16 / 9 <= output->first) {
      return selection;
    }
  }
  return 0;
}

bool RenderFits(const Draft& draft, const GraphicsPageServices& services) {
  const auto largest = LargestFittingScale(services);
  return !largest || RenderHeight(DraftedScale(draft, services)) <= RenderHeight(*largest);
}

// Whether the drafted upscaler is one this GPU runs.
bool UpscalerChosen(const Draft& draft, const GraphicsPageServices& services) {
  const std::string effect = draft.Value(kUpscaler);
  return effect != "bilinear" && !UpscalerProblem(services, effect);
}

std::string ScreenSize(const GraphicsPageServices& services) {
  const auto [width, height] = *services.output_size();
  return std::to_string(width) + "x" + std::to_string(height);
}

std::string ResolutionName(uint32_t scale) {
  const int index = Find(kScale, std::to_string(scale));
  return index >= 0 ? kScale.choices[size_t(index)].label : std::to_string(720ull * scale) + "p";
}

// The chosen upscaler, when it runs on this GPU but not at the drafted scale,
// which renders larger than the output; empty otherwise.
std::string IdleUpscaler(const Draft& draft, const GraphicsPageServices& services) {
  return !UpscalerChosen(draft, services) || RenderFits(draft, services)
             ? std::string()
             : Label(kUpscaler, Find(kUpscaler, draft.Value(kUpscaler)));
}

// Why the chosen upscaler does not run at the drafted scale, or empty.
std::string IdleUpscalerNote(const Draft& draft, const GraphicsPageServices& services) {
  const std::string upscaler = IdleUpscaler(draft, services);
  if (upscaler.empty()) {
    return upscaler;
  }
  const uint32_t largest = *LargestFittingScale(services);
  const std::string screen = ScreenSize(services);
  return (largest ? upscaler + " runs up to " + ResolutionName(largest) + " on this " + screen
                  : upscaler + " does not run on this " + screen) +
         " screen. At " + ResolutionName(DraftedScale(draft, services)) +
         " the picture is downscaled without it" +
         (upscaler == "FSR 1" ? "." : ", with MSAA and post AA instead.");
}

// The upscaler that is upscaling, or will once the drafted upscaler or scale
// is applied; empty for none.
std::string UpscalerName(const Draft& draft, const GraphicsPageServices& services) {
  if (!draft.values.contains(&kUpscaler) && !draft.values.contains(&kScale)) {
    const std::string effect = Live("present_effect");
    if (effect == "fsr") {
      return RenderFits(draft, services) ? "FSR 1" : std::string();
    }
    if (!rex::ui::IsTemporalUpscalerActive()) {
      return std::string();
    }
    return effect == "dlss"   ? "DLSS"
           : effect == "fsr4" ? "FSR 4"
           : effect == "fsr3" ? "FSR 3"
                              : "FSR 2";
  }
  return !UpscalerChosen(draft, services) || !RenderFits(draft, services)
             ? std::string()
             : Label(kUpscaler, Find(kUpscaler, draft.Value(kUpscaler)));
}

// The upscaler that takes over the anti-aliasing; FSR 1 works on the finished
// picture and leaves it to MSAA and post AA.
std::string TemporalUpscalerName(const Draft& draft, const GraphicsPageServices& services) {
  const std::string upscaler = UpscalerName(draft, services);
  return upscaler == "FSR 1" ? std::string() : upscaler;
}

std::string ResolutionLine(const Draft& draft, const GraphicsPageServices& services) {
  const uint32_t scale = DraftedScale(draft, services);
  const uint32_t render_height = RenderHeight(scale);
  const uint32_t render_width = render_height * 16 / 9;
  const auto size = [](uint32_t width, uint32_t height) {
    return std::to_string(width) + "x" + std::to_string(height);
  };
  std::string line = "Renders " + size(render_width, render_height);
  const auto output = services.output_size ? services.output_size() : std::nullopt;
  if (!output || !output->first || !output->second) {
    return line;
  }
  const auto [output_width, output_height] = *output;
  const std::string upscaler = UpscalerName(draft, services);
  if (output_width == render_width && output_height == render_height) {
    // At 1:1 the upscalers only anti-alias, under the names NVIDIA and AMD give that mode;
    // FSR 1 only sharpens.
    if (upscaler.empty()) {
      return line + ", shown at that size.";
    }
    if (upscaler == "FSR 1") {
      return line + ", shown at that size, sharpened by FSR 1.";
    }
    return line + ", shown at that size with " + (upscaler == "DLSS" ? "DLAA" : upscaler + " Native AA") + ".";
  }
  if (output_width * output_height > render_width * render_height) {
    if (upscaler.empty()) {
      return line + ", stretched to " + size(output_width, output_height) + ".";
    }
    return line + ", upscaled to " + size(output_width, output_height) + " by " + upscaler + ".";
  }
  return line + ", downscaled to " + size(output_width, output_height) + ".";
}

// FSR 1's sharpening reads its own cvar, in stops below the sharpest: SHARPNESS
// sets it as the SDK derives the temporal upscalers' (GetRcasSharpnessReduction).
void SetFsr1Sharpness() {
  constexpr const char* kReduction = "present_fsr_sharpness_reduction";
  if (FromCommandLine(kReduction)) {
    return;
  }
  const double stops = 2.0 * (1.0 - LiveNumber(kSharpness.name) / 100.0);
  rex::cvar::SetFlagByName(kReduction, std::to_string(stops));
}

void SetLive(const Setting& setting, const std::string& value) {
  if (!SetRegisteredValue(setting, value)) {
    REXLOG_ERROR("Graphics page: {} refused {}", setting.name, value);
    return;
  }
  REXLOG_INFO("Graphics page: {} = {}", setting.name, value);
  if (&setting == &kSharpness) {
    SetFsr1Sharpness();
  }
  if (&setting == &kScale) {
    // The per-axis scales win over resolution_scale while either is set.
    for (const char* axis : {"draw_resolution_scale_x", "draw_resolution_scale_y"}) {
      if (rex::cvar::HasNonDefaultValue(axis)) {
        rex::cvar::SetFlagByName(axis, PhysicalScale(value));
      }
    }
  }
}

// Saves every drafted value, then queues them: the settings that rebuild
// something first, each alone.
void SaveAndApply(Draft& draft, config::HostConfig& config, const GraphicsPageServices& services) {
  if (draft.busy() || draft.values.empty()) {
    return;
  }
  auto pending = config;
  const bool loaded = pending.Load();
  if (loaded) {
    for (const auto& [setting, value] : draft.values) {
      pending.Set(setting->name, setting->string ? config::Quote(value) : value);
    }
  }
  if (!loaded || !pending.Save()) {
    REXLOG_ERROR("Graphics page: could not write {}", config.path().string());
    draft.save_failed = true;
    return;
  }
  config = std::move(pending);
  draft.save_failed = false;
  if (draft.values.contains(&kScale) && services.presented_guest_frame &&
      services.resolution_loading) {
    const auto& value = draft.values.at(&kScale);
    const auto target = uint32_t(std::atoi(value.c_str()));
    draft.resolution_unconfirmed = false;
    if (target != LiveScale(services)) {
      draft.loading_resolution = target;
      services.resolution_loading(true, Label(kScale, Find(kScale, value)));
    }
  }
  for (const Setting* setting : {&kScale, &kUpscaler, &kVsync}) {
    if (draft.values.contains(setting)) {
      draft.queue.push_back(setting);
    }
  }
  for (const Setting* setting : kSettings) {
    if (draft.values.contains(setting) &&
        std::find(draft.queue.begin(), draft.queue.end(), setting) == draft.queue.end()) {
      draft.queue.push_back(setting);
    }
  }
  REXLOG_INFO("Graphics page: applying {} settings", draft.queue.size());
  draft.leave_warned = false;
  draft.applied = true;
}

// Whether the setting set last has taken effect, so the next may be set.
bool Settled(Draft& draft, const GraphicsPageServices& services,
             std::chrono::steady_clock::time_point now) {
  const auto elapsed = now - draft.set_at;
  if (draft.settling != &kScale) {
    return elapsed >= kPresenterSettle;
  }
  bool has_frame = true;
  if (draft.loading_resolution) {
    const auto frame = services.presented_guest_frame();
    const auto height = RenderHeight(draft.loading_resolution);
    has_frame = frame.number != 0 && frame.number != draft.frame_before_resolution &&
                frame.height == height && frame.width == height * 16 / 9;
  }
  if (has_frame && (!services.draw_resolution_scale ||
                    std::to_string(LiveScale(services)) == Live(kScale.name))) {
    return true;
  }
  if (elapsed >= kRebuildTimeout) {
    draft.resolution_unconfirmed = true;
    REXLOG_WARN("Graphics page: the renderer did not take {} = {} within {} s", kScale.name,
                Live(kScale.name), kRebuildTimeout.count());
    return true;
  }
  return false;
}

// Sets the next queued setting once the one before has settled.
void ApplyNext(Draft& draft, const GraphicsPageServices& services) {
  const auto now = std::chrono::steady_clock::now();
  if (draft.settling) {
    if (!Settled(draft, services, now)) {
      return;
    }
    if (draft.settling == &kScale && draft.loading_resolution) {
      services.resolution_loading(false, {});
      draft.loading_resolution = 0;
    }
    draft.settling = nullptr;
    if (draft.queue.empty()) {
      if (!draft.resolution_unconfirmed) REXLOG_INFO("Graphics page: all settings applied");
    }
  }
  if (draft.queue.empty()) {
    return;
  }
  const Setting* setting = draft.queue.front();
  draft.queue.erase(draft.queue.begin());
  if (setting == &kScale && draft.loading_resolution) {
    draft.frame_before_resolution = services.presented_guest_frame().number;
  }
  const auto& value = draft.values.at(setting);
  SetLive(*setting, value);
  if (setting == &kDisplayMode && services.set_fullscreen &&
      services.is_fullscreen && services.is_fullscreen() != (value == "true")) {
    services.set_fullscreen(value == "true");
  }
  draft.values.erase(setting);
  if (setting == &kScale || setting == &kUpscaler || setting == &kVsync) {
    draft.settling = setting;
    draft.set_at = now;
  } else if (draft.queue.empty() && !draft.resolution_unconfirmed) {
    REXLOG_INFO("Graphics page: all settings applied");
  }
}

int DraftedPreset(const Draft& draft) {
  for (int i = 0; i < kPresetCount; ++i) {
    bool match = true;
    for (size_t j = 0; j < kPresetSettingCount && match; ++j) {
      match = draft.Value(*kPresetSettings[j]) == kPresets[i].values[j];
    }
    if (match) {
      return i;
    }
  }
  return -1;
}

MenuRow Row(const char* label, const Setting& setting, const std::shared_ptr<Draft>& draft) {
  MenuRow row;
  row.label = label;
  row.description = Text(label).description;
  row.divider_above = Text(label).divider_above;
  row.value = [&setting, draft] { return Label(setting, Find(setting, draft->Value(setting))); };
  // A launch option sets the starting value only; the badge goes once the
  // page applies a change to the setting.
  row.badge = [&setting] {
    return SetByLaunchOption(setting) ? std::string("Set by a launch option for this session.")
                                      : std::string();
  };
  row.adjust = [&setting, draft](int direction) {
    const int next = Step(setting, Find(setting, draft->Value(setting)), direction);
    draft->Change(setting, setting.choices[size_t(next)].value);
  };
  row.unapplied = [&setting, draft] { return draft->values.contains(&setting); };
  return row;
}

// A temporal upscaler takes over the anti-aliasing (TilingChoice in
// game_hooks.cpp, the post effect in the SDK): the row says so and keeps the
// player's choice for later.
MenuRow AntiAliasingRow(const char* label, const Setting& setting,
                        const std::shared_ptr<Draft>& draft,
                        const GraphicsPageServices& services) {
  MenuRow row = Row(label, setting, draft);
  row.value = [&setting, draft, services] {
    const std::string upscaler = TemporalUpscalerName(*draft, services);
    return upscaler.empty() ? Label(setting, Find(setting, draft->Value(setting)))
                            : "OFF (" + upscaler + ")";
  };
  row.enabled = [draft, services] { return TemporalUpscalerName(*draft, services).empty(); };
  return row;
}

// Steps over the upscalers this GPU cannot run; OFF always can.
MenuRow UpscalerRow(const std::shared_ptr<Draft>& draft, const GraphicsPageServices& services) {
  MenuRow row = Row("UPSCALER", kUpscaler, draft);
  row.adjust = [draft, services](int direction) {
    int index = Find(kUpscaler, draft->Value(kUpscaler));
    do {
      index = Step(kUpscaler, index, direction);
    } while (UpscalerProblem(services, kUpscaler.choices[size_t(index)].value));
    draft->Change(kUpscaler, kUpscaler.choices[size_t(index)].value);
    // An upscaler only upscales: a scale rendering larger than the screen
    // comes down to the largest that fits, drafted like any change.
    const auto largest = LargestFittingScale(services);
    if (UpscalerChosen(*draft, services) && largest && *largest &&
        RenderHeight(DraftedScale(*draft, services)) > RenderHeight(*largest)) {
      draft->Change(kScale, std::to_string(*largest));
      draft->lowered_scale = *largest;
    }
  };
  // A chosen upscaler that cannot run at the scale says so, as the rows it
  // would take over do.
  row.value = [draft, services] {
    const std::string label = Label(kUpscaler, Find(kUpscaler, draft->Value(kUpscaler)));
    return IdleUpscaler(*draft, services).empty() ? label : label + " (OFF)";
  };
  row.note = [draft, services] {
    if (draft->lowered_scale) {
      return "Resolution lowered to " + ResolutionName(draft->lowered_scale) + " so " +
             Label(kUpscaler, Find(kUpscaler, draft->Value(kUpscaler))) + " can run on this " +
             ScreenSize(services) + " screen.";
    }
    if (!UpscalerName(*draft, services).empty()) {
      return ResolutionLine(*draft, services);
    }
    if (std::string idle = IdleUpscalerNote(*draft, services); !idle.empty()) {
      return idle;
    }
    const int index = Find(kUpscaler, draft->Value(kUpscaler));
    const auto problem =
        index > 0 ? UpscalerProblem(services, kUpscaler.choices[size_t(index)].value)
                  : std::nullopt;
    return problem ? Label(kUpscaler, index) + " does not run here: " + *problem + "."
                   : std::string();
  };
  return row;
}

std::vector<MenuRow> DetailedGraphicsRows(const std::shared_ptr<Draft>& draft,
                                         const GraphicsPageServices& services) {
  std::vector<MenuRow> rows;
  MenuRow frame_rate;
  frame_rate.label = "FRAME RATE";
  frame_rate.slider = hostui::MenuSlider{
      .range =
          [services] {
            return std::pair{kFrameRate.low, FrameRateCeiling(services.display_refresh_hz
                                                                  ? services.display_refresh_hz()
                                                                  : 0)};
          },
      .get = [draft] { return std::atoi(draft->Value(kFrameRate).c_str()); },
      .set = [draft](int value) { draft->Change(kFrameRate, std::to_string(value)); }};
  frame_rate.badge = [] {
    return SetByLaunchOption(kFrameRate) ? std::string("Set by a launch option for this session.")
                                         : std::string();
  };
  frame_rate.description = Text("FRAME RATE").description;
  frame_rate.note = [services] {
    const uint32_t hz = services.display_refresh_hz ? services.display_refresh_hz() : 0;
    return hz ? "Detected display limit: " + std::to_string(hz) + " Hz."
              : "Display refresh rate unavailable. Select a target from 60 to 240 FPS.";
  };
  frame_rate.unapplied = [draft] { return draft->values.contains(&kFrameRate); };
  rows.push_back(std::move(frame_rate));

  MenuRow fullscreen;
  fullscreen.label = "FULLSCREEN";
  fullscreen.description =
      "Run in borderless fullscreen (ON) or a window (OFF). Press F11 to toggle anytime.";
  fullscreen.value = [services] {
    return services.is_fullscreen && services.is_fullscreen() ? "ON" : "OFF";
  };
  fullscreen.adjust = [services](int) {
    if (services.set_fullscreen && services.is_fullscreen) {
      services.set_fullscreen(!services.is_fullscreen());
    }
  };
  rows.push_back(std::move(fullscreen));

  MenuRow vsync = Row("VSYNC", kVsync, draft);
  rows.push_back(std::move(vsync));

  MenuRow scale = Row("RESOLUTION", kScale, draft);
  // While an upscaler is chosen, only the scales it can upscale from.
  scale.adjust = [draft, services](int direction) {
    const auto largest = LargestFittingScale(services);
    const bool limited = UpscalerChosen(*draft, services) && largest && *largest;
    int index = Find(kScale, draft->Value(kScale));
    do {
      index = Step(kScale, index, direction);
    } while (limited &&
             RenderHeight(uint32_t(std::atoi(kScale.choices[size_t(index)].value))) > RenderHeight(*largest));
    draft->Change(kScale, kScale.choices[size_t(index)].value);
  };
  scale.note = [draft, services] {
    const std::string idle = IdleUpscalerNote(*draft, services);
    return ResolutionLine(*draft, services) + (idle.empty() ? "" : " " + idle);
  };
  rows.push_back(std::move(scale));

  if (CanExtendView()) {
    rows.push_back(Row("EXTENDED VIEW", kExtendedView, draft));
  }

  rows.push_back(UpscalerRow(draft, services));

  MenuRow sharpness = Row("SHARPNESS", kSharpness, draft);
  sharpness.enabled = [draft, services] { return !UpscalerName(*draft, services).empty(); };
  rows.push_back(std::move(sharpness));

  MenuRow preset;
  preset.label = "GRAPHICS PRESET";
  preset.value = [draft] {
    const int index = DraftedPreset(*draft);
    return std::string(index >= 0 ? kPresets[index].label : "CUSTOM");
  };
  preset.adjust = [draft](int direction) {
    const int current = DraftedPreset(*draft);
    const int next = current < 0 ? (direction > 0 ? 0 : kPresetCount - 1)
                                 : (current + direction + kPresetCount) % kPresetCount;
    for (size_t j = 0; j < kPresetSettingCount; ++j) {
      draft->Change(*kPresetSettings[j], kPresets[next].values[j]);
    }
  };
  preset.description = Text("GRAPHICS PRESET").description;
  rows.push_back(std::move(preset));

  MenuRow showroom_msaa = AntiAliasingRow("SHOWROOM MSAA", kShowroomMsaa, draft, services);
  rows.push_back(std::move(showroom_msaa));

  MenuRow race_msaa = AntiAliasingRow("RACE MSAA", kRaceMsaa, draft, services);
  rows.push_back(std::move(race_msaa));

  MenuRow anisotropy = Row("ANISOTROPIC FILTERING", kAnisotropy, draft);
  rows.push_back(std::move(anisotropy));

  MenuRow detail = Row("LEVEL OF DETAIL", kDetail, draft);
  rows.push_back(std::move(detail));

  MenuRow scenery_shadows = Row("SCENERY SHADOWS", kSceneryShadows, draft);
  rows.push_back(std::move(scenery_shadows));

  MenuRow reflection_updates = Row("REFLECTION UPDATES", kReflectionUpdates, draft);
  rows.push_back(std::move(reflection_updates));

  MenuRow mirror = Row("MIRROR", kMirror, draft);
  rows.push_back(std::move(mirror));

  MenuRow soft_particles = Row("SOFT PARTICLES", kSoftParticles, draft);
  rows.push_back(std::move(soft_particles));

  MenuRow crowds = Row("CROWDS", kCrowds, draft);
  rows.push_back(std::move(crowds));

  MenuRow post_aa = AntiAliasingRow("POST AA", kPostAa, draft, services);
  rows.push_back(std::move(post_aa));

  MenuRow bloom = Row("BLOOM", kBloom, draft);
  rows.push_back(std::move(bloom));

  MenuRow motion_blur = Row("MOTION BLUR", kMotionBlur, draft);
  rows.push_back(std::move(motion_blur));

  MenuRow depth_of_field = Row("DEPTH OF FIELD", kDepthOfField, draft);
  rows.push_back(std::move(depth_of_field));

  MenuRow car_dirt = Row("CAR DIRT", kCarDirt, draft);
  rows.push_back(std::move(car_dirt));

  return rows;
}

std::vector<MenuRow> PlayerGraphicsRows(const std::shared_ptr<Draft>& draft) {
  std::vector<MenuRow> rows;
  rows.push_back(Row("DISPLAY MODE", kDisplayMode, draft));
  auto resolution = Row("RESOLUTION", kScale, draft);
  resolution.description = "Choose the game render resolution. FSR automatically enlarges a smaller picture to fit the display.";
  rows.push_back(std::move(resolution));

  auto msaa = Row("RACE MSAA", kRaceMsaa, draft);
  msaa.label = "MSAA";
  msaa.description = "Smooth geometry edges. Game uses 2x in races and 4x in the showroom. Changes take effect in the next scene.";
  const auto current = [draft] {
    const auto race = draft->Value(kRaceMsaa);
    const auto showroom = draft->Value(kShowroomMsaa);
    if (race == "2x" && showroom == "4x") return 0;
    if (race != showroom) return -1;
    if (race == "off") return 1;
    return race == "2x" ? 2 : 3;
  };
  msaa.value = [current] {
    static constexpr const char* labels[] = {"GAME", "OFF", "2X", "4X"};
    const int index = current();
    return std::string(index < 0 ? "CUSTOM" : labels[index]);
  };
  msaa.adjust = [current, draft](int direction) {
    const int index = current();
    const int next = index < 0 ? 0 : (index + direction + 4) % 4;
    static constexpr const char* race[] = {"2x", "off", "2x", "4x"};
    static constexpr const char* showroom[] = {"4x", "off", "2x", "4x"};
    draft->Change(kRaceMsaa, race[next]);
    draft->Change(kShowroomMsaa, showroom[next]);
  };
  msaa.unapplied = [draft] {
    return draft->values.contains(&kShowroomMsaa) || draft->values.contains(&kRaceMsaa);
  };
  rows.push_back(std::move(msaa));
  rows.push_back(Row("HDR", kHdr, draft));
  if (CanExtendView()) rows.push_back(Row("EXTENDED VIEW", kExtendedView, draft));
  rows.push_back(Row("SCENERY SHADOWS", kSceneryShadows, draft));
  rows.push_back(Row("REFLECTION UPDATES", kReflectionUpdates, draft));
  rows.push_back(Row("MIRROR", kMirror, draft));
  rows.push_back(Row("SOFT PARTICLES", kSoftParticles, draft));
  rows.push_back(Row("CROWDS", kCrowds, draft));
  rows.push_back(Row("MOTION BLUR", kMotionBlur, draft));
  rows.push_back(Row("DEPTH OF FIELD", kDepthOfField, draft));
  rows.push_back(Row("CAR DIRT", kCarDirt, draft));
  return rows;
}

}  // namespace

bool WriteGraphicsSchema(const std::filesystem::path& path) {
  std::string out = "{\"schema_version\":1,\"blocks\":[{\"rows\":[";
  bool first = true;
  for (const RowText& row : kRows) {
    // This versioned schema is also consumed by the separate desktop launcher.
    if (row.setting == &kDisplayMode || row.setting == &kHdr) continue;
    if (row.setting == &kExtendedView && !CanExtendView()) continue;
    if (row.divider_above) {
      out += "]},{\"rows\":[";
      first = true;
    }
    out += (first ? "" : ",") + RowJson(row);
    first = false;
  }
  out += "]}],\"presets\":[";
  for (int i = 0; i < kPresetCount; ++i) {
    out += std::string(i ? "," : "") + "{\"label\":" + JsonString(kPresets[i].label) +
           ",\"values\":{";
    for (size_t j = 0; j < kPresetSettingCount; ++j) {
      out += std::string(j ? "," : "") + JsonString(kPresetSettings[j]->name) + ":" +
             JsonString(kPresets[i].values[j]);
    }
    out += "}}";
  }
  out += "]}\n";
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << out;
  return bool(file);
}

void ApplySavedGraphicsSettings(const config::HostConfig& config) {
  for (const Setting* setting : kSettings) {
    if (setting == &kExtendedView && !CanExtendView()) continue;
    const auto value = config.Get(setting->name);
    if (!value) {
      continue;
    }
    if (SetByLaunchOption(*setting)) {
      REXLOG_INFO("Graphics settings: {} comes from the command line, not {}", setting->name,
                  config.path().string());
    } else if (Find(*setting, *value) < 0 || !SetRegisteredValue(*setting, *value)) {
      REXLOG_WARN("Graphics settings: ignored {} = {} in {}", setting->name, *value,
                  config.path().string());
    } else {
      REXLOG_INFO("Graphics settings: {} = {}", setting->name, *value);
    }
  }
  SetFsr1Sharpness();
}

uint32_t DisplayRefreshHz(void* native_window) {
#if defined(__APPLE__)
  return CurrentMacWindowRefreshHz();
#elif defined(_WIN32)
  MONITORINFOEXW monitor = {};
  monitor.cbSize = sizeof(monitor);
  DEVMODEW mode = {};
  mode.dmSize = sizeof(mode);
  if (native_window &&
      GetMonitorInfoW(MonitorFromWindow(HWND(native_window), MONITOR_DEFAULTTONEAREST),
                      &monitor) &&
      EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
      mode.dmDisplayFrequency > 1) {
    return mode.dmDisplayFrequency;
  }
#endif
  return 0;
}

void LimitFrameRateToDisplay(uint32_t refresh_hz) {
  const int ceiling = FrameRateCeiling(refresh_hz);
  if (!refresh_hz || SetByLaunchOption(kFrameRate) || LiveNumber(kFrameRate.name) <= ceiling) {
    return;
  }
  rex::cvar::SetFlagByName(kFrameRate.name, std::to_string(ceiling));
  REXLOG_INFO("Graphics settings: {} lowered to {} for a {} Hz display", kFrameRate.name, ceiling,
              refresh_hz);
}

std::unique_ptr<MenuScreen> CreateGraphicsPage(config::HostConfig& config,
                                               GraphicsPageServices services) {
  auto draft = std::make_shared<Draft>();
  draft->is_fullscreen = services.is_fullscreen;
  auto rows = services.player_layout ? PlayerGraphicsRows(draft)
                                     : DetailedGraphicsRows(draft, services);
  auto screen = std::make_unique<MenuScreen>("GRAPHICS", std::move(rows));
  screen->set_apply([draft, &config, services] { SaveAndApply(*draft, config, services); });
  // Unapplied changes keep the page open once; leaving again discards them.
  screen->set_leave([draft] {
    if (draft->busy()) {
      return false;
    }
    if (draft->values.empty() || draft->leave_warned || draft->save_failed) {
      return true;
    }
    draft->leave_warned = true;
    return false;
  });
  screen->set_status([draft] {
    if (draft->loading_resolution) {
      return hostui::MenuStatus{
          .text = "CHANGING RESOLUTION…", .busy = true, .block_navigation = true};
    }
    if (draft->busy()) {
      if (draft->settling == &kScale &&
          std::chrono::steady_clock::now() - draft->set_at >= kShaderBuildShown) {
        return hostui::MenuStatus{
            .text = "BUILDING SHADERS FOR " + Label(kScale, Find(kScale, Live(kScale.name))) +
                    ", FIRST TIME ONLY",
            .busy = true};
      }
      return hostui::MenuStatus{.text = "APPLYING SETTINGS", .busy = true};
    }
    if (draft->save_failed) {
      return hostui::MenuStatus{.text = "SAVE FAILED. RETRY SAVE & APPLY OR DISCARD.", .warning = true};
    }
    if (!draft->values.empty()) {
      return hostui::MenuStatus{.text = "UNSAVED CHANGES", .warning = draft->leave_warned};
    }
    if (draft->resolution_unconfirmed) {
      return hostui::MenuStatus{.text = "SAVED. NEW RESOLUTION COULD NOT BE CONFIRMED.", .warning = true};
    }
    if (draft->applied) {
      return hostui::MenuStatus{.text = "SETTINGS APPLIED. YOU CAN CONTINUE."};
    }
    return hostui::MenuStatus{};
  });
  screen->set_update([draft, services](bool closing) {
    if (!closing) {
      ApplyNext(*draft, services);
    } else if (draft->loading_resolution) {
      services.resolution_loading(false, {});
      draft->loading_resolution = 0;
    } else if (!draft->values.empty() && !draft->busy()) {
      REXLOG_INFO("Graphics page: discarded {} unapplied changes", draft->values.size());
    }
  });
  return screen;
}

}  // namespace fm4_static_recomp::ui
