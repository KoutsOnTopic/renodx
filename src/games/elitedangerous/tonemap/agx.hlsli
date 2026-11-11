#ifndef RENODX_ELITEDANGEROUS_TONEMAP_AGX_HLSLI_
#define RENODX_ELITEDANGEROUS_TONEMAP_AGX_HLSLI_

// AgX tone mapper variants for the SDR Tone Mapper setting (types 5 to 8, see
// ApplyPostLUTToneMap in tonemap.hlsli). Ported to HLSL from two MIT-licensed
// sources, whose notices follow. Sources checked on 5 Nov 2026; sources without
// a licence (sobotka/AgX, EaryChow's AgX repos) and Blender's GPL config were
// deliberately not used.
//
// ---------------------------------------------------------------------------
// Base, Punchy and Golden: "Minimal AgX", https://iolite-engine.com/blog_posts/minimal_agx_implementation
//
// MIT License
//
// Copyright (c) 2024 Missing Deadlines (Benjamin Wrensch)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// All values used to derive this implementation are sourced from Troy's
// initial AgX implementation/OCIO config file available here:
//   https://github.com/sobotka/AgX
//
// ---------------------------------------------------------------------------
// Blender-style (Rec.2020 primaries): three.js AgXToneMapping,
// src/renderers/shaders/ShaderChunk/tonemapping_pars_fragment.glsl.js
// "AgX Tone Mapping implementation based on Filament, which in turn is based
// on Blender's implementation using rec 2020 primaries",
// https://github.com/google/filament/pull/7236
//
// The MIT License
//
// Copyright (c) 2010-2026 three.js authors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
// ---------------------------------------------------------------------------
//
// Port notes:
// - GLSL mat3(...) lists columns and HLSL float3x3(...) lists rows, so the
//   numbers below are kept in the published order and applied as mul(v, M),
//   which equals GLSL's M * v.
// - All four use the 7th-order curve fit from the Minimal AgX post (mean
//   error^2 1.86e-6; three.js uses the 6th-order one, a near-identical fit).
// - Golden keeps the Minimal AgX post's sat 0.8. Troy Sobotka's original
//   config and Filament use 1.3; change AGX_GOLDEN_SATURATION to try that.
// - Looks run on the curve's output, luminance taken after the CDL, as in the
//   post's 2025-04-20 revision.
// - log2 input is floored at 1e-10 (as three.js does) so black or negative
//   values cannot produce NaN.

static const float AGX_MIN_EV = -12.47393f;  // log2(2^-10 * 0.18)
static const float AGX_MAX_EV = 4.026069f;   // log2(2^6.5 * 0.18)
static const float AGX_GOLDEN_SATURATION = 0.8f;

// Minimal AgX: inset/outset for sRGB (BT.709) primaries
static const float3x3 AGX_INSET_BT709 = float3x3(
    0.842479062253094, 0.0423282422610123, 0.0423756549057051,
    0.0784335999999992, 0.878468636469772, 0.0784336,
    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
static const float3x3 AGX_OUTSET_BT709 = float3x3(
    1.19687900512017, -0.0528968517574562, -0.0529716355144438,
    -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
    -0.0990297440797205, -0.0989611768448433, 1.15107367264116);

// three.js: inset/outset for Rec.2020 primaries (Blender-style)
static const float3x3 AGX_INSET_BT2020 = float3x3(
    0.856627153315983, 0.137318972929847, 0.11189821299995,
    0.0951212405381588, 0.761241990602591, 0.0767994186031903,
    0.0482516061458583, 0.101439036467562, 0.811302368396859);
static const float3x3 AGX_OUTSET_BT2020 = float3x3(
    1.1271005818144368, -0.1413297634984383, -0.14132976349843826,
    -0.11060664309660323, 1.157823702216272, -0.11060664309660294,
    -0.016493938717834573, -0.016493938717834257, 1.2519364065950405);

float3 AgXContrastApprox(float3 x) {
  float3 x2 = x * x;
  float3 x4 = x2 * x2;
  float3 x6 = x4 * x2;
  return -17.86f * x6 * x
         + 78.01f * x6
         - 126.7f * x4 * x
         + 92.06f * x4
         - 28.72f * x2 * x
         + 4.361f * x2
         - 0.1718f * x
         + 0.002857f;
}

// variant: 0 Base, 1 Punchy, 2 Golden, 3 Blender-style. Linear BT.709 in,
// display-linear BT.709 out, about 0..1. gamma is AgX's display gamma (2.2 as
// published; the AgX Gamma setting): lower lifts midtones and shadows, black
// and white stay where they are.
float3 ApplyAgX(float3 color, float variant, float gamma = 2.2f) {
  const bool rec2020 = variant == 3.f;
  float3 val = rec2020 ? renodx::color::bt2020::from::BT709(color) : color;
  val = mul(val, rec2020 ? AGX_INSET_BT2020 : AGX_INSET_BT709);

  val = clamp(log2(max(val, 1e-10f)), AGX_MIN_EV, AGX_MAX_EV);
  val = (val - AGX_MIN_EV) / (AGX_MAX_EV - AGX_MIN_EV);
  val = AgXContrastApprox(val);

  if (variant == 1.f || variant == 2.f) {  // ASC CDL look
    const bool golden = variant == 2.f;
    const float3 slope = golden ? float3(1.f, 0.9f, 0.5f) : 1.f;
    const float3 power = golden ? 0.8f : 1.35f;
    const float sat = golden ? AGX_GOLDEN_SATURATION : 1.4f;
    val = pow(max(val * slope, 0.f), power);
    const float luma = dot(val, float3(0.2126f, 0.7152f, 0.0722f));
    val = luma + sat * (val - luma);
  }

  val = mul(val, rec2020 ? AGX_OUTSET_BT2020 : AGX_OUTSET_BT709);
  val = pow(max(val, 0.f), gamma);
  return rec2020 ? renodx::color::bt709::from::BT2020(val) : val;
}

#endif  // RENODX_ELITEDANGEROUS_TONEMAP_AGX_HLSLI_
