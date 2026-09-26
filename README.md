# DisplayManager

A small [SWRSToys](https://github.com/SokuDev/SokuMods) mod for Touhou Hisoutensoku (th123) 1.10a that manages the game's display: **crisp, integer-scaled exclusive fullscreen** plus **window sizing/positioning**. A modern replacement for WindowResizer.

Instead of the base game's blurry fullscreen, this mod keeps your desktop at its **native resolution** and draws the game **centered with black borders**, in one of three modes. Because it uses **true exclusive fullscreen**, it also gets the low-latency direct-flip present path ("Independent Flip"), which a legacy Direct3D9 game like this one can't get in a borderless window. When windowed, it sizes the window (by drag or hotkey) with a locked 4:3 aspect. It can also remember your settings between runs, set a spawn position, and color the border.

- **FitToScreen** (default) — the largest aspect-correct size that fills the screen (fills the height, pillarbox on the sides). Biggest image; may be a non-integer scale (e.g. 2.25× on 1080p).
- **IntegerScaling** — an exact whole-number multiple of 640×480 (e.g. x2 = 1280×960). Auto-reduced if it wouldn't fit.
- **CustomResolution** — an exact size in pixels.

The upscale itself uses the `Filter` setting: by default **Sharp**, a tunable sharp-bilinear filter that reproduces WindowResizer's look; `Point` gives hard pixels (perfectly crisp at integer scales), `Linear` plain smooth, `Auto` point at integer scales and linear otherwise.

**Hotkeys** (live, in-game). Default modifier is **Alt**; all keys and the modifier are rebindable in the `[Hotkeys]` ini section, and commenting out a line disables that hotkey:
- **Alt+1**…**Alt+6** — in fullscreen, switch to IntegerScaling at ×1…×6 (`FullscreenScale`); when windowed, resize the window to that scale (640×480 × N, `WindowScale`) without touching the fullscreen mode.
- **Alt+0** — FitToScreen.
- **Alt+P** — toggle always-on-top.
- **Alt+F** — cycle the upscale filter (Auto → Point → Linear → Sharp) live.
- **Alt+K** / **Alt+L** — decrease / increase the Sharpness (live; also switches `Filter` to `Sharp`).

In fullscreen, the scale and filter hotkeys briefly show the new value on-screen (e.g. `X2`, `FITTOSCREEN`, `SHARP 1.50`); Alt+P has no on-screen readout, and nothing is shown while windowed.

## Install

1. Download the latest [release](https://github.com/Aneroze/SokuDisplayManager/releases/latest) and unarchive the `DisplayManager` folder into your Soku `modules` directory.
   Release notes: see [CHANGELOG.md](CHANGELOG.md).
2. Enable **DisplayManager** in SokuLauncher's mod settings (or add a line for it in `SWRSToys.ini` if you don't use the launcher).
3. **Use either this or WindowResizer, but never both at the same time.** The same goes for the older IntegerFullscreen and ExclusiveFullscreen mods. If one of them (`WindowResizer.dll`, `IntegerFullscreen.dll`, `ExclusiveFullscreen.dll`) is loaded, DisplayManager detects it when the game creates its device and disables itself (no fullscreen changes, scaling, window management or hotkeys; noted in the log with `Log=1`), so disable the other mod to use this one.
4. Launch the game and press **Alt+Enter** to toggle fullscreen.

## Configuration

All options live in `DisplayManager.ini`:

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | `1` | Master switch. |
| `Mode` | `FitToScreen` | `FitToScreen`, `IntegerScaling`, or `CustomResolution`. The Alt+0…6 hotkeys change this live (Alt+1…6 only while fullscreen). |
| `FullscreenScale` | `x2` | Fullscreen size when `Mode=IntegerScaling`: whole-number scale of 640×480 (`x2` = 1280×960). Auto-reduced if it wouldn't fit the screen. Named `IntegerScaling` before v1.0.4 — old inis are still read, and renamed on first launch when `PersistState=1`. |
| `WindowScale` | `x2` (the `FullscreenScale` value if missing) | Windowed client size, as a whole-number scale of 640×480. Changed by Alt+1…6 while windowed; independent of the fullscreen `Mode`. Reduced to the largest scale that fits the monitor's work area (the window is also kept on-screen). |
| `CustomWidth` / `CustomHeight` | `1280` / `960` | Used when `Mode=CustomResolution`. Exact output size in pixels (centered). |
| `BackgroundColor` | `000000` | Border/letterbox color in fullscreen, as `RRGGBB` hex. |
| `Filter` | `Sharp` | How the 640×480 image is scaled up. `Auto` = point at integer scales, linear otherwise. `Point` = always hard pixels. `Linear` = always smooth. `Sharp` = tunable sharp-bilinear (see `Sharpness`) — **this is what reproduces WindowResizer's look** (WR's smoothness is just D3D9's windowed-present bilinear, which `Sharp` matches with correct half-texel alignment that plain `Linear` misses). |
| `Sharpness` | `1.50` | Only used when `Filter=Sharp`. Range `1.0`–`4.0`: `1.0` = plain aligned bilinear (WR's smooth upscale); higher = crisper toward hard pixels; the default `~1.5` closely matches WindowResizer; `~4.0` is effectively point. Tune live with Alt+K/Alt+L (shown on-screen). |
| `Resizable` | `1` | Allow resizing the window by dragging its edges (aspect locked to 4:3). Alt+1…6 resizing works either way. |
| `PersistState` | `1` | Save the current `Mode`, `FullscreenScale`, `WindowScale`, `Filter` and `Sharpness` to the ini on exit (only keys that changed are written), so hotkey changes stick across launches. `0` = never write the ini. |
| `PositionX` / `PositionY` | `-1` | Where the window spawns on launch, in screen pixels. `-1` = leave the initial position alone. The live position is never written back to the ini. |
| `FullscreenWidth` / `FullscreenHeight` | `0` | *(advanced)* Force the fullscreen **display mode** — the actual screen resolution the monitor switches to (not the game surface, not the scaled output). `0` = auto-detect your monitor's native resolution (recommended). Set both to override if auto-detection picks the wrong mode. A mode your monitor doesn't list falls back to the current desktop mode. |
| `FullscreenRefresh` | `0` | *(advanced)* Refresh rate for the forced mode; `0` keeps the native refresh. A rate the mode doesn't have snaps to the closest one it does. |
| `VSync` | `-1` | *(advanced)* Vertical sync in exclusive fullscreen: `-1` = the game's own setting (off in vanilla; SokuDirectXOptimizations' `vsync` can turn it on), `0` = force off, `1` = force on. See the 62 fps note below. No effect with `Borderless=1`, which the desktop always syncs. |
| `Log` | `0` | Set to `1` to write a `DisplayManager.log` next to the ini for troubleshooting. |
| `Borderless` | `0` | Use a borderless window for "fullscreen" instead of true exclusive fullscreen. **Not recommended:** borderless loses the low-latency direct-flip path (a legacy D3D9 game can't get Independent Flip in a window), so it has more input latency. Enable only if you want easier alt-tab/overlays. |
| `AllowWinKey` *(section `[Input]`)* | `0` | `1` = let the Windows key work while the game has focus (Win+Shift+S screenshots, virtual-desktop switching, ...). The base game itself blocks it: its DirectInput keyboard is created with `DISCL_NOWINKEY`, and this clears just that flag. In exclusive fullscreen, opening the Start menu minimizes the game like Alt+Tab. Restart the game to change it. |

The `[Hotkeys]` section sets `Modifier` (`Alt`/`Ctrl`/`Shift`/`Win`/`None`) and rebinds each hotkey (a letter/digit; commented-out or blank = disabled): `FitToScreen`, `Scale1`…`Scale6`, `AlwaysOnTop`, `CycleFilter`, `SharpnessDown`, `SharpnessUp`.

With `Borderless` available, this mod is a superset of WindowResizer — window sizing/positioning, borderless *and* exclusive fullscreen, always-on-top, and crisp integer scaling.

Only the value for the active `Mode` matters; the others are ignored. Hotkey changes apply immediately; with `PersistState=1` (default) they are also written back to the ini on exit, with `PersistState=0` the ini values are your fixed startup defaults.

## Notes

- If you also run **SokuDirectXOptimizations**, set its `use_d3d9ex=0`. Its Direct3D 9Ex mode wraps the graphics device in a way DisplayManager can't hook, so scaling and hotkeys won't work; with `use_d3d9ex=0` the two run together fine.
- It's real exclusive fullscreen, so Alt-Tab minimizes the game (normal, and fast at native resolution).
- In windowed mode the game's rendering is left alone; the mod only manages the window: at launch it sizes it to the `WindowScale` scale (and moves it to `PositionX`/`PositionY` if set), keeps drag-resizing at 4:3, and handles always-on-top. Coming back from fullscreen (Alt+Enter) restores the window's previous position, the `WindowScale` size and always-on-top. Window sizes are capped to what fits the monitor's work area, and the window is kept on-screen.
- Borderless fullscreen covers the monitor the window was on when you pressed Alt+Enter; exclusive fullscreen uses the monitor of the adapter the game started on.
- If the graphics driver refuses the native fullscreen mode (custom CRU modes, rotated screens, Wine/DXVK), DisplayManager retries with the default refresh rate and then falls back to the game's own (vanilla) fullscreen instead of hanging; `Log=1` shows each step.
- **62 fps (giuroll `enable_f62`) on a 60 Hz screen:** the game runs faster than the screen refreshes. In exclusive fullscreen with vsync off (the vanilla default) a tear line rolls up the whole screen about twice a second; with `VSync=1`, or in borderless/WindowResizer, the tear is gone but about 2 frames per second are skipped and latency goes up by up to a frame. A 120 Hz or faster screen avoids both, as does G-Sync/FreeSync. At 60 fps none of this applies.
- Some other mods can freeze Alt+Enter if they create Direct3D resources in `D3DPOOL_DEFAULT` without handling a device reset. That's a bug in those mods, not this one.
- **For mod authors** (overlays, side windows, mouse input):
  - In fullscreen the game window's client area covers the whole monitor and the 640×480 image is a centered, scaled rect inside it, so mouse coordinates can't be mapped by dividing by the client size. `DisplayManager.dll` exports `BOOL DisplayManager_GetGameRect(RECT *out)` (cdecl): the image's rect in client pixels (the whole client when windowed); `FALSE` if DisplayManager is off — then assume a 4:3 image centered in the client.
  - With `Borderless=1` the game's own present parameters (`0x8A0F68`) still say fullscreen (`Windowed=0`) — DisplayManager only changes its own copy, which is what keeps Alt+Enter working — while the device is actually windowed. To know whether the device is windowed (e.g. before creating an additional swap chain for a side window), ask the device: `GetSwapChain(0)` → `GetPresentParameters`.

## How it works

The game builds one global `D3DPRESENT_PARAMETERS` and creates a plain Direct3D9 device (`Direct3DCreate9`, not Ex; `SwapEffect` `DISCARD`). Its "fullscreen" state is literally `present.Windowed == 0`, and it always draws its 640×480 surface at 1:1 into the top-left of the backbuffer, relying on the fullscreen display mode to upscale the whole framebuffer.

The mod:

1. Intercepts `Direct3DCreate9` (via the import table) → hooks `IDirect3D9::CreateDevice`.
2. On `CreateDevice`/`Reset`, when the game asks for fullscreen, forces the backbuffer to the **native** desktop mode so the monitor never rescales (true exclusive fullscreen at native resolution; with `Borderless=1`, a windowed device at native size instead). These changes are made to a *copy* of the game's present parameters — the game's own struct is never modified, so its Alt+Enter toggle (and other mods) always see the real windowed/fullscreen state. Windowed requests pass through untouched.
3. Hooks the swapchain's `Present`: grabs the game's 640×480 frame, upscales it **centered** into an offscreen backbuffer-sized surface — with the sharp-bilinear pixel shader for `Filter=Sharp`, or `StretchRect` with point/linear filtering (also the fallback if the shader can't draw) — then fills the backbuffer with the border color and copies the result over. It also pins the viewport to 640×480 (again whenever the backbuffer is re-bound mid-frame), which the game's 3D stage and some character draws rely on. Only the game's own device and swapchain are touched; other Direct3D 9 devices in the process (overlays, other mods) are left alone.
4. After an Alt+Enter toggle, sets up the window (borderless popup, or the restored windowed size/position/always-on-top) *after* the game's own post-toggle window repositioning, via a message to its window.

It only patches the exact th123 1.10a build (verified by hash) and has no external dependencies beyond the Windows system DLLs.

## Building

**Windows (MSVC):** run `build.bat` from an *x86 Native Tools Command Prompt for VS* (or plain `cmd` — it locates Visual Studio via `vswhere`). Output: `build\DisplayManager.dll`.

**Any host (mingw-w64):** run `./build.sh` with `g++-mingw-w64-i686` installed.

Only the Windows SDK / mingw headers are needed — no game SDK, no third-party libraries.

## License

[MIT](LICENSE).
