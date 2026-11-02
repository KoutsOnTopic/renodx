/*
 * Copyright (C) 2026 KoutsOnTopic
 * SPDX-License-Identifier: MIT
 */

#pragma once

// Experimental light-to-star detection ("Journal + engine (test)"), selected
// with the Detection setting next to the default "Star draws" detection.
//
// NOT a standalone header: starlight.hpp includes it part-way through, after
// the types and functions it uses (LightState, FrameMatch, LookupClass,
// UpdateApplied, StateLock, ...), with the starlight namespace closed around
// the include.
//
// Why it exists: STARLIGHT-ATTRIBUTION-RESEARCH.md (owner's working folder)
// found the default detection right whenever a star is drawn, but weak at
// naming the star's class (a learned colour signature resolves correctly
// about 60 % of the time out of sample) and blind to stars that are never
// drawn (a distant primary seen from a far companion). This mode tests three
// alternatives on top of the default star-draw match:
//   1. Journal context. The arrival class names the first star identified
//      after a hyperspace jump. The system's star list (from Scan events)
//      narrows the class guess, but ONLY once a discovery scan listed the
//      stars, this visit or an earlier one (owner's rule: in an unexplored
//      system the journal knows only the arrival star). Status.json pauses
//      new identities while a map is open; the journal does so during a
//      hyperspace jump.
//   2. Radius link. Corona, edge and lens-flare shaders read
//      gStellarBodyRadius, the drawn star's true radius; matched against the
//      journal's scanned Radius it names the exact body (class, temperature).
//      The unit is unknown, so the ratio is calibrated on arrival stars.
//   3. Local star slots. gLocalStarDirectionSize1..3 hold the directions of
//      up to three local stars (lighting space, read by the skybox shaders),
//      which can recognise a star that is not drawn as a body.
// It also remembers each identified star's direction for the visit, so a
// light that loses its star (map opened, binary hand-over and back) gets it
// back without waiting for a draw.
//
// Isolation: when the mode is not selected, none of this runs (every hook is
// behind Selected()) and ActiveLight() returns the default light state, so
// the default behaviour is unchanged. When selected, the default detection
// still runs every frame; this mode copies its result into `view`, overrides
// the identity/class fields, and the colour pipeline (policies, saturation,
// roll-over) is the same code fed with `view` instead of `lights`.
//
// Data for statistics: while selected it appends one JSON object per line to
// renodx-starlight-detection.jsonl next to the game executable: a "frame"
// row per light every kSampleEvery frames with every method's verdict and
// evidence (main_*, exp_* including the tint applied, R_*, slot_*, and m_* =
// every class method evaluated on its own), plus "system" (the journal's star
// list whenever it changes),
// "draw", "journal", "visit", "calib" and "session" rows.

