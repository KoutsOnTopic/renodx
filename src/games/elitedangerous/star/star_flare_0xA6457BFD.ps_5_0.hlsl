// ps_SunLensFlareBillboard_Forward0_1: the star's flare draw (starlight.hpp kStarShaders).
// Replaced only to scale its output by GLOW_BRIGHTNESS (Star/Glow Brightness):
// the drawn star alone. The light the star casts comes from GlobalViewData
// and is untouched, so this changes how bright the star looks, not how
// much it lights anything.
#include "../shared.h"

Texture2D<float4> t2 : register(t2);

Texture2D<float4> t1 : register(t1);

Texture2D<float4> t0 : register(t0);

SamplerState s1_s : register(s1);

SamplerState s0_s : register(s0);

cbuffer cb2 : register(b2)
{
  float4 cb2[2];
}

cbuffer cb1 : register(b1)
{
  float4 cb1[211];
}




// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : TEXCOORD1,
  float4 v1 : TEXCOORD2,
  float4 v2 : TEXCOORD3,
  float4 v3 : TEXCOORD4,
  float4 v4 : TEXCOORD6,
  float3 v5 : TEXCOORD7,
  out float4 o0 : SV_TARGET0)
{
  float4 r0,r1,r2,r3,r4,r5;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xyz = v5.xyz + -v0.xyz;
  r0.w = dot(r0.xyz, r0.xyz);
  r0.w = -cb1[126].x * cb1[126].x + r0.w;
  r0.w = 4 * r0.w;
  r1.x = dot(v5.xyz, v5.xyz);
  r1.x = rsqrt(r1.x);
  r1.xyz = v5.xyz * r1.xxx;
  r0.x = dot(-r1.xyz, r0.xyz);
  r0.y = r0.x + r0.x;
  r0.y = r0.y * r0.y + -r0.w;
  r0.y = sqrt(r0.y);
  r0.x = -r0.x * 2 + r0.y;
  r0.y = cmp(0 < r0.y);
  r0.x = 0.5 * r0.x;
  r0.x = r0.y ? r0.x : 0;
  r0.x = -r1.z * r0.x + v5.z;
  r0.zw = v1.xy / v1.zz;
  r0.zw = r0.zw * float2(0.5,-0.5) + float2(0.5,0.5);
  r0.z = t0.Sample(s1_s, r0.zw).x;
  r0.x = r0.z + -r0.x;
  r0.z = -v1.z + r0.z;
  r0.x = cb1[126].x * 9.99999975e-005 + r0.x;
  r0.x = cmp(r0.x < 0);
  if (r0.x != 0) discard;
  r0.x = 0.400000006 * cb1[126].x;
  r0.x = saturate(r0.z / r0.x);
  r0.x = r0.y ? 1 : r0.x;
  r0.x = cb1[126].z * r0.x;
  r0.yzw = v3.xyz + -v2.xyz;
  r0.y = dot(r0.yzw, r0.yzw);
  r0.z = cb1[126].x * cb1[126].x;
  r0.y = cmp(r0.y >= r0.z);
  r0.z = max(9.99999975e-006, cb2[1].x);
  r0.y = r0.y ? r0.z : 1;
  r0.z = 1 / cb2[0].x;
  r1.xy = float2(-0.0250000004,0.0250000004) * cb1[210].yy;
  sincos(r1.x, r1.x, r2.x);
  sincos(r1.y, r3.x, r4.x);
  r1.yz = float2(-0.5,-0.5) + v4.yx;
  r1.xw = r1.xx * r1.yz;
  r5.x = r2.x * r1.z + -r1.x;
  r5.y = r2.x * r1.y + r1.w;
  r1.xw = r5.xy * r0.zz + float2(0.5,0.5);
  r0.w = t2.Sample(s0_s, r1.xw).x;
  r0.w = log2(r0.w);
  r0.w = r0.y * r0.w;
  r0.w = exp2(r0.w);
  r1.xw = r3.xx * r1.yz;
  r2.x = r4.x * r1.z + -r1.x;
  r2.y = r4.x * r1.y + r1.w;
  r1.xy = r2.xy * r0.zz + float2(0.5,0.5);
  r0.z = t2.Sample(s0_s, r1.xy).x;
  r0.z = log2(r0.z);
  r0.z = r0.y * r0.z;
  r0.z = exp2(r0.z);
  r0.z = r0.w + r0.z;
  r0.w = t1.Sample(s0_s, v4.xy).x;
  r0.w = log2(r0.w);
  r0.y = r0.y * r0.w;
  r0.y = exp2(r0.y);
  r0.y = r0.z * 1.5 + r0.y;
  r0.x = saturate(r0.x * r0.y);
  r0.x = r0.x * r0.x;
  o0.w = cb1[125].x * r0.x;
  o0.xyz = cb2[0].yzw * cb1[90].yyy;

  o0.rgb *= GLOW_BRIGHTNESS;
  return;
}
