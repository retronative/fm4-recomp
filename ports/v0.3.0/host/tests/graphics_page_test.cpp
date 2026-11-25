#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/ui/upscaler.h>

#include "ui/graphics_page.h"

// The GPU's and the game hooks' settings, which this binary does not link.
REXCVAR_DEFINE_INT32(anisotropic_override, 3, "GPU", "Test stand-in");
#if defined(__APPLE__)
REXCVAR_DEFINE_BOOL(fm4_native_fhd, false, "GPU", "Test stand-in for the native 1080p renderer");
#endif
REXCVAR_DEFINE_INT32(resolution_scale, 1, "GPU", "Test stand-in");
REXCVAR_DEFINE_INT32(fm4_frame_rate, 120, "Forza Motorsport 4", "Test stand-in");
REXCVAR_DEFINE_STRING(swap_post_effect, "none", "GPU", "Test stand-in");
REXCVAR_DEFINE_STRING(fm4_showroom_msaa, "4x", "Forza Motorsport 4", "Test stand-in");
REXCVAR_DEFINE_STRING(fm4_race_msaa, "2x", "Forza Motorsport 4", "Test stand-in");
REXCVAR_DEFINE_BOOL(fm4_extended_view, false, "Forza Motorsport 4", "Test stand-in");
REXCVAR_DEFINE_BOOL(fm4_scenery_shadows, false, "Forza Motorsport 4", "Test stand-in");
REXCVAR_DEFINE_STRING(fm4_reflection_updates, "console", "Forza Motorsport 4", "Test stand-in");
REXCVAR_DEFINE_BOOL(fm4_full_rate_mirror, false, "Forza Motorsport 4", "Test stand-in");
REXCVAR_DEFINE_BOOL(fm4_soft_particles, false, "Forza Motorsport 4", "Test stand-in");
REXCVAR_DEFINE_BOOL(fm4_crowds_every_race, false, "Forza Motorsport 4", "Test stand-in");
REXCVAR_DEFINE_BOOL(fm4_road_detail_blur, true, "Forza Motorsport 4", "Test stand-in");
REXCVAR_DEFINE_BOOL(fm4_depth_of_field, true, "Forza Motorsport 4", "Test stand-in");
REXCVAR_DEFINE_BOOL(fm4_car_dirt, true, "Forza Motorsport 4", "Test stand-in");

namespace fm4_static_recomp {
bool CanExtendView() { return true; }
}  // namespace fm4_static_recomp

namespace {

namespace config = fm4_static_recomp::config;
namespace ui = fm4_static_recomp::ui;
using fm4_static_recomp::hostui::MenuRow;
using fm4_static_recomp::hostui::MenuScreen;
using fm4_static_recomp::hostui::NavCommand;
using fm4_static_recomp::hostui::PadNavigator;

std::filesystem::path TempFile(const char* name) {
  return std::filesystem::temp_directory_path() / ("fm4_graphics_page_test_" + std::string(name));
}

rex::ui::UpscalerInfo Upscaler(const char* id, bool available, const char* reason = "") {
  rex::ui::UpscalerInfo info;
  info.id = id;
  info.kind = rex::ui::UpscalerKind::kTemporal;
  info.available = available;
  info.unavailable_reason = reason;
  return info;
}

// The presenter lists FSR 1 among its spatial effects, on any GPU.
rex::ui::UpscalerInfo Fsr1() {
  rex::ui::UpscalerInfo info;
  info.id = "fsr";
  info.kind = rex::ui::UpscalerKind::kSpatial;
  info.available = true;
  return info;
}

// FSR 1, 2 and 3 run, DLSS does not: the page on an AMD GPU.
ui::GraphicsPageServices Services() {
  ui::GraphicsPageServices services;
  services.player_layout = false;  // Exercise the full non-Mac layout here.
  services.draw_resolution_scale = [] { return 1u; };
  services.output_size = [] { return std::optional(std::pair{2560u, 1440u}); };
  services.upscalers = [] {
    return std::vector{Fsr1(), Upscaler("dlss", false, "needs an NVIDIA RTX GPU"),
                       Upscaler("fsr3", true), Upscaler("fsr2", true)};
  };
  return services;
}

// Every upscaler runs: the page on an NVIDIA RTX GPU.
ui::GraphicsPageServices NvidiaServices() {
  auto services = Services();
  services.upscalers = [] {
    return std::vector{Fsr1(), Upscaler("dlss", true), Upscaler("fsr3", true),
                       Upscaler("fsr2", true)};
  };
  return services;
}

const MenuRow* Find(const MenuScreen& screen, const char* label) {
  const auto& rows = screen.rows();
  const auto it = std::find_if(rows.begin(), rows.end(),
                               [label](const MenuRow& row) { return row.label == label; });
  return it == rows.end() ? nullptr : &*it;
}

// The cvars are the runtime's, shared by every test: each test starts from no upscaler.
struct Upscaling {
  Upscaling(const char* effect, bool active) {
    rex::cvar::SetFlagByName("present_effect", effect);
    rex::ui::SetTemporalUpscalerActive(active);
  }
  ~Upscaling() {
    rex::cvar::SetFlagByName("present_effect", "bilinear");
    rex::ui::SetTemporalUpscalerActive(false);
  }
};

// Puts the named cvars back as they were.
struct CvarsRestored {
  explicit CvarsRestored(std::vector<std::string> names) : names(std::move(names)) {
    for (const std::string& name : this->names) {
      values.push_back(rex::cvar::GetFlagByName(name));
    }
  }
  ~CvarsRestored() {
    for (size_t i = 0; i < names.size(); ++i) {
      rex::cvar::SetFlagByName(names[i], values[i]);
    }
  }
  std::vector<std::string> names;
  std::vector<std::string> values;
};

// Steps the row until it shows `label`.
void Choose(const MenuRow& row, const std::string& label) {
  for (int i = 0; i < 16 && row.value() != label; ++i) {
    row.adjust(+1);
  }
  REQUIRE(row.value() == label);
}

// The label of the row the focus reaches from `from` by `command`.
std::string FocusAfter(MenuScreen& page, const char* from, NavCommand command) {
  const auto& rows = page.rows();
  const auto it = std::find_if(rows.begin(), rows.end(),
                               [from](const MenuRow& row) { return row.label == from; });
  REQUIRE(page.SetFocus(size_t(it - rows.begin())));
  page.Handle(command);
  return rows[page.focus()].label;
}

// Runs the page's per-frame update while it applies, at most `frames` times.
void Settle(MenuScreen& page, int frames = 64) {
  for (int i = 0; i < frames && page.status().busy; ++i) {
    page.Update(false);
  }
}

}  // namespace

