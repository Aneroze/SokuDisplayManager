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

## PersistPosition (unreleased)

1. Windowed: move the window, close the game (X button, not a kill). PositionX/Y in the ini = the window's top-left;
   next launch opens there. **Verified 2026-10-06** (automated: SetWindowPos + WM_CLOSE).
2. Minimize, then close: the ini gets the last position before minimizing. **Verified 2026-10-06.**
3. Exit from exclusive fullscreen and from borderless: the ini gets the pre-fullscreen window position, not the
   monitor origin. **Verified 2026-10-07** (both modes: windowed at (500,250), the game's own Alt+Enter, WM_CLOSE while
   fullscreen -> (500,250) saved; the log confirms real exclusive 2560x1440 @144Hz / borderless).
4. Window on a monitor left of / above the main one (negative coordinates): saved and restored there. **Verified
   2026-10-07** (left monitor at x -2304..0, closed at (-2000,450) -> relaunched at (-2000,450)). That monitor's
   scale differs from the main one's, so Windows scales th123 (system-DPI aware) there: the ini holds th123's own
   coordinates ((-1667,375) here), not screen pixels - correct, as DM reads them back the same way. A position that
   leaves the window partly off-screen comes back moved on-screen.
5. `PersistPosition=0`: PositionX/Y are never written. `PersistState=0` alone: the position is still saved.
   **Verified 2026-10-07.**
6. Ini upgrade (IniVersion): a 1.1.2 ini on first launch is rewritten so that `diff` against the shipped ini shows only
   the user's values: `-1` positions blanked (window not moved), set positions kept, IntegerScaling -> FullscreenScale,
   missing WindowScale = the FullscreenScale value, commented-out / blank hotkeys stay disabled, an enabled dev hotkey
   is active in its template spot, hand-added keys (MultiSample, ToggleMSAA) at the end of their section, unknown
   sections at the end. A current ini isn't rewritten on load; one stamped with a newer version is left alone.
   **Verified 2026-10-06**, and again 2026-10-07 with the sharp-sprite lines in the template (they arrive commented out).
7. Literal -1: `PositionX=-1`, `PositionY=-1` in a current ini moves the window to (-1,-1) (clamped on-screen).
   **Verified 2026-10-07** (spawned at (-1,0)).

## StartInLatinInput (unreleased)

Needs a CJK IME: add Chinese (Simplified) + Microsoft Pinyin (Windows PowerShell 5.1: `Set-WinUserLanguageList`),
back up `HKCU\Control Panel\International\User Profile` + `HKCU\Keyboard Layout` first and restore after. To start
the game on Pinyin, switch a window to it (`WM_INPUTLANGCHANGEREQUEST` 0x08040804): with the default shared input
method the next game window starts with it. Read the game window's state from outside: `GetKeyboardLayout(thread)`,
and `WM_IME_CONTROL` IMC_GETOPENSTATUS / IMC_GETCONVERSIONMODE on `ImmGetDefaultIMEWnd(hwnd)` (bit 0 = native).

Composition check (2026-10-06): a temporary build logging the window's key / IME messages - composing = keys
arrive as VK_PROCESSKEY (0xE5) plus WM_IME_STARTCOMPOSITION; the box itself is a th123-owned CiceroUIWndFrame in the
screen's top-right corner (no caret), visible while composing. Typed `nihen` (unbound keys). All **verified
2026-10-06** with Microsoft's IMEs (option on unless noted):

| Setup | At start | Result |
| --- | --- | --- |
| en + Pinyin, option off | Chinese mode | composes, box shown (the problem) |
| en + Pinyin | Chinese mode | -> US layout, nothing composed |
| Pinyin only | Chinese mode | IME off, nothing composed; Shift -> Chinese again |
| Pinyin + Japanese (no Latin) | Pinyin, Chinese mode | IME off, nothing composed |
| en + Japanese | off (MS-IME default) | left alone; IME forced on at start -> US layout |
| Japanese only | off | left alone. Forcing it on (hiragana) doesn't stick in th123's window, before or after activation: it is off again within ~1 s, so the Japanese turn-off branch is unexercised (same call as the verified Pinyin one) |
| en + Korean | English mode (default) | left alone; Hangul at start -> US layout |
| Korean only, Hangul at start | Hangul | English mode, nothing composed; Han/Eng -> Hangul again |
| en only | - | left alone |

Lessons: clearing IME_CMODE_NATIVE doesn't stop Pinyin (reports it, keeps composing), turning it off does; turning
the Korean IME off doesn't stop Hangul, clearing the native bit does. The IME reports "off" for ~1 s after the
window appears, hence the 5 s wait.

5. Hands-on (user, own binds, windowed): starts on Pinyin, switched to US on first focus, no candidate box while
   playing. Win+Space doesn't switch back in-game unless `AllowWinKey=1` (the game's DISCL_NOWINKEY); after Alt+Tab
   it does; with `AllowWinKey=1` Win+Space switches in-game. **Verified 2026-10-06.**
