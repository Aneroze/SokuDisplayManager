# DisplayManager — known bugs to fix before release (recorded 2026-09-24)

Two rendering bugs, both appearing with DisplayManager active in **exclusive fullscreen**. Both are almost
certainly DM's fullscreen post-process (force native backbuffer + grab the top-left 640 + upscale centered)
interacting with other rendering. Test on the COPY install (`F:\Games\Touhou\SokuLauncher - Copy`).

> ✅ All three bugs below are now FIXED in `src/` and shipped in `dist/DisplayManager-v1.0.1.zip`
> (the older `v1.0.0.zip` predates the fixes — use v1.0.1).

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

## Bug 3 — Sharpness slider (Alt+K/L) had no visible effect — ✅ FIXED (2026-09-24)

**Symptom:** With `Filter=Sharp`, changing `Sharpness` (via ini or Alt+K/L) did nothing — 1.0 and 4.0 were
pixel-identical. Sharp still looked distinct from Point/Linear (an every-other-pixel seam blend), but the
*value* was inert.

**Root cause — missing D3D9 −0.5 half-pixel offset in `drawSharp`.** The sharp-bilinear quad's vertices sat
exactly on the destination-rect corners (no offset). A prior change had deliberately set the offset to 0,
believing "offset 0 matches WR" — but that apparent match came from an offline compare script that itself
sampled at pixel centers (equivalent to −0.5), so it validated the wrong thing. Without the offset, at
integer scale N every output pixel samples texel-space position exactly `k/N`, which only ever lands where
the sharp-bilinear math is a no-op: at 2× only `s=0.0` (fixed 50/50 blend) or `s=0.5` (exact texel), for any
Sharpness. Confirmed by a second model's analysis (`docs/SHARPNESS-NOOP-ANALYSIS.md`, with a Python sim: max
|1.0−4.0| diff = 0.000 at x2 with offset 0) and `docs/WindowResizer-rendering-research.md` L166-173, which
already said the quad must use the −0.5 offset.

**Fix (implemented in `src/DisplayManager.cpp`, `drawSharp`):**
`float L = dstRect->left - 0.5f, T = dstRect->top - 0.5f, R = dstRect->right - 0.5f, B = dstRect->bottom - 0.5f;`
so output pixel centers sample `(k+0.5)/N`. Sharpness now varies correctly (1.0 = aligned bilinear/smooth →
4.0 ≈ point). Verified live by the user. Default `Sharpness` retuned under the corrected alignment: **1.50**.

## Bug 4 — Sharp filter blurry in a top-left block at non-integer scales — ✅ FIXED (2026-09-25)

**Reported by Quosu** (1920×1080 monitor, FitToScreen = 2.25×): with `Filter=Sharp`, a rectangle at the
top-left of the game image looked blurry. Never seen at 2560×1440 because FitToScreen there is an exact 3×.

**Root cause — the large-scale PracticeEx dupe workaround (1d01ea7).** After the upscale, `mySCPresent`
re-blitted the part of the image overlapping the 640×480 grab region (screen `[dst.left..640]×[dst.top..480]`)
from the clean capture with `StretchRect` — POINT at integer scales but **LINEAR otherwise**, never the Sharp
shader. At 2.25× that is a 400×480 plain-bilinear block (≈178×213 game px); its integer-truncated source
rect also shifted it ~1 px. At 3× POINT is close enough to Sharp that nobody noticed.

**Fix:** compose the upscale (any filter) into an offscreen backbuffer-sized render target (`g_stageSurf`),
then ColorFill the backbuffer and copy the stage over 1:1 — every output pixel comes from the one upscale
pass. `drawSharp` binds the *backbuffer* at BeginScene/EndScene and the stage only for its draw, so the
PracticeEx redraw that our scene triggers lands in the backbuffer (wiped by the ColorFill) instead of on the
frame. The corner re-blit and the border re-clear are gone. Verified with a 1920×1080 forced backbuffer
(borderless + `FullscreenWidth/Height`), via a test-only build that dumps the final backbuffer right before
the real Present (the harness `shotbb` fires *before* DM's hook, so it can't see the final top-left):
old build = "Vs Network" (in the corner) visibly softer than "Practice" below y=480; new build = identical
crispness, and the PracticeEx menu (Backspace) shows no top-left dupe.

## Common thread (resolved)
Bugs 1–2 stemmed from DM's model (native backbuffer + game draws 640 top-left + grab/upscale): Bug 1 = the
default D3D9 **viewport** became native, scaling transformed draws (fixed by pinning the viewport to 640×480);
Bug 2 = a post-clear 640 re-draw of the menu in the grab-source region (fixed by re-clearing that region).
Bug 3 was independent — a half-pixel sampling-alignment error in the Sharp shader path.

## State to resume from
- DM current: `Filter=Sharp` default, `Sharpness=1.50`; Bugs 1–3 fixed. Both the `dist/DisplayManager-v1.0.1.zip`
  package and the Copy install have the current DLL (md5 `8C67BB4D…`). Build: `build.bat`. Commits: `fe0f055`
  (Bug 1), `7a5b0d8` (Bug 2), `5ad94ec` (Bug 3).
- **Release TODO:** repo is not pushed yet (`git push -u origin master` after creating the GitHub repo — the
  README's release link 404s until then). `dist/` is gitignored (the zip is a local artifact). Untracked and
  optional to add: `tools/SokuHarness`, `docs/SHARPNESS-NOOP-ANALYSIS.md`, `okuu-viewport*`.
- InfiniteDecks Alt+Enter fix (`../SokuMods/modules/InfiniteDecks/main.cpp`, D3DPOOL_MANAGED) is built +
  deployed to the Copy install but uncommitted and not yet user-verified.
- Full rendering research: `docs/WindowResizer-rendering-research.md` (UPDATE 1–5); Bug 1 trail:
  `docs/bug1-okuu-research.md`; Bug 3 analysis: `docs/SHARPNESS-NOOP-ANALYSIS.md`.