namespace elitedangerous::starlight::experiment {

// ---- layout (GlobalViewData = b1 byte offsets, b0 = per-draw shared block) ---
namespace layout {
constexpr uint64_t kLocalStars = 1280;       // gLocalStarDirectionSize1..3: 3 x float4 (xyz direction, w size)
constexpr uint64_t kLocalStarInfo = 1328;    // gLocalStarAdditionalInfo: float4 (x falloff exponent, y magnitude offset)
constexpr uint64_t kBodyRadius = 2016;       // gStellarBodyRadius: float, the drawn body's radius (render units)
constexpr uint64_t kCameraPos = 4400;        // gCameraPos: float3
constexpr uint64_t kObjectToLighting = 144;  // b0 cb0[9..11]: gObjectToLightingMatrices rows
}  // namespace layout

constexpr float kLinkTolerance = 0.01f;        // |R * scale - Rj| / Rj to accept a radius link
constexpr float kLinkGap = 0.02f;              // the second-best journal star must be at least this much worse
constexpr float kNearestInSystem = 0.05f;      // signature distance when only the system's classes are candidates
constexpr float kRestoreDegrees = 3.f;         // a light this close to a remembered star's direction is that star
constexpr uint64_t kRestoreFrames = 600;       // ...if that star was seen this recently (or a local star slot confirms)
constexpr uint64_t kArrivalWindow = 600;       // frames after arrival in which the first identity is the arrival star
constexpr uint64_t kSampleEvery = 30;          // frames between "frame" rows
constexpr size_t kMemoryMax = 8;               // remembered stars per visit
constexpr size_t kDrawRowsPerVisit = 96;       // "draw" rows per visit
constexpr uint64_t kFileCap = 256ull << 20;    // stop writing the data file beyond this size
constexpr uint32_t kFlagFsdCharging = 1u << 17;  // Status.json Flags
constexpr uint32_t kFlagFsdJump = 1u << 30;

// ---- journal state (written by the thread under StateLock) ------------------
struct JStar {
  std::string body, type;
  int subclass = -1;
  double temperature = 0, radius = 0, distance_ls = -1;
};
struct JSystem {
  std::string name;
  std::vector<JStar> stars;
  bool discovery = false;  // a discovery scan listed the stars (this visit or an earlier one)
};
struct JournalState {
  bool ready = false;          // history indexed
  uint64_t address = 0;        // current SystemAddress
  JSystem system;
  uint64_t visit = 0;          // arrivals seen (indexing included); compared only for change
  bool live_jump = false;      // the current visit began with a live hyperspace jump
  std::string arrival_class;   // its StartJump StarClass
  bool hyperspace = false;     // between a live hyperspace StartJump and the arrival
  bool status_ok = false;
  int gui_focus = 0;
  uint32_t flags = 0;
};
inline JournalState journal;

inline bool MapOpen() { return journal.status_ok && (journal.gui_focus == 6 || journal.gui_focus == 7 || journal.gui_focus == 8); }
inline bool Paused() { return journal.hyperspace || MapOpen(); }
inline bool Luminous(const std::string& type) { return type != "H" && type.find("BlackHole") == std::string::npos; }

// ---- per-frame evidence (render thread, under StateLock) ---------------------
struct FrameEvidence {
  uint32_t uploads = 0, slot_uploads = 0;
  float slot_cos[2] = {-2.f, -2.f};
  int slot_k[2] = {-1, -1};
  float slots[3][4] = {};  // last carried slot values (raw)
  float slot_info[4] = {};
  float radius[2] = {0.f, 0.f};  // radius of a corona/edge/flare draw matched to the light
  float radius_cos[2] = {-2.f, -2.f};
  StarFx radius_kind[2] = {StarFx::Count, StarFx::Count};
  float radius_dist[2] = {0.f, 0.f};  // |centre| in lighting space
  // Best journal star for that radius, by name and system: the journal thread
  // may replace journal.system between a draw and EndFrame, so no indices.
  std::string best_body[2], best_type[2];
  uint64_t best_addr[2] = {0, 0};
  bool linked[2] = {false, false};
  float link_err[2] = {-1.f, -1.f}, link_gap[2] = {-1.f, -1.f};
};
inline FrameEvidence ev;

// ---- per-visit state ---------------------------------------------------------
struct Remembered {
  float dir[3] = {0, 0, 0};
  std::string signature, cls, body;
  Rgb star_colour;
  StarFx source = StarFx::Count;
  bool l_corona = false;  // LightState::l_corona, sticky per star like the default's
  uint64_t frame = 0;
};
struct Visit {
  uint64_t visit = UINT64_MAX, address = 0, start_frame = 0;
  std::map<std::string, std::string> link;       // signature -> body name (radius link)
  std::map<std::string, std::string> sig_class;  // signature -> class this mode resolved
  std::string arrival_sig;
  bool arrival_done = false;
  bool calibrated = false;
  std::vector<Remembered> memory;
  std::set<std::string> draw_rows;  // "draw" rows already written
  size_t system_row_key = 0;        // star list last written in a "system" row
};
inline Visit visit;
inline std::vector<double> calibrations;  // journal Radius / gStellarBodyRadius per arrival
inline double radius_scale = 1.0;         // median of calibrations; 1 until the first

struct Verdict {
  bool known = false, restored = false, paused = false;
  std::string cls, how, body;
};
inline std::array<LightState, 2> view;  // what the colour pipeline uses when this mode is selected
inline std::array<Verdict, 2> verdict;

// ---- data file (rows queued under StateLock, written by the thread) ----------
inline std::vector<std::string> pending_rows;
inline std::filesystem::path data_path;
inline std::atomic_bool selected_flag = false;  // Selected(), mirrored for the thread
inline std::atomic_bool thread_run = false;
inline std::thread thread;
inline bool session_row_written = false;
inline std::string status_note;  // overlay: what the thread is doing

inline bool Selected() { return config.detection == 1.f; }

// The light state the colour pipeline uses: the default one unless this mode
// is selected. Follows selected_flag (set at the end of each frame), so view is
// only used once EndFrame has built it; always lights[i] in the default mode.
inline const LightState& ActiveLight(int i) { return selected_flag ? view[i] : lights[i]; }

// ---- small JSON writer -------------------------------------------------------
inline std::string JsonEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 2);
  for (const char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
    else o += c;
  }
  return o;
}
struct Row {
  std::stringstream s;
  bool first = true;
  // with_frame = false on the journal thread, which must not read frame_index
  explicit Row(const char* type, bool with_frame = true) {
    s.precision(9);
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_s(&tm, &t);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    char ts[40];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    s << "{";
    Str("t", ts);
    Str("type", type);
    if (with_frame) Int("frame", frame_index);
  }
  void Key(const char* k) {
    s << (first ? "" : ",") << "\"" << k << "\":";
    first = false;
  }
  Row& Str(const char* k, const std::string& v) {
    Key(k);
    s << "\"" << JsonEscape(v) << "\"";
    return *this;
  }
  Row& Num(const char* k, double v) {
    Key(k);
    if (std::isfinite(v)) s << v;
    else s << "null";
    return *this;
  }
  Row& Int(const char* k, int64_t v) {  // exact: flags, addresses, counters
    Key(k);
    s << v;
    return *this;
  }
  Row& Raw(const char* k, const std::string& json) {
    Key(k);
    s << json;
    return *this;
  }
  Row& Bool(const char* k, bool v) {
    Key(k);
    s << (v ? "true" : "false");
    return *this;
  }
  Row& Vec(const char* k, const float* v, int n) {
    Key(k);
    s << "[";
    for (int i = 0; i < n; ++i) s << (i ? "," : "") << (std::isfinite(v[i]) ? v[i] : 0.f);
    s << "]";
    return *this;
  }
  std::string Finish() {
    s << "}";
    return s.str();
  }
  void Queue() {  // call with StateLock held
    if (pending_rows.size() < 20000) pending_rows.push_back(Finish());
  }
};

