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
 *   +16 float3 direction (camera space)
 *   Records are recognised by their colour: a bit-exact copy of a b1 light
 *   colour. The same goes for every other buffer the engine copies the
 *   light colour into (a 784-byte cbuffer, per-terrain-patch sun records).
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
constexpr uint64_t kAutoMax = 4096;        // continuous patching: small buffers, every write
// Larger buffers are patched on every write once their kind (size, usage) is
// known to hold light colours. Atmospheric planets keep per-terrain-patch sun
// records (direction, colour, ...) in ~320 KB structured buffers, found by a
// scan. To find them without the manual scan, every kDiscoveryPeriod frames
// up to kDiscoveryBytes of unknown mid-size buffer writes are searched too.
constexpr uint64_t kHotMax = 1ull << 20;
constexpr uint64_t kDiscoveryBytes = 1ull << 20;
constexpr uint64_t kDiscoveryPeriod = 30;
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
// The surface shader (StarOpaqueNoise) never reads gStarColour/gStarArtistColour:
// its output is pDiffuseTexture * gStarBaseColourWeight * gStarOverallBrightness
// (+ gStarBurnTexture in variant _1); hotspot = texture * gStarArtistColour,
// corona = gCoronaArtistColour. The constant below is therefore only the
// class signature and the fallback colour; the seen colour is measured from
// the rendered frame (StarProbe).
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
  float mode = 1.f;  // 0 vanilla, 1 star colour, 2 class table
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
  std::array<reshade::api::resource_view, 8> ps_srv{};  // pixel-stage t0..t7 (diagnostics)
  reshade::api::resource rt0{};                         // resource behind render target 0
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
  uint32_t frames_inactive = 0;
  // measured from the rendered star (see StarProbe); reset with the identity
  bool measured_valid = false;
  Rgb measured_acc;           // robust estimate over the ring (linear light)
  uint32_t measured_samples = 0, measured_used = 0;
  float measured_peak = 0.f;  // brightest sample recently; much dimmer ones are not the star
  std::array<Rgb, 32> ring{}; // the last accepted samples
  uint32_t ring_count = 0, ring_pos = 0;
  // The light's unattenuated colour: component-wise maximum of the colour
  // variants the engine uploaded for it this frame and last. Passes inside an
  // atmosphere carry the same light filtered by transmittance (each channel
  // scaled down); recolouring relative to this base keeps that filtering.
  bool base_valid = false;
  Rgb base_colour;
  // The tint actually applied (chromaticity, sum 1) and its class saturation.
  // On a hand-over inside a system it rolls over from the previous star's
  // values over kRolloverSeconds; on arrival it switches at once.
  bool applied_valid = false;
  Rgb applied;                 // current
  float applied_sat = 1.f;
  Rgb roll_from;               // start of the roll-over
  float roll_from_sat = 1.f;
  bool rolling = false;
  std::chrono::steady_clock::time_point roll_start{};
  std::string applied_identity;  // signature the goal belongs to
  uint64_t applied_frame = 0;    // last frame the goal was computed
  uint64_t applied_arrival = 0;  // arrival_seq at that time
};

struct FrameMatch {
  bool any = false;
  StarFx fx = StarFx::Count;
  Rgb colour;      // what the star looks like (texture average when available)
  Rgb sig_colour;  // the b2 constant, stable per class: used for the signature
  float cos = -2.f;
  // every draw kind that matched this light this frame, so a class learned
  // from the surface can be attached to the corona/hotspot/flare signatures
  std::array<bool, static_cast<size_t>(StarFx::Count)> fx_any{};
  std::array<Rgb, static_cast<size_t>(StarFx::Count)> fx_sig{};
  std::array<float, static_cast<size_t>(StarFx::Count)> fx_cos{};
};

// ---- rendered star colour ---------------------------------------------------
// What the star looks like cannot be read from its constants: the surface
// shader colours by texture, and hotspot/corona/edge layers (own constants)
// are drawn on top (a Y dwarf: orange texture + blue/purple layers = magenta).
// So the colour is measured: a small window of the scene render target around
// the star's projected centre is copied out and averaged (see
// RunStarReadback). The measurement is kept per light while its identity
// holds.
struct StarProbe {
  bool want = false;
  reshade::api::resource target{};  // render target the star surface was drawn into
  float ndc[2] = {0, 0};            // star centre in normalised device coordinates
  uint64_t frame = 0;
  uint64_t cmd_list = 0;            // command list of that draw
  uint64_t last_sampled = 0;
};
inline std::array<StarProbe, 2> probe;
struct Readback {
  reshade::api::resource staging{};  // persistent kProbeBox^2 staging texture
  reshade::api::format format{};
  bool pending = false;              // a copy has been recorded and not read yet
  uint64_t frame = 0;
  int light = 0;
  // diagnostics of the pending copy
  uint64_t target = 0;
  uint32_t width = 0, height = 0;
  int px = 0, py = 0;
  bool warned_msaa = false, warned_format = false;
};
inline Readback readback;
inline uint32_t stat_readbacks = 0, stat_probes_skipped = 0;
// The star surface is also drawn in side passes (a 256x256 reflection probe
// face per frame), which keep matching while the main view looks away. Only
// the main view is sampled: a target at least half the back buffer's width
// (the scene may render below output resolution with upscaling).
inline uint32_t backbuffer_width = 0, backbuffer_height = 0;
constexpr int kProbeBox = 48;            // pixels, square window at the star centre (averages disc patches)
constexpr uint64_t kProbePeriod = 10;    // frames between samples of the same light
constexpr uint64_t kReadbackDelay = 3;   // frames before mapping the copy
constexpr float kPeakDecay = 0.95f;      // per accepted sample
constexpr float kChromaInlier = 0.60f;   // L1 chromaticity distance from the median to count (a disc with two patch colours ~0.6 apart must average, not flip)
constexpr float kMinSampleFraction = 0.02f;  // of the brightest sample; dimmer ones are ignored
constexpr uint32_t kReadbackLogPerStar = 30;  // probe log: samples logged per identified star, then 1 in 50
constexpr float kMinPeakPixelLuma = 0.015f;   // HDR scene units: sky is ~0.004, a brown dwarf's disc ~0.04, hot stars >> 1

inline std::mutex state_mutex;
// ReShade raises destroy_resource from inside D3D calls (the runtime fires
// its destruction notifier when it finally frees a game resource, e.g. during
// a copy or a map). If that happens on a thread that already holds
// state_mutex, locking again would abort the process, so (a) nothing calls
// into D3D while holding the lock, and (b) every event callback bails out
// when this thread already holds it.
inline thread_local bool in_state_lock = false;
struct StateLock {
  StateLock() {
    state_mutex.lock();
    in_state_lock = true;
  }
  ~StateLock() {
    in_state_lock = false;
    state_mutex.unlock();
  }
  StateLock(const StateLock&) = delete;
  StateLock& operator=(const StateLock&) = delete;
};
inline std::unordered_map<uint64_t, BufferShadow> shadows;
inline std::unordered_map<uint64_t, Bindings> bindings;  // per command list
inline std::array<LightState, 2> lights;
inline std::array<FrameMatch, 2> frame_match;

