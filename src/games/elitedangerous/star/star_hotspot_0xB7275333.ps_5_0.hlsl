// ps_SunHotspotQuad_Forward0_0_0: the star's hotspot draw (starlight.hpp kStarShaders).
// Replaced only to scale its output by STAR_BRIGHTNESS (Star/Glow Brightness):
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
  float4 cb2[3];
}

cbuffer cb1 : register(b1)
{
  float4 cb1[276];
}




// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : TEXCOORD0,
  float4 v1 : TEXCOORD1,
  float2 v2 : TEXCOORD2,
  out float4 o0 : SV_TARGET0)
{
  float4 r0,r1,r2,r3,r4,r5;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = v0.xy / v0.zz;
  r0.xy = r0.xy * float2(0.5,0.5) + float2(0.5,0.5);
  r0.z = 1 + -r0.y;
  r0.x = t0.Sample(s1_s, r0.xz).x;
  r0.x = -v0.z + r0.x;
  r0.x = cmp(r0.x < 0);
  if (r0.x != 0) discard;
  r0.xyz = -cb1[125].wyz + v1.zxy;
  r0.w = dot(r0.xyz, r0.xyz);
  r0.w = rsqrt(r0.w);
  r0.xyz = r0.xyz * r0.www;
  r1.xyz = -cb1[275].xyz + v1.xyz;
  r0.w = dot(r1.xyz, r1.xyz);
  r0.w = rsqrt(r0.w);
  r1.xyz = r1.xyz * r0.www;
  r0.w = dot(r0.yzx, r1.xyz);
  r0.w = ceil(r0.w);
  r0.w = saturate(1 + -r0.w);
  r1.xyz = rsqrt(abs(r0.yzx));
  r1.xyz = float3(1,1,1) / r1.xyz;
  r1.w = 0.0199999996 * cb2[2].y;
  r2.xy = float2(1,-1) * cb1[210].yy;
  r2.xy = r2.xy * r1.ww;
  r2.zw = float2(0.899999976,0.899999976) * r2.yx;
  r3.xyz = cmp(float3(0,0,0) < abs(r0.xyz));
  if (r3.x != 0) {
    r3.xw = r0.yz * cb2[2].zz + r2.xy;
    r4.xyz = t1.Sample(s0_s, r3.xw).xyz;
    r4.xyz = r4.xyz * float3(2,2,2) + float3(-1,-1,-1);
    r4.xyz = r4.xyz * r1.zzz;
    r3.xw = r0.yz * cb2[2].zz + r2.zw;
    r5.xyz = t1.Sample(s0_s, r3.xw).xyz;
    r5.xyz = r5.xyz * float3(2,2,2) + float3(-1,-1,-1);
    r5.xyz = r5.xyz * r1.zzz;
  } else {
    r4.xyz = float3(0,0,0);
    r5.xyz = float3(0,0,0);
  }
  if (r3.y != 0) {
    r1.zw = r0.zx * cb2[2].zz + r2.xy;
    r3.xyw = t1.Sample(s0_s, r1.zw).xyz;
    r3.xyw = r3.xyw * float3(2,2,2) + float3(-1,-1,-1);
    r4.xyz = r3.xyw * r1.xxx + r4.xyz;
    r1.zw = r0.zx * cb2[2].zz + r2.zw;
    r3.xyw = t1.Sample(s0_s, r1.zw).xyz;
    r3.xyw = r3.xyw * float3(2,2,2) + float3(-1,-1,-1);
    r5.xyz = r3.xyw * r1.xxx + r5.xyz;
  }
  if (r3.z != 0) {
    r1.xz = r0.yx * cb2[2].zz + r2.xy;
    r1.xzw = t1.Sample(s0_s, r1.xz).xyz;
    r1.xzw = r1.xzw * float3(2,2,2) + float3(-1,-1,-1);
    r4.xyz = r1.xzw * r1.yyy + r4.xyz;
    r0.xy = r0.yx * cb2[2].zz + r2.zw;
    r0.xyz = t1.Sample(s0_s, r0.xy).xyz;
    r0.xyz = r0.xyz * float3(2,2,2) + float3(-1,-1,-1);
    r5.xyz = r0.xyz * r1.yyy + r5.xyz;
  }
  r0.xyz = r5.xyz * r4.xyz;
  r0.z = dot(r0.xyz, r0.xyz);
  r0.z = rsqrt(r0.z);
  r0.xy = r0.xy * r0.zz;
  r0.xy = r0.xy * cb2[2].xx + v2.xy;
  r0.xyz = t2.Sample(s0_s, r0.xy).xyz;
  r1.xyz = cb2[1].xyz * r0.xyz;
  r0.x = cb2[1].w * cb1[126].z;
  r0.y = r1.x + r1.y;
  r0.y = r0.z * cb2[1].z + r0.y;
  r0.y = r0.y * r0.w;
  r0.y = saturate(0.333333343 * r0.y);
  o0.w = r0.x * r0.y;
  o0.xyz = cb1[90].yyy * r1.xyz;

  o0.rgb *= STAR_BRIGHTNESS;
  return;
}