// ---- helpers -----------------------------------------------------------------
inline float Len3(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// Same projection test as OnStarDraw (kept separate so the default path is
// untouched): cosine between the light's vanishing point and the draw centre.
inline float DrawCos(const float m[4][4], const GvdFields& g, int i) {
  const float L[3] = {-g.dir[i][0], -g.dir[i][1], -g.dir[i][2]};
  float lc[4];
  for (int k = 0; k < 4; ++k) lc[k] = g.to_clip_t[0][k] * L[0] + g.to_clip_t[1][k] * L[1] + g.to_clip_t[2][k] * L[2];
  static const float origin[3] = {0, 0, 0};
  const float* cands[2] = {g.body_pos, origin};
  float best = -2.f;
  for (const float* p : cands) {
    float sc[4];
    for (int k = 0; k < 4; ++k) sc[k] = m[k][0] * p[0] + m[k][1] * p[1] + m[k][2] * p[2] + m[k][3];
    best = std::max(best, CosXYW(sc, lc));
  }
  return best;
}

inline float MatchThreshold() { return std::cos(config.match_degrees * 3.14159265f / 180.f); }

inline const JStar* FindStar(const std::string& body) {
  for (const auto& s : journal.system.stars)
    if (s.body == body) return &s;
  return nullptr;
}

// Luminous stars of the system per class, minus classes already given to
// other surface signatures this visit.
inline std::map<std::string, int> UnassignedClasses(const std::string& except_sig) {
  std::map<std::string, int> left;
  for (const auto& s : journal.system.stars)
    if (Luminous(s.type)) ++left[s.type];
  for (const auto& [sig, cls] : visit.sig_class)
    if (sig != except_sig && sig.rfind("Surface:", 0) == 0)
      if (auto it = left.find(cls); it != left.end() && it->second > 0) --it->second;
  for (auto it = left.begin(); it != left.end();) it = it->second > 0 ? std::next(it) : left.erase(it);
  return left;
}

// Nearest learned signature of the same draw kind whose class is a candidate.
inline std::string NearestInClasses(const std::string& sig, const std::map<std::string, int>& classes) {
  const auto colon = sig.find(':');
  float r, g;
  if (colon == std::string::npos || std::sscanf(sig.c_str() + colon + 1, "%f,%f", &r, &g) != 2) return {};
  std::string best;
  float best_d = kNearestInSystem;
  for (const auto& [k, v] : learned) {
    if (!classes.contains(v) || k.size() <= colon || k.compare(0, colon + 1, sig, 0, colon + 1) != 0) continue;
    float kr, kg;
    if (std::sscanf(k.c_str() + colon + 1, "%f,%f", &kr, &kg) != 2) continue;
    const float d = std::max(std::abs(kr - r), std::abs(kg - g));
    if (d < best_d) best_d = d, best = v;
  }
  return best;
}

// ---- hooks called from starlight.hpp (all with StateLock held) ---------------

// Every GlobalViewData upload: shadow the extra fields this mode reads, and
// test the light directions of this upload against its local star slots.
inline void OnGvd(BufferShadow& sh, const uint8_t* base, const GvdFields& g) {
  GvdExtra& x = sh.extra;
  std::memcpy(x.local_star, base + layout::kLocalStars, sizeof(x.local_star));
  std::memcpy(x.local_info, base + layout::kLocalStarInfo, sizeof(x.local_info));
  std::memcpy(&x.body_radius, base + layout::kBodyRadius, sizeof(x.body_radius));
  std::memcpy(x.camera, base + layout::kCameraPos, sizeof(x.camera));
  x.valid = Finite(&x.local_star[0][0], 12) && Finite(x.local_info, 4) && Finite(&x.body_radius, 1) && Finite(x.camera, 3);
  ++ev.uploads;
  if (!x.valid) return;
  bool any = false;
  for (const auto& s : x.local_star) any = any || Len3(s) > 0.f;
  if (!any) return;
  ++ev.slot_uploads;
  std::memcpy(ev.slots, x.local_star, sizeof(ev.slots));
  std::memcpy(ev.slot_info, x.local_info, sizeof(ev.slot_info));
  for (int j = 0; j < 2; ++j) {
    if (!(Luma({g.colour[j][0], g.colour[j][1], g.colour[j][2]}) > 0.f)) continue;
    const float dl = Len3(g.dir[j]);
    if (!(dl > 0.f)) continue;
    for (int k = 0; k < 3; ++k) {
      const float sl = Len3(x.local_star[k]);
      if (!(sl > 0.f)) continue;
      const float* s = x.local_star[k];
      // slot: camera -> star; -dir: towards the light
      const float c = -(s[0] * g.dir[j][0] + s[1] * g.dir[j][1] + s[2] * g.dir[j][2]) / (sl * dl);
      if (c > ev.slot_cos[j]) ev.slot_cos[j] = c, ev.slot_k[j] = k;
    }
  }
}

// Every star draw with its bindings resolved: the radius link, plus one
// "draw" row per (kind, signature, radius) per visit for the statistics.
inline void OnStarDraw(StarFx fx, const Rgb& sig_colour, const float m[4][4], const BufferShadow& b0,
                       const BufferShadow& b1) {
  const GvdExtra& x = b1.extra;
  if (!x.valid) return;
  const GvdFields& g = b1.gvd;
  const float R = x.body_radius;
  const std::string sig = SignatureKey(fx, sig_colour);
  const bool carries_radius = fx == StarFx::Corona || fx == StarFx::Edge || fx == StarFx::Flare;

  // centre in lighting space, as vs_SunBillboardGlow computes it
  float dist_ls = -1.f;
  if (b0.bytes.size() >= layout::kObjectToLighting + 48) {
    float o[3][4];
    std::memcpy(o, b0.bytes.data() + layout::kObjectToLighting, sizeof(o));
    float c[3];
    for (int r = 0; r < 3; ++r) c[r] = o[r][0] * g.body_pos[0] + o[r][1] * g.body_pos[1] + o[r][2] * g.body_pos[2] + o[r][3];
    if (Finite(c, 3)) dist_ls = Len3(c);
  }
  const float rel[3] = {g.body_pos[0] - x.camera[0], g.body_pos[1] - x.camera[1], g.body_pos[2] - x.camera[2]};
  const float dist_cam = Len3(rel);

  // best and second-best journal star by radius
  int best = -1;
  float best_err = 1e9f, second_err = 1e9f;
  if (R > 0.f) {
    const auto& stars = journal.system.stars;
    for (size_t k = 0; k < stars.size(); ++k) {
      if (!(stars[k].radius > 0.0)) continue;
      const float err = static_cast<float>(std::abs(R * radius_scale - stars[k].radius) / stars[k].radius);
      if (err < best_err) second_err = best_err, best_err = err, best = static_cast<int>(k);
      else if (err < second_err) second_err = err;
    }
  }
  // Only against a list a discovery scan completed: before the honk the list
  // usually holds just the arrival star, and any star of similar size would
  // "match" it with no runner-up to rule it out.
  const bool linked = carries_radius && journal.system.discovery && best >= 0 && best_err < kLinkTolerance
                      && second_err - best_err >= kLinkGap;

  int matched = -1;
  float matched_cos = -2.f;
  const float threshold = MatchThreshold();
  for (int i = 0; i < 2; ++i) {
    if (!(Luma({g.colour[i][0], g.colour[i][1], g.colour[i][2]}) > 0.f)) continue;
    const float c = DrawCos(m, g, i);
    if (c > matched_cos) matched_cos = c, matched = c >= threshold ? i : -1;
    if (c < threshold || !carries_radius || !(R > 0.f)) continue;
    if (c > ev.radius_cos[i]) {
      ev.radius[i] = R, ev.radius_cos[i] = c, ev.radius_kind[i] = fx, ev.radius_dist[i] = dist_ls;
      ev.best_body[i] = best >= 0 ? journal.system.stars[best].body : std::string{};
      ev.best_type[i] = best >= 0 ? journal.system.stars[best].type : std::string{};
      ev.best_addr[i] = journal.address;
      ev.linked[i] = linked;
      ev.link_err[i] = best >= 0 ? best_err : -1.f;
      ev.link_gap[i] = best >= 0 && second_err < 1e8f ? second_err - best_err : -1.f;
    }
    if (linked && visit.address == journal.address) visit.link[sig] = journal.system.stars[best].body;
  }

  char key[96];
  std::snprintf(key, sizeof(key), "%s|%.4g|%d", sig.c_str(), R, matched);
  if (visit.draw_rows.size() < kDrawRowsPerVisit && visit.draw_rows.insert(key).second) {
    Row row("draw");
    row.Str("kind", FxName(fx)).Str("sig", sig).Int("light", matched).Num("cos", matched_cos).Num("R", R)
        .Num("dist_ls", dist_ls).Num("dist_cam", dist_cam)
        .Num("angle_deg", dist_ls > R && R > 0.f ? std::asin(R / dist_ls) * 57.29578f : -1.f)
        .Str("best_body", best >= 0 ? journal.system.stars[best].body : "")
        .Num("best_err", best >= 0 ? best_err : -1.f)
        .Num("second_err", second_err < 1e8f ? second_err : -1.f)
        .Num("scale", radius_scale)
        .Bool("linked", linked)
        .Str("sys", journal.system.name);
    row.Queue();
  }
}

inline void Remember(const LightState& m, const Verdict& vd) {
  Remembered* r = nullptr;
  for (auto& e : visit.memory)
    if (e.signature == m.signature) r = &e;
  if (r == nullptr) {
    if (visit.memory.size() >= kMemoryMax) visit.memory.erase(visit.memory.begin());
    r = &visit.memory.emplace_back();
  }
  std::memcpy(r->dir, m.dir, sizeof(r->dir));
  r->signature = m.signature, r->cls = vd.cls, r->body = vd.body;
  r->star_colour = m.star_colour, r->source = m.source, r->frame = frame_index;
  r->l_corona = r->l_corona || m.l_corona;
}

// The default detection expired a light's identity on arrival (a star of the
// previous system, see the top of EndFrame in starlight.hpp). This thread
// polls the journal faster, so the new visit may already remember that star;
// drop it, or Recall would restore it by direction at once.
inline void Forget(const std::string& signature) {
  std::erase_if(visit.memory, [&](const Remembered& r) { return r.signature == signature; });
  visit.sig_class.erase(signature);  // else it counts as assigned (UnassignedClasses)
  if (visit.arrival_sig == signature) {  // taken as the arrival star by mistake: let the real one claim it
    visit.arrival_sig.clear();
    visit.arrival_done = false;
  }
}

// True if this remembered star is what the other light is showing now.
inline bool HeldByOtherLight(const Remembered& e, int light) {
  const LightState& o = lights[1 - light];
  return o.known && e.signature == o.signature;
}

inline const Remembered* Recall(const float dir[3], int light) {
  const float threshold = std::cos(kRestoreDegrees * 3.14159265f / 180.f);
  const bool slot_confirms = ev.slot_cos[light] >= MatchThreshold();
  const Remembered* best = nullptr;
  float best_dot = threshold;
  for (const auto& e : visit.memory) {
    if (HeldByOtherLight(e, light)) continue;
    const float d = e.dir[0] * dir[0] + e.dir[1] * dir[1] + e.dir[2] * dir[2];
    if (d < best_dot) continue;
    if (frame_index - e.frame > kRestoreFrames && !slot_confirms) continue;
    best_dot = d, best = &e;
  }
  return best;
}

// The class for an identified star, best evidence first.
inline void ResolveClass(int i, const LightState& m, Verdict& vd) {
  const std::string& sig = m.signature;
  // 1. radius link (this frame or earlier this visit)
  if (ev.linked[i] && ev.best_addr[i] == journal.address && m.frames_since_match == 0) visit.link[sig] = ev.best_body[i];
  if (auto it = visit.link.find(sig); it != visit.link.end()) {
    if (const JStar* s = FindStar(it->second); s != nullptr) {
      vd.cls = s->type, vd.how = "radius link", vd.body = s->body;
      visit.sig_class[sig] = vd.cls;
      return;
    }
  }
  // 2. the first identity of light 0 after a live hyperspace arrival is the arrival star
  if (i == 0 && journal.live_jump && !visit.arrival_done && !Paused()) {
    if (frame_index - visit.start_frame <= kArrivalWindow) {
      visit.arrival_sig = sig;
      Row("visit").Str("event", "arrival_identity").Str("sig", sig).Str("class", journal.arrival_class)
          .Str("sys", journal.system.name).Queue();
    }
    visit.arrival_done = true;
  }
  if (!visit.arrival_sig.empty() && sig == visit.arrival_sig && !journal.arrival_class.empty()) {
    vd.cls = journal.arrival_class, vd.how = "arrival class";
    for (const auto& s : journal.system.stars)
      if (s.distance_ls == 0.0) vd.body = s.body;
    visit.sig_class[sig] = vd.cls;
    return;
  }
  // 3. the system's star list, only once a discovery scan listed the stars
  if (journal.system.discovery && !journal.system.stars.empty()) {
    std::map<std::string, int> in_system;
    for (const auto& s : journal.system.stars)
      if (Luminous(s.type)) ++in_system[s.type];
    if (!m.cls.empty() && in_system.contains(m.cls)) {
      vd.cls = m.cls, vd.how = "learned, in system list";
    } else if (std::string n = NearestInClasses(sig, in_system); !n.empty()) {
      vd.cls = n, vd.how = "nearest learned in system list";
    } else if (const Remembered* r = Recall(m.dir, i); m.cls.empty() && r != nullptr && !r->cls.empty()) {
      // the same star through another draw kind (its corona after its surface)
      vd.cls = r->cls, vd.how = "same direction as a remembered star", vd.body = r->body;
    } else if (auto left = UnassignedClasses(sig); m.cls.empty() && left.size() == 1) {
      vd.cls = left.begin()->first, vd.how = "only unassigned class in system";
    } else {
      vd.cls = m.cls, vd.how = m.cls.empty() ? "unlearned, no fit in system list" : "learned, not in system list";
    }
    if (!vd.cls.empty()) visit.sig_class[sig] = vd.cls;
    return;
  }
  // 4. the default lookup
  vd.cls = m.cls, vd.how = m.cls.empty() ? "unlearned" : "learned";
  if (!vd.cls.empty()) visit.sig_class[sig] = vd.cls;
}

// Class for a light recognised only by a local star slot: the system's one
// luminous star not yet tied to a drawn identity, if the list is complete.
inline std::string SlotClass(const float dir[3], int light, bool* l_corona) {
  *l_corona = false;
  if (!journal.system.discovery) return {};
  // a remembered star within 15 degrees most likely moved while unidentified:
  // it is that star, not the one class left over
  const float near_dot = std::cos(15.f * 3.14159265f / 180.f);
  const Remembered* nearest = nullptr;
  float best_dot = near_dot;
  for (const auto& e : visit.memory) {
    if (HeldByOtherLight(e, light)) continue;
    const float d = e.dir[0] * dir[0] + e.dir[1] * dir[1] + e.dir[2] * dir[2];
    if (d >= best_dot) best_dot = d, nearest = &e;
  }
  if (nearest != nullptr) {
    *l_corona = nearest->l_corona;
    return nearest->cls;
  }
  auto left = UnassignedClasses("");
  return left.size() == 1 ? left.begin()->first : std::string{};
}

// Copy the default detection's state into view, keeping view's own roll-over state.
inline void CopyKeepApplied(const LightState& m, LightState& v) {
  const LightState keep = v;
  v = m;
  v.applied_valid = keep.applied_valid, v.applied = keep.applied, v.applied_sat = keep.applied_sat;
  v.roll_from = keep.roll_from, v.roll_from_sat = keep.roll_from_sat, v.rolling = keep.rolling;
  v.roll_start = keep.roll_start, v.applied_identity = keep.applied_identity;
  v.applied_frame = keep.applied_frame, v.applied_arrival = keep.applied_arrival;
  v.applied_time = keep.applied_time, v.holding = keep.holding;
}

// Every class method evaluated on its own for the default identity, without
// side effects, so the data shows what each would have said (not only the
// one that won in ResolveClass). Prefix m_.
inline void MethodColumns(Row& row, int i, const LightState& m) {
  const bool listed = journal.system.discovery && !journal.system.stars.empty();
  std::string link_body, link_type;
  if (m.known) {
    if (auto it = visit.link.find(m.signature); it != visit.link.end()) {
      link_body = it->second;
      if (const JStar* s = FindStar(link_body); s != nullptr) link_type = s->type;
    }
  }
  const bool is_arrival = m.known && !visit.arrival_sig.empty() && m.signature == visit.arrival_sig;
  std::map<std::string, int> in_system;
  for (const auto& s : journal.system.stars)
    if (Luminous(s.type)) ++in_system[s.type];
  std::string nearest, single;
  if (listed && m.known) {
    nearest = NearestInClasses(m.signature, in_system);
    if (auto left = UnassignedClasses(m.signature); left.size() == 1) single = left.begin()->first;
  }
  row.Str("m_link_body", link_body).Str("m_link_type", link_type)
      .Bool("m_is_arrival", is_arrival).Str("m_arrival_class", is_arrival ? journal.arrival_class : "")
      .Bool("m_listed", listed).Str("m_learned", m.known ? m.cls : "")
      .Bool("m_learned_in_list", listed && m.known && in_system.contains(m.cls))
      .Str("m_nearest_in_list", nearest).Str("m_unassigned_single", single);
  (void)i;
}

// The system's journal star list, written at visit start and whenever it
// changes, so the analysis knows what was knowable at each moment.
inline double Finite0(double v) { return std::isfinite(v) ? v : 0.0; }

inline void SystemRowIfChanged() {
  const size_t key = journal.system.stars.size() * 2 + (journal.system.discovery ? 1 : 0) + 1;
  if (key == visit.system_row_key) return;
  visit.system_row_key = key;
  std::stringstream a;
  a.precision(9);
  a << "[";
  for (size_t k = 0; k < journal.system.stars.size(); ++k) {
    const JStar& st = journal.system.stars[k];
    a << (k ? "," : "") << "{\"body\":\"" << JsonEscape(st.body) << "\",\"star_type\":\"" << JsonEscape(st.type)
      << "\",\"subclass\":" << st.subclass << ",\"T\":" << Finite0(st.temperature) << ",\"R\":" << Finite0(st.radius)
      << ",\"dist_ls\":" << Finite0(st.distance_ls) << "}";
  }
  a << "]";
  Row("system").Str("sys", journal.system.name).Int("addr", static_cast<int64_t>(journal.address))
      .Bool("discovery", journal.system.discovery).Raw("stars", a.str()).Queue();
}

inline void QueueFrameRow(int i, const LightState& m, const LightState& v, const Verdict& vd) {
  Row row("frame");
  const float s = m.game_colour.r + m.game_colour.g + m.game_colour.b;
  const float chroma[2] = {s > 0.f ? m.game_colour.r / s : 0.f, s > 0.f ? m.game_colour.g / s : 0.f};
  const FrameMatch& fm = frame_match[i];
  // the tint this mode applies (chromaticity r,g; 0,0 = the game's own light)
  const bool tinted = config.mode != 0.f && v.applied_valid && (v.known || v.holding);
  const float applied[2] = {tinted ? v.applied.r : 0.f, tinted ? v.applied.g : 0.f};
  row.Int("light", i).Bool("active", m.active).Vec("dir", m.dir, 3).Vec("chroma", chroma, 2).Num("luma", Luma(m.game_colour))
      .Str("sys", journal.system.name).Int("addr", static_cast<int64_t>(journal.address)).Int("visit", static_cast<int64_t>(journal.visit))
      .Int("gui", journal.gui_focus).Int("flags", journal.flags).Bool("hyperspace", journal.hyperspace).Bool("paused", vd.paused)
      // default detection
      .Bool("main_known", m.known).Str("main_kind", m.known ? FxName(m.source) : "").Str("main_sig", m.known ? m.signature : "")
      .Str("main_cls", m.known ? m.cls : "").Int("main_since", m.frames_since_match).Str("main_policy", m.known ? PolicyClass(m) : "")
      // this frame's star draws
      .Bool("draw", fm.any).Str("draw_kind", fm.any ? FxName(fm.fx) : "").Num("draw_cos", fm.any ? fm.cos : -2.f)
      // this mode
      .Bool("exp_known", v.known).Str("exp_cls", vd.cls).Str("exp_how", vd.how).Str("exp_body", vd.body)
      .Bool("exp_restored", vd.restored).Str("exp_policy", v.known ? PolicyClass(v) : "")
      .Vec("exp_applied", applied, 2).Num("exp_applied_sat", tinted ? v.applied_sat : 0.f)
      .Bool("exp_rolling", v.rolling).Bool("exp_holding", v.holding)
      // radius evidence
      .Num("R", ev.radius[i]).Str("R_kind", ev.radius[i] > 0.f ? FxName(ev.radius_kind[i]) : "").Num("R_dist", ev.radius_dist[i])
      .Str("R_best_body", ev.best_body[i]).Str("R_best_type", ev.best_type[i]).Bool("R_linked", ev.linked[i])
      .Bool("R_same_sys", ev.best_addr[i] == journal.address).Num("R_err", ev.link_err[i]).Num("R_gap", ev.link_gap[i])
      // local star slots
      .Int("slot_k", ev.slot_k[i] + 1).Num("slot_cos", ev.slot_cos[i])
      .Int("slot_uploads", ev.slot_uploads).Int("uploads", ev.uploads)
      // journal context
      .Str("arrival_class", journal.arrival_class).Bool("discovery", journal.system.discovery)
      .Int("stars", static_cast<int64_t>(journal.system.stars.size())).Num("scale", radius_scale);
  MethodColumns(row, i, m);
  if (i == 0 && ev.slot_uploads > 0) row.Vec("slots", &ev.slots[0][0], 12).Vec("slot_info", ev.slot_info, 4);
  row.Queue();
}

// Once per frame, after the default identity logic and before its roll-over
// update: build view from lights and this mode's decisions.
inline void EndFrame(uint64_t arrival) {
  if (!session_row_written) {
    session_row_written = true;
    Row("session").Str("note", "experimental detection selected").Num("match_degrees", config.match_degrees).Queue();
  }
  if (journal.visit != visit.visit || journal.address != visit.address) {
    visit = {};
    visit.visit = journal.visit, visit.address = journal.address, visit.start_frame = frame_index;
    Row("visit").Str("event", "start").Str("sys", journal.system.name).Bool("live_jump", journal.live_jump)
        .Str("arrival_class", journal.arrival_class).Int("stars", static_cast<int64_t>(journal.system.stars.size()))
        .Bool("discovery", journal.system.discovery).Queue();
  }
  SystemRowIfChanged();
  const bool paused = Paused();
  const float threshold = MatchThreshold();
  for (int i = 0; i < 2; ++i) {
    const LightState& m = lights[i];
    LightState& v = view[i];
    Verdict& vd = verdict[i];
    vd = {};
    vd.paused = paused;
    CopyKeepApplied(m, v);
    if (m.known) {
      ResolveClass(i, m, vd);
      v.cls = vd.cls;
      if (!paused) Remember(m, vd);
    } else if (m.active) {
      const bool may_recall = !paused || ev.slot_cos[i] >= threshold;  // map/tunnel lights are not stars
      if (const Remembered* r = may_recall ? Recall(m.dir, i) : nullptr; r != nullptr) {
        v.known = true, v.signature = r->signature, v.cls = r->cls, v.star_colour = r->star_colour, v.source = r->source;
        v.l_corona = r->l_corona;
        vd.cls = r->cls, vd.body = r->body, vd.how = "remembered direction", vd.restored = true;
      } else if (!paused && ev.slot_cos[i] >= threshold) {
        bool l_corona = false;
        const std::string cls = SlotClass(m.dir, i, &l_corona);
        if (!cls.empty()) {
          v.known = true, v.signature = "Slot:" + cls, v.cls = cls, v.source = StarFx::Count;
          v.l_corona = l_corona;
          const Rgb* t = LookupClassColour(cls);
          v.star_colour = t != nullptr ? *t : Rgb{};
          vd.cls = cls, vd.how = "local star slot";
        } else {
          vd.how = "local star slot, class unknown";
        }
      } else {
        vd.how = paused ? "paused" : "not identified";
      }
    }
    vd.known = v.known;
    if (v.signature != m.signature) {  // the measurement belongs to the default identity
      v.measured_valid = false, v.measured_samples = v.measured_used = 0, v.ring_count = v.ring_pos = 0;
    }
    UpdateApplied(v, arrival);
    if (frame_index % kSampleEvery == 0) QueueFrameRow(i, m, v, vd);
  }

  // radius calibration: the arrival star's journal radius over the drawn radius
  if (!visit.calibrated && !visit.arrival_sig.empty() && !paused && lights[0].known
      && lights[0].signature == visit.arrival_sig && ev.radius[0] > 0.f) {
    for (const auto& s : journal.system.stars) {
      if (s.distance_ls != 0.0 || !(s.radius > 0.0)) continue;
      const double ratio = s.radius / ev.radius[0];
      calibrations.push_back(ratio);
      std::vector<double> sorted = calibrations;
      std::sort(sorted.begin(), sorted.end());
      radius_scale = sorted[sorted.size() / 2];
      visit.calibrated = true;
      Row("calib").Str("sys", journal.system.name).Str("body", s.body).Str("star_type", s.type).Num("journal_R", s.radius)
          .Num("drawn_R", ev.radius[0]).Str("kind", FxName(ev.radius_kind[0])).Num("ratio", ratio)
          .Num("scale", radius_scale).Int("n", static_cast<int64_t>(calibrations.size())).Queue();
      std::stringstream msg;
      msg << "starlight exp: radius calibration at " << s.body << ": journal " << s.radius << " / drawn "
          << ev.radius[0] << " = " << ratio << " (median of " << calibrations.size() << ": " << radius_scale << ")";
      reshade::log::message(reshade::log::level::info, msg.str().c_str());
      break;
    }
  }
  ev = {};
}

inline void DrawOverlay() {  // StateLock held by the caller
  ImGui::Separator();
  ImGui::Text("Experimental detection: %s", status_note.c_str());
  ImGui::Text("   system %s: %zu stars in journal%s, radius scale %.6g (%zu calibrations)%s",
              journal.system.name.empty() ? "?" : journal.system.name.c_str(), journal.system.stars.size(),
              journal.system.discovery ? " (discovery scanned)" : " (not discovery scanned: list not used)", radius_scale,
              calibrations.size(), MapOpen() ? "  [map open]" : journal.hyperspace ? "  [hyperspace]" : "");
  for (int i = 0; i < 2; ++i) {
    const LightState& m = lights[i];
    const Verdict& vd = verdict[i];
    if (!m.active) continue;
    const std::string cur = m.known ? (m.cls.empty() ? std::string("unlearned") : m.cls) : std::string("none");
    const std::string alt = vd.known ? (vd.cls.empty() ? std::string("unlearned") : vd.cls) : std::string("none");
    ImGui::Text("   Light %d: star draws say %s | test mode says %s via %s%s%s  [%s]", i + 1, cur.c_str(), alt.c_str(),
                vd.how.c_str(), vd.body.empty() ? "" : ", ", vd.body.c_str(), cur == alt ? "agree" : "DIFFER");
  }
}

// ---- journal thread ----------------------------------------------------------
inline double JsonNumber(const std::string& line, const char* key, double fallback = 0.0) {
  const std::string k = std::string("\"") + key + "\":";
  const auto a = line.find(k);
  if (a == std::string::npos) return fallback;
  const char* p = line.c_str() + a + k.size();
  while (*p == ' ') ++p;
  char* end = nullptr;
  const double v = std::strtod(p, &end);
  return end == p ? fallback : v;
}

inline uint64_t JsonUint(const std::string& line, const char* key) {
  const std::string k = std::string("\"") + key + "\":";
  const auto a = line.find(k);
  if (a == std::string::npos) return 0;
  const char* p = line.c_str() + a + k.size();
  while (*p == ' ') ++p;
  return std::strtoull(p, nullptr, 10);
}

struct ThreadState {
  std::map<uint64_t, JSystem> history;  // SystemAddress -> stars seen in any journal
  uint64_t current = 0;
  std::string pending_class;  // StartJump (Hyperspace) StarClass waiting for its FSDJump
  uint64_t visit = 0;
  bool live_jump = false, hyperspace = false;
  std::string arrival_class;
  std::chrono::steady_clock::time_point jump_started{};
  bool dirty = true;
  std::vector<std::string> rows;  // journal rows to queue
};

inline bool Has(const std::string& line, const char* event) {
  return line.find(std::string("\"event\":\"") + event + "\"") != std::string::npos;
}

inline void HandleLine(const std::string& line, bool live, ThreadState& st) {
  if (Has(line, "Scan") && line.find("\"StarType\"") != std::string::npos) {
    const auto addr = JsonUint(line, "SystemAddress");
    JStar s;
    s.body = JsonField(line, "BodyName");
    s.type = JsonField(line, "StarType");
    s.subclass = static_cast<int>(JsonNumber(line, "Subclass", -1));
    s.temperature = JsonNumber(line, "SurfaceTemperature");
    s.radius = JsonNumber(line, "Radius");
    s.distance_ls = JsonNumber(line, "DistanceFromArrivalLS", -1);
    auto& sys = st.history[addr != 0 ? addr : st.current];
    bool found = false;
    for (auto& e : sys.stars)
      if (e.body == s.body) e = s, found = true;
    if (!found) sys.stars.push_back(s);
    st.dirty = true;
    if (live) {
      Row row("journal", false);
      row.Str("event", "Scan").Str("body", s.body).Str("star_type", s.type).Int("subclass", s.subclass)
          .Num("T", s.temperature).Num("R", s.radius).Num("dist_ls", s.distance_ls).Str("scan", JsonField(line, "ScanType"));
      st.rows.push_back(row.Finish());
    }
  } else if (Has(line, "FSSDiscoveryScan")) {
    const auto addr = JsonUint(line, "SystemAddress");
    st.history[addr != 0 ? addr : st.current].discovery = true;
    st.dirty = true;
    if (live) {
      Row row("journal", false);
      row.Str("event", "FSSDiscoveryScan").Int("bodies", static_cast<int64_t>(JsonNumber(line, "BodyCount")));
      st.rows.push_back(row.Finish());
    }
  } else if (Has(line, "StartJump")) {
    if (JsonField(line, "JumpType") == "Hyperspace") {
      st.pending_class = JsonField(line, "StarClass");
      if (live) {
        st.hyperspace = true, st.jump_started = std::chrono::steady_clock::now(), st.dirty = true;
        Row row("journal", false);
        row.Str("event", "StartJump").Str("class", st.pending_class);
        st.rows.push_back(row.Finish());
      }
    }
  } else if (Has(line, "FSDJump") || Has(line, "CarrierJump") || Has(line, "Location")) {
    const auto addr = JsonUint(line, "SystemAddress");
    st.current = addr;
    auto& sys = st.history[addr];
    sys.name = JsonField(line, "StarSystem");
    // every arrival starts a visit; only a live hyperspace jump has an arrival class
    const bool jump = Has(line, "FSDJump");
    ++st.visit;
    st.live_jump = live && jump && !st.pending_class.empty();
    st.arrival_class = st.live_jump ? st.pending_class : std::string{};
    st.hyperspace = false;
    if (live) {
      Row row("journal", false);
      row.Str("event", jump ? "FSDJump" : Has(line, "CarrierJump") ? "CarrierJump" : "Location").Str("sys", sys.name)
          .Int("addr", static_cast<int64_t>(addr)).Str("arrival_class", st.arrival_class)
          .Int("known_stars", static_cast<int64_t>(sys.stars.size())).Bool("discovery", sys.discovery);
      st.rows.push_back(row.Finish());
    }
    st.pending_class.clear();
    st.dirty = true;
  }
}

// Oldest first by write time, like JournalLoop: file names changed format in
// 2022 (Journal.YYMMDDhhmmss -> Journal.YYYY-MM-DDThhmmss) and do not sort.
inline std::vector<std::filesystem::path> JournalFiles(const std::filesystem::path& dir) {
  std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> found;
  std::error_code ec;
  for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
    const auto n = e.path().filename().string();
    if (n.rfind("Journal.", 0) == 0 && e.path().extension() == ".log") found.emplace_back(e.last_write_time(ec), e.path());
  }
  std::sort(found.begin(), found.end());
  std::vector<std::filesystem::path> files;
  for (auto& f : found) files.push_back(std::move(f.second));
  return files;
}

