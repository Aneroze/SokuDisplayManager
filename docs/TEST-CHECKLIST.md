# DisplayManager — in-game test checklist

Run on the Copy install (`F:\Games\Touhou\SokuLauncher - Copy`). Check the REAL window size/position on screen, not
only what the game renders (a backbuffer capture can't show window placement).

## Increment 1 — must-fix items (master: 0fa2204 .. 39dd628)

1. **Windowed start**: window is the IntegerScaling size, at PositionX/Y if set, 4:3 client; drag-resize stays 4:3.
2. **Fullscreen start** (quit while fullscreen, relaunch), exclusive AND borderless: comes up fullscreen; Alt+Enter
   out gives the IntegerScaling size at the spawn position.
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

## Increment 2 — should-fix items 7–12 (branch `should-fix`)

(to be filled in by the agent implementing it)

## Increment 3 — StretchRect-only Sharp filter (branch `sharp-stretchrect`, stacked on `should-fix`)

(to be filled in by the agent implementing it)
