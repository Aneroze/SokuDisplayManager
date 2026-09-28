# Plan: MSAA and a pixel-art filter (DM 1.1.0)

Status: planned 2026-09-28, nothing implemented. Soku was busy (another agent), so this is from code reading
only. Items marked **VERIFY** must be checked in-game before building on them.

## 1. MSAA (`MultiSample=` in `[Display]`)

### What it can do
MSAA only smooths polygon edges. In th123 that means the 3D stage geometry and solid-edged effects. Sprite
outlines come from texture alpha, so MSAA doesn't touch them. Expect a small, stage-only difference; the point is
to see it.

### Design: redirect the game into its own multisampled 640x480 target (backbuffer stays non-MSAA)
Multisampling the backbuffer itself (`pp->MultiSampleType`) is the textbook way, but it breaks DM's own pipeline
and every overlay. ColorFill / StretchRect into a multisampled surface are restricted, so `fillBorders`, the
Point/Linear upscale, the stage copy, the OSD and SideNotes' `StretchRect` would all need rework or fail. It would
also multisample a native-size (e.g. 2560x1440) surface for a 640x480 game. Instead:

- After CreateDevice/Reset (fullscreen, `g_active`, `MultiSample > 0`), create:
  - `g_msRT`: `CreateRenderTarget(640, 480, g_bbFormat, type, quality, FALSE)`
  - `g_msDS`: `CreateDepthStencilSurface(640, 480, <game's AutoDepthStencilFormat>, type, quality, TRUE)`, only
    if the game uses a depth buffer (**VERIFY** `EnableAutoDepthStencil` / format; log the game's pp).
- Bind them (`SetRenderTarget(0, g_msRT)`, `SetDepthStencilSurface(g_msDS)`) after creation and again after every
  Present. th123 never calls SetRenderTarget (KNOWN-BUGS #11), so it keeps drawing into ours. Its Clear, ImGui
  overlays (ReplayHudExtras, InGameHostlist) and anything that uses the *current* target follow along.
- In `mySCPresent`: the grab becomes `StretchRect(g_msRT → g_captureSurf)`, a 1:1 multisample *resolve* (allowed:
  MS source, non-MS destination, no stretch). Everything after it (borders, upscale, overlays, OSD) is unchanged
  and still targets the normal backbuffer. Rebind `g_msRT`/`g_msDS` after `oSCPresent`.
- Bonus: a 640x480 render target gets a 640x480 default viewport, so the Okuu viewport pin (Bug 1) is naturally
  right for draws into it. Keep `setGameViewport` anyway.
- `mySetRenderTarget` (the viewport re-pin) must treat `g_msRT` as the game's target, the same as `g_bbSurf` today.
- Reset: release `g_msRT`/`g_msDS` with the other default-pool resources in `releaseCapture`.
- Type/quality: `CheckDeviceMultiSampleType(adapter, HAL, g_bbFormat, windowed, type, &q)`, also for the depth
  format. Fall back to the highest supported count ≤ the request; log it. `MultiSample=0` (default) = today's path,
  byte for byte.
- Fullscreen only (exclusive and borderless). Windowed stays untouched passthrough, as DM does now.
- `D3DRS_MULTISAMPLEANTIALIAS` defaults to TRUE. **VERIFY** the game doesn't turn it off; if it does, force it
  in the SetRenderState path or at rebind.

### Risks
- A mod that draws via `GetBackBuffer(0)` + its own `SetRenderTarget` would land in the backbuffer and be hidden
  by the grab. Only IntegerFullscreen (retired, conflicts with DM anyway) does this among the mods in SokuMods;
  closed-source ones (giuroll, PracticeEx, ...) → **VERIFY** in-game with the user's full mod set.
- A mod calling `GetDepthStencilSurface` / `SetDepthStencilSurface` with its own non-MS target while ours is bound
  → mismatched sizes/multisample → its draws fail. Same verification.
- DXVK and SokuDirectXOptimizations (`use_d3d9ex`): test both (the user runs DXVK in another install).
- Cost: 4x MSAA at 640x480 is trivial on any GPU; the resolve is one 640x480 blit.

### Test
Copy install, Borderless=1, harness to a stage with 3D geometry, pause (nav `p`), DM test build that dumps its
final backbuffer. Compare MultiSample=0/4/8 crops of the same stage region (a restart is needed per value, so
compare static background areas). Then check with the full mod set: PracticeEx menu, ReplayHudExtras,
InGameHostlist, riv++, SideNotes, Alt+Enter both ways, Okuu.

## 2. Pixel-art filter (`Filter=xBR`)

### Choice
**xBR-lv2** (Hyllian): single pass, works at any output scale (DM also does FitToScreen 2.25x, custom sizes),
detects edges in a 5x5-ish neighbourhood and blends diagonals/curves while flat areas stay crisp. It's the most
common pixel-art upscaler in emulators, and the license is compatible (MIT). Alternatives considered:
- ScaleFX: better shapes but 5 passes (needs intermediate targets) → maybe later if xBR looks promising.
- MMPX / Scale2x / EPX: fixed 2x only → would need a second scaling step at non-integer scales.
- HQx: needs a lookup texture; older look.

### Honest caveat
th123's 640 frame isn't pure pixel art. SamplerProbe showed most draws use LINEAR filtering, and camera zoom plus
the 3D stages already produce soft, non-grid pixels. xBR works best on hard-edged pixel art, so it may do little
on zoomed or filtered content and may add artifacts. That's the experiment.

### Implementation
- `shader/xbr.hlsl` → `shader/xbr.h` (bytecode, compiled with fxc like sharpbilinear). xBR-lv2 needs ~20 texture
  reads and well over 64 ALU ops → beyond ps_2_0. Use **ps_3_0 + a trivial vs_3_0** (ps_3_0 can't be paired with
  fixed-function/pre-transformed vertices). The vertex shader takes the same quad in pixel coordinates and a
  constant for the viewport → clip-space transform, keeping the -0.5 half-pixel rule from
  SHARPNESS-NOOP-ANALYSIS.md. Fallback: if `CreatePixelShader`/`CreateVertexShader` fail, use Sharp and log it.
- Generalize `drawSharp` into `drawShaderQuad(ps, vs, constants)`; Sharp and xBR share the state block, scene
  handling (`g_sceneDirect` / stage path) and the StretchRect fallback.
- Sampler: POINT for xBR (it does its own blending).
- Cycle: Alt+F goes Point → Linear → Sharp → xBR; OSD shows `XBR` (add X/B glyphs to the OSD font; B is missing).
  Persisted like the other filters.
- Optional knob, only if needed after seeing it: xBR's edge-strength / corner-type constant as `XbrStrength`.

### Result (2026-09-28): implemented on branch `feat/xbr`, working
`shader/xbr.hlsl` → `src/xbr.h` (fxc ps_2_a 188 slots, + ps_2_b fallback; ps_2_0 too small; ps_2_x needs no vertex
shader). `drawSharp` generalized into `drawShaderQuad`; `Filter=xBR`, Alt+F cycles Point → Linear → Sharp → xBR,
OSD "XBR" (B glyph added). Verified on the Copy install (x2, 2560x1440): shader created (S_OK), frames captured
with a test-only runtime filter switch. Output looks like proper xBR (rounded sprite contours, clean HUD text);
offline numpy reference `xbr_ref.py` (scratch) matched the synthetic pixel-art test. Pending: the user's own look
in-game (Alt+F), then merge + release decision (with or without MSAA).

### Test
Same dump method: one paused frame per filter (Point, Sharp, xBR) at x2 and at FitToScreen 2.25x; crop character
outlines, text/HUD, a zoomed-out moment and a 3D stage. The user judges the look.

## Order and release
1. MSAA (smaller, independent) → verify → commit.
2. xBR → verify → commit.
3. Bump to **1.1.0** (new features, both off by default: `MultiSample=0`, `Filter` stays `Sharp`). Document both
   in README/ini/CHANGELOG. The ini isn't auto-extended (user decision): the new keys are documented, and missing
   keys use the defaults.