TEST_CASE("The launcher's schema offers the upscaler, its sharpness and the DLSS preset while DLSS "
          "is chosen",
          "[graphics_page]") {
  const auto path = TempFile("schema.json");
  REQUIRE(ui::WriteGraphicsSchema(path));
  std::stringstream text;
  text << std::ifstream(path).rdbuf();
  const std::string schema = text.str();
  const auto at = [&schema](const char* label) {
    return schema.find(std::string("{\"label\":\"") + label + "\"");
  };
  REQUIRE(at("UPSCALER") != std::string::npos);
  REQUIRE(at("DLSS PRESET") != std::string::npos);
  REQUIRE(at("SHARPNESS") != std::string::npos);
  REQUIRE(at("RESOLUTION") != std::string::npos);
  CHECK(at("RESOLUTION") < at("UPSCALER"));
  const std::string resolution = schema.substr(at("RESOLUTION"), at("EXTENDED VIEW") - at("RESOLUTION"));
  CHECK(resolution.find("\"key\":\"resolution_scale\"") != std::string::npos);
  for (const auto& [label, value] : {std::pair{"720p", "1"}, std::pair{"1440p (QHD)", "2"},
                                     std::pair{"4K (2160p)", "3"}}) {
    CHECK(resolution.find(std::string("{\"label\":\"") + label + "\",\"value\":\"" + value + "\"}") != std::string::npos);
  }
#if defined(__APPLE__)
  CHECK(resolution.find("\"default\":\"5\"") != std::string::npos);
  const std::string post_aa = schema.substr(at("POST AA"), at("BLOOM") - at("POST AA"));
  CHECK(post_aa.find("\"default\":\"fxaa\"") != std::string::npos);
  CHECK(resolution.find("{\"label\":\"1080p (Full HD)\",\"value\":\"5\"}") != std::string::npos);
  CHECK(resolution.find("5K (2880p)") == std::string::npos);
  CHECK(resolution.find("\"value\":\"4\"") == std::string::npos);
#else
  CHECK(resolution.find("{\"label\":\"5K (2880p)\",\"value\":\"4\"}") != std::string::npos);
#endif
  CHECK(at("UPSCALER") < at("DLSS PRESET"));
  CHECK(at("DLSS PRESET") < at("SHARPNESS"));
  CHECK(at("SHARPNESS") < at("GRAPHICS PRESET"));
  CHECK(schema.find("\"key\":\"present_effect\"") != std::string::npos);
  CHECK(schema.find("{\"label\":\"OFF\",\"value\":\"bilinear\"}") != std::string::npos);
  CHECK(schema.find("{\"label\":\"30 %\",\"value\":\"30\"}") != std::string::npos);
  const std::string preset = schema.substr(at("DLSS PRESET"), at("SHARPNESS") - at("DLSS PRESET"));
  CHECK(preset.find("\"key\":\"present_dlss_preset\"") != std::string::npos);
  CHECK(preset.find("\"default\":\"auto\"") != std::string::npos);
  CHECK(preset.find("\"shown_when\":{\"present_effect\":\"dlss\"}") != std::string::npos);
  CHECK(schema.find("shown_when") == schema.rfind("shown_when"));
  CHECK(schema.find("FULLSCREEN") == std::string::npos);
  CHECK(schema.find("\"fullscreen\"") == std::string::npos);
  std::filesystem::remove(path);
}

TEST_CASE("The in-game page has FULLSCREEN, UPSCALER and SHARPNESS and leaves the DLSS preset to the launcher",
          "[graphics_page]") {
  config::HostConfig config(TempFile("rows.toml"));
  const auto page = ui::CreateGraphicsPage(config, Services());
  CHECK(Find(*page, "FULLSCREEN"));
  CHECK(Find(*page, "UPSCALER"));
  CHECK(Find(*page, "SHARPNESS"));
  CHECK_FALSE(Find(*page, "DLSS PRESET"));
}

TEST_CASE("Existing numeric resolution settings open with their named render sizes", "[graphics_page]") {
  const auto path = TempFile("saved_resolution.toml");
  const CvarsRestored restored({"resolution_scale"});
  for (const auto& [value, label] : {std::pair{1, "720p"}, std::pair{2, "1440p (QHD)"},
                                     std::pair{3, "4K (2160p)"}}) {
    std::ofstream(path) << "resolution_scale = " << value << '\n';
    config::HostConfig config(path);
    REQUIRE(config.Load());
    ui::ApplySavedGraphicsSettings(config);
    auto services = Services();
    services.draw_resolution_scale = [value] { return uint32_t(value); };
    const auto page = ui::CreateGraphicsPage(config, services);
    const MenuRow* resolution = Find(*page, "RESOLUTION");
    REQUIRE(resolution);
    CHECK(resolution->value() == label);
    CHECK(resolution->note().starts_with("Renders " + std::to_string(1280 * value) + "x" +
                                         std::to_string(720 * value) + ","));
    CHECK(config.Get("resolution_scale").value_or("") == std::to_string(value));
  }
  std::filesystem::remove(path);
}

#if defined(__APPLE__)
TEST_CASE("Mac graphics offers the display controls, eight race effects and 4K", "[graphics_page]") {
  auto services = Services();
  REQUIRE(ui::GraphicsPageServices{}.player_layout);
  services.player_layout = true;
  services.output_size = [] { return std::optional(std::pair{1920u, 1080u}); };
  const CvarsRestored restore({"resolution_scale", "fm4_native_fhd", "fm4_showroom_msaa",
                              "fm4_race_msaa", "fullscreen", "present_hdr", "present_effect"});
  const Upscaling spatial("fsr", false);
  const auto path = TempFile("player_graphics.toml");
  std::filesystem::remove(path);
  config::HostConfig config(path);
  bool fullscreen = true;
  services.is_fullscreen = [&] { return fullscreen; };
  services.set_fullscreen = [&](bool value) { fullscreen = value; };
  auto page = ui::CreateGraphicsPage(config, services);
  REQUIRE(page->rows().size() == 13);
  CHECK(page->rows()[0].label == "DISPLAY MODE");
  CHECK(page->rows()[1].label == "RESOLUTION");
  CHECK(page->rows()[2].label == "MSAA");
  CHECK(page->rows()[3].label == "HDR");
  CHECK(page->rows()[4].label == "EXTENDED VIEW");
  SECTION("the eight race effects can each be changed, saved and reopened") {
    const std::vector<std::pair<std::string, std::string>> effects = {
        {"SCENERY SHADOWS", "fm4_scenery_shadows"}, {"REFLECTION UPDATES", "fm4_reflection_updates"},
        {"MIRROR", "fm4_full_rate_mirror"}, {"SOFT PARTICLES", "fm4_soft_particles"},
        {"CROWDS", "fm4_crowds_every_race"}, {"MOTION BLUR", "fm4_road_detail_blur"},
        {"DEPTH OF FIELD", "fm4_depth_of_field"}, {"CAR DIRT", "fm4_car_dirt"}};
    std::vector<std::string> names, before, chosen;
    for (const auto& effect : effects) names.push_back(effect.second);
    const CvarsRestored restore_effects(names);
    for (size_t i = 0; i < effects.size(); ++i) {
      const auto& row = page->rows()[5 + i];
      CHECK(row.label == effects[i].first);
      REQUIRE(row.is_enabled());
      before.push_back(row.value());
      row.adjust(1);
      chosen.push_back(row.value());
      CHECK(before.back() != chosen.back());
    }
    CHECK_FALSE(std::filesystem::exists(path));
    page->Handle(NavCommand::kApply);
    Settle(*page);
    REQUIRE(config.Load());
    page = ui::CreateGraphicsPage(config, services);
    for (size_t i = 0; i < effects.size(); ++i) {
      REQUIRE(config.Get(names[i]) == rex::cvar::GetFlagByName(names[i]));
      CHECK(page->rows()[5 + i].value() == chosen[i]);
    }
  }
  SECTION("every resolution fits in the picker on a Full HD display") {
    const auto& row = page->rows()[1];
    for (const char* resolution : {"720p", "1080p (Full HD)", "1440p (QHD)", "4K (2160p)"}) {
      Choose(row, resolution);
    }
    CHECK_FALSE(std::filesystem::exists(path));
    row.adjust(1);
    CHECK(row.value() == "720p");
  }
  SECTION("display mode, grouped MSAA and HDR wait for Apply and are saved") {
    rex::cvar::SetFlagByName("present_hdr", "true");
    Choose(page->rows()[0], "WINDOWED");
    Choose(page->rows()[2], "4X");
    Choose(page->rows()[3], "OFF");
    CHECK(fullscreen);
    CHECK_FALSE(std::filesystem::exists(path));
    page->Handle(NavCommand::kApply);
    Settle(*page);
    CHECK_FALSE(fullscreen);
    REQUIRE(config.Load());
    CHECK(config.Get("fullscreen") == "false");
    CHECK(config.Get("fm4_race_msaa") == "4x");
    CHECK(config.Get("present_hdr") == "false");
    CHECK(rex::cvar::GetFlagByName("fm4_showroom_msaa") == "4x");
    CHECK(rex::cvar::GetFlagByName("present_effect") == "fsr");
  }
  SECTION("D-pad changes tabs only after reaching the tab strip") {
    bool in_tabs = false;
    CHECK_FALSE(page->RouteTabNavigation(NavCommand::kUp, in_tabs));
    REQUIRE(in_tabs);
    CHECK(page->RouteTabNavigation(NavCommand::kRight, in_tabs) == NavCommand::kNextTab);
    CHECK(page->RouteTabNavigation(NavCommand::kLeft, in_tabs) == NavCommand::kPreviousTab);
    CHECK_FALSE(page->RouteTabNavigation(NavCommand::kDown, in_tabs));
    REQUIRE_FALSE(in_tabs);
    CHECK(page->RouteTabNavigation(NavCommand::kRight, in_tabs) == NavCommand::kRight);
  }
  std::filesystem::remove(path);
}

