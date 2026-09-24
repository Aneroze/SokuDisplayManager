# DisplayManager — known bugs to fix before release (recorded 2026-09-24)

Two rendering bugs, both appearing with DisplayManager active in **exclusive fullscreen**. Both are almost
certainly DM's fullscreen post-process (force native backbuffer + grab the top-left 640 + upscale centered)
interacting with other rendering. Test on the COPY install (`F:\Games\Touhou\SokuLauncher - Copy`).

> ⚠️ The `dist/DisplayManager-v1.0.0.zip` archive prepared for Quosu predates these fixes. Bug 1 (Okuu) is
> now FIXED in `src/` — rebuild/repackage before shipping. Bug 2 (PracticeEx dupe) is still open.

## Bug 1 — Okuu (Utsuho Reiuji): giant off-screen sprite intrudes — ✅ FIXED (2026-09-24)

**Symptom:** With Okuu, a huge Okuu sprite sits off-screen; moving her sometimes brings it on-screen. (What
looked like a "cloak" issue is actually her whole character draw + the 3D stage being over-scaled.)

**Root cause — the default D3D9 viewport.** th123 never calls `SetViewport`; D3D9 defaults the viewport to
the whole render target at CreateDevice/Reset. Vanilla = 640×480 backbuffer → viewport 640×480. DM forces a
**native** backbuffer (e.g. 2560×1440) → default viewport becomes native. Draws that go through the viewport
transform (the 3D stage and Okuu's character draw) are scaled by ~backbuffer/640 from the top-left corner →
the giant off-screen Okuu (and a slightly over-zoomed stage). Windowed was fine because DM keeps the 640×480
backbuffer there. Pre-transformed (XYZRHW) HUD/most sprites ignore the viewport, so only she looked wrong.

**Fix (implemented in `src/DisplayManager.cpp`):** `setGameViewport(dev)` pins the viewport to
`g_srcW × g_srcH` (640×480) after CreateDevice, after Reset, and re-pinned every Present on the render thread
(so Reset / SetRenderTarget / other mods can't undo it). Gated to `g_active`, so windowed passthrough is
untouched. **Verified** in real borderless fullscreen with the full mod set (harness confirmed `vp=640x480`
during Okuu's draw; user-confirmed on screen). Patch: `okuu-viewport-fix.patch`; details in
`bug1-okuu-research.md`.

**NOT the cause (red herrings):** the `[0x871538]`=3.0 effect-loop scale (same const windowed & fullscreen);
`clampvp`/`fakevp` (neither pins the default viewport). See the research doc.

## Bug 2 — PracticeEx menu duplicated (small, top-left)

**Symptom:** In fullscreen, PracticeEx's menu shows correctly (upscaled + centered) **and** a second,
smaller, un-upscaled copy in the top-left corner. The menu still works. Screenshot:
`F:\Games\Touhou\screenshots\practiceex-dupe-bug.png` (2560×1440: big centered menu, plus a ~250 px copy at
top-left).

**Leading hypothesis:** hook-ordering between DM's **swapchain-Present** post-process (grab top-left 640 →
ColorFill black → draw upscaled centered) and PracticeEx's overlay drawing. The small top-left copy is
PracticeEx's menu drawn at native 640 scale **after** DM's grab/upscale (so it isn't captured or centered),
while the centered copy is the menu that was in the 640 frame DM grabbed. i.e. PracticeEx draws its menu at
a stage DM doesn't cover (its own EndScene/Present hook, possibly via shady-loader/ImGui), landing in the
top-left over DM's output.

**Investigation plan:**
1. Find PracticeEx's render hook: does it draw in EndScene, device Present, the swapchain Present, or via
   shady-lua/ImGui? (`C:\Projects\SokuMods\modules\PracticeEx` — read its source.)
2. Determine DM-vs-PracticeEx present-hook order (which runs first).
3. Fix options: (a) DM grabs/upscales at a later point that includes overlays; (b) DM detects and also
   upscales/relocates the post-grab overlay region; (c) special-case: DM re-grab if content changed after
   its pass. Prefer a general fix so any overlay mod (InGameHostlist, ReplayHudExtras, etc.) composits
   correctly — those set their own 2/clientW projection and draw overlays too (see UPDATE 4/5).

## Common root cause to weigh
Both bugs stem from DM's exclusive-fullscreen model: **force a native backbuffer, let the game draw 640 in
the top-left, then grab+upscale.** Overlays/effects that draw at native scale or after DM's grab, or that
size render targets from the backbuffer, break. Consider whether a different composition point (e.g. grab
later, or hook the actual final present after all mods) fixes both at once — without losing exclusive/
Independent-Flip latency (hard requirement).

## State to resume from
- DM current: `Filter=Sharp` default (commit 405622f); clean base + core SmoothRender; probes removed from
  MAIN install. Copy install has the current DLL. Build: `build.bat` / the fxc+cl one-liner in this session.
- Full rendering research: `docs/WindowResizer-rendering-research.md` (UPDATE 1–5).
