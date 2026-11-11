// ps_SunEdgeBillboards_Forward0_0_0_0: the star's edge draw (starlight.hpp kStarShaders).
// Replaced only to scale its output by STAR_BRIGHTNESS (Star/Glow Brightness):
// the drawn star alone. The light the star casts comes from GlobalViewData
// and is untouched, so this changes how bright the star looks, not how
// much it lights anything.
#include "../shared.h"

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
  float4 cb1[211];
}




// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : TEXCOORD0,
  float4 v1 : TEXCOORD1,
  float4 v2 : TEXCOORD2,
  float2 v3 : TEXCOORD3,
  out float4 o0 : SV_TARGET0)
{
  float4 r0,r1;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xyz = v2.xyz + -v0.xyz;
  r0.w = dot(r0.xyz, r0.xyz);
  r0.w = -cb1[126].x * cb1[126].x + r0.w;
  r0.w = 4 * r0.w;
  r1.x = dot(v2.xyz, v2.xyz);
  r1.x = rsqrt(r1.x);
  r1.xyz = v2.xyz * r1.xxx;
  r0.x = dot(-r1.xyz, r0.xyz);
  r0.y = r0.x + r0.x;
  r0.y = r0.y * r0.y + -r0.w;
  r0.y = sqrt(r0.y);
  r0.x = -r0.x * 2 + r0.y;
  r0.y = cmp(0 < r0.y);
  r0.x = 0.5 * r0.x;
  r0.x = r0.y ? r0.x : 0;
  r0.x = -r1.z * r0.x + v2.z;
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
  r0.y = cb2[1].z * cb1[126].z;
  r0.x = r0.y * r0.x;
  r0.yz = float2(0,1) + v3.xy;
  r1.z = cb2[1].w * cb1[210].y;
  r1.yw = float2(0,0);
  r0.yz = r0.yz * float2(1,0.5) + -r1.zw;
  r0.y = t1.Sample(s0_s, r0.yz).x;
  r1.x = cb2[2].x * cb1[210].y;
  r0.zw = v3.xy * float2(1,0.5) + r1.xy;
  r0.z = t1.Sample(s0_s, r0.zw).x;
  r0.y = r0.z + r0.y;
  o0.w = r0.y * r0.x;
  o0.xyz = cb2[0].xyz * cb1[90].yyy;

  o0.rgb *= STAR_BRIGHTNESS;
  return;
}
