# Changelog

## Unreleased

**Added**
- `VSync` option (`-1` = the game's own setting, `0` = off, `1` = on) for exclusive fullscreen, and a note on
  what 62 fps (giuroll `enable_f62`) looks like on a 60 Hz screen: rolling tear with vsync off, ~2 skipped
  frames per second with it on.

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
