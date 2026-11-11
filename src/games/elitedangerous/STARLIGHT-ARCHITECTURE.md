# Starlight: architecture and the reasoning behind it

Starlight is the part of the Elite Dangerous RenoDX addon that recolours the
light cast by stars so it matches the star casting it. Code: `starlight.hpp`
(the module), `addon.cpp` (wiring and the star-draw observers),
`renodx-starlight.ini` (per-class table and policies), `release/` (the
release zip's packing script, readme and signatures seed; section 12). The chronological
history, reverse-engineering data and open items are in
`STARLIGHT-HANDOFF.md` in the owner's working folder; this file explains how
the module is built and why each piece is the way it is.

The owner's acceptance test: look at a shadow cast by a star; the light
colour must match that star. Everything below serves that.

## 1. What the game gives us

- **Forward lighting** reads two directional lights from the `GlobalViewData`
  constant buffer (b1, 5368 bytes): `gDirectionalLightDir[2]` at byte 4656,
  `gDirectionalLightColour[2]` at 4688, plus `gAtmosphereDirectionalLightColour`
  at 416 for the sky. Light 0 is the star; light 1 is normally a fixed, dim,
  bluish fill light, not a second star (its direction never moves).
- **Deferred/clustered lighting** (ships, terrain, planets, rings) reads a
  48-byte-per-light raw buffer and a 784-byte constant buffer. Planet terrain
  also keeps per-patch sun records in ~320 KB structured buffers. The engine
  writes **bit-identical** colour floats into all of them, so copies can be
  found by exact match (`ScanBuffer`).
- **Star draws** use known pixel shaders (CRC32 table `kStarShaders`): surface
  (StarOpaqueNoise), hotspot, corona, lens flare, edge. Their b2 constants
  hold the designers' colours; `gObjectToClipMatrix` is in b0.
- About 200 GlobalViewData uploads happen per frame, one per pass, and
  passes inside an atmosphere carry the star light already multiplied by
  transmittance. See §4 and §6 for why that matters.

Layouts were derived from `Win64\EffectsBinary\*.arc` with
`ed_starlight_layout.py`; shaders were disassembled with `fxc /dumpbin`.
Nothing in the game files is modified; everything happens through ReShade
addon events at runtime.

## 2. Frame pipeline

1. **Shadows of constant buffers** (`init_resource`, `map`/`unmap`,
   `update_buffer_region`): whole copies for buffers ≤ 1 KB (b0, b2), selected
   fields for GlobalViewData. Needed because draw callbacks cannot read GPU
   memory; we must remember what the CPU wrote.
2. **Bindings per command list** (`push_descriptors`,
   `bind_render_targets_and_depth_stencil`): which b0/b1/b2 and which render
   target a star draw uses. Keyed by command list because deferred contexts
   exist.
3. **Star draw observers** (`OnStarDraw`, called by RenoDX `on_draw`, no
   shader replacement): identify which star each light is (§3) and record
   where the star is on screen for the measurement (§5).
4. **Patching at upload** (`OnGvdData`, `ScanBuffer`): every copy of a light
   colour is recoloured as it is written (§6). Nothing is patched after the
   fact, so there is no frame of lag.
5. **Present** (`EndFrame`): finalise identities, roll tints over (§7),
   finish readbacks, log.

## 3. Identifying which star casts a light

- **Method:** project `-dir` through `gLightingToClipMatrixT` (w = 0, the
  vanishing point) and the star centre through the draw's own
  `gObjectToClipMatrix`; compare homogeneous (x, y, w) by cosine, threshold
  cos(0.75°). This needs no knowledge of world or lighting space, works in
  every pass, and was verified in-game at cos = 1.000.
- **Which upload defines the light** (`latest_gvd`): the **brightest**
  GlobalViewData upload of the frame, not the last one. Reason: side passes
  upload dimmed or zeroed lights; "last wins" made the light flicker on and
  off on atmospheric worlds.
- **Persistence:** identity survives the star leaving the screen; it is
  dropped on a direction jump > 3° per frame with no match (a different star:
  jump, binary hand-over), or after `kForgetAfterFrames` of darkness.
  Reason: on planet surfaces the star is often hidden, and the light must
  not revert.
- **Expiry on arrival** (top of `EndFrame`): when `arrival_seq` changes
  (FSDJump after a Hyperspace StartJump), a light whose last accepted match
  (`LightState::last_match`) is older than half way between the StartJump
  read (`jump_start_time`) and now is forgotten. Wall-clock time, not
  frames: the frame rate can drop in the tunnel (alt-tabbed under a driver
  background frame cap), which would pull a frame-count half way back into
  the countdown. A gap under `kMinJumpSeconds` (10 s) between the two reads
  means the reader stalled, so that check is skipped.
  Reason: light 1 is often a fixed, dim, bluish fill light (dir
  -0.30,0.94,-0.19) that never goes dark or moves, so neither rule above
  ever fires. It lined up with Col 285 Sector BC-R b6-2's L primary while
  the ship turned for the jump (0.7 s before StartJump) and stayed that L
  dwarf, with the L ×2.4 boost, through 13 later systems. Why the half: in
  35 measured jumps (18.0–18.2 s, one 29.7 s) the old system's stars were
  last matched ≤ 5.0 s after StartJump (countdown), the tunnel draws no
  stars, and the arrival star matched at the FSDJump line; the half-way
  point (≥ 9 s) always falls in the tunnel. Each journal event is read up
  to ~1 s late, well inside those margins. Light 0 normally loses its old
  star by itself (dark in the tunnel); its arrival star is matched after
  the half-way point and kept. Carrier jumps do not bump `arrival_seq`, so
  they do not expire identities. With the test mode selected, the expired
  star is also dropped from its memory (`experiment::Forget`): its journal
  reader polls faster, so it may already have remembered the stale star in
  the new visit and would restore it by direction.
- **Preference between draw kinds** (`FxRank`): surface > hotspot > corona >
  flare > edge. The surface constant is the stable class signature. A
  worse-ranked kind cannot replace a better identity unless the direction
  jumped or the better source has been silent for `kIdentityHold` frames.
  Reason: a corona-only frame used to flip the identity to a signature with
  no learned class and snap the light to the corona's constant.
- **Class signature:** `"<Fx>:r/sum,g/sum"` of the b2 constant, 3 decimals.
  The journal is only a teacher: right after an `FSDJump` the arrival star's
  class is attached to light 0's signature and saved to
  `renodx-starlight-signatures.ini`. Identification never uses the journal.
- **Lookup tolerance** (`LookupClass`): exact, then nearest learned signature
  of the same kind within 0.012 on both channels. Reason: the same class
  lands one rounding step away (0.625 vs 0.624). Classes are NOT reliably
  separated: the surface constant varies per star within a class (19 learned
  M signatures), and learned pairs of different classes sit 0.009–0.018
  apart (TTS/K, B/DA, A/F, B/A, Y/TTS). White dwarfs (DA 0.158, DC 0.157) sit
  0.018 from the nearest learned Y but only 0.012 below the band's r minimum.
  A leave-one-out test on the learned set gave the right class for about
  60 % of signatures. In Star Colour mode only classes with their own policy
  (Y, T, L) change the light, so most of these mix-ups are invisible.
- **Y/T band:** unlearned surface signatures with r 0.17–0.50 and g
  0.195–0.225 count as Y. Reason: Y and T look identical in game and the band
  is clearly off the main-sequence ramp. **L gets no band**: one learned L
  lies on the extension of the K–M ramp (0.835,0.165 vs M 0.761,0.195), the
  other well off it (0.624,0.242); two points do not define a band. Ten
  more L dwarfs since (subclass 0–8) spread their surfaces from 0.562,0.233
  to 0.845,0.155, overlapping M and TTS. Their **corona** constants
  separate them from every other class except the T Tauri stars the game
  draws like L dwarfs, which are then lit as L (see `IsLDwarfCorona`, §4).
- **Class propagation:** once a light has a class, the corona/hotspot/flare
  signatures matched in the same frame are learned with it. Reason: makes a
  corona-only match resolve next time without a journal event.

## 4. Which colour a star "is" (`StarColourFor`, policies)

Three candidate sources exist and none is right for every class:

| Source | What it is | Right for | Wrong for |
|---|---|---|---|
| constant | the surface draw's `gStarArtistColour` | ordinary classes (K orange, B violet) | brown dwarfs (blue constant, magenta/red star) |
| measured | the rendered disc, read back from the frame (§5) | a disc that carries colour | hot stars (disc is white-hot) and discs with two patch colours |
| vanilla | the game's own light colour | brown dwarfs (right hue, just bland) | main-sequence stars (near white) |

Decision: **default = constant** (what the owner found right before any
measurement existed), **exceptions per class in the ini**
(`<Class>.source = constant | measured | table | vanilla`,
`<Class>.saturation = x`), with the same prefix fallback as the colour
table. Shipped: Y and T `vanilla` ×2.6, L `vanilla` ×2.4 (dialled in live by
the owner with a temporary slider), `TTS` pinned to the default because the
prefix fallback would otherwise give T Tauri stars the T policy. Unlearned
stars get the default. A heuristic that compared hues of measured vs
constant was tried and removed: explicit per-class rules are predictable.
All per-class keys share one prefix lookup (`FindByClass`).

**B stars** (`B.min_saturation = 0.75`, `ClassSaturationFor`). B designer
colours run from violet (hue ~248°) to a pale sky blue (204–227°). At slider
100 the pale ones came out at HSV saturation 0.29–0.55, while the game's own
light at them measured 0.56–0.82 (~0.75 near the star; test log, 5 Nov), so
the owner found them duller than vanilla. The violet ones were already
0.70–0.86. A lower limit raises the class multiplier only as far as the tint
needs to reach 0.75 at slider 100 (closed form: `Saturate` keeps the channel
order, so its HSV saturation is solvable for the multiplier). The hue is
unchanged, the violet ones are untouched (HD 196088, 0.70, gets ×1.25), and the
slider still scales everything proportionally. Owner's choices: a fixed limit
at the game's up-close level rather than tracking the game's light live (that
light changes with distance, and the tint would follow it); keep each star's
hue; B only, as an ini key. The multiplier is part of the goal saturation, so
hand-overs roll it over like any tint (§7). It applies in Class Table mode (dev build) too
(the placeholder table B lifts ×8.2 to the same 0.75).

**The Y/T band beats the journal label** (`PolicyClass`): a star whose
surface signature lies in the band gets the Y policy even if the journal
called it something else (except T, which keeps its own). Reason: the game
draws some cold T Tauri stars exactly like Y dwarfs (Synuefe EL-E a66-2, a
TTS at 450 K, surface 0.217,0.209), the teacher labelled it TTS, and the TTS
policy gave the blue constant. The label is still stored and shown.

**An L dwarf's corona makes a T Tauri star an L** (`PolicyClass`,
`IsLDwarfCorona`): a light labelled TTS whose star showed a corona, edge or
hotspot constant with r ≥ 0.70 and g ≤ 0.09 gets the L policy. Data: every L
dwarf corona seen lies at 0.776,0.068 … 1.000,0.000, every other star's has
g ≥ 0.12 except those T Tauri stars, T/Y dwarfs' have r ≤ 0.50. Reason:
the game draws some cool T Tauri stars like L dwarfs (Wredguia HV-H
b39-0 B, TTS at 1629 K, corona 0.892,0.041) and the TTS policy gave their
orange constant. The flag
(`LightState::l_corona`) is set from any draw kind matched to the light on a
frame whose match became the identity (not when a worse-ranked draw was kept
out: the identity's own draw is then unmatched and the corona may be another
star's), and kept while its identity holds, because the corona is not matched
every frame. The test mode's memory keeps it per remembered star.
**Its light is not its own vanilla:** the game lights that star a bland
magenta (0.416,0.255), nothing like an L dwarf, so where the L policy says
`vanilla` it takes `kLDwarfGameLight` instead, the mean of the game's light
at ten real L dwarfs (0.623,0.186). The L saturation applies on top, so it
ends up like a real L dwarf. Side effect: an L dwarf misread as TTS by the
nearest-signature match (Wredguia CZ-M b35-0 B, surface 0.710,0.248) is lit
as L too.

Class Table mode (dev build only, §6) uses the ini colour for the class; `table` as a policy
uses it inside Star Colour mode; `measured` falls back to the class-table
colour until the first sample exists (so a Y dwarf starts purple, not blue).

**S, MS and carbon stars** (`S/MS/C.source = table`, `.saturation = 2.0`,
`.match = exact`). The game gives each class an up-close light and a much
whiter far-away one, switching at roughly 3–10 ls (measured for every class in
the 2 Nov test data). For S and C (and K, T Tauri) the up-close light is one
palette value, chromaticity 0.5153,0.3507,0.1340, which the owner judged right;
it is the table colour for these classes, and ×2.0 cancels the Saturation
slider's ×0.5, so 100 gives exactly that light at every distance. Their draws
look almost like K, M and T Tauri stars (signatures 0.002–0.007 apart), so
`.match = exact` applies the policy only when the identity's own signature was
learned with the class (`ExactOnly`, `LearnedExactly` in `PolicyClass`), and
class propagation spreads such a class only from an exact match.

## 5. Measuring the rendered star (kept for `measured` policies)

- Why not the texture: the surface shader colours by `pDiffuseTexture`
  only, but hotspot/corona/edge layers draw over it (orange texture + blue
  layers = the magenta you see). No constant or texture holds the final look.
- How: on a matched surface draw, remember the star's NDC position and the
  render target; the moment that target is unbound (the star pass is over,
  nothing has reused it yet) copy a 48×48 window on the draw's own command
  list into a persistent staging texture; map it 3 frames later at present.
  Copying at present was wrong: later passes had already reused the target.
- Gates: only targets at least half the back-buffer width (reflection-probe
  faces are 256²; they kept matching while looking away and fed dark sky);
  window's brightest pixel ≥ 0.015 (sky ≈ 0.004, a brown dwarf disc ≈ 0.04,
  hot stars ≫ 1); sample luma ≥ 2 % of a slowly decaying peak.
