# Changelog

## 1.1.2 — 2026-10-05

**Fixed**
- PracticeEx's menu showing a second, small copy in the top-left with the Sharp and xBR filters.

## 1.1.1 — 2026-10-05

**Fixed**
- Freeze or crash on Alt+Enter, window resize or alt-tab when another mod (e.g. SokuShaderPro) made Direct3D drop
  DisplayManager's device hooks.

## 1.1.0 — 2026-09-29

**Added**
- `WindowedFilter=1` (default): the filters (Sharp, Point, Linear, xBR), MSAA, the on-screen readout and the overlay
  API now work in windowed mode too. The backbuffer is made the size of the window instead of the game's 640x480,
  so a window resize resets the device once: after a drag-resize ends (the image is stretched during the drag), or
  right away for a Scale hotkey. No change to display latency. `WindowedFilter=0` = the old behaviour (D3D9
  stretches the 640x480 image bilinearly). If a windowed Reset ever fails, DisplayManager falls back to the old
  behaviour for the rest of the session.
- `Filter=xBR` (experimental): the xBR-lv2 pixel-art upscaler (Hyllian, MIT), in the Alt+F cycle after Sharp. Falls
  back to Linear if the GPU can't run its shader (needs pixel shader 2.b). Tuned with `XbrStrength` (default 0.65,
  blended with the plain pixels), `XbrCorner` (A-D, default B), `XbrSlopes` (default 0) and `XbrWidth` (default
  2.0). Development hotkeys to cycle them live (Alt+S / C / E / W) are in the ini, commented out.
- `MultiSample=0/2/4/8`: MSAA for the game's rendering (fullscreen, and windowed with `WindowedFilter`), and a `ToggleMSAA` hotkey to switch it on (×8
  when `MultiSample=0`) and off live. It only smooths polygon edges and made no visible difference in testing, so
  both are left out of the default ini; add them by hand to try them (see the README).

**Changed**
- Alt+F cycles Point → Linear → Sharp → xBR.

**Note for existing installs**: DisplayManager doesn't add new keys to an existing ini; the new settings use their
defaults when missing.

## 1.0.6 — 2026-09-28

**Added**
- Overlay API for other mods: `DisplayManager_AddOverlay` / `DisplayManager_RemoveOverlay` (see
  `src/DisplayManagerOverlay.h`). A registered callback draws on the composited fullscreen frame right before it
  is presented, and is told before a device Reset.

## 1.0.5 — 2026-09-26

**Fixed**
- An ini from before 1.0.3 got `WindowScale` added at the end of `[Display]` (after `Borderless`) without its
  comment. It is now inserted, with its comment, right after `FullscreenScale` (only with `PersistState=1`).

**Removed**
- The `Auto` filter (point at integer scales, linear otherwise). An ini that still says `Filter=Auto` uses the
  default, `Sharp`. Alt+F now cycles Point → Linear → Sharp.

