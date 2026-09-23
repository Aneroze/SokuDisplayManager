# WindowResizer rendering research — why "1280-start looks better than 640→1280"

**Status:** empirically settled at the pixel level (2026-09-23). The SamplerProbe run below then
**DISPROVED** the "it's the D3D texture filter" hypothesis — see the UPDATE block.

---
## UPDATE (SamplerProbe results, 2026-09-23) — the sampler-filter theory is WRONG

SamplerProbe (hooks `SetSamplerState`, logs every distinct state) was run on the MAIN install under
upstream WindowResizer for a **1280-start** and a **640→Alt+2 start**. Result:

- **The game sets IDENTICAL sampler states in both startups.** Filter histograms match (~1:3.3
  POINT:LINEAR on sampler 0, per-draw toggling); init sequence byte-identical. Confirmed a second
  time with full-state logging: the *complete distinct state set* diffs to EMPTY between a 640-start
  and a 1280-start (logs: `docs/samplerprobe-640start-fullstate.log`, `...-1280start-fullstate.log`).
- **The ONLY sampler states the game ever sets** are `ADDRESSU/ADDRESSV=WRAP` and
  `MAG/MIN/MIPFILTER ∈ {POINT, LINEAR}`, on samplers 0–1. **No MIPMAPLODBIAS, no MAXMIPLEVEL, no
  anisotropy, no SRGB, nothing else.** So there is no sampler knob that differs.

⇒ The smooth-vs-aliased difference is **NOT** the D3D sampler filter, and **forcing the sampler to
LINEAR would do nothing** (the game already uses LINEAR predominantly in *both* startups). The
"DM smooth mode = force sampler LINEAR" idea below is **abandoned.**

Also disproven as the cause: a global **sub-pixel shift** (best fit dx=dy=0, no improvement) and a
simple **gaussian blur** (only 8.35→7.79 MAD, negligible) of the crisp 640 → the smooth 640. The two
static 640 renders differ by ~7.8 MAD in a way none of these simple post-process models reproduce.

**What's left:** the difference is baked into the game's **draw-time rendering** — mip-LOD selection
(driven by screen-space texture-coord derivatives) and/or vertex/projection scale — which the game
computes from the **window size at device-creation time** (WR's `0x414F8C` only forces the *viewport*
dimensions, not whatever feeds the draw-time scale). Window=1280 at init → smooth render; window=640 at
init → aliased render; Alt+2 afterward doesn't re-trigger it.

---
## UPDATE 2 (DrawProbe results, 2026-09-23) — it's the PROJECTION, tied to window size

DrawProbe (hooks `SetTexture` + `SetTransform`, read-only) run autonomously for a 640-start and a
1280-start (harness `scratchpad/run_soku.ps1`; logs `scratchpad/runs/DrawProbe-*.log`):

- **No mipmaps.** *Every* texture the game binds is `levels=1` (fmt 21 = A8R8G8B8). So mip-LOD is
  physically impossible; the `MIPFILTER=LINEAR` state is a no-op. **Mip theory dead.**
- **The projection matrix differs by startup**, and it's exactly the window size:
  - 640-start: `PROJ m11=2/640 (0.0031), m22=2/480 (0.0042)` → a 640×480 logical space.
  - 1280-start: `PROJ m11=2/1280 (0.0016), m22=2/960 (0.0021)` → a 1280×960 logical space.
  - VIEW and WORLD are identity in both; the texture set is byte-identical; viewport is 640 in both.