- Estimate (`RobustColour`): ring of the last 32 samples, per-channel median
  of chromaticity, luminance-weighted mean of samples within L1 0.6 of it.
  Reason: a strut or HUD element crossing the star gave one bright grey
  sample that a running average took seconds to forget; a Y dwarf disc has
  exactly two patch colours, which a tight median flips between.
- Even so, brown dwarfs are now `vanilla` (§4) because the game already has
  their hue; measurement remains available for classes that want it.

### Star and Glow Brightness (experiment, 9 Nov; not committed yet)

Owner: brighten "the star itself (not the light it's outputting)", tunable.
Two sliders in the Starlight section, 0-400%, 100 = vanilla, HDR and SDR,
whatever the Starlight Colour mode: **Star Brightness** scales the disc
(StarOpaqueNoise, both variants), hotspot and edge draws; **Glow Brightness**
the corona (both variants) and lens flares (both variants). The eight
`kStarShaders` pixel shaders are replaced (`star/*.hlsl`, decompiled with
3Dmigoto's cmd_Decompiler) with one change each, `o0.rgb *= STAR_BRIGHTNESS`
or `GLOW_BRIGHTNESS` before the return. Six came from the old DevKit dump;
the corona and flare `_1` variants were never dumped and were taken from
`Effects*_Win64_SM50.arc` (raw deflate after a 16-byte header, as
`ed_starlight_layout.py` reads it), each matched by its CRC32. Recompiled, the
eight match the originals' instructions except for scheduling (fxc merges the
flare's two `sincos`). `StarlightObserveShader` now carries `.code`: the
generated `__ALL_CUSTOM_SHADERS` lists the same CRCs, and the first entry for
a CRC wins. At 0% the draw is skipped (the observer's `on_draw` returns
false after `OnStarDraw` has seen it, `StarDrawFactor`), because only rgb is
scaled: a draw blended by its alpha would otherwise paint black. Identification
is unaffected, since the draw was observed first.

Why the light cannot change: lights are read from GlobalViewData (and its
copies), and stars are identified from the draws' constant buffers (b0
matrix, b2 colours), never from their pixels. The one consumer of the drawn
pixels is the measurement above, which only `measured` class policies use
(none shipped). With such a policy the sliders would matter: different
Star and Glow factors shift the measured mix of disc and glow colours, and a
star dimmed far below 100% can fall under the 0.015 peak gate. More bloom
around a brighter star is expected: bloom is made from the drawn scene.

