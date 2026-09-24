# DisplayManager Bug 1 — Okuu (Utsuho) giant off-screen sprite: research trail

## ✅ RESOLVED (2026-09-24)

**Root cause — the default D3D9 viewport.** th123 never calls `SetViewport`; it relies on D3D9 setting the
viewport to the whole render target at CreateDevice/Reset. In vanilla that's the 640×480 backbuffer, so the
viewport is 640×480. DisplayManager forces a **native-sized backbuffer** (e.g. 2560×1440), so the default
viewport becomes 2560×1440. Draws that go through the viewport transform (the 3D stage, and Okuu/Utsuho's
character draw) are therefore scaled by ~backbuffer/640 from the top-left corner → the giant off-screen Okuu
and a slightly over-zoomed stage. Windowed was fine because DM keeps the 640×480 backbuffer there.

**Fix (committed):** `setGameViewport(dev)` pins the viewport to `g_srcW × g_srcH` (640×480) after
CreateDevice, after Reset, and **re-pinned every Present** on the render thread (so nothing — Reset,
SetRenderTarget, other mods — can undo it). ~15 lines in `src/DisplayManager.cpp`. Verified in real
borderless fullscreen with the full mod set: harness shows `vp=640x480` during Okuu's draw and the giant is
gone (user-confirmed on screen). Windowed passthrough is untouched (the pin is gated to `g_active`).

**Red herrings (what the long trail below chased and why it was wrong):**
- **`[0x871538]`=3.0 effect-loop scale** (sites `0x4778A9/BA/CB`): a real ×3 on Okuu's 470×270 XYZRHW cape
  sprite, but it's the SAME const in windowed and fullscreen, so not the cause. Patching it shrank that one
  hidden layer in the raw backbuffer but did **not** change the on-screen result (BEFORE/AFTER 640-crops were
  identical — the tell that it was the wrong lever). Do NOT patch it; it also feeds the sky.
- `clampvp` only rewrites viewports the game *sets* (it sets none); `fakevp` only fakes `GetViewport` reads —
  neither pins the actual default viewport, so both looked like "no effect."
- **Tooling gap:** the harness's oversized-draw detector reads *raw vertex data* (XYZRHW screen coords),
  which the viewport transform does **not** change — so it was structurally blind to a viewport-scaling bug.
  The `diag DRAW[..]`/`info` dump does record the live viewport (`vp=WxH`), which is what actually confirms
  the fix. Lesson: to diagnose scaling, check the viewport/rasterized pixels, not just vertex coords.

---

## Original research trail (superseded — kept for the record)

**Status (2026-09-24):** cause fully characterized, exact draw path found; the precise scale computation is
being disassembled. Reproduced/diagnosed with `tools/SokuHarness/` (see its README).

## Symptom
With Okuu (Utsuho, char id 18) under DisplayManager in **exclusive fullscreen**, a giant Okuu sprite
(~2.8-4x) is drawn far outside the 640×480 game area, tracking her animation. When she moves up/left it
slides into the visible region. Her **shadow and chibi sprite render correctly** — only the cloak/effect
layer is oversized.

## What it is NOT (ruled out empirically)
- Not the backbuffer resolution: DM at bb=1280×960 and bb=2560×1440 give the **same** giant.
- Not the window/client size: 640 windowed and 1280 windowed both render fine.
- Not: present-param bbW/bbH globals (`0x8A0F68/6C`), cached desktop mode (`0x8A0FA0/A4`), the D3D viewport
  (clamping to 640 = no effect), the fullscreen flag (`0x8998B0` poked to 0 = no effect), live
  `GetClientRect`/`GetViewport`/`GetDisplayMode`/`GetBackBuffer` (faked = no effect), any static int/float
  scale global (no writable 2560/1440/3.0/4.0 in .data). `0x871538`=3.0 is the **3D stage/sky** scale, not
  the cloak's.

