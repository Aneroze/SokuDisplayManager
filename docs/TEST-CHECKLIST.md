# DisplayManager — in-game test checklist

Run on the Copy install (`F:\Games\Touhou\SokuLauncher - Copy`). Check the REAL window size/position on screen, not
only what the game renders (a backbuffer capture can't show window placement).

## Increment 1 — must-fix items (master: 0fa2204 .. 39dd628)

1. **Windowed start**: window is the WindowScale size, at PositionX/Y if set, 4:3 client; drag-resize stays 4:3.
2. **Fullscreen start** (quit while fullscreen, relaunch), exclusive AND borderless: comes up fullscreen; Alt+Enter
   out gives the WindowScale size at the spawn position.
3. **Exclusive round-trips**: Alt+Enter out and in 3+ times. Each return to windowed keeps the previous position (not
   re-centered on the primary), the right size, a 4:3 client (1280x960 at x2), and always-on-top if Alt+P was on.
4. **Borderless round-trips**: Alt+Enter in AND out actually works. Covers the whole monitor (no offset/oversize);
   returns with the normal frame, old position and topmost state. Check whether the mouse cursor gets hidden.
5. **Multi-monitor**: drag the window to monitor 2, then Alt+Enter in borderless (covers monitor 2, returns there) and
   in exclusive (uses the game's adapter/main monitor at its native mode, returns to monitor 2).
6. **Device loss**: Alt+Tab / lock screen while fullscreen; the game recovers without the window jumping.
7. **PracticeEx menu** (Backspace in Practice): no small un-upscaled copy in the top-left.
8. **Okuu** in fullscreen: normal size (no giant sprite).
9. **Hotkeys**: Alt+0..6, Alt+F, Alt+K/L show OSD readouts; Alt+P shows none; Alt+1..6 resize the window when windowed.
10. **Filters**: Sharp, Point, Linear, Auto at 3x and at 2.25x (e.g. FullscreenWidth/Height=1920x1080 or a 1080p
    screen); Sharpness visibly changes; no blurry block in the top-left.
11. **Persistence**: delete the `Filter` line from the ini; it behaves as Sharp.

Risks to watch: cursor hidden in borderless; other mods reading the game's present params (0x8A0F68) now see 640x480
/ Windowed=0; SokuDirectXOptimizations with use_d3d9ex=1 (fallback path, untested).

## Increment 2 — should-fix items 7–12 (branch `should-fix`: 7d5a8e0 .. 8fdef7e)

Set `Log=1` for all of these; the log lines quoted below are what to look for.

7. **Mode validation / fallback** (exclusive, `Borderless=0`):
   - Default ini: log shows `mode check: WxH@RHz ok` with the monitor's real refresh (e.g. 144, not 143) and
     `exclusive fullscreen -> native ...`; the monitor's OSD/refresh readout shows its usual rate.
   - `FullscreenRefresh=61` (a rate the monitor doesn't have): log `has no 61Hz in the mode list - using NHz`;
     fullscreen works.
   - `FullscreenWidth=1234` / `FullscreenHeight=567`: log `not in the adapter's mode list - using the current
     mode`; fullscreen comes up at native.
   - Failure chain, if you can provoke it (a CRU custom mode, a rotated panel, Wine/DXVK): the log shows
     `Reset failed (...)`, then `retry with the default refresh rate`, then if needed `retry with the game's own
     params` - the game must end up in some working fullscreen (vanilla-style 640x480 in the last case), never
     a hang/black screen/exit. Alt+Enter back to windowed must work, and the next Alt+Enter tries native again.
8. **Window scale vs fullscreen mode**: with `Mode=FitToScreen`, windowed Alt+3 resizes the window to x3; Alt+Enter
   into fullscreen is still FitToScreen (not x3). Quit: the ini has `WindowScale=x3`, `Mode=FitToScreen` and
   `FullscreenScale` unchanged. Fullscreen Alt+2 still gives FullscreenScale x2 (and sets `Mode=IntegerScaling`).
   Remove the `WindowScale` line: the window uses the `FullscreenScale` size. On a 1080p monitor Alt+4..6 give the
   largest scale that fits the work area (x2 with the taskbar; log `clamped to the work area`); a window near the
   right/bottom edge is moved back fully on-screen; `PositionX=5000` spawns on-screen.
9. **Only the game's device**: with an overlay/mod that makes its own D3D9 device (e.g. a Steam/Discord/RTSS
   overlay, OBS game capture), fullscreen still upscales, the overlay isn't upscaled/cropped, and the log has no
   extra `Reset:`/`CreateDevice:` lines for it (only `CreateDevice from another caller ... passed through`).
10. **Double-Present guard**: SokuDirectXOptimizations with `present_wait=0` (and without it: th123 itself presents
    with DONOTWAIT), vsync on, a heavy scene: watch for single-frame flashes of a zoomed top-left corner in
    fullscreen - there must be none.
11. **Viewport across SetRenderTarget**: Okuu in fullscreen with the full mod set (all overlays: ReplayHudExtras,
    InGameHostlist, PunishDisplay, LabTool, giuroll UI) - normal size in every scene, including while those overlays
    are visible; no giant sprite flicker. Sharp/Point/Linear all still render correctly (DM's own post-process
    binds the backbuffer and must not be affected).
12. **Conflicting mods**: enable WindowResizer together with DM: the log says `WindowResizer.dll is loaded -
    DisplayManager is passing everything through`; the game behaves exactly as with WindowResizer alone (no DM
    hotkeys/OSD, no DM window changes), and DM's ini is not rewritten on exit. Repeat with IntegerFullscreen and
    ExclusiveFullscreen. Disable them again: DM works normally.

Regression pass: re-run increment 1 items 3, 4, 7, 8 and 10 on this branch.

## Increment 3 — Sharp shader with direct scene calls (branch `sharp-origscene`, stacked on `should-fix`)

The StretchRect-only Sharp (branch `sharp-stretchrect`) was dropped: whole-number sharpness only (at 2x that means
exactly Linear or exactly Point). This branch keeps the fractional shader and removes the side effects instead.

Already verified automatically (2026-09-25, 2560x1440, borderless, final-backbuffer dumps from a test-only build):
log says `scene calls: direct (runtime ...\D3D9.DLL)` and `scene=direct`; output identical to increment 2's shader
at Sharpness 1.50 and 4.00 (99.9% on static title text, rest = animated clouds); Sharpness still changes the
image; borders pure black (border-only fill).

1. **PracticeEx menu** (Backspace in Practice) in exclusive fullscreen with Filter=Sharp: NO small un-upscaled copy
   in the top-left. This is the key check - the stage that used to hide the dupe is gone.
2. Log line `scene calls:` says `direct`. If it says `via vtable + stage`, another mod hooked BeginScene/EndScene
   before DM; the old stage path is then used (still correct, just more work per frame).
3. Sharpness 1.25 / 1.50 / 1.75 at 2x look different from each other and sit between Linear and Point.
