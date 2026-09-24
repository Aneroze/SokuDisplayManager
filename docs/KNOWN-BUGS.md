# DisplayManager — known bugs to fix before release (recorded 2026-09-24)

Two rendering bugs, both appearing with DisplayManager active in **exclusive fullscreen**. Both are almost
certainly DM's fullscreen post-process (force native backbuffer + grab the top-left 640 + upscale centered)
interacting with other rendering. Test on the COPY install (`F:\Games\Touhou\SokuLauncher - Copy`).

> ✅ Both bugs are now FIXED in `src/`. The `dist/DisplayManager-v1.0.0.zip` archive prepared for Quosu
> predates these fixes — rebuild/repackage from `build/DisplayManager.dll` before shipping.

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

## Bug 2 — PracticeEx menu duplicated (small, top-left) — ✅ FIXED (2026-09-24)

**Symptom:** In fullscreen, PracticeEx's menu shows correctly (upscaled + centered) **and** a second,
smaller, un-upscaled copy in the top-left corner. The menu still works. Screenshot:
`F:\Games\Touhou\screenshots\practiceex-dupe-bug.png` (2560×1440: big centered menu, plus a ~250 px copy at
top-left).

**Root cause (established by tracing, not the original hypothesis):** the small top-left copy is the game's
own 640×480 menu render **re-drawn into the top-left after DM's grab+ColorFill+upscale**. Proof: forcing
DM's ColorFill to magenta made the top-left dupe render *over* the magenta (semi-transparent), so it is
composited AFTER DM clears the buffer — and it survives into DM's own present output (not a display-layer or
separate-window artifact). PracticeEx.dll imports **no** graphics APIs (only kernel32/psapi/shlwapi), so it
draws nothing itself; it invokes th123's own render functions, and the extra 640 render lands over DM's
composited frame during our upscale. (Ruled out along the way: separate window, device-level Present, GDI,
and — via `0x8A0F68/6C` poke — a backbuffer-size read.)

**Fix (implemented in `src/DisplayManager.cpp`, `mySCPresent`):** after the grab/ColorFill/upscale, re-clear
the grab-source region `[0,0,g_srcW × g_srcH]` to `g_bgColor` (one extra `dev->ColorFill`). That region is
border area in DM's centered output, so clearing it removes the dupe; in normal gameplay nothing redraws
there, so it is a harmless no-op. Verified in borderless fullscreen with the full mod set (harness `clearsrc`
test first, then the DM-built fix; user-confirmed the menu now renders with no dupe and the centered menu
correct). Diagnosed with `tools/SokuHarness` (`pex`/`dupe`/`capafterdm`/`cfcolor`/`clearsrc` commands).

**Note / possible follow-up:** the re-clear currently covers only the top-left grab-source rect. If another
overlay mod (InGameHostlist, ReplayHudExtras, PunishDisplay, …) is ever seen drawing an un-upscaled dupe
*outside* that rect, generalize the re-clear to all border regions (everything outside the centered output).

## Both bugs — common thread (resolved)
Both stemmed from DM's model (native backbuffer + game draws 640 top-left + grab/upscale). Bug 1 = the
default D3D9 **viewport** became native, scaling transformed draws (fixed by pinning the viewport to 640×480).
Bug 2 = a post-clear 640 re-draw of the menu in the grab-source region (fixed by re-clearing that region).

## State to resume from
- DM current: `Filter=Sharp` default; Bug 1 (viewport pin) + Bug 2 (grab-source re-clear) fixed. Copy
  install has the current DLL. Build: `build.bat`.
- Full rendering research: `docs/WindowResizer-rendering-research.md` (UPDATE 1–5); Bug 1 trail:
  `docs/bug1-okuu-research.md`.