## What it IS (confirmed)
- **Gated by the exclusive-fullscreen render path.** User matrix: 640 windowed=fine, 1280 windowed=fine,
  1280 exclusive=bug; toggling windowed↔fullscreen flips it live. (DM's borderless also triggers it because
  DM writes the game's fullscreen flag `0x8998B0=1` in borderless.)
- The giant = Okuu's cloak/effect layer, a **470×270 texture drawn as a ~1320×768 screen-space (XYZRHW)
  `DrawPrimitiveUP` quad** (uniform ~2.8x), plus a 320×320 aura layer. Drawn from **`0x004075D0`** (in the
  effect/particle loop at `~0x477880`). Call chain: `0x477950 → 0x470511 → 0x47A9D1 → 0x481AC8 → 0x41E157`.
- `0x4075D0` reads pre-computed screen corner coords from the effect object at `[obj+0xB0..0xDC]`, applies
  the standard **D3D9 −0.5 texel offset** (const `[0x8677A8]`=0.5, correct/crisp mapping — not a bug), writes
  XYZRHW verts to `[obj+8..0x64]`, then calls game-device (`0x896B50`) vtbl+0x14C = DrawPrimitiveUP.
- So the ~3x scale is applied **upstream**, when `obj+0xB0` is built (effect world-rect × a fullscreen
  camera/screen scale). That scale is computed dynamically (no static global), fullscreen ≈ displayH/480 ≈ 3.

## Fix direction (compatible with DM)
Make the game build this effect at the **windowed (1×) scale** while DM keeps its native backbuffer and does
its grab+upscale — exactly WindowResizer's philosophy (force the game to render 640 while presenting native).
The chibi/shadow already do this; only the effect layer reads the fullscreen scale. Find the fullscreen
multiplier feeding `obj+0xB0` (disassemble `0x477950`/`0x470511`) and force it to the windowed value, or patch
the code so the effect scale matches the 640 render.

## Instruction-level mechanism (2026-09-24, via HW write-breakpoint on the effect object)
The cloak effect object's screen-quad corners live at `obj+0xB0..0xDC` (obj = the DrawPrimitiveUP vertex
pointer − 8; `0x4075D0` draws verts = obj+8). The base LOCAL quad at `obj+0x80..0xAC` is the CORRECT 470×270
(e.g. (-235,-270)..(235,0)). Per-frame transform chain that fills obj+0xB0 (found by a debug-register write
BP on obj+0xB0, logging each writer EIP + stack args — see harness `watchwrite`):
1. `0x406EB3` — `rep movsd` copies obj+0x80 (local 470×270) → obj+0xB0.
2. `0x406FB2` (in `0x406FA0`, called from `0x4778BA` in the effect loop) — multiplies the X corners by
   arg = `[0x871538]` = **3.0**. (`0x406FA0/0x407040/0x4070E0` = scale X/Y/Z by their float arg.)
3. `0x406EF2` (in `0x406EE0`, translate) from `0x4778F9` — adds (0,-100).
4. `0x406EF2` from `0x42949F` — adds the camera-projected position (e.g. +336,+456). (`0x429440` = camera
   project; game camera obj = `0x898600`.)
5. `0x406FB2` from `0x4294AD` (inside 0x429440) — multiplies by the **camera zoom** (fullscreen ≈ 0.916).
Net size ≈ base × 3.0 × cameraZoom ≈ 2.8× = the giant. `0x871538` is an **RDATA const 3.0** (also used by
the sky — poking it shrinks the sky), so windowed (cloak=1×) must NOT reach this 3.0-scaled effect path, i.e.
this whole path is **fullscreen-only** and DM's fullscreen state triggers it. `0x8998B0` (fullscreen flag)
poked to 0 did NOT disable it, so the path is gated by a different indicator up the chain (0x481AC8/0x41E157).