TEST_CASE("Full HD stores its own ID, fits a 1080p output and switches back to integer scales", "[graphics_page]") {
  const auto path = TempFile("native_fhd.toml");
  const CvarsRestored restored({"resolution_scale", "fm4_native_fhd", "present_effect"});
  const Upscaling off("bilinear", false);
  std::ofstream(path) << "resolution_scale = 2\nkeep_this_setting = true\n";
  config::HostConfig config(path);
  REQUIRE(config.Load());
  ui::ApplySavedGraphicsSettings(config);
  auto services = Services();
  auto active = std::make_shared<uint32_t>(2);
  services.draw_resolution_scale = [active] { return *active; };
  services.output_size = [] { return std::optional(std::pair{1920u, 1080u}); };
  auto page = ui::CreateGraphicsPage(config, services);
  const auto& row = *Find(*page, "RESOLUTION");
  const auto& up = *Find(*page, "UPSCALER");
  Choose(up, "FSR 3");
  REQUIRE(row.value() == "1080p (Full HD)");
  CHECK(up.note().find("1920x1080") != std::string::npos);
  page->Handle(NavCommand::kApply);
  page->Update(false);
  CHECK(rex::cvar::GetFlagByName("resolution_scale") == "2");
  CHECK(rex::cvar::GetFlagByName("fm4_native_fhd") == "true");
  CHECK(page->status().busy);
  *active = 5;
  std::this_thread::sleep_for(std::chrono::milliseconds(170));
  Settle(*page);
  REQUIRE(config.Load());
  CHECK(config.Get("resolution_scale") == "5");
  CHECK(config.Get("keep_this_setting") == "true");
  rex::cvar::SetFlagByName("fm4_native_fhd", "false");
  rex::cvar::SetFlagByName("resolution_scale", "2");
  ui::ApplySavedGraphicsSettings(config);
  CHECK(rex::cvar::GetFlagByName("fm4_native_fhd") == "true");
  CHECK(rex::cvar::GetFlagByName("resolution_scale") == "2");
  page = ui::CreateGraphicsPage(config, services);
  CHECK(Find(*page, "RESOLUTION")->note().starts_with("Renders 1920x1080,"));
  Choose(*Find(*page, "RESOLUTION"), "720p");
  page->Handle(NavCommand::kApply);
  page->Update(false);
  CHECK(rex::cvar::GetFlagByName("fm4_native_fhd") == "false");
  CHECK(rex::cvar::GetFlagByName("resolution_scale") == "1");
  CHECK(config.Get("resolution_scale") == "1");
  std::filesystem::remove(path);
}
#endif

TEST_CASE("FULLSCREEN reflects actual window state and does not save to graphics config",
          "[graphics_page]") {
  const auto path = TempFile("fullscreen_test.toml");
  std::filesystem::remove(path);
  config::HostConfig config(path);

  bool fullscreen_state = true;
  ui::GraphicsPageServices services = Services();
  services.is_fullscreen = [&] { return fullscreen_state; };
  services.set_fullscreen = [&](bool fs) { fullscreen_state = fs; };

  const auto page = ui::CreateGraphicsPage(config, services);
  const MenuRow* row = Find(*page, "FULLSCREEN");
  REQUIRE(row);
  CHECK(row->value() == "ON");
  row->adjust(+1);
  CHECK(fullscreen_state == false);
  CHECK(row->value() == "OFF");
  row->adjust(-1);
  CHECK(fullscreen_state == true);
  CHECK(row->value() == "ON");

  page->Handle(NavCommand::kApply);
  CHECK_FALSE(config.Get("fullscreen").has_value());
  std::filesystem::remove(path);
}

TEST_CASE("UPSCALER steps past an upscaler the GPU cannot run", "[graphics_page]") {
  const auto path = TempFile("step.toml");
  std::filesystem::remove(path);
  config::HostConfig config(path);
  const Upscaling upscaling("bilinear", false);
  const auto page = ui::CreateGraphicsPage(config, Services());
  const MenuRow& row = *Find(*page, "UPSCALER");
  CHECK(row.value() == "OFF");
  row.adjust(+1);
  CHECK(row.value() == "FSR 1");
  row.adjust(+1);
  CHECK(row.value() == "FSR 2");
  row.adjust(+1);
  CHECK(row.value() == "FSR 3");
  row.adjust(+1);
  CHECK(row.value() == "OFF");
  row.adjust(-1);
  CHECK(row.value() == "FSR 3");
  CHECK(rex::cvar::GetFlagByName("present_effect") == "bilinear");
  page->Handle(NavCommand::kApply);
  CHECK(config.Get("present_effect").value_or("") == "fsr3");
  Settle(*page);
  CHECK(rex::cvar::GetFlagByName("present_effect") == "fsr3");
  std::filesystem::remove(path);
}

