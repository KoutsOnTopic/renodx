Starlight __VERSION__ for RenoDX Elite Dangerous
================================================
Starlight gives starlight the colour of the star that is actually casting
it: an M dwarf lights your ship orange-red, an A star a cool white, a brown
dwarf a deep red or magenta. In multi-star systems it follows whichever star
is lighting you. Brightness is never changed, only the colour, and it
reaches ships, stations, carriers, planet surfaces, planets and atmospheres.

This addon is the full RenoDX Elite Dangerous mod (HDR) with Starlight
added. It replaces the standard renodx-elitedangerous.addon64.


REQUIREMENTS
- The live game, in its elite-dangerous-odyssey-64 folder (the one with
  EliteDangerous64.exe; on Steam: ...\Elite Dangerous\Products\
  elite-dangerous-odyssey-64).
- ReShade with FULL ADD-ON SUPPORT (reshade.me, the "with full add-on
  support" download), installed for EliteDangerous64.exe with the
  DirectX 10/11/12 API. Tested with ReShade 6.8.0. If you already use the
  RenoDX Elite Dangerous mod you have this.
- HDR output needs an HDR monitor with Windows HDR on. Output is on Auto by
  default, which picks HDR or SDR to match Windows (see SETTINGS).


INSTALL
1. Close the game.
2. If you already have renodx-elitedangerous.addon64 and want to be able to
   go back, rename yours to renodx-elitedangerous.addon64.bak.
3. Drag these files into the elite-dangerous-odyssey-64 folder and replace
   when asked (this readme is optional):
     renodx-elitedangerous.addon64
     renodx-starlight.ini
     renodx-starlight-signatures.ini
   The signatures file holds __SIGCOUNT__ signatures of brown dwarfs (and of
   stars that look like them) seen so far, so most brown dwarfs are lit
   right from the start. If you have one from an earlier Starlight test
   build, replace it: the old one can hold wrong labels.


SETTINGS  (in game: press Home to open ReShade, then the RenoDX tab; Output
and Starlight are near the bottom, above Links)
Output
  Auto                             the default on a new install: at launch,
                                   HDR if Windows HDR is on for the game's
                                   display, SDR if it is off. The panel says
                                   what it found, and warns if the game ends
                                   up on a display set the other way.
  HDR (RenoDX)                     RenoDX HDR. Its Tone Mapper defaults to
                                   RenoDX (Vanilla+); Custom and Vanilla
                                   are one click away.
  SDR                              the game's own SDR picture with Starlight
                                   added. SDR has its own Tone Mapper
                                   setting. Starlight AgX (the default) is
                                   a fixed AgX look that keeps the game's
                                   hues (matched in darktable UCS); Vanilla
                                   is the game's own look; Vanilla+ and
                                   Custom are RenoDX's own. All but Vanilla
                                   tone map the scene before the game's SDR
                                   clip, and the Color Grading sliders then
                                   adjust from there (50 = as shipped).
  Restart the game after changing it. A choice saved by an earlier version
  is kept; pick Auto to switch to it. HDR and SDR each keep their own
  Tone Mapper choice. From v0.1.3 HDR starts on Vanilla+ once, also when
  an earlier version saved Custom (that was its default).
Starlight
  Starlight Colour  Star Colour (default) tints each light with its star.
                    Vanilla turns Starlight off (use it to compare: the
                    RenoDX preset "Off" does not turn Starlight off).
  Strength          how far each light moves towards its star's colour
                    (100 = all the way).
  Saturation        chroma of the starlight. 100 is the natural look;
                    higher gets heavy quickly.
  Star Brightness   brightness of the star's disc, hotspot and edge
                    (0-400, 100 = vanilla).
  Glow Brightness   brightness of its corona glow and lens flares
                    (0-400, 100 = vanilla). Neither changes the light the
                    star casts.
  Learn classes on arrival
                    Keep on. After a hyperspace jump the arrival star's class
                    from the journal is attached to that star's colour, which
                    is how brown dwarfs get their look. Which star is lighting
                    you is always found from the renderer, never the journal;
                    the journal only supplies the class.
Leave the other buttons alone unless asked for a bug report.


KNOWN LIMITS (beta)
- Opening the System Map can keep the star's tint for up to half a second.
- The Full Spectrum System Scanner keeps the tint of the star lighting you
  while it is open, even when you zoom into a body orbiting another star.
  The System Map does not do this yet.
- A light whose star is not drawn (a far-away primary seen from a distant
  companion) keeps roughly the game's own colour. When the game switches
  your light to such a star for a moment, the tint now fades to that
  colour and back instead of snapping; the shadow still moves at once,
  as the game draws it.
- An L dwarf that is not in the signatures file is lit like an ordinary
  star. If it is a system's main star it is learned the first time you jump
  in; a companion L dwarf stays that way.
- The overlay shows most ordinary stars as "unlearned". That is expected:
  in Star Colour mode their colour does not depend on the class.


REPORTING A BUG
Turn on "Probe log", reproduce the problem and close the game. Before you
start it again (ReShade.log is replaced on every launch), send from the
game folder ReShade.log and renodx-starlight-signatures.ini, plus the
system name, a screenshot, whether you use HDR or SDR, and the "Build"
line from the About section of the RenoDX tab. Then turn Probe log off: it
stays on for every session until you do.


UNINSTALL
Delete renodx-elitedangerous.addon64, renodx-starlight.ini and
renodx-starlight-signatures.ini (and this readme if you copied it). To go
back to the standard RenoDX mod, rename your .bak back to
renodx-elitedangerous.addon64.


CREDITS AND LICENCE
Starlight: CMDR SaltyVet.
RenoDX Elite Dangerous mod: Musa Haji (mqhaji).
RenoDX: Carlos Lopez Jr. (clshortfuse) and contributors,
https://github.com/clshortfuse/renodx
ReShade: crosire, https://reshade.me (not included).
AgX: "Minimal AgX" by Missing Deadlines (Benjamin Wrensch) and three.js, MIT.
darktable UCS: Aurelien Pierre, darktable project,
https://eng.aurelienpierre.com/2022/02/color-saturation-control-for-the-21th-century/
RenoDX, its Elite Dangerous mod and Starlight, and the AgX and darktable UCS
code they include, are released under the MIT License (darktable UCS with
its addenda: credit as above, and the name "darktable UCS" in the UI):

  Copyright (c) 2025 Carlos Lopez Jr.
  Copyright (c) 2023-2026 Musa Haji
  Copyright (c) 2026 CMDR SaltyVet (Starlight)
  Copyright (c) 2024 Missing Deadlines (Benjamin Wrensch) (AgX tone mapper,
    "Minimal AgX"; values from Troy Sobotka's AgX)
  Copyright (c) 2010-2026 three.js authors (AgX tone mapper, Blender-style)
  Copyright 2022 Aurelien Pierre / darktable project (darktable UCS)

__MIT__
