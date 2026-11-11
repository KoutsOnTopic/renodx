// ps_SunBillboardGlow_Forward0_0: the star's corona draw (starlight.hpp kStarShaders).
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
  float4 cb2[5];
}

cbuffer cb1 : register(b1)
{
  float4 cb1[211];
}




// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : TEXCOORD1,
  float4 v1 : TEXCOORD3,
  float4 v2 : TEXCOORD4,
  float4 v3 : TEXCOORD5,
  float4 v4 : TEXCOORD7,
  float3 v5 : TEXCOORD8,
  out float4 o0 : SV_TARGET0)
{
  float4 r0,r1,r2;
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
  r0.xw = cb2[3].zw * cb1[210].yy;
  r1.xyzw = cb2[2].zzww * v4.xyxy;
  r2.xyzw = r0.wwww * float4(-0.00600000005,0.0149999997,0.0120000001,-0.00999999978) + r1.zwzw;
  r1.xyzw = r0.xxxx * float4(-0.00600000005,0.0149999997,0.0120000001,-0.00999999978) + r1.xyxy;
  r0.xw = t2.Sample(s0_s, r2.xy).xy;
  r2.xy = t2.Sample(s0_s, r2.zw).xy;
  r2.xy = r2.xy * float2(2,2) + float2(-1,-1);
  r0.xw = r0.xw * float2(2,2) + float2(-1,-1);
  r0.xw = r0.xw * r2.xy;
  r1.xy = t2.Sample(s0_s, r1.xy).xy;
  r1.zw = t2.Sample(s0_s, r1.zw).xy;
  r1.zw = r1.zw * float2(2,2) + float2(-1,-1);
  r1.xy = r1.xy * float2(2,2) + float2(-1,-1);
  r1.xy = r1.xy * r1.zw;
  r1.xy = cb2[2].xx * r1.xy;
  r0.xw = r0.xw * cb2[2].yy + -r1.xy;
  r2.xyz = -v3.xyz + v2.xyz;
  r1.z = dot(r2.xyz, r2.xyz);
  r1.z = sqrt(r1.z);
  r1.w = -v1.w + r1.z;
  r1.z = r1.z / v0.w;
  r1.z = saturate(-r1.z * 2 + 2);
  r1.z = log2(r1.z);
  r2.x = -v1.w + v0.w;
  r2.y = cb2[3].y * r2.x;
  r2.x = r1.w / r2.x;
  r1.w = r1.w / r2.y;
  r1.w = max(9.99999997e-007, r1.w);
  r1.w = log2(r1.w);
  r1.w = cb2[3].x * r1.w;
  r1.w = exp2(r1.w);
  r1.w = min(1, r1.w);
  r0.xw = r1.ww * r0.xw + r1.xy;
  r0.xw = v4.xy + r0.xw;
  r1.x = max(9.99999997e-007, r2.x);
  r1.x = log2(r1.x);
  r1.x = cb2[0].y * r1.x;
  r1.x = exp2(r1.x);
  r1.x = min(1, r1.x);
  r1.yw = v4.xy + -r0.xw;
  r0.xw = r1.xx * r1.yw + r0.xw;
  r1.x = 1 + -r1.x;
  r2.xyz = t1.Sample(s0_s, r0.xw).xyz;
  r1.xyw = r2.xyz * r1.xxx;
  r1.xyw = cb2[1].xyz * r1.xyw;
  r1.xyw = cb2[1].www * r1.xyw;
  r0.xw = float2(-0.5,-0.5) + v4.xy;
  r2.x = dot(r0.xw, r0.xw);
  r2.x = rsqrt(r2.x);
  r0.xw = r2.xx * r0.xw;
  r0.xw = r0.xw * float2(0.5,0.5) + float2(0.5,0.5);
  r0.xw = t2.Sample(s0_s, r0.xw).xy;
  r0.x = 1 + r0.x;
  r0.w = r0.w * 0.200000003 + 0.800000012;
  r0.w = cb2[1].w * r0.w;
  r2.xyz = cb2[1].xyz * r0.www;
  r0.x = r0.x * r1.z;
  r0.x = exp2(r0.x);
  r2.xyz = r2.xyz * r0.xxx;
  r2.xyz = float3(0.0500000007,0.0500000007,0.0500000007) * r2.xyz;
  r2.xyz = max(float3(0,0,0), r2.xyz);
  r0.w = dot(v2.xyz, v2.xyz);
  r0.w = sqrt(r0.w);
  r0.w = r0.w / v0.w;
  r0.w = -cb2[4].y + r0.w;
  r1.z = cb2[4].x + -cb2[4].y;
  r1.z = 1 / r1.z;
  r0.w = saturate(r1.z * r0.w);
  r1.z = r0.w * -2 + 3;
  r0.w = r0.w * r0.w;
  r0.w = r1.z * r0.w;
  r2.xyz = r0.www * r2.xyz;
  r0.w = cmp(r0.x < 0.99000001);
  r0.x = min(1, r0.x);
  r0.x = cb1[126].z * r0.x;
  r1.xyz = r0.www ? r2.xyz : r1.xyw;
  o0.xyz = cb1[90].yyy * r1.xyz;
  r0.w = 0.400000006 * cb1[126].x;
  r0.z = saturate(r0.z / r0.w);
  r0.y = r0.y ? 1 : r0.z;
  o0.w = r0.x * r0.y;

  o0.rgb *= GLOW_BRIGHTNESS;
  return;
}