**OPEN (resume here):** poking `0x871538`→1.0 makes the 0x4778BA factor 1.0 live (confirmed) but the sprite
stayed giant and it's multi-object (cloak = several layers: 470×270 + a 320×320 aura, different obj addrs) —
so the net giant scale is NOT solely 0x871538. The fullscreen scaling likely rides the **camera zoom**
(0x429440, ≈0.916 fs vs a smaller windowed value) and/or a fullscreen render-path branch. NEXT: disassemble
`0x429440` (camera project/zoom) and the dispatch at `0x481AC8`/`0x41E157` to find the fullscreen scale/branch
that DM must neutralize so effects render at the 640/character scale (then DM's grab+upscale is consistent).
Understand how the game renders 640 under DM while effects take the fullscreen path — that mismatch is the bug.

## FIX PROGRESS (2026-09-24) — mechanism proven, partial fix working
The effect loop at `0x477880` scales each effect's working rect by `[0x871538]`=3.0 via THREE calls
(scale X `0x406FA0`, Y `0x407040`, Z `0x4070E0`), each preceded by `fld dword [0x871538]` at **`0x4778A9`,
`0x4778BA`, `0x4778CB`**. **Patching all three `fld [0x871538]` (6 bytes `D9 05 38 15 87 00`) → `fld1`+nop×4
(`D9 E8 90 90 90 90`)** makes those scales 1.0 and **correctly shrinks the 470×270 cape layer** (verts went
from ~1320×760 to ~441×250; it drops out of the oversized-draw log). Camera zoom (`0x429440`, ~0.916 fs)
stays, giving the correct ~1× net. This is the DM-applied patch shape (force the fullscreen effect scale to 1
so effects render at the 640/character scale; DM then grabs+upscales). Sky uses `0x871538` at a DIFFERENT
site, so patching only these 3 code sites leaves the sky correct (unlike poking the shared const).

**REMAINING:** Okuu's giant is composed of MULTIPLE layers; the cape (470×270, via the loop above) is fixed,
but the main body layer is still drawn giant via another path/scale site (it tracks her live, so it's not
stale). It wasn't logged after the patch only because the diag big-draw rate-limit (`g_diagDrawCount<60`)
filled up with the mod post-process (tex 640×480) + fill (tex 0) draws before the character draw. NEXT:
raise/refocus the diag capacity (exclude tex==640×480 and tex==0 from the cap; bump the limit) to log the
remaining giant draw(s), get their `watchwrite obj+0xB0` scale sites, and patch those `fld [0x871538]`
(or equivalent) the same way. Then verify the whole Okuu renders at the correct size, and confirm no other
character/effect is broken by the patch. Finally implement in DM (apply the byte patches at init, gated to
the effect-scale sites) — this is fully compatible with DM's model.

## REFRAME (2026-09-24 late) — `0x871538` is the game-wide fullscreen scale (~100+ refs)
`fld/fmul [0x871538]` appears at 100+ sites across `.text` (byte pattern `?? 05 38 15 87 00`, opcode D8-DF) —
it's a **read-only const 3.0** used as the game's global fullscreen sprite/effect scale, NOT specific to Okuu.
So the per-site `fld→fld1` patch (0x4778A9/BA/CB) only fixed ONE of many effect layers; Okuu's giant also uses
a SECOND path (`438dd4 → 41fca1 → 59bde8`, the square auras/glows) and there are many more sites. Patching all
is not viable, and it's a shared const (poking it breaks the sky).

Because 0x871538 is a const 3.0 (same in windowed & fullscreen) yet **windowed renders fine (1×)**, windowed
must NOT execute these 3×-scaled effect paths — i.e. there is a **fullscreen-vs-windowed render-path branch**
that selects the 3×-scaled effect rendering only in fullscreen. It is NOT gated by the fullscreen flag
`0x8998B0` (poked to 0, no effect). The single clean fix = find that selector (disassemble the render dispatch
`0x41E157` / `0x481AC8` / `0x47A9D1` up from the effect draws) and make the game take the WINDOWED effect path
while DM presents fullscreen (like WindowResizer forcing the game to render as if windowed). That fixes ALL
effect layers at once, with no per-site patching and without touching the sky.

**Next step:** `disasm.py d 0x41E157` and `d 0x481AC8` (and refs) to locate the `if (fullscreen-ish) {3x effect
render} else {1x}` branch and the exact flag/field it tests; force it to the windowed path in DM. Then verify
Okuu (all layers) + other characters + sky all render correctly under DM fullscreen.

## How it was investigated
`tools/SokuHarness/` (inject DLL + `soku.ps1` + `goto_okuu.ps1`): drove the game to an Okuu practice match,
hooked the device vtable (re-asserting each frame — other mods re-patch the shared slots), auto-logged
oversized DPUP draws with vertex bbox + texture + a manual stack scan (RtlCaptureStackBackTrace fails on
th123's FPO code), and skip/poke commands to test hypotheses live. Disassembly via `tools/disasm.py`.
