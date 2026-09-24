# Sharp filter: why Sharpness 1.0 vs 4.0 looks identical

> ✅ RESOLVED 2026-09-24. The proposed fix was correct and is now applied (commit `5ad94ec`): `drawSharp`
> uses the −0.5 offset on all four quad corners; the misleading comment was rewritten; the debug shader was
> removed and the real shader restored. Verified live (1.0 vs 4.0 now clearly differ). Default `Sharpness`
> retuned to **1.50** under the corrected alignment. Shipped in `dist/DisplayManager-v1.0.1.zip`. Original
> analysis below, kept for the record.

Handoff for the agent working on the Copy install. Analysis only. No code changed and no game launched.

## Summary

At `IntegerScaling=x2`, `Filter=Sharp` output is **mathematically independent of Sharpness**.
This is not a regression from the recent commits. The quad in `drawSharp` has had no −0.5 half-pixel
offset since Sharp was introduced (`405622f`). It became visible because the Copy is now at x2
(1280×960). The earlier tuning (~1.5) was done at FitToScreen on 1440p = x3, where Sharpness only partly works.

## Evidence

1. **The constant reaches the shader.** The Copy's `modules/DisplayManager/DisplayManager.log` shows the debug
   line `drawSharp: c0.z sharpness=2.000 … 4.000` stepping correctly, each followed by
   `output -> 1280x960 centered at (640,240)`. So the hotkeys, ini loading and `SetPixelShaderConstantF` all work,
   and the scale is exactly 2×.
2. **The shader and quad are unchanged since `405622f`.** The commits after `7a5b0d8` (`d46a196`, `1f26835`, `318dae3`,
   `1d01ea7`) don't touch the upscale math.
3. **The math.** `drawSharp` (`src/DisplayManager.cpp` ~L384-389) places the XYZRHW quad exactly on `dstRect`
   (L/T/R/B, UV 0..1). D3D9 pixel centres are at integer coordinates, so output pixel `k` samples texel-space
   position `k/N`. At N=2 that is only ever `s = frac = 0.0` or `0.5`:
   - `s = 0.5` → `cd = 0` → `f = 0.5`: that texel exactly, for any sharpness.
   - `s = 0.0` → `cd = −0.5` → clamped to `−region` → `f = (−0.5/sharp)·sharp + 0.5 = 0`: always a 50/50
     blend of two texels, for any sharpness.

   Sharpness only affects samples strictly between the texel centre and edge, and at 2× with offset 0 there are none.
   That's why Sharp looks different from Point and Linear (every other pixel is a seam blend) but never
   changes with Sharpness.

## Simulation

The script is
`C:\Users\Andrei\AppData\Local\Temp\claude\C--Projects-SokuMods\434f5345-bf45-47e0-b387-5f625c51b0fa\scratchpad\sharpsim.py`. It models D3D9 rasterization, the HLSL, and a LINEAR
sampler with texel centres at `(i+0.5)/W` and CLAMP. The input is alternating 0/1 texels. The metric is the
max per-pixel |Sharpness 1 − Sharpness 4|:

| Scale | Current (offset 0) | With −0.5 offset |
|---|---|---|
| x2 | **0.000**: identical output | 0.250: bilinear at 1.0, pure nearest-pixel at ≥2.0 |
| x3 | 0.167, and one pixel in three is always a 50/50 blend | 0.333 |
| 2.25 (1080p Fit) | 0.333 | 0.333 |

Even at x3, pure nearest-pixel (point) was never reachable with offset 0.

## Proposed fix (one line, in `drawSharp`)

```cpp
float L = dstRect->left - 0.5f, T = dstRect->top - 0.5f, R = dstRect->right - 0.5f, B = dstRect->bottom - 0.5f;
```

With this offset, output pixel centres sample at `(k+0.5)/N`, so Sharpness 1.0 is correctly aligned bilinear and higher values
move toward point. `docs/WindowResizer-rendering-research.md` (L166-173) already says the quad should
use the −0.5 offset and that omitting it causes 50/50 blending. The code comment above `drawSharp`
says the opposite ("verified against WR's output that offset 0 matches"). That match most likely came from
the offline comparison script, which samples at pixel centres like the −0.5 offset does, so switching the code to 0
put the half-pixel error back in. Fix the comment too.

**After the fix:** the default Sharpness (1.75) and the user's ~1.5 preference were tuned under the broken
alignment, so re-tune them. A/B against WindowResizer at x2 and x3.

## Secondary issues

1. **Top-left re-blit at large scales.** In `mySCPresent` (~L523), when `dstRect` overlaps the 640×480 grab region
   (x3 or FitToScreen on 1440p), the overlap corner is re-drawn with a Point/Linear StretchRect, not the shader.
   At x3 that's a 320×480 strip where Sharpness has no effect and the look can differ from the rest of the frame.
   It's a no-op at x2 on 1440p (`dstRect.left = 640`). Keep this in mind when comparing at x3.
2. **The debug shader is in the working tree.** `src/sharpbilinear.h` is currently the brightness-debug build
   (`shader/_debug_sharp.hlsl`), and the Copy's DLL (21:28) was probably built from it. It dims the output rather than
   sharpening, so restore `src/sharpbilinear.h` (`git checkout`) and rebuild before any visual A/B, and don't commit
   it. `shader/sharpbilinear.h` also has an uncommitted one-byte change (flags byte 1→129) that should be checked.