## 6. Recolouring (`RemapLight`)

- **Chroma only, luminance kept.** Brightness is the game's.
- **Multiplicative against the unattenuated light** (`base_colour`, the
  per-channel max of the colour variants uploaded this frame and last):
  gain = target chroma at base luminance / base, applied to every copy.
  Reason: inside an atmosphere some copies already carry transmittance;
  replacing their chroma erased the reddening the vanilla light showed.
  Outside an atmosphere base == copy, so nothing changes there.
- **The atmosphere's filter is carried over after saturating**
  (`FilteredChannel`, `RemapLight`). A copy that keeps one channel of the
  base and lowers another by more than 1 % is that light filtered by an
  atmosphere (the engine keeps one channel bit-identical; red in both cases
  seen: Phrio Hypa TJ-O d7-0 4 on 7 Nov, a 1 Nov landing). The kept channel
  is compared to 0.1 %, not bit-exactly, because the base lags a frame and
  the light drifts or ramps; an exact test fell back to the old formula on
  every drift step (review: 1-2 frame flicker). For such a copy of a light
  with a star and a pure base, the base is recoloured and saturated, then
  multiplied by copy / base per channel, so the filter is exactly the game's. Reason: saturating the filtered copy
  itself scaled the atmosphere's tint by the slider too: halved at
  Saturation 100 (ground red/green 2.37 vanilla, 1.62 Starlight, 2.39 now),
  exaggerated for classes above ×1 (brown dwarfs). Every other copy (outside
  atmospheres, unidentified lights, a dimmer or different light) keeps the
  old formula bit for bit. The sky light at GlobalViewData 416 keeps the old
  handling (`carry_filter = false`) until the probe log shows how it relates
  to the base.
