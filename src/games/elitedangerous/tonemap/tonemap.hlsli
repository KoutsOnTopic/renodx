#include "../common.hlsli"
#include "./psychov/customtest31.hlsli"
#include "./agx.hlsli"

static const float MID_GRAY_IN = 0.119121851127f;
static const float MID_GRAY_OUT = 0.163979921774f;
static const float MID_GRAY_SLOPE = 1.95752308422f;

float3 ApplyAdaptiveMBPurity(float3 lms_input, float3 adaptive_neutral_lms, float purity_scale) {
  if (abs(purity_scale - 1.f) <= 1e-5f) return lms_input;

  float3 relative_weighted = renodx::tonemap::psychov::psycho17_ToAdaptiveRelativeWeightedLMS(lms_input, adaptive_neutral_lms);
  float3 mb = renodx::color::macleod_boynton::from::WeightedLMS(relative_weighted);
  float3 mb_neutral = renodx::color::macleod_boynton::from::LMS(1.f.xxx);
  float2 mb_scaled_xy = lerp(mb_neutral.xy, mb.xy, purity_scale);
  float3 relative_weighted_out = renodx::color::macleod_boynton::WeightedLMSFromMacleodBoynton(float3(mb_scaled_xy, mb.z));

  return renodx::color::macleod_boynton::UnweighLMS(
      renodx::tonemap::psychov::psycho17_FromAdaptiveRelativeWeightedLMS(relative_weighted_out, adaptive_neutral_lms));
}

#define CONTRAST_AND_FLARE_GENERATOR(T)                                                                       \
  T ContrastAndFlare(T x, float contrast, float flare, T mid_gray_in = (T)0.18f, T mid_gray_out = (T)0.18f) { \
    T x_normalized = x / mid_gray_in;                                                                         \
    T flare_ratio = (T)1.f + renodx::math::DivideSafe(flare, x_normalized + flare, (T)0.f);                   \
    return pow(x_normalized, contrast * flare_ratio) * mid_gray_out;                                          \
  }

CONTRAST_AND_FLARE_GENERATOR(float)
CONTRAST_AND_FLARE_GENERATOR(float3)
#undef CONTRAST_AND_FLARE_GENERATOR

float3 ApplyLuminanceGradingLMS(float3 color_lms, float exposure, float highlights, float shadows, float contrast, float flare, float mid_gray_yf = 0.18f) {
  float yf = max(0.f, renodx::color::yf::from::LMS(color_lms));
  float yf_adjusted = yf * exposure;
  yf_adjusted = renodx::color::grade::Highlights(yf_adjusted, highlights, mid_gray_yf);
  yf_adjusted = renodx::color::grade::Shadows(yf_adjusted, shadows, mid_gray_yf);
  yf_adjusted = ContrastAndFlare(yf_adjusted, contrast, flare, mid_gray_yf, mid_gray_yf);
  return color_lms * renodx::math::DivideSafe(yf_adjusted, yf, 1.f);
}

float3 ApplyLuminanceGradingBT2020(float3 color_bt2020, float exposure, float highlights, float shadows, float contrast, float flare, float mid_gray_yf = 0.18f) {
  float3 color_lms = renodx::color::lms::from::BT2020(color_bt2020);
  color_lms = ApplyLuminanceGradingLMS(color_lms, exposure, highlights, shadows, contrast, flare, mid_gray_yf);
  return renodx::color::bt2020::from::LMS(color_lms);
}