TEST_CASE("A change waits for SAVE & APPLY, then the page says it is applied", "[graphics_page]") {
  const auto path = TempFile("draft.toml");
  std::filesystem::remove(path);
  config::HostConfig config(path);
  const CvarsRestored restored({"anisotropic_override"});
  rex::cvar::SetFlagByName("anisotropic_override", "-1");
  const auto page = ui::CreateGraphicsPage(config, Services());
  const MenuRow& row = *Find(*page, "ANISOTROPIC FILTERING");
  CHECK(page->status().text.empty());
  row.adjust(+1);
  CHECK(row.value() == "2X");
  CHECK(row.unapplied());
  CHECK(rex::cvar::GetFlagByName("anisotropic_override") == "-1");
  CHECK(page->status().text == "UNSAVED CHANGES");
  row.adjust(-1);
  CHECK_FALSE(row.unapplied());
  CHECK(page->status().text.empty());
  row.adjust(+1);
  page->Handle(NavCommand::kApply);
  CHECK(config.Get("anisotropic_override").value_or("") == "2");
  CHECK(page->status().busy);
  Settle(*page);
  CHECK(rex::cvar::GetFlagByName("anisotropic_override") == "2");
  CHECK_FALSE(row.unapplied());
  CHECK(page->status().text == "SETTINGS APPLIED. YOU CAN CONTINUE.");
  row.adjust(+1);
  CHECK(page->status().text == "UNSAVED CHANGES");
  std::filesystem::remove(path);
}

#if defined(__APPLE__)
TEST_CASE("Resolution stays drafted until Apply and waits for a new game image", "[graphics_page]") {
  const auto path = TempFile("resolution_loading.toml");
  std::filesystem::remove(path);
  config::HostConfig config(path);
  const CvarsRestored restored({"resolution_scale", "fm4_native_fhd"});
  rex::cvar::SetFlagByName("resolution_scale", "1");
  rex::cvar::SetFlagByName("fm4_native_fhd", "false");
  uint32_t scale = 1;
  ui::PresentedGuestFrame frame{21, 1280, 720};
  std::vector<std::string> notices;
  auto services = Services();
  services.draw_resolution_scale = [&] { return scale; };
  services.presented_guest_frame = [&] { return frame; };
  services.resolution_loading = [&](bool shown, std::string_view target) {
    notices.emplace_back(shown ? target : "hidden");
  };
  auto page = ui::CreateGraphicsPage(config, services);
  const auto& row = *Find(*page, "RESOLUTION");
  Choose(row, "1080p (Full HD)");
  Choose(row, "1440p (QHD)");
  page->Update(false);
  CHECK(notices.empty());
  CHECK_FALSE(std::filesystem::exists(path));
  CHECK(rex::cvar::GetFlagByName("resolution_scale") == "1");
  SECTION("discard or return to the starting choice") {
    Choose(row, "720p");
    page->Handle(NavCommand::kApply);
    CHECK(notices.empty());
    Choose(row, "1080p (Full HD)");
    page->Handle(NavCommand::kBack);
    CHECK(page->Handle(NavCommand::kBack) == MenuScreen::Result::kBack);
    page->Update(true);
    CHECK(notices.empty());
    CHECK_FALSE(std::filesystem::exists(path));
    CHECK(rex::cvar::GetFlagByName("resolution_scale") == "1");
  }
  SECTION("save and wait for fresh pixels at the target size") {
    page->Handle(NavCommand::kApply);
    REQUIRE(notices.size() == 1);
    CHECK(notices.front() == "1440p (QHD)");
    REQUIRE(config.Load());
    CHECK(config.Get("resolution_scale") == "2");
    page->Update(false);
    scale = 2;
    frame = {21, 2560, 1440};
    page->Update(false);
    CHECK(page->status().busy);
    frame = {22, 1280, 720};
    page->Update(false);
    CHECK(page->status().busy);
    CHECK_FALSE(page->MayLeave());
    const auto focus = page->focus();
    page->Handle(NavCommand::kDown);
    CHECK(page->focus() == focus);
    page->Handle(NavCommand::kApply);
    CHECK(notices.size() == 1);
    frame = {23, 2560, 1440};
    page->Update(false);
    CHECK_FALSE(page->status().busy);
    CHECK(page->MayLeave());
    CHECK(notices.size() == 2);
    CHECK(notices.back() == "hidden");
    page->Handle(NavCommand::kApply);
    CHECK(notices.size() == 2);
  }
  std::filesystem::remove(path);
}
#endif

TEST_CASE("Leaving with unapplied changes warns once, then discards them", "[graphics_page]") {
  config::HostConfig config(TempFile("leave.toml"));
  const CvarsRestored restored({"anisotropic_override"});
  rex::cvar::SetFlagByName("anisotropic_override", "-1");
  const auto page = ui::CreateGraphicsPage(config, Services());
  CHECK(page->Handle(NavCommand::kBack) == MenuScreen::Result::kBack);
  Find(*page, "ANISOTROPIC FILTERING")->adjust(+1);
  CHECK(page->Handle(NavCommand::kClose) == MenuScreen::Result::kChanged);
  CHECK(page->status().warning);
  // A further change asks again.
  Find(*page, "ANISOTROPIC FILTERING")->adjust(+1);
  CHECK_FALSE(page->status().warning);
  CHECK(page->Handle(NavCommand::kBack) == MenuScreen::Result::kChanged);
  CHECK(page->Handle(NavCommand::kBack) == MenuScreen::Result::kBack);
  page->Update(true);
  CHECK(rex::cvar::GetFlagByName("anisotropic_override") == "-1");
}

TEST_CASE("Extended View applies live on save and is restored on relaunch", "[graphics_page]") {
  const auto path = TempFile("extended_view.toml");
  std::filesystem::remove(path);
  const CvarsRestored restored({"fm4_extended_view"});
  rex::cvar::SetFlagByName("fm4_extended_view", "false");
  config::HostConfig config(path);
  REQUIRE(config.Load());
  const auto page = ui::CreateGraphicsPage(config, Services());
  const auto* row = Find(*page, "EXTENDED VIEW");
  REQUIRE(row);
  CHECK(row->value() == "OFF");
  row->adjust(+1);
  CHECK(row->value() == "ON");
  CHECK(rex::cvar::GetFlagByName("fm4_extended_view") == "false");
  page->Handle(NavCommand::kApply);
  Settle(*page);
  CHECK(rex::cvar::GetFlagByName("fm4_extended_view") == "true");
  REQUIRE(std::filesystem::is_regular_file(path));

  rex::cvar::SetFlagByName("fm4_extended_view", "false");
  config::HostConfig reloaded(path);
  REQUIRE(reloaded.Load());
  ui::ApplySavedGraphicsSettings(reloaded);
  CHECK(rex::cvar::GetFlagByName("fm4_extended_view") == "true");
  const auto reopened = ui::CreateGraphicsPage(reloaded, Services());
  const auto* saved_row = Find(*reopened, "EXTENDED VIEW");
  REQUIRE(saved_row);
  REQUIRE(saved_row->value() == "ON");
  saved_row->adjust(-1);
  reopened->Handle(NavCommand::kApply);
  Settle(*reopened);
  CHECK(rex::cvar::GetFlagByName("fm4_extended_view") == "false");
  REQUIRE(reloaded.Load());
  CHECK(reloaded.Get("fm4_extended_view").value_or("") == "false");
  std::filesystem::remove(path);
}