// Many render passes upload their own GlobalViewData every frame (about 200
// uploads per frame in-game), and on atmospheric worlds some of them (sky and
// scattering passes) carry a dimmed or zeroed copy of the star light. "Last
// upload wins" therefore flickered: whichever pass happened to upload last
// decided whether the light looked on or off and which colour the deferred
// light list had to match. Instead, latest_gvd follows the brightest upload
// of the current frame, and copies are matched against every colour variant
// seen this frame and the previous one.
inline GvdFields latest_gvd{};
inline bool latest_gvd_valid = false;
inline float frame_gvd_luma = -1.f;  // light luma of latest_gvd within this frame
inline uint32_t frame_gvd_uploads = 0;

struct ColourKey {
  uint32_t w[3];  // float bits of an original light colour
  bool operator==(const ColourKey& o) const { return w[0] == o.w[0] && w[1] == o.w[1] && w[2] == o.w[2]; }
};
constexpr size_t kMaxKeysPerLight = 16;
inline std::array<std::vector<ColourKey>, 2> frame_keys, prev_keys;  // per light
inline uint32_t stat_gvd_per_frame = 0, stat_variants[2] = {0, 0};
// A light that goes dark (night side, eclipse, hangar) keeps its identity as
// long as its direction stays continuous; forgotten only after this long.
constexpr uint32_t kForgetAfterFrames = 1800;

inline std::map<std::string, Rgb> colour_table;       // class -> colour
// class -> how "Star Colour" picks that class's colour ("<class>.source = ..." in the ini)
enum class StarSource { Constant, Measured, Table, Vanilla };
inline std::map<std::string, StarSource> class_policy;
inline std::map<std::string, float> class_saturation;  // class -> extra chroma multiplier ("<class>.saturation = 1.5")
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
inline uint64_t discovery_budget = 0;                      // bytes of unknown mid-size buffers to search this frame
inline int scan_frames_left = 0;
inline uint32_t scan_hits_logged = 0, scan_buffers = 0;
inline uint64_t scan_bytes = 0;
inline std::set<std::tuple<uint64_t, uint32_t, char, int, uint64_t>> scan_seen;
inline std::set<uint64_t> scan_hit_handles;
constexpr uint32_t kScanLogCap = 150;
inline std::array<uint32_t, static_cast<size_t>(StarFx::Count)> stat_fx_draws{};
inline std::set<std::string> star_dumps_seen;  // probe log: each distinct star constant block once
constexpr size_t kStarDumpCap = 64;
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
  std::map<std::string, StarSource> policy;
  std::map<std::string, float> saturation;
  parse(table_path, [&](const std::string& k, const std::string& v) {
    if (k.size() > 11 && k.compare(k.size() - 11, 11, ".saturation") == 0) {
      std::istringstream f(v);
      float x;
      if ((f >> x) && x >= 0.f) saturation[k.substr(0, k.size() - 11)] = x;
      return;
    }
    if (k.size() > 7 && k.compare(k.size() - 7, 7, ".source") == 0) {
      const std::string cls = k.substr(0, k.size() - 7);
      if (v == "constant") policy[cls] = StarSource::Constant;
      else if (v == "measured") policy[cls] = StarSource::Measured;
      else if (v == "table") policy[cls] = StarSource::Table;
      else if (v == "vanilla") policy[cls] = StarSource::Vanilla;
      return;
    }
    std::istringstream s(v);
    Rgb c;
    if ((s >> c.r >> c.g >> c.b) && c.r >= 0 && c.g >= 0 && c.b >= 0) table[k] = c;
  });
  parse(signatures_path, [&](const std::string& k, const std::string& v) { sigs[k] = v; });
  StateLock lock;
  colour_table = std::move(table);
  class_policy = std::move(policy);
  class_saturation = std::move(saturation);
  learned = std::move(sigs);
}

inline void SaveSignatures() {  // call with state_mutex held
  std::ofstream f(signatures_path, std::ios::trunc);
  f << "# Learned by RenoDX: star draw chromaticity -> star class.\n"
       "# Edit freely; delete a line to forget it.\n";
  for (auto& [k, v] : learned) f << k << " = " << v << "\n";
}

// Learned classes are keyed by the signature rounded to 3 decimals, and a
// star of the same class can land one rounding step away (an L dwarf at
// 0.625,0.242 against a learned 0.624,0.242). Exact match first, otherwise
// the nearest learned signature of the same draw kind within the tolerance,
// which is far below the distance between classes.
constexpr float kSignatureTolerance = 0.012f;  // white dwarfs sit >= 0.018 from the Y band
// Surface signatures of Y and T dwarfs form a flat band (g ~0.21, r 0.17..0.45)
// clearly off the main-sequence ramp, and both look the same in game, so an
// unlearned signature in that band counts as Y. L dwarfs lie on the ramp next
// to late M stars and cannot be told apart this way.
constexpr float kBrownDwarfBand[4] = {0.17f, 0.50f, 0.195f, 0.225f};  // r min, r max, g min, g max
inline bool InBrownDwarfBand(const std::string& sig) {
  const auto colon = sig.find(':');
  float r, g;
  return colon != std::string::npos && sig.compare(0, colon, "Surface") == 0
         && std::sscanf(sig.c_str() + colon + 1, "%f,%f", &r, &g) == 2 && r >= kBrownDwarfBand[0]
         && r <= kBrownDwarfBand[1] && g >= kBrownDwarfBand[2] && g <= kBrownDwarfBand[3];
}
inline std::string LookupClass(const std::string& sig) {
  if (sig.empty()) return {};
  if (auto it = learned.find(sig); it != learned.end()) return it->second;
  const auto colon = sig.find(':');
  float r, g;
  if (colon == std::string::npos || std::sscanf(sig.c_str() + colon + 1, "%f,%f", &r, &g) != 2) return {};
  std::string best;
  if (InBrownDwarfBand(sig)) best = "Y";
  float best_d = kSignatureTolerance;
  for (const auto& [k, v] : learned) {
    if (k.size() <= colon || k.compare(0, colon + 1, sig, 0, colon + 1) != 0) continue;
    float kr, kg;
    if (std::sscanf(k.c_str() + colon + 1, "%f,%f", &kr, &kg) != 2) continue;
    const float d = std::max(std::abs(kr - r), std::abs(kg - g));
    if (d <= best_d) best_d = d, best = v;
  }
  return best;
}

// The class whose policy, saturation and table colour apply. A star in the
// Y/T band gets the Y policy even when the journal labelled it otherwise: the
// game draws some cold T Tauri stars exactly like Y dwarfs (Synuefe EL-E
// a66-2, TTS at 450 K, surface 0.217,0.209, magenta disc and light), and the
// TTS policy would give it its blue constant. The label is kept for display.
inline std::string PolicyClass(const LightState& l) {
  return l.cls != "T" && InBrownDwarfBand(l.signature) ? std::string("Y") : l.cls;
}

inline const Rgb* LookupClassColour(const std::string& cls) {
  if (cls.empty()) return nullptr;
  if (auto it = colour_table.find(cls); it != colour_table.end()) return &it->second;
  for (size_t n = cls.size() - 1; n > 0; --n)  // "K_OrangeGiant" -> "K"
    if (auto it = colour_table.find(cls.substr(0, n)); it != colour_table.end()) return &it->second;
  return nullptr;
}

