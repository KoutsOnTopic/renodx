/*
 * Starlight colour remapping for Elite Dangerous (Odyssey).
 *
 * WHERE THE DATA LIVES (from Win64/EffectsBinary, all 976 effects agree):
 *
 * b1 = GlobalViewData, one fixed layout for every shader (5368 bytes):
 *   gStellarBodyPosition        float3      byte 2004  (c125.yzw)  per-draw
 *   gLightingToClipMatrixT      float4[4]   byte 4320  (c270..c273)
 *   gDirectionalLightDir        float4[2]   byte 4656  (c291, c292)
 *   gDirectionalLightColour     float4[2]   byte 4688  (c293, c294)
 *   Forward shaders light with  dot(N, -dir[i]) * colour[i]  for i = 0, 1:
 *   the engine natively lights binary systems with two stars.
 *
 *   gAtmosphereDirectionalLightColour float4 byte 416 (c26), planet sky LUT
 *   This buffer drives forward shading, atmospheres and fog only.
 *
 * g_CullingBuffer_DirectionalLight: raw (ByteAddress) buffer read by the
 * clustered/deferred lighting that lights ships, stations, terrain and
 * planet surfaces. One 48-byte record per directional light, count in
 * g_numDirectionalLights (b1 c245.y):
 *   +0  float3 colour   (shaders multiply by 1/pi)
 *   +16 float3 direction (same convention as gDirectionalLightDir)
 *   Records are recognised by matching their direction to the b1 lights.
 *
 * b0 = __shared_f3d_globals (per object). For every star effect:
 *   gObjectToClipMatrix         float4[4]   c4..c7 (rows)
 *
 * b2 = __f3dxLocalConstantBuffer (per effect), the star's own colours:
 *   StarOpaqueNoise (surface)   gStarColour c0, gStarArtistColour c1
 *   SunHotspotQuad              gStarColour c0, gStarArtistColour c1
 *   SunBillboardGlow (corona)   gCoronaArtistColour c1.xyz
 *   SunLensFlareBillboard       gStarLensFlareColour c0.yzw
 *   SunEdgeBillboards           gStarEdgeColour c0.xyz
 *
 * WHICH STAR IS THE LIGHT: the light direction and every star draw are
 * projected into the same clip space, the light via
 * gLightingToClipMatrixT * (-dir, 0) (its vanishing point), the star via
 * its own gObjectToClipMatrix * (centre, 1). Matching homogeneous (x, y, w)
 * directions identifies the star without knowing any world/lighting space.
 * The match is remembered while the light direction moves continuously, so
 * it survives the star going off-screen.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <windows.h>

#include <shlobj.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include "../../utils/settings.hpp"

namespace elitedangerous::starlight {

// ---- layout ----------------------------------------------------------------
namespace gvd {
constexpr uint64_t kSize = 5368;                // struct size; ByteWidth rounds up
constexpr uint64_t kMaxSize = kSize + 1024;     // tolerance for padding/versions
constexpr uint64_t kBodyPosition = 2004;        // float3
constexpr uint64_t kLightingToClipT = 4320;     // 4 x float4
constexpr uint64_t kLightDir = 4656;            // 2 x float4
constexpr uint64_t kLightColour = 4688;         // 2 x float4
constexpr uint64_t kAtmosphereLightColour = 416;  // float4
}  // namespace gvd
namespace light_list {
constexpr uint64_t kScanMax = 8ull << 20;  // diagnostic scan: buffers up to 8 MB
constexpr uint64_t kAutoMax = 4096;        // continuous patching: small buffers
}  // namespace light_list
constexpr uint64_t kSharedObjectToClip = 64;    // b0 c4..c7
constexpr uint64_t kSmallBufferMax = 1024;      // b0/b2 are copied whole

enum class StarFx : uint8_t { Surface,
                              Hotspot,
                              Corona,
                              Flare,
                              Edge,
                              Count };
inline const char* FxName(StarFx fx) {
  switch (fx) {
    case StarFx::Surface: return "Surface";
    case StarFx::Hotspot: return "Hotspot";
    case StarFx::Corona:  return "Corona";
    case StarFx::Flare:   return "LensFlare";
    case StarFx::Edge:    return "Edge";
    default:              return "?";
  }
}
// Preference when one light was matched by several draws in a frame.
inline int FxRank(StarFx fx) {
  switch (fx) {
    case StarFx::Surface: return 0;
    case StarFx::Hotspot: return 1;
    case StarFx::Corona:  return 2;
    case StarFx::Flare:   return 3;
    default:              return 4;
  }
}

struct StarShader {
  StarFx fx;
  uint32_t colour_offset;  // byte offset of the star colour inside b2
};
// CRC32 of the pixel shader bytecode (same hash RenoDX uses).
inline const std::unordered_map<uint32_t, StarShader> kStarShaders = {
    {0xB0ED86B7, {StarFx::Surface, 16}},  // ps_StarOpaqueNoise_Forward0_0_0_0  gStarArtistColour
    {0x7AE10FE9, {StarFx::Surface, 16}},  // ps_StarOpaqueNoise_Forward0_0_0_1
    {0xB7275333, {StarFx::Hotspot, 16}},  // ps_SunHotspotQuad_Forward0_0_0     gStarArtistColour
    {0xDC21D860, {StarFx::Corona, 16}},   // ps_SunBillboardGlow_Forward0_0     gCoronaArtistColour
    {0xF1CBC91F, {StarFx::Corona, 16}},   // ps_SunBillboardGlow_Forward0_1
    {0x509F27CB, {StarFx::Flare, 4}},     // ps_SunLensFlareBillboard_Forward0_0 gStarLensFlareColour
    {0xA6457BFD, {StarFx::Flare, 4}},     // ps_SunLensFlareBillboard_Forward0_1
    {0x82FC416E, {StarFx::Edge, 0}},      // ps_SunEdgeBillboards_Forward0_0_0_0 gStarEdgeColour
};

// ---- settings --------------------------------------------------------------
struct Config {
  float mode = 2.f;  // 0 vanilla, 1 saturation, 2 star colour, 3 class table
  float strength = 1.f;
  float saturation = 1.f;
  float match_degrees = 0.75f;
  float auto_learn = 1.f;
  float probe_log = 0.f;
};
inline Config config;

// ---- small maths -----------------------------------------------------------
struct Rgb {
  float r = 0, g = 0, b = 0;
};
inline float Luma(const Rgb& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }
inline bool Finite(const float* v, int n) {
  for (int i = 0; i < n; ++i)
    if (!std::isfinite(v[i])) return false;
  return true;
}
inline Rgb Saturate(const Rgb& c, float s) {
  const float y = Luma(c);
  return {std::max(0.f, y + (c.r - y) * s), std::max(0.f, y + (c.g - y) * s), std::max(0.f, y + (c.b - y) * s)};
}
inline float CosXYW(const float a[4], const float b[4]) {
  const float d = a[0] * b[0] + a[1] * b[1] + a[3] * b[3];
  const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[3] * a[3]);
  const float lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[3] * b[3]);
  return (la > 0 && lb > 0) ? d / (la * lb) : -2.f;
}

// Chromaticity signature, independent of brightness.
inline std::string SignatureKey(StarFx fx, const Rgb& c) {
  const float s = c.r + c.g + c.b;
  if (!(s > 0.f)) return {};
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%s:%.3f,%.3f", FxName(fx), c.r / s, c.g / s);
  return buf;
}

// ---- buffer shadows --------------------------------------------------------
struct GvdFields {
  float dir[2][4];
  float colour[2][4];
  float to_clip_t[4][4];
  float body_pos[3];
};

struct BufferShadow {
  uint64_t size = 0;
  bool is_gvd = false;
  uint8_t* mapped = nullptr;
  std::vector<uint8_t> bytes;  // whole contents for small buffers
  GvdFields gvd{};             // original (unpatched) fields for GlobalViewData
  bool gvd_valid = false;
};

struct Bindings {
  // [0] vertex, [1] pixel; slots b0..b2
  std::array<std::array<reshade::api::buffer_range, 3>, 2> cb{};
};

// ---- per-light identity ----------------------------------------------------
struct LightState {
  bool active = false;
  float dir[3] = {0, 0, 0};
  Rgb game_colour;
  // identity
  bool known = false;
  StarFx source = StarFx::Count;
  Rgb star_colour;
  std::string signature;
  std::string cls;
  float match_cos = 0.f;
  uint32_t frames_since_match = 0;
};

struct FrameMatch {
  bool any = false;
  StarFx fx = StarFx::Count;
  Rgb colour;
  float cos = -2.f;
};

inline std::mutex state_mutex;
inline std::unordered_map<uint64_t, BufferShadow> shadows;
inline std::unordered_map<uint64_t, Bindings> bindings;  // per command list
inline std::array<LightState, 2> lights;
inline std::array<FrameMatch, 2> frame_match;
inline GvdFields latest_gvd{};
inline bool latest_gvd_valid = false;

inline std::map<std::string, Rgb> colour_table;       // class -> colour
inline std::map<std::string, std::string> learned;    // signature -> class
inline std::string table_path, signatures_path;

inline uint32_t stat_gvd_uploads = 0, stat_gvd_rejected = 0, stat_star_draws = 0, stat_star_unbound = 0;
inline uint64_t stat_gvd_size_seen = 0;
inline uint32_t stat_list_patched = 0, stat_list_matched_frame = 0;

// Every buffer, for the light-record search
struct AnyBuffer {
  uint64_t size = 0;
  uint32_t usage = 0;
  uint8_t* mapped = nullptr;
};
inline std::unordered_map<uint64_t, AnyBuffer> any_buffers;
inline std::set<std::pair<uint64_t, uint32_t>> hot_kinds;  // (size, usage) known to hold light records
inline int scan_frames_left = 0;
inline uint32_t scan_hits_logged = 0, scan_buffers = 0;
inline uint64_t scan_bytes = 0;
inline std::set<std::tuple<uint64_t, uint32_t, char, int, uint64_t>> scan_seen;
inline std::set<uint64_t> scan_hit_handles;
constexpr uint32_t kScanLogCap = 150;
inline std::array<uint32_t, static_cast<size_t>(StarFx::Count)> stat_fx_draws{};
inline float stat_best_cos[2] = {-2.f, -2.f};
inline uint64_t frame_index = 0;
inline thread_local bool in_own_update = false;

// journal "teacher": labels a signature with the arrival star's class
inline std::string arrival_class;
inline uint64_t arrival_seq = 0, arrival_seq_used = 0;
inline uint64_t arrival_frame = 0;
inline std::atomic_bool journal_run = false;
inline std::thread journal_thread;

// ---- files -----------------------------------------------------------------
inline void LoadTables() {
  std::map<std::string, Rgb> table;
  std::map<std::string, std::string> sigs;
  auto parse = [](const std::string& path, auto&& fn) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
      if (auto h = line.find_first_of("#;"); h != std::string::npos) line.resize(h);
      auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string key = line.substr(0, eq), val = line.substr(eq + 1);
      auto trim = [](std::string& s) {
        s.erase(0, s.find_first_not_of(" \t\r"));
        s.erase(s.find_last_not_of(" \t\r") + 1);
      };
      trim(key);
      trim(val);
      if (!key.empty() && !val.empty()) fn(key, val);
    }
  };
  parse(table_path, [&](const std::string& k, const std::string& v) {
    std::istringstream s(v);
    Rgb c;
    if ((s >> c.r >> c.g >> c.b) && c.r >= 0 && c.g >= 0 && c.b >= 0) table[k] = c;
  });
  parse(signatures_path, [&](const std::string& k, const std::string& v) { sigs[k] = v; });
  std::scoped_lock lock(state_mutex);
  colour_table = std::move(table);
  learned = std::move(sigs);
}

inline void SaveSignatures() {  // call with state_mutex held
  std::ofstream f(signatures_path, std::ios::trunc);
  f << "# Learned by RenoDX: star draw chromaticity -> star class.\n"
       "# Edit freely; delete a line to forget it.\n";
  for (auto& [k, v] : learned) f << k << " = " << v << "\n";
}

inline const Rgb* LookupClassColour(const std::string& cls) {
  if (cls.empty()) return nullptr;
  if (auto it = colour_table.find(cls); it != colour_table.end()) return &it->second;
  for (size_t n = cls.size() - 1; n > 0; --n)  // "K_OrangeGiant" -> "K"
    if (auto it = colour_table.find(cls.substr(0, n)); it != colour_table.end()) return &it->second;
  return nullptr;
}

// ---- colour remap ----------------------------------------------------------
// Chroma comes from the chosen source; luminance stays exactly the game's.
inline Rgb RemapLight(const LightState& l, const Rgb& in) {
  const float y = Luma(in);
  if (!(y > 0.f) || config.mode == 0.f) return in;

  const Rgb* target = nullptr;
  Rgb star;
  if (l.known) {
    if (config.mode == 3.f) target = LookupClassColour(l.cls);
    if (target == nullptr && config.mode >= 2.f) {
      star = l.star_colour;
      target = &star;
    }
  }
  Rgb out = in;
  if (target != nullptr && Luma(*target) > 0.f) {
    const float k = y / Luma(*target), s = config.strength;
    out = {in.r + (target->r * k - in.r) * s, in.g + (target->g * k - in.g) * s, in.b + (target->b * k - in.b) * s};
  }
  return Saturate(out, config.saturation);
}

// ---- GlobalViewData upload -------------------------------------------------
inline bool ReadGvd(const uint8_t* base, GvdFields& g) {
  std::memcpy(g.dir, base + gvd::kLightDir, sizeof(g.dir));
  std::memcpy(g.colour, base + gvd::kLightColour, sizeof(g.colour));
  std::memcpy(g.to_clip_t, base + gvd::kLightingToClipT, sizeof(g.to_clip_t));
  std::memcpy(g.body_pos, base + gvd::kBodyPosition, sizeof(g.body_pos));
  if (!Finite(&g.dir[0][0], 8) || !Finite(&g.colour[0][0], 8) || !Finite(&g.to_clip_t[0][0], 16)) return false;
  // light 0 must be a unit vector, colours non-negative
  const float* d = g.dir[0];
  const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  if (len < 0.98f || len > 1.02f) return false;
  for (auto& c : g.colour)
    if (c[0] < 0 || c[1] < 0 || c[2] < 0) return false;
  return true;
}

// Called with state_mutex held. Patches the light colours in place.
inline void OnGvdData(BufferShadow& sh, uint8_t* base) {
  GvdFields g;
  if (!ReadGvd(base, g)) {
    ++stat_gvd_rejected;
    sh.gvd_valid = false;
    return;
  }
  ++stat_gvd_uploads;
  sh.gvd = g;
  sh.gvd_valid = true;
  latest_gvd = g;
  latest_gvd_valid = true;

  if (config.mode == 0.f) return;
  for (int i = 0; i < 2; ++i) {
    Rgb in{g.colour[i][0], g.colour[i][1], g.colour[i][2]};
    if (!(Luma(in) > 0.f)) continue;
    Rgb out = RemapLight(lights[i], in);
    float* dst = reinterpret_cast<float*>(base + gvd::kLightColour + 16 * i);
    float v[3] = {out.r, out.g, out.b};
    std::memcpy(dst, v, sizeof(v));
  }
  // planet sky colour follows light 1
  float atmo[3];
  std::memcpy(atmo, base + gvd::kAtmosphereLightColour, sizeof(atmo));
  Rgb a{atmo[0], atmo[1], atmo[2]};
  if (Finite(atmo, 3) && a.r >= 0 && a.g >= 0 && a.b >= 0 && Luma(a) > 0.f) {
    Rgb out = RemapLight(lights[0], a);
    float v[3] = {out.r, out.g, out.b};
    std::memcpy(base + gvd::kAtmosphereLightColour, v, sizeof(v));
  }
}

inline bool NearlyEqual(const Rgb& a, const Rgb& b) {
  auto eq = [](float x, float y) { return std::abs(x - y) <= 1e-5f * std::max({1.f, std::abs(x), std::abs(y)}); };
  return eq(a.r, b.r) && eq(a.g, b.g) && eq(a.b, b.b);
}

inline void LogScanHit(char type, int light, uint64_t handle, const AnyBuffer& info, const char* path,
                       uint64_t off, const uint8_t* data, uint64_t size) {
  if (scan_hits_logged >= kScanLogCap) return;
  if (!scan_seen.insert({info.size, info.usage, type, light, off}).second) return;
  ++scan_hits_logged;
  scan_hit_handles.insert(handle);
  std::stringstream s;
  s << "starlight scan: " << (type == 'D' ? "DIR" : "COLOUR") << " of light " << light + 1 << " in buffer "
    << std::hex << handle << std::dec << " size=" << info.size << " usage=0x" << std::hex << info.usage << std::dec
    << " via " << path << " at +" << off << " |";
  const uint64_t from = off >= 32 ? off - 32 : 0;
  for (uint64_t o = from; o + 4 <= size && o < off + 48; o += 4) {
    float f;
    uint32_t u;
    std::memcpy(&f, data + o, 4);
    std::memcpy(&u, data + o, 4);
    s << (o == off ? " [" : " ");
    if (std::isfinite(f) && (std::abs(f) > 1e-6f || u == 0)) s << f;
    else s << "0x" << std::hex << u << std::dec;
    if (o == off) s << "]";
  }
  reshade::log::message(reshade::log::level::info, s.str().c_str());
}

// Finds copies of this frame's light colours in CPU-written buffers and
// recolours them. The engine writes the same float values everywhere it
// stores a directional light colour (verified in-game: deferred light list
// g_CullingBuffer_DirectionalLight is a 48-byte record {colour, ?, dir(view
// space)} and a 784-byte constant buffer holds colour@640, dir@672), so a
// bit-exact match against b1's original colour is a precise signature.
// state_mutex held. Returns true when data changed.
inline bool ScanBuffer(uint64_t handle, const AnyBuffer& info, uint8_t* base, uint64_t size, const char* path) {
  if (!latest_gvd_valid || size < 12) return false;
  const bool scanning = scan_frames_left > 0;
  if (!scanning && size > light_list::kAutoMax) return false;
  if (size > light_list::kScanMax) return false;
  if (scanning) {
    ++scan_buffers;
    scan_bytes += size;
  }
  const GvdFields& g = latest_gvd;
  uint32_t key[2][3];
  bool key_ok[2];
  for (int i = 0; i < 2; ++i) {
    std::memcpy(key[i], g.colour[i], sizeof(key[i]));
    key_ok[i] = lights[i].active && Luma({g.colour[i][0], g.colour[i][1], g.colour[i][2]}) > 0.f;
  }
  if (!key_ok[0] && !key_ok[1] && !scanning) return false;

  std::vector<uint8_t> local(base, base + size);  // one sequential read of (possibly write-combined) memory
  bool changed = false;
  for (uint64_t off = 0; off + 12 <= size; off += 4) {
    uint32_t w[3];
    std::memcpy(w, local.data() + off, sizeof(w));
    for (int i = 0; i < 2; ++i) {
      if (!key_ok[i] || w[0] != key[i][0] || w[1] != key[i][1] || w[2] != key[i][2]) continue;
      if (scanning) LogScanHit('C', i, handle, info, path, off, local.data(), size);
      hot_kinds.insert({info.size, info.usage});
      ++stat_list_matched_frame;
      if (config.mode == 0.f) break;
      Rgb out = RemapLight(lights[i], {g.colour[i][0], g.colour[i][1], g.colour[i][2]});
      float v[3] = {out.r, out.g, out.b};
      std::memcpy(base + off, v, sizeof(v));
      ++stat_list_patched;
      changed = true;
      break;
    }
    if (!scanning) continue;
    // diagnostics: direction copies in the same space as b1
    float v[3];
    std::memcpy(v, local.data() + off, sizeof(v));
    if (!(std::abs(v[0]) <= 1.02f && std::abs(v[1]) <= 1.02f && std::abs(v[2]) <= 1.02f)) continue;
    const float len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (len2 < 0.96f || len2 > 1.04f) continue;
    for (int i = 0; i < 2; ++i) {
      if (!lights[i].active) continue;
      const float* d = g.dir[i];
      if ((v[0] * d[0] + v[1] * d[1] + v[2] * d[2]) / std::sqrt(len2) >= 0.9995f)
        LogScanHit('D', i, handle, info, path, off, local.data(), size);
    }
  }
  return changed;
}

// state_mutex held; returns true when the data was modified
inline bool OnBufferData(BufferShadow& sh, uint8_t* base) {
  if (sh.is_gvd) {
    OnGvdData(sh, base);
    return config.mode != 0.f;
  }
  sh.bytes.assign(base, base + sh.size);
  return false;
}

// ---- star draw -------------------------------------------------------------
inline const BufferShadow* Bound(const Bindings& b, int stage, int slot, uint64_t& offset) {
  const auto& range = b.cb[stage][slot];
  if (range.buffer.handle == 0u) return nullptr;
  auto it = shadows.find(range.buffer.handle);
  if (it == shadows.end()) return nullptr;
  offset = range.offset;
  return &it->second;
}

inline bool OnStarDraw(reshade::api::command_list* cmd_list, uint32_t crc) {
  auto si = kStarShaders.find(crc);
  if (si == kStarShaders.end()) return true;
  const StarShader& star = si->second;

  std::scoped_lock lock(state_mutex);
  ++stat_star_draws;
  ++stat_fx_draws[static_cast<size_t>(star.fx)];

  auto bit = bindings.find(reinterpret_cast<uint64_t>(cmd_list));
  if (bit == bindings.end()) {
    ++stat_star_unbound;
    return true;
  }
  uint64_t off0 = 0, off1 = 0, off2 = 0;
  const BufferShadow* b0 = Bound(bit->second, 0, 0, off0);  // VS b0: object matrices
  const BufferShadow* b1 = Bound(bit->second, 1, 1, off1);  // PS b1: GlobalViewData
  const BufferShadow* b2 = Bound(bit->second, 1, 2, off2);  // PS b2: star colours
  if (b0 == nullptr || b1 == nullptr || b2 == nullptr || !b1->gvd_valid || off0 != 0 || off2 != 0
      || b0->bytes.size() < kSharedObjectToClip + 64 || b2->bytes.size() < star.colour_offset + 12) {
    ++stat_star_unbound;
    return true;
  }

  float m[4][4];
  std::memcpy(m, b0->bytes.data() + kSharedObjectToClip, sizeof(m));
  float colour[3];
  std::memcpy(colour, b2->bytes.data() + star.colour_offset, sizeof(colour));
  if (!Finite(&m[0][0], 16) || !Finite(colour, 3)) return true;

  // star centre candidates: gStellarBodyPosition, and the object origin
  const GvdFields& g = b1->gvd;
  const float* cands[2] = {g.body_pos, nullptr};
  static const float origin[3] = {0, 0, 0};
  cands[1] = origin;

  for (int i = 0; i < 2; ++i) {
    Rgb gc{g.colour[i][0], g.colour[i][1], g.colour[i][2]};
    if (!(Luma(gc) > 0.f)) continue;
    {
      // -dir points at the light (confirmed in-game: cos = 1.0)
      const float L[3] = {-g.dir[i][0], -g.dir[i][1], -g.dir[i][2]};
      float lc[4];
      for (int k = 0; k < 4; ++k)
        lc[k] = g.to_clip_t[0][k] * L[0] + g.to_clip_t[1][k] * L[1] + g.to_clip_t[2][k] * L[2];
      for (const float* p : cands) {
        float sc[4];
        for (int k = 0; k < 4; ++k) sc[k] = m[k][0] * p[0] + m[k][1] * p[1] + m[k][2] * p[2] + m[k][3];
        const float c = CosXYW(sc, lc);
        stat_best_cos[i] = std::max(stat_best_cos[i], c);
        const float threshold = std::cos(config.match_degrees * 3.14159265f / 180.f);
        if (c < threshold) continue;
        FrameMatch& fm = frame_match[i];
        const bool better = !fm.any || FxRank(star.fx) < FxRank(fm.fx) || (FxRank(star.fx) == FxRank(fm.fx) && c > fm.cos);
        if (better) fm = {true, star.fx, {colour[0], colour[1], colour[2]}, c};
      }
    }
  }
  return true;
}

// ---- per frame -------------------------------------------------------------
inline void EndFrame() {
  std::scoped_lock lock(state_mutex);
  ++frame_index;
  bool learned_changed = false;

  for (int i = 0; i < 2; ++i) {
    LightState& l = lights[i];
    const bool active = latest_gvd_valid
                        && Luma({latest_gvd.colour[i][0], latest_gvd.colour[i][1], latest_gvd.colour[i][2]}) > 0.f;
    if (active) {
      const float* d = latest_gvd.dir[i];
      const float dot = l.dir[0] * d[0] + l.dir[1] * d[1] + l.dir[2] * d[2];
      // a sudden jump in direction = a different star (jump, binary swap)
      if (l.active && dot < std::cos(3.f * 3.14159265f / 180.f) && !frame_match[i].any) l.known = false;
      std::memcpy(l.dir, d, sizeof(l.dir));
      l.game_colour = {latest_gvd.colour[i][0], latest_gvd.colour[i][1], latest_gvd.colour[i][2]};
    } else {
      l.known = false;
    }
    l.active = active;

    const FrameMatch& fm = frame_match[i];
    if (fm.any) {
      l.known = true;
      l.source = fm.fx;
      l.star_colour = fm.colour;
      l.match_cos = fm.cos;
      l.frames_since_match = 0;
      l.signature = SignatureKey(fm.fx, fm.colour);
      auto it = learned.find(l.signature);
      l.cls = it != learned.end() ? it->second : std::string{};
    } else {
      ++l.frames_since_match;
    }
  }

  // Teacher: just after arriving, light 0 is the arrival star.
  if (config.auto_learn != 0.f && arrival_seq != arrival_seq_used && !arrival_class.empty()) {
    if (arrival_frame == 0) arrival_frame = frame_index;
    LightState& l = lights[0];
    if (l.known && l.frames_since_match == 0 && !l.signature.empty()) {
      if (!learned.contains(l.signature)) {
        learned[l.signature] = arrival_class;
        l.cls = arrival_class;
        learned_changed = true;
        std::string msg = "starlight: learned " + l.signature + " = " + arrival_class;
        reshade::log::message(reshade::log::level::info, msg.c_str());
      }
      arrival_seq_used = arrival_seq;
      arrival_frame = 0;
    } else if (frame_index - arrival_frame > 600) {  // gave up (~10 s)
      arrival_seq_used = arrival_seq;
      arrival_frame = 0;
    }
  }
  if (learned_changed) SaveSignatures();

  if (config.probe_log != 0.f && frame_index % 120 == 0) {
    std::stringstream s;
    s << "starlight probe: gvd=" << stat_gvd_uploads << "/" << stat_gvd_rejected << " size=" << stat_gvd_size_seen
      << " stars=" << stat_star_draws << " unbound=" << stat_star_unbound << " fx[";
    for (size_t k = 0; k < stat_fx_draws.size(); ++k) s << stat_fx_draws[k] << (k + 1 < stat_fx_draws.size() ? "," : "]");
    for (int i = 0; i < 2; ++i) {
      const auto& l = lights[i];
      s << " | L" << i << (l.active ? "" : " off") << " dir=(" << l.dir[0] << "," << l.dir[1] << "," << l.dir[2] << ")"
        << " col=(" << l.game_colour.r << "," << l.game_colour.g << "," << l.game_colour.b << ") best_cos=" << stat_best_cos[i];
      if (l.known) s << " star=" << FxName(l.source) << " " << l.signature << " class=" << l.cls;
    }
    s << " | colour copies: kinds=" << hot_kinds.size() << " patched=" << stat_list_patched
      << " matched/frame=" << stat_list_matched_frame;
    reshade::log::message(reshade::log::level::info, s.str().c_str());
  }

  frame_match = {};
  stat_list_matched_frame = 0;

  if (scan_frames_left > 0 && --scan_frames_left == 0) {
    std::stringstream s;
    s << "starlight scan: finished, " << scan_buffers << " buffer writes / " << (scan_bytes >> 10)
      << " KB searched, " << scan_hits_logged << " hits logged, buffers with hits: " << scan_hit_handles.size()
      << ", hot kinds: " << hot_kinds.size();
    reshade::log::message(reshade::log::level::info, s.str().c_str());
  }
  stat_best_cos[0] = stat_best_cos[1] = -2.f;
}

// ---- journal teacher -------------------------------------------------------
inline std::filesystem::path JournalDir() {
  PWSTR p = nullptr;
  std::filesystem::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &p)))
    out = std::filesystem::path(p) / "Frontier Developments" / "Elite Dangerous";
  CoTaskMemFree(p);
  return out;
}

inline std::string JsonField(const std::string& line, const char* key) {
  const std::string k = std::string("\"") + key + "\":\"";
  auto a = line.find(k);
  if (a == std::string::npos) return {};
  a += k.size();
  auto b = line.find('"', a);
  return b == std::string::npos ? std::string{} : line.substr(a, b - a);
}

inline void JournalLoop() {
  const auto dir = JournalDir();
  std::filesystem::path file;
  std::streamoff pos = 0;
  std::string pending;
  while (journal_run) {
    std::error_code ec;
    std::filesystem::path newest;
    std::filesystem::file_time_type t{};
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
      auto n = e.path().filename().string();
      if (n.rfind("Journal.", 0) != 0 || e.path().extension() != ".log") continue;
      auto wt = e.last_write_time(ec);
      if (newest.empty() || wt > t) newest = e.path(), t = wt;
    }
    if (!newest.empty()) {
      if (newest != file) {  // start at the end of a new file: only live events teach
        file = newest;
        pos = static_cast<std::streamoff>(std::filesystem::file_size(file, ec));
      }
      std::ifstream f(file, std::ios::binary);
      f.seekg(pos);
      std::string line;
      while (std::getline(f, line)) {
        if (line.find("\"event\":\"StartJump\"") != std::string::npos
            && line.find("\"JumpType\":\"Hyperspace\"") != std::string::npos) {
          pending = JsonField(line, "StarClass");
        } else if (line.find("\"event\":\"FSDJump\"") != std::string::npos && !pending.empty()) {
          std::scoped_lock lock(state_mutex);
          arrival_class = pending;
          ++arrival_seq;
          arrival_frame = 0;
          pending.clear();
        }
      }
      if (f.eof()) {
        f.clear();
        f.seekg(0, std::ios::end);
      }
      if (auto p = f.tellg(); p >= 0) pos = p;
    }
    for (int i = 0; i < 10 && journal_run; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

// ---- ReShade events --------------------------------------------------------
inline void OnInitResource(reshade::api::device*, const reshade::api::resource_desc& desc,
                           const reshade::api::subresource_data* initial, reshade::api::resource_usage,
                           reshade::api::resource resource) {
  if (desc.type != reshade::api::resource_type::buffer) return;
  const uint64_t size = desc.buffer.size;
  const auto usage = static_cast<uint32_t>(desc.usage);
  const bool is_cb = (usage & static_cast<uint32_t>(reshade::api::resource_usage::constant_buffer)) != 0u;
  const bool is_gvd = is_cb && size >= gvd::kSize && size <= gvd::kMaxSize;
  if (!is_gvd) {
    std::scoped_lock lock(state_mutex);
    any_buffers[resource.handle] = {size, usage, nullptr};
  }
  if (!is_cb) return;
  if (!is_gvd && size > kSmallBufferMax) return;
  std::scoped_lock lock(state_mutex);
  auto& sh = shadows[resource.handle];
  sh = {};
  sh.size = size;
  sh.is_gvd = is_gvd;
  if (is_gvd) stat_gvd_size_seen = size;
  if (initial != nullptr && initial->data != nullptr && !is_gvd) {
    auto* p = static_cast<const uint8_t*>(initial->data);
    sh.bytes.assign(p, p + size);
  }
}

inline void OnDestroyResource(reshade::api::device*, reshade::api::resource resource) {
  std::scoped_lock lock(state_mutex);
  shadows.erase(resource.handle);
  any_buffers.erase(resource.handle);
}

inline void OnMapBufferRegion(reshade::api::device*, reshade::api::resource resource, uint64_t offset, uint64_t,
                              reshade::api::map_access access, void** data) {
  if (data == nullptr || *data == nullptr || offset != 0 || access == reshade::api::map_access::read_only) return;
  std::scoped_lock lock(state_mutex);
  auto it = shadows.find(resource.handle);
  if (it != shadows.end()) it->second.mapped = static_cast<uint8_t*>(*data);
  auto ab = any_buffers.find(resource.handle);
  if (ab != any_buffers.end() && latest_gvd_valid
      && (scan_frames_left > 0 || ab->second.size <= light_list::kAutoMax)) {
    ab->second.mapped = static_cast<uint8_t*>(*data);
  }
}

inline void OnUnmapBufferRegion(reshade::api::device*, reshade::api::resource resource) {
  std::scoped_lock lock(state_mutex);
  auto it = shadows.find(resource.handle);
  if (it != shadows.end() && it->second.mapped != nullptr) {
    OnBufferData(it->second, it->second.mapped);
    it->second.mapped = nullptr;
  }
  auto ab = any_buffers.find(resource.handle);
  if (ab != any_buffers.end() && ab->second.mapped != nullptr) {
    ScanBuffer(resource.handle, ab->second, ab->second.mapped, ab->second.size, "map");
    ab->second.mapped = nullptr;
  }
}

inline bool OnUpdateBufferRegion(reshade::api::device* device, const void* data, reshade::api::resource dest,
                                 uint64_t dest_offset, uint64_t size) {
  if (in_own_update || data == nullptr) return false;
  std::vector<uint8_t> copy;
  bool changed = false;
  {
    std::scoped_lock lock(state_mutex);
    auto it = shadows.find(dest.handle);
    if (it != shadows.end() && dest_offset == 0) {
      const uint64_t n = size == UINT64_MAX ? it->second.size : size;
      if (n >= it->second.size) {
        copy.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + it->second.size);
        changed = OnBufferData(it->second, copy.data());
        size = copy.size();
      }
    }
    auto ab = any_buffers.find(dest.handle);
    if (ab != any_buffers.end()) {
      const uint64_t n = size == UINT64_MAX ? ab->second.size - dest_offset : size;
      if (n > 0 && n <= light_list::kScanMax) {
        if (copy.empty()) copy.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + n);
        changed = ScanBuffer(dest.handle, ab->second, copy.data(), copy.size(), "update") || changed;
      }
    }
    if (!changed) return false;
  }
  in_own_update = true;
  device->update_buffer_region(copy.data(), dest, dest_offset, copy.size());
  in_own_update = false;
  return true;
}

// Diagnostic: show where buffers holding light records are copied to.
inline bool OnCopyBufferRegion(reshade::api::command_list*, reshade::api::resource src, uint64_t src_offset,
                               reshade::api::resource dst, uint64_t dst_offset, uint64_t size) {
  std::scoped_lock lock(state_mutex);
  if (scan_frames_left <= 0 || !scan_hit_handles.contains(src.handle) || scan_hits_logged >= kScanLogCap) return false;
  ++scan_hits_logged;
  auto d = any_buffers.find(dst.handle);
  std::stringstream s;
  s << "starlight scan: copy " << std::hex << src.handle << std::dec << "+" << src_offset << " -> " << std::hex
    << dst.handle << std::dec << "+" << dst_offset << " (" << size << " bytes, dst size "
    << (d != any_buffers.end() ? d->second.size : 0) << " usage=0x" << std::hex
    << (d != any_buffers.end() ? d->second.usage : 0) << ")";
  reshade::log::message(reshade::log::level::info, s.str().c_str());
  return false;
}

inline bool OnCopyResource(reshade::api::command_list* cmd_list, reshade::api::resource src, reshade::api::resource dst) {
  return OnCopyBufferRegion(cmd_list, src, 0, dst, 0, UINT64_MAX);
}

inline void StartScan() {
  std::scoped_lock lock(state_mutex);
  scan_frames_left = 180;
  scan_hits_logged = 0;
  scan_buffers = 0;
  scan_bytes = 0;
  scan_seen.clear();
  scan_hit_handles.clear();
  std::stringstream s;
  s << "starlight scan: start. light 1 dir=(" << latest_gvd.dir[0][0] << "," << latest_gvd.dir[0][1] << ","
    << latest_gvd.dir[0][2] << ") col=(" << latest_gvd.colour[0][0] << "," << latest_gvd.colour[0][1] << ","
    << latest_gvd.colour[0][2] << ")  light 2 dir=(" << latest_gvd.dir[1][0] << "," << latest_gvd.dir[1][1] << ","
    << latest_gvd.dir[1][2] << ") col=(" << latest_gvd.colour[1][0] << "," << latest_gvd.colour[1][1] << ","
    << latest_gvd.colour[1][2] << ")";
  reshade::log::message(reshade::log::level::info, s.str().c_str());
}

inline void OnPushDescriptors(reshade::api::command_list* cmd_list, reshade::api::shader_stage stages,
                              reshade::api::pipeline_layout, uint32_t,
                              const reshade::api::descriptor_table_update& update) {
  if (update.type != reshade::api::descriptor_type::constant_buffer || update.descriptors == nullptr) return;
  int stage = -1;
  if (stages == reshade::api::shader_stage::vertex) stage = 0;
  if (stages == reshade::api::shader_stage::pixel) stage = 1;
  if (stage < 0) return;
  const auto* ranges = static_cast<const reshade::api::buffer_range*>(update.descriptors);
  std::scoped_lock lock(state_mutex);
  auto& b = bindings[reinterpret_cast<uint64_t>(cmd_list)];
  for (uint32_t i = 0; i < update.count; ++i) {
    const uint32_t slot = update.binding + i;
    if (slot < 3) b.cb[stage][slot] = ranges[i];
  }
}

inline void OnPresent(reshade::api::command_queue*, reshade::api::swapchain*, const reshade::api::rect*,
                      const reshade::api::rect*, uint32_t, const reshade::api::rect*) {
  EndFrame();
}

// ---- settings UI -----------------------------------------------------------
inline int label_choice = 0;

inline std::vector<renodx::utils::settings::Setting*> Settings() {
  using renodx::utils::settings::Setting;
  using renodx::utils::settings::SettingValueType;
  return {
      new Setting{
          .key = "StarlightMode",
          .binding = &config.mode,
          .value_type = SettingValueType::INTEGER,
          .default_value = 2.f,
          .label = "Starlight Colour",
          .section = "Starlight",
          .tooltip = "Saturation: boost the game's own tint.\n"
                     "Star Colour: tint each light with the colour of the star actually casting it.\n"
                     "Class Table: use renodx-starlight.ini for stars whose class has been learned.",
          .labels = {"Vanilla", "Saturation", "Star Colour", "Class Table"},
      },
      new Setting{
          .key = "StarlightStrength",
          .binding = &config.strength,
          .default_value = 100.f,
          .label = "Strength",
          .section = "Starlight",
          .max = 100.f,
          .is_enabled = [] { return config.mode >= 2.f; },
          .parse = [](float v) { return v * 0.01f; },
      },
      new Setting{
          .key = "StarlightSaturation",
          .binding = &config.saturation,
          .default_value = 100.f,
          .label = "Saturation",
          .section = "Starlight",
          .tooltip = "Chroma multiplier applied last (100 = unchanged)",
          .max = 400.f,
          .is_enabled = [] { return config.mode != 0.f; },
          .parse = [](float v) { return v * 0.01f; },
      },
      new Setting{
          .key = "StarlightAutoLearn",
          .binding = &config.auto_learn,
          .value_type = SettingValueType::BOOLEAN,
          .default_value = 1.f,
          .label = "Learn classes on arrival",
          .section = "Starlight",
          .tooltip = "After a hyperspace jump the arrival star is lighting you; its class from the\n"
                     "journal is attached to that star's colour signature. Identification itself\n"
                     "never uses the journal.",
      },
      new Setting{
          .key = "StarlightProbeLog",
          .binding = &config.probe_log,
          .value_type = SettingValueType::BOOLEAN,
          .default_value = 0.f,
          .label = "Probe log",
          .section = "Starlight",
          .tooltip = "Every 120 frames, write lights, matches and buffer stats to ReShade.log",
      },
      new Setting{
          .value_type = SettingValueType::BUTTON,
          .label = "Scan buffers for light data (3 s)",
          .section = "Starlight",
          .tooltip = "Searches everything the game uploads for this frame's light direction and colour\n"
                     "and writes the locations to ReShade.log. Expect a short stutter.",
          .on_change = [] { StartScan(); },
      },
      new Setting{
          .value_type = SettingValueType::BUTTON,
          .label = "Reload tables",
          .section = "Starlight",
          .on_change = [] { LoadTables(); },
      },
      new Setting{
          .value_type = SettingValueType::CUSTOM,
          .section = "Starlight",
          .on_draw = [] {
            std::scoped_lock lock(state_mutex);
            ImGui::Text("GlobalViewData uploads %u (rejected %u, size %llu)   star draws %u (unbound %u)",
                        stat_gvd_uploads, stat_gvd_rejected, static_cast<unsigned long long>(stat_gvd_size_seen),
                        stat_star_draws, stat_star_unbound);
            for (int i = 0; i < 2; ++i) {
              const auto& l = lights[i];
              if (!l.active) {
                ImGui::Text("Light %d: off", i + 1);
                continue;
              }
              ImGui::Text("Light %d: game %.3f %.3f %.3f", i + 1, l.game_colour.r, l.game_colour.g, l.game_colour.b);
              if (l.known) {
                ImGui::Text("   star via %s  %.3f %.3f %.3f  cos %.5f%s  %u frames ago",
                            FxName(l.source), l.star_colour.r, l.star_colour.g, l.star_colour.b, l.match_cos,
                            "", l.frames_since_match);
                ImGui::Text("   %s  class: %s", l.signature.c_str(), l.cls.empty() ? "unlearned" : l.cls.c_str());
              } else {
                ImGui::TextUnformatted("   star not identified yet (look towards it)");
              }
            }
            // manual labelling of light 1's star
            std::vector<const char*> names;
            for (auto& [k, v] : colour_table) names.push_back(k.c_str());
            if (!names.empty() && lights[0].known && !lights[0].signature.empty()) {
              label_choice = std::clamp(label_choice, 0, static_cast<int>(names.size()) - 1);
              ImGui::SetNextItemWidth(120);
              ImGui::Combo("##starlight_class", &label_choice, names.data(), static_cast<int>(names.size()));
              ImGui::SameLine();
              if (ImGui::Button("Label light 1's star")) {
                learned[lights[0].signature] = names[label_choice];
                lights[0].cls = names[label_choice];
                SaveSignatures();
              }
            }
            ImGui::Text("Light colour copies: %zu buffer kinds, %u patched%s",
                        hot_kinds.size(), stat_list_patched, scan_frames_left > 0 ? "  [scanning]" : "");
            ImGui::Text("Learned signatures: %zu", learned.size());
            return false;
          },
      },
  };
}

inline void Use(DWORD fdw_reason) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH: {
      wchar_t exe[MAX_PATH];
      GetModuleFileNameW(nullptr, exe, MAX_PATH);
      const auto dir = std::filesystem::path(exe).parent_path();
      table_path = (dir / "renodx-starlight.ini").string();
      signatures_path = (dir / "renodx-starlight-signatures.ini").string();
      LoadTables();
      reshade::register_event<reshade::addon_event::init_resource>(OnInitResource);
      reshade::register_event<reshade::addon_event::destroy_resource>(OnDestroyResource);
      reshade::register_event<reshade::addon_event::map_buffer_region>(OnMapBufferRegion);
      reshade::register_event<reshade::addon_event::unmap_buffer_region>(OnUnmapBufferRegion);
      reshade::register_event<reshade::addon_event::update_buffer_region>(OnUpdateBufferRegion);
      reshade::register_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
      reshade::register_event<reshade::addon_event::present>(OnPresent);
      reshade::register_event<reshade::addon_event::copy_buffer_region>(OnCopyBufferRegion);
      reshade::register_event<reshade::addon_event::copy_resource>(OnCopyResource);
      journal_run = true;
      journal_thread = std::thread(JournalLoop);
      break;
    }
    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::init_resource>(OnInitResource);
      reshade::unregister_event<reshade::addon_event::destroy_resource>(OnDestroyResource);
      reshade::unregister_event<reshade::addon_event::map_buffer_region>(OnMapBufferRegion);
      reshade::unregister_event<reshade::addon_event::unmap_buffer_region>(OnUnmapBufferRegion);
      reshade::unregister_event<reshade::addon_event::update_buffer_region>(OnUpdateBufferRegion);
      reshade::unregister_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);
      reshade::unregister_event<reshade::addon_event::copy_buffer_region>(OnCopyBufferRegion);
      reshade::unregister_event<reshade::addon_event::copy_resource>(OnCopyResource);
      journal_run = false;
      if (journal_thread.joinable()) journal_thread.detach();  // never block in DllMain
      break;
  }
}

}  // namespace elitedangerous::starlight