TEST_CASE("SAVE & APPLY sets the settings one at a time, the rest after the renderer takes the "
          "scale",
          "[graphics_page]") {
  config::HostConfig config(TempFile("sequence.toml"));
  const CvarsRestored restored({"resolution_scale", "anisotropic_override", "fm4_frame_rate"});
  rex::cvar::SetFlagByName("resolution_scale", "1");
  rex::cvar::SetFlagByName("anisotropic_override", "-1");
  rex::cvar::SetFlagByName("fm4_frame_rate", "60");
  auto renderer_scale = std::make_shared<uint32_t>(1);
  auto services = Services();
  services.draw_resolution_scale = [renderer_scale] { return *renderer_scale; };
  const auto page = ui::CreateGraphicsPage(config, services);
  Find(*page, "ANISOTROPIC FILTERING")->adjust(+1);
  Choose(*Find(*page, "RESOLUTION"), "1440p (QHD)");
  page->SetFocus(0);
  CHECK(page->Slide(+12) == MenuScreen::Result::kChanged);
  CHECK(Find(*page, "FRAME RATE")->unapplied());
  CHECK(rex::cvar::GetFlagByName("fm4_frame_rate") == "60");
  page->Handle(NavCommand::kApply);
  page->Update(false);
  CHECK(rex::cvar::GetFlagByName("resolution_scale") == "2");
  // Until the renderer draws at 2x nothing else is set, no row changes and the page stays.
  for (int i = 0; i < 8; ++i) {
    page->Update(false);
  }
  CHECK(rex::cvar::GetFlagByName("fm4_frame_rate") == "60");
  CHECK(rex::cvar::GetFlagByName("anisotropic_override") == "-1");
  CHECK(page->status().busy);
  CHECK(page->Handle(NavCommand::kBack) == MenuScreen::Result::kChanged);
  CHECK(page->Handle(NavCommand::kRight) == MenuScreen::Result::kNone);
  CHECK(page->status().text == "APPLYING SETTINGS");
  // A switch this long is the driver building the scale's pipelines.
  std::this_thread::sleep_for(std::chrono::milliseconds(3100));
  page->Update(false);
  CHECK(page->status().text == "BUILDING SHADERS FOR 1440p (QHD), FIRST TIME ONLY");
  CHECK(page->status().busy);
  *renderer_scale = 2;
  page->Update(false);
  CHECK(rex::cvar::GetFlagByName("fm4_frame_rate") == "72");
  CHECK(rex::cvar::GetFlagByName("anisotropic_override") == "-1");
  page->Update(false);
  CHECK(rex::cvar::GetFlagByName("anisotropic_override") == "2");
  CHECK_FALSE(page->status().busy);
  CHECK(page->Handle(NavCommand::kBack) == MenuScreen::Result::kBack);
}

TEST_CASE("A drafted upscaler already takes over the anti-aliasing rows", "[graphics_page]") {
  config::HostConfig config(TempFile("drafted_upscaler.toml"));
  const Upscaling upscaling("bilinear", false);
  const auto page = ui::CreateGraphicsPage(config, Services());
  Find(*page, "UPSCALER")->adjust(-1);
  CHECK(Find(*page, "UPSCALER")->value() == "FSR 3");
  CHECK(Find(*page, "POST AA")->value() == "OFF (FSR 3)");
  CHECK_FALSE(Find(*page, "RACE MSAA")->is_enabled());
  CHECK(Find(*page, "SHARPNESS")->is_enabled());
  CHECK(Find(*page, "UPSCALER")->note() ==
        "Renders 1280x720, upscaled to 2560x1440 by FSR 3.");
}

TEST_CASE("Y applies, and START does not close while BACK is held for BACK+START",
          "[graphics_page]") {
  PadNavigator pad;
  pad.Poll(0, 0, 0, 0);
  CHECK(pad.Poll(PadNavigator::kButtonY, 0, 0, 10) == std::vector{NavCommand::kApply});
  CHECK(pad.Poll(PadNavigator::kBack, 0, 0, 20).empty());
  CHECK(pad.Poll(PadNavigator::kBack | PadNavigator::kStart, 0, 0, 30).empty());
  CHECK(pad.Poll(0, 0, 0, 40).empty());
  CHECK(pad.Poll(PadNavigator::kStart, 0, 0, 50) == std::vector{NavCommand::kClose});
}

TEST_CASE("UPSCALER says why a chosen upscaler does not run", "[graphics_page]") {
  config::HostConfig config(TempFile("reason.toml"));
  const Upscaling upscaling("dlss", false);
  const auto page = ui::CreateGraphicsPage(config, Services());
  const MenuRow& row = *Find(*page, "UPSCALER");
  CHECK(row.value() == "DLSS");
  CHECK(row.note() == "DLSS does not run here: needs an NVIDIA RTX GPU.");
}

TEST_CASE("While an upscaler runs, the notes give the render size and output size",
          "[graphics_page]") {
  config::HostConfig config(TempFile("note.toml"));
  const Upscaling upscaling("fsr3", true);
  const auto page = ui::CreateGraphicsPage(config, Services());
  const std::string line = "Renders 1280x720, upscaled to 2560x1440 by FSR 3.";
  CHECK(Find(*page, "UPSCALER")->note() == line);
  CHECK(Find(*page, "RESOLUTION")->note() == line);
}

TEST_CASE("At the output size the notes name the upscaler's anti-aliasing mode", "[graphics_page]") {
  config::HostConfig config(TempFile("native.toml"));
  auto services = Services();
  services.output_size = [] { return std::optional(std::pair{1280u, 720u}); };
  {
    const Upscaling upscaling("dlss", true);
    const auto page = ui::CreateGraphicsPage(config, services);
    CHECK(Find(*page, "RESOLUTION")->note() == "Renders 1280x720, shown at that size with DLAA.");
  }
  const Upscaling upscaling("fsr3", true);
  const auto page = ui::CreateGraphicsPage(config, services);
  const std::string line = "Renders 1280x720, shown at that size with FSR 3 Native AA.";
  CHECK(Find(*page, "UPSCALER")->note() == line);
  CHECK(Find(*page, "RESOLUTION")->note() == line);
}

TEST_CASE("Without an upscaler the notes keep the plain stretch", "[graphics_page]") {
  config::HostConfig config(TempFile("stretch.toml"));
  const Upscaling upscaling("bilinear", false);
  const auto page = ui::CreateGraphicsPage(config, Services());
  CHECK(Find(*page, "RESOLUTION")->note() == "Renders 1280x720, stretched to 2560x1440.");
  CHECK(Find(*page, "UPSCALER")->note().empty());
}

TEST_CASE("An upscaler takes over POST AA and enables SHARPNESS", "[graphics_page]") {
  config::HostConfig config(TempFile("owned.toml"));
  const auto page = ui::CreateGraphicsPage(config, Services());
  const MenuRow& post_aa = *Find(*page, "POST AA");
  const MenuRow& sharpness = *Find(*page, "SHARPNESS");
  {
    const Upscaling upscaling("bilinear", false);
    CHECK(post_aa.is_enabled());
    CHECK_FALSE(sharpness.is_enabled());
  }
  {
    const Upscaling upscaling("dlss", true);
    CHECK(post_aa.value() == "OFF (DLSS)");
    CHECK_FALSE(post_aa.is_enabled());
    CHECK(sharpness.is_enabled());
    CHECK(sharpness.value() == "30 %");
  }
}

TEST_CASE("Every live and drafted upscaler and scale greys exactly the anti-aliasing rows or "
          "SHARPNESS",
          "[graphics_page]") {
  struct Case {
    const char* live;
    bool active;     // the live upscaler is upscaling
    bool nvidia;     // DLSS runs on this GPU
    const char* drafted;  // the UPSCALER row's drafted choice, or null
    uint32_t scale;       // the scale the renderer draws at
    const char* upscaling;  // the upscaler the rows should follow, or empty
  };
  const Case cases[] = {
      {"bilinear", false, false, nullptr, 1, ""},
      {"bilinear", false, false, "FSR 2", 1, "FSR 2"},
      {"bilinear", false, true, "DLSS", 1, "DLSS"},
      {"fsr3", true, false, nullptr, 1, "FSR 3"},
      {"fsr3", true, false, "OFF", 1, ""},
      {"fsr3", true, false, "FSR 2", 1, "FSR 2"},
      {"dlss", true, true, nullptr, 1, "DLSS"},
      {"dlss", true, true, "OFF", 1, ""},
      {"dlss", true, true, "FSR 3", 1, "FSR 3"},
      // Chosen in the file, but this GPU cannot run it, so nothing upscales.
      {"dlss", false, false, nullptr, 1, ""},
      {"dlss", false, false, "FSR 2", 1, "FSR 2"},
      // Chosen, but its runtime failed to start.
      {"fsr3", false, false, nullptr, 1, ""},
      // The output is 2560x1440: at 2x the upscaler runs at 1:1. A larger render (3x,
      // saved with the upscaler in the launcher) is downscaled without one, and choosing
      // an upscaler lowers the scale to 2x.
      {"bilinear", false, false, "FSR 2", 2, "FSR 2"},
      {"dlss", true, true, nullptr, 2, "DLSS"},
      {"fsr3", false, false, nullptr, 3, ""},
      {"bilinear", false, false, "FSR 2", 3, "FSR 2"},
      {"dlss", false, true, "FSR 3", 3, "FSR 3"},
  };
  for (const Case& c : cases) {
    CAPTURE(c.live, c.active, c.nvidia, c.drafted ? c.drafted : "-", c.scale);
    config::HostConfig config(TempFile("greying.toml"));
    const Upscaling live(c.live, c.active);
    const CvarsRestored restored(
        {"swap_post_effect", "fm4_showroom_msaa", "fm4_race_msaa", "resolution_scale"});
    rex::cvar::SetFlagByName("resolution_scale", std::to_string(c.scale));
    rex::cvar::SetFlagByName("swap_post_effect", "fxaa");
    rex::cvar::SetFlagByName("fm4_showroom_msaa", "2x");
    rex::cvar::SetFlagByName("fm4_race_msaa", "4x");
    auto services = c.nvidia ? NvidiaServices() : Services();
    services.draw_resolution_scale = [scale = c.scale] { return scale; };
    const auto page = ui::CreateGraphicsPage(config, services);
    if (c.drafted) {
      Choose(*Find(*page, "UPSCALER"), c.drafted);
    }
    const std::string upscaling = c.upscaling;
    const bool upscales = !upscaling.empty();
    CHECK(Find(*page, "SHARPNESS")->is_enabled() == upscales);
    CHECK(Find(*page, "UPSCALER")->is_enabled());
    CHECK(Find(*page, "RESOLUTION")->is_enabled());
    CHECK(Find(*page, "GRAPHICS PRESET")->is_enabled());
    for (const auto& [label, own] : {std::pair{"SHOWROOM MSAA", "2X"}, std::pair{"RACE MSAA", "4X"},
                                     std::pair{"POST AA", "FXAA"}}) {
      CAPTURE(label);
      const MenuRow& row = *Find(*page, label);
      CHECK(row.is_enabled() == !upscales);
      CHECK(row.value() == (upscales ? "OFF (" + upscaling + ")" : std::string(own)));
    }
    // The focus steps over the greyed rows.
    CHECK(FocusAfter(*page, "UPSCALER", NavCommand::kDown) ==
          (upscales ? "SHARPNESS" : "GRAPHICS PRESET"));
    CHECK(FocusAfter(*page, "GRAPHICS PRESET", NavCommand::kDown) ==
          (upscales ? "ANISOTROPIC FILTERING" : "SHOWROOM MSAA"));
    CHECK(FocusAfter(*page, "CROWDS", NavCommand::kDown) == (upscales ? "BLOOM" : "POST AA"));
    CHECK(FocusAfter(*page, "GRAPHICS PRESET", NavCommand::kUp) ==
          (upscales ? "SHARPNESS" : "UPSCALER"));
    const std::string note = Find(*page, "RESOLUTION")->note();
    CAPTURE(note);
    if (upscales) {
      // DLSS at 1:1 goes by DLAA.
      CHECK((note.find(upscaling) != std::string::npos ||
             (upscaling == "DLSS" && note.find("DLAA") != std::string::npos)));
    } else {
      CHECK(note.find("upscaled to") == std::string::npos);
      CHECK(note.find("shown at that size with") == std::string::npos);
    }
  }
}

TEST_CASE("FSR 1 upscales after MSAA and post AA, which stay, and SHARPNESS sets its sharpening",
          "[graphics_page]") {
  const auto path = TempFile("fsr1.toml");
  std::filesystem::remove(path);
  config::HostConfig config(path);
  const CvarsRestored restored({"swap_post_effect", "fm4_race_msaa", "resolution_scale",
                                "present_upscaler_sharpness", "present_fsr_sharpness_reduction"});
  rex::cvar::SetFlagByName("swap_post_effect", "fxaa");
  rex::cvar::SetFlagByName("fm4_race_msaa", "4x");
  {
    // Like the others, FSR 1 only upscales: choosing it lowers 3x to the 2x that fits 1440p.
    const Upscaling upscaling("bilinear", false);
    rex::cvar::SetFlagByName("resolution_scale", "3");
    auto services = Services();
    services.draw_resolution_scale = [] { return 3u; };
    const auto page = ui::CreateGraphicsPage(config, services);
    Choose(*Find(*page, "UPSCALER"), "FSR 1");
    CHECK(Find(*page, "RESOLUTION")->value() == "1440p (QHD)");
    CHECK(Find(*page, "UPSCALER")->note() ==
          "Resolution lowered to 1440p (QHD) so FSR 1 can run on this 2560x1440 screen.");
  }
  const Upscaling upscaling("fsr", false);
  rex::cvar::SetFlagByName("resolution_scale", "1");
  const auto page = ui::CreateGraphicsPage(config, Services());
  CHECK(Find(*page, "UPSCALER")->note() ==
        "Renders 1280x720, upscaled to 2560x1440 by FSR 1.");
  CHECK(Find(*page, "POST AA")->value() == "FXAA");
  CHECK(Find(*page, "POST AA")->is_enabled());
  CHECK(Find(*page, "RACE MSAA")->value() == "4X");
  CHECK(Find(*page, "RACE MSAA")->is_enabled());
  CHECK(Find(*page, "SHARPNESS")->is_enabled());
  Choose(*Find(*page, "SHARPNESS"), "80 %");
  page->Handle(NavCommand::kApply);
  Settle(*page);
  CHECK(rex::cvar::GetFlagByName("present_upscaler_sharpness") == "80");
  CHECK(std::abs(std::stod(rex::cvar::GetFlagByName("present_fsr_sharpness_reduction")) - 0.4) <
        1e-6);
  std::filesystem::remove(path);
}

