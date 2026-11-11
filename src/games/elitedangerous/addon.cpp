/*
 * Copyright (C) 2023 Musa Haji
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#define DEBUG_LEVEL_0

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include <embed/shaders.h>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../utils/date.hpp"
#include "../../utils/random.hpp"
#include "../../utils/settings.hpp"
#include "./shared.h"
#include "./starlight.hpp"

namespace {

ShaderInjectData shader_injection;

thread_local uint64_t g_current_uav0 = 0;

#define UpgradeRTVReplaceShader(value)                                                                         \
  {                                                                                                            \
      value,                                                                                                   \
      {                                                                                                        \
          .crc32 = value,                                                                                      \
          .code = __##value,                                                                                   \
          .on_draw = [](auto* cmd_list) {                                                                      \
            auto rtvs = renodx::utils::swapchain::GetRenderTargets(cmd_list);                                  \
            bool changed = false;                                                                              \
            for (auto rtv : rtvs) {                                                                            \
              changed = renodx::mods::swapchain::ActivateCloneHotSwap(cmd_list->get_device(), rtv) || changed; \
            }                                                                                                  \
            if (changed) {                                                                                     \
              renodx::mods::swapchain::FlushDescriptors(cmd_list);                                             \
              renodx::mods::swapchain::RewriteRenderTargets(cmd_list, rtvs.size(), rtvs.data(), {0});          \
            }                                                                                                  \
            return true;                                                                                       \
          },                                                                                                   \
      },                                                                                                       \
  }

#define UpgradeRTVShader(value)                                                                                \
  {                                                                                                            \
      value,                                                                                                   \
      {                                                                                                        \
          .crc32 = value,                                                                                      \
          .on_draw = [](auto* cmd_list) {                                                                      \
            auto rtvs = renodx::utils::swapchain::GetRenderTargets(cmd_list);                                  \
            bool changed = false;                                                                              \
            for (auto rtv : rtvs) {                                                                            \
              changed = renodx::mods::swapchain::ActivateCloneHotSwap(cmd_list->get_device(), rtv) || changed; \
            }                                                                                                  \
            if (changed) {                                                                                     \
              renodx::mods::swapchain::FlushDescriptors(cmd_list);                                             \
              renodx::mods::swapchain::RewriteRenderTargets(cmd_list, rtvs.size(), rtvs.data(), {0});          \
            }                                                                                                  \
            return true;                                                                                       \
          },                                                                                                   \
      },                                                                                                       \
  }

#define UpgradeUAVShader(value)                                                                        \
  {                                                                                                    \
      value,                                                                                           \
      {                                                                                                \
          .crc32 = value,                                                                              \
          .on_draw = [](auto* cmd_list) {                                                              \
            reshade::api::resource_view current_uav0 = {g_current_uav0};                               \
            if (current_uav0.handle == 0) return true;                                                 \
            if (renodx::mods::swapchain::ActivateCloneHotSwap(cmd_list->get_device(), current_uav0)) { \
              renodx::mods::swapchain::FlushDescriptors(cmd_list);                                     \
            }                                                                                          \
            return true;                                                                               \
          },                                                                                           \
      },                                                                                               \
  }

// The factor a star draw is scaled by (star/*.hlsl, shared.h): the body
// (disc, hotspot, edge) by Star Brightness, the corona and lens flares by
// Glow Brightness.
float StarDrawFactor(uint32_t crc) {
  using elitedangerous::starlight::StarFx;
  const StarFx fx = elitedangerous::starlight::kStarShaders.at(crc).fx;
  return (fx == StarFx::Corona || fx == StarFx::Flare) ? shader_injection.glow_brightness
                                                       : shader_injection.star_brightness;
}

// Star draws: observed to find which star casts each light, and replaced
// (star/*.hlsl) only to scale the drawn star by Star/Glow Brightness. This
// entry must carry the code: __ALL_CUSTOM_SHADERS also lists these CRCs, and
// the first entry for a CRC wins (DefineCustomShaders / unordered_map).
#define StarlightObserveShader(value)                                                    \
  {                                                                                      \
      value,                                                                             \
      {                                                                                  \
          .crc32 = value,                                                                \
          .code = __##value,                                                             \
          .on_draw = [](reshade::api::command_list* cmd_list) {                          \
            const bool drawn = elitedangerous::starlight::OnStarDraw(cmd_list, value);   \
            /* at 0% skip the draw: scaled to black it could cover what is behind */     \
            return drawn && StarDrawFactor(value) > 0.f;                                 \
          },                                                                             \
      },                                                                                 \
  }