⇒ **Confirmed mechanism:** the game builds its 2D orthographic projection from the **window size at
device creation**. In the 1280-start it renders its scene in **1280×960 logical space clamped into a
640×480 viewport** (WR's `0x414F8C`); in the 640-start it renders **1:1** at 640. That geometry-to-pixel
mapping difference is the smooth-vs-aliased cause — not mips, not sampler state. The causal lever is
**window size at device-creation → projection scale**, reproducible on demand.

Remaining unknown (needs a `DrawPrimitiveUP` vertex/UV probe at character-select to be 100%): the exact
reason 1280-logical-into-640-viewport samples textures more smoothly with no intermediate 1280 target and
no mips (per-quad UV phase / coverage). Not required to act — the lever is known.

**DM implementation path (the game is committed to):** make the game render in "1280-logical mode" the
way WR's 1280-start does — i.e. ensure the game sees a **large window at device creation** so it builds
the large-space projection, while the viewport stays 640 (as DM/WR already arrange). This is distinct
from the abandoned HighRes attempt (which chased a high-res *backbuffer*); here the backbuffer/viewport
stay 640 and only the projection/geometry scale changes. Prototype + A/B against WR before finalizing.

## UPDATE 3 (SmoothRender prototype in DisplayManager, 2026-09-23)

Implemented `SmoothRender` in DM (`src/DisplayManager.cpp`): WR's two tricks — redirect the game's main
`CreateWindowExA` to force a 1280x960 client at creation, + patch `0x414F8C` to clamp viewport init to
640. Results from testing on the copy install (logs: DM's own timestamped `DisplayManager-*.log`):

- **Windowed SmoothRender = WR's 1280-start, confirmed identical by the user.** The core render fix works.
- **Projection = 2/clientW**, tracking the window client size live (verified via a `SetTransform` diag
  hook: client 1280 → renderW 1280 smooth; client 640 → renderW 640 aliased; client 2560 → renderW 2560).
- **Exclusive fullscreen breaks it.** DM forces bb=native, so the game window becomes native 2560x1440
  (16:9); projection → 2/2560, and the present-time **viewport = 2560x1440** (native, NOT 640 — the
  `0x414F8C` clamp does not hold after the fullscreen Reset). The game renders full-native but its content
  scale decouples → small-in-corner + sprite overflow (the old "HighRes" symptom). 
- **Native passthrough** (present the raw native render, skip DM's grab): produced exactly that
  small/overflow/dead-hotkeys result — because content scale and viewport decouple. NOT a dead end in
  principle: if the game's internal content-scale value is found + patched to match the native viewport,
  raw native render would give TRUE high-res (the best outcome). Worth revisiting via disassembly/memory.
- **The projection alone does NOT govern smoothness.** Alt+2 restores renderW=1280 in both the broken
  (start→Alt+1→Alt+2) and smooth (fullscreen-exit→Alt+2) cases — same projection, different look. The
  differentiator is a device **Reset**: window resizes (Alt+1/2, no Reset) change the projection but leave
  a hidden render-state stuck; a Reset re-initializes it. Shrinking the window to 640 breaks smooth
  stickily until a Reset. So the smooth-vs-aliased state lives in the game's own render-size logic (set at
  device init/Reset), not in any single D3D state we can clamp from outside.
- Dead ends this ruled out: `GetClientRect` IAT hook = **NOT FOUND** (game reads client size another way,
  e.g. WM_SIZE — not a hookable import); `SetViewport` clamp = **no effect** (game never calls SetViewport
  in fullscreen; viewport is the device default = native bb).

**Conclusion:** the black-box (D3D-hook) approach has plateaued. Windowed smooth works; exclusive-smooth
and native-high-res both require understanding the game's internal render-size/projection logic by
**disassembly** (find where it reads the window/client size and computes the render scale + the state a
Reset re-inits), then patching it — exactly the kind of memory patch WR does at `0x414F8C`. The reliable
no-RE fallback for smooth fullscreen is **borderless 4:3** (replicate WR exactly: 4:3 window filling the
height, black backdrop, bb=640, D3D9 stretch, no grab) — proven, but loses exclusive/Independent-Flip.

Config added: `SmoothRender` (0/1), `SmoothRenderWidth/Height` (1280x960), `SmoothPassthrough`
(1=native passthrough, 0=forced-4:3/640 grab path). Diagnostics: DM logs are timestamped per run; a
`SetTransform` hook logs `PROJ set` on change when `Log=1`.

## UPDATE 4 (disassembly of th123.exe, 2026-09-23)

Disassembler: `scratchpad/disasm.py` (Capstone + manual PE parse; VA↔file-offset; `d <va> <len>`,
`refs`, byte-search). th123.exe: ImageBase 0x400000, `.text` 0x401000. Note linear sweep desyncs on
data — use byte-pattern search for call sites.

Confirmed pipeline:
- **Backbuffer size = `GetClientRect(hwnd)`**. At `0x414F86`: `call [0x8571F8]` (GetClientRect, IAT
  0x8571F8 — the ONLY call site). At `0x414F8C`: `edx = rect.right-rect.left` (client W),
  `eax = rect.bottom-rect.top` (client H) → stored to **`0x8A0F68`** (BackBufferWidth) / **`0x8A0F6C`**
  (Height). **This is exactly what WR (and DM's `patchViewportInit`) overwrite** with
  `mov edx,640 / mov eax,480` to force a 640 backbuffer.
- **The projection (`2/clientW`) uses a DIFFERENT source** — with `0x414F8C` patched to 640, the
  backbuffer globals are 640 yet the projection stayed `2/1280` (client). So a second place reads the
  window/client size and the per-frame projection follows it live (Alt+1/2/3 change it with no Reset).
  NOT located yet: it is not `GetClientRect` (only the one call above) nor `GetWindowRect`
  (IAT 0x85720C, one call at `0x40E9CE`, which is input/mouse coordinate mapping). Most likely the
  `WM_SIZE` handler stores the client size into a render-size global that the projection reads.
- Window plumbing: class registered at `0x7FB6BE` (RegisterClassExA), `lpfnWndProc = 0x7FB560` (a thin
  wrapper that tail-calls the real handler **`0x408260`** with context struct `0x89FF90`); main window
  created at `0x7FB713` (CreateWindowExA). **Next RE step:** trace `0x408260`'s `WM_SIZE` (5) path to
  find the render-size global; patching it to a constant (e.g. 1280) would make the render always smooth
  regardless of window size / fullscreen — the clean fix (and would also kill the Alt+1 break).
- Globals: `0x8A0F68` bbW, `0x8A0F6C` bbH, `0x8A0E30` device, `0x8A0E34` swapchain, `0x8998B0` fullscreen
  flag, `0x8A0FA0` cached desktop mode.

### Exclusive-smooth attempt shipped for testing (2026-09-23)
Key RE result: `0x408260` (the real wndproc) does NOT handle `WM_SIZE` (falls to DefWindowProc), so the
projection is rebuilt from the **single `GetClientRect` at `0x414F86`** (IAT `0x8571F8`) when the game
re-runs its device setup on resize. So DM's `SmoothRender` + `SmoothPassthrough=0` now:
1. **Hooks the `GetClientRect` IAT slot `0x8571F8` directly** (a generic import search missed it) →
   `myGetClientRect` returns `g_smoothW x g_smoothH` (4:3) for the game window. This makes the game build
   a `2/1280` (4:3, smooth) projection regardless of the real window size or exclusive-native size — and
   also fixes the Alt+1-shrinks-to-640 break, uniformly. (DM/WR's `0x414F8C` patch still forces the
   backbuffer to 640 separately.)
2. **Forces the render viewport to 640** — once after each Reset (`forceSmoothFullscreen`) and per-frame
   in the `mySetTransform` hook (re-asserted when the game sets its scene projection), since the game's
   default viewport otherwise follows the native backbuffer in fullscreen and the game never calls
   `SetViewport` itself.

DM then grabs the 640 top-left and upscales as usual, preserving exclusive/Independent-Flip. The earlier
window-resize-in-exclusive hack was dropped (unnecessary once GetClientRect is faked, and risked
glitching exclusive mode). UNTESTED as of writing (built + deployed to the copy). Remaining risk: the
game may reset the viewport via `SetRenderTarget` after setting the projection (would need a
`SetRenderTarget` hook to re-clamp); and faking GetClientRect could offset mouse-coordinate mapping in
menus (char-select is keyboard, so likely fine). Also still open: exit-fullscreen restores the window too
small. Fallbacks if this fails: borderless-4:3 (WR-exact), or find & patch the projection's size read
directly (the ortho is built manually with a half-texel offset: `m11=2/w`, `m41≈-1-1/w`).

## UPDATE 5 (mechanism SOLVED + fix, 2026-09-23)

Vertex probe (DrawPrimitiveUP hook) proved the game draws its scene in **640x480 space regardless of
window size** (background quad byte-identical in the smooth and aliased phases). It uses **no projection**
(the `2/clientW` projections seen earlier were from mods **InGameHostlist.dll** / **ReplayHudExtras.dll**,
not th123), **no render target** (renders straight to the backbuffer), identical samplers, bb=640 in both.
So the game does NOT render at higher quality with a bigger window.

⟹ **The "smooth" look is purely D3D9's WINDOWED-PRESENT stretch filter.** A background research pass
(Wine `wined3d swapchain_blit`, DXVK `d3d9_swapchain`, MS docs) established:
- D3D9's windowed present uses **bilinear (LINEAR) when it scales**, point/none when 1:1. No special kernel.
- The runtime picks "1:1 copy present" vs "stretch present" **at device-creation time** and does NOT
  re-evaluate on a plain window resize (only a device `Reset` does). Window **larger than backbuffer at
  creation** → stretch present → linear → smooth. Created at 640 then resized → copy path → point → aliased.
  (This is exactly the WR 1280-start vs 640-start-then-Alt+2 difference, and DM's SmoothRender=1 vs =0.)
- DWM composites the client-sized redirection surface 1:1; it does not do the 640->client upscale.
- **Why DM's `Filter=Linear` looked blurrier than WR:** the D3D9 **-0.5 texel/pixel offset**. D3D9's
  present bilinear is correctly texel-aligned; a StretchRect/quad without the -0.5 offset blends every
  output pixel 50/50 instead of only boundary pixels → too soft. The kernel is the same; alignment differs.

**Fix shipped (exclusive-fullscreen compatible):** DM's fullscreen post-process gained a **`Filter=Sharp`**
mode — a `ps_2_0` sharp-bilinear pixel shader (`shader/sharpbilinear.hlsl`, compiled to `src/sharpbilinear.h`
via fxc, embedded; no d3dx dependency). It draws a full-screen quad over the centered dst rect sampling the
captured 640 render-target texture, **with the -0.5 offset**, through a LINEAR sampler. `Sharpness=1` =
correctly-aligned bilinear (should match D3D9's smooth windowed present = WR look); higher narrows the
interpolation band toward point. Tunable live via **Alt+K / Alt+L** ([Hotkeys] SharpnessDown/Up) and the
`Sharpness` ini key. This keeps **true exclusive fullscreen / Independent-Flip** (hard user requirement) —
the smooth look is reproducible in exclusive because DM already grabs the game's 640 frame and can apply
any kernel. The capture RT is now an RT **texture** (`g_captureTex`) + surface; Point/Linear still use
StretchRect, Sharp uses the shader; a D3DSBT_ALL state block saves/restores device state around the draw.

Status: built + deployed to the copy, UNTESTED as of writing. Note: `SmoothRender` (windowed-only) is kept
for the user to revisit; all other experiments (GetClientRect fake, SetViewport clamp, passthrough, the
diagnostic SetTransform/SetRenderTarget/DrawPrimitiveUP hooks, forceSmoothFullscreen) were stripped back to
the clean base (git HEAD b4fec1c) before adding the filter.

### Autonomous harness (`scratchpad/run_soku.ps1`, mirrored in `tools/`)
Safe self-driving Soku runner for these experiments: refuses to launch if th123/th123e is already
running (never touches a live game — per the standing rule); sets the startup window size via
WindowResizer's `[Size]` ini; clears + scoops DrawProbe/SamplerProbe/DMProbe logs to `scratchpad/runs/`;
closes only processes it launched. `-Width N`, `-Tag`, `-WaitSec`, `-NoClose` (leave up for manual
char-select nav), `-CloseOnly`. Menu-nav-to-character-select and D3D-window screenshotting are not yet
automated (title/menu screens work for probe data; portrait visuals still want a hand or a vertex probe).

**Consequence for DisplayManager (corrected):** DM post-processes the *finished* 640 backbuffer, so it
**cannot** turn a 640-start (aliased) render into a 1280-start (smooth) render by any upscale/filter/
shift. Under DM the game's window is ~640 at device creation → it renders in the **aliased** mode →
DM's linear upscale then blurs an aliased image (why Quosu saw blur). The ONLY way for DM to get WR's
smooth render is to make the **game** render in "1280 mode" — i.e. **size the game window large at
device-creation time** (what WR's 1280-start does), while still presenting/scaling as DM does. This is
a real, invasive change (hook window creation / the init size the game reads) and needs experimentation;
it is NOT the same as the abandoned "force a high-res backbuffer" HighRes attempt (that forced backbuffer
size; this only needs the game to *think* it started large so it picks the smooth draw-time scale).
Next confirming probe if pursued: hook `SetTexture`/`GetLevelCount` (do portraits have mipmaps?) and
`SetTransform`/`DrawPrimitive` to compare the draw-time scale between startups.

---

This documents a long-standing, repeatedly-observed phenomenon so we never have to re-derive it:
starting Touhou Hisoutensoku with WindowResizer **directly at 1280×960** looks *softer but better*
than starting at **640×480 and then Alt+2**-ing up to 1280×960, which looks *sharper but worse*.

## TL;DR

1. **Both** WR startups produce a final image that is an **exact nearest-neighbor 2× of a 640×480
   render** (measured: fraction of perfectly-uniform 2×2 blocks = **1.0** for *both* screenshots).
   Neither uses a bilinear/bicubic *present-time* upscale. The 2× doubling is identical in both.
2. **The two 640×480 source renders themselves differ.** The 1280-start renders a **smooth** 640
   (the game uses **LINEAR** texture filtering); the 640-start renders a **hard / aliased** 640
   (the game uses **POINT**). Verified visually on 100%-static content (menu-bar text and the
   character-roster portraits — smooth vs jagged hair/outlines).
3. So "1280-start is blurry but better" is **not** an upscale-filter effect. The softness is
   **baked into the game's own 640 render**; nearest-neighbor 2× just faithfully doubles whatever
   640 it is given. Blurry source → blurry-looking 2×; crisp source → crisp-looking 2×.
4. **WindowResizer itself does not filter anything.** Upstream WR sets **no** sampler state. The
   smooth-vs-hard difference is the **game's own** texture filtering, and the only thing that differs
   between the two startups is the **window size at device-creation time** (1280 vs 640).

## How it was measured

Two full-screen captures at `F:\Games\Touhou\screenshots\`:
`2026-09-23 19_03_17-1280p-start-up.png` and `...19_03_47-640p-start-up.png`, both 1280×960.
Not the same frame (the flame/wind backgrounds animate), so analysis used only **static** regions:
the bottom menu bar and the roster portraits.

Analysis script: `scratchpad/wr_filter_analysis.py` (pixel math with PIL/numpy). Findings:

- **Uniform 2×2-block fraction = 1.0 for both images** → both are exact NN-2× of a 640 render.
- Reconstructed each true 640 by taking the top-left pixel of every 2×2 block (`img[::2,::2]`).
- Upscaling the reconstructed 640 with BILINEAR/BICUBIC/LANCZOS did **not** match the other startup;
  NEAREST/BOX matched best — i.e. no present-time smoothing filter explains the difference.
- The two reconstructed 640 renders differ in static regions (menu bar MAD ≈ 4.7, roster ≈ 7.7),
  and the difference is edge/texture smoothing: 1280-start smooth (LINEAR), 640-start aliased (POINT).

## WindowResizer source analysis (upstream)

Upstream `WindowResizer.cpp` (fetched from `raw.githubusercontent.com/SokuDev/SokuMods/master`;
last upstream commit ~2 years before 2026) `setupHooks()` does only:

- **NOP `0x00445817` (3 bytes)** and **NOP `0x004405BC` (5 bytes)** — stop the game resetting its
  internal fullscreen/window state, so WR owns the fullscreen flag (`*(char*)0x8998B0`).
- **Patch `0x00414F8C` (16 bytes)** — hardcode the D3D viewport/backbuffer init to `baseWidth=640` /
  `baseHeight=480` *"instead of getting the window size."* i.e. the game **normally reads the window
  size here**; WR overrides just this site to 640.
- **Hook `CreateWindowExA`** (call operand at `0x007fb713+2`) — sizes/positions/styles the main
  window at creation (this is where the 1280-vs-640 window size at startup comes from).
- **Hook `SendMessageA`** (ptr at `0x00857248`) — intercept the game's Alt+Enter
  `SendMessageA(hwnd, WM_SYSKEYDOWN, VK_RETURN, 0)` to run WR's own fullscreen toggle.

**Upstream sets no `SetSamplerState`.** The `SetSamplerState`→`D3DTEXF_POINT` hook in our *local*
`modules/WindowResizer/WindowResizer.cpp` is **the user's own patch, not upstream** (it forces
MAGFILTER=POINT, deliberately leaving MINFILTER alone). That patch's purpose ("force it to behave the
same between a 640 and 1280 start") is consistent with everything here: forcing POINT makes both
startups render the hard/aliased look.

## Mechanism (strong hypothesis, empirically supported)

The game selects its texture filter **once, at graphics init, from the window size it sees** at
`CreateWindowExA`/device-creation time:

- **1280-start:** WR creates the window at 1280 → game sees a scaled (non-640) window → chooses
  **LINEAR** texture filtering → smooth 640 render.
- **640-start:** window is 640 at init → game chooses **POINT** → aliased 640 render. A later Alt+2
  resize does **not** re-trigger the decision, so it stays aliased.

WR forces the *viewport/backbuffer* to 640 either way (`0x414F8C`), but does **not** patch whatever
site reads the window size for the filter decision — hence the difference survives. Not yet confirmed
by disassembling the game's init; the **SamplerProbe** tool (below) is meant to confirm exactly which
sampler state(s) differ.

## Why this matters for DisplayManager (and Quosu's "DM is blurrier")

DisplayManager grabs the game's backbuffer and upscales it **in the Present hook — after** the game
has already rendered its 640. Under DM the game creates a ~640 window, so it picks **POINT** → an
**aliased** 640 source. DM then linear-upscales that → it is **blurring an already-aliased image**,
the worst of both worlds. That is why Quosu (1080p, non-integer 2.25× → linear) perceived DM as
blurrier than WR, which had the **smooth** source to begin with.

**To reproduce WR's good look, DM must make the *game* render the smooth 640**, not fix it up after.
Two routes:

- **(preferred) Force the game's texture sampler to LINEAR** via a `SetSamplerState` vtable hook —
  the *inverse* of the user's POINT patch (cover MIN and/or MAG per what SamplerProbe shows) — so the
  game produces the smooth 640 regardless of startup. Then integer point-double for crisp scaling
  (smooth source + point 2× = the WR-1280-start look); linear for non-integer scales.
- (fragile) Recreate WR's startup condition by creating the window large before device init so the
  game itself picks LINEAR. Brittle with DM's exclusive-fullscreen + own-scaling design.

`IDirect3DDevice9::SetSamplerState` vtable index = **69** (3 IUnknown + d3d9.h order;
GetSamplerState=68, SetSamplerState=69). Filter values: NONE=0, POINT=1, LINEAR=2, ANISO=3.
Sampler types: MAGFILTER=5, MINFILTER=6, MIPFILTER=7.

## The "which WindowResizer.dll is patched" confusion — resolved

The game only ever loads the file **literally named `WindowResizer.dll`**. In the main install
(`F:\Games\Touhou\SokuLauncher\Soku\modules\WindowResizer\`):

| File | size | date | sha1 | what it is |
| --- | --- | --- | --- | --- |
| `WindowResizer.dll` | 87552 | 2024-09-05 | `ea40005c…` | **official upstream** (forces 640) — the one that loads |
| `WindowResizer-old.dll` | 95744 | 2024-01-12 | `c8e6ba0f…` | older official build (not loaded) |
| `WindowResizer-patched.dll` | 98816 | 2026-09-11 | — | **the user's POINT patch** (wrong filename → never loaded) |

So all our DMProbe runs showing backbuffer=640 were **genuine upstream behavior, never polluted** —
`ea40005c` is upstream. The user's patch was inert because of its filename.

**SokuLauncher reverts manual swaps.** `SokuLauncher.json` has `AutoCheckForUpdates:true` and
`AutoCheckForInstallable:true` with source `soku-launcher-modpacks.github.io`. On launch it re-syncs
mod folders, backing the previous folder up to `F:\Games\Touhou\SokuLauncher\bk\WindowResizer.rar`
(that backup is where the three DLLs above were catalogued). Any DLL you drop in as
`WindowResizer.dll` gets reverted to the official build on the next launch. **To test a custom WR
build:** disable `AutoCheckForUpdates` (and/or the WindowResizer source) in `SokuLauncher.json`, or
launch `th123.exe`/`th123e.exe` directly instead of through `SokuLauncher.exe`.

Also present and possibly relevant: a disabled **`PointMin`** mod and a `PointMin.log` (2026-09-11) in
the Soku root — likely a prior point/min-filter experiment. Worth reading before building DM's LINEAR
mode.

## SamplerProbe — the confirming tool

`scratchpad/samplerprobe/SamplerProbe.cpp` → `SamplerProbe.dll` (MSVC x86, links
shlwapi/user32/gdi32). Deployed to `F:\Games\Touhou\SokuLauncher\Soku\modules\SamplerProbe\` and
enabled in the main `ModLoaderSettings.json` (backup: `ModLoaderSettings.json.samplerprobebak`).

It hooks `IDirect3DDevice9::SetSamplerState` (vtable slot 69) on the shared d3d9 vtable and **only
observes** — it forwards every call unchanged, so it is safe alongside upstream WindowResizer (which
sets no sampler state). It writes `SamplerProbe.log` next to the DLL:

- one `CONTEXT …` line (backbuffer/viewport/client) so you can tell which startup it was,
- the first ~200 filter-setting calls verbatim (the init sequence),
- a per-second histogram (on change) of how many times each value was set, per sampler, for
  MAG/MIN/MIP.

**Procedure:** run a **1280-start**, save/rename the log; delete it; run a **640-start**, save.
**Prediction to confirm:** in the 1280-start the main sprite sampler shows **MIN (and/or MAG) =
LINEAR** dominant; in the 640-start the same sampler shows **POINT** dominant. If confirmed, build the
DM "smooth mode" = force that sampler state to LINEAR + integer point-double.

**Cleanup when done:** remove `modules/SamplerProbe/` (and `modules/DMProbe/`) and restore
`ModLoaderSettings.json` from the `.samplerprobebak` / `.dmprobebak` backups.