float3 ApplyPurityGradingLMS(float3 color_lms, float purity_scale, float highlight_saturation, float dechroma, float3 adaptive_neutral_lms = 0.18f) {
  if (purity_scale == 1.f && highlight_saturation == 1.f && dechroma == 0.f) return color_lms;

  if (dechroma != 0.f || highlight_saturation != 1.f) {
    float luminance = renodx::color::yf::from::LMS(color_lms);
    float neutral_luminance = renodx::color::yf::from::LMS(adaptive_neutral_lms);

    // Ramp purity grading over 2.75 decades above the adaptive neutral.
    static const float INVERSE_HIGHLIGHT_RANGE_STOPS = 1.f / (2.75f * log2(10.f));
    static const float HIGHLIGHT_ROLLOFF_CUBIC_BLEND = 0.5f;
    static const float HIGHLIGHT_PURITY_STRENGTH = 2.f / 3.f;

    float luminance_from_neutral = max(luminance, neutral_luminance) / neutral_luminance;
    float rolloff_position = saturate(log2(luminance_from_neutral) * INVERSE_HIGHLIGHT_RANGE_STOPS);

    float rolloff_position_squared = rolloff_position * rolloff_position;
    float rolloff = rolloff_position_squared * rolloff_position * mad(rolloff_position, mad(6.f, rolloff_position, -15.f), 10.f);

    // Base smootherstep brings dechroma into the midtones while remaining monotonic and C2.
    if (dechroma != 0.f) {
      purity_scale *= mad(-dechroma, rolloff, 1.f);
    }

    if (highlight_saturation != 1.f) {
      // Blend smootherstep squared and cubed for a later, gentler C2 progression.
      float highlight_rolloff = rolloff * rolloff * mad(HIGHLIGHT_ROLLOFF_CUBIC_BLEND, rolloff, 1.f - HIGHLIGHT_ROLLOFF_CUBIC_BLEND);
      purity_scale *= mad(highlight_saturation - 1.f, highlight_rolloff * HIGHLIGHT_PURITY_STRENGTH, 1.f);
    }
  }

  if (purity_scale != 1.f) {
    color_lms = ApplyAdaptiveMBPurity(color_lms, adaptive_neutral_lms, purity_scale);
  }

  return color_lms;
}

float3 ApplyPurityGradingBT2020(float3 color_bt2020, float purity_scale, float highlight_saturation, float dechroma) {
  float3 color_lms = renodx::color::lms::from::BT2020(color_bt2020);
  color_lms = ApplyPurityGradingLMS(color_lms, purity_scale, highlight_saturation, dechroma, renodx::color::lms::from::BT2020(0.18f.xxx));
  return renodx::color::bt2020::from::LMS(color_lms);
}

/// Elite Dangerous vanilla SDR tonemapper.
/// Output is in gamma space.
#define APPLY_VANILLA_TONEMAP_GENERATOR(T)                        \
  T ApplyVanillaTonemap(T untonemapped, float N4 = -1.274e-7f) {  \
    const float N0 = 8.46800041f;                                 \
    const float N1 = 1.0f;                                        \
    const float N2 = -0.00295699993f;                             \
    const float N3 = 0.000100400001f;                             \
                                                                  \
    const float D0 = 8.3604002f;                                  \
    const float D1 = 1.82270002f;                                 \
    const float D2 = 0.218899995f;                                \
    const float D3 = -0.00211700005f;                             \
    const float D4 = 3.67300017e-5f;                              \
                                                                  \
    T x = untonemapped;                                           \
    T numerator = (((x * N0 + N1) * x + N2) * x + N3) * x + N4;   \
    T denominator = (((x * D0 + D1) * x + D2) * x + D3) * x + D4; \
    return max((T)0.0f, numerator / denominator);                 \
  }

APPLY_VANILLA_TONEMAP_GENERATOR(float)
APPLY_VANILLA_TONEMAP_GENERATOR(float3)
#undef APPLY_VANILLA_TONEMAP_GENERATOR

#define APPLY_EXTENDED_VANILLA_TONEMAP_GENERATOR(T)                                         \
  T ApplyExtendedVanillaTonemap(T x) {                                                      \
    const float INFLECTION_X = 0.119121851127f;                                             \
    const float INFLECTION_Y = 0.163979921774f;                                             \
    const float INFLECTION_SLOPE = 1.95752308422f;                                          \
    /* r^2 * log2(e), where r = sqrt(2 * V'(x0) / abs(V'''(x0))). */                        \
    const float NATURAL_RELEASE_EXP2_NUMERATOR = 0.0386582117104f;                          \
                                                                                            \
    /* Vanilla+ uses N4 = 0 to remove the original black clip. */                           \
    T vanilla_gamma = ApplyVanillaTonemap(x, 0.f);                                          \
    T vanilla_linear = pow(vanilla_gamma, 2.2f);                                            \
                                                                                            \
    T distance_from_inflection = max(x - (T)INFLECTION_X, (T)0.f);                          \
    T tangent_linear = mad((T)INFLECTION_SLOPE, distance_from_inflection, (T)INFLECTION_Y); \
    T release_exponent = renodx::math::DivideSafe(                                          \
        (T) - NATURAL_RELEASE_EXP2_NUMERATOR,                                               \
        distance_from_inflection * distance_from_inflection,                                \
        (T) - renodx::math::FLT_MAX);                                                       \
    T release_weight = exp2(release_exponent);                                              \
                                                                                            \
    return max((T)0.f, lerp(vanilla_linear, tangent_linear, release_weight));               \
  }

