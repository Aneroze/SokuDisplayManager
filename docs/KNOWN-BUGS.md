# DisplayManager — known bugs to fix before release (recorded 2026-09-24)

Bugs 1–4 are rendering bugs that appeared with DisplayManager active in fullscreen — DM's fullscreen
post-process (force native backbuffer + grab the top-left 640 + upscale centered) interacting with other
rendering. "Bug 5+" at the end are the window/Alt+Enter/hooking issues found by two code reviews. Test on
the COPY install (`F:\Games\Touhou\SokuLauncher - Copy`).

> ✅ Bugs 1–4 are FIXED in `src/`; Bugs 1–3 shipped in `dist/DisplayManager-v1.0.1.zip` (the older
> `v1.0.0.zip` predates them). Bug 4 and the Bug 5+ fixes are newer than v1.0.1; the Bug 5+ fixes are
> built but **not yet verified in-game**.

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

**Superseded by Bug 4:** the re-clear is gone; the upscale is now composed in an offscreen stage and the
PracticeEx redraw lands in the (wiped) backbuffer instead. Old follow-up note, kept for history: the
re-clear covered only the top-left grab-source rect. If another
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
Bugs 1, 2 and 4 stemmed from DM's model (native backbuffer + game draws 640 top-left + grab/upscale): Bug 1 =
the default D3D9 **viewport** became native, scaling transformed draws (fixed by pinning the viewport to
640×480); Bug 2 = a 640 re-draw of the PracticeEx menu during our scene, and Bug 4 = the corner re-blit that
worked around it; both are now handled by composing the upscale in an offscreen stage (drawSharp binds the
backbuffer at Begin/EndScene so the redraw lands there and gets wiped). Bug 3 was independent — a half-pixel
sampling-alignment error in the Sharp shader path.

## State to resume from
- DM current: `Filter=Sharp` default (also the code default when the ini has no `Filter` line),
  `Sharpness=1.50`; Bugs 1–4 and 5–9 fixed in `src/`. `dist/DisplayManager-v1.0.1.zip` (md5 `8C67BB4D…`)
  predates Bug 4 and Bug 5+. Build: `build.bat`. Commits: `fe0f055` (Bug 1), `7a5b0d8` (Bug 2), `5ad94ec`
  (Bug 3), `3c7c3d5` (Bug 4); Bug 5+ commits listed in that section.
- **Release TODO:** repo is not pushed yet (`git push -u origin master` after creating the GitHub repo — the
  README's release link 404s until then). `dist/` is gitignored (the zip is a local artifact). Untracked and
  optional to add: `tools/SokuHarness`, `docs/SHARPNESS-NOOP-ANALYSIS.md`, `okuu-viewport*`.
- InfiniteDecks Alt+Enter fix (`../SokuMods/modules/InfiniteDecks/main.cpp`, D3DPOOL_MANAGED) is built +
  deployed to the Copy install but uncommitted and not yet user-verified.
- Full rendering research: `docs/WindowResizer-rendering-research.md` (UPDATE 1–5); Bug 1 trail:
  `docs/bug1-okuu-research.md`; Bug 3 analysis: `docs/SHARPNESS-NOOP-ANALYSIS.md`.

## Bug 5+ — window state, Alt+Enter, and hook races (code review, 2026-09-25) — ✅ FIXED, needs in-game test

Verified against th123's disassembly (`0x415220` = the toggle, `0x415100` = its Reset wrapper). Commits:
`8fce093` (Bug 5), `9fa4482` + `c2cafd7` (Bug 6), `0fa2204` (Bug 7), `a033073` (Bug 8), `f205eb1` (Bug 9).

- **Bug 5 — Alt+Enter couldn't leave borderless; DM polluted the game's present params.** The toggle only
  flips `Windowed` in the game's global struct (`0x8A0F68`, `Windowed` at `+0x20` = `0x8A0F88`) and calls
  Reset with it; device-lost recovery (`0x407D82 -> 0x415100`) and the post-toggle window code read it
  back. DM wrote `Windowed=TRUE` (borderless) and the native size into it, so the next Alt+Enter "toggled"
  to fullscreen again. **Fix:** CreateDevice/Reset apply DM's changes to a local copy; the game's struct is
  never modified (windowed copies are synced back whole, fullscreen copies only for runtime-filled
  defaults — none for th123, which passes explicit values). `0x8998B0` turned out to be only the saved
  "start fullscreen" config flag (stored from `Windowed` at exit, `0x4405AF`, part of the config block
  written by `0x4295D0`; read at `0x442EC7` to SendMessage a startup Alt+Enter), so the `0x4405BC` NOP,
  `writeFsFlag` and the windowed-size restore were removed.
- **Bug 6 — the game's own SetWindowPos undid DM's window setup.** After Reset returns, `0x415220` calls
  `SetWindowPos`: to windowed = `HWND_NOTOPMOST`, centered on the primary, size = backbuffer + fixed-frame
  metrics (non-4:3 client with DM's `WS_THICKFRAME`), `SWP_FRAMECHANGED`; to fullscreen = move the client
  to the primary's origin. **Fix:** on a real windowed<->fullscreen switch (and at first CreateDevice),
  myReset posts a private registered message; the subclassed wndProc applies the window state (scale +
  restored position + topmost, or the borderless popup) after the game's code. The pre-fullscreen window
  position and monitor are recorded (borderless covers that monitor).
- **Bug 7 — Sharp had no fallback.** drawSharp now returns success; on failure mySCPresent uses StretchRect.
- **Bug 8 — device-watch fallback broken.** `GetModuleHandle("d3d9.dll")` is the SokuModLoader proxy, so it
  never attached; and if it did, it could race the CreateDevice path (th123 passes `&0x8A0E30` as
  `ppDevice`) and hook twice, recording our own hook as the original (infinite recursion). **Fix:**
  VirtualQuery "executable image" check, watch thread stands down once CreateDevice is hooked,
  single-shot `hookDevice`/`installWndProc`, `hookSlot` no-op on an already-hooked slot. Still
  best-effort: a late attach misses Resets if another mod redirected the Reset call site (`0x4151AC`).
- **Bug 9 — docs/defaults.** README/ini/comments corrected (Alt+0..6, PersistState writes
  Mode/IntegerScaling/Filter/Sharpness, windowed window management, Sharp default); code default Filter
  is now Sharp (was Auto).

Side effect to check: in borderless the game now knows it is "fullscreen", so its toggle wrapper
(`0x408350`) applies its fullscreen cursor handling (`ShowCursor(FALSE)`, when its cursor flag is set)
like in exclusive mode, and it saves/restores the "start fullscreen" state normally.