// ---- colour remap ----------------------------------------------------------
// Saturation in the HSV sense, 0 = grey, 1 = a pure hue.
inline float Saturation(const Rgb& c) {
  const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
  return mx > 0.f ? 1.f - mn / mx : 0.f;
}

// Hue angle in degrees (HSV), 0 = red, 120 = green, 240 = blue.
inline float Hue(const Rgb& c) {
  const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b}), d = mx - mn;
  if (!(d > 0.f)) return 0.f;
  float h;
  if (mx == c.r) h = (c.g - c.b) / d;
  else if (mx == c.g) h = 2.f + (c.b - c.r) / d;
  else h = 4.f + (c.r - c.g) / d;
  h *= 60.f;
  return h < 0.f ? h + 360.f : h;
}
inline float HueDistance(const Rgb& a, const Rgb& b) {
  const float d = std::abs(Hue(a) - Hue(b));
  return d > 180.f ? 360.f - d : d;
}

// Policy for a class, with the same prefix fallback as the colour table
// ("K_OrangeGiant" -> "K"); unlearned classes get the default.
inline StarSource LookupPolicy(const std::string& cls) {
  if (cls.empty()) return StarSource::Constant;
  if (auto it = class_policy.find(cls); it != class_policy.end()) return it->second;
  for (size_t n = cls.size() - 1; n > 0; --n)
    if (auto it = class_policy.find(cls.substr(0, n)); it != class_policy.end()) return it->second;
  return StarSource::Constant;
}

inline float LookupClassSaturation(const std::string& cls) {
  if (cls.empty()) return 1.f;
  if (auto it = class_saturation.find(cls); it != class_saturation.end()) return it->second;
  for (size_t n = cls.size() - 1; n > 0; --n)
    if (auto it = class_saturation.find(cls.substr(0, n)); it != class_saturation.end()) return it->second;
  return 1.f;
}

// The star's colour for "Star Colour" mode. The default is the star's own
// constant (the designers' hue), which looks right for ordinary classes.
// Classes that look odd with it get a policy in renodx-starlight.ini:
//   measured  the rendered disc's colour (brown dwarfs: magenta texture over
//             a blue constant)
//   table     that class's colour from the ini
//   vanilla   leave the game's light alone
// Returns nullptr when the light is to be left as the game made it.
inline const Rgb* StarColourFor(const LightState& l, Rgb& out, StarSource* used = nullptr) {
  const std::string cls = PolicyClass(l);
  StarSource src = LookupPolicy(cls);
  // not sampled yet: the class's table colour is a better stand-in than the constant
  if (src == StarSource::Measured && !(l.measured_valid && Luma(l.measured_acc) > 0.f)) src = StarSource::Table;
  if (src == StarSource::Table && LookupClassColour(cls) == nullptr) src = StarSource::Constant;
  if (used != nullptr) *used = src;
  switch (src) {
    case StarSource::Measured: out = l.measured_acc; return &out;
    case StarSource::Table: out = *LookupClassColour(cls); return &out;
    case StarSource::Vanilla: return nullptr;
    default: out = l.star_colour; return &out;
  }
}

// The colour the current mode wants for this light, or nullptr for "leave
// the game's light alone".
inline const Rgb* GoalColour(const LightState& l, Rgb& out) {
  const Rgb* target = nullptr;
  if (config.mode == 2.f) target = LookupClassColour(PolicyClass(l));
  if (target == nullptr) target = StarColourFor(l, out);
  else out = *target, target = &out;
  return target;
}

constexpr float kRolloverSeconds = 2.5f;   // hand-over inside a system: roll the tint over this long
constexpr uint64_t kHandoverGapFrames = 120;  // identified again within this: a hand-over, not an arrival

inline Rgb Chromaticity(const Rgb& c) {
  const float sum = c.r + c.g + c.b;
  return sum > 0.f ? Rgb{c.r / sum, c.g / sum, c.b / sum} : Rgb{1.f / 3, 1.f / 3, 1.f / 3};
}

// Once per frame (EndFrame, lock held): computes the goal and rolls the
// applied tint towards it. A goal of "vanilla" is the light's own
// chromaticity, so a class set to vanilla blends in and out like any other.
inline void UpdateApplied(LightState& l, uint64_t arrival) {
  if (!l.known || config.mode == 0.f) {
    l.rolling = false;
    return;
  }
  Rgb tmp;
  const Rgb* goal_ptr = GoalColour(l, tmp);
  Rgb goal;
  if (goal_ptr != nullptr && Luma(*goal_ptr) > 0.f) goal = Chromaticity(*goal_ptr);
  else if (l.base_valid && Luma(l.base_colour) > 0.f) goal = Chromaticity(l.base_colour);
  else return;
  const float goal_sat = LookupClassSaturation(PolicyClass(l));
  const auto now = std::chrono::steady_clock::now();

  if (l.applied_valid && l.applied_identity != l.signature) {
    // another star: a hand-over if the previous one was applied a moment
    // ago in the same system, otherwise (arrival after a jump) switch at once
    const bool handover = frame_index - l.applied_frame < kHandoverGapFrames && arrival == l.applied_arrival;
    if (handover) {
      l.roll_from = l.applied;
      l.roll_from_sat = l.applied_sat;
      l.roll_start = now;
      l.rolling = true;
    } else {
      l.rolling = false;
    }
  }
  if (!l.applied_valid) l.rolling = false;
  l.applied_identity = l.signature;
  l.applied_frame = frame_index;
  l.applied_arrival = arrival;

  float t = 1.f;
  if (l.rolling) {
    t = std::chrono::duration<float>(now - l.roll_start).count() / kRolloverSeconds;
    if (t >= 1.f) t = 1.f, l.rolling = false;
    t = t * t * (3.f - 2.f * t);  // ease in and out
  }
  l.applied = l.rolling ? Chromaticity({l.roll_from.r + (goal.r - l.roll_from.r) * t, l.roll_from.g + (goal.g - l.roll_from.g) * t,
                                        l.roll_from.b + (goal.b - l.roll_from.b) * t})
                        : goal;
  l.applied_sat = l.rolling ? l.roll_from_sat + (goal_sat - l.roll_from_sat) * t : goal_sat;
  l.applied_valid = true;
}

