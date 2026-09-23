# ExclusiveFullscreen

A small [SWRSToys](https://github.com/SokuDev/SokuMods) mod for Touhou Hisoutensoku (th123) 1.10a that gives you **crisp, integer-scaled exclusive fullscreen**.

Instead of the base game's fullscreen — which stretches the 640×480 image to fill the monitor height by a non-integer factor (e.g. 1080 / 480 = 2.25×) and looks blurry — this mod keeps your desktop at its **native resolution** and draws the game **centered at an exact integer scale** (2× = 1280×960 by default) with **black borders** around it. Because it uses **true exclusive fullscreen**, it also gets the low-latency direct-flip present path ("Independent Flip"), which a legacy Direct3D9 game like this one can't get in a borderless window.

It's zero-configuration: it detects your resolution at runtime and picks the largest integer scale that fits (up to `MaxScale`).

## Install

1. Download the latest [release](https://github.com/Aneroze/SokuExclusiveFullscreen/releases/latest) and unarchive the `ExclusiveFullscreen` folder into your Soku `modules` directory.
2. Enable **ExclusiveFullscreen** in SokuLauncher's mod settings (or add a line for it in `SWRSToys.ini` if you don't use the launcher).
3. **Disable WindowResizer.** Both mods manage the window/fullscreen path — use one or the other.
4. Launch the game and press **Alt+Enter** to toggle fullscreen.

## Configuration

All options live in `ExclusiveFullscreen.ini`:

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | `1` | Master switch. |
| `MaxScale` | `2` | Largest integer scale to use (`2` = up to 2× / 1280×960). Never exceeds what fits your screen, and drops to 1× on screens too small for 2×. Set `3` to allow 3× on tall enough monitors, etc. |
| `WidthOverride` / `HeightOverride` | `0` | Set **both** to force an exact output size in pixels (centered), ignoring `MaxScale`. Keep a 4:3 ratio and an integer multiple of 640×480 for crisp pixels. |
| `SourceWidth` / `SourceHeight` | `640` / `480` | The game's own render size that is grabbed and upscaled. Leave as-is unless the image looks clipped or has a border of leftover pixels. |
| `Log` | `0` | Set to `1` to write an `ExclusiveFullscreen.log` next to the ini for troubleshooting. |

## Notes

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

**Windows (MSVC):** run `build.bat` from an *x86 Native Tools Command Prompt for VS* (or plain `cmd` — it locates Visual Studio via `vswhere`). Output: `build\ExclusiveFullscreen.dll`.

**Any host (mingw-w64):** run `./build.sh` with `g++-mingw-w64-i686` installed.

Only the Windows SDK / mingw headers are needed — no game SDK, no third-party libraries.

## License

[MIT](LICENSE).