- **The base is held over filtered-only frames.** When every variant of
  both frames is a filtered copy of the current base (the Galaxy Map on an
  atmospheric world uploads just that), the base is kept, scaled with the
  kept channel so it follows drift. Rebuilt from them, the base would be the
  filtered colour and the first frame after the map (the base lags a frame)
  would light the ship cyan-white.
- **Pure base** (`LightState::base_pure`): the per-channel max is (to 0.1 %)
  one real variant. During a hand-over it is a mix of two lights, of which
  the new light looks like a "filter"; neither the carry-over nor the hold
  applies then (review: a hold locked in the mixed tint for good), so those
  frames keep the old formula.
- **A light that jumps to another star** (`ColourKey::jumped`, `kJumpCos`,
  `jump_base`). Patching runs during the frame with the state built at the
  end of the previous one, so on the first frame of a swap `base_colour` is
  still the previous star's: the gain was wrong for one or two frames and the
  carry-over misjudged (9 Nov review of the Qiedia WD-A d1-0 flicker). An
  upload whose light direction is more than 3 degrees from last frame's is
  flagged, unless the pure base explains its colour (equal, or a filtered copy
  of it: a map or the scanner moving the same light; the Galaxy Map's
  filtered-only uploads must keep the base held). Flagged uploads and copies
  are recoloured against `jump_base`, the per-channel maximum of the frame's
  flagged colours so far (`RemapLight`'s `own_base`), with the filter carried
  over against it. When every variant of the frame is flagged, the base is
  rebuilt from this frame alone and not held. Known limit: a filtered copy
  uploaded before the frame's first unfiltered one loses its filter that frame.
- **Copies are matched against every colour variant** seen this frame and
  last (`frame_keys`/`prev_keys`), not the latest upload. Reason: two
  variants exist per frame near planets; matching only one left the terrain
  light untinted on part of the frames (flicker).
- **Which buffers:** ≤ 4 KB always; 4 KB–1 MB when their (size, usage) kind
  is known to hold light colours (`hot_kinds`), discovered by a 1 MB search
  budget every 30 frames or the manual scan. Reason: terrain sun records live
  in ~320 KB buffers; scanning every large write would cost frame time.
- **Saturation:** `Saturate(out, slider × class)` (class = `.saturation`, lifted
  by `.min_saturation`, §4); slider 100 = ×0.5 (`kNaturalChroma`), max 200
  (= ×1.0, the raw colour), because raw star colours are more saturated than
  looks natural. The owner first settled on 0.7, then on 0.5 (G and K stars
  look most accurate, far less colour clipping in SDR). Setting key
  `StarlightTintChroma`, renamed on each rescale so old percentages are not
  reused (`StarlightSaturation`, then `StarlightChroma`). Modes: Vanilla / Star Colour / Class Table
  (key `StarlightColour`; the old Saturation mode was removed as it only
  complicated A/B testing). Class Table is offered only in the dev build: its
  placeholder colours looked much like vanilla (owner, 5 Nov). A saved 2 is
  capped to Star Colour on load in the regular build, which still ships and
  reads the ini because Star Colour's class rules live there.
- **Two light slots stay independent** (`lights[0]`, `lights[1]`, copies
  matched per slot). Required: an L and a B lighting a planet's rings in their
  own colours looked natural and must keep working.

## 7. Hand-overs (`UpdateApplied`)

Each frame computes the light's goal tint (chromaticity + class saturation;
"vanilla" is the light's own chromaticity) and keeps the applied one.

**Rule: inside a system the tint never steps.** Every change eases from what
is on screen over `kRolloverSeconds` (2.5 s wall clock, smoothstep), starting
at once, so nothing is delayed:

| Event | What happens |
|---|---|
| Another star identified, previous one applied < `kHandoverGapFrames` ago or still held | roll from the applied tint |
| Same star, goal moved by more than `kGoalStep` since last frame (class or policy changed; `applied_goal`) | roll |
| Star lost | held as it is for `kHoldSeconds` (0.5 s), a running roll paused (`holding`) |
| Still lost after the hold | fades to the light's own chromaticity (`fading`), then follows it; `holding` stays set |
| Star found after a fade | rolls in from the faded value |
| The faded light moves to another unidentified light | rolls to that light's own chromaticity |

It switches at once only where the picture itself cuts: the first star, an
arrival (another `arrival_seq`, unless the tint is already this star's: the
journal is read up to a second late), a light dark for longer than
`kHandoverGapFrames` (the tunnel), and a scene cut (`LightState::cut`). A cut
is a lost frame while `map_open` (GuiFocus 6-8) or `hyperspace_jump`
(StartJump read, FSDJump not yet); it lasts `kHoldSeconds` past the last such
frame, because Status.json and the journal lag the picture. During a cut the
tint follows the light at once, as before the fade existed, and a star found
then is applied at once.

Reason for the fade (9 Nov, Qiedia WD-A d1-0): the game moved light 0 to
another star for a moment. Before, the tint was dropped in one frame when the
hold ended and put back in one frame on return (same signature, so no roll),
and a star found between 0.5 s and `kHandoverGapFrames` rolled from a tint no
longer on screen. The owner saw the ground snap between two colours.

While `holding` (held, fading or faded) `RemapLight` keeps using `applied` and
`ScanBuffer` keeps patching copies, so GlobalViewData and copies change
together. A faded tint equals the unidentified result on GlobalViewData
(`Saturate(in, slider)`); copies of a never-identified light stay raw, as
before. `lost_frame` stamps every lost frame: a hold or fade not updated last
frame is stale (Mode was off, or the test mode's view was not selected) and is
dropped rather than revived.

Reason for the hold: the game sometimes points a light between two stars for
a few frames (HIP 21776, light 0 8-32 degrees off the T dwarf towards the B,
colour mixed to match; about 3 frames in flight, up to about 0.37 s at System
Map close). No star is drawn there and the identity drops.

**Scanner hold.** Zoomed in on a body, the Full Spectrum System Scanner
lights it from that body's side: light 0 swung 70–165° away from its star
for each zoom (CSI+46-20087, 5 Nov), no star is drawn there, so the identity
dropped and the body was lit in the game's own colour. The journal thread
reads `Status.json` 4 times a second (only a read holding both `Flags` and
`GuiFocus` counts: the game rewrites the file in place). While `GuiFocus` is 9
(`scanner_open`), a known light keeps its identity and tint whatever its
direction does, and its reference direction (`l.dir`) stays where it was before
the scanner, so a swing spread over several frames is caught as well. Once
it is more than 3° off, star matches are ignored (no take-over, no class
propagation) and do not count as silence for `kIdentityHold`. On each light's first lit frame
after the scanner closes, the direction is taken as it is without a jump check: the ship
keeps flying in supercruise while scanning, so the star can be more than 3°
from the frozen direction by then, and if it is not drawn that frame the jump
would drop it. While the scanner is open the star readback is not armed and
the test mode makes no radius links, since a star drawn near a held light may
be another one. The owner's choice: no change of star while scanning, even
for a body that orbits another star.

## 8. Locking rule (crash lesson)

`StateLock` wraps `state_mutex` and records per thread that it is held;
every event callback returns early if its thread already holds it, and **no
D3D call is made while holding the lock**. Reason: ReShade attaches a
destruction notifier to every game resource; the D3D11 runtime fires it from
inside unrelated calls (a copy, a map) on the same thread, the callback
raises `destroy_resource`, and re-locking a `std::mutex` on the same thread
aborted the process. Readback work runs between short locked sections.

## 9. Diagnostics

- **Probe log** (every 120 frames): uploads, rejected uploads, star draws per
  kind, per light direction/colour/best cos/variants/identity/class, copies
  patched, `scanner open` while the scanner is open (§7), and `atmo:` (light
  0's base, the original sky light at 416 with `sky-filtered` when it is a
  filtered copy of the base, copies recoloured with their filter carried over,
  frames the base was held; §6). First thing to read for any report.
- **`starlight tint:`** (probe log on): one line whenever a light's tint
  state changes (star, tint held, tint fading, rolling), with direction, game
  colour, applied tint, base and whether a star matched. Per frame, so a swap
  of a few frames is on record; capped at `kTransitionLogCap` lines.
- **`starlight star:`** once per distinct star constant block: c0..c3, the
  game light, bound textures.
- **`starlight readback:`** per accepted/dropped sample (30 per star, then 1
  in 50): sample, luma, brightest pixel, source target, position, resulting
  chroma.
- **Scan button** (3 s): searches every buffer write up to 8 MB for the
  light's direction and colour and logs where copies live.
- Overlay: per light the game colour, unattenuated colour, identity, class,
  policy in use, measured chroma, roll-over state.

Debugging a crash: `cdb -z <dump>` from `%LOCALAPPDATA%\CrashDumps` with
`_NT_SYMBOL_PATH` set to `build.vs\Release`; the Release build has no PDB.

## 10. Known limits

- Classes are only known for learned signatures; secondary stars never
  arrived at by jump stay unlearned until labelled (overlay button) or hit by
  the Y/T band or propagation.
- Signatures come from Frontier's constants; a game update that changes
  them or the buffer layouts breaks identification. Signs: GlobalViewData
  uploads rejected, no star matches, 0 patched copies. Re-run
  `ed_starlight_layout.py` and compare with the tables in the handoff.
- A star too far away to be drawn as a body is never identified, so its
  light stays vanilla. Seen at HIP 28711: about 11 minutes at the black hole
  B, 156,000 ls from the O star A. The engine's `gLocalStarDirectionSize1..3`
  (b1 bytes 1280–1312, lighting space, read by the skybox shaders) do not
  help: the test mode found them zero in every valid upload of a 35-jump
  session (2 Nov 2026), including about 5 minutes under a dim, unnamed
  light in HIP 28711.
- Test mode: a star restored from memory by direction (`Recall`) has its
  entry's direction and frame refreshed while the light stays on it (9 Nov).
  Before, the entry aged out `kRestoreFrames` after the last star draw and a
  star out of view lost its tint about 8 s after any unmatched swap. The
  default detection has no such memory: after an unmatched swap its light
  stays unidentified (tint faded, §7) until the star is drawn again.
- The System Map has its own directional light (seen as light 0 while the
  map is open). It is correctly left unidentified.
- For the first second after a jump, light 0's class can come from a
  neighbouring signature (a TTS read as K once) because the journal is polled
  once per second and the teacher fires after the first match.
- Measurement needs the star in the main view; `measured` classes use the
  class-table colour until then.

## 11. Test-mode detection (`starlight_experiment.hpp`)

Selected with **Detection = Journal + engine (test)**; the default is **Star
draws** (sections 3–7). Built to find out which way of tying a light to its
star is most reliable (research: `STARLIGHT-ATTRIBUTION-RESEARCH.md` in the
owner's working folder). **Dev build only** (section 12): players have no use
for its ~100 MB-a-session data file, so the public build leaves it out.

- **Isolation.** Every hook in `starlight.hpp`'s frame path is behind
  `experiment::Selected()` (only `Start`/`Stop` at load and unload and the
  per-frame copy of the flag, `selected_flag`, run always); the colour
  pipeline reads
  `experiment::ActiveLight(i)`, which is `lights[i]` unless the mode is
  selected. Its journal thread idles until then. The default detection still
  runs every frame in test mode; the mode copies its result into `view` and
  overrides identity and class there, so `lights`, learned signatures and
  the signatures file are never changed by it.
- **What it adds**, best evidence first when naming a star's class:
  1. *Radius link.* Corona, edge and lens-flare draws carry
     `gStellarBodyRadius` (b1 2016), the star's true radius; matched within
     1 % to a journal Scan `Radius` of the current system (next best ≥ 2 %
     worse), it names the body. Only after a discovery scan: before it the
     list usually holds just the arrival star, so a similar-sized companion
     would "match" it with nothing to rule it out. The unit is calibrated on arrival stars
     (journal Radius / drawn radius, median).
  2. *Arrival class.* The first light-0 identity within 600 frames of a
     live hyperspace arrival gets the StartJump `StarClass` (the default's
     teacher can lose a race with its 1 s journal poll).
  3. *System list.* Only once a discovery scan listed the system's stars
     (this visit or any earlier journal): classes are narrowed to the
     system's luminous stars. Owner's rule: never in an unexplored system
     before the honk, where the journal knows only the arrival star.
  4. Otherwise the default lookup.
- **Lights without a drawn star.** A light that loses its identity gets the
  star remembered at the same direction (≤ 3°) this visit, if seen within
  600 frames or confirmed by a local star slot. A light that matches a local
  star slot (`gLocalStarDirectionSize1..3`, b1 1280–1312) but no remembered
  star gets the class of a remembered star within 15° (it moved while
  unidentified), otherwise the system's one unaccounted luminous star, if
  the list is complete; colour from the class table. Memory lookups skip the
  star the other light shows at that moment.
- **Pauses.** No new identities while a map is open (Status.json `GuiFocus`
  6–8) or during a hyperspace jump (StartJump to arrival; Status.json FSD
  flags end it if the countdown is cancelled). The map and tunnel lights
  have their own directions, so they stay unidentified.
- **Data file.** `renodx-starlight-detection.jsonl` next to the game, one
  JSON object per line: `frame` rows (each light every 30 frames: the
  default verdict, this mode's verdict and why, radius link, slot match,
  journal context, the tint applied (`exp_applied` chromaticity, 0,0 = the
  game's own light; `exp_applied_sat`, `exp_rolling`, `exp_holding`), and
  `m_*`: every class method evaluated on its own so
  methods can be compared, not only the winner), `system` rows (the
  journal's star list whenever it changes), `draw` rows (per draw kind,
  signature and radius once per visit), `journal`, `visit`, `calib` and
  `session` rows. Integers (flags, SystemAddress) are written exactly. Capped at
  256 MB. Rows are queued under the lock and written by the journal thread.
- **Locking.** The journal thread takes `StateLock` only to publish parsed
  state or swap the row queue; file reads and writes happen outside it, and
  it makes no D3D calls (section 8).

## 12. Builds

Two build folders, both configured from the renodx folder (the repo root):

- **Public build** (what players get), `build.vs`: a plain configure.
  `RENODX_STARLIGHT_TEST_MODE` is 0, so the test mode is left out: there is no
  Detection setting, `experiment::Start` starts no thread and
  `experiment::Selected()` is always false (a `StarlightDetection=1` left in
  ReShade.ini does nothing).
- **Dev build** (the owner's own testing, or a tester asked for data),
  `build.dev`: the same configure plus the define. In game the two differ
  only by the Detection setting, which only the dev build shows.

```
cmake -S . -B build.vs -G "Visual Studio 17 2022" -A x64 -T v143,host=x64 -DCMAKE_SYSTEM_VERSION=10.0.26100.0
cmake -S . -B build.dev -G "Visual Studio 17 2022" -A x64 -T v143,host=x64 -DCMAKE_SYSTEM_VERSION=10.0.26100.0 "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc /DRENODX_STARLIGHT_TEST_MODE=1"
cmake --build build.vs --config Release --target elitedangerous
cmake --build build.dev --config Release --target elitedangerous
```

**Output setting** (`OutputMode`, addon.cpp, global): 0 = HDR (RenoDX
swapchain, tone mapping, upgrades), 1 = SDR (the game's own 8-bit image), 2 =
Auto, the default. 0 and 1 predate Auto, so a saved choice keeps
its meaning. Which one runs is decided once in DllMain, because it decides
what gets hooked. Auto reads Windows' HDR switch there for the display the
game window is on (main display if no window is found): ReShade loads add-ons
inside the game's first D3D11CreateDevice, after the window exists and before
the swap chain does. Display Config API only, no DXGI, since ReShade is the
game's dxgi.dll and the loader lock is held. A switch that cannot be read
gives HDR, the output before Auto existed. If the swap chain later turns out
to be on a display whose switch reads the other way from Auto's reading
(checked whatever the setting, so switching to Auto mid-session shows it), the
panel asks the player to pick HDR or SDR by hand.

**SDR tone mappers** (experimental, 5 Nov). The owner finds vanilla SDR flat
(low midtone contrast, dull colour). SDR now replaces the same game shaders as
HDR (tone map, bloom, final copy, video: `sdr_shaders`), but with no swap
chain proxy and no resource upgrades, so the image stays the game's 8-bit one.
`shader_injection.output_sdr` (set once in DllMain) tells the shaders: the
tone mapper is the SDR session's own setting (`SDRToneMapper` since the menu
was cut, default position 1 = Starlight AgX since v0.1.3, kept apart from HDR's `HDRToneMapper` (default 1 = RenoDX Vanilla+ since v0.1.3; the old key `ToneMapType`, default Custom, is no longer read) so each output keeps its choice;
`RENODX_TONE_MAP_TYPE` in shared.h picks), the peak is game white
(`PeakRatio`), PsychoV targets BT.709, the result is clipped to 0..1, and the
final copy and video shaders skip the HDR white scaling (the video keeps the
game's gamma). SDR Vanilla restores the game's clamp after its
brightness/contrast step (`ApplyVanillaSdrClamp`; the HDR shaders dropped it,
"remove saturate"), so it should be the unmodded image; that is still to be
confirmed in game against the v0.1.2 release build, which hooked nothing in SDR.
Tone mappers 3 (RenoDRT) and 4 (ACES) are offered in SDR only, through
`renodx::tonemap::config::Apply`, with the input scaled so the game curve's
inflection (0.119 -> 0.164) sits at their mid gray and comes out at the game's
level. Peak, Game and UI Brightness are hidden in SDR.

**AgX** (SDR tone mappers 5-8, `tonemap/agx.hlsli`): Base, Punchy and Golden
from "Minimal AgX" (Missing Deadlines / Benjamin Wrensch, MIT, values from
Troy Sobotka's AgX), and a Blender-style variant (Rec.2020 primaries) from
three.js (MIT). Both notices are in the file and the copyright lines in the
release README, since the zip ships compiled copies. Licences were checked on
5 Nov: Sobotka's and Eary Chow's AgX repos have no licence and Blender's
config is GPL, so nothing was taken from them; the RoboCop and Control AgX code
in this repo models those games' shaders, so it was not used either. All four
use the 7th-order curve fit. Golden keeps the Minimal AgX post's saturation
0.8 (Sobotka's original and Filament use 1.3: `AGX_GOLDEN_SATURATION`). Each
variant gets an exposure (`AGX_EXPOSURE`, solved offline) that puts the game
curve's inflection at the game's output level; Punchy needs about 2x, since its
look darkens mid gray. The Highlights/Shadows/Contrast/Saturation sliders
apply before AgX at mid gray 0.18.

**SDR menu after testing** (owner, 5 Nov): Vanilla, AgX Punchy, Vanilla+,
Custom, in every build (AgX Punchy became Starlight AgX in v0.1.3, see
below). ACES, RenoDRT and AgX Base/Golden/Blender-style are
hidden; their code stays (a label plus a `kTypeAt` entry brings one back). The
setting key became `SDRToneMapper` because saved menu positions would
otherwise point at different tone mappers. Until v0.1.3 AgX Punchy had these
controls of its own (now fixed values, see Starlight AgX below): **AgX Gamma** (AgX's display gamma, 1.8-2.6,
2.2 as published), **sRGB Shadow Correction** (0-100%: blends the final
gamma-2.2 encode toward the sRGB encode, so a monitor that decodes with the
sRGB curve, whose linear toe lifts the darkest tones, shows gamma 2.2; the
curves meet near mid gray and differ by under 1% above it), and **Hue
Shift** (100% = AgX as published; lower moves the OkLab hue toward the game
curve's result for the same light, keeping AgX's lightness and chroma, over
the whole image). The owner found AgX turned bright oranges (the ship
hologram's outline, a carbon star) red next to the game. Measured: the game's
per-channel curve pushes bright oranges toward yellow (a star orange of hue
22 deg at twice mid-white: 46 deg through the game's curve, 34 through AgX
Punchy). A first version restored the light's own pre-tone-map hue and
weighted by luminance; that hue is redder still and saturated oranges never
reach high luminance, so it barely did anything (key `AgXHighlightHueShift`,
replaced by `AgXHueShift`). At 0 the oranges land within 1 deg of the game's
hue; greys stay neutral. **Hue Space** (`AgXHueSpace`, 6 Nov) picks the space
the blend works in: OkLab (default, Ottosson, public domain or MIT), ICtCp
(BT.2100), IPT (Ebner and Fairchild; `HueIPT` in tonemap.hlsli, the library
has none) or darktable UCS (Aurélien Pierre, MIT with addenda: the UI must say
"darktable UCS", and the author, project and URL are credited in the code and
release README). All are RenoDX's own code. The blend fades out below 2% HSV
saturation of either colour: where the game's curve clips a bright light to
pure white there is no hue to take (OkLab followed rounding noise there), and
darktable UCS divides by zero on grey and black. PsychoV37, asked for on 6 Nov,
is not public yet (upstream has up to test30; the mod's Custom is a custom
test31).

**AgX Grading** (AgX Punchy only, section "AgX Grading" until v0.1.3, now fixed
values; `ApplyAgXGrade`):
Shadows, Midtones, Highlights (brightness), a tint and hue per zone, Contrast
and Contrast Pivot; every slider's 50 is "no change", so the owner's dialed-in
look stays the default. It runs after AgX and the Hue Shift, on the gamma-2.2
encoded picture: the Hue Shift re-takes the game's hues and would overwrite a
tint applied earlier. All maths is float, so the game's 8-bit write is the
only rounding (no added banding). Brightness moves the encoded level through
degree-4 Bernstein weights (peaks at levels 0.25 / 0.5 / 0.75) as a ratio of
all channels, so hue is kept; contrast is a power curve either side of the
pivot (50 = the game's mid-grey, level 0.4396). Black and white stay fixed for
every setting and the curve stays rising at every slider extreme (checked
offline, scratch agx_grade_check.py); tints are pure chroma offsets through the
same weights, so black and white stay neutral. The general Color Grading
sliders still work, before AgX.

**Starlight AgX** (v0.1.3, owner, 9 Nov: "have it be the default look, and
hide the sliders, it would be too bloated"). The AgX controls above are no
longer settings: menu position 1 (shader type 6, formerly "AgX Punchy") is
now "Starlight AgX" with the owner's tuning fixed as `#define`s in shared.h
(`AGX_*`; the `agx_*` cbuffer fields are gone). The values are the owner's
preset put through the old slider parse functions, so the picture is the
same: Gamma 2.6, sRGB Shadow Correction 0, Hue Shift 0 in darktable UCS,
Shadows 31, Midtones 63, Highlights 67, Contrast 53, Pivot 0, no tints. The
owner's general Color Grading Contrast 46 and Saturation 54 became factors
(`STARLIGHT_AGX_CONTRAST` 0.92, `STARLIGHT_AGX_SATURATION` 1.08) on those
sliders for type 6 only, so their default 50 is the owner's picture and
people adjust from there. To retune, change the defines; to bring sliders
back, the parse maps are in the defines' comment. It is the SDR default
(`SDRToneMapper` default 1); that key is new in v0.1.3, so every SDR install
starts on it. Old `AgX*` keys in ReShade.ini are simply no longer read.
Licences: the darktable UCS addendum asks that any implementation be called
"darktable UCS" in the user interface. Starlight AgX implements it even
though no setting names a colour space any more, so the RenoDX tab shows
"Hues matched in darktable UCS." under Tone Mapping while Starlight AgX is
picked, and the Tone Mapper tooltip says so. The other addendum (author,
project and URL) is about reusing the code: dtucs.hlsl and the release README
carry it, with the copyright line (owner, 9 Nov: no wall of text in the UI).

From Git Bash, prefix the configure with `MSYS_NO_PATHCONV=1`, or the `/D`
flags are rewritten into paths. The addon's file version is the time of the
last configure, so re-run the configure before a release build.

**Release zip.** `.github/workflows/starlight-release.yml` builds the public
addon from a `v*` tag on GitHub's runners (`vs-x64` preset), packs the zip with
`release/make-zip.py` and signs a build-provenance attestation for the addon
and the zip. Its publish job then waits for the owner's approval (environment
`starlight-release`), so the zip, a run artifact meanwhile, can be tested in
game first; approved, it uploads that same zip to the release, replacing an
asset of the same name, and regenerates a "Source and verification" section
(SHA-256, commits, build log, attestation link) at the end of the release
text, keeping the text above it. A tag pushed with the workflow in place
triggers it; an older tag (v0.1.1-beta) is built by a manual run from this
branch, which checks out the tag for the source and the run's commit for the
packing files. The zip holds the addon, `renodx-starlight.ini`, the seed
`release/renodx-starlight-signatures.ini` and `release/README.txt` with the
version, the seed's entry count and the MIT text filled in; text files get
CRLF and one fixed timestamp, so they depend only on the repository. The addon
does not rebuild byte for byte (it embeds its build time and depends on the
compiler version), so a download is checked through its attestation.

**The signatures seed** (147 entries) is the owner's learned file (264
entries, 2 Nov) checked against the stars of the 2 Nov test-mode data whose
drawn radius matched a journal Scan radius exactly: 14 wrong labels corrected,
18 verified brown-dwarf entries added (stars that were lit as ordinary stars),
then every entry removed whose removal does not change any light in Star
Colour mode, except the Surface entries that can be the nearest neighbour of a
point in the Y/T band (their labels still propagate to the star's other
draws). Checked in Python ports of the lookup against every 0.001 grid point
and 700 simulated visit sequences: no star lit worse than with the full file.
