// ps_StarOpaqueNoise_Forward0_0_0_1: the star's surface draw (starlight.hpp kStarShaders).
// Replaced only to scale its output by STAR_BRIGHTNESS (Star/Glow Brightness):
// the drawn star alone. The light the star casts comes from GlobalViewData
// and is untouched, so this changes how bright the star looks, not how
// much it lights anything.
#include "../shared.h"

Texture2D<float4> t4 : register(t4);

Texture2D<float4> t3 : register(t3);

Texture2D<float4> t2 : register(t2);

Texture2D<float4> t1 : register(t1);

Texture2D<float4> t0 : register(t0);

SamplerState s2_s : register(s2);

SamplerState s1_s : register(s1);

SamplerState s0_s : register(s0);

cbuffer cb2 : register(b2)
{
  float4 cb2[8];
}

cbuffer cb1 : register(b1)
{
  float4 cb1[211];
}




// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : TEXCOORD0,
  float4 v1 : TEXCOORD2,
  float4 v2 : TEXCOORD3,
  float4 v3 : TEXCOORD4,
  float3 v4 : TEXCOORD5,
  out float4 o0 : SV_TARGET0)
{
  float4 r0,r1,r2,r3,r4,r5,r6;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = v0.xy / v0.zz;
  r0.xy = r0.xy * float2(0.5,0.5) + float2(0.5,0.5);
  r0.z = 1 + -r0.y;
  r0.x = t0.Sample(s2_s, r0.xz).x;
  r0.x = -v0.z + r0.x;
  r0.x = cmp(r0.x < 0);
  if (r0.x != 0) discard;
  r0.x = dot(v4.xyz, v4.xyz);
  r0.x = rsqrt(r0.x);
  r0.xyz = v4.zxy * r0.xxx;
  r1.x = dot(cb2[5].zxy, r0.xyz);
  r1.y = dot(cb2[6].zxy, r0.xyz);
  r1.z = dot(cb2[7].zxy, r0.xyz);
  r0.w = dot(r1.xyz, r1.xyz);
  r0.w = rsqrt(r0.w);
  r1.xyw = r1.xyz * r0.www;
  r2.x = 1 + cb2[4].w;
  r2.y = r2.x * r2.x + -1;
  r2.y = sqrt(r2.y);
  r2.z = -r1.z * r0.w + r2.x;
  r2.y = r2.y / r2.z;
  r2.yz = r2.yy * r1.xy;
  r2.x = 1 / r2.x;
  r2.w = dot(abs(r1.xy), abs(r1.xy));
  r2.w = rsqrt(r2.w);
  r3.xy = -abs(r1.xy) * r2.ww + float2(1,1);
  r0.w = r1.z * r0.w + 1;
  r1.z = 1 + r2.x;
  r0.w = r0.w / r1.z;
  r0.w = max(0, r0.w);
  r0.w = r0.w * r0.w;
  r1.xy = cmp(r1.xy < float2(0,0));
  r1.xy = r1.xy ? float2(-1,-1) : float2(1,1);
  r3.xy = -r0.ww * r3.xy + float2(1,1);
  r1.xy = r3.xy * r1.xy;
  r0.w = cmp(r2.x >= r1.w);
  r1.xy = r0.ww ? r1.xy : r2.yz;
  r1.xy = r1.xy * float2(0.498046875,0.498046875) + float2(0.5,0.5);
  r2.xyz = -v3.xyz + v1.xyz;
  r0.w = dot(r2.xyz, r2.xyz);
  r0.w = rsqrt(r0.w);
  r2.xyz = r2.xyz * r0.www;
  r3.xyz = cb1[123].zxy * -v2.yzx;
  r3.xyz = cb1[123].yzx * -v2.zxy + -r3.xyz;
  r3.x = dot(r3.xyz, r2.xyz);
  r3.y = dot(cb1[123].xyz, r2.xyz);
  r1.zw = r3.xy * float2(0.5,-0.5) + float2(0.5,0.5);
  r2.xyz = rsqrt(abs(r0.yzx));
  r2.xyz = float3(1,1,1) / r2.xyz;
  r0.w = 0.0199999996 * cb2[3].y;
  r3.xy = float2(1,-1) * cb1[210].yy;
  r3.xy = r3.xy * r0.ww;
  r3.zw = float2(0.899999976,0.899999976) * r3.yx;
  r4.xyz = cmp(float3(0,0,0) < abs(r0.xyz));
  if (r4.x != 0) {
    r4.xw = r0.yz * cb2[3].zz + r3.xy;
    r5.xyz = t4.Sample(s0_s, r4.xw).xyz;
    r5.xyz = r5.xyz * float3(2,2,2) + float3(-1,-1,-1);
    r5.xyz = r5.xyz * r2.zzz;
    r4.xw = r0.yz * cb2[3].zz + r3.zw;
    r6.xyz = t4.Sample(s0_s, r4.xw).xyz;
    r6.xyz = r6.xyz * float3(2,2,2) + float3(-1,-1,-1);
    r6.xyz = r6.xyz * r2.zzz;
  } else {
    r5.xyz = float3(0,0,0);
    r6.xyz = float3(0,0,0);
  }
  if (r4.y != 0) {
    r2.zw = r0.zx * cb2[3].zz + r3.xy;
    r4.xyw = t4.Sample(s0_s, r2.zw).xyz;
    r4.xyw = r4.xyw * float3(2,2,2) + float3(-1,-1,-1);
    r5.xyz = r4.xyw * r2.xxx + r5.xyz;
    r0.zw = r0.zx * cb2[3].zz + r3.zw;
    r4.xyw = t4.Sample(s0_s, r0.zw).xyz;
    r4.xyw = r4.xyw * float3(2,2,2) + float3(-1,-1,-1);
    r6.xyz = r4.xyw * r2.xxx + r6.xyz;
  }
  if (r4.z != 0) {
    r0.zw = r0.yx * cb2[3].zz + r3.xy;
    r2.xzw = t4.Sample(s0_s, r0.zw).xyz;
    r2.xzw = r2.xzw * float3(2,2,2) + float3(-1,-1,-1);
    r5.xyz = r2.xzw * r2.yyy + r5.xyz;
    r0.xy = r0.yx * cb2[3].zz + r3.zw;
    r0.xyz = t4.Sample(s0_s, r0.xy).xyz;
    r0.xyz = r0.xyz * float3(2,2,2) + float3(-1,-1,-1);
    r6.xyz = r0.xyz * r2.yyy + r6.xyz;
  }
  r0.xyz = r6.xyz * r5.xyz;
  r0.z = dot(r0.xyz, r0.xyz);
  r0.z = rsqrt(r0.z);
  r0.xy = r0.xy * r0.zz;
  r2.xyz = t1.Sample(s1_s, r1.zw).xyz;
  r0.zw = r2.yz * float2(2,2) + float2(-1,-1);
  r0.zw = r0.zw * cb2[2].ww + r1.xy;
  r0.xy = r0.xy * cb2[3].xx + r0.zw;
  r1.xyz = t3.Sample(s1_s, r0.xy).xyz;
  r0.z = cmp(cb2[7].w < 1);
  if (r0.z != 0) {
    r0.xyz = t2.Sample(s1_s, r0.xy).xyz;
    r2.yzw = r1.xyz + -r0.xyz;
    r1.xyz = cb2[7].www * r2.yzw + r0.xyz;
  }
  r0.xyz = r1.xyz * r2.xxx;
  r0.xyz = cb2[2].yyy * r0.xyz;
  r0.xyz = r1.xyz * cb2[2].xxx + r0.xyz;
  r0.xyz = cb2[2].zzz * r0.xyz;
  o0.xyz = cb1[90].yyy * r0.xyz;
  o0.w = cb1[126].z;

  o0.rgb *= STAR_BRIGHTNESS;
  return;
}