inline void Publish(ThreadState& st, bool ready) {
  StateLock lock;
  if (st.dirty || ready != journal.ready) {
    journal.address = st.current;
    auto it = st.history.find(st.current);
    journal.system = it != st.history.end() ? it->second : JSystem{};
    journal.visit = st.visit;
    journal.live_jump = st.live_jump;
    journal.arrival_class = st.arrival_class;
    journal.hyperspace = st.hyperspace;
    journal.ready = ready;
    st.dirty = false;
  }
  for (auto& r : st.rows)
    if (pending_rows.size() < 20000) pending_rows.push_back(std::move(r));
  st.rows.clear();
}

inline void PollStatus(const std::filesystem::path& dir, ThreadState& st) {
  std::ifstream f(dir / "Status.json", std::ios::binary);
  if (!f) return;
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (text.find("\"Flags\"") == std::string::npos) return;  // being rewritten: try again next time
  const int gui = static_cast<int>(JsonNumber(text, "GuiFocus", 0));
  const auto flags = static_cast<uint32_t>(JsonNumber(text, "Flags", 0));
  // a hyperspace countdown that was cancelled leaves no journal event
  if (st.hyperspace && std::chrono::steady_clock::now() - st.jump_started > std::chrono::seconds(3)
      && (flags & (kFlagFsdCharging | kFlagFsdJump)) == 0u) {
    st.hyperspace = false, st.dirty = true;
  }
  if (st.hyperspace && std::chrono::steady_clock::now() - st.jump_started > std::chrono::seconds(90)) st.hyperspace = false, st.dirty = true;
  StateLock lock;
  if (journal.gui_focus != gui || !journal.status_ok) {
    Row row("journal");
    row.Str("event", "GuiFocus").Int("gui", gui).Int("flags", flags);
    row.Queue();
  }
  journal.status_ok = true;
  journal.gui_focus = gui;
  journal.flags = flags;
}

