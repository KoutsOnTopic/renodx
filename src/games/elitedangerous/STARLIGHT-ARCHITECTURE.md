# Starlight: architecture and the reasoning behind it

Starlight is the part of the Elite Dangerous RenoDX addon that recolours the
light cast by stars so it matches the star casting it. Code: `starlight.hpp`
(the module), `addon.cpp` (wiring and the star-draw observers),
`renodx-starlight.ini` (per-class table and policies). The chronological
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
  lands one rounding step away (0.625 vs 0.624). Classes are ≥ 0.03 apart on
  a channel, white dwarfs ≥ 0.018 from the Y band.
- **Y/T band:** unlearned surface signatures with r 0.17–0.50 and g
  0.195–0.225 count as Y. Reason: Y and T look identical in game and the band
  is clearly off the main-sequence ramp. **L gets no band**: its constants
  lie on the same ramp as late M stars (0.835,0.165 vs M 0.761,0.195).
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

Class Table mode uses the ini colour for the class; `table` as a policy
uses it inside Star Colour mode; `measured` falls back to the class-table
colour until the first sample exists (so a Y dwarf starts purple, not blue).

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

## 6. Recolouring (`RemapLight`)

- **Chroma only, luminance kept.** Brightness is the game's.
- **Multiplicative against the unattenuated light** (`base_colour`, the
  per-channel max of the colour variants uploaded this frame and last):
  gain = target chroma at base luminance / base, applied to every copy.
  Reason: inside an atmosphere some copies already carry transmittance;
  replacing their chroma erased the reddening the vanilla light showed.
  Outside an atmosphere base == copy, so nothing changes there.
- **Copies are matched against every colour variant** seen this frame and
  last (`frame_keys`/`prev_keys`), not the latest upload. Reason: two
  variants exist per frame near planets; matching only one left the terrain
  light untinted on part of the frames (flicker).
- **Which buffers:** ≤ 4 KB always; 4 KB–1 MB when their (size, usage) kind
  is known to hold light colours (`hot_kinds`), discovered by a 1 MB search
  budget every 30 frames or the manual scan. Reason: terrain sun records live
  in ~320 KB buffers; scanning every large write would cost frame time.
- **Saturation:** `Saturate(out, slider × class)`; slider 100 = ×0.7, max 200,
  because raw star colours are more saturated than looks natural and the
  owner settled on 0.7. Setting key `StarlightChroma` (renamed so old
  percentages are not reused). Modes: Vanilla / Star Colour / Class Table
  (key `StarlightColour`; the old Saturation mode was removed as it only
  complicated A/B testing).
- **Two light slots stay independent** (`lights[0]`, `lights[1]`, copies
  matched per slot). Required: an L and a B lighting a planet's rings in their
  own colours looked natural and must keep working.

## 7. Hand-overs (`UpdateApplied`)

Each frame computes the light's goal tint (chromaticity + class saturation;
"vanilla" is the light's own chromaticity) and keeps the applied one. When
the identified star changes and the previous one was applied <
`kHandoverGapFrames` ago with the same `arrival_seq` (no jump), the tint
eases from old to new over `kRolloverSeconds` (2.5 s wall clock, smoothstep).
Otherwise it switches at once, so arrivals have no delay. Reason: in-system
hand-overs were jarring; jumps must not be.

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
  patched. First thing to read for any report.
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
- Measurement needs the star in the main view; `measured` classes use the
  class-table colour until then.