**Added**
- `VSync` option (`-1` = the game's own setting, `0` = off, `1` = on) for exclusive fullscreen, and a note on
  what 62 fps (giuroll `enable_f62`) looks like on a 60 Hz screen: rolling tear with vsync off, ~2 skipped
  frames per second with it on.
- `[Input] AllowWinKey` (default `0`): `1` lets the Windows key work while the game has focus (Win+Shift+S
  screenshots, virtual-desktop switching). The base game blocks it itself - its DirectInput keyboard is created
  with `DISCL_NOWINKEY` - and this clears just that flag.
- For other mods: the export `DisplayManager_GetGameRect(RECT *)` gives the rect the 640x480 image occupies in
  the game window's client area, so overlays/side panels (e.g. ReplayInputView++'s F6 panel) can map mouse
  positions in fullscreen, where the client covers the whole monitor. README notes how to tell whether the device
  is really windowed in `Borderless=1` (ask the swap chain, not the game's present-parameters global).

## 1.0.4 — 2026-09-25

**Changed**
- The fullscreen scale setting is renamed from `IntegerScaling` to `FullscreenScale`, so it follows the same
  naming as `WindowScale` (and `FullscreenWidth/Height/Refresh`). `Mode=IntegerScaling` is unchanged.
- Existing ini files keep working: the old `IntegerScaling=` key is still read when `FullscreenScale` is
  missing. With `PersistState=1` (the default) the key is renamed in place on first launch, keeping its value
  and your comments; with `PersistState=0` the file is never touched. If both keys are present,
  `FullscreenScale` wins and the old key is removed on exit.

## 1.0.3 — 2026-09-25

Fixes from a full code review, all tested in-game.

**Fixed**
- Leaving fullscreen no longer loses the window setup: it comes back at the same position and size (4:3 client),
  with always-on-top kept. Before, the game's own window code ran afterwards and re-centered/resized the window.
- `Borderless=1`: Alt+Enter now works both ways (it used to get stuck in borderless), and borderless covers the
  monitor the window is on.
- DisplayManager no longer modifies the game's own display settings in memory, so the game and other mods see
  the values they expect.
- If the Sharp filter can't draw (missing resources, driver issue), it falls back to normal scaling instead of
  showing a frozen or garbage frame.
- A rare crash at startup (two hook paths racing each other) is fixed, and the fallback that attaches to a device
  created by another mod now works with the mod loader.
- A frame the game presents twice (it retries when the GPU is busy) is no longer upscaled twice (a flash of a
  zoomed-in corner).
- The 640x480 viewport stays pinned even if another mod switches render targets mid-frame (prevents the
  "giant Okuu" problem from coming back).
- Only the game's own Direct3D device is touched, not overlays or other mods' devices.
- The Sharp filter's extra drawing pass no longer triggers other mods' per-frame hooks a second time. This also
  removes a full-screen copy and fill every frame (cheaper on integrated GPUs and at 4K).

**Changed**
- The forced fullscreen mode is checked against what the display actually supports. An unsupported
  `FullscreenWidth/Height` falls back to the monitor's current mode, an unlisted `FullscreenRefresh` snaps to the
  nearest supported rate, and if the driver still refuses, that switch falls back to the game's own settings
  instead of hanging on a black screen.
- New `WindowScale` setting: Alt+1..6 in a window now only change the window size and no longer overwrite the
  fullscreen mode (`Mode`/`IntegerScaling`). Windows are kept inside the screen.
- If WindowResizer (or the older IntegerFullscreen/ExclusiveFullscreen) is also loaded, DisplayManager stands
  down completely and logs why, instead of processing every frame twice.
- `Filter` defaults to Sharp when missing from the ini (was Auto).
- README / ini documentation corrected to match the actual behavior (Alt+0..6, what is saved on exit, window
  handling).

## 1.0.2 — 2026-09-25

**Fixed**
- Sharp filter: a block at the top-left of the image looked blurry at non-integer scales (e.g. 2.25x on a 1080p
  monitor).
- Exclusive fullscreen always uses the native resolution of the monitor the game's Direct3D device is on.
  Dragging the window to another monitor no longer switches the main monitor to the wrong resolution.

## 1.0.1 — 2026-09-24

**Fixed**
- Okuu (Utsuho): a giant copy of her sprite could appear off-screen / intrude on the stage in fullscreen.
- PracticeEx: a small un-upscaled copy of its menu appeared in the top-left corner.
- The Sharpness setting (and Alt+K/L) had no visible effect.
- The ini is only rewritten on exit when a setting actually changed.

**Added**
- On-screen readout for every hotkey (mode, scale, filter, sharpness).

## 1.0.0 — 2026-09-23

First release.
- Crisp exclusive fullscreen at the monitor's native resolution, with the game scaled and centered:
  FitToScreen, IntegerScaling (x1..x6) and CustomResolution. Low-latency direct-flip presentation.
- Filters: Sharp (tunable sharp-bilinear, matches WindowResizer's look), Point, Linear, Auto.
- Optional borderless fullscreen, background/border color, manual fullscreen mode override.
- Window sizing (Alt+1..6, aspect-locked drag-resize), spawn position, always-on-top (Alt+P).
- Hotkeys with a configurable modifier; settings saved to the ini on exit.