inline void FlushRows() {
  std::vector<std::string> rows;
  {
    StateLock lock;
    rows.swap(pending_rows);
  }
  if (rows.empty() || data_path.empty()) return;
  std::error_code ec;
  if (std::filesystem::exists(data_path, ec) && std::filesystem::file_size(data_path, ec) > kFileCap) return;
  std::ofstream f(data_path, std::ios::app | std::ios::binary);
  for (const auto& r : rows) f << r << "\n";
}

inline void ThreadLoopBody() {
  const auto dir = JournalDir();
  ThreadState st;
  bool indexed = false;
  bool catch_up = false;  // switched off and on again: lines written meanwhile are history
  std::filesystem::path file;
  std::streamoff pos = 0;
  while (thread_run) {
    if (!selected_flag) {
      catch_up = indexed;
      for (int i = 0; i < 5 && thread_run; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    if (!indexed) {
      {
        StateLock lock;
        status_note = "indexing journals...";
      }
      // every journal once: which stars each system has, and where we are
      const auto files = JournalFiles(dir);
      for (const auto& p : files) {
        std::ifstream f(p, std::ios::binary);
        std::string line;
        while (std::getline(f, line)) HandleLine(line, false, st);
        if (p == files.back()) {
          file = p;
          f.clear();
          f.seekg(0, std::ios::end);
          pos = f.tellg() >= 0 ? static_cast<std::streamoff>(f.tellg()) : 0;
        }
      }
      indexed = true;
      size_t stars = 0;
      for (const auto& [a, s] : st.history) stars += s.stars.size();
      {
        StateLock lock;
        status_note = "journals indexed (" + std::to_string(st.history.size()) + " systems, " + std::to_string(stars) + " stars)";
      }
      Publish(st, true);
    }
    // follow the newest journal
    std::error_code ec;
    const auto files = JournalFiles(dir);
    if (!files.empty()) {
      if (files.back() != file) file = files.back(), pos = 0;  // a new session: read it from the start
      std::ifstream f(file, std::ios::binary);
      f.seekg(pos);
      std::string line;
      while (std::getline(f, line)) {
        if (f.eof()) break;  // incomplete last line: read it again next time
        HandleLine(line, !catch_up, st);
        pos = f.tellg() >= 0 ? static_cast<std::streamoff>(f.tellg()) : pos;
      }
      catch_up = false;
    }
    PollStatus(dir, st);
    Publish(st, true);
    FlushRows();
    for (int i = 0; i < 2 && thread_run; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

inline void ThreadLoop() {
  try {
    ThreadLoopBody();
  } catch (...) {
    StateLock lock;
    status_note = "journal reader stopped after an error (test mode keeps the star-draw result)";
  }
}

inline void Start(const std::filesystem::path& game_dir) {
  data_path = game_dir / "renodx-starlight-detection.jsonl";
  thread_run = true;
  thread = std::thread(ThreadLoop);
}

inline void Stop() {
  thread_run = false;
  if (thread.joinable()) thread.detach();  // never block in DllMain
}

}  // namespace elitedangerous::starlight::experiment
