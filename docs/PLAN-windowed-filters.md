# Plan: filters in windowed mode

Status: **option 1 implemented and shipped in 1.1.0** (`WindowedFilter=1`, see "Implementation" at the end); options
2 and 3 kept for the record. Tested in-game by the user 2026-09-29 (startup without an extra Reset, Alt+Enter both ways,
filter cycling windowed). Screenshot check the same day (one frozen frame, captured after DM's upscale): windowed
and fullscreen give pixel-identical game images for Point/Linear/Sharp at x2, x2.25 and x3 (xBR: 0.01% of pixels at
x2.25 differ - exact texel-boundary ties depend on the image's screen position); borderless vs exclusive at x3 are
pixel-identical for every filter.

## Why filters don't apply windowed today

Windowed, DM leaves the device exactly as the game made it and only sizes the window (`setWindowScaled`). The
backbuffer stays 640x480, and D3D9's windowed Present stretches it to the client area with bilinear filtering.
That's the same stretch that gives WindowResizer its soft look (see `WindowResizer-rendering-research.md`), and
we can't choose its filter.

Any filter (Sharp, Point, xBR, even plain integer Point) needs a backbuffer at least as large as the client area,
so our upscale can write the final pixels. There is no cheaper "Point only" variant: once the backbuffer is large
enough, every filter, MSAA, the overlay API and the OSD come along for free.

Borderless already shows the approach works: it is a *windowed* device with a native-size backbuffer, and
`mySCPresent` runs the full composite on it (grab 640x480, upscale with the filter, overlays, OSD). Windowed
filtering is that same path inside a decorated window. The only new problem is **resizing**: Alt+1..6 and the 4:3
drag-resize change the client size, and today that works because the runtime stretches whatever backbuffer exists.

## Options

### 1. Backbuffer = client size, device Reset on every resize

- In `applyFullscreenParams`, the windowed branch sizes our copy to the client (640N x 480N) and sets `g_active`.
- Every resize needs a device Reset: a visible hitch each time, and drag-resize has to defer the Reset to
  `WM_EXITSIZEMOVE` (runtime-stretched in between).
- The Reset must go through the game's own wrapper (the Alt+Enter path, 0x415220), because an external Reset
  would leave the game's default-pool resources released. That wrapper is where the Alt+Enter crash reports came
  from (it leaves textures released on failure), so this option is the **riskiest**.

### 2. Oversized backbuffer + Present source rect (not chosen)

- Create the windowed backbuffer **once**, at the largest monitor's size (or the largest 640N x 480N that fits
  it). Switch *our copy* of the present params to `D3DSWAPEFFECT_COPY`: D3D9 requires `pSourceRect`/`pDestRect` to
  be NULL unless the swap chain uses COPY. The game's global keeps DISCARD and is never modified, same as today.
- Each frame, `mySCPresent` composites into the top-left `clientW x clientH` region, then calls `oSCPresent` with
  `src = {0, 0, clientW, clientH}`, `dst = NULL`. The copy to the window is 1:1, with no runtime stretch. The game
  already presents through the swapchain (0x8A0E34), which we hook, so no new hook is needed.
- Resizing is free (only the rect changes), drag-resize updates live, and non-integer sizes work (Sharp and xBR
  handle any scale).
- If the client ever grows past the backbuffer (dragged onto a larger monitor after creation): fall back to
  today's runtime stretch, or do one deferred Reset through the game's wrapper.

Code touch points:
- `applyFullscreenParams`: new windowed branch (size, `SwapEffect = COPY`, `g_active = true`, `g_bbW/H` = the
  allocation).
- `computeOutput`: windowed output rect = the whole client at (0,0) (the window is already locked to 4:3), not
  the Fit/Integer/Custom modes, which are fullscreen-only. Recompute on `WM_SIZE`.
- `mySCPresent`: pass the source rect windowed. `fillBorders`/stage `ColorFill` only need to cover the client
  region.
- `syncPresentParams`: a windowed copy currently goes back to the game whole; it must no longer write back our
  size or `SwapEffect`.
- `wndProc`: the `WM_SIZING` 4:3 lock is currently `!g_active` only; keep it on while windowed.
- `DisplayManager_GetGameRect` / overlay `gameRect`: the client rect windowed (already documented that way).
- Ini: opt-in switch (e.g. `WindowedFilter=1`), default off so windowed stays byte-for-byte passthrough.

**VERIFY:**
- COPY + non-NULL `pSourceRect` behaves on the drivers we care about (NVIDIA, AMD, Intel), with both
  `IMMEDIATE` and `ONE` intervals.
- The Bug 1 viewport pin (`setGameViewport`) and the Bug 4 stage path behave the same with a windowed large
  backbuffer (borderless suggests yes).
- Alt+Enter both ways, and windowed spawn → fullscreen → windowed with a different scale.
- Moving the window between monitors of different sizes and DPI.
- Other mods that read the backbuffer size themselves (ImGui HUDs: ReplayHudExtras, InGameHostlist) still draw
  at 640x480 top-left, as they do in fullscreen.

### 3. Second swap chain sized to the client

- Keep the game's 640x480 implicit swap chain, and `CreateAdditionalSwapChain` for the window at client size.
  Recreate it on resize, which needs no device Reset.
- The game's Present has to be redirected to the new swap chain. Mods that hook Present (ImGui HUDs, etc.) may
  stop firing depending on hook order. More invasive than option 2 for no gain. **Not recommended.**

## Display latency

- **The filter pass:** one 640x480 grab, one shader quad at client size and possibly one 1:1 stage copy. That's
  a fraction of a millisecond of GPU time (probably microseconds), done in the same frame before Present, so no
  extra frame is queued. It also replaces the runtime's own bilinear stretch at Present, so net work is about
  the same. **Expected: no added latency.**
- **Windowed mode itself:** it stays composited by DWM (blt model), about a frame behind exclusive fullscreen.
  That's the 10-20 ms the README mentions. Filters neither add to nor remove it.
- **COPY vs DISCARD:** both are a copy into the window's redirection surface when windowed, so there's no
  latency difference.
- Getting windowed latency down would need D3D9Ex + FLIPEX, which rejects `D3DPOOL_MANAGED` resources (the game
  almost certainly uses them). Out of scope.
- **VERIFY** with the PresentMon kit (`C:\Projects\soku-latency-kit`): windowed with the runtime stretch vs
  windowed + Sharp at the same scale. Expect identical within noise.

## Why option 1 after all

Option 2's oversized backbuffer is visible to everything that hooks Present: OBS game capture would likely record the
whole oversized surface with the game in a corner, and overlays anchored to a corner (FPS counters, Steam/Discord)
would sit outside the visible part. It also relies on the rarely used COPY swap effect with a source rect. Option 1
keeps the ordinary setup (DISCARD, backbuffer = window, the same shape borderless already uses), and its cost - one
Reset per resize - is rare in practice: the window size is set once and saved, and the first spawn and a return
from fullscreen are sized right up front, so they need no extra Reset.

## Implementation (option 1)

- **Size at creation / mode switch:** `applyFullscreenParams(pp, entering, firstTime)`. For a windowed request it
  sizes our copy of the present params to `windowedBackbufferSize`: when `entering` (first CreateDevice, or
  fullscreen -> windowed) the client size `onWindowedEntry` is about to give the window (`placeWindowScaled` with
  the saved `WindowScale` and the spawn / pre-fullscreen position), otherwise the current client area. At least
  640x480 in each direction, because the game draws its frame 1:1 into the top-left.
- **Resize = one Reset:** `wndProc` posts a private message on `WM_SIZE` (outside a drag) and on
  `WM_EXITSIZEMOVE`; `WM_SIZE` during a drag (`WM_ENTERSIZEMOVE`..`WM_EXITSIZEMOVE`) is ignored, so a drag costs
  one Reset when released. `onWindowResized` compares the client area with the backbuffer and, if they differ,
  calls the game's own Reset wrapper `0x415100` (the function Alt+Enter uses, from the same place: the window
  procedure, between frames, under the game's render lock); `myReset` then sizes it. Skipped while a window setup
  is still queued (`g_applyPending`, it posts a check when done), minimized, or fullscreen.
- **Everything else is the fullscreen path:** `g_active` is set, so the Present post-process, viewport pin, MSAA,
  OSD and overlays run unchanged; `computeOutput` fills the backbuffer (`g_winActive` ignores the fullscreen
  `Mode`). The game's own struct keeps 640x480.
- **Failure:** if a forced windowed Reset fails, `callWithFallback` retries with the game's own params (as for
  fullscreen) and sets `g_winFilterFailed`: windowed stays the plain stretch for the rest of the session instead of
  retrying on every resize.
- `WindowedFilter=0` restores the old passthrough exactly.

**VERIFY in-game:** first spawn needs no Reset (log: no "window resized ... resetting" at startup); Alt+1..6 and a
drag each log exactly one Reset; Alt+Enter both ways (exclusive and `Borderless=1`) with no extra Reset; a
fullscreen start (`0x8998B0`); all filters + the OSD windowed; ImGui mods (ReplayHudExtras, InGameHostlist) and
SideNotes windowed; minimize/restore; moving between monitors; `WindowedFilter=0`.