APPLY_EXTENDED_VANILLA_TONEMAP_GENERATOR(float)
APPLY_EXTENDED_VANILLA_TONEMAP_GENERATOR(float3)
#undef APPLY_EXTENDED_VANILLA_TONEMAP_GENERATOR

float3 ApplyPreLUTToneMapAndGammaEncode(float3 untonemapped) {
  float3 tonemapped_gamma;
  if (RENODX_TONE_MAP_TYPE == 0.f) {
    tonemapped_gamma = ApplyVanillaTonemap(untonemapped);
    // The game's curve ends in saturate(n/d) (original decompile, commit
    // 7f6d1eb8); n/d tends to N0/D0 = 1.0128 above a scene value of about 8.
    // SDR Vanilla clamps like the game; HDR keeps max(0) and is unchanged.
    if (RENODX_OUTPUT_SDR != 0.f) tonemapped_gamma = saturate(tonemapped_gamma);
  } else if (RENODX_TONE_MAP_TYPE == 1.f) {
    float3 tonemapped = ApplyExtendedVanillaTonemap(untonemapped);
    if (RENODX_TONE_MAP_PER_CHANNEL == 0.f) {
      float perch_yf = renodx::color::yf::from::BT709(tonemapped);
      float lum_yf = ApplyExtendedVanillaTonemap(renodx::color::yf::from::BT709(untonemapped));
      tonemapped = renodx::color::correct::Luminance(tonemapped, perch_yf, lum_yf);
    }
    tonemapped_gamma = renodx::color::gamma::Encode(tonemapped, 2.2f);
  } else {
    tonemapped_gamma = renodx::color::gamma::Encode(max(0, untonemapped), 2.2f);
  }

  return tonemapped_gamma;
}

float3 ApplyLUT(float3 lut_input_gamma, Texture3D<float4> lut_texture, SamplerState lut_sampler, float texel_size, float lut_strength) {
  if (CUSTOM_LUT_STRENGTH == 0.f || lut_strength == 0.f) return lut_input_gamma;

  float3 lut_input_linear = renodx::color::gamma::Decode(lut_input_gamma, 2.2f);

  float maxch_scale = 1.f;
  if (RENODX_TONE_MAP_TYPE != 0.f) {
    if (RENODX_TONE_MAP_TYPE == 1.f) {  // Vanilla+
      maxch_scale = ApplyAnchoredCInfinityShoulderMaxChannelScale(lut_input_linear, 1.f, MID_GRAY_OUT, 1.5f);
    } else {  // Custom
      maxch_scale = ApplyAnchoredCInfinityShoulderMaxChannelScale(lut_input_linear, 1.f, MID_GRAY_IN, 1.5f);
    }
    lut_input_linear *= maxch_scale;
    lut_input_gamma = renodx::color::gamma::Encode(lut_input_linear, 2.2f);
  }

  float3 lut_output_gamma;
  if (RENODX_TONE_MAP_TYPE == 0.f) {
    lut_output_gamma = lut_texture.Sample(lut_sampler, saturate(lut_input_gamma) * (1 - texel_size) + (0.5 * texel_size)).rgb;  // LUT
  } else {
    lut_output_gamma = renodx::lut::SampleTetrahedral(lut_texture, lut_input_gamma, uint(1.f / texel_size));
  }
  lut_output_gamma = lerp(lut_input_gamma, lut_output_gamma, lut_strength * CUSTOM_LUT_STRENGTH);  // Blend in LUT as a percentage

  float3 lut_output_linear = renodx::color::gamma::Decode(lut_output_gamma, 2.2f) / maxch_scale;

  return renodx::color::gamma::Encode(lut_output_linear, 2.2f);
}

// The game's clamp after its brightness/contrast step ("remove saturate" in
// the shaders: HDR needs the values above 1). Put back for SDR Vanilla only, so
// that mode is the unmodded image; every other mode tone maps into 0..1 itself.
float3 ApplyVanillaSdrClamp(float3 color) {
  return RENODX_OUTPUT_SDR != 0.f && RENODX_TONE_MAP_TYPE == 0.f ? saturate(color) : color;
}

// Luminance pivot of the game's saturation step. The game uses these weights
// (original decompile); the HDR shaders use yf, whose weights sum to 1.05 and
// so shift greys whenever the game desaturates (cb2[10].z < 1). Put back for
// SDR Vanilla only, so HDR stays as it was.
float VanillaSaturationLuma(float3 color) {
  return RENODX_OUTPUT_SDR != 0.f && RENODX_TONE_MAP_TYPE == 0.f
             ? dot(color, float3(0.308600008, 0.609399974, 0.0820000023))
             : renodx::color::yf::from::BT709(color);
}

// Hue correction in IPT (Ebner and Fairchild 1998), the same shape as the
// library's HueOKLab/HueICtCp (src/shaders/colorcorrect.hlsl), which has no
// IPT version: blend the P-T direction, keep the incorrect colour's I and
// chroma (the length of P-T).
float3 HueIPT(float3 incorrect_color, float3 correct_color, float strength) {
  float3 incorrect_ipt = renodx::color::ipt::from::BT709(incorrect_color);
  const float2 correct_pt = renodx::color::ipt::from::BT709(correct_color).yz;
  const float chroma = length(incorrect_ipt.yz);
  float2 blended_pt = lerp(incorrect_ipt.yz, correct_pt, strength);
  blended_pt *= renodx::math::DivideSafe(chroma, length(blended_pt), 1.f);
  incorrect_ipt.yz = blended_pt;
  return renodx::color::bt709::clamp::AP1(renodx::color::bt709::from::IPT(incorrect_ipt));
}

// AgX Grading (Starlight AgX only; owner, 5 Nov: "grade this as well as I can";
// fixed values since v0.1.3, shared.h AGX_*).
// Runs after AgX and its Hue Shift, on the picture as it will be shown: the
// Hue Shift re-takes the game's hues, so a tint applied before it would be
// overwritten. Works on the gamma-2.2 encoded image (the 8-bit image's own
// scale); everything is float and the game's 8-bit write is the only
// rounding, so it adds no banding. Black (0) and white (1) stay fixed under
// every control, and brightness moves as a ratio of all three channels, so
// hues are kept.
//   Contrast  power curve either side of the pivot (0, pivot and 1 fixed)
//   Shadows / Midtones / Highlights  brightness through degree-4 Bernstein
//             weights peaking at 0.25 / 0.5 / 0.75 of the encoded level
//             (linear about 0.05 / 0.22 / 0.53); scale 0.2 keeps the curve
//             rising even with all three at -1 (slope 0.2 at black)
//   Tints     a pure-chroma offset (the hue's colour minus its luminance)
//             through the same weights, each normalised to peak 1, so zone
//             tints never touch black or white
float3 ApplyAgXGrade(float3 color) {
  if (AGX_CONTRAST == 1.f && all(AGX_ZONE_BRIGHTNESS == 0.f) && all(AGX_ZONE_TINT == 0.f)) return color;
  static const float3 LUMA = float3(0.2126f, 0.7152f, 0.0722f);
  static const float ZONE_BRIGHTNESS_SCALE = 0.2f;
  static const float ZONE_TINT_SCALE = 0.08f;
  static const float3 ZONE_WEIGHT_PEAK = float3(0.421875f, 0.375f, 0.421875f);

  float3 encoded = renodx::color::gamma::Encode(saturate(color), 2.2f);
  const float level = dot(encoded, LUMA);
  float graded = level;

  if (AGX_CONTRAST != 1.f) {
    const float pivot = AGX_CONTRAST_PIVOT;
    graded = graded < pivot ? pivot * pow(graded / pivot, AGX_CONTRAST)
                            : 1.f - (1.f - pivot) * pow((1.f - graded) / (1.f - pivot), AGX_CONTRAST);
  }

  const float t = saturate(graded), u = 1.f - t;
  const float3 zone_weight = float3(4.f * t * u * u * u, 6.f * t * t * u * u, 4.f * t * t * t * u);
  graded += ZONE_BRIGHTNESS_SCALE * dot(zone_weight, AGX_ZONE_BRIGHTNESS);
  encoded *= renodx::math::DivideSafe(graded, level, 1.f);
  // A saturated colour brightened this way can push its strongest channel
  // past 1, and the 8-bit clip would then shift its hue. Instead: scale back
  // to the same hue with that channel at 1, then fade toward white just
  // enough to reach the level the curve asked for (any channel above 1 means
  // the luminance is below it, so the blend stays within 0..1).
  const float peak = max(encoded.r, max(encoded.g, encoded.b));
  if (peak > 1.f) {
    encoded /= peak;
    const float scaled_level = dot(encoded, LUMA);
    encoded = lerp(encoded, 1.f, renodx::math::DivideSafe(saturate(graded) - scaled_level, 1.f - scaled_level, 0.f));
  }

  [unroll]
  for (int zone = 0; zone < 3; ++zone) {
    const float3 hue_rgb = saturate(abs(frac(AGX_ZONE_TINT_HUE[zone] + float3(0.f, 2.f / 3.f, 1.f / 3.f)) * 6.f - 3.f) - 1.f);
    encoded += ZONE_TINT_SCALE * AGX_ZONE_TINT[zone] * (zone_weight[zone] / ZONE_WEIGHT_PEAK[zone]) * (hue_rgb - dot(hue_rgb, LUMA));
  }

  return renodx::color::gamma::Decode(max(encoded, 0.f), 2.2f);
}