TEST_CASE("A saved FSR 1 at a scale larger than the screen is idle, without taking the "
          "anti-aliasing",
          "[graphics_page]") {
  config::HostConfig config(TempFile("idle_fsr1.toml"));
  const Upscaling upscaling("fsr", false);
  const CvarsRestored restored({"resolution_scale"});
  rex::cvar::SetFlagByName("resolution_scale", "3");
  auto services = Services();
  services.draw_resolution_scale = [] { return 3u; };
  const auto page = ui::CreateGraphicsPage(config, services);
  CHECK(Find(*page, "UPSCALER")->value() == "FSR 1 (OFF)");
  CHECK(Find(*page, "UPSCALER")->note() ==
        "FSR 1 runs up to 1440p (QHD) on this 2560x1440 screen. At 4K (2160p) the picture is downscaled without "
        "it.");
  CHECK_FALSE(Find(*page, "SHARPNESS")->is_enabled());
  CHECK(Find(*page, "RACE MSAA")->is_enabled());
}

TEST_CASE("A preset chosen while an upscaler greys the anti-aliasing rows sets them for later",
          "[graphics_page]") {
  config::HostConfig config(TempFile("greyed_preset.toml"));
  const Upscaling upscaling("bilinear", false);
  const CvarsRestored restored({"swap_post_effect", "fm4_showroom_msaa", "fm4_race_msaa"});
  rex::cvar::SetFlagByName("swap_post_effect", "none");
  rex::cvar::SetFlagByName("fm4_showroom_msaa", "4x");
  rex::cvar::SetFlagByName("fm4_race_msaa", "2x");
  const auto page = ui::CreateGraphicsPage(config, Services());
  const MenuRow& upscaler = *Find(*page, "UPSCALER");
  Choose(upscaler, "FSR 3");
  Choose(*Find(*page, "GRAPHICS PRESET"), "EXPERIMENTAL");
  CHECK(Find(*page, "POST AA")->value() == "OFF (FSR 3)");
  CHECK(Find(*page, "RACE MSAA")->unapplied());
  Choose(upscaler, "OFF");
  CHECK(Find(*page, "POST AA")->value() == "FXAA EXTREME");
  CHECK(Find(*page, "SHOWROOM MSAA")->value() == "4X");
  CHECK(Find(*page, "RACE MSAA")->value() == "4X");
  CHECK(Find(*page, "GRAPHICS PRESET")->value() == "EXPERIMENTAL");
  page->Handle(NavCommand::kApply);
  Settle(*page);
  CHECK(rex::cvar::GetFlagByName("swap_post_effect") == "fxaa_extreme");
  CHECK(rex::cvar::GetFlagByName("fm4_race_msaa") == "4x");
  CHECK(rex::cvar::GetFlagByName("present_effect") == "bilinear");
}

TEST_CASE("A greyed row cannot take the focus, so neither the pad nor the mouse changes it", "[graphics_page]") {
  config::HostConfig config(TempFile("greyed_input.toml"));
  const Upscaling upscaling("fsr3", true);
  const auto page = ui::CreateGraphicsPage(config, Services());
  const auto& rows = page->rows();
  const size_t race_msaa = size_t(std::find_if(rows.begin(), rows.end(), [](const MenuRow& row) {
                                    return row.label == "RACE MSAA";
                                  }) -
                                  rows.begin());
  CHECK_FALSE(page->SetFocus(race_msaa));
  CHECK(page->rows()[page->focus()].label != "RACE MSAA");
  CHECK(page->Handle(NavCommand::kApply) == MenuScreen::Result::kChanged);
  CHECK_FALSE(page->status().busy);
  CHECK(page->status().text.empty());
}

TEST_CASE("A saved upscaler that cannot run at the saved scale says so on its row and the "
          "scale's",
          "[graphics_page]") {
  config::HostConfig config(TempFile("idle_upscaler.toml"));
  const Upscaling upscaling("dlss", false);
  const CvarsRestored restored({"resolution_scale"});
  rex::cvar::SetFlagByName("resolution_scale", "3");
  auto services = NvidiaServices();
  services.draw_resolution_scale = [] { return 3u; };
  const auto page = ui::CreateGraphicsPage(config, services);
  const MenuRow& upscaler = *Find(*page, "UPSCALER");
  const MenuRow& scale = *Find(*page, "RESOLUTION");
  const std::string idle =
      "DLSS runs up to 1440p (QHD) on this 2560x1440 screen. At 4K (2160p) the picture is downscaled without it, "
      "with MSAA and post AA instead.";
  CHECK(upscaler.value() == "DLSS (OFF)");
  CHECK(upscaler.note() == idle);
  CHECK(scale.note() == "Renders 3840x2160, downscaled to 2560x1440. " + idle);
  CHECK(Find(*page, "RACE MSAA")->is_enabled());
  CHECK_FALSE(Find(*page, "SHARPNESS")->is_enabled());
  // A screen smaller than 1280x720 leaves the upscaler no scale at all, and limits none.
  rex::cvar::SetFlagByName("resolution_scale", "1");
  services.draw_resolution_scale = [] { return 1u; };
  services.output_size = [] { return std::optional(std::pair{1024u, 576u}); };
  const auto small = ui::CreateGraphicsPage(config, services);
  CHECK(Find(*small, "UPSCALER")->value() == "DLSS (OFF)");
  CHECK(Find(*small, "UPSCALER")->note() ==
        "DLSS does not run on this 1024x576 screen. At 720p the picture is downscaled without it, "
        "with MSAA and post AA instead.");
  Choose(*Find(*small, "RESOLUTION"), "4K (2160p)");
}

TEST_CASE("With an upscaler chosen, RESOLUTION offers only the scales that fit the screen",
          "[graphics_page]") {
  config::HostConfig config(TempFile("fitting_scales.toml"));
  const CvarsRestored restored({"resolution_scale"});
  rex::cvar::SetFlagByName("resolution_scale", "3");
  auto services = NvidiaServices();
  services.draw_resolution_scale = [] { return 3u; };
  const auto steps = [](const MenuRow& row, int direction, int count) {
    std::vector<std::string> values;
    for (int i = 0; i < count; ++i) {
      row.adjust(direction);
      values.push_back(row.value());
    }
    return values;
  };
  {
    // Without an upscaler every scale, supersampling included.
    const Upscaling upscaling("bilinear", false);
    const auto page = ui::CreateGraphicsPage(config, services);
#if defined(__APPLE__)
    CHECK(steps(*Find(*page, "RESOLUTION"), +1, 4) ==
          std::vector<std::string>{"720p", "1080p (Full HD)", "1440p (QHD)", "4K (2160p)"});
    CHECK(steps(*Find(*page, "RESOLUTION"), -1, 4) ==
          std::vector<std::string>{"1440p (QHD)", "1080p (Full HD)", "720p", "4K (2160p)"});
#else
    CHECK(steps(*Find(*page, "RESOLUTION"), +1, 4) ==
          std::vector<std::string>{"5K (2880p)", "720p", "1440p (QHD)", "4K (2160p)"});
#endif
  }
  {
    // A saved 3x under DLSS steps into the fitting 1x and 2x and stays there.
    const Upscaling upscaling("dlss", false);
    const auto page = ui::CreateGraphicsPage(config, services);
    const MenuRow& scale = *Find(*page, "RESOLUTION");
#if defined(__APPLE__)
    CHECK(steps(scale, +1, 4) == std::vector<std::string>{"720p", "1080p (Full HD)", "1440p (QHD)", "720p"});
    CHECK(steps(scale, -1, 3) == std::vector<std::string>{"1440p (QHD)", "1080p (Full HD)", "720p"});
#else
    CHECK(steps(scale, +1, 3) == std::vector<std::string>{"720p", "1440p (QHD)", "720p"});
    CHECK(steps(scale, -1, 2) == std::vector<std::string>{"1440p (QHD)", "720p"});
#endif
    CHECK(Find(*page, "UPSCALER")->value() == "DLSS");
  }
  {
    // On a 4K screen 3x fits too.
    const Upscaling upscaling("fsr3", true);
    auto uhd = services;
    uhd.output_size = [] { return std::optional(std::pair{3840u, 2160u}); };
    const auto page = ui::CreateGraphicsPage(config, uhd);
#if defined(__APPLE__)
    const std::vector<std::string> expected{"720p", "1080p (Full HD)", "1440p (QHD)", "4K (2160p)"};
    CHECK(steps(*Find(*page, "RESOLUTION"), +1, 4) == expected);
    uhd.output_size = [] { return std::optional(std::pair{5120u, 2880u}); };
    const auto large_display = ui::CreateGraphicsPage(config, uhd);
    CHECK(steps(*Find(*large_display, "RESOLUTION"), +1, 4) == expected);
#else
    CHECK(steps(*Find(*page, "RESOLUTION"), +1, 3) ==
          std::vector<std::string>{"720p", "1440p (QHD)", "4K (2160p)"});
#endif
  }
}