#define STARLIGHT_OBSERVERS                                                              \
  StarlightObserveShader(0xB0ED86B7), /* StarOpaqueNoise (surface) */                    \
      StarlightObserveShader(0x7AE10FE9), /* StarOpaqueNoise (surface) */                \
      StarlightObserveShader(0xB7275333), /* SunHotspotQuad */                           \
      StarlightObserveShader(0xDC21D860), /* SunBillboardGlow (corona) */                \
      StarlightObserveShader(0xF1CBC91F), /* SunBillboardGlow (corona) */                \
      StarlightObserveShader(0x509F27CB), /* SunLensFlareBillboard */                    \
      StarlightObserveShader(0xA6457BFD), /* SunLensFlareBillboard */                    \
      StarlightObserveShader(0x82FC416E) /* SunEdgeBillboards */

// SDR output: the star draws, plus the game's own shaders replaced in place
// (tone map, bloom, final copy, video) with no swap chain proxy and no resource
// upgrades, so the image stays the game's 8-bit SDR one. With the SDR Tone
// Mapper on Vanilla the replacements reproduce the game's image (shared.h
// output_sdr); the other tone mappers work before the game's 8-bit clip.
renodx::mods::shader::CustomShaders sdr_shaders = {STARLIGHT_OBSERVERS, __ALL_CUSTOM_SHADERS};

renodx::mods::shader::CustomShaders custom_shaders = {
    STARLIGHT_OBSERVERS,

    // Upgrades
    UpgradeRTVReplaceShader(0xD7E646E3),  // tonemap
    UpgradeRTVReplaceShader(0x6BE69EF3),  // tonemap, no bloom
    UpgradeRTVReplaceShader(0xEBD83636),  // tonemap, no bloom, no lut

    UpgradeRTVShader(0x723CBF3F),  // AA?
    UpgradeRTVShader(0x315EA715),  // AA?

    UpgradeRTVShader(0x7561AC83),  // MLAAx2/x4
    UpgradeRTVShader(0x44FAE038),  // MLAAx2/x4

    UpgradeRTVShader(0x8A421A14),  // FXAA
    UpgradeRTVShader(0x0B263009),  // FXAA

    UpgradeUAVShader(0xF28CD45D),  // FSR1 Compute Shader
    UpgradeUAVShader(0x5647AAC2),  // FSR1 Compute Shader

    __ALL_CUSTOM_SHADERS};

// Output setting: 0 = HDR (RenoDX swapchain + tone mapping), 1 = SDR (the
// game's SDR image, its own SDR tone mapper setting), 2 = Auto (the default: Windows' HDR switch for the
// game's display, read at launch). 0 and 1 predate Auto, so a saved choice
// keeps its meaning.
float output_mode = 2.f;
bool output_sdr_at_start = false;  // what this session runs: decided once in DllMain
bool auto_sdr = false;             // Auto's choice at launch
std::string auto_reading;          // what Auto read, for the settings panel and the log
bool auto_mismatch = false;        // the swap chain's display reads the other way (OnInitSwapchain)

bool SdrSelected() { return output_mode == 2.f ? auto_sdr : output_mode == 1.f; }

// The tone mapper this session uses (shared.h RENODX_TONE_MAP_TYPE): SDR and
// HDR keep separate choices, so SDR can start on Vanilla while HDR stays on
// its own default.
float ToneMapType() { return output_sdr_at_start ? shader_injection.sdr_tone_map_type : shader_injection.tone_map_type; }

// Starlight AgX (SDR tone mapper type 6, shared.h): shows its credit line
bool StarlightAgXShown() { return output_sdr_at_start && ToneMapType() == 6.f; }