6. Not tested: third-party IMEs (Sogou, Google Japanese Input - which has an initial-mode setting), exclusive fullscreen,
   a real CJK player. Windows 10's Microsoft Japanese IME has no "start in hiragana" setting (only Windows 11's IME has
   "Default input mode", reported unreliable), so a fresh window always starts with it off.

## Sharp sprites (unreleased, experimental)

1. All lines commented out (shipped ini): the four game-function entries keep their original bytes, no
   DrawPrimitiveUP hook, log shows `spriteSharpness=0.00 backgroundSharpness=0.00`. **Verified 2026-10-07.**
2. `SpriteSharpness=2.5`, `BackgroundSharpness=1.5`: log `first stage draw` / `first character draw`; zoomed-out frames
   clean, HUD untouched. **Verified 2026-10-07** (Practice, CharactersInForeground + PracticeEx on - CIF calls the draw
   functions itself, which is why DM detours their entries).
3. Stages: shader-off vs shader-on runs (SokuHarness navat/rec, fixed seed) on stage ids 0, 1, 2, 3, 4, 5, 10, 11:
   strongly changed pixels (|d| > 96) <= 0.12% per frame, no tile seams in the difference maps. **Verified 2026-10-07.**
4. Exclusive fullscreen: the game's 640x480 frames are pixel-identical to the windowed run (231 frames).
   **Verified 2026-10-07.**
5. Replay playback (ReplayDnD): both layers applied, no hang, clean exit. **Verified 2026-10-07.**
6. Hotkeys (`SharpSprites` / `SpriteSharpnessDown` / `Up`, `SharpBackground` / `BackgroundSharpnessDown` / `Up`):
   uncomment, press in a match: the on-screen readout (`SPRITES 2.50`, `BG OFF`, ...) and the sprites change; a toggle
   with its `[Display]` value commented out turns on at 2.5 / 1.5. Needs real key presses (DM's WH_KEYBOARD hook doesn't
   see posted keys). **Verified 2026-10-07 by the user.**
8. Whole-number scale with the camera on a half pixel (found by the user: Practice, both characters close = resting x2
   zoom, P1 x=643 / P2 x=800 -> camera centre 721.5): every texel edge lands on a pixel centre, sharp-bilinear blended
   them 50/50 and every other column was a mix (horizontally blurry, both characters). Fixed: an axis whose scale is a
   whole number gets point behaviour, a draw whole on both axes is left to the game's own POINT draw. Same state after
   the fix: clean column pairs; the shimmer run is identical except one frame where the stage (x1) was on a half pixel -
   now vanilla, before blurred. **Verified 2026-10-07.**