// Tone mappers after the game's LUT and grade, gamma 2.2 in and out:
//   1 Vanilla+  the game's curve extended, smooth shoulder up to the peak
//   2 Custom    PsychoV
//   3 RenoDRT   RenoDX's standard tone mapper (SDR only, for comparison)
//   4 ACES      the ACES film curve (SDR only, for comparison)
//   5-8 AgX     Base, Punchy, Golden, Blender-style (SDR only, agx.hlsli)
// In SDR the peak is game white (PeakRatio), PsychoV targets BT.709 instead of
// BT.2020, and the result is clipped to 0..1, which the game's 8-bit image
// would do anyway. Why SDR at all (owner, 5 Nov): the vanilla SDR image looks
// flat (low midtone contrast, dull colour); hooking the same shader as HDR
// gives SDR the scene's full range before the game's curve, and the Color
// Grading sliders with it.
float3 ApplyPostLUTToneMap(float3 untonemapped_gamma) {
  if (RENODX_TONE_MAP_TYPE == 0.f) return untonemapped_gamma;

  float3 untonemapped = renodx::color::gamma::DecodeSafe(untonemapped_gamma, 2.2f);

  float3 tonemapped;
  if (RENODX_TONE_MAP_TYPE == 1.f) {  // Vanilla+
    untonemapped = max(0, renodx::color::bt2020::from::BT709(untonemapped));

    // Apply BT.2020 luminance and purity grading.
    untonemapped = ApplyLuminanceGradingBT2020(untonemapped,
                                               1.f,
                                               RENODX_TONE_MAP_HIGHLIGHTS,
                                               RENODX_TONE_MAP_SHADOWS,
                                               RENODX_TONE_MAP_CONTRAST,
                                               0.10f * pow(RENODX_TONE_MAP_FLARE, 10.f),
                                               MID_GRAY_OUT);
    untonemapped = ApplyPurityGradingBT2020(untonemapped,
                                            RENODX_TONE_MAP_SATURATION,
                                            RENODX_TONE_MAP_HIGHLIGHT_SATURATION,
                                            RENODX_TONE_MAP_DECHROMA);
    // untonemapped = max(untonemapped, 1e-7f);

    untonemapped = renodx::color::bt709::from::BT2020(untonemapped);
    tonemapped = renodx::math::CopySign(
        ApplyAnchoredCInfinityShoulder(abs(untonemapped), PeakRatio(), MID_GRAY_OUT, 1.5f),
        untonemapped);
  } else if (RENODX_TONE_MAP_TYPE >= 5.f) {  // AgX: Base, Punchy, Golden, Blender-style (agx.hlsli)
    // Graded at mid gray 0.18 like the other tone mappers, then exposed so the
    // game curve's inflection (MID_GRAY_IN) comes out at MID_GRAY_OUT through
    // each variant. Exposures solved offline per variant (luminance of a grey
    // input; scratch script agx_k.py, 5 Nov): the looks shift mid gray
    // themselves, Punchy darkens it most.
    static const float AGX_EXPOSURE[4] = {1.094477f, 1.963863f, 0.807903f, 1.094477f};
    const float variant = RENODX_TONE_MAP_TYPE - 5.f;
    // Starlight AgX (6) carries the owner's Contrast and Saturation as factors
    // on the sliders (shared.h), so the sliders' 50 is the owner's picture
    const bool starlight_agx = RENODX_TONE_MAP_TYPE == 6.f;
    float3 graded = renodx::color::grade::UserColorGrading(
        untonemapped * (0.18f / MID_GRAY_IN),
        1.f,
        RENODX_TONE_MAP_HIGHLIGHTS,
        RENODX_TONE_MAP_SHADOWS,
        RENODX_TONE_MAP_CONTRAST * (starlight_agx ? STARLIGHT_AGX_CONTRAST : 1.f),
        RENODX_TONE_MAP_SATURATION * (starlight_agx ? STARLIGHT_AGX_SATURATION : 1.f));
    tonemapped = ApplyAgX(graded * (AGX_EXPOSURE[uint(variant)] * MID_GRAY_IN / 0.18f), variant, AGX_GAMMA);
    // Hue Shift (owner, 5 Nov: AgX turned bright oranges, the ship hologram's
    // outline and a carbon star, red next to the game's look). The game's own
    // curve works per channel and pushes bright oranges strongly toward
    // yellow: at twice mid-white a star orange of hue 22 deg comes out at 46
    // through it and 34 through AgX Punchy (scratch agx_hue.py). The game's
    // hue is the reference, not the light's own: that is redder still, which
    // is why a first version that restored it barely changed anything. Below
    // 100% the hue moves toward the game's curve on the same light (OkLab hue;
    // AgX's lightness and chroma are kept), over the whole image: saturated
    // oranges never reach high luminance, so a luminance ramp missed them.
    // Hue Space (owner, 6 Nov): which colour space defines "the same hue".
    // Each keeps AgX's lightness and chroma in that space and blends only the
    // hue, so they differ in how hues bend toward the game's, not in strength.
    // All are RenoDX's own MIT code: OkLab (Björn Ottosson, public domain or
    // MIT), ICtCp (Dolby, ITU-R BT.2100), IPT (Ebner and Fairchild 1998), and
    // darktable UCS (Aurélien Pierre / darktable project,
    // https://eng.aurelienpierre.com/2022/02/color-saturation-control-for-the-21th-century/,
    // MIT with addenda: credit author, project and URL, and call it "darktable
    // UCS" in the user interface; see src/shaders/color/dtucs.hlsl).
    if (AGX_HUE_SHIFT < 1.f) {
      const float3 game_hue = renodx::color::gamma::Decode(saturate(ApplyVanillaTonemap(untonemapped)), 2.2f);
      // Only where both colours have a hue to compare: where the game's curve
      // clips a bright light to pure white, or either colour is grey or
      // black, there is no hue to take (the blend would follow rounding noise,
      // and darktable UCS divides by zero on grey). Fades out below 2% HSV
      // saturation, leaving AgX's colour there.
      const float max_game = max(game_hue.r, max(game_hue.g, game_hue.b));
      const float max_agx = max(tonemapped.r, max(tonemapped.g, tonemapped.b));
      const float sat_game = renodx::math::DivideSafe(max_game - min(game_hue.r, min(game_hue.g, game_hue.b)), max_game, 0.f);
      const float sat_agx = renodx::math::DivideSafe(max_agx - min(tonemapped.r, min(tonemapped.g, tonemapped.b)), max_agx, 0.f);
      const float strength = (1.f - AGX_HUE_SHIFT) * smoothstep(0.f, 0.02f, min(sat_game, sat_agx));
      if (AGX_HUE_SPACE == 2.f) {
        tonemapped = HueIPT(tonemapped, game_hue, strength);
      } else {
        // renodx::color::correct::Hue methods: 0 OkLab, 1 ICtCp, 2 darktable UCS
        tonemapped = renodx::color::correct::Hue(tonemapped, game_hue, strength,
                                                 AGX_HUE_SPACE == 1.f ? 1 : AGX_HUE_SPACE == 3.f ? 2 : 0);
      }
    }
    if (starlight_agx) tonemapped = ApplyAgXGrade(tonemapped);  // Starlight AgX's own grading
  } else if (RENODX_TONE_MAP_TYPE >= 3.f) {  // RenoDRT, ACES
    // Scaled so the game curve's inflection (MID_GRAY_IN -> MID_GRAY_OUT)
    // lands where each tone mapper expects mid gray, and comes out at the
    // game's level: the comparison is then about contrast and colour, not
    // exposure.
    renodx::tonemap::Config tm_config = renodx::tonemap::config::Create();
    tm_config.type = RENODX_TONE_MAP_TYPE == 3.f ? renodx::tonemap::config::type::RENODRT
                                                 : renodx::tonemap::config::type::ACES;
    tm_config.peak_nits = 100.f * PeakRatio();
    tm_config.game_nits = 100.f;
    tm_config.highlights = RENODX_TONE_MAP_HIGHLIGHTS;
    tm_config.shadows = RENODX_TONE_MAP_SHADOWS;
    tm_config.contrast = RENODX_TONE_MAP_CONTRAST;
    tm_config.saturation = RENODX_TONE_MAP_SATURATION;
    tm_config.mid_gray_value = MID_GRAY_OUT;
    tm_config.mid_gray_nits = 100.f * MID_GRAY_OUT;
    if (RENODX_OUTPUT_SDR != 0.f) tm_config.reno_drt_clamp_color_space = renodx::color::convert::COLOR_SPACE_BT709;
    tonemapped = renodx::tonemap::config::Apply(untonemapped * (0.18f / MID_GRAY_IN), tm_config);
  } else {  // Custom
    tonemapped = renodx::tonemap::psychov::custom_psychotm_test31(
        untonemapped,
        PeakRatio(),
        1.f,
        RENODX_TONE_MAP_HIGHLIGHTS,
        RENODX_TONE_MAP_SHADOWS,
        1.55f * RENODX_TONE_MAP_CONTRAST,
        0.10f * pow(0.85f, 10.f) + 0.10f * pow(RENODX_TONE_MAP_FLARE, 10.f),
        1.f,
        1.f,
        RENODX_TONE_MAP_SATURATION,
        RENODX_TONE_MAP_HIGHLIGHT_SATURATION,
        RENODX_TONE_MAP_DECHROMA,
        MID_GRAY_IN,
        MID_GRAY_OUT,
        0.f,  // SDR EOTF emulation.
        1.f,
        RENODX_OUTPUT_SDR != 0.f ? renodx::tonemap::psychov::CUSTOM_PSYCHO31_TARGET_GAMUT_BT709
                                 : renodx::tonemap::psychov::CUSTOM_PSYCHO31_TARGET_GAMUT_BT2020,
        1.5f,
        1.f,   // Mean-A2 shadow source weight.
        0.5f,  // Mean-A2 midgray source weight.
        0.f,   // Mean-A2 highlight source weight.
        renodx::tonemap::psychov::PSYCHO30_SOURCE_BOUNDARY_BT709,
        1.f);
  }

  if (RENODX_OUTPUT_SDR != 0.f) tonemapped = saturate(tonemapped);
  float3 encoded = renodx::color::gamma::EncodeSafe(tonemapped, 2.2f);
  // sRGB Shadow Correction (AgX, SDR only): a monitor decoding with the sRGB
  // curve shows the darkest tones lighter than gamma 2.2 (its linear toe).
  // Encoding with the sRGB curve instead makes it show gamma 2.2; the setting
  // blends toward that, darkening the shadows only (the curves meet near mid
  // gray and differ by under 1% above it).
  if (RENODX_OUTPUT_SDR != 0.f && RENODX_TONE_MAP_TYPE >= 5.f && AGX_SRGB_SHADOWS != 0.f) {
    encoded = lerp(encoded, renodx::color::srgb::EncodeSafe(tonemapped), AGX_SRGB_SHADOWS);
  }
  return encoded;
}