// Auto: is Windows HDR on for the game's display? Asked in DllMain, because
// what gets hooked depends on it. ReShade loads add-ons inside the game's first
// D3D11CreateDevice: the game window already exists (found by its class,
// logged as "FrontierDevelopmentsAppWinClass"), the swap chain does not, so the
// display is the one the window is on, or the main display if no window is
// found. Display Config API (user32) only, no DXGI: ReShade is the game's
// dxgi.dll, and making a factory under the loader lock would re-enter it.
// Empty when the display's switch cannot be read; *display names the display.
std::optional<bool> ReadWindowsHdr(std::string* display) {
  HWND window = nullptr;
  EnumWindows(
      [](HWND hwnd, LPARAM out) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        char cls[64] = "";
        if (pid != GetCurrentProcessId() || GetClassNameA(hwnd, cls, sizeof(cls)) == 0
            || std::strcmp(cls, "FrontierDevelopmentsAppWinClass") != 0) {
          return TRUE;
        }
        *reinterpret_cast<HWND*>(out) = hwnd;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&window));
  HMONITOR monitor = window != nullptr ? MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY)
                                       : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFOEX info = {};
  info.cbSize = sizeof(info);
  *display = GetMonitorInfo(monitor, &info) ? std::string(info.szDevice) : std::string("unknown display");
  *display += window != nullptr ? " (game window)" : " (main display, no game window yet)";
  const auto path = renodx::utils::platform::GetPathInfo(monitor);
  if (!path.has_value()) return std::nullopt;
  return renodx::utils::swapchain::GetHDREnabled(path.value());
}