// Chroma comes from the chosen source; luminance stays exactly the game's.
inline Rgb RemapLight(const LightState& l, const Rgb& in) {
  const float y = Luma(in);
  if (!(y > 0.f) || config.mode == 0.f) return in;

  const Rgb* target = nullptr;
  Rgb star;
  float class_sat = 1.f;
  if (l.known && l.applied_valid) {
    target = &l.applied;  // the goal colour, rolled over on hand-overs (see UpdateApplied)
    class_sat = l.applied_sat;
  } else if (l.known) {
    target = GoalColour(l, star);
    class_sat = LookupClassSaturation(PolicyClass(l));
  }
  Rgb out = in;
  if (target != nullptr && Luma(*target) > 0.f) {
    // Multiplicative recolouring: the per-channel gain that turns the light's
    // unattenuated colour into the target chroma at unchanged luminance, then
    // applied to this copy. A copy already filtered by the atmosphere keeps
    // that filter on top of the new colour; outside an atmosphere base == in.
    const Rgb base = l.base_valid ? l.base_colour : in;
    const float yb = Luma(base), k = yb / Luma(*target), s = config.strength;
    const float floor = 1e-6f * yb;  // a zero channel in the base cannot be scaled: keep it tiny
    const Rgb gain{target->r * k / std::max(base.r, floor), target->g * k / std::max(base.g, floor),
                   target->b * k / std::max(base.b, floor)};
    out = {in.r + (in.r * gain.r - in.r) * s, in.g + (in.g * gain.g - in.g) * s, in.b + (in.b * gain.b - in.b) * s};
  }
  // the class's own chroma multiplier ("Y.saturation = 1.5": the game's Y dwarf
  // light has the right hue, only bland) on top of the global slider
  return Saturate(out, config.saturation * class_sat);
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
  ++frame_gvd_uploads;
  sh.gvd = g;
  sh.gvd_valid = true;

  // brightest upload of the frame defines the lights (see latest_gvd)
  float luma = 0.f;
  for (int i = 0; i < 2; ++i) {
    Rgb c{g.colour[i][0], g.colour[i][1], g.colour[i][2]};
    const float y = Luma(c);
    luma += y;
    if (y > 0.f) {
      ColourKey key;
      std::memcpy(key.w, g.colour[i], sizeof(key.w));
      auto& keys = frame_keys[i];
      if (keys.size() < kMaxKeysPerLight && std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
    }
  }
  if (!latest_gvd_valid || luma > frame_gvd_luma) {
    latest_gvd = g;
    latest_gvd_valid = true;
    frame_gvd_luma = luma;
  }

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
// Which buffer writes are searched (state_mutex held). Diagnostic scan: all up
// to kScanMax; otherwise small ones always, known kinds always, and a little
// discovery budget for the rest.
inline bool WantScan(const AnyBuffer& b) {
  if (scan_frames_left > 0) return b.size <= light_list::kScanMax;
  if (b.size <= light_list::kAutoMax) return true;
  if (b.size > light_list::kHotMax) return false;
  if (hot_kinds.contains({b.size, b.usage})) return true;
  return discovery_budget >= b.size;
}

inline bool ScanBuffer(uint64_t handle, const AnyBuffer& info, uint8_t* base, uint64_t size, const char* path) {
  if (!latest_gvd_valid || size < 12 || size > light_list::kScanMax) return false;
  if (!WantScan(info)) return false;
  const bool scanning = scan_frames_left > 0;
  if (scanning) {
    ++scan_buffers;
    scan_bytes += size;
  } else if (info.size > light_list::kAutoMax && !hot_kinds.contains({info.size, info.usage})) {
    discovery_budget -= std::min(discovery_budget, info.size);
  }
  const GvdFields& g = latest_gvd;
  // Every original colour of each light seen this frame or the previous one.
  // Patching a copy only does something once the light's star is known.
  std::array<std::vector<ColourKey>, 2> keys;
  bool any_key = false;
  for (int i = 0; i < 2; ++i) {
    if (!lights[i].known && !scanning) continue;
    keys[i] = frame_keys[i];
    for (const auto& k : prev_keys[i])
      if (std::find(keys[i].begin(), keys[i].end(), k) == keys[i].end()) keys[i].push_back(k);
    any_key = any_key || !keys[i].empty();
  }
  if (!any_key) return false;

  std::vector<uint8_t> local(base, base + size);  // one sequential read of (possibly write-combined) memory
  bool changed = false;
  for (uint64_t off = 0; off + 12 <= size; off += 4) {
    ColourKey w;
    std::memcpy(w.w, local.data() + off, sizeof(w.w));
    for (int i = 0; i < 2; ++i) {
      if (std::find(keys[i].begin(), keys[i].end(), w) == keys[i].end()) continue;
      if (scanning) LogScanHit('C', i, handle, info, path, off, local.data(), size);
      hot_kinds.insert({info.size, info.usage});
      ++stat_list_matched_frame;
      if (config.mode == 0.f) break;
      float orig[3];
      std::memcpy(orig, w.w, sizeof(orig));
      Rgb out = RemapLight(lights[i], {orig[0], orig[1], orig[2]});
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

// ---- pixel decoding (for the star readback) ---------------------------------
inline float HalfToFloat(uint16_t h) {
  const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
  float v;
  if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
  else if (e == 31) v = m == 0 ? INFINITY : NAN;
  else v = std::ldexp(1.f + static_cast<float>(m) / 1024.f, static_cast<int>(e) - 15);
  return s != 0u ? -v : v;
}
// R11G11B10_FLOAT: 5-bit exponents, 6/6/5-bit mantissas, no sign.
inline float SmallFloat(uint32_t bits, int mantissa_bits) {
  const uint32_t e = (bits >> mantissa_bits) & 0x1Fu, m = bits & ((1u << mantissa_bits) - 1u);
  const float mf = static_cast<float>(m) / static_cast<float>(1u << mantissa_bits);
  if (e == 0) return std::ldexp(mf, -14);
  if (e == 31) return m == 0 ? INFINITY : NAN;
  return std::ldexp(1.f + mf, static_cast<int>(e) - 15);
}

// Average (linear) colour of a mapped subresource, or nullopt for formats we
// do not decode. Only HDR scene formats and 8-bit RGBA are needed.
inline std::optional<Rgb> AverageTexels(reshade::api::format fmt, uint32_t w, uint32_t h, const uint8_t* data,
                                        uint32_t row_pitch, float* max_luma = nullptr) {
  using reshade::api::format;
  double sum[3] = {0, 0, 0};
  uint64_t n = 0;
  float peak = 0.f;
  auto add = [&](const float* c) {
    if (!Finite(c, 3)) return;
    for (int k = 0; k < 3; ++k) sum[k] += c[k];
    peak = std::max(peak, Luma({c[0], c[1], c[2]}));
    ++n;
  };
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* row = data + y * row_pitch;
    for (uint32_t x = 0; x < w; ++x) {
      float c[3];
      switch (fmt) {
        case format::r16g16b16a16_typeless:
        case format::r16g16b16a16_float: {
          uint16_t hv[3];
          std::memcpy(hv, row + x * 8, sizeof(hv));
          c[0] = HalfToFloat(hv[0]), c[1] = HalfToFloat(hv[1]), c[2] = HalfToFloat(hv[2]);
          break;
        }
        case format::r32g32b32a32_typeless:
        case format::r32g32b32a32_float:
          std::memcpy(c, row + x * 16, sizeof(c));
          break;
        case format::r11g11b10_float: {
          uint32_t v;
          std::memcpy(&v, row + x * 4, sizeof(v));
          c[0] = SmallFloat(v, 6), c[1] = SmallFloat(v >> 11, 6), c[2] = SmallFloat(v >> 22, 5);
          break;
        }
        case format::r8g8b8a8_typeless:
        case format::r8g8b8a8_unorm:
        case format::r8g8b8a8_unorm_srgb:
        case format::b8g8r8a8_typeless:
        case format::b8g8r8a8_unorm:
        case format::b8g8r8a8_unorm_srgb: {
          const bool bgr = fmt == format::b8g8r8a8_typeless || fmt == format::b8g8r8a8_unorm
                           || fmt == format::b8g8r8a8_unorm_srgb;
          const uint8_t* p = row + x * 4;
          c[0] = p[bgr ? 2 : 0] / 255.f, c[1] = p[1] / 255.f, c[2] = p[bgr ? 0 : 2] / 255.f;
          break;
        }
        default:
          return std::nullopt;
      }
      add(c);
    }
  }
  if (n == 0) return std::nullopt;
  if (max_luma != nullptr) *max_luma = peak;
  return Rgb{static_cast<float>(sum[0] / n), static_cast<float>(sum[1] / n), static_cast<float>(sum[2] / n)};
}

// ---- rendered star readback -------------------------------------------------
// Two halves, both WITHOUT state_mutex held around D3D calls (see StateLock):
// CopyStarWindow runs when the render target the star was drawn into gets
// unbound (the star pass is over, nothing has reused the target yet) and
// records the copy on that command list; RunStarReadback at present maps the
// finished copy. One copy in flight at a time; the staging texture persists.

// Called from OnBindRenderTargets, lock not held.
inline void CopyStarWindow(reshade::api::command_list* cmd_list, const StarProbe& p, int light) {
  auto* device = cmd_list->get_device();
  const auto desc = device->get_resource_desc(p.target);
  if (desc.type != reshade::api::resource_type::texture_2d) return;
  {
    StateLock lock;
    if (backbuffer_width == 0 || desc.texture.width * 2 < backbuffer_width) {  // side pass, not the main view
      ++stat_probes_skipped;
      return;
    }
  }
  if (desc.texture.samples > 1) {
    StateLock lock;
    if (!readback.warned_msaa) {
      readback.warned_msaa = true;
      reshade::log::message(reshade::log::level::info, "starlight readback: scene target is multisampled, not supported");
    }
    return;
  }
  const float fx = (p.ndc[0] * 0.5f + 0.5f) * static_cast<float>(desc.texture.width);
  const float fy = (0.5f - p.ndc[1] * 0.5f) * static_cast<float>(desc.texture.height);
  const int half = kProbeBox / 2;
  const int left = static_cast<int>(fx) - half, top = static_cast<int>(fy) - half;
  if (left < 0 || top < 0 || left + kProbeBox > static_cast<int>(desc.texture.width)
      || top + kProbeBox > static_cast<int>(desc.texture.height))
    return;

  // staging texture: kept across samples, replaced when the format changes
  reshade::api::resource staging{};
  reshade::api::resource stale{};
  {
    StateLock lock;
    if (readback.staging.handle != 0u && readback.format != desc.texture.format) {
      stale = readback.staging;
      readback.staging = {};
    }
    staging = readback.staging;
  }
  if (stale.handle != 0u) device->destroy_resource(stale);
  if (staging.handle == 0u) {
    const reshade::api::resource_desc staging_desc(kProbeBox, kProbeBox, 1, 1, desc.texture.format, 1,
                                                   reshade::api::memory_heap::gpu_to_cpu,
                                                   reshade::api::resource_usage::copy_dest);
    if (!device->create_resource(staging_desc, nullptr, reshade::api::resource_usage::copy_dest, &staging)) return;
  }
  reshade::api::subresource_box box{static_cast<uint32_t>(left), static_cast<uint32_t>(top), 0,
                                    static_cast<uint32_t>(left + kProbeBox), static_cast<uint32_t>(top + kProbeBox), 1};
  cmd_list->copy_texture_region(p.target, 0, &box, staging, 0, nullptr);
  StateLock lock;
  readback.staging = staging;
  readback.format = desc.texture.format;
  readback.pending = true;
  readback.frame = frame_index;
  readback.light = light;
  readback.target = p.target.handle;
  readback.width = desc.texture.width;
  readback.height = desc.texture.height;
  readback.px = left + half;
  readback.py = top + half;
  probe[light].last_sampled = frame_index;
}

// Colour of a star from its recent samples. The disc is not one colour (a Y
// dwarf alternates red and magenta patches) and the odd sample is something
// else entirely (a lit strut crossing the star), so instead of a running
// average: per-channel median of the samples' chromaticities, then the
// luminance-weighted mean of the samples near that median. Outliers are
// ignored, patches average out.
inline Rgb RobustColour(const std::array<Rgb, 32>& ring, uint32_t n, uint32_t* used) {
  std::array<std::array<float, 3>, 32> chroma{};
  std::array<float, 32> ch{};
  float median[3];
  for (uint32_t i = 0; i < n; ++i) {
    const float sum = ring[i].r + ring[i].g + ring[i].b;
    chroma[i] = sum > 0.f ? std::array<float, 3>{ring[i].r / sum, ring[i].g / sum, ring[i].b / sum} : std::array<float, 3>{0, 0, 0};
  }
  for (int k = 0; k < 3; ++k) {
    for (uint32_t i = 0; i < n; ++i) ch[i] = chroma[i][k];
    std::sort(ch.begin(), ch.begin() + n);
    median[k] = n % 2 == 1 ? ch[n / 2] : 0.5f * (ch[n / 2 - 1] + ch[n / 2]);
  }
  Rgb acc{0, 0, 0};
  uint32_t kept = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const float d = std::abs(chroma[i][0] - median[0]) + std::abs(chroma[i][1] - median[1]) + std::abs(chroma[i][2] - median[2]);
    if (d > kChromaInlier) continue;
    acc = {acc.r + ring[i].r, acc.g + ring[i].g, acc.b + ring[i].b};
    ++kept;
  }
  if (used != nullptr) *used = kept;
  if (kept == 0) return {median[0], median[1], median[2]};
  return acc;
}

// Called at present, before EndFrame, lock not held: maps a finished copy and
// folds it into the light's measured colour.
inline void RunStarReadback(reshade::api::command_queue* queue) {
  auto* device = queue->get_device();
  Readback rb;
  {
    StateLock lock;
    if (!readback.pending || frame_index - readback.frame < kReadbackDelay) return;
    rb = readback;
  }
  reshade::api::subresource_data mapped{};
  std::optional<Rgb> avg;
  float max_luma = 0.f;
  if (device->map_texture_region(rb.staging, 0, nullptr, reshade::api::map_access::read_only, &mapped)) {
    avg = AverageTexels(rb.format, kProbeBox, kProbeBox, static_cast<const uint8_t*>(mapped.data), mapped.row_pitch,
                        &max_luma);
    device->unmap_texture_region(rb.staging, 0);
  }
  StateLock lock;
  readback.pending = false;
  ++stat_readbacks;
  if (!avg.has_value()) {
    if (!readback.warned_format) {
      readback.warned_format = true;
      std::stringstream s;
      s << "starlight readback: scene format " << static_cast<uint32_t>(rb.format) << " not decoded";
      reshade::log::message(reshade::log::level::info, s.str().c_str());
    }
    return;
  }
  // Bright samples dominate (accumulated in linear light), and samples far
  // dimmer than the brightest seen are not the star at all (a strut in front,
  // empty space): they are dropped rather than allowed to decay it.
  LightState& l = lights[rb.light];
  const float y = Luma(*avg);
  // The star's core in the HDR scene is far brighter than anything else that
  // could be at its position (sky, a strut); a window without such a pixel is
  // not the star.
  const bool accept = y > 0.f && max_luma >= kMinPeakPixelLuma && y >= kMinSampleFraction * l.measured_peak;
  if (accept) {
    // the peak forgets slowly, so a far (dim) star or one bright foreign
    // sample does not lock the gate for good
    l.measured_peak = std::max(l.measured_peak * kPeakDecay, y);
    l.ring[l.ring_pos] = *avg;
    l.ring_pos = (l.ring_pos + 1) % static_cast<uint32_t>(l.ring.size());
    l.ring_count = std::min<uint32_t>(l.ring_count + 1, static_cast<uint32_t>(l.ring.size()));
    l.measured_acc = RobustColour(l.ring, l.ring_count, &l.measured_used);
    l.measured_valid = true;
    ++l.measured_samples;
  }
  if (config.probe_log != 0.f && (l.measured_samples <= kReadbackLogPerStar || stat_readbacks % 50 == 0)) {
    const float s3 = l.measured_acc.r + l.measured_acc.g + l.measured_acc.b;
    std::stringstream s;
    s << "starlight readback: light " << rb.light + 1 << (accept ? " sample (" : " dropped (") << avg->r << "," << avg->g
      << "," << avg->b << ") luma " << y << " brightest " << max_luma << " peak " << l.measured_peak << " from "
      << std::hex << rb.target << std::dec
      << " " << rb.width << "x" << rb.height << " fmt" << static_cast<uint32_t>(rb.format) << " at " << rb.px << ","
      << rb.py << " -> chroma (" << (s3 > 0 ? l.measured_acc.r / s3 : 0) << "," << (s3 > 0 ? l.measured_acc.g / s3 : 0)
      << "," << (s3 > 0 ? l.measured_acc.b / s3 : 0) << ")";
    reshade::log::message(reshade::log::level::info, s.str().c_str());
  }
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
  if (in_state_lock) return true;
  auto si = kStarShaders.find(crc);
  if (si == kStarShaders.end()) return true;
  const StarShader& star = si->second;

  StateLock lock;
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

  const Rgb seen{colour[0], colour[1], colour[2]};  // fallback until the star has been measured
  const Rgb sig{colour[0], colour[1], colour[2]};

  // Probe log: dump each distinct star's constants (c0..c3) and its textures once.
  if (config.probe_log != 0.f && star_dumps_seen.size() < kStarDumpCap) {
    const size_t n = std::min<size_t>(b2->bytes.size(), 64);
    char kb[96];
    std::snprintf(kb, sizeof(kb), "%s:%.3f,%.3f,%.3f", FxName(star.fx), sig.r, sig.g, sig.b);  // ignore jitter
    if (star_dumps_seen.insert(kb).second) {
      std::stringstream s;
      s << "starlight star: " << FxName(star.fx) << " b2";
      for (size_t k = 0; k + 4 <= n; k += 4) {
        float f;
        std::memcpy(&f, b2->bytes.data() + k, 4);
        s << (k % 16 == 0 ? " c" + std::to_string(k / 16) + "=(" : ",") << f << (k % 16 == 12 ? ")" : "");
      }
      const GvdFields& gg = b1->gvd;
      s << " light0=(" << gg.colour[0][0] << "," << gg.colour[0][1] << "," << gg.colour[0][2] << ")"
        << " seen=(" << seen.r << "," << seen.g << "," << seen.b << ")";
      auto* device = cmd_list->get_device();
      for (size_t t = 0; t < bit->second.ps_srv.size(); ++t) {
        const auto view = bit->second.ps_srv[t];
        if (view.handle == 0u) continue;
        const auto res = device->get_resource_from_view(view);
        if (res.handle == 0u) continue;
        const auto d = device->get_resource_desc(res);
        if (d.type != reshade::api::resource_type::texture_2d) continue;
        s << " t" << t << "=" << std::hex << res.handle << std::dec << ":fmt" << static_cast<uint32_t>(d.texture.format)
          << ":" << d.texture.width << "x" << d.texture.height << ":mips" << d.texture.levels;
      }
      reshade::log::message(reshade::log::level::info, s.str().c_str());
    }
  }

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
        if (better) {
          fm.any = true, fm.fx = star.fx, fm.colour = seen, fm.sig_colour = sig, fm.cos = c;
        }
        if (const auto k = static_cast<size_t>(star.fx); !fm.fx_any[k] || c > fm.fx_cos[k]) {
          fm.fx_any[k] = true, fm.fx_sig[k] = sig, fm.fx_cos[k] = c;
        }
        // The surface draw tells where the star is on screen and which target
        // it is drawn into: sample the rendered colour there at present.
        if (star.fx == StarFx::Surface && sc[3] > 0.f && bit->second.rt0.handle != 0u) {
          const float nx = sc[0] / sc[3], ny = sc[1] / sc[3];
          if (std::abs(nx) < 0.95f && std::abs(ny) < 0.95f)
            probe[i] = {true, bit->second.rt0, {nx, ny}, frame_index, reinterpret_cast<uint64_t>(cmd_list),
                        probe[i].last_sampled};
        }
      }
    }
  }
  return true;
}

