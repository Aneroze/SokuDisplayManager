# Plan: the "wobbly" pixel art during camera zoom

Status: analysis only, written 2026-09-29. Nothing implemented or measured yet. Items marked **VERIFY** are
assumptions to confirm first.

## The symptom

While the camera zooms in and out, the characters' pixel art shifts in an unpleasant way, and sometimes the stage
does too - "slightly wobbly". It is not tied to a DM filter: it shows with all of them.

## Why it happens (and why DM's upscale can't fix it)

The zoom is done by the game itself, at draw time, inside its 640x480 frame: sprites and the stage are drawn at a
non-integer scale (e.g. 0.83x) into 640x480 pixels. DM only ever sees the finished 640x480 frame, so whatever the
zoom did to it is already baked in.

- **Characters:** at a non-integer scale each sprite texel covers a fraction of a screen pixel. With point
  sampling, some texel rows/columns are dropped or doubled, and *which* ones changes with every zoom step and every
  sub-pixel of movement: the pixel art crawls. The SamplerProbe (`docs/samplerprobe-*.log`,
  `WindowResizer-rendering-research.md`) showed the game switching MAG/MIN filter between POINT and LINEAR per
  draw (LINEAR-dominant, ~1:3.3), so some draws crawl and others blur differently. **Measured 2026-10-06**
  (SokuHarness `filt on` / `filt dump` / `filt detail N`; draws made inside the players' draw 0x46E0D0..0x46E2E0):
  each character is two XYZRHW quads per frame, the sprite with **MAG/MIN = POINT** (white, alpha blend) and its
  drop shadow with LINEAR (black 50%, squashed). Practice, Reimu vs Reimu: sprite scale x1.96 close, x1.47 after
  walking apart, so character sprites are point-sampled at a continuously changing non-integer magnification.
- **Stage:** the 3D stage geometry is minified as the camera zooms out. Textures without mipmaps (or with
  MIPFILTER NONE on that draw) alias and shimmer as the scale changes. **VERIFY** per-draw MIPFILTER / mip levels
  of the stage textures.
- DM's upscale then enlarges that frame faithfully. A filter choice changes how the already-wobbly pixels are
  enlarged, not the wobble. xBR can make it slightly worse (its edge detection re-decides differently every frame).

## Options

| Option | Effect on the wobble | Cost | Latency |
| --- | --- | --- | --- |
| Another upscale filter | none | - | none |
| Spatial blur / lower `Sharpness` | hides it a little | everything softer, all the time | none |
| Temporal blend (TAA-style) | reduces shimmer well | ghosting/smear on fast motion - wrong for a fighting game | none added, but motion smears |
| Force LINEAR sampling on sprite draws (sampler hook) | texels no longer drop out: motion becomes smooth instead of crawling | sprites softer whenever scaled; cuts against the crisp look | none |
| Higher internal resolution (render at 2x/3x) | fixes it at the source: zoomed sprites and the stage are resampled with 4-9x the pixels | big project (below) | none (tiny GPU cost) |

### Cheap experiment: force LINEAR on sprite draws

Hook `SetSamplerState` (or set the state before the game's sprite draws) so MAG/MINFILTER = LINEAR for textured
XYZRHW draws while the camera is zoomed (or always, as a toggle). Compare in motion: smooth-but-softer vs crisp-but-
crawling is a matter of taste. The user's own WindowResizer patch forced POINT for crispness, so this is a real
trade-off, not a free win. Optional refinement: MIPFILTER LINEAR / anisotropy for the stage where mip levels exist.

### The real fix: render at a higher internal resolution ("HD mode")

Let the game draw its frame at N x 640x480 directly (N = the output scale, or 2/3 and let DM do the rest):

- **3D stage / transformed vertices:** they go through the viewport, so a viewport of N x 640x480 already renders
  them at N x (this is exactly what the Okuu bug did by accident - `docs/bug1-okuu-research.md`).
- **Sprites (XYZRHW, `DrawPrimitiveUP`, vtable 83):** scale every vertex x/y by N (keeping the -0.5 half-pixel
  convention), in a DrawPrimitiveUP/DrawIndexedPrimitiveUP hook.
- **Everything else in 640x480 pixel space:** `Clear`/`ColorFill` rects, scissor rects, any render-to-texture
  passes the game uses for effects (**VERIFY** with the harness draw diagnostics: `diag on`, `nat on`), and screen-
  space effects that sample the frame.
- **Other mods:** ImGui overlays (InGameHostlist, ReplayHudExtras, Giuroll-UI) also draw XYZRHW; they must not be
  scaled, or must be scaled consistently (caller filtering, as the harness `PEX` trace does).
- DM's post-process then grabs an N x 640 frame instead of 640x480 (or skips the grab at the output scale).

Big but contained: the draw-diagnostic tooling from the Bug 1 work (DrawProbe, harness `diag`/`nat`/`pex`) is the
starting point.

## Suggested order

1. **Confirm the cause:** capture a zoom sequence frame by frame before DM (harness `shotbb`, or `freeze` off +
   repeated shots) and check the wobble is in the 640x480 frame; log the sampler state of the character and stage
   draws during a zoom.
2. **Try the LINEAR-sampling toggle** and judge it in motion.
3. **Scope the internal-resolution mode** only if 2 isn't good enough.

## Prototype result (2026-10-06): sharp-bilinear on character sprites

Built in SokuHarness (`src/sharpsprite.hlsl` -> `sharpsprite.h`, ps_2_0, 47 slots; `filt sharp on|off|k <v>`, in-game
F8 / F2 / F3, readout through DM's overlay API). Applied only to POINT-filtered character sprite draws (inside the
players' draw 0x46E0D0..0x46E2E0) with the plain fixed-function setup (stage 0 texture x diffuse, stage 1 off); per
draw: scale from the quad's sides, alpha-premultiplied 4-tap blend with the sampler left on POINT and ADDRESS
switched to CLAMP, all state restored after the draw. Shadows (already LINEAR) untouched.

- Forcing plain LINEAR on the sprites: rejected by the user, "blurry as all hell".
- Sharp shader: the user found **k = 2.5** best (band = 1/(k*scale) texels, i.e. 0.4 screen px) and said the game
  looks better with it than with the vanilla POINT. Next: make it a DM option; consider the other POINT-sampled
  draws (bullets, effects) too.

### Backgrounds (2026-10-06)

`filt stage` (draws inside the stage background 0x470500..0x4705C3 / foreground 0x4705D0..0x470623 draws): on the
Practice stage tested, the background is **screen-space POINT quads of 256x256 tiles** (DrawPrimitiveUP, ~20 per
frame, the game projects them itself), drawn shrunk at x0.71..0.98, plus one LINEAR 470x270 layer (x2.1..2.9). No
foreground draws there. Other stages untested (may use 3D draws or magnified tiles).

- Sharp shader on the tiles reduces the shimmer. The user's pick: **k = 1.5 for backgrounds** (characters stay 2.5),
  so the option needs two values. Separate keys in the prototype: F5 = background on/off, Shift+F2/F3 = its k.
- A highly detailed roof texture with many close horizontal lines still shimmers a little (also at very blurry
  settings): detail near the pixel grid's limit under minification, which a 2x2 filter can't remove. Accepted by
  the user. Possible later refinement: pre-filtering for shrunk draws (generated mips / a wider kernel).

### In DisplayManager (2026-10-07, experimental)

Ported as `SpriteSharpness` / `BackgroundSharpness` (commented out = off: nothing patched) plus six hotkeys, all
commented out in the shipped ini. Instead of the prototype's per-draw stack scan, the entries of the players' draw
(0x46E0D0) and the stage draws (0x470500 / 0x470570 / 0x4705D0) are detoured to set a layer flag that the
DrawPrimitiveUP hook reads (re-hooked after vtable resets like SetRenderTarget). Not the battle render's calls to them:
CharactersInForeground NOPs the game's call to the battle render (0x47A9BF) and calls these functions itself, so
call-site patches never fired. Extra check vs the prototype: no pixel shader bound (skips the stage's effect pass,
0x470240). Verified on the Copy install (Practice, CharactersInForeground + PracticeEx on): both layers applied,
zoomed-out frame clean, HUD unaffected; commented out = entries unpatched. Note: the harness's own device-hook
re-assert bypasses DM's DrawPrimitiveUP hook - test with `SOKUHARNESS_NODEVHOOKS=1`.

Whole-number scales (2026-10-07): at an exact x2 (characters at rest) or x1 (stage at rest), POINT is already even, but
sharp-bilinear depends on the sub-pixel phase - with the camera centre on a half pixel every texel edge sits on a pixel
centre and gets a 50/50 blend (every other column a mix: blurry). Any edge-smoothing filter does that at that phase; the
shader now treats a whole-number axis as point and skips draws that are whole on both axes.