TEST_CASE("Choosing an upscaler over a scale above the screen drafts the scale down and says so",
          "[graphics_page]") {
  config::HostConfig config(TempFile("lowered_scale.toml"));
  const Upscaling upscaling("bilinear", false);
  const CvarsRestored restored({"resolution_scale"});
  rex::cvar::SetFlagByName("resolution_scale", "3");
  auto services = NvidiaServices();
  services.draw_resolution_scale = [] { return 3u; };
  const auto page = ui::CreateGraphicsPage(config, services);
  const MenuRow& upscaler = *Find(*page, "UPSCALER");
  const MenuRow& scale = *Find(*page, "RESOLUTION");
  upscaler.adjust(-1);
  CHECK(upscaler.value() == "DLSS");
  CHECK(scale.value() == "1440p (QHD)");
  CHECK(scale.unapplied());
  CHECK(upscaler.unapplied());
  CHECK(upscaler.note() == "Resolution lowered to 1440p (QHD) so DLSS can run on this 2560x1440 "
                           "screen.");
  CHECK(Find(*page, "POST AA")->value() == "OFF (DLSS)");
  CHECK(rex::cvar::GetFlagByName("resolution_scale") == "3");
  // The next change drops the note; turning the upscaler off keeps the lowered scale.
  upscaler.adjust(+1);
  CHECK(upscaler.value() == "OFF");
  CHECK(scale.value() == "1440p (QHD)");
  CHECK(upscaler.note().empty());
  // Discarding leaves the original 4K as it was.
  CHECK(page->Handle(NavCommand::kBack) == MenuScreen::Result::kChanged);
  CHECK(page->Handle(NavCommand::kBack) == MenuScreen::Result::kBack);
  CHECK(rex::cvar::GetFlagByName("resolution_scale") == "3");
}

TEST_CASE("Keys keep the focus in the middle of a long list", "[host_menu]") {
  using fm4_static_recomp::hostui::FirstVisibleRow;
  CHECK(FirstVisibleRow(30, 0, 13, 3, std::nullopt) == 0);
  CHECK(FirstVisibleRow(30, 0, 13, 10, std::nullopt) == 4);
  CHECK(FirstVisibleRow(30, 0, 13, 29, std::nullopt) == 17);
  CHECK(FirstVisibleRow(10, 0, 13, 9, std::nullopt) == 0);
}

TEST_CASE("Pointing at a row does not scroll the list", "[host_menu]") {
  using fm4_static_recomp::hostui::FirstVisibleRow;
  CHECK(FirstVisibleRow(30, 0, 13, 12, 0) == 0);
  CHECK(FirstVisibleRow(30, 0, 13, 16, 4) == 4);
  // A focus the kept list no longer shows (a key moved it) centres again.
  CHECK(FirstVisibleRow(30, 0, 13, 20, 4) == 14);
}

TEST_CASE("Graphics save failure retains the draft and the original file until retry or discard",
          "[graphics_page]") {
  const auto path = TempFile("retry_graphics.toml");
  const std::filesystem::path obstacle(path.string() + ".tmp");
  const std::string previous = "fm4_frame_rate = 60\n";
  REQUIRE(config::WriteAtomically(path, previous));
  REQUIRE(std::filesystem::create_directory(obstacle));
  config::HostConfig config(path);
  REQUIRE(config.Load());
  const CvarsRestored restored({"fm4_frame_rate"});
  REQUIRE(rex::cvar::SetFlagByName("fm4_frame_rate", "60"));
  auto page = ui::CreateGraphicsPage(config, Services());
  const auto* rate = Find(*page, "FRAME RATE");
  REQUIRE(rate);
  rate->slider->set(90);
  page->Handle(NavCommand::kApply);
  Settle(*page);
  CHECK(page->status().warning);
  CHECK(page->status().text.find("SAVE FAILED") != std::string::npos);
  CHECK(rate->unapplied());
  CHECK(rate->slider->get() == 90);
  CHECK(config.Get("fm4_frame_rate") == "60");
  CHECK(rex::cvar::GetFlagByName("fm4_frame_rate") == "60");
  CHECK(config::ReadFile(path) == previous);
  CHECK(std::filesystem::is_directory(obstacle));
  SECTION("another change followed by retry saves the new draft") {
    rate->slider->set(75);
    std::filesystem::remove(obstacle);
    page->Handle(NavCommand::kApply);
    Settle(*page);
    CHECK_FALSE(rate->unapplied());
    CHECK(config.Get("fm4_frame_rate") == "75");
    CHECK(rex::cvar::GetFlagByName("fm4_frame_rate") == "75");
    CHECK(config::ReadFile(path) == "fm4_frame_rate = 75\n");
  }
  SECTION("discard leaves storage, runtime and a reopened page at the saved setting") {
    CHECK(page->Handle(NavCommand::kBack) == MenuScreen::Result::kBack);
    page->Update(true);
    auto reopened = ui::CreateGraphicsPage(config, Services());
    CHECK(Find(*reopened, "FRAME RATE")->slider->get() == 60);
    CHECK(config.Get("fm4_frame_rate") == "60");
    CHECK(config::ReadFile(path) == previous);
    std::filesystem::remove(obstacle);
  }
  std::filesystem::remove(path);
}

TEST_CASE("Frame rate guidance follows the reported display or clearly labels the unknown limit",
          "[graphics_page]") {
  config::HostConfig config(TempFile("display_rate.toml"));
  for (uint32_t hz : {0u, 60u, 120u, 144u}) {
    auto services = Services();
    services.display_refresh_hz = [hz] { return hz; };
    const auto page = ui::CreateGraphicsPage(config, services);
    const auto* row = Find(*page, "FRAME RATE");
    REQUIRE(row);
    CHECK(row->slider->range().second == int(hz ? hz : 240));
    if (hz) {
      CHECK(row->note().find(std::to_string(hz)) != std::string::npos);
    } else {
      CHECK(row->note().find("unavailable") != std::string::npos);
    }
  }
}