renodx::utils::settings::Settings settings = {
    new renodx::utils::settings::Setting{
        // RenoDX (Vanilla+) is the HDR default since v0.1.3 (owner, 11 Nov);
        // up to v0.1.2 it was Custom under the key "ToneMapType". RenoDX
        // writes every setting of a preset whenever one changes, so a saved
        // "ToneMapType" cannot tell a deliberate Custom from the old default:
        // the new key moves every HDR install to Vanilla+ once, and a choice
        // made from then on is kept. SDR keeps its own key (SDRToneMapper).
        .key = "HDRToneMapper",
        .binding = &shader_injection.tone_map_type,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .label = "Tone Mapper",
        .section = "Tone Mapping",
        .tooltip = "HDR tone mapper. Vanilla+ (the default) is the game's curve with a smooth roll-off;\n"
                   "Custom is RenoDX's PsychoV; Vanilla is the game's own image.",
        .labels = {"Vanilla", "RenoDX (Vanilla+)", "RenoDX (Custom)"},
        .is_visible = []() { return !output_sdr_at_start; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapPeakNits",
        .binding = &shader_injection.peak_white_nits,
        .default_value = 1000.f,
        .can_reset = false,
        .label = "Peak Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the value of peak white in nits",
        .min = 48.f,
        .max = 10000.f,
        .is_enabled = []() { return ToneMapType() != 0; },
        .is_visible = []() { return !output_sdr_at_start; },
        .is_logarithmic = true,
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapGameNits",
        .binding = &shader_injection.diffuse_white_nits,
        .default_value = 203.f,
        .label = "Game Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the value of 100% white in nits",
        .min = 48.f,
        .max = 500.f,
        .is_visible = []() { return !output_sdr_at_start; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapUINits",
        .binding = &shader_injection.graphics_white_nits,
        .default_value = 203.f,
        .label = "UI Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the brightness of UI and HUD elements in nits",
        .min = 48.f,
        .max = 500.f,
        .is_visible = []() { return !output_sdr_at_start; },
    },
    // After the brightness settings: OnInitSwapchain sets settings[1] (Peak
    // Brightness) by index.
    new renodx::utils::settings::Setting{
        // A new key: the menu was cut from nine entries to four (owner, 5 Nov),
        // so positions saved under the old key "SDRToneMapType" would now point
        // at different tone mappers; it is simply no longer read.
        .key = "SDRToneMapper",
        .binding = &shader_injection.sdr_tone_map_type,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        // Starlight AgX is the default (owner, 9 Nov, v0.1.3). The key is new
        // in v0.1.3 (v0.1.2 had no SDR tone mapper), so every SDR install
        // starts on it; Vanilla is one click away.
        .default_value = 1.f,
        .label = "Tone Mapper",
        .section = "Tone Mapping",
        .tooltip = "SDR tone mapper. Vanilla is the game's own image.\n"
                   "The others tone map the scene before the game's SDR clip:\n"
                   "Starlight AgX (the default: AgX with a fixed grade, the game's hues kept in darktable UCS),\n"
                   "Vanilla+ (the game's curve with a smooth roll-off), Custom (PsychoV).\n"
                   "Highlights, Shadows, Contrast and Saturation work with all but Vanilla;\n"
                   "Highlight Saturation, Dechroma and Flare with Vanilla+ and Custom only.",
        // Menu order (owner, 5 Nov): Vanilla, the SDR tone mapper, then the two
        // HDR ones. ACES, RenoDRT and AgX Base/Golden/Blender-style (types 3, 4,
        // 5, 7, 8) are hidden after testing; their shader code stays, so adding
        // a label and a kTypeAt entry brings one back. The saved value is the
        // menu position; parse maps it to the shader's type number (shared.h
        // sdr_tone_map_type), so the shaders never renumber.
        .labels = {"Vanilla", "Starlight AgX", "RenoDX (Vanilla+)", "RenoDX (Custom)"},
        .parse = [](float position) {
          static constexpr float kTypeAt[] = {0.f, 6.f, 1.f, 2.f};
          return kTypeAt[std::clamp(static_cast<int>(position), 0, 3)];
        },
        .is_visible = []() { return output_sdr_at_start; },
    },
    // The darktable UCS licence (MIT with addenda, src/shaders/color/dtucs.hlsl)
    // requires the colour space to be called "darktable UCS" in the user
    // interface; Starlight AgX matches its hues in it, so the name is on screen
    // while it is picked. The other addendum (author, project and URL) is about
    // reusing the code, and is met in dtucs.hlsl and the release README.
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::TEXT,
        .label = "Hues matched in darktable UCS.",
        .section = "Tone Mapping",
        .is_visible = StarlightAgXShown,
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapScaling",
        .binding = &shader_injection.tone_map_per_channel,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .label = "Scaling",
        .section = "Tone Mapping",
        .tooltip = "Luminance scales colors consistently while per-channel saturates and blows out sooner",
        .labels = {"Luminance", "Per Channel"},
        .is_enabled = []() { return ToneMapType() == 1.f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeHighlights",
        .binding = &shader_injection.tone_map_highlights,
        .default_value = 50.f,
        .label = "Highlights",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = []() { return ToneMapType() != 0; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeShadows",
        .binding = &shader_injection.tone_map_shadows,
        .default_value = 50.f,
        .label = "Shadows",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = []() { return ToneMapType() != 0; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeContrast",
        .binding = &shader_injection.tone_map_contrast,
        .default_value = 50.f,
        .label = "Contrast",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = []() { return ToneMapType() != 0; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeSaturation",
        .binding = &shader_injection.tone_map_saturation,
        .default_value = 50.f,
        .label = "Saturation",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = []() { return ToneMapType() != 0; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeHighlightSaturation",
        .binding = &shader_injection.tone_map_highlight_saturation,
        .default_value = 50.f,
        .label = "Highlight Saturation",
        .section = "Color Grading",
        .tooltip = "Adds or removes highlight color.",
        .max = 100.f,
        .is_enabled = []() { return ToneMapType() == 1.f || ToneMapType() == 2.f; },  // Vanilla+ and Custom only
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeDechroma",
        .binding = &shader_injection.tone_map_dechroma,
        .default_value = 0.f,
        .label = "Dechroma",
        .section = "Color Grading",
        .tooltip = "Controls highlight desaturation due to overexposure.",
        .max = 100.f,
        .is_enabled = []() { return ToneMapType() == 1.f || ToneMapType() == 2.f; },  // Vanilla+ and Custom only
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeFlare",
        .binding = &shader_injection.tone_map_flare,
        .default_value = 0.f,
        .label = "Flare",
        .section = "Color Grading",
        .tooltip = "Flare/Glare",
        .max = 100.f,
        .is_enabled = []() { return ToneMapType() == 1.f || ToneMapType() == 2.f; },  // Vanilla+ and Custom only
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeLUTStrength",
        .binding = &shader_injection.custom_lut_strength,
        .default_value = 100.f,
        .label = "LUT Strength",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "FxBloom",
        .binding = &shader_injection.custom_bloom,
        .default_value = 100.f,
        .label = "Bloom",
        .section = "Effects",
        .max = 100.f,
        .is_enabled = []() { return ToneMapType() != 0; },
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "FxGrainStrength",
        .binding = &shader_injection.custom_grain_strength,
        .default_value = 0.f,
        .label = "Grain Strength",
        .section = "Effects",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Reset All",
        .section = "Options",
        .group = "button-line-0",
        .on_change = []() {
          for (auto* setting : settings) {
            if (setting->key.empty()) continue;
            if (!setting->can_reset) continue;
            renodx::utils::settings::UpdateSetting(setting->key, setting->default_value);
          }
        },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Discord",
        .section = "Links",
        .group = "button-line-1",
        .tint = 0x5865F2,
        .on_change = []() {
          renodx::utils::platform::LaunchURL("https://discord.gg/", "5WZXDpmbpP");
        },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "More Mods",
        .section = "Links",
        .group = "button-line-1",
        .tint = 0x2B3137,
        .on_change = []() {
          renodx::utils::platform::LaunchURL("https://github.com/", "clshortfuse/renodx/wiki/Mods");
        },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Github",
        .section = "Links",
        .group = "button-line-1",
        .tint = 0x2B3137,
        .on_change = []() {
          renodx::utils::platform::LaunchURL("https://github.com/", "clshortfuse/renodx");
        },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Musa's Ko-Fi",
        .section = "Links",
        .group = "button-line-2",
        .tint = 0xFF5A16,
        .on_change = []() {
          renodx::utils::platform::LaunchURL("https://ko-fi.com/", "musaqh");
        },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "ShortFuse's Ko-Fi",
        .section = "Links",
        .group = "button-line-2",
        .tint = 0xFF5A16,
        .on_change = []() {
          renodx::utils::platform::LaunchURL("https://ko-fi.com/", "shortfuse");
        },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::TEXT,
        .label = std::string("Build: ") + renodx::utils::date::ISO_DATE_TIME,
        .section = "About",
    },
};
void OnPresetOff() {
  renodx::utils::settings::UpdateSettings({
      {"HDRToneMapper", 0.f},
      {"SDRToneMapper", 0.f},
      {"ToneMapPeakNits", 203.f},
      {"ToneMapGameNits", 203.f},
      {"ToneMapUINits", 203.f},
      {"ToneMapScaling", 0.f},
      {"ColorGradeHighlights", 50.f},
      {"ColorGradeShadows", 50.f},
      {"ColorGradeContrast", 50.f},
      {"ColorGradeSaturation", 50.f},
      {"ColorGradeHighlightSaturation", 50.f},
      {"ColorGradeDechroma", 0.f},
      {"ColorGradeFlare", 0.f},
      {"ColorGradeLUTStrength", 100.f},
      {"FxBloom", 100.f},
      {"FxGrainStrength", 0.f},
      {"StarBrightness", 100.f},
      {"GlowBrightness", 100.f},
  });
}

bool fired_on_init_swapchain = false;

void OnInitSwapchain(reshade::api::swapchain* swapchain, bool resize) {
  if (fired_on_init_swapchain) return;
  fired_on_init_swapchain = true;
  // Auto read the display before the swap chain existed: say so if the swap
  // chain's display has its HDR switch the other way (the window moved to
  // another display after the add-on loaded). Checked whatever the setting,
  // so switching to Auto later in the session shows it too.
  const auto display = renodx::utils::swapchain::GetDisplayInfo(swapchain);
  if (display.display_config.has_value() && display.hdr_enabled == auto_sdr) {
    auto_mismatch = true;
    reshade::log::message(reshade::log::level::warning,
                          "RenoDX Elite: the game's display has Windows HDR the other way from Auto's choice at launch");
  }
  auto peak = renodx::utils::swapchain::GetPeakNits(swapchain);
  if (peak.has_value()) {
    settings[1]->default_value = peak.value();
    settings[1]->can_reset = true;
  }
}

bool initialized = false;

void OnPushDescriptors(
    reshade::api::command_list* cmd_list,
    reshade::api::shader_stage stages,
    reshade::api::pipeline_layout layout,
    uint32_t layout_param,
    const reshade::api::descriptor_table_update& update) {
  if (update.count == 0u) return;
  if (update.type != reshade::api::descriptor_type::texture_unordered_access_view) return;

  for (uint32_t i = 0u; i < update.count; i++) {
    if ((update.binding + i) != 0u) continue;

    g_current_uav0 = static_cast<const reshade::api::resource_view*>(update.descriptors)[i].handle;
    return;
  }
}

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX for Elite Dangerous";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;

      if (!initialized) {
        renodx::mods::shader::expected_constant_buffer_index = 13;
        renodx::mods::shader::force_pipeline_cloning = true;

        // Read even when HDR or SDR is picked: the panel shows the reading, and
        // switching to Auto later needs it for the restart notice
        {
          std::string display;
          const auto hdr = ReadWindowsHdr(&display);
          // unreadable: HDR, the output this addon had before Auto existed
          auto_sdr = hdr.has_value() && !hdr.value();
          auto_reading = !hdr.has_value() ? "could not read Windows HDR for " + display + ", so HDR (pick SDR by hand on an SDR display)"
                                          : "Windows HDR is " + std::string(auto_sdr ? "off" : "on") + " for " + display
                                                + ", so " + (auto_sdr ? "SDR" : "HDR");
        }

        // Output + Starlight panels go just before the Links section
        {
          auto extra = elitedangerous::starlight::Settings();
          // Star and Glow Brightness sit with the Starlight settings, after
          // Saturation. They bind to shader_injection, which lives here, so
          // they are added here rather than in starlight.hpp.
          {
            auto after = std::find_if(extra.begin(), extra.end(), [](auto* s) { return s->key == "StarlightTintChroma"; });
            if (after != extra.end()) {
              ++after;
            }
            extra.insert(after, {
                                    new renodx::utils::settings::Setting{
                                        .key = "StarBrightness",
                                        .binding = &shader_injection.star_brightness,
                                        .default_value = 100.f,
                                        .label = "Star Brightness",
                                        .section = "Starlight",
                                        .tooltip = "Brightness of the star itself: its disc, hotspot and edge.\n"
                                                   "100 is vanilla. The light the star casts does not change.",
                                        .max = 400.f,
                                        .parse = [](float value) { return std::clamp(value, 0.f, 400.f) * 0.01f; },
                                    },
                                    new renodx::utils::settings::Setting{
                                        .key = "GlowBrightness",
                                        .binding = &shader_injection.glow_brightness,
                                        .default_value = 100.f,
                                        .label = "Glow Brightness",
                                        .section = "Starlight",
                                        .tooltip = "Brightness of the star's corona glow and lens flares.\n"
                                                   "100 is vanilla. The light the star casts does not change.",
                                        .max = 400.f,
                                        .parse = [](float value) { return std::clamp(value, 0.f, 400.f) * 0.01f; },
                                    },
                                });
          }
          extra.insert(extra.begin(), {
                                          new renodx::utils::settings::Setting{
                                              .key = "OutputMode",
                                              .binding = &output_mode,
                                              .value_type = renodx::utils::settings::SettingValueType::INTEGER,
                                              .default_value = 2.f,
                                              .can_reset = false,
                                              .label = "Output",
                                              .section = "Output",
                                              .tooltip = "SDR keeps the game's own SDR image (no HDR swapchain or upgrades).\n"
                                                         "It has its own Tone Mapper setting; Vanilla there is the game's own look.\n"
                                                         "Auto picks HDR or SDR at launch from the Windows HDR\n"
                                                         "switch of the display the game is on.\n"
                                                         "Restart the game after changing.",
                                              .labels = {"HDR (RenoDX)", "SDR", "Auto"},
                                              .is_global = true,
                                          },
                                          new renodx::utils::settings::Setting{
                                              .value_type = renodx::utils::settings::SettingValueType::TEXT,
                                              .label = "Auto: " + auto_reading + ".",
                                              .section = "Output",
                                              .is_visible = []() { return output_mode == 2.f && !auto_mismatch; },
                                          },
                                          new renodx::utils::settings::Setting{
                                              .value_type = renodx::utils::settings::SettingValueType::TEXT,
                                              .label = "Auto: " + auto_reading + ", but the game's display has Windows HDR "
                                                       + (auto_sdr ? "on" : "off") + ". Pick HDR or SDR by hand.",
                                              .section = "Output",
                                              .tint = 0xFF5A16,
                                              .is_visible = []() { return output_mode == 2.f && auto_mismatch; },
                                          },
                                          new renodx::utils::settings::Setting{
                                              .value_type = renodx::utils::settings::SettingValueType::TEXT,
                                              .label = "Restart the game to apply the output change.",
                                              .section = "Output",
                                              .tint = 0xFF5A16,
                                              .is_visible = []() { return SdrSelected() != output_sdr_at_start; },
                                          },
                                      });
          auto pos = std::find_if(settings.begin(), settings.end(), [](auto* s) { return s->section == "Links"; });
          settings.insert(pos, extra.begin(), extra.end());
        }

        // Load settings now: the output mode decides what gets hooked
        renodx::utils::settings::Use(DLL_PROCESS_ATTACH, &settings, &OnPresetOff);
        output_sdr_at_start = SdrSelected();
        shader_injection.output_sdr = output_sdr_at_start ? 1.f : 0.f;
        {
          std::string msg = std::string("RenoDX Elite: ") + (output_sdr_at_start ? "SDR output" : "HDR output")
                            + (output_mode == 2.f ? " (Auto: " + auto_reading + ")" : " (picked in settings)");
          reshade::log::message(reshade::log::level::info, msg.c_str());
        }
        if (output_sdr_at_start) initialized = true;  // skip the HDR swapchain setup below
      }
      if (!initialized) {
        // Swapchain Proxy
        renodx::mods::swapchain::SetUseHDR10();
        renodx::mods::swapchain::use_resource_cloning = true;
        renodx::mods::swapchain::expected_constant_buffer_index = 13;
        renodx::mods::swapchain::swap_chain_proxy_vertex_shader = __swap_chain_proxy_vertex_shader;
        renodx::mods::swapchain::swap_chain_proxy_pixel_shader = __swap_chain_proxy_pixel_shader;

        // On Demand Upgrades
        renodx::mods::swapchain::resource_upgrade_infos.push_back({
            .old_format = reshade::api::format::r8g8b8a8_typeless,
            .new_format = reshade::api::format::r16g16b16a16_float,
            .ignore_size = true,
            .use_resource_view_cloning = true,
            .use_resource_view_hot_swap = true,
        });

        renodx::mods::swapchain::resource_upgrade_infos.push_back({
            .old_format = reshade::api::format::r8g8b8a8_typeless,
            .new_format = reshade::api::format::r16g16b16a16_float,
            .ignore_size = true,
            .use_resource_view_cloning = true,
            .use_resource_view_hot_swap = true,
            .usage_include = reshade::api::resource_usage::unordered_access,
        });

        initialized = true;
      }

      reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::register_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);

      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::unregister_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  if (output_sdr_at_start) {
    renodx::mods::shader::Use(fdw_reason, sdr_shaders, &shader_injection);
  } else {
    renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);
    renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
  }
  renodx::utils::random::Use(fdw_reason, {&shader_injection.custom_random});
  elitedangerous::starlight::Use(fdw_reason);

  return TRUE;
}
