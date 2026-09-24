# DisplayManager

A small [SWRSToys](https://github.com/SokuDev/SokuMods) mod for Touhou Hisoutensoku (th123) 1.10a that manages the game's display: **crisp, integer-scaled exclusive fullscreen** plus **window sizing/positioning**. A modern replacement for WindowResizer.

Instead of the base game's blurry fullscreen, this mod keeps your desktop at its **native resolution** and draws the game **centered with black borders**, in one of three modes. Because it uses **true exclusive fullscreen**, it also gets the low-latency direct-flip present path ("Independent Flip"), which a legacy Direct3D9 game like this one can't get in a borderless window. When windowed, it sizes the window (by drag or hotkey) with a locked 4:3 aspect. It can also remember your settings between runs, set a spawn position, and color the border.

- **FitToScreen** (default) — the largest aspect-correct size that fills the screen (fills the height, pillarbox on the sides). Biggest image; may be a non-integer scale (smoothed with linear filtering).
- **IntegerScaling** — an exact whole-number multiple of 640×480 (e.g. x2 = 1280×960), point-sampled for perfectly crisp pixels. Auto-reduced if it wouldn't fit.
- **CustomResolution** — an exact size in pixels.

**Hotkeys** (live, in-game). Default modifier is **Alt**; all keys and the modifier are rebindable in the `[Hotkeys]` ini section, and commenting out a line disables that hotkey:
- **Alt+1**…**Alt+6** — set IntegerScaling ×1…×6 in fullscreen, or resize the window to that scale (640×480 × N) when windowed.
- **Alt+0** — FitToScreen.
- **Alt+P** — toggle always-on-top.
- **Alt+F** — cycle the upscale filter (Auto → Point → Linear → Sharp) live.
- **Alt+K** / **Alt+L** — decrease / increase the Sharpness (live; also switches `Filter` to `Sharp`).

In fullscreen, each of these changes briefly shows the new value on-screen (e.g. `X2`, `FITTOSCREEN`, `SHARP 1.50`).

## Install

1. Download the latest [release](https://github.com/Aneroze/SokuDisplayManager/releases/latest) and unarchive the `DisplayManager` folder into your Soku `modules` directory.
2. Enable **DisplayManager** in SokuLauncher's mod settings (or add a line for it in `SWRSToys.ini` if you don't use the launcher).
3. **Use either this or WindowResizer, but never both at the same time.**
4. Launch the game and press **Alt+Enter** to toggle fullscreen.

## Configuration

All options live in `DisplayManager.ini`:

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | `1` | Master switch. |
| `Mode` | `FitToScreen` | `FitToScreen`, `IntegerScaling`, or `CustomResolution`. The Alt+0…4 hotkeys change this live. |
| `IntegerScaling` | `x2` | Used when `Mode=IntegerScaling`. Whole-number scale of 640×480 (`x2` = 1280×960). Auto-reduced if it wouldn't fit the screen. |
| `CustomWidth` / `CustomHeight` | `1280` / `960` | Used when `Mode=CustomResolution`. Exact output size in pixels (centered). |
| `BackgroundColor` | `000000` | Border/letterbox color in fullscreen, as `RRGGBB` hex. |
| `Filter` | `Sharp` | How the 640×480 image is scaled up. `Auto` = point at integer scales, linear otherwise. `Point` = always hard pixels. `Linear` = always smooth. `Sharp` = tunable sharp-bilinear (see `Sharpness`) — **this is what reproduces WindowResizer's look** (WR's smoothness is just D3D9's windowed-present bilinear, which `Sharp` matches with correct half-texel alignment that plain `Linear` misses). |
| `Sharpness` | `1.50` | Only used when `Filter=Sharp`. Range `1.0`–`4.0`: `1.0` = plain aligned bilinear (WR's smooth upscale); higher = crisper toward hard pixels; the default `~1.5` closely matches WindowResizer; `~4.0` is effectively point. Tune live with Alt+K/Alt+L (shown on-screen). |
| `Resizable` | `1` | Allow resizing the window by dragging its edges (aspect locked to 4:3). Alt+1–4 resizing works either way. |
| `PersistState` | `1` | Remember the current mode/scale on exit and restore it next launch. |
| `PositionX` / `PositionY` | `-1` | Where the window spawns on launch, in screen pixels. `-1` = leave the initial position alone. The live position is never written back to the ini. |
| `FullscreenWidth` / `FullscreenHeight` | `0` | *(advanced)* Force the fullscreen **display mode** — the actual screen resolution the monitor switches to (not the game surface, not the scaled output). `0` = auto-detect your monitor's native resolution (recommended). Set both to override if auto-detection picks the wrong mode. |
| `FullscreenRefresh` | `0` | *(advanced)* Refresh rate for the forced mode; `0` keeps the native refresh. |
| `Log` | `0` | Set to `1` to write a `DisplayManager.log` next to the ini for troubleshooting. |
| `Borderless` | `0` | Use a borderless window for "fullscreen" instead of true exclusive fullscreen. **Not recommended:** borderless loses the low-latency direct-flip path (a legacy D3D9 game can't get Independent Flip in a window), so it has more input latency. Enable only if you want easier alt-tab/overlays. |

The `[Hotkeys]` section sets `Modifier` (`Alt`/`Ctrl`/`Shift`/`Win`/`None`) and rebinds each hotkey (a letter/digit; commented-out or blank = disabled): `FitToScreen`, `Scale1`…`Scale6`, `AlwaysOnTop`, `CycleFilter`, `SharpnessDown`, `SharpnessUp`.

With `Borderless` available, this mod is a superset of WindowResizer — window sizing/positioning, borderless *and* exclusive fullscreen, always-on-top, and crisp integer scaling.

Only the value for the active `Mode` matters; the others are ignored. Hotkey changes apply immediately and are not written back to the ini (set the `Mode` there for your startup default).

## Notes

- If you also run **SokuDirectXOptimizations**, set its `use_d3d9ex=0`. Its Direct3D 9Ex mode wraps the graphics device in a way DisplayManager can't hook, so scaling and hotkeys won't work; with `use_d3d9ex=0` the two run together fine.
- It's real exclusive fullscreen, so Alt-Tab minimizes the game (normal, and fast at native resolution).
- Windowed mode is left completely alone; the mod only acts when you go fullscreen.
- Some other mods can freeze Alt+Enter if they create Direct3D resources in `D3DPOOL_DEFAULT` without handling a device reset. That's a bug in those mods, not this one.

## How it works

The game builds one global `D3DPRESENT_PARAMETERS` and creates a plain Direct3D9 device (`Direct3DCreate9`, not Ex; `SwapEffect` `DISCARD`). Its "fullscreen" state is literally `present.Windowed == 0`, and it always draws its 640×480 surface at 1:1 into the top-left of the backbuffer, relying on the fullscreen display mode to upscale the whole framebuffer.

The mod:

1. Intercepts `Direct3DCreate9` (via the import table) → hooks `IDirect3D9::CreateDevice`.
2. On `CreateDevice`/`Reset`, when the game asks for fullscreen, forces the backbuffer to the **native** desktop mode so the monitor never rescales (true exclusive fullscreen at native resolution). Windowed requests are restored to the game's own size.
3. Hooks the swapchain's `Present`: grabs the game's rendered frame into an offscreen render target, clears the whole backbuffer black, then `StretchRect`s it back **scaled and centered** with **point filtering** — crisp integer scaling with black borders, independent of how the game maps its coordinates.

It only patches the exact th123 1.10a build (verified by hash) and has no external dependencies beyond the Windows system DLLs.

## Building

**Windows (MSVC):** run `build.bat` from an *x86 Native Tools Command Prompt for VS* (or plain `cmd` — it locates Visual Studio via `vswhere`). Output: `build\DisplayManager.dll`.

**Any host (mingw-w64):** run `./build.sh` with `g++-mingw-w64-i686` installed.

Only the Windows SDK / mingw headers are needed — no game SDK, no third-party libraries.

## License

[MIT](LICENSE).
