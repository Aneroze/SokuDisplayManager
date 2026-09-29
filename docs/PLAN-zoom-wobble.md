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
  draw (LINEAR-dominant, ~1:3.3), so some draws crawl and others blur differently. **VERIFY** which filter the
  character sprite draws use while zoomed.
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