// ---- per frame -------------------------------------------------------------
constexpr uint32_t kIdentityHold = 300;  // frames a better-ranked identity outranks a worse-ranked match

inline void ResetMeasurement(LightState& l) {
  l.measured_valid = false;
  l.measured_samples = l.measured_used = 0;
  l.measured_peak = 0.f;
  l.measured_acc = {};
  l.ring_count = l.ring_pos = 0;
}

inline void EndFrame() {
  StateLock lock;
  ++frame_index;
  bool learned_changed = false;

  for (int i = 0; i < 2; ++i) {
    LightState& l = lights[i];
    const bool active = latest_gvd_valid
                        && Luma({latest_gvd.colour[i][0], latest_gvd.colour[i][1], latest_gvd.colour[i][2]}) > 0.f;
    bool jumped = false;
    if (active) {
      const float* d = latest_gvd.dir[i];
      const float dot = l.dir[0] * d[0] + l.dir[1] * d[1] + l.dir[2] * d[2];
      // a sudden jump in direction = a different star (jump, binary swap);
      // also checked when the light comes back after being dark
      jumped = l.known && dot < std::cos(3.f * 3.14159265f / 180.f);
      if (jumped && !frame_match[i].any) l.known = false;
      std::memcpy(l.dir, d, sizeof(l.dir));
      l.game_colour = {latest_gvd.colour[i][0], latest_gvd.colour[i][1], latest_gvd.colour[i][2]};
      l.frames_inactive = 0;
    } else if (++l.frames_inactive > kForgetAfterFrames) {
      l.known = false;  // dark for long: whatever lights up next is treated as new
    }
    l.active = active;

    if (!l.known) ResetMeasurement(l);  // a new star: measure afresh
    const FrameMatch& fm = frame_match[i];
    if (fm.any) {
      const std::string sig = SignatureKey(fm.fx, fm.sig_colour);
      // A worse-ranked draw (the corona billboard while the surface is briefly
      // unmatched) must not replace a better identity of the same star: its
      // signature has no learned class and the light would snap to the
      // corona's constant. It may only take over after a direction jump or
      // once the better source has been silent for a while.
      const bool keep_current = l.known && !jumped && FxRank(fm.fx) > FxRank(l.source)
                                && l.frames_since_match < kIdentityHold;
      if (!keep_current) {
        if (l.known && sig != l.signature) ResetMeasurement(l);
        l.known = true;
        l.source = fm.fx;
        l.star_colour = fm.colour;
        l.match_cos = fm.cos;
        l.frames_since_match = 0;
        l.signature = sig;
        l.cls = LookupClass(l.signature);
      }
      // propagate a known class to the other draw kinds seen for this star
      if (l.known && !l.cls.empty()) {
        for (size_t k = 0; k < fm.fx_any.size(); ++k) {
          if (!fm.fx_any[k]) continue;
          const std::string other = SignatureKey(static_cast<StarFx>(k), fm.fx_sig[k]);
          if (other.empty() || LookupClass(other) == l.cls) continue;
          learned[other] = l.cls;
          learned_changed = true;
          std::string msg = "starlight: learned " + other + " = " + l.cls + " (from " + l.signature + ")";
          reshade::log::message(reshade::log::level::info, msg.c_str());
        }
      }
    } else {
      ++l.frames_since_match;
    }
  }

  for (auto& l : lights) UpdateApplied(l, arrival_seq);

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
        << " col=(" << l.game_colour.r << "," << l.game_colour.g << "," << l.game_colour.b << ") best_cos=" << stat_best_cos[i]
        << " variants=" << frame_keys[i].size();
      if (!l.active) s << " dark=" << l.frames_inactive;
      if (l.known) s << " star=" << FxName(l.source) << " " << l.signature << " class=" << l.cls;
      if (l.known && PolicyClass(l) != l.cls) s << " policy=" << PolicyClass(l) << " (Y/T band)";
    }
    s << " | gvd/frame=" << frame_gvd_uploads << " colour copies: kinds=" << hot_kinds.size()
      << " patched=" << stat_list_patched << " matched/frame=" << stat_list_matched_frame;
    reshade::log::message(reshade::log::level::info, s.str().c_str());
  }

  frame_match = {};
  stat_list_matched_frame = 0;
  stat_gvd_per_frame = frame_gvd_uploads;
  for (int i = 0; i < 2; ++i) stat_variants[i] = static_cast<uint32_t>(frame_keys[i].size());
  // unattenuated light colour = component-wise max of this frame's and last frame's variants
  for (int i = 0; i < 2; ++i) {
    Rgb mx{0, 0, 0};
    bool any = false;
    for (const auto* set : {&frame_keys[i], &prev_keys[i]})
      for (const ColourKey& k : *set) {
        float c[3];
        std::memcpy(c, k.w, sizeof(c));
        mx = {std::max(mx.r, c[0]), std::max(mx.g, c[1]), std::max(mx.b, c[2])};
        any = true;
      }
    if (any) {
      lights[i].base_colour = mx;
      lights[i].base_valid = true;
    }
  }
  prev_keys = std::move(frame_keys);
  for (auto& k : frame_keys) k.clear();
  frame_gvd_uploads = 0;
  frame_gvd_luma = -1.f;  // next frame picks its own brightest upload
  discovery_budget = (frame_index % light_list::kDiscoveryPeriod == 0) ? light_list::kDiscoveryBytes : 0;

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
          StateLock lock;
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
  if (in_state_lock) return;
  if (desc.type != reshade::api::resource_type::buffer) return;
  const uint64_t size = desc.buffer.size;
  const auto usage = static_cast<uint32_t>(desc.usage);
  const bool is_cb = (usage & static_cast<uint32_t>(reshade::api::resource_usage::constant_buffer)) != 0u;
  const bool is_gvd = is_cb && size >= gvd::kSize && size <= gvd::kMaxSize;
  if (!is_gvd) {
    StateLock lock;
    any_buffers[resource.handle] = {size, usage, nullptr};
  }
  if (!is_cb) return;
  if (!is_gvd && size > kSmallBufferMax) return;
  StateLock lock;
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

