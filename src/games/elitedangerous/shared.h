#ifndef SRC_ELITEDANGEROUS_SHARED_H_
#define SRC_ELITEDANGEROUS_SHARED_H_

// Must be 32bit aligned
// Should be 4x32
struct ShaderInjectData {
  float peak_white_nits;
  float diffuse_white_nits;
  float graphics_white_nits;
  float tone_map_type;
  float tone_map_highlights;
  float tone_map_shadows;
  float tone_map_contrast;
  float tone_map_saturation;
  float tone_map_highlight_saturation;
  float tone_map_dechroma;
  float tone_map_flare;

  float tone_map_per_channel;
  float custom_lut_strength;
  float custom_bloom;
  float custom_random;
  float custom_grain_strength;

  // SDR output (Output = SDR, or Auto on an SDR display): 1 when this session
  // runs the game's own SDR swap chain. Set once at launch, never by a setting.
  float output_sdr;
  // The SDR session's own tone mapper, separate from the HDR one so that each
  // output keeps its choice. Setting "SDRToneMapper" saves a menu position (0
  // Vanilla, 1 Starlight AgX, 2 Vanilla+, 3 Custom) that its parse maps to this
  // value: 0 Vanilla (the game's own image), 1 Vanilla+, 2 Custom (PsychoV),
  // 3 RenoDRT, 4 ACES, 5-8 AgX (Base, Punchy, Golden, Blender-style); only 0,
  // 1, 2 and 6 are in the menu. 6 is shown as "Starlight AgX": AgX Punchy with
  // the fixed look below.
  float sdr_tone_map_type;

  // Star Brightness and Glow Brightness (settings in the Starlight section,
  // shaders in star/): factors on the drawn star's body (disc, hotspot, edge)
  // and its glow (corona, lens flares). 1 = vanilla. The light the star casts
  // is not touched (it comes from GlobalViewData, not from these draws).
  float star_brightness;
  float glow_brightness;
};

#ifndef __cplusplus
cbuffer cb13 : register(b13) {
  ShaderInjectData shader_injection : packoffset(c0);
}

#define RENODX_OUTPUT_SDR           shader_injection.output_sdr
// Starlight AgX (SDR tone mapper 6, tonemap.hlsli AgX branch): the owner's
// tuned look, fixed for v0.1.3 (owner, 9 Nov: "have it be the default look,
// and hide the sliders"). Until then these were sliders; the values are the
// owner's slider settings put through the old parse functions, so the
// picture is unchanged:
//   AgX Gamma 2.6, sRGB Shadow Correction 0, Hue Shift 0 (the game's hues),
//   Hue Space darktable UCS, Shadows 31, Midtones 63, Highlights 67,
//   Contrast 53, Contrast Pivot 0, tints 50 (none),
//   Color Grading Contrast 46 and Saturation 54.
// Brightness sliders were (v - 50) / 50, Contrast exp2((v - 50) / 50), Pivot 0
// the encoded level 0.15. The two Color Grading values are factors on the
// user's own Contrast and Saturation sliders, so those sliders' default 50
// gives the owner's picture and moving one adjusts from there.
// Gamma, Hue Shift and Hue Space also apply to the AgX variants hidden from
// the menu (5, 7, 8); the grading and the two factors are Punchy's only.
#define AGX_GAMMA                   2.6f
#define AGX_SRGB_SHADOWS            0.f
#define AGX_HUE_SHIFT               0.f
#define AGX_HUE_SPACE               3.f  // 0 OkLab, 1 ICtCp, 2 IPT, 3 darktable UCS (named in the UI: its licence requires it)
#define AGX_ZONE_BRIGHTNESS         float3(-0.38f, 0.26f, 0.34f)  // shadows, midtones, highlights
#define AGX_ZONE_TINT_HUE           float3(0.5f, 0.5f, 0.5f)
#define AGX_ZONE_TINT               float3(0.f, 0.f, 0.f)
#define AGX_CONTRAST                1.0424657f  // exp2(0.06)
#define AGX_CONTRAST_PIVOT          0.15f
#define STARLIGHT_AGX_CONTRAST      0.92f
#define STARLIGHT_AGX_SATURATION    1.08f
#define STAR_BRIGHTNESS             shader_injection.star_brightness
#define GLOW_BRIGHTNESS             shader_injection.glow_brightness
#define RENODX_TONE_MAP_TYPE        (shader_injection.output_sdr != 0.f ? shader_injection.sdr_tone_map_type : shader_injection.tone_map_type)
#define RENODX_PEAK_WHITE_NITS      shader_injection.peak_white_nits
#define RENODX_DIFFUSE_WHITE_NITS   shader_injection.diffuse_white_nits
#define RENODX_GRAPHICS_WHITE_NITS  shader_injection.graphics_white_nits
#define RENODX_TONE_MAP_PER_CHANNEL shader_injection.tone_map_per_channel

#define RENODX_TONE_MAP_HIGHLIGHTS           shader_injection.tone_map_highlights
#define RENODX_TONE_MAP_SHADOWS              shader_injection.tone_map_shadows
#define RENODX_TONE_MAP_CONTRAST             shader_injection.tone_map_contrast
#define RENODX_TONE_MAP_SATURATION           shader_injection.tone_map_saturation
#define RENODX_TONE_MAP_HIGHLIGHT_SATURATION shader_injection.tone_map_highlight_saturation
#define RENODX_TONE_MAP_DECHROMA             shader_injection.tone_map_dechroma
#define RENODX_TONE_MAP_FLARE                shader_injection.tone_map_flare
#define CUSTOM_LUT_STRENGTH                  shader_injection.custom_lut_strength

#define CUSTOM_BLOOM          shader_injection.custom_bloom
#define CUSTOM_RANDOM         shader_injection.custom_random
#define CUSTOM_GRAIN_STRENGTH shader_injection.custom_grain_strength

#include "../../shaders/renodx.hlsl"

#endif

#endif  // SRC_ELITEDANGEROUS_SHARED_H_