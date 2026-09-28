# DisplayManager

A [SWRSToys](https://github.com/SokuDev/SokuMods) mod for Touhou Hisoutensoku (th123) 1.10a that handles fullscreen scaling and window sizing. It is capable of exclusive fullscreen mode with control over visual filters, which can improve display latency by 10-20ms. It is an alternative to WindowResizer: use one or the other, not both.

## Scaling

Fullscreen size (`Mode`):
- `FitToScreen` (default): the largest 4:3 size that fits the screen. Can be a non-integer scale (2.25× on 1080p).
- `IntegerScaling`: a whole-number multiple of 640×480 (`FullscreenScale`), reduced if it doesn't fit.
- `CustomResolution`: an exact size in pixels.

Upscale filter (`Filter`):
- `Sharp` (default): sharp-bilinear with adjustable `Sharpness`. At the default 1.5 it closely matches WindowResizer's look.
- `Point`: hard pixels. Pixels are only even at integer scales.
- `Linear`: bilinear.
- `xBR` (experimental): the xBR-lv2 pixel-art upscaler. It smooths diagonal and curved sprite edges and keeps flat areas crisp. The stages and zoomed sprites are already smooth in the game's own frame, so it mostly changes character outlines and text.

## Hotkeys

The modifier is Alt by default. Keys and modifier can be changed in the `[Hotkeys]` section; a blank or commented-out line disables that hotkey.

| Default | ini key | Action |
| --- | --- | --- |
| Alt+1…6 | `Scale1`…`Scale6` | Fullscreen: `IntegerScaling` at ×N. Windowed: resize the window to ×N. |
| Alt+0 | `FitToScreen` | Switch to `FitToScreen`. |
| Alt+F | `CycleFilter` | Cycle the filter. |
| Alt+K / Alt+L | `SharpnessDown` / `SharpnessUp` | Lower / raise `Sharpness` (switches to Sharp). |
| Alt+S / C / E / W | `XbrStrength` / `XbrCorner` / `XbrSlopes` / `XbrWidth` | Development: cycle an xBR setting (switches to xBR). Not saved; see the ini. |
| Alt+M | `ToggleMSAA` | Fullscreen: turn MSAA on (`MultiSample`, or ×8 if that is 0) / off. Not saved. |
| Alt+P | `AlwaysOnTop` | Toggle always-on-top. |

In fullscreen, scale and filter changes are shown briefly on screen.

## Install

1. Extract the `DisplayManager` folder from the latest [release](https://github.com/Aneroze/SokuDisplayManager/releases/latest) into Soku's `modules` folder.
2. Enable it in SokuLauncher, or add it to `SWRSToys.ini`.
3. Disable WindowResizer, IntegerFullscreen and ExclusiveFullscreen. If any of them is loaded, DisplayManager turns itself off.
4. In-game, Alt+Enter toggles fullscreen.

Changes between versions are in [CHANGELOG.md](CHANGELOG.md).

## Configuration

`DisplayManager.ini`, section `[Display]` unless noted:

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | `1` | Master switch. |
| `Mode` | `FitToScreen` | Fullscreen size, see [Scaling](#scaling). |
| `FullscreenScale` | `x2` | Scale for `Mode=IntegerScaling`. Called `IntegerScaling` before 1.0.4; old ini files are still read. |
| `WindowScale` | `x2` | Window size as a multiple of 640×480, capped to the monitor's work area. Falls back to `FullscreenScale` if missing. |
| `CustomWidth` / `CustomHeight` | `1280` / `960` | Size for `Mode=CustomResolution`. |
| `BackgroundColor` | `000000` | Border color, `RRGGBB`. |
| `Filter` | `Sharp` | Upscale filter, see [Scaling](#scaling). |
| `MultiSample` | `0` | Fullscreen MSAA: `0` (off), `2`, `4` or `8`. Only smooths polygon edges; the game's sprites and stages mostly aren't affected. |
| `Sharpness` | `1.50` | For `Filter=Sharp`: `1.0` (bilinear) to `4.0` (about the same as point). |
| `Resizable` | `1` | Allow resizing the window by dragging its edges (kept at 4:3). |
| `PersistState` | `1` | Save hotkey changes (mode, scales, filter, sharpness) to the ini on exit. |
| `PositionX` / `PositionY` | `-1` | Window position at launch. `-1` = leave it where the game puts it. |
| `Borderless` | `0` | `1` = a borderless window instead of exclusive fullscreen. Easier Alt+Tab and overlays, but frames go through the desktop compositor, which adds latency. |
| `FullscreenWidth` / `FullscreenHeight` / `FullscreenRefresh` | `0` | Force a fullscreen display mode instead of the monitor's current one. The refresh rate is only used when width and height are set. |
| `VSync` | `-1` | Exclusive fullscreen only. `-1` = the game's setting (off in vanilla), `0` = off, `1` = on. |
| `Log` | `0` | Write `DisplayManager.log` next to the ini. |
| `AllowWinKey` (`[Input]`) | `0` | `1` = let the Windows key through; the base game blocks it. Needs a restart. In exclusive fullscreen, opening the Start menu minimizes the game. |

## Notes

- **SokuDirectXOptimizations:** set its `use_d3d9ex=0`. With Direct3D 9Ex on, DisplayManager can have trouble attaching to the device.
- Exclusive fullscreen minimizes on Alt+Tab. It uses the monitor the game started on; borderless covers the monitor the window was on.

## For mod authors

- In fullscreen, the game window's client area covers the monitor and the 640×480 image is a scaled rect inside it. `DisplayManager.dll` exports `BOOL DisplayManager_GetGameRect(RECT *out)` (cdecl), which gives that rect in client pixels (the whole client when windowed). It returns `FALSE` when DisplayManager is inactive; assume a centered 4:3 image then.
- To draw on the final fullscreen frame (e.g. into the borders), register an overlay with `DisplayManager_AddOverlay` (see [`src/DisplayManagerOverlay.h`](src/DisplayManagerOverlay.h)). It is called after DisplayManager composites the frame, right before it is presented, whatever the mod load order. [SideNotes](https://github.com/Aneroze/SokuSideNotes) uses it.
- With `Borderless=1`, the game's present parameters at `0x8A0F68` still say `Windowed=0` (this keeps the game's Alt+Enter working), but the device is windowed. Use `GetSwapChain(0)` → `GetPresentParameters` to check.

## How it works

The game creates a plain Direct3D 9 device from one global `D3DPRESENT_PARAMETERS`, treats `Windowed == 0` as fullscreen, and always draws its 640×480 frame into the top-left of the backbuffer.

1. DisplayManager hooks `Direct3DCreate9` through the import table, then `CreateDevice` and `Reset`.
2. When the game asks for fullscreen, a copy of its present parameters gets a native-size backbuffer (exclusive, or windowed for borderless). The game's own struct is never modified.
3. The swapchain `Present` hook copies the 640×480 frame out of the backbuffer, fills the borders and draws it back scaled and centered: with the sharp-bilinear shader for `Sharp`, `StretchRect` otherwise. It also keeps the viewport at 640×480, which the game's 3D stage relies on. Other Direct3D devices in the process are left alone.
4. After Alt+Enter, the window is set up (borderless, or the saved window size and position) once the game's own window code has run.

It only loads into the th123 1.10a executable (checked by hash) and needs nothing beyond Windows system DLLs.

## Building

- **Windows (MSVC):** run `build.bat`. It finds Visual Studio with `vswhere`. Output: `build\DisplayManager.dll`.
- **mingw-w64:** run `./build.sh` with `g++-mingw-w64-i686` installed.

## License

[MIT](LICENSE).