inline void OnDestroyResource(reshade::api::device* device, reshade::api::resource resource) {
  if (in_state_lock) return;
  StateLock lock;
  shadows.erase(resource.handle);
  any_buffers.erase(resource.handle);
  for (auto& p : probe)
    if (p.target.handle == resource.handle) p = {};
  for (auto& [cl, b] : bindings)
    if (b.rt0.handle == resource.handle) b.rt0 = {};
  (void)device;
}

inline void OnBindRenderTargets(reshade::api::command_list* cmd_list, uint32_t count,
                                const reshade::api::resource_view* rtvs, reshade::api::resource_view) {
  if (in_state_lock) return;
  reshade::api::resource rt{};
  if (count > 0 && rtvs != nullptr && rtvs[0].handle != 0u) rt = cmd_list->get_device()->get_resource_from_view(rtvs[0]);
  // The target the star was drawn into is being switched away: the star pass
  // is complete, copy the window out now (before anything reuses the target).
  StarProbe p;
  int light = -1;
  {
    StateLock lock;
    auto& b = bindings[reinterpret_cast<uint64_t>(cmd_list)];
    const auto previous = b.rt0;
    b.rt0 = rt;
    if (previous.handle != 0u && previous.handle != rt.handle && !readback.pending) {
      for (int i = 0; i < 2 && light < 0; ++i) {
        StarProbe& q = probe[i];
        if (!q.want || q.cmd_list != reinterpret_cast<uint64_t>(cmd_list) || q.target.handle != previous.handle) continue;
        q.want = false;
        if (q.frame != frame_index || frame_index - q.last_sampled < kProbePeriod) continue;
        p = q;
        light = i;
      }
    }
  }
  if (light >= 0) CopyStarWindow(cmd_list, p, light);
}