7. Testing with SokuHarness: set `SOKUHARNESS_NODEVHOOKS=1`, or the harness's DrawPrimitiveUP re-hook bypasses DM's.
9. Rest strength (`SpriteRestStrength` / `BackgroundRestStrength`, the mix at whole-number scales; the shader mixes the
   blend weights per axis): 0 for both = frame-identical to the item-8 fix (231/231), 1 for both = frame-identical to
   the original full filter (231/231). Tuning hotkeys Alt+R / T, Alt+Z / X tried live by the user. Sprite step keys
   moved from N / M to H / J (Alt+M = ToggleMSAA in the user's ini). **Verified 2026-10-07.**
10. Tiled quads (bug report: Dust Storm with BackgroundSharpness on - the scrolling dust at the bottom turned into long
    yellow streaks after a while): the dust is a 512x128 texture tiled through WRAP addressing (u 0.01..1.01 and
    scrolling), and the shader forced CLAMP, so everything past the texture's edge repeated the edge texels. Fixed:
    CLAMP only for quads whose texture coordinates stay inside the texture; others keep the game's addressing. Dust
    Storm in Practice for 30 s: normal dust (also checked by the user). **Verified 2026-10-07.**

11. Weathers that tint the stage (user report: with Dust Storm the stage filter didn't affect the building): under
    Cloudy, Dust Storm and others the game draws the stage tiles through its own ps_1_1 shader `saturate(texture + c0)
    x diffuse` (c0 = the weather's colour offset, e.g. Dust Storm (0, 0, -0.019, 0)), which the filter stepped aside
    for. Now that exact shader (compared byte for byte) is reproduced in ours (c2) and restored after the draw; other
    shaders are still left alone. No weather: frame-identical to before (231/231). Dust Storm: tinted draws filtered,
    building colour unchanged (mean RGB 60/63/28 off vs 61/64/29 on). **Verified 2026-10-07.**

## DpiAware (unreleased)

Main monitor 2560x1440 at 150%, left monitor 1920x1080 at 125% (x -1920..0 in physical coordinates). Measured from a
per-monitor-aware script (physical pixels); WindowScale=x2.

1. Copy install - th123.exe has the HIGHDPIAWARE compatibility setting (HKLM), so the process mode is already set:
   DM logs "through the thread mode"; window per-monitor aware; client 1280x960 on both monitors (was 1536x1152 on
   the 125% one); exclusive fullscreen native 2560x1440; position saved / restored. The title bar keeps the main
   monitor's size on the 125% monitor (no WM_DPICHANGED in thread mode) - cosmetic. **Verified 2026-10-07.**
2. CC2 install - no compatibility setting: "per-monitor aware (v2)"; client 1280x960 on both monitors, WM_DPICHANGED
   keeps the client and the position, only the frame resizes; position saved / restored. **Verified 2026-10-07.**
3. CC2, `DpiAware=0`: unaware, stretched by Windows (x2 doesn't fit at 150%, so x1 = 960x720 physical; 800x600 on the
   125% monitor) - the old behaviour. **Verified 2026-10-07.**
4. Not tested: Windows 7 / 8.1 fallbacks (system aware / per-monitor v1), exclusive fullscreen on the 125% monitor.

## 1.2.0 — Auto filter, per-scale defaults, zoom-following characters

Values picked 2026-10-08 (docs/calibration/CALIBRATION.md).

1. Ini upgrade from the v1.1.2 ini (Copy install, launched windowed): as shipped -> `Filter=Sharp`, `Sharpness=Auto`,
   `Xbr*=Auto`, IniVersion 1.2.0; `Filter=Point` + `Sharpness=2.25` -> both kept;
   `PersistState=0` + `PersistPosition=0` (no rewrite) -> ini untouched, loaded as Sharpness Auto (2.00 at x1/x2).
   **Verified 2026-10-08.**
2. `Filter=Auto`, windowed x1 (x2 column): Sharpness 2.00, characters 1.50 / stage 1.75, both layers drawn in
   Practice. x2.25 and x3 columns in the README captures (4.00, 1.50 / 4.00; 2.50, 1.75 / 2.50). **Verified
   2026-10-08.**
3. Alt+F into and out of Auto restores your own Sharpness / sprite / stage values; the Sharpness and sprite keys in
   Auto change only Auto's live values (until the output size, the filter or fullscreen changes). (User test.)
4. Characters at the farthest zoom (Reisen idle in a corner, the other player in the opposite one): no blur (the
   strength fades to the near strength within 0.1 of a whole-number scale). **Verified by the user 2026-10-08.**
   Also watch for a visible switch near the resting zoom (x2). (User test.)
5. Sprite hooks are now installed by default (`CycleFilter` is bound and reaches Auto): netplay, a replay and the
   PracticeEx menu on the MAIN install with its usual mods. (User test.)
6. Dust Storm seam (user report: with Auto, a 1-pixel dust-coloured line along the scrolling dust when zoomed out,
   not at zoom 1.0): the dust quad (512x128, u 0.01..1.01, v 0..1) kept WRAP on both axes, so at its top edge the
   vertical blend wrapped to the texture's bottom row. CLAMP vs the game's addressing is now chosen per axis (dust:
   v clamped, u wraps). Practice, Dust Storm forced, P1 at the left wall (zoom 0.762, quad top at y 316): row 316
   yellower than its neighbours on 154 px (mean +1.83) before, 86 px (+0.36, the texture's own noise) after.
   **Verified 2026-10-08.**

## Settings menu (Alt+M)

Build with Dear ImGui in `third_party/imgui` (see its README). Set `Log=1`; `menu: open` / `menu: closed` are logged.

1. **Open / close**: Alt+M, the Close button and the window's X close it. Fullscreen (exclusive and borderless) and
   windowed with `WindowedFilter=1`; windowed with `WindowedFilter=0` logs `menu: not available`.
2. **Mouse**: the cursor is drawn (the Windows one is hidden over the client); clicks, drags and the wheel work; the game
   gets none of them. Drag a slider and let go outside the window: the button isn't stuck (release capture).
3. **Alt+Tab / lock screen while open**, then back: no stuck button, the menu still draws (Reset recreates ImGui's buffers).
4. **Alt+Enter with the menu open**, both ways, and a windowed drag-resize: the menu survives the Reset at the new size.
5. **Size** (`Mode`, scales, custom size, window scale), **Always on top**, **Border color**: apply live, like the hotkeys.
6. **Filter** Point/Linear/Sharp/xBR/Auto: the output changes at once; Auto greys out the Sharpness and sprite controls.
   With the default ini (no sprite hotkeys, `CycleFilter` bound) the sharp sprites can be switched on from the menu.
7. **xBR / Sharp sprites / MSAA**: the Auto checkboxes keep the live value when unticked; MSAA x2/x4/x8 recreate the targets
   without a hitch or a black frame.
8. **Menu size**: Auto by screen height; the slider rescales fonts and widgets; `[Menu] Scale` applies at startup.
9. **Save settings**: with `PersistState=0` the button still writes `Mode`, scales, `Filter`, `Sharpness` and the menu's
   keys (`BackgroundColor`, `Custom*`, `Xbr*`, sprite values, `MultiSample`); an ini the menu never touched stays
   byte-identical after exit; with `PersistState=1` a touched menu is saved on exit.
10. **Other mods**: PracticeEx's menu doesn't show a copy in the top-left while the menu is open; SokuDirectXOptimizations
    (rendering on its own thread) with `use_d3d9ex=0`; `DisplayManager_AddOverlay` overlays still draw under the menu.