inline void OnMapBufferRegion(reshade::api::device*, reshade::api::resource resource, uint64_t offset, uint64_t,
                              reshade::api::map_access access, void** data) {
  if (in_state_lock) return;
  if (data == nullptr || *data == nullptr || offset != 0 || access == reshade::api::map_access::read_only) return;
  StateLock lock;
  auto it = shadows.find(resource.handle);
  if (it != shadows.end()) it->second.mapped = static_cast<uint8_t*>(*data);
  auto ab = any_buffers.find(resource.handle);
  if (ab != any_buffers.end() && latest_gvd_valid && WantScan(ab->second)) {
    ab->second.mapped = static_cast<uint8_t*>(*data);
  }
}

inline void OnUnmapBufferRegion(reshade::api::device*, reshade::api::resource resource) {
  if (in_state_lock) return;
  StateLock lock;
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
  if (in_state_lock) return false;
  if (in_own_update || data == nullptr) return false;
  std::vector<uint8_t> copy;
  bool changed = false;
  {
    StateLock lock;
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
  if (in_state_lock) return false;
  StateLock lock;
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
  if (in_state_lock) return false;
  return OnCopyBufferRegion(cmd_list, src, 0, dst, 0, UINT64_MAX);
}

inline void StartScan() {
  StateLock lock;
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
  if (in_state_lock) return;
  if (update.descriptors == nullptr) return;
  int stage = -1;
  if (stages == reshade::api::shader_stage::vertex) stage = 0;
  if (stages == reshade::api::shader_stage::pixel) stage = 1;
  if (stage < 0) return;
  if (update.type == reshade::api::descriptor_type::shader_resource_view) {
    if (stage != 1) return;
    const auto* views = static_cast<const reshade::api::resource_view*>(update.descriptors);
    StateLock lock;
    auto& b = bindings[reinterpret_cast<uint64_t>(cmd_list)];
    for (uint32_t i = 0; i < update.count; ++i) {
      const uint32_t slot = update.binding + i;
      if (slot < b.ps_srv.size()) b.ps_srv[slot] = views[i];
    }
    return;
  }
  if (update.type != reshade::api::descriptor_type::constant_buffer) return;
  const auto* ranges = static_cast<const reshade::api::buffer_range*>(update.descriptors);
  StateLock lock;
  auto& b = bindings[reinterpret_cast<uint64_t>(cmd_list)];
  for (uint32_t i = 0; i < update.count; ++i) {
    const uint32_t slot = update.binding + i;
    if (slot < 3) b.cb[stage][slot] = ranges[i];
  }
}

inline void OnPresent(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain, const reshade::api::rect*,
                      const reshade::api::rect*, uint32_t, const reshade::api::rect*) {
  if (swapchain != nullptr && queue != nullptr) {
    const auto bb = swapchain->get_current_back_buffer();
    if (bb.handle != 0u) {
      const auto desc = queue->get_device()->get_resource_desc(bb);
      StateLock lock;
      backbuffer_width = desc.texture.width;
      backbuffer_height = desc.texture.height;
    }
  }
  if (queue != nullptr) RunStarReadback(queue);
  EndFrame();
}

// ---- settings UI -----------------------------------------------------------
inline int label_choice = 0;

inline std::vector<renodx::utils::settings::Setting*> Settings() {
  using renodx::utils::settings::Setting;
  using renodx::utils::settings::SettingValueType;
  return {
      new Setting{
          .key = "StarlightColour",  // renamed when the Saturation mode was dropped, so stored values do not shift
          .binding = &config.mode,
          .value_type = SettingValueType::INTEGER,
          .default_value = 1.f,
          .label = "Starlight Colour",
          .section = "Starlight",
          .tooltip = "Star Colour: tint each light with the colour of the star actually casting it.\n"
                     "Class Table: use renodx-starlight.ini for stars whose class has been learned.",
          .labels = {"Vanilla", "Star Colour", "Class Table"},
      },
      new Setting{
          .key = "StarlightStrength",
          .binding = &config.strength,
          .default_value = 100.f,
          .label = "Strength",
          .section = "Starlight",
          .max = 100.f,
          .is_enabled = [] { return config.mode != 0.f; },
          .parse = [](float v) { return v * 0.01f; },
      },
      new Setting{
          // 100 on the slider is a chroma multiplier of 0.7: the raw star
          // colours are more saturated than looks natural in the lit scene,
          // and 0.7 is where the owner and testers settled. New key so a
          // value saved by an older build (percent of 1.0) is not reused.
          .key = "StarlightChroma",
          .binding = &config.saturation,
          .default_value = 100.f,
          .label = "Saturation",
          .section = "Starlight",
          .tooltip = "Chroma of the tint. 100 is the natural look; above it the colour gets heavy quickly.",
          .max = 200.f,
          .is_enabled = [] { return config.mode != 0.f; },
          .parse = [](float v) { return v * 0.007f; },
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
            StateLock lock;
            ImGui::Text("GlobalViewData uploads %u (rejected %u, size %llu)   star draws %u (unbound %u)",
                        stat_gvd_uploads, stat_gvd_rejected, static_cast<unsigned long long>(stat_gvd_size_seen),
                        stat_star_draws, stat_star_unbound);
            for (int i = 0; i < 2; ++i) {
              const auto& l = lights[i];
              if (!l.active) {
                ImGui::Text("Light %d: off", i + 1);
                continue;
              }
              ImGui::Text("Light %d: game %.3f %.3f %.3f   unattenuated %.3f %.3f %.3f", i + 1, l.game_colour.r,
                          l.game_colour.g, l.game_colour.b, l.base_colour.r, l.base_colour.g, l.base_colour.b);
              if (l.known) {
                ImGui::Text("   star via %s  %.3f %.3f %.3f  cos %.5f%s  %u frames ago",
                            FxName(l.source), l.star_colour.r, l.star_colour.g, l.star_colour.b, l.match_cos,
                            "", l.frames_since_match);
                ImGui::Text("   %s  class: %s", l.signature.c_str(), l.cls.empty() ? "unlearned" : l.cls.c_str());
                if (l.measured_valid) {
                  const float s = l.measured_acc.r + l.measured_acc.g + l.measured_acc.b;
                  StarSource src;
                  Rgb chosen;
                  const Rgb* used = StarColourFor(l, chosen, &src);
                  ImGui::Text("   seen on screen: chroma %.3f %.3f %.3f  sat %.2f hue %.0f  (%u samples, %u of last %u used)",
                              l.measured_acc.r / s, l.measured_acc.g / s, l.measured_acc.b / s,
                              Saturation(l.measured_acc), Hue(l.measured_acc), l.measured_samples, l.measured_used, l.ring_count);
                  if (l.rolling) ImGui::Text("   rolling over from the previous star");
                  const float su = used != nullptr ? used->r + used->g + used->b : 1.f;
                  ImGui::Text("   policy for class %s (saturation x%.2f): %s%s%.3f %.3f %.3f",
                              PolicyClass(l).empty() ? "(unlearned)" : PolicyClass(l).c_str(),
                              LookupClassSaturation(PolicyClass(l)),
                              src == StarSource::Measured ? "measured" : src == StarSource::Table ? "class table"
                              : src == StarSource::Vanilla ? "vanilla" : "constant",
                              used != nullptr ? ", using chroma " : " (light left as the game made it) ",
                              used != nullptr ? used->r / su : 0.f, used != nullptr ? used->g / su : 0.f,
                              used != nullptr ? used->b / su : 0.f);
                } else {
                  ImGui::TextUnformatted("   seen on screen: not measured yet (using the star's constant)");
                }
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
            ImGui::Text("GlobalViewData per frame: %u uploads, colour variants %u / %u",
                        stat_gvd_per_frame, stat_variants[0], stat_variants[1]);
            ImGui::Text("Learned signatures: %zu   star readbacks: %u (%u side-pass probes skipped)", learned.size(),
                        stat_readbacks, stat_probes_skipped);
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
      reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
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
      reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);
      reshade::unregister_event<reshade::addon_event::copy_buffer_region>(OnCopyBufferRegion);
      reshade::unregister_event<reshade::addon_event::copy_resource>(OnCopyResource);
      journal_run = false;
      if (journal_thread.joinable()) journal_thread.detach();  // never block in DllMain
      break;
  }
}

}  // namespace elitedangerous::starlight
