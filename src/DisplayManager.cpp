// DisplayManager - crisp, integer-scaled exclusive fullscreen for Touhou Hisoutensoku (th123 1.10a)
//
// Problem this solves
// -------------------
// With WindowResizer off, the base game's fullscreen switches to a mode that fills the monitor height at
// 4:3, so 480 logical pixels get a non-integer scale (e.g. 1080/480 = 2.25x) and look blurry. This mod
// keeps the desktop at its native resolution and renders the game centered with borders: FitToScreen
// (default), IntegerScaling or CustomResolution (Mode in the ini, Alt+0..6), with a Sharp (tunable
// sharp-bilinear, default), Point or Linear filter. True exclusive fullscreen keeps the low-latency
// "Independent Flip" present path, which a legacy D3D9 / DISCARD game can't get in a borderless window
// (an optional borderless mode exists anyway). Windowed, it sizes the game window (4:3 drag-resize,
// Alt+1..6 scale, spawn position, always-on-top) and, with WindowedFilter=1, upscales with the same filters.
//
// How it works
// ------------
// The game builds one global D3DPRESENT_PARAMETERS (0x8A0F68) and creates a plain Direct3D9 device
// (Direct3DCreate9, not Ex; SwapEffect DISCARD). Its fullscreen state is literally that struct's
// Windowed == 0: Alt+Enter (0x4082DE -> 0x415220) just flips Windowed and calls Reset with the struct.
// (0x8998B0 is only the saved "start fullscreen" config flag: stored from Windowed at exit, 0x4405AF, and
// read at startup, 0x442EC7, to send an Alt+Enter.) Crucially, the game always draws its 640x480 surface
// at 1:1 into the top-left of the backbuffer and relies on the fullscreen *display mode* to upscale the
// whole framebuffer - it does not scale its scene to the backbuffer. So we:
//   1. Intercept Direct3DCreate9 (IAT thunk at 0x8572A0) -> hook IDirect3D9::CreateDevice.
//   2. In CreateDevice/Reset, when the game asks for fullscreen (Windowed == FALSE), force the
//      backbuffer to the *native* desktop mode so the monitor never rescales (borderless: a windowed
//      device at native size instead). These changes go into a COPY of the game's struct (for Reset: put
//      into the struct only while the game's Reset wrapper runs, then the game's values go back), so the
//      game (and other mods) always see its real windowed/fullscreen state. Windowed requests pass
//      through untouched. Resets are intercepted at the wrapper's entry, and DM's own D3DPOOL_DEFAULT
//      targets are released/recreated by the wrapper as one of the game's resource owners - see
//      "device Reset" for why a Reset vtable hook alone is not enough.
//   3. Hook the swapchain's Present: grab the 640x480 frame from the backbuffer's top-left into a
//      render-target texture, fill the borders and upscale it centered into the backbuffer (Sharp =
//      sharp-bilinear pixel shader quad, falling back to StretchRect; Point/Linear = StretchRect). The
//      viewport is pinned to 640x480 (the game relies on the default one).
// Windowed (WindowedFilter=1), the copy gets a backbuffer the size of the window's client area and the same
// Present post-process runs, so the filters apply there too (WindowedFilter=0: the device is left exactly as
// the game made it and D3D9's windowed present stretches the 640x480 backbuffer bilinearly). A backbuffer
// can only change size in a Reset, so a window resize ends in one Reset through the game's own wrapper
// (0x415100, what Alt+Enter uses): after a drag (WM_EXITSIZEMOVE) or any other resize, never during a drag.
// Either way Alt+Enter still toggles windowed <-> crisp fullscreen.
//
// If WindowResizer, IntegerFullscreen or ExclusiveFullscreen is loaded, DM detects it at device creation
// and passes everything through.
//
// Self-contained: Windows SDK headers and Dear ImGui (third_party/imgui, compiled in, for the settings menu in
// DisplayManagerMenu.h), no d3d9.lib import (Direct3DCreate9 is hooked via the IAT).

#include <windows.h>
#include <Shlwapi.h>
#include <imm.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include "imgui.h"                     // the settings menu (DisplayManagerMenu.h)
#include "backends/imgui_impl_dx9.h"
#include "sharpbilinear.h"   // compiled ps_2_0 bytecode: g_sharpBilinearPS[]
#include "xbr.h"             // compiled ps_2_b bytecode: g_xbrPS2b[]
#include "sharpsprite.h"     // compiled ps_2_0 bytecode: g_sharpSpritePS[]
#include "DisplayManagerOverlay.h"
#include "version.h"          // DM_VERSION

// ---- game constants (th123 1.10a, fixed addresses - the game has no ASLR) -------------------------
// Build hash the loader passes to CheckVersion; only this exact build is patched.
static const BYTE TARGET_HASH[16] = {
	0xdf, 0x35, 0xd1, 0xfb, 0xc7, 0xb5, 0x83, 0x31,
	0x7a, 0xda, 0xbe, 0x8c, 0xd9, 0xf5, 0x3b, 0x2e,
};
// IAT slot the game's `call 0x81F6B8` thunk jumps through for Direct3DCreate9.
static const DWORD ADDR_D3DCREATE9_IAT = 0x008572A0;
// The game's IDirect3DDevice9* global. th123 passes its address as CreateDevice's ppReturnedDeviceInterface
// (0x414FB2 / 0x415038 / 0x415059: HAL+HW VP, HAL+SW VP, REF), which is how we tell its call from others.
#define GAME_DEVICE (*reinterpret_cast<IDirect3DDevice9 **>(0x008A0E30))
// The game's implicit swapchain (GetSwapChain(0) right after CreateDevice/Reset, 0x4150BF / 0x4151D3; 0
// while it resets). It presents every frame through this pointer (0x401078 / 0x4081CD).
#define GAME_SWAPCHAIN (*reinterpret_cast<IDirect3DSwapChain9 **>(0x008A0E34))
// D3DDISPLAYMODE the game fetched via GetAdapterDisplayMode at startup (Width+0, Height+4, Refresh+8, Format+0xC).
static const DWORD ADDR_DESKTOP_MODE = 0x008A0FA0;
// DirectInput keyboard setup (0x40D830, called once from the game's init at 0x407A63, after the window exists):
// CreateDevice(GUID_SysKeyboard) -> SetDataFormat(c_dfDIKeyboard) -> SetCooperativeLevel(hwnd, 0x16), whose flags
// are the `push 0x16` (6A 16) at 0x40D8B3. 0x16 = DISCL_NONEXCLUSIVE | DISCL_FOREGROUND | DISCL_NOWINKEY: the
// NOWINKEY bit is what disables the Windows key while the game is in the foreground. (Mouse: flags 6 at 0x40DB6C,
// joypads: 5 at 0x40DDA2 - neither has NOWINKEY.) The keyboard device pointer is stored at 0x8A01A0.
static const DWORD ADDR_KB_COOPLEVEL_PUSH = 0x0040D8B3;
static const DWORD ADDR_KB_DEVICE         = 0x008A01A0;
static const BYTE  DI_NONEXCLUSIVE = 0x02, DI_FOREGROUND = 0x04, DI_NOWINKEY = 0x10;   // dinput.h DISCL_*
// The game's Reset wrapper: bool __cdecl (void), Reset with its global present params (0x8A0F68). It first sets
// the struct's BackBufferFormat from its Windowed (0x415132: windowed = the desktop format, fullscreen =
// X8R8G8B8). Under the render lock (0x8A0E14) it calls every registered D3DPOOL_DEFAULT owner's "lost" slot
// (list 0x8A0FC0), releases the swapchain 0x8A0E34, calls Reset (0x4151A8), and on success gets the swapchain
// again and calls the owners' "reset" slot. Returns false without doing anything while the device is lost
// (0x8A0FB4 == D3DERR_DEVICELOST) or before the device exists. Called by Alt+Enter (0x415220, from the window
// procedure) and device-lost recovery (0x407D82). On a FAILED Reset it returns without recreating (KNOWN-BUGS).
// Every Reset the game does goes through here, so DM hooks its entry (see installGameResetHook).
static const DWORD ADDR_GAME_RESET = 0x00415100;
typedef bool (__cdecl *GameReset_t)();
// Its first 6 bytes (push ebp / mov ebp,esp / and esp,-8): position-independent, so they can run from a trampoline.
static const BYTE  GAME_RESET_PROLOGUE[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 };
#define GAME_PP (reinterpret_cast<D3DPRESENT_PARAMETERS *>(0x008A0F68))   // the game's present params
// The game's render lock: the Reset wrapper, BeginScene..EndScene (0x401000 / 0x401040) and Present (0x401060) take it.
#define GAME_RENDER_LOCK (reinterpret_cast<CRITICAL_SECTION *>(0x008A0E14))
static const DWORD ADDR_GAME_D3D = 0x008A0E2C;   // the game's IDirect3D9* (the wrapper's first early-out)
static const DWORD ADDR_GAME_TCL = 0x008A0FB4;   // last TestCooperativeLevel result (its second early-out)
// Register a D3DPOOL_DEFAULT owner in the list the wrapper walks: EAX = 0x8A0E10 (the game's D3D context),
// one stack argument (the owner, ret 4); takes the render lock, ignores duplicates. The owner is a C++ object
// whose vtable slot 0 is "device lost" and slot 1 "device reset" (thiscall, no arguments). The game's own
// effects (CBaseEffect, vtable 0x871334) register the same way (0x418190).
static const DWORD ADDR_ADD_DEVICE_LISTENER = 0x004153A0;

// ---- vtable indices (verified against d3d9.h) ----------------------------------------------------
static const int VT_D3D9_CREATEDEVICE = 16;   // IDirect3D9::CreateDevice        (+0x40)
static const int VT_DEV_RESET         = 16;   // IDirect3DDevice9::Reset          (+0x40)
static const int VT_DEV_PRESENT       = 17;   // IDirect3DDevice9::Present         (+0x44)
static const int VT_DEV_SETRT         = 37;   // IDirect3DDevice9::SetRenderTarget (+0x94)
static const int VT_DEV_BEGINSCENE    = 41;   // IDirect3DDevice9::BeginScene      (+0xA4)
static const int VT_DEV_ENDSCENE      = 42;   // IDirect3DDevice9::EndScene        (+0xA8)
static const int VT_DEV_DRAWPRIMUP    = 83;   // IDirect3DDevice9::DrawPrimitiveUP (+0x14C)
static const int VT_UNK_QUERYINTERFACE = 0;   // IUnknown::QueryInterface          (+0x00)
static const int VT_SC_PRESENT        = 3;    // IDirect3DSwapChain9::Present     (+0x0C)
// NOTE: the game presents via the SWAPCHAIN (0x8A0E34)->Present, not the device, so we hook that.

// ---- config --------------------------------------------------------------------------------------
enum Mode { MODE_FIT = 0, MODE_INTEGER = 1, MODE_CUSTOM = 2 };

static HMODULE g_module;
static char    g_iniPath[1024 + MAX_PATH];
static bool    g_enabled   = true;
static int     g_mode      = MODE_FIT; // FitToScreen (default) / IntegerScaling / CustomResolution
static int     g_intScale  = 2;        // used when g_mode == MODE_INTEGER (x1, x2, ...)
static int     g_winScale  = 2;        // windowed client size = 640x480 x this (separate from the fullscreen mode)
static int     g_customW   = 1280;     // used when g_mode == MODE_CUSTOM
static int     g_customH   = 960;
static int     g_scaleW    = 1280;     // resolved output width  (computed from mode + native res)
static int     g_scaleH    = 960;      // resolved output height
static int     g_srcW      = 640;      // th123's fixed render size (grab region / pinned viewport); const
static int     g_srcH      = 480;      // - th123 always renders 640x480, so this is not configurable
static DWORD   g_filter    = D3DTEXF_POINT;  // resolved upscale filter for this frame
// FILTER_AUTO: Sharp plus sharp characters and stage, all at the values for the output scale (SCALE_DEFAULTS).
enum { FILTER_POINT, FILTER_LINEAR, FILTER_SHARP, FILTER_XBR, FILTER_AUTO, FILTER_COUNT };
static int     g_filterCfg = FILTER_POINT;
// xBR knobs (shader/xbr.hlsl c1), live-cycled by the Xbr* hotkeys.
static float   g_xbrStrength = 0.65f;  // 0..1 blend: plain texel .. full xBR (full xBR looked too strong)
static int     g_xbrCorner   = 1;      // corner type 0..3 = A..D
static bool    g_xbrSlopes   = false;  // also smooth 30/60-degree edges (xBR level 2); off looked better at x3
static float   g_xbrWidth    = 2.0f;   // edge anti-aliasing band, x the original (2 = softer; picked at x3)
// Sharp sprites (experimental): the game's POINT-sampled character sprites / stage tiles drawn through a
// sharp-bilinear shader (see "sharp sprites"). Per layer: the live sharpness k (0 = off) and the k a toggle hotkey
// turns back on (the ini value, else the output scale's default).
enum { SPR_CHARS = 0, SPR_STAGE = 1, SPR_LAYERS };
static volatile float g_sprK[SPR_LAYERS]    = { 0.0f, 0.0f };
static float          g_sprLastK[SPR_LAYERS] = { 1.5f, 1.75f };
static bool           g_sprLastKSet[SPR_LAYERS] = { false, false };   // g_sprLastK came from the ini or a hotkey
static const float    SPR_K_MIN = 0.5f, SPR_K_MAX = 16.0f;
// Filter strength, 0 = the game's own POINT draw, 1 = the full filter, in between a mix. See "sharp sprites".
// Stage: g_sprRest at a whole-number scale (its resting zoom, x1; BackgroundRestStrength), else 1.
// Characters: by the camera's zoom (how far apart they are) - g_sprRest (SpriteNearStrength) at the resting zoom 1.0,
// g_sprFar (SpriteFarStrength) at g_sprFarZoom (SpriteFarZoom) and beyond, linear in between.
static volatile float g_sprRest[SPR_LAYERS] = { 0.0f, 0.5f };
static volatile float g_sprFar     = 1.0f;
static volatile float g_sprFarZoom = 0.5f;   // the game's camera zooms between 1.0 (close) and 0.5 (farthest)
static const float    SPR_FARZOOM_MIN = 0.5f, SPR_FARZOOM_MAX = 0.95f;
static const float    SPR_WHOLE_FADE = 0.1f;   // characters: scale distance from a whole number where the strength fades
static bool           g_sprWanted = false;   // sharp sprites can be turned on (ini, Filter=Auto, a hotkey): hook
static bool           g_sprFailed = false;   // CreatePixelShader failed: off for the session

// Per-output-scale defaults: the values picked by eye at x2, x2.25 and x3 (docs/calibration). A setting left at Auto
// takes the column of the nearest of those scales (x2's below x2, x3's above x3), and Filter=Auto is the Sharp
// filter plus sharp characters and stage, all from that column. resolveSettings applies it.
struct ScaleDefaults {
	float scale, sharpness, xbrStrength; int xbrCorner; bool xbrSlopes; float xbrWidth, sprK, bgK, sprNear, sprFar,
	      farZoom, bgRest;
};
static const ScaleDefaults SCALE_DEFAULTS[] = {
	//  scale  Sharp  xBR str corner slopes width   chars  stage  near  far   far at bg rest
	{ 2.0f,  2.00f, 0.65f,  1,     false, 2.00f,  1.50f, 1.75f, 0.0f, 1.0f, 0.70f, 0.30f },
	{ 2.25f, 4.00f, 0.50f,  2,     false, 0.50f,  1.50f, 4.00f, 0.0f, 1.0f, 0.65f, 0.50f },
	{ 3.0f,  2.50f, 0.80f,  2,     false, 0.75f,  1.75f, 2.50f, 0.0f, 1.0f, 0.70f, 0.30f },
};
// Settings that follow SCALE_DEFAULTS (ini value Auto, blank or missing) until a hotkey changes them.
enum { AUTO_SHARP, AUTO_XBR_STRENGTH, AUTO_XBR_CORNER, AUTO_XBR_SLOPES, AUTO_XBR_WIDTH, AUTO_FARZOOM, AUTO_BGREST,
       AUTO_COUNT };
static bool  g_auto[AUTO_COUNT] = { true, true, true, true, true, true, true };
// The chosen (ini / hotkey) values of the settings Filter=Auto overrides; the live ones (g_sharpness, g_sprK,
// g_sprRest, g_sprFar, g_sprFarZoom) are resolved from them.
static float g_cfgSharp = 2.0f, g_cfgFarZoom = 0.7f, g_cfgBgRest = 0.3f, g_cfgSprK[SPR_LAYERS] = { 0.0f, 0.0f };
static float g_cfgSprNear = 0.0f, g_cfgSprFar = 1.0f;
static D3DCOLOR g_bgColor  = D3DCOLOR_XRGB(0, 0, 0);  // fullscreen border/letterbox color
static bool    g_resizable = true;     // add a drag-resize border to the window (hotkeys work regardless)
static bool    g_persist   = true;     // save the current scaling settings to the ini on exit
static bool    g_persistPos = true;    // save the window's position to the ini on exit (next spawn position)
static bool    g_havePos   = false;    // PositionX/Y set (not blank): move the window there on spawn
static int     g_posX      = -1;       // spawn position (negative values are valid: monitors left of / above
static int     g_posY      = -1;       // the primary, or a window flush with an edge)
static bool    g_borderless = false;   // fullscreen as a borderless window instead of exclusive (higher latency)
static bool    g_winFilter = true;     // WindowedFilter: windowed, a window-sized backbuffer + our upscale
static int     g_fsW       = 0;        // manual fullscreen display-mode override (0 = auto / native)
static int     g_fsH       = 0;
static int     g_fsRefresh = 0;        // manual refresh override (0 = keep the native refresh)
static int     g_vsync     = -1;       // exclusive PresentationInterval: -1 = the game's own, 0 = immediate, 1 = vsync
static bool    g_dpiAware  = true;    // [Display] DpiAware: declare per-monitor DPI awareness (see "DPI awareness")
static bool    g_allowWinKey = false;  // [Input] AllowWinKey: drop the game's DISCL_NOWINKEY (vanilla blocks the Win key)
static bool    g_latinPending = false; // [Input] StartInLatinInput, not done yet (see startInLatinInput)
static volatile int g_msaaCfg = 0;     // MultiSample: requested MSAA sample count (0 = off); ToggleMSAA flips it
static int     g_msaaIni   = 0;        // MultiSample as read from the ini (what ToggleMSAA turns on, 8 if it's 0)
static volatile bool g_msaaApply = false;   // ToggleMSAA pressed: the render thread recreates the MSAA targets
static bool    g_log       = false;
static FILE   *g_logFile   = nullptr;

// Hotkeys: the configured modifier + a per-action key. VK code 0 = that hotkey is disabled (which is
// also what a commented-out / missing ini line produces).
enum Action { ACT_FIT = 0, ACT_S1, ACT_S2, ACT_S3, ACT_S4, ACT_S5, ACT_S6, ACT_TOP, ACT_FILTER,
              ACT_SHARP_DOWN, ACT_SHARP_UP, ACT_MSAA, ACT_XBR_STRENGTH, ACT_XBR_CORNER, ACT_XBR_SLOPES,
              ACT_XBR_WIDTH, ACT_SPR_TOGGLE, ACT_SPR_DOWN, ACT_SPR_UP, ACT_BG_TOGGLE, ACT_BG_DOWN, ACT_BG_UP,
              ACT_SPR_NEAR_DOWN, ACT_SPR_NEAR_UP, ACT_BG_REST_DOWN, ACT_BG_REST_UP, ACT_SPR_FAR_DOWN, ACT_SPR_FAR_UP,
              ACT_SPR_FARZOOM_DOWN, ACT_SPR_FARZOOM_UP, ACT_MENU, ACT_COUNT };
enum ModKey { MODK_ALT = 0, MODK_CTRL, MODK_SHIFT, MODK_WIN, MODK_NONE }; // MOD_* are taken by winuser.h
static int g_hotkeyVk[ACT_COUNT];      // filled by loadConfig
static int g_modifier = MODK_ALT;

// The in-game settings menu (Dear ImGui): DisplayManagerMenu.h, included after doAction. Declared here for the code
// that comes before it (the Present hook, releaseCapture, wndProc, persistState).
static volatile bool g_menuOpen  = false;   // shown (window thread toggles it, the render thread draws it)
static bool          g_menuTouched = false; // the menu changed something: persistState also writes what it sets
static float         g_menuScaleCfg = 0.0f; // [Menu] Scale / the menu's size slider; 0 = Auto (by screen height)
static UINT          g_menuMsg   = 0;       // private registered message: a menu command for the window thread
static bool menuShown();
static void menuToggle();
static bool menuWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, LRESULT *res);
static void menuCommand(int cmd, LPARAM value);
static void menuRender(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb);
static void menuInvalidate();
static void menuPersist();
static void persistSettings();

// ---- runtime state -------------------------------------------------------------------------------
static volatile bool g_createDeviceHooked = false;   // (volatile: polled by the device-watch thread)
static volatile bool g_deviceHooked       = false;
static bool      g_active             = false;  // forcing our backbuffer + post-process (fullscreen or WindowedFilter)?
static bool      g_winActive          = false;  // ...on a real windowed device (WindowedFilter, not borderless)
static bool      g_devWindowed        = false;  // the device we forced is windowed (borderless or WindowedFilter)
static bool      g_winFilterFailed    = false;  // a forced windowed Reset failed: WindowedFilter off for this session
static UINT      g_bbW = 0, g_bbH = 0;          // forced backbuffer size (native desktop; windowed: the client area)
static D3DFORMAT g_bbFormat = D3DFMT_X8R8G8B8;  // backbuffer format (for the capture RT)
// Grabbed game frame: a render-target TEXTURE (so the Sharp shader can sample it) plus its level-0
// surface (the StretchRect grab target, and the source for Point/Linear StretchRect upscales).
static IDirect3DTexture9      *g_captureTex  = nullptr;
static IDirect3DSurface9      *g_captureSurf = nullptr;
// Offscreen backbuffer-sized render target the upscale is composed into, then copied 1:1 to the backbuffer.
// Only used when we could NOT get the runtime's own BeginScene/EndScene (g_sceneDirect false): then our
// Begin/EndScene go through other mods' vtable hooks, and the redraw some of them do there (PracticeEx's
// 640x480 menu) must be kept off the upscaled frame. See drawSharp.
static IDirect3DSurface9      *g_stageSurf   = nullptr;
// Tiny render target bound while our own BeginScene/EndScene run (drawShaderQuad). PracticeEx hooks the D3D runtime's
// EndScene CODE with Detours (whatever function the vtable slot held when it set up - the runtime's own, unless
// another mod had hooked the slot first), so even the direct calls above run it, and it draws its 640x480 menu into
// the bound target: on our finished frame that was the small menu copy in the top-left (KNOWN-BUGS Bug 11).
static IDirect3DSurface9      *g_sceneGuard  = nullptr;
// The D3D runtime's own BeginScene/EndScene, read from the device vtable right after CreateDevice (before
// other mods patch the shared slots). Calling them directly means our Sharp pass does not re-fire every other
// mod's per-scene hook once more per frame (double work, double-ticked mod logic, PracticeEx's menu dupe).
typedef HRESULT (WINAPI *Scene_t)(IDirect3DDevice9 *);
static Scene_t g_origBeginScene = nullptr;
static Scene_t g_origEndScene   = nullptr;
static bool    g_sceneDirect    = false;   // true when the two pointers above are trusted to be the runtime's
static IDirect3DPixelShader9  *g_ps          = nullptr;  // sharp-bilinear upscale shader (Filter=Sharp)
static IDirect3DPixelShader9  *g_psXbr       = nullptr;  // xBR-lv2 pixel-art upscale shader (Filter=xBR)
static IDirect3DStateBlock9   *g_stateBlock  = nullptr;  // save/restore device state around the shader draw
// The game swapchain's backbuffer surface, for identity checks in the SetRenderTarget hook only. NOT
// AddRef'd (the swapchain owns it); cached after CreateDevice/Reset and each Present, dropped before Reset.
static IDirect3DSurface9      *g_bbSurf      = nullptr;
static bool                    g_inPost      = false;    // inside our Present post-process (render thread)
// MSAA (MultiSample=N, whenever DM composites - fullscreen, or windowed with WindowedFilter): the game draws into
// our own multisampled 640x480 target (+ depth) instead of the backbuffer, and the Present grab resolves it. The
// backbuffer stays single-sampled, so the rest of the post-process and other mods' overlays work on it unchanged.
// The game never calls SetRenderTarget itself, so binding ours after CreateDevice/Reset and after every Present is
// enough.
static IDirect3DSurface9      *g_msRT        = nullptr;
static IDirect3DSurface9      *g_msDS        = nullptr;
static IDirect3DSurface9      *g_autoDS      = nullptr;  // the device's own depth buffer (not AddRef'd), rebound before Reset
static bool                    g_gameDepth   = false;    // the game asked for an auto depth-stencil
static D3DFORMAT               g_gameDepthFmt = D3DFMT_UNKNOWN;
static D3DMULTISAMPLE_TYPE     g_msType      = D3DMULTISAMPLE_NONE;   // what we actually got
static float                   g_sharpness   = 1.50f;    // 1 = aligned bilinear; higher = crisper toward point
static const float             SHARP_MIN     = 1.0f;     // clamp: 1.0 = bilinear
static const float             SHARP_MAX     = 4.0f;     // clamp: ~4.0 is already visually point (shader
                                                         // interp band = 0.5/sharp), so no point going higher
static volatile DWORD          g_osdUntil = 0;          // GetTickCount() deadline for the OSD toast
static char                    g_osdText[32] = {0};     // current OSD message (uppercase; see OSD_CHARS)
static HWND g_hwnd = nullptr;   // the game's window (from present params), for windowed resizing
// Monitor of the adapter the game's device was created on. Exclusive fullscreen always takes over THIS
// monitor (the adapter is fixed at CreateDevice), wherever the window has been dragged.
static HMONITOR g_adapterMon = nullptr;
// The game's IDirect3D9 and the adapter its device was created on, for the exclusive-mode query/validation
// (GetAdapterDisplayMode / EnumAdapterModes). Not AddRef'd: the game keeps it alive (0x8A0E2C), and so
// does its device.
static IDirect3D9 *g_d3d = nullptr;
static UINT g_adapter = D3DADAPTER_DEFAULT;
static bool g_topmost = false;          // always-on-top toggle (Alt+P)
// Borderless-mode state: g_wantFullscreen tracks the game's real intent (its pp.Windowed, which we never
// modify - we only override our own copy) so we can tell a forced-windowed borderless-fullscreen apart
// from a genuine windowed request. Saved styles restore the normal window when leaving borderless.
static bool g_wantFullscreen = false;
static bool g_borderlessActive = false;
static bool g_styleSaved = false;
static LONG g_savedStyle = 0, g_savedExStyle = 0;
// Window state across a windowed <-> fullscreen switch. g_windowFs = the state the window was last set up
// for (a Reset that doesn't change it, e.g. device-lost recovery, leaves the window alone). When leaving
// windowed we remember where the window was (the game's own post-toggle SetWindowPos re-centers it on the
// primary monitor) and which monitor it was on (borderless covers that one - by the time we apply it the
// game has already moved the window to the primary's origin).
static bool     g_windowFs   = false;
static bool     g_haveWinPos = false;
static POINT    g_winPos     = { 0, 0 };
static HMONITOR g_fsMon      = nullptr;
static UINT     g_applyMsg   = 0;       // private registered message: apply the window state (wndProc)
static bool     g_spawnPending = false; // first-spawn window setup posted but not applied yet
static volatile bool g_applyPending = false; // a posted window-state apply hasn't run yet
static UINT     g_resizeMsg  = 0;       // private registered message: resize the windowed backbuffer (wndProc)
static bool     g_inSizeMove = false;   // inside a drag-resize / move loop (WM_ENTERSIZEMOVE..WM_EXITSIZEMOVE)
// PersistPosition: the window's last top-left while it was a normal window (windowed, not minimized / maximized, not in
// a fullscreen switch or with our window setup still queued), so exiting from fullscreen or minimized saves where the
// window really was rather than where the game or D3D9 parked it.
static bool     g_haveLastPos = false;
static POINT    g_lastPos     = { 0, 0 };

static void logf(const char *fmt, ...) {
	if (!g_log) return;
	if (!g_logFile) {
		char path[1024 + MAX_PATH];
		lstrcpynA(path, g_iniPath, sizeof(path));
		PathRemoveExtensionA(path);
		lstrcatA(path, ".log");
		g_logFile = fopen(path, "w");
		if (!g_logFile) { g_log = false; return; }
	}
	va_list ap; va_start(ap, fmt);
	vfprintf(g_logFile, fmt, ap);
	va_end(ap);
	fputc('\n', g_logFile);
	fflush(g_logFile);
}

// ---- DPI awareness ([Display] DpiAware) ---------------------------------------------------------------
// th123 (2008) declares no DPI awareness, so with display scaling Windows stretches its window as a bitmap (blurry at
// 125% / 150%, on top of DM's own filter), or - with the "Override high DPI scaling behavior: Application"
// compatibility setting players use against that - still on every monitor whose scale differs from the main one's.
// DpiAware=1 (default) makes the game per-monitor DPI aware (v2) before it creates its window: window sizes are real
// pixels on every monitor, and DM's filter alone decides the look. When the process mode is already fixed (that
// compatibility setting, a manifest, another mod), the thread mode is used instead: a window takes its DPI mode from
// the thread that creates it - the game's main thread, which runs Initialize - and DM's other threads that measure
// monitors (the render thread with SokuDirectXOptimizations, the device watch) switch to it too (dpiThread).
static const HANDLE DPI_PER_MONITOR_V2 = (HANDLE)(LONG_PTR)-4;   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
typedef BOOL (WINAPI *SetProcessDpiAwarenessContext_t)(HANDLE);
typedef HANDLE (WINAPI *SetThreadDpiAwarenessContext_t)(HANDLE);
typedef UINT (WINAPI *GetDpiForWindow_t)(HWND);
typedef BOOL (WINAPI *AdjustWindowRectExForDpi_t)(LPRECT, DWORD, BOOL, DWORD, UINT);
static SetThreadDpiAwarenessContext_t pSetThreadDpiAwarenessContext = nullptr;
static GetDpiForWindow_t              pGetDpiForWindow = nullptr;
static AdjustWindowRectExForDpi_t     pAdjustWindowRectExForDpi = nullptr;
static bool g_dpiOn = false;            // per-monitor v2 is in effect for the game window
static bool g_dpiThreadMode = false;    // ...through the thread mode: DM's other threads set it too

static void dpiThread() {
	if (g_dpiThreadMode) pSetThreadDpiAwarenessContext(DPI_PER_MONITOR_V2);
}

static void applyDpiAwareness() {
	HMODULE u = GetModuleHandleA("user32.dll");
	pSetThreadDpiAwarenessContext = (SetThreadDpiAwarenessContext_t)GetProcAddress(u, "SetThreadDpiAwarenessContext");
	pGetDpiForWindow = (GetDpiForWindow_t)GetProcAddress(u, "GetDpiForWindow");
	pAdjustWindowRectExForDpi = (AdjustWindowRectExForDpi_t)GetProcAddress(u, "AdjustWindowRectExForDpi");
	auto setProcess = (SetProcessDpiAwarenessContext_t)GetProcAddress(u, "SetProcessDpiAwarenessContext");
	if (setProcess && pSetThreadDpiAwarenessContext && pGetDpiForWindow && pAdjustWindowRectExForDpi) {   // Windows 10 1703+
		if (setProcess(DPI_PER_MONITOR_V2)) {
			g_dpiOn = true;
			logf("DPI: per-monitor aware (v2)");
			return;
		}
		DWORD err = GetLastError();
		g_dpiOn = g_dpiThreadMode = pSetThreadDpiAwarenessContext(DPI_PER_MONITOR_V2) != nullptr;
		logf("DPI: the process mode is already set (error %lu: the compatibility setting, a manifest or another mod) - "
		     "%s", err, g_dpiOn ? "per-monitor aware (v2) through the thread mode" : "thread mode FAILED, left as it is");
		return;
	}
	// Older Windows: per-monitor without frame scaling (8.1), else system aware (Vista / 7).
	typedef HRESULT (WINAPI *SetProcessDpiAwareness_t)(int);
	HMODULE shcore = LoadLibraryA("shcore.dll");
	auto setAwareness = shcore ? (SetProcessDpiAwareness_t)GetProcAddress(shcore, "SetProcessDpiAwareness") : nullptr;
	if (setAwareness) logf("DPI: per-monitor aware (Windows 8.1 mode): 0x%08lx", (long)setAwareness(2));
	else              logf("DPI: system aware: %d", SetProcessDPIAware());
}

// ini names of the current Mode / Filter.
static const char *modeName() {
	return g_mode == MODE_INTEGER ? "IntegerScaling" : g_mode == MODE_CUSTOM ? "CustomResolution" : "FitToScreen";
}
static const char *filterName() {
	return g_filterCfg == FILTER_POINT ? "Point" : g_filterCfg == FILTER_LINEAR ? "Linear" :
	       g_filterCfg == FILTER_XBR ? "xBR" : g_filterCfg == FILTER_AUTO ? "Auto" : "Sharp";
}

// g_sharpness as "X.XX" (wsprintf has no %f).
static void formatHundredths(float v, char *buf) {
	int hundredths = (int)(v * 100.0f + 0.5f);
	wsprintfA(buf, "%d.%02d", hundredths / 100, hundredths % 100);
}

static void formatSharpness(char *buf) {
	int hundredths = (int)(g_sharpness * 100.0f + 0.5f);
	wsprintfA(buf, "%d.%02d", hundredths / 100, hundredths % 100);
}

static void clampSharpness() {
	if (g_sharpness < SHARP_MIN) g_sharpness = SHARP_MIN;
	if (g_sharpness > SHARP_MAX) g_sharpness = SHARP_MAX;
}

// forward declarations
static void installKeyboardHook();
static void installWndProc();
static void setWindowScaled(int n, const POINT *pos);
static void onWindowedEntry(bool firstTime);
static void enterBorderlessFullscreen();
static void postWindowApply(bool firstTime);
static void applyWindowState();
static void applyTopmost();
static void windowedBackbufferSize(bool entering, bool firstTime, UINT *w, UINT *h);
static void healSlots(IDirect3DDevice9 *dev);

// ---- original function pointers ------------------------------------------------------------------
typedef IDirect3D9 * (WINAPI *Direct3DCreate9_t)(UINT);
static Direct3DCreate9_t oDirect3DCreate9 = nullptr;

typedef HRESULT (WINAPI *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                         D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
static CreateDevice_t oCreateDevice = nullptr;

typedef HRESULT (WINAPI *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static Reset_t oReset = nullptr;

// SwapChain::Present has an extra dwFlags parameter compared to Device::Present.
typedef HRESULT (WINAPI *SCPresent_t)(IDirect3DSwapChain9 *, const RECT *, const RECT *, HWND,
                                      const RGNDATA *, DWORD);
static SCPresent_t oSCPresent = nullptr;

typedef HRESULT (WINAPI *SetRenderTarget_t)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
static SetRenderTarget_t oSetRenderTarget = nullptr;

typedef HRESULT (WINAPI *DrawPrimitiveUP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
static DrawPrimitiveUP_t oDrawPrimitiveUP = nullptr;

// Overwrite one vtable slot, storing the previous entry in *orig first (so a call that goes through the
// new slot immediately already finds the original). A single aligned pointer store, safe while the render
// thread may be calling through the table. No-op if the slot already holds our hook: recording our own
// hook as the "original" would make it call itself forever. Made writable keeping it executable: the table may
// share a page with code (DXVK, Wine), and healSlots re-hooks at run time while other threads run.
static void hookSlot(void **vtable, int index, void *hook, void **orig) {
	if (vtable[index] == hook) return;
	DWORD old;
	VirtualProtect(&vtable[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
	*orig = vtable[index];
	vtable[index] = hook;
	VirtualProtect(&vtable[index], sizeof(void *), old, &old);
}

// The SCALE_DEFAULTS column for the current output size: the nearest of its scales.
static const ScaleDefaults &scaleDefaults() {
	float s = min((float)g_scaleW / g_srcW, (float)g_scaleH / g_srcH);
	const int n = sizeof(SCALE_DEFAULTS) / sizeof(SCALE_DEFAULTS[0]);
	int best = 0;
	for (int i = 1; i < n; i++)
		if (fabsf(s - SCALE_DEFAULTS[i].scale) < fabsf(s - SCALE_DEFAULTS[best].scale)) best = i;
	return SCALE_DEFAULTS[best];
}

// Set the live values from the chosen ones: Filter=Auto takes the output scale's column for Sharpness, the sharp
// characters / stage and their far zoom / rest strength; otherwise the chosen values, where the Auto ones (g_auto)
// take the column too. Leaving Filter=Auto brings the chosen values back. Called when the output size, the filter or
// a chosen value changes; a hotkey changed while Filter=Auto changes only the live value (until the next call).
static void resolveSettings() {
	const ScaleDefaults &d = scaleDefaults();
	const bool af = g_filterCfg == FILTER_AUTO;
	g_sharpness             = af || g_auto[AUTO_SHARP]   ? d.sharpness : g_cfgSharp;
	g_sprFarZoom            = af || g_auto[AUTO_FARZOOM] ? d.farZoom   : g_cfgFarZoom;
	g_sprRest[SPR_STAGE]    = af || g_auto[AUTO_BGREST]  ? d.bgRest    : g_cfgBgRest;
	g_sprRest[SPR_CHARS]    = af ? d.sprNear : g_cfgSprNear;
	g_sprFar                = af ? d.sprFar  : g_cfgSprFar;
	g_sprK[SPR_CHARS]       = g_sprFailed ? 0.0f : af ? d.sprK : g_cfgSprK[SPR_CHARS];
	g_sprK[SPR_STAGE]       = g_sprFailed ? 0.0f : af ? d.bgK  : g_cfgSprK[SPR_STAGE];
	if (g_auto[AUTO_XBR_STRENGTH]) g_xbrStrength = d.xbrStrength;
	if (g_auto[AUTO_XBR_CORNER])   g_xbrCorner   = d.xbrCorner;
	if (g_auto[AUTO_XBR_SLOPES])   g_xbrSlopes   = d.xbrSlopes;
	if (g_auto[AUTO_XBR_WIDTH])    g_xbrWidth    = d.xbrWidth;
	if (!g_sprLastKSet[SPR_CHARS]) g_sprLastK[SPR_CHARS] = d.sprK;   // what a toggle hotkey turns on
	if (!g_sprLastKSet[SPR_STAGE]) g_sprLastK[SPR_STAGE] = d.bgK;
}

// Resolve the centered output size (g_scaleW/H) and upscale filter from the current mode and the native
// backbuffer size (g_bbW/g_bbH). Safe to call any time the native size is known (e.g. from a hotkey).
// Windowed, the backbuffer is the (4:3) client area, so the image just fills it: the fullscreen Mode is ignored.
static void computeOutput() {
	if (g_bbW == 0 || g_bbH == 0) return;
	int outW, outH;
	switch (g_winActive ? MODE_FIT : g_mode) {
	case MODE_INTEGER: {
		int n = g_intScale < 1 ? 1 : g_intScale;
		int maxFit = (int)min(g_bbW / (UINT)g_srcW, g_bbH / (UINT)g_srcH);
		if (maxFit < 1) maxFit = 1;
		int use = n > maxFit ? maxFit : n;          // clamp so it never exceeds the screen
		if (use != n)
			logf("FullscreenScale x%d doesn't fit %ux%u; using x%d", n, g_bbW, g_bbH, use);
		outW = g_srcW * use; outH = g_srcH * use;
		break;
	}
	case MODE_CUSTOM:
		outW = g_customW > 0 ? g_customW : g_srcW;
		outH = g_customH > 0 ? g_customH : g_srcH;
		break;
	case MODE_FIT:
	default: {
		// Largest size preserving the source's aspect ratio that fits the screen (fills the height on a
		// wider-than-4:3 monitor).
		double s = min((double)g_bbW / g_srcW, (double)g_bbH / g_srcH);
		outW = (int)(g_srcW * s + 0.5);
		outH = (int)(g_srcH * s + 0.5);
		break;
	}
	}
	if (outW > (int)g_bbW) outW = g_bbW;            // final safety: dst must fit the backbuffer
	if (outH > (int)g_bbH) outH = g_bbH;
	if (outW < 1) outW = 1;
	if (outH < 1) outH = 1;
	g_scaleW = outW; g_scaleH = outH;
	// Sharp, xBR and Auto draw with their shaders; linear is only their StretchRect fallback.
	g_filter = g_filterCfg == FILTER_POINT ? D3DTEXF_POINT : D3DTEXF_LINEAR;
	resolveSettings();
	char sh[16], ch[16], bg[16];
	formatHundredths(g_sharpness, sh); formatHundredths(g_sprK[SPR_CHARS], ch); formatHundredths(g_sprK[SPR_STAGE], bg);
	logf("output -> %dx%d centered at (%d,%d), filter=%s (%s), sharpness=%s sprites=%s/%s (x%d.%02d defaults)",
	     g_scaleW, g_scaleH, ((int)g_bbW - g_scaleW) / 2, ((int)g_bbH - g_scaleH) / 2,
	     g_filter == D3DTEXF_POINT ? "point" : "linear", filterName(), sh, ch, bg,
	     (int)scaleDefaults().scale, (int)(scaleDefaults().scale * 100.0f + 0.5f) % 100);
}

// The native resolution/refresh of the monitor fullscreen will land on, queried live: the game's cached
// GetAdapterDisplayMode global (0x8A0FA0) can be stale or the wrong monitor, which set a wrong (small)
// mode - blurry, and it shuffled the user's windows.
// Exclusive uses the device's ADAPTER monitor (the window's is wrong once it has been dragged to another
// screen) and prefers the runtime's GetAdapterDisplayMode: same refresh rounding as its mode list
// (EnumDisplaySettings can say 59/143 where the list has 60/144), and it works under Wine/DXVK.
// Borderless is a windowed device, so it covers g_fsMon (the window's monitor when fullscreen was requested).
static void nativeMode(UINT *w, UINT *h, UINT *refresh) {
	*w = *h = *refresh = 0;
	D3DDISPLAYMODE am;
	if (!g_borderless && g_d3d && SUCCEEDED(g_d3d->GetAdapterDisplayMode(g_adapter, &am)) &&
	    am.Width && am.Height) {
		*w = am.Width; *h = am.Height; *refresh = am.RefreshRate;
	}
	HMONITOR mon = (!g_borderless && g_adapterMon) ? g_adapterMon
	             : (g_borderless && g_fsMon) ? g_fsMon
	             : g_hwnd ? MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY) : nullptr;
	if (mon && !*w) {
		MONITORINFOEXA mi; mi.cbSize = sizeof(mi);
		if (GetMonitorInfoA(mon, &mi)) {
			DEVMODEA dm; ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
			if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
			    dm.dmPelsWidth && dm.dmPelsHeight) {
				*w = dm.dmPelsWidth; *h = dm.dmPelsHeight; *refresh = dm.dmDisplayFrequency;
			} else {
				*w = mi.rcMonitor.right - mi.rcMonitor.left;
				*h = mi.rcMonitor.bottom - mi.rcMonitor.top;
			}
		}
	}
	if (!*w || !*h) {
		const D3DDISPLAYMODE *d = reinterpret_cast<const D3DDISPLAYMODE *>(ADDR_DESKTOP_MODE);
		*w = d->Width; *h = d->Height; *refresh = d->RefreshRate;
	}
	// Manual FullscreenWidth/Height/Refresh override.
	if (g_fsW > 0 && g_fsH > 0) {
		*w = g_fsW; *h = g_fsH;
		if (g_fsRefresh > 0) *refresh = g_fsRefresh;
	}
}

// Is w x h in the adapter's mode list for `fmt`? If so, *rate = the listed refresh closest to `want`
// (ties: the higher one).
static bool listedMode(D3DFORMAT fmt, UINT w, UINT h, UINT want, UINT *rate) {
	bool found = false; UINT bestDiff = 0xFFFFFFFF;
	UINT n = g_d3d->GetAdapterModeCount(g_adapter, fmt);
	for (UINT i = 0; i < n; i++) {
		D3DDISPLAYMODE m;
		if (FAILED(g_d3d->EnumAdapterModes(g_adapter, fmt, i, &m)) || m.Width != w || m.Height != h) continue;
		UINT diff = m.RefreshRate > want ? m.RefreshRate - want : want - m.RefreshRate;
		if (!found || diff < bestDiff || (diff == bestDiff && m.RefreshRate > *rate)) {
			*rate = m.RefreshRate; bestDiff = diff;
		}
		found = true;
	}
	return found;
}

// Check the forced exclusive mode against the adapter's mode list (EnumAdapterModes) before
// CreateDevice/Reset sees it. A refresh the list doesn't have for that size snaps to the closest listed
// one; a size it doesn't have (e.g. a bad FullscreenWidth/Height override) falls back to the adapter's
// current mode. An empty list (some Wine/DXVK setups) skips the check. Whatever still gets rejected is
// caught by the retry chain in callWithFallback.
static void validateExclusiveMode(UINT *w, UINT *h, UINT *refresh, D3DFORMAT fmt) {
	if (!g_d3d) return;
	if (fmt == D3DFMT_A8R8G8B8) fmt = D3DFMT_X8R8G8B8;      // the mode list takes display formats only
	else if (fmt == D3DFMT_A1R5G5B5) fmt = D3DFMT_X1R5G5B5;
	if (g_d3d->GetAdapterModeCount(g_adapter, fmt) == 0) {
		logf("mode check: the adapter lists no modes for format %d - not checked", (int)fmt);
		return;
	}
	UINT rate = 0;
	if (!listedMode(fmt, *w, *h, *refresh, &rate)) {
		D3DDISPLAYMODE cur;
		if (FAILED(g_d3d->GetAdapterDisplayMode(g_adapter, &cur)) || (cur.Width == *w && cur.Height == *h) ||
		    !listedMode(fmt, cur.Width, cur.Height, cur.RefreshRate, &rate)) {
			logf("mode check: %ux%u is not in the adapter's mode list - trying it anyway", *w, *h);
			return;
		}
		logf("mode check: %ux%u is not in the adapter's mode list - using the current mode %ux%u",
		     *w, *h, cur.Width, cur.Height);
		*w = cur.Width; *h = cur.Height; *refresh = cur.RefreshRate;
	}
	if (*refresh && rate != *refresh) {
		logf("mode check: %ux%u has no %uHz in the mode list - using %uHz", *w, *h, *refresh, rate);
		*refresh = rate;
	} else {
		logf("mode check: %ux%u@%uHz ok", *w, *h, *refresh);
	}
}

// Shape the present parameters for this (Create)Device/Reset: fullscreen requests are forced to the native
// desktop mode so the monitor is never rescaled; windowed requests get a backbuffer the size of the window's
// client area (WindowedFilter=1) or pass through untouched. `pp` is always OUR COPY of the game's struct: the
// global (0x8A0F68) must keep its real values - Alt+Enter just flips its Windowed, the post-toggle window code,
// device-lost recovery and other mods read it back. Writing Windowed=TRUE (borderless) or the native size into
// it made Alt+Enter unable to leave borderless.
// `entering`: the window is about to be set up for windowed (first CreateDevice, or coming back from
// fullscreen), so the windowed size is predicted from the saved WindowScale; otherwise it is the current client
// area. `firstTime`: that setup is the first spawn (spawn position instead of the pre-fullscreen one).
static void applyFullscreenParams(D3DPRESENT_PARAMETERS *pp, bool entering, bool firstTime) {
	g_winActive = false;
	if (!g_enabled || !pp) { g_active = false; return; }
	if (pp->hDeviceWindow) g_hwnd = pp->hDeviceWindow;   // remember the game window for windowed resizing
	g_wantFullscreen = !pp->Windowed;                    // the game's real intent (before we override it)
	g_gameDepth = pp->EnableAutoDepthStencil != FALSE;
	g_gameDepthFmt = pp->AutoDepthStencilFormat;
	D3DFORMAT fmt = (pp->BackBufferFormat != D3DFMT_UNKNOWN) ? pp->BackBufferFormat : D3DFMT_X8R8G8B8;

	if (pp->Windowed) {
		if (!g_winFilter || g_winFilterFailed) {
			g_active = false;
			return;
		}
		// The game keeps drawing its 640x480 frame into the top-left (at least 640x480, see windowedBackbufferSize);
		// the post-process upscales it over the whole backbuffer, which Present then copies 1:1 to the client.
		UINT w = 0, h = 0;
		windowedBackbufferSize(entering, firstTime, &w, &h);
		pp->BackBufferWidth  = w;
		pp->BackBufferHeight = h;
		g_bbW = w; g_bbH = h; g_bbFormat = fmt;
		g_active = g_winActive = g_devWindowed = true;
		logf("windowed -> backbuffer %ux%u (%s)", w, h, entering ? "WindowScale" : "client area");
		computeOutput();
		return;
	}

	UINT w = 0, h = 0, refresh = 0;
	nativeMode(&w, &h, &refresh);
	g_devWindowed = g_borderless;

	if (g_borderless) {
		// Borderless: a windowed device with a native-sized backbuffer; enterBorderlessFullscreen covers
		// the monitor after the reset.
		pp->Windowed = TRUE;
		pp->BackBufferWidth  = w;
		pp->BackBufferHeight = h;
		pp->FullScreen_RefreshRateInHz = 0;
		g_borderlessActive = true;
		logf("borderless fullscreen -> native %ux%u (windowed device)", w, h);
	} else {
		validateExclusiveMode(&w, &h, &refresh, fmt);
		pp->BackBufferWidth  = w;
		pp->BackBufferHeight = h;
		pp->FullScreen_RefreshRateInHz = refresh;
		// th123 presents IMMEDIATE unless SokuDirectXOptimizations patches it (62 fps on 60 Hz: rolling tear).
		if (g_vsync >= 0)
			pp->PresentationInterval = g_vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
		logf("exclusive fullscreen -> native %ux%u @%uHz, present interval 0x%x", w, h, refresh,
		     pp->PresentationInterval);
	}
	g_bbW = w; g_bbH = h; g_bbFormat = fmt; g_active = true;
	computeOutput();
}

// After a (Create)Device/Reset made with our copy `used` of the game's struct `game`, hand back only what
// the runtime itself fills in. An untouched copy (DM inactive) goes back whole, exactly as if the game had
// passed its own struct; for a forced copy (fullscreen, or windowed with our window-sized backbuffer) we never
// write back our size / Windowed overrides - only the defaults the runtime resolves (th123 passes explicit
// values for both, so this is normally a no-op).
static void syncPresentParams(D3DPRESENT_PARAMETERS *game, const D3DPRESENT_PARAMETERS *used) {
	if (!g_active) { *game = *used; return; }
	if (game->BackBufferCount == 0) game->BackBufferCount = used->BackBufferCount;
	if (game->BackBufferFormat == D3DFMT_UNKNOWN) game->BackBufferFormat = used->BackBufferFormat;
}

// Run a CreateDevice/Reset `call` with our copy `local` of the game's params `orig`, retrying if the runtime
// rejects the forced mode (custom CRU modes, rotated panels, Wine/DXVK mode lists, a bad manual override):
// first the same mode with the default refresh rate, then the game's own unmodified params with DM
// inactive (vanilla-style fullscreen), so the game still runs instead of hanging on a black screen or
// exiting. Each (Create)Device/Reset starts again from the forced mode. A lost device (a Reset while
// alt-tabbed) is not a mode problem - the game retries that Reset itself - so it gets no fallback.
template <typename F>
static HRESULT callWithFallback(const char *what, F call, const D3DPRESENT_PARAMETERS *orig,
                                D3DPRESENT_PARAMETERS *local) {
	HRESULT hr = call(local);
	if (SUCCEEDED(hr) || !g_active || !orig || hr == D3DERR_DEVICELOST) return hr;
	logf("%s failed (0x%08lx) with %ux%u@%uHz windowed=%d", what, (long)hr, local->BackBufferWidth,
	     local->BackBufferHeight, local->FullScreen_RefreshRateInHz, local->Windowed);
	if (!local->Windowed && local->FullScreen_RefreshRateInHz != 0) {
		local->FullScreen_RefreshRateInHz = 0;
		hr = call(local);
		logf("%s retry with the default refresh rate -> 0x%08lx", what, (long)hr);
		if (SUCCEEDED(hr) || hr == D3DERR_DEVICELOST) return hr;
	}
	*local = *orig;
	if (g_winActive) {   // don't retry on every resize: windowed stays the plain stretched 640x480 from now on
		g_winFilterFailed = true;
		logf("%s: WindowedFilter turned off for this session", what);
	}
	g_active = false;
	g_winActive = false;
	g_borderlessActive = false;
	hr = call(local);
	logf("%s retry with the game's own params (DisplayManager inactive until the next Reset) -> 0x%08lx",
	     what, (long)hr);
	return hr;
}

// ---- capture render target (holds the game's rendered frame so we can rescale it) ----------------
// Unbind and release the MSAA targets, putting the device's own backbuffer and depth buffer back (needed before a
// Reset, and when ToggleMSAA turns MSAA off).
static void releaseMsaa() {
	if (g_msRT) {
		IDirect3DDevice9 *dev = GAME_DEVICE;
		if (dev) {
			if (g_bbSurf) dev->SetRenderTarget(0, g_bbSurf);
			dev->SetDepthStencilSurface(g_autoDS);
		}
	}
	if (g_msDS)        { g_msDS->Release();        g_msDS = nullptr; }
	if (g_msRT)        { g_msRT->Release();        g_msRT = nullptr; }
	g_autoDS = nullptr;
	g_msType = D3DMULTISAMPLE_NONE;
}

static void releaseCapture() {
	menuInvalidate();               // ImGui's D3DPOOL_DEFAULT buffers / font texture go too (it makes them again)
	releaseMsaa();
	g_bbSurf = nullptr;
	if (g_stateBlock)  { g_stateBlock->Release();  g_stateBlock = nullptr; }
	if (g_ps)          { g_ps->Release();          g_ps = nullptr; }
	if (g_psXbr)       { g_psXbr->Release();       g_psXbr = nullptr; }
	if (g_stageSurf)   { g_stageSurf->Release();   g_stageSurf = nullptr; }
	if (g_sceneGuard)  { g_sceneGuard->Release();  g_sceneGuard = nullptr; }
	if (g_captureSurf) { g_captureSurf->Release(); g_captureSurf = nullptr; }
	if (g_captureTex)  { g_captureTex->Release();  g_captureTex = nullptr; }
}

// Point the game's rendering at our multisampled target (and depth). The viewport follows the 640x480 target.
static void bindMsaa(IDirect3DDevice9 *dev) {
	if (!g_msRT) return;
	dev->SetRenderTarget(0, g_msRT);
	if (g_msDS) dev->SetDepthStencilSurface(g_msDS);
}

static void createMsaa(IDirect3DDevice9 *dev) {
	if (g_msaaCfg < 2) return;
	D3DDEVICE_CREATION_PARAMETERS cp = {};
	IDirect3D9 *d3d = nullptr;
	if (FAILED(dev->GetCreationParameters(&cp)) || FAILED(dev->GetDirect3D(&d3d)) || !d3d) return;
	BOOL windowed = g_devWindowed ? TRUE : FALSE;
	D3DMULTISAMPLE_TYPE type = D3DMULTISAMPLE_NONE;
	for (int n = g_msaaCfg > 16 ? 16 : g_msaaCfg; n >= 2; n--) {   // the highest supported count <= the request
		D3DMULTISAMPLE_TYPE t = (D3DMULTISAMPLE_TYPE)n;
		if (FAILED(d3d->CheckDeviceMultiSampleType(cp.AdapterOrdinal, cp.DeviceType, g_bbFormat, windowed, t, nullptr)))
			continue;
		if (g_gameDepth && FAILED(d3d->CheckDeviceMultiSampleType(cp.AdapterOrdinal, cp.DeviceType, g_gameDepthFmt,
		                                                         windowed, t, nullptr)))
			continue;
		type = t;
		break;
	}
	d3d->Release();
	if (type == D3DMULTISAMPLE_NONE) { logf("MSAA x%d: not supported - off", g_msaaCfg); return; }
	HRESULT hrRT = dev->CreateRenderTarget((UINT)g_srcW, (UINT)g_srcH, g_bbFormat, type, 0, FALSE, &g_msRT, nullptr);
	HRESULT hrDS = S_FALSE;
	if (SUCCEEDED(hrRT) && g_gameDepth)
		hrDS = dev->CreateDepthStencilSurface((UINT)g_srcW, (UINT)g_srcH, g_gameDepthFmt, type, 0, FALSE, &g_msDS, nullptr);
	if (FAILED(hrRT) || FAILED(hrDS)) {
		if (g_msRT) { g_msRT->Release(); g_msRT = nullptr; }
		logf("MSAA x%d: creating the targets failed (rt=0x%08lx ds=0x%08lx) - off", (int)type, (long)hrRT, (long)hrDS);
		return;
	}
	IDirect3DSurface9 *ds = nullptr;
	if (SUCCEEDED(dev->GetDepthStencilSurface(&ds)) && ds) { g_autoDS = ds; ds->Release(); }
	g_msType = type;
	bindMsaa(dev);
	logf("MSAA x%d on (requested x%d, depth %s fmt=%d)", (int)type, g_msaaCfg, g_msDS ? "yes" : "no", (int)g_gameDepthFmt);
}

static void createCapture(IDirect3DDevice9 *dev) {
	releaseCapture();
	IDirect3DSurface9 *bb = nullptr;
	if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
		g_bbSurf = bb;
		bb->Release();
	}
	HRESULT hr = dev->CreateTexture((UINT)g_srcW, (UINT)g_srcH, 1, D3DUSAGE_RENDERTARGET, g_bbFormat,
	                                D3DPOOL_DEFAULT, &g_captureTex, nullptr);
	if (SUCCEEDED(hr) && g_captureTex)
		g_captureTex->GetSurfaceLevel(0, &g_captureSurf);
	HRESULT hrStage = S_FALSE;                 // S_FALSE = not needed (direct scene calls, see g_sceneDirect)
	if (!g_sceneDirect)
		hrStage = dev->CreateRenderTarget(g_bbW, g_bbH, g_bbFormat, D3DMULTISAMPLE_NONE, 0, FALSE,
		                                  &g_stageSurf, nullptr);
	// Scene guard (64x64: smaller than any depth buffer, so drawing into it is valid; what lands there is discarded).
	HRESULT hrGuard = dev->CreateRenderTarget(64, 64, g_bbFormat, D3DMULTISAMPLE_NONE, 0, FALSE, &g_sceneGuard, nullptr);
	if (FAILED(hrGuard)) logf("scene guard: CreateRenderTarget failed (0x%08lx) - other mods' scene hooks draw on the frame",
	                          (long)hrGuard);
	// Sharp filter shader (falls back to StretchRect if this fails).
	HRESULT hrPs = dev->CreatePixelShader((const DWORD *)g_sharpBilinearPS, &g_ps);
	// xBR needs ps_2_b (any ps_3_0-class GPU takes it); if it can't be created, xBR falls back to StretchRect.
	HRESULT hrXbr = dev->CreatePixelShader((const DWORD *)g_xbrPS2b, &g_psXbr);
	logf("xBR shader: 0x%08lx", (long)hrXbr);
	dev->CreateStateBlock(D3DSBT_ALL, &g_stateBlock);
	createMsaa(dev);
	logf("createCapture %dx%d fmt=%d -> tex=0x%08lx stage=0x%08lx ps=0x%08lx", g_srcW, g_srcH, (int)g_bbFormat,
	     (long)hr, (long)hrStage, (long)hrPs);
}

// ---- device / swapchain method hooks -------------------------------------------------------------
static bool g_presentLogged = false;
// The backbuffer holds our composited (upscaled) frame that hasn't been presented yet. th123 presents with
// D3DPRESENT_DONOTWAIT (dwFlags=1 at 0x401082 / 0x4081D4) and, while Present fails, keeps its "frame
// pending" flag (0x896B76) and presents the SAME frame again (retry loop at 0x4081B0); present_wait=0 in
// SokuDirectXOptimizations does the same. Post-processing that retry would grab the top-left 640x480 of our
// own output and upscale it again (a flash of a zoomed corner). Render thread only.
static bool g_composited = false;

// Sharp-bilinear upscale: draw a quad over the centered destination rect, sampling the captured 640 texture
// through the sharp-bilinear shader. The quad needs the D3D9 -0.5 half-pixel offset so output pixel centers
// sample (k+0.5)/N; WITHOUT it every output pixel samples exactly k/N, where at integer N the sharp-bilinear
// math is a no-op, so Sharpness has NO visible effect (docs/WindowResizer-rendering-research.md L166-173).
// Sharpness 1 = aligned bilinear; higher narrows the interpolation band toward point (~1.5 matches WR).
//
// Normally (g_sceneDirect) the runtime's own BeginScene/EndScene are called, so no other mod's vtable scene hook
// runs and the quad goes straight into the backbuffer (`target` == bb). Otherwise they go through the vtable and
// the quad is drawn into the offscreen stage. Either way BeginScene/EndScene run with g_sceneGuard bound: a mod
// that hooks the runtime's own functions inline (PracticeEx Detours EndScene) still runs, and draws its 640x480
// menu into whatever is bound - the guard, instead of our finished frame.
//
// Returns false if nothing was drawn, so the caller can fall back to StretchRect instead of presenting a
// stale or uninitialised stage.
static bool drawShaderQuad(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb, IDirect3DSurface9 *target,
                           const RECT *dstRect, IDirect3DPixelShader9 *ps, const float (*consts)[4], UINT nConsts,
                           D3DTEXTUREFILTERTYPE samplerFilter) {
	if (!ps || !g_captureTex || !g_stateBlock) return false;
	g_stateBlock->Capture();
	// Begin/EndScene run with the scene guard bound (see g_sceneGuard), the quad's target only in between.
	IDirect3DSurface9 *guard = g_sceneGuard ? g_sceneGuard : bb;
	dev->SetRenderTarget(0, guard);
	HRESULT hrScene = g_sceneDirect ? g_origBeginScene(dev) : dev->BeginScene();
	if (FAILED(hrScene)) { dev->SetRenderTarget(0, bb); g_stateBlock->Apply(); return false; }
	dev->SetRenderTarget(0, target);
	dev->SetPixelShader(ps);
	dev->SetVertexShader(nullptr);
	dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
	dev->SetTexture(0, g_captureTex);
	dev->SetSamplerState(0, D3DSAMP_MINFILTER, samplerFilter);
	dev->SetSamplerState(0, D3DSAMP_MAGFILTER, samplerFilter);
	dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
	dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
	dev->SetPixelShaderConstantF(0, &consts[0][0], nConsts);
	float L = dstRect->left - 0.5f, T = dstRect->top - 0.5f, R = dstRect->right - 0.5f, B = dstRect->bottom - 0.5f;
	struct V { float x, y, z, rhw, u, v; } q[4] = {
		{ L, T, 0.0f, 1.0f, 0.0f, 0.0f },
		{ R, T, 0.0f, 1.0f, 1.0f, 0.0f },
		{ L, B, 0.0f, 1.0f, 0.0f, 1.0f },
		{ R, B, 0.0f, 1.0f, 1.0f, 1.0f },
	};
	HRESULT hr = dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
	dev->SetRenderTarget(0, guard);
	if (g_sceneDirect) g_origEndScene(dev); else dev->EndScene();
	dev->SetRenderTarget(0, bb);
	g_stateBlock->Apply();
	return SUCCEEDED(hr);
}

static bool drawSharp(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb, IDirect3DSurface9 *target, const RECT *dstRect) {
	const float c[1][4] = { { (float)g_srcW, (float)g_srcH, g_sharpness, 0.0f } };
	return drawShaderQuad(dev, bb, target, dstRect, g_ps, c, 1, D3DTEXF_LINEAR);
}

// xBR-lv2 (shader/xbr.hlsl): point-sampled neighbourhood; its edge blending width follows the output scale.
static bool drawXbr(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb, IDirect3DSurface9 *target, const RECT *dstRect) {
	float scale = (float)(dstRect->right - dstRect->left) / (float)g_srcW;
	const float c[2][4] = { { (float)g_srcW, (float)g_srcH, 1.0f / g_srcW, 1.0f / g_srcH },
	                        { g_xbrWidth / (scale > 0.0f ? scale : 1.0f), g_xbrStrength, g_xbrSlopes ? 1.0f : 0.0f,
	                          (float)g_xbrCorner } };
	return drawShaderQuad(dev, bb, target, dstRect, g_psXbr, c, 2, D3DTEXF_POINT);
}

// Pin the game's viewport to its 640x480 (g_srcW x g_srcH) frame. th123 never calls SetViewport: it
// relies on D3D9 setting the viewport to the whole render target at CreateDevice/Reset, which is 640x480
// in vanilla. With our native-sized backbuffer that default viewport is e.g. 2560x1440, and every draw
// that uses TRANSFORMED (non-RHW) vertices - the 3D stage and Okuu (Utsuho) - is mapped through it, i.e.
// scaled by backbuffer/640 from the top-left corner: the giant off-screen Okuu. Pre-transformed (XYZRHW)
// sprites ignore the viewport, which is why only she looked wrong. Applied after CreateDevice/Reset, every
// Present, and whenever the backbuffer is bound again mid-frame (mySetRenderTarget).
static void setGameViewport(IDirect3DDevice9 *dev) {
	D3DVIEWPORT9 vp = { 0, 0, (DWORD)g_srcW, (DWORD)g_srcH, 0.0f, 1.0f };
	dev->SetViewport(&vp);
}

// Tiny 5x7 bitmap font for the on-screen hotkey readout (OSD). GDI text via GetDC does NOT composite on
// th123's backbuffer, so we draw glyphs as solid rectangles with ColorFill instead - pure D3D, works in
// exclusive flip, and (unlike a DrawPrimitiveUP quad) needs no BeginScene/EndScene, so it can't
// re-trigger a mod's per-scene overlay redraw. Rows are 5 bits, MSB = leftmost column. Only the
// characters used by the OSD messages are defined; OSD_CHARS is the parallel lookup key (uppercase).
static const char OSD_CHARS[] = "0123456789. SHARPCEFILNOTUXBMDWG";
static const BYTE OSD_FONT[][7] = {
	{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
	{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
	{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
	{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 3
	{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
	{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
	{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
	{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
	{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
	{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
	{0x00,0x00,0x00,0x00,0x00,0x06,0x06}, // .
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // (space)
	{0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}, // S
	{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // H
	{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // A
	{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // R
	{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // P
	{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // C
	{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // E
	{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // F
	{0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}, // I
	{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // L
	{0x11,0x19,0x15,0x13,0x11,0x11,0x11}, // N
	{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // O
	{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // T
	{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // U
	{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // X
	{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // B
	{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // M
	{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, // D
	{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, // W
	{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, // G
};
static int osdGlyph(char c) {
	for (int i = 0; OSD_CHARS[i]; i++) if (OSD_CHARS[i] == c) return i;
	return 11;   // space
}

// Post a message to the on-screen readout for ~1.5s (drawn by drawOsd in the Present post-process).
// Message text must use only OSD_CHARS characters (uppercase). Called from the hotkey thread.
static void showOsd(const char *msg) {
	lstrcpynA(g_osdText, msg, sizeof(g_osdText));
	g_osdUntil = GetTickCount() + 1500;
}

// Draw g_osdText as ColorFill glyphs at the top-left of the centered game (`out`).
static void drawOsd(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb, const RECT *out) {
	char txt[32]; lstrcpynA(txt, g_osdText, sizeof(txt));
	const int S = 5, gw = 5 * S, gh = 7 * S, sp = S, pad = 3 * S;   // glyph 25x35, 5px gaps
	int n = lstrlenA(txt); if (n <= 0) return;
	int textW = n * (gw + sp) - sp, x0 = out->left + 20, y0 = out->top + 16;
	RECT bar = { x0 - pad, y0 - pad, x0 + textW + pad, y0 + gh + pad };
	dev->ColorFill(bb, &bar, D3DCOLOR_XRGB(0, 0, 0));               // opaque backdrop for contrast
	for (int i = 0; i < n; i++) {
		int gi = osdGlyph(txt[i]), gx = x0 + i * (gw + sp);
		for (int r = 0; r < 7; r++) {
			BYTE row = OSD_FONT[gi][r];
			for (int c = 0; c < 5; c++)
				if (row & (1 << (4 - c))) {
					RECT p = { gx + c * S, y0 + r * S, gx + c * S + S, y0 + r * S + S };
					dev->ColorFill(bb, &p, D3DCOLOR_XRGB(255, 255, 255));
				}
		}
	}
}

static void showSharpnessOsd() {
	char num[16], msg[32];
	formatSharpness(num);
	wsprintfA(msg, "SHARP %s", num);
	showOsd(msg);
}

// Fill everything outside the centered game rect with the border color (up to four rects). The game rect
// itself is fully overwritten by the upscale, so a full-screen fill would only waste bandwidth.
static void fillBorders(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb, const RECT &d) {
	LONG W = (LONG)g_bbW, H = (LONG)g_bbH;
	if (d.top    > 0) { RECT r = { 0, 0, W, d.top };               dev->ColorFill(bb, &r, g_bgColor); }
	if (d.bottom < H) { RECT r = { 0, d.bottom, W, H };            dev->ColorFill(bb, &r, g_bgColor); }
	if (d.left   > 0) { RECT r = { 0, d.top, d.left, d.bottom };   dev->ColorFill(bb, &r, g_bgColor); }
	if (d.right  < W) { RECT r = { d.right, d.top, W, d.bottom };  dev->ColorFill(bb, &r, g_bgColor); }
}

// ---- overlays (DisplayManagerOverlay.h) -----------------------------------------------------------
// Other mods' callbacks, run after the frame is composited. Registered from any thread, called on the render
// thread; a slim lock keeps the table consistent (it is only held for the copy, never across a callback).
static const int MAX_OVERLAYS = 8;
struct Overlay { DisplayManager_OverlayProc proc; void *user; };
static Overlay g_overlays[MAX_OVERLAYS];
static int     g_overlayCount = 0;
static SRWLOCK g_overlayLock = SRWLOCK_INIT;

static void runOverlays(int event, IDirect3DDevice9 *dev, IDirect3DSurface9 *bb, const RECT *gameRect) {
	Overlay list[MAX_OVERLAYS];
	AcquireSRWLockShared(&g_overlayLock);
	int n = g_overlayCount;
	memcpy(list, g_overlays, n * sizeof(Overlay));
	ReleaseSRWLockShared(&g_overlayLock);
	if (!n) return;
	DisplayManager_OverlayInfo info = {};
	info.size = sizeof info;
	info.device = dev;
	info.backbuffer = bb;
	info.bbWidth = g_bbW;
	info.bbHeight = g_bbH;
	info.bbFormat = g_bbFormat;
	if (gameRect) info.gameRect = *gameRect;
	info.borderColor = g_bgColor;
	for (int i = 0; i < n; i++) list[i].proc(event, &info, list[i].user);
}

// The swapchain/device vtables are shared by every D3D9 swapchain/device in the process (overlays, mods that
// make their own device), so the hooks only act on the game's. Fast path: the game's swapchain global; else
// (e.g. a wrapper stored its own object there) compare with the game device's implicit swapchain. Neither
// check AddRefs anything that outlives the call.
static bool isGameSwapChain(IDirect3DSwapChain9 *sc) {
	if (sc == GAME_SWAPCHAIN) return true;
	IDirect3DDevice9 *dev = GAME_DEVICE;
	IDirect3DSwapChain9 *own = nullptr;
	if (!dev || FAILED(dev->GetSwapChain(0, &own)) || !own) return false;
	own->Release();
	return own == sc;
}

static HRESULT WINAPI mySCPresent(IDirect3DSwapChain9 *sc, const RECT *src, const RECT *dst,
                                  HWND wnd, const RGNDATA *dirty, DWORD flags) {
	if (!isGameSwapChain(sc)) return oSCPresent(sc, src, dst, wnd, dirty, flags);
	if (g_deviceHooked && GAME_DEVICE) healSlots(GAME_DEVICE);
	// Post-process: grab the game's g_srcW x g_srcH frame from the backbuffer's top-left, fill the borders
	// and upscale it centered into the backbuffer.
	if (g_active && g_captureSurf && !g_composited) {
		IDirect3DDevice9 *dev = GAME_DEVICE;
		IDirect3DSurface9 *bb = nullptr;
		if (dev && SUCCEEDED(sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
			g_bbSurf = bb;
			g_inPost = true;                   // our own SetRenderTarget calls must not re-pin the viewport
			RECT srcRect = { 0, 0, g_srcW, g_srcH };
			LONG x = ((LONG)g_bbW - g_scaleW) / 2, y = ((LONG)g_bbH - g_scaleH) / 2;
			RECT dstRect = { x, y, x + g_scaleW, y + g_scaleH };
			// 1:1 grab (with MSAA: the resolve of the game's multisampled target). Our 640x480 multisampled depth
			// can't go with the native-size backbuffer the rest of the post-process draws into, so unbind it;
			// bindMsaa puts everything back after Present.
			HRESULT a = dev->StretchRect(g_msRT ? g_msRT : bb, &srcRect, g_captureSurf, nullptr, D3DTEXF_NONE);
			if (g_msRT) {
				dev->SetRenderTarget(0, bb);
				if (g_msDS) dev->SetDepthStencilSurface(nullptr);
			}
			// Every output pixel comes from this one upscale pass - never re-blit part of the frame with a
			// different filter (that blurred the corner overlapping the grab region at non-integer scales,
			// KNOWN-BUGS Bug 4). Without direct scene calls the upscale goes into the stage and the backbuffer
			// is filled + copied AFTER it, which also wipes PracticeEx's menu dupe from drawSharp's scene.
			IDirect3DSurface9 *target = g_stageSurf ? g_stageSurf : bb;
			HRESULT b = S_OK, c = S_OK;
			if (!g_stageSurf) fillBorders(dev, bb, dstRect);
			// Sharp and xBR fall back to StretchRect (g_filter resolves to linear for them) if the shader pass
			// couldn't draw - otherwise a stale / uninitialised frame would be shown.
			bool drawn = g_filterCfg == FILTER_SHARP || g_filterCfg == FILTER_AUTO ? drawSharp(dev, bb, target, &dstRect) :
			             g_filterCfg == FILTER_XBR   ? drawXbr(dev, bb, target, &dstRect) : false;
			if (!drawn)
				c = dev->StretchRect(g_captureSurf, nullptr, target, &dstRect, (D3DTEXTUREFILTERTYPE)g_filter);
			if (g_stageSurf) {
				b = dev->ColorFill(bb, nullptr, g_bgColor);                                  // borders
				c = dev->StretchRect(g_stageSurf, &dstRect, bb, &dstRect, D3DTEXF_NONE);   // 1:1 copy
			}
			if (!g_presentLogged) {
				DWORD msaaRS = 0;
				dev->GetRenderState(D3DRS_MULTISAMPLEANTIALIAS, &msaaRS);
				logf("first present: grab=0x%08lx fill=0x%08lx blit=0x%08lx filterCfg=%d sharp=%.2f scene=%s msaa=x%d rs=%lu",
				     (long)a, (long)b, (long)c, g_filterCfg, g_sharpness, g_sceneDirect ? "direct" : "vtable+stage",
				     (int)g_msType, msaaRS);
				g_presentLogged = true;
			}
			runOverlays(DM_OVERLAY_DRAW, dev, bb, &dstRect);
			if (GetTickCount() < g_osdUntil) drawOsd(dev, bb, &dstRect);   // hotkey readout (Alt+K/L/F/0-6)
			if (menuShown()) menuRender(dev, bb);                         // the settings menu, on top of everything
			g_inPost = false;
			bb->Release();
			g_composited = true;
		}
		// Re-pin every frame on the render thread so nothing (Reset, SetRenderTarget, other mods) undoes it.
		if (dev) setGameViewport(dev);
	}
	HRESULT hr = oSCPresent(sc, src, dst, wnd, dirty, flags);
	if (hr != D3DERR_WASSTILLDRAWING) g_composited = false;   // presented (or dropped): next call is a new frame
	if (g_msaaApply && g_active) {                             // ToggleMSAA: recreate for the next frame
		g_msaaApply = false;
		IDirect3DDevice9 *dev = GAME_DEVICE;
		if (dev && g_bbSurf) {
			releaseMsaa();
			createMsaa(dev);                                   // binds the new target, or leaves the backbuffer
			setGameViewport(dev);
			logf("MSAA toggled: requested x%d -> x%d", (int)g_msaaCfg, (int)g_msType);
		}
	}
	if (g_msRT && g_active) {                                  // the next frame draws into the MSAA target again
		IDirect3DDevice9 *dev = GAME_DEVICE;
		if (dev) bindMsaa(dev);
	}
	return hr;
}

// Keep the 640x480 viewport pinned across a mid-frame SetRenderTarget: D3D9 resets the viewport to the
// whole new render target, so binding the (native-sized) backbuffer again after drawing into a texture would
// bring back the giant-Okuu scaling (see setGameViewport) for the rest of that frame. Only for the game
// device's backbuffer at index 0 while we force our backbuffer, and not during our own post-process. Other mods
// may hook this slot too; hookSlot chains to whatever was there.
static HRESULT WINAPI mySetRenderTarget(IDirect3DDevice9 *dev, DWORD index, IDirect3DSurface9 *rt) {
	HRESULT hr = oSetRenderTarget(dev, index, rt);
	if (index == 0 && g_active && !g_inPost && rt && (rt == g_bbSurf || rt == g_msRT) && dev == GAME_DEVICE &&
	    SUCCEEDED(hr))
		setGameViewport(dev);
	return hr;
}

// ---- sharp sprites (experimental: SpriteSharpness / BackgroundSharpness) -----------------------------
// The game draws the characters and the stage tiles POINT-sampled at its camera zoom, which is almost never a whole
// number: some texels come out a screen pixel wider than others, and edges crawl while the camera zooms. Those draws
// go through sharp-bilinear instead (shader/sharpsprite.hlsl): texel interiors stay flat, texel edges get a blend
// band of 1/(k*scale) texels, scale = the draw's own screen pixels per texel (from the quad's sides). It happens in
// the game's own 640x480 frame, so it is independent of the upscale Filter and of whether DM composites at all.
// Which draws: those made inside the players' draw (0x46E0D0) and the stage background (0x470500, 0x470570) and
// foreground (0x4705D0) draws. Their entries are detoured to wrappers that mark the layer for the DrawPrimitiveUP
// hook - entries rather than the battle render's calls to them (0x47A8D0), because mods that change the draw order
// (CharactersInForeground) NOP the game's call to that and call these functions themselves. Only
// XYZRHW|DIFFUSE|TEX1 quads with MAG=POINT and the plain fixed-function setup (stage 0 = texture x diffuse, stage 1
// off; no pixel shader, or the game's weather tint shader - WEATHER_TINT_PS) are changed: the drop shadows (LINEAR) and the stage's effect pass (0x470240, which 0x470500
// tail-jumps to; its own shaders) are left alone. Everything set is restored right after the draw: the game's
// renderer caches its sampler state (0x896B4C) and skips setting what it thinks is already set. Prototyped in
// SokuHarness (docs/PLAN-zoom-wobble.md).
static volatile int g_sprLayer = -1;   // SPR_* while inside one of the wrapped draws (render thread), else -1
static IDirect3DPixelShader9 *g_psSprite = nullptr;   // survives Reset (not a D3DPOOL_DEFAULT resource)
static bool g_sprLogged[SPR_LAYERS] = { false, false };
static bool g_sprTiledLogged = false;
static bool g_sprTintLogged = false;

typedef void (__fastcall *GameDraw_t)(void *self);   // thiscall without arguments = fastcall with ecx = this
static GameDraw_t oPlayersDraw = nullptr, oStageBgA = nullptr, oStageBgB = nullptr, oStageFg = nullptr;

static void sprWrap(GameDraw_t fn, void *self, int layer) {
	int prev = g_sprLayer;
	g_sprLayer = layer;
	fn(self);
	g_sprLayer = prev;
}
static void __fastcall myPlayersDraw(void *self) { sprWrap(oPlayersDraw, self, SPR_CHARS); }
static void __fastcall myStageBgA(void *self)    { sprWrap(oStageBgA, self, SPR_STAGE); }
static void __fastcall myStageBgB(void *self)    { sprWrap(oStageBgB, self, SPR_STAGE); }
static void __fastcall myStageFg(void *self)     { sprWrap(oStageFg, self, SPR_STAGE); }

// Detour the game function at `fn` to `hook`. Its first `n` bytes must be `expect` (whole, position-independent
// instructions; nothing jumps back into them): they move to a trampoline that continues at fn + n, which is returned
// (nullptr = left alone, e.g. another mod already patched that entry).
static BYTE *g_trampolines = nullptr;
static int   g_trampolineUsed = 0;
static GameDraw_t detourEntry(DWORD fn, const BYTE *expect, int n, void *hook) {
	BYTE *p = (BYTE *)fn;
	if (memcmp(p, expect, n) != 0) return nullptr;
	if (!g_trampolines)
		g_trampolines = (BYTE *)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!g_trampolines || g_trampolineUsed + n + 5 > 4096) return nullptr;
	BYTE *tr = g_trampolines + g_trampolineUsed;
	g_trampolineUsed += 16;
	memcpy(tr, p, n);
	tr[n] = 0xE9;
	*(LONG *)(tr + n + 1) = (LONG)((fn + n) - ((DWORD)tr + n + 5));
	DWORD old;
	if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return nullptr;
	p[0] = 0xE9;
	*(LONG *)(p + 1) = (LONG)((DWORD)hook - (fn + 5));
	for (int i = 5; i < n; i++) p[i] = 0xCC;
	VirtualProtect(p, n, old, &old);
	FlushInstructionCache(GetCurrentProcess(), p, n);
	return (GameDraw_t)tr;
}

// Screen pixels per texel along u and v for a 4-vertex quad (strip or fan), from two of its sides.
static bool quadScale(D3DPRIMITIVETYPE t, UINT count, const BYTE *pb, UINT stride, UINT uvOff, UINT tw, UINT th,
                      float *su, float *sv) {
	int other;
	if (t == D3DPT_TRIANGLESTRIP && count == 2) other = 2;
	else if (t == D3DPT_TRIANGLEFAN && count == 2) other = 3;
	else return false;
	*su = *sv = 0.0f;
	const int ends[2] = { 1, other };
	const float *v0 = (const float *)pb, *t0 = (const float *)(pb + uvOff);
	for (int e = 0; e < 2; e++) {
		const float *v = (const float *)(pb + (size_t)ends[e] * stride);
		const float *uv = (const float *)(pb + (size_t)ends[e] * stride + uvOff);
		float dx = v[0] - v0[0], dy = v[1] - v0[1], du = (uv[0] - t0[0]) * tw, dv = (uv[1] - t0[1]) * th;
		float L = sqrtf(dx * dx + dy * dy), T = sqrtf(du * du + dv * dv);
		if (T < 0.5f) return false;
		if (fabsf(du) >= fabsf(dv)) *su = L / T; else *sv = L / T;
	}
	return *su > 0.0f && *sv > 0.0f;
}

struct SprSaved { DWORD addrU, addrV; bool clampU, clampV; IDirect3DPixelShader9 *ps; float c[12]; };

// The game's own stage shader under some weathers (Cloudy, Dust Storm, ...): `colour = saturate(texture + c0) x
// diffuse`, c0 = the weather's colour offset (ps_1_1: tex t0 / add_sat r0, t0, c0 / mul r0, r0, v0). Our shader can
// do the same after its own sampling (c2), so such draws are filtered too, with the game's tint. Any other shader:
// left alone.
static const BYTE WEATHER_TINT_PS[48] = {
	0x01, 0x01, 0xff, 0xff, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xb0, 0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x1f, 0x80, 0x00, 0x00, 0xe4, 0xb0, 0x00, 0x00, 0xe4, 0xa0, 0x05, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x0f, 0x80, 0x00, 0x00, 0xe4, 0x80, 0x00, 0x00, 0xe4, 0x90, 0xff, 0xff, 0x00, 0x00 };
static bool isWeatherTintPS(IDirect3DPixelShader9 *ps) {
	static IDirect3DPixelShader9 *last = nullptr;   // the game creates it once: compare the bytecode only on a change
	static bool lastIs = false;
	if (ps != last) {
		BYTE code[sizeof(WEATHER_TINT_PS)];
		UINT size = 0;
		lastIs = SUCCEEDED(ps->GetFunction(nullptr, &size)) && size == sizeof(code) &&
		         SUCCEEDED(ps->GetFunction(code, &size)) && memcmp(code, WEATHER_TINT_PS, sizeof(code)) == 0;
		last = ps;
	}
	return lastIs;
}

// The characters' filter strength for the current camera zoom (SokuLib camera.scale, 0x898614). The game zooms
// out as the players move apart (zoom = 640 / the distance between them, from 1.0 at up to ~560 px apart down to
// 0.5), and their sprites' pixels get more uneven the further it is from the resting 1.0: SpriteNearStrength at
// 1.0, SpriteFarStrength at SpriteFarZoom and below, linear in between.
static float sprStrengthByZoom() {
	float z = *(const volatile float *)0x898614;
	if (!(z > 0.1f && z < 4.0f)) z = 1.0f;          // not set up (never expected inside a battle draw)
	const float nearS = g_sprRest[SPR_CHARS], farS = g_sprFar, farZ = g_sprFarZoom;
	float t = (1.0f - z) / (1.0f - farZ);
	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	return nearS + (farS - nearS) * t;
}

// Set up the sharp-sprite shader for this draw if it is one of the plain POINT sprite quads; false = leave it alone.
static bool sprBegin(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, UINT count, const void *data, UINT stride, int layer,
                     SprSaved *sv) {
	DWORD fvf = 0;
	if (!data || FAILED(dev->GetFVF(&fvf)) || (fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW ||
	    !(fvf & D3DFVF_DIFFUSE) || (fvf & D3DFVF_TEXCOUNT_MASK) != D3DFVF_TEX1)
		return false;
	DWORD mag = 0, cop = 0, ca1 = 0, ca2 = 0, aop = 0, aa1 = 0, aa2 = 0, cop1 = 0;
	dev->GetSamplerState(0, D3DSAMP_MAGFILTER, &mag);
	dev->GetTextureStageState(0, D3DTSS_COLOROP, &cop);  dev->GetTextureStageState(0, D3DTSS_COLORARG1, &ca1);
	dev->GetTextureStageState(0, D3DTSS_COLORARG2, &ca2); dev->GetTextureStageState(0, D3DTSS_ALPHAOP, &aop);
	dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &aa1); dev->GetTextureStageState(0, D3DTSS_ALPHAARG2, &aa2);
	dev->GetTextureStageState(1, D3DTSS_COLOROP, &cop1);
	bool diff2 = (ca2 == D3DTA_DIFFUSE || ca2 == D3DTA_CURRENT) && (aa2 == D3DTA_DIFFUSE || aa2 == D3DTA_CURRENT);
	if (mag != D3DTEXF_POINT || cop != D3DTOP_MODULATE || aop != D3DTOP_MODULATE || ca1 != D3DTA_TEXTURE ||
	    aa1 != D3DTA_TEXTURE || !diff2 || cop1 != D3DTOP_DISABLE)
		return false;
	IDirect3DBaseTexture9 *bt = nullptr;
	UINT tw = 0, th = 0;
	if (FAILED(dev->GetTexture(0, &bt)) || !bt) return false;
	if (bt->GetType() == D3DRTYPE_TEXTURE) {
		D3DSURFACE_DESC d;
		if (SUCCEEDED(((IDirect3DTexture9 *)bt)->GetLevelDesc(0, &d))) { tw = d.Width; th = d.Height; }
	}
	bt->Release();
	float su = 0.0f, svv = 0.0f;
	UINT uvOff = 16 + 4 + ((fvf & D3DFVF_SPECULAR) ? 4 : 0);   // after XYZRHW, DIFFUSE (and SPECULAR)
	if (!tw || !th || stride < uvOff + 8 || !quadScale(t, count, (const BYTE *)data, stride, uvOff, tw, th, &su, &svv))
		return false;
	// The filter strength per axis (0 = the nearest texel = POINT, 1 = the full filter; the shader mixes the two).
	// Stage: at a whole-number scale, POINT is already even, while sharp-bilinear depends on the sprite's sub-pixel
	// position: when the camera puts texel edges right on pixel centres (e.g. x1 with the camera on a half pixel), it
	// blends every edge 50/50 and every other column / row comes out as a mix - visibly blurry. In exchange it moves
	// smoothly where POINT steps a whole pixel at a time (the stage panning at x1). So a whole axis gets the rest
	// strength. Characters: by the camera's zoom (sprStrengthByZoom), faded the same way. Strength 0 on both axes is the
	// same image as the game's own POINT draw, so such a draw is left alone.
	float strU, strV;
	if (layer == SPR_CHARS) {
		// Near a whole-number scale the same blur appears (at x1, the camera's farthest zoom 0.5, every pixel of a
		// sprite on a half pixel is a 50/50 mix, while POINT is already even there), so the strength fades to the
		// near strength within SPR_WHOLE_FADE of one.
		const float s = sprStrengthByZoom(), nearS = g_sprRest[SPR_CHARS];
		const float fu = fminf(fabsf(su - floorf(su + 0.5f)) / SPR_WHOLE_FADE, 1.0f);
		const float fv = fminf(fabsf(svv - floorf(svv + 0.5f)) / SPR_WHOLE_FADE, 1.0f);
		strU = nearS + (s - nearS) * fu;
		strV = nearS + (s - nearS) * fv;
	} else {
		const bool wholeU = fabsf(su - floorf(su + 0.5f)) < 0.001f, wholeV = fabsf(svv - floorf(svv + 0.5f)) < 0.001f;
		strU = wholeU ? g_sprRest[layer] : 1.0f;
		strV = wholeV ? g_sprRest[layer] : 1.0f;
	}
	if (strU <= 0.0f && strV <= 0.0f) return false;
	// No pixel shader, or the game's weather tint (its colour offset goes to our c2); anything else: left alone.
	float tint[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	sv->ps = nullptr;
	if (SUCCEEDED(dev->GetPixelShader(&sv->ps)) && sv->ps) {
		if (!isWeatherTintPS(sv->ps)) { sv->ps->Release(); return false; }
		dev->GetPixelShaderConstantF(0, tint, 1);
	}
	if (!g_psSprite) {
		HRESULT hr = dev->CreatePixelShader((const DWORD *)g_sharpSpritePS, &g_psSprite);
		if (FAILED(hr)) {
			g_psSprite = nullptr;
			g_sprFailed = true;
			g_sprK[SPR_CHARS] = g_sprK[SPR_STAGE] = 0.0f;
			logf("filtered sprites: CreatePixelShader failed (0x%08lx) - off", (long)hr);
			if (sv->ps) sv->ps->Release();
			return false;
		}
	}
	// CLAMP keeps the 4-tap blend at a sprite's edge from pulling in texels from the opposite side of its texture - but
	// only along an axis where the quad stays inside the texture. Along an axis where its texture coordinates go past
	// it, the quad tiles the texture through the game's WRAP addressing (the scrolling Dust Storm dust, along u):
	// clamped, everything beyond the edge would repeat the edge texels as long streaks, so that axis keeps the game's
	// addressing (the taps wrap like the texture does). Per axis: the dust (u 0.01..1.01, v 0..1) wrapping along v too
	// blended its top and bottom rows with each other - a 1-pixel seam at its edges when zoomed out.
	float uMin = 1e9f, uMax = -1e9f, vMin = 1e9f, vMax = -1e9f;
	for (int i = 0; i < 4; i++) {
		const float *uv = (const float *)((const BYTE *)data + (size_t)i * stride + uvOff);
		uMin = min(uMin, uv[0]); uMax = max(uMax, uv[0]); vMin = min(vMin, uv[1]); vMax = max(vMax, uv[1]);
	}
	sv->clampU = uMin > -0.001f && uMax < 1.001f;
	sv->clampV = vMin > -0.001f && vMax < 1.001f;
	if (sv->clampU) dev->GetSamplerState(0, D3DSAMP_ADDRESSU, &sv->addrU);
	if (sv->clampV) dev->GetSamplerState(0, D3DSAMP_ADDRESSV, &sv->addrV);
	dev->GetPixelShaderConstantF(0, sv->c, 3);
	const float c[12] = { (float)tw, (float)th, su, svv, g_sprK[layer], 0.0f, strU, strV,
	                      tint[0], tint[1], tint[2], tint[3] };
	dev->SetPixelShader(g_psSprite);
	dev->SetPixelShaderConstantF(0, c, 3);
	if (sv->clampU) dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	if (sv->clampV) dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	if ((!sv->clampU || !sv->clampV) && !g_sprTiledLogged) {
		g_sprTiledLogged = true;
		const float *p0 = (const float *)data, *p3 = (const float *)((const BYTE *)data + (size_t)3 * stride);
		logf("filtered sprites: first tiled %s draw (%ux%u texture, u %.2f..%.2f, v %.2f..%.2f, at %.1f,%.1f..%.1f,%.1f) - "
		     "kept the game's addressing along %s", layer == SPR_CHARS ? "character" : "stage", tw, th, uMin, uMax, vMin,
		     vMax, p0[0], p0[1], p3[0], p3[1], !sv->clampU && !sv->clampV ? "u and v" : !sv->clampU ? "u" : "v");
	}
	if (sv->ps && !g_sprTintLogged) {
		g_sprTintLogged = true;
		logf("filtered sprites: first %s draw with the game's weather tint (%.3f, %.3f, %.3f, %.3f)",
		     layer == SPR_CHARS ? "character" : "stage", tint[0], tint[1], tint[2], tint[3]);
	}
	if (!g_sprLogged[layer]) {
		g_sprLogged[layer] = true;
		logf("filtered sprites: first %s draw (%ux%u texture, x%.2f/x%.2f, k=%.2f)",
		     layer == SPR_CHARS ? "character" : "stage", tw, th, su, svv, (double)g_sprK[layer]);
	}
	return true;
}

static void sprEnd(IDirect3DDevice9 *dev, const SprSaved *sv) {
	dev->SetPixelShader(sv->ps);    // none, or the game's weather tint shader
	if (sv->ps) sv->ps->Release();
	dev->SetPixelShaderConstantF(0, sv->c, 3);
	if (sv->clampU) dev->SetSamplerState(0, D3DSAMP_ADDRESSU, sv->addrU);
	if (sv->clampV) dev->SetSamplerState(0, D3DSAMP_ADDRESSV, sv->addrV);
}

static HRESULT WINAPI myDrawPrimitiveUP(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, UINT count, const void *data,
                                       UINT stride) {
	int layer = g_sprLayer;
	SprSaved sv;
	bool sharp = layer >= 0 && g_sprK[layer] > 0.0f && dev == GAME_DEVICE &&
	             sprBegin(dev, t, count, data, stride, layer, &sv);
	HRESULT hr = oDrawPrimitiveUP(dev, t, count, data, stride);
	if (sharp) sprEnd(dev, &sv);
	return hr;
}

// Only when SpriteSharpness / BackgroundSharpness or one of their hotkeys is set: with all of them commented out,
// nothing is patched. The game-code detours from Initialize (before the game runs), the DrawPrimitiveUP hook from
// hookDevice.
static void installSharpSpriteDetours() {
	if (!g_sprWanted) return;
	static const BYTE players[] = { 0x51, 0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x40 };   // push ecx; push esi; mov esi,ecx; mov ecx,[esi+40]
	static const BYTE bgA[]     = { 0x53, 0x55, 0x56, 0x8B, 0xE9 };               // push ebx; push ebp; push esi; mov ebp,ecx
	static const BYTE layer[]   = { 0x8B, 0x41, 0x2C, 0x53, 0x56 };               // mov eax,[ecx+2C]; push ebx; push esi
	oPlayersDraw = detourEntry(0x0046E0D0, players, sizeof players, (void *)myPlayersDraw);
	oStageBgA    = detourEntry(0x00470500, bgA, sizeof bgA, (void *)myStageBgA);
	oStageBgB    = detourEntry(0x00470570, layer, sizeof layer, (void *)myStageBgB);
	oStageFg     = detourEntry(0x004705D0, layer, sizeof layer, (void *)myStageFg);
	logf("filtered sprites: sprites=%.2f background=%.2f; game draws hooked: characters %s, stage %d/%d/%d",
	     (double)g_sprK[SPR_CHARS], (double)g_sprK[SPR_STAGE], oPlayersDraw ? "ok" : "FAILED (entry already patched)",
	     oStageBgA != nullptr, oStageBgB != nullptr, oStageFg != nullptr);
}
static void installSharpSprites(IDirect3DDevice9 *dev) {
	if (!g_sprWanted) return;
	hookSlot(*(void ***)dev, VT_DEV_DRAWPRIMUP, (void *)myDrawPrimitiveUP, (void **)&oDrawPrimitiveUP);
}

// ---- device Reset ---------------------------------------------------------------------------------
// Why DM doesn't rely on a Reset vtable hook alone: whenever anything records a state block (Begin/EndStateBlock -
// D3DX does it in ValidateTechnique, FindNextValidTechnique and Effect::Begin without D3DXFX_DONOTSAVESTATE),
// Windows' d3d9 rewrites the device vtable with its own entries, silently dropping every in-place slot hook.
// Mods that patch the game's Reset call site (0x4151AC: InGameHostlist, ReplayHudExtras - the "ImGuiMan"
// pattern) save the slot's value once, so if they do it after such a reset they call the runtime's Reset
// directly. DM's Reset hook was then skipped: its D3DPOOL_DEFAULT targets were never released, Reset failed with
// D3DERR_INVALIDCALL, and the game (which doesn't recreate its textures after a failed Reset) froze or crashed.
// Seen with SokuShaderPro (FindNextValidTechnique at startup). So:
//   - DM's targets are released/recreated by the game itself: DM registers as one of its D3DPOOL_DEFAULT owners
//     (g_deviceListener), which the Reset wrapper calls around every Reset, whoever hooks what.
//   - The present-params override hooks the wrapper's ENTRY (myGameReset), which every game Reset goes through.
//     Other mods' patches inside the wrapper keep working (the original body runs). The vtable hook (myReset)
//     is only the fallback when the entry can't be hooked.
//   - The SetRenderTarget slot is re-hooked when found reset (healSlots). A Reset that skipped DM's params is
//     detected by the listener: in fallback mode DM then stands down (passthrough) for the session instead of
//     breaking; with the entry hook it can only be a race with a late attach, so just that one passes through.
static bool g_listenerOn    = false;   // DM is registered in the game's D3DPOOL_DEFAULT owner list
static bool g_gameResetHook = false;   // the wrapper entry is hooked (myGameReset); no Reset vtable hook then
static bool g_resetSeen     = false;   // DM applied its params to the Reset in progress (cleared by the listener)
static bool g_resetBypassed = false;   // fallback mode only: a Reset skipped DM's hook - passthrough from then on

// What the wrapper writes into BackBufferFormat before resetting (0x415132), from the struct's Windowed.
static D3DFORMAT wrapperFormat(BOOL windowed) {
	return windowed ? reinterpret_cast<const D3DDISPLAYMODE *>(ADDR_DESKTOP_MODE)->Format : D3DFMT_X8R8G8B8;
}

// Bookkeeping before a Reset with the game's own params `pp` (both paths).
static void beforeReset(IDirect3DDevice9 *dev, const D3DPRESENT_PARAMETERS *pp) {
	g_resetSeen = true;
	if (!g_listenerOn) {            // (registered: the owner "lost" call does all this, under the render lock)
		releaseCapture();           // default-pool resources must be freed before Reset
		runOverlays(DM_OVERLAY_RESET, dev, nullptr, nullptr);   // ...theirs too
		g_composited = false;       // Reset discards the backbuffer contents
	}
	if (!g_d3d) {                   // device hooked without our CreateDevice (device-watch fallback)
		D3DDEVICE_CREATION_PARAMETERS cp; IDirect3D9 *d3d = nullptr;
		if (SUCCEEDED(dev->GetCreationParameters(&cp)) && SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d) {
			g_adapterMon = d3d->GetAdapterMonitor(cp.AdapterOrdinal);
			g_d3d = d3d; g_adapter = cp.AdapterOrdinal;
			d3d->Release();         // the device keeps its IDirect3D9 alive
		}
	}
	// Leaving windowed: remember the window's position and monitor before anything moves it. If the
	// first-spawn setup is still queued (a fullscreen start: th123 SendMessages its startup Alt+Enter at
	// 0x442EC7, possibly before our posted message is pumped), run it now so that is what we restore.
	if (pp && !pp->Windowed && !g_windowFs && g_hwnd) {
		if (g_spawnPending) applyWindowState();
		RECT r;
		if (GetWindowRect(g_hwnd, &r)) { g_winPos.x = r.left; g_winPos.y = r.top; g_haveWinPos = true; }
		g_fsMon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
	}
}

// After the Reset: recreate our targets if nothing else does (not registered as an owner yet), and set the window up
// for a real switch. `ownersDone`: the wrapper has returned (myGameReset; it handles the not-yet-registered case
// itself, under the lock). From myReset, inside the wrapper, the owner "reset" call is still to come, so g_resetSeen
// stays set for it - unless the Reset failed, which gets no owner call at all.
static void afterReset(IDirect3DDevice9 *dev, bool ok, bool ownersDone) {
	if (ok && g_active && !ownersDone && !g_listenerOn) {
		createCapture(dev);
		setGameViewport(dev);
	}
	if (!ok || ownersDone) g_resetSeen = false;   // the next Reset must set it again
	if (ok) {
		// A real windowed <-> fullscreen switch: set the window up for the new state (borderless popup, or
		// the restored frame + remembered scale/position + topmost). Deferred via postWindowApply, because
		// the game's own SetWindowPos runs after the Reset returns and would undo it.
		if (g_wantFullscreen != g_windowFs) {
			g_windowFs = g_wantFullscreen;
			postWindowApply(false);
		}
	}
	g_presentLogged = false;
}

// The game's D3DPOOL_DEFAULT owner (see "device Reset" above). Called by the Reset wrapper on the thread doing the
// Reset, under the render lock, so the render thread is never inside our Present meanwhile.
struct DeviceListener {
	virtual void onLost() {         // slot 0: before Reset (also on every retry of a failed one)
		releaseCapture();
		runOverlays(DM_OVERLAY_RESET, GAME_DEVICE, nullptr, nullptr);
		g_composited = false;
	}
	virtual void onReset() {        // slot 1: after a successful Reset, the swapchain fetched again
		IDirect3DDevice9 *dev = GAME_DEVICE;
		if (!g_resetSeen && !g_gameResetHook) {
			// The device was reset without DM's params, in fallback mode: the vtable hook was bypassed - and a mod
			// that saved the runtime's Reset keeps bypassing it, so DM stands down for the session: the backbuffer
			// is whatever the game asked for (no compositing into it at the wrong size), no WindowedFilter resize
			// Resets, and the window state follows the game's own.
			if (!g_resetBypassed)
				logf("Reset happened without DisplayManager's params (Reset hook bypassed by another mod) - "
				     "passing everything through for this session");
			g_resetBypassed = true;
			g_active = g_winActive = g_borderlessActive = false;
			g_winFilterFailed = true;
			g_wantFullscreen = g_windowFs = !GAME_PP->Windowed;
		} else if (!g_resetSeen) {
			// With the entry hook every game Reset goes through myGameReset, so this is only one that started before
			// DM finished attaching (device-watch race): pass this one through, the next Reset is DM's again.
			static bool logged = false;
			if (!logged) { logged = true; logf("a Reset ran before DisplayManager was attached - passed through"); }
			g_active = g_winActive = g_borderlessActive = false;
		}
		g_resetSeen = false;
		if (dev && g_active) {
			createCapture(dev);
			setGameViewport(dev);
		}
	}
};
static DeviceListener g_deviceListener;

// Add g_deviceListener to the game's owner list (ADDR_ADD_DEVICE_LISTENER: EAX = context, stdcall-style 1 arg).
static void registerDeviceListener() {
	if (g_listenerOn) return;
	void *owner = &g_deviceListener;
	DWORD fn = ADDR_ADD_DEVICE_LISTENER;
	__asm {
		mov  eax, 0x008A0E10
		push owner
		call fn
	}
	g_listenerOn = true;
	logf("registered as a D3DPOOL_DEFAULT owner (released/recreated by the game's Reset wrapper)");
}

// Fallback (wrapper entry not hooked): the Reset vtable slot.
static HRESULT WINAPI myReset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp) {
	dpiThread();
	// Someone else's device (the vtable is shared), or DM stood down after a bypassed Reset.
	if (dev != GAME_DEVICE || g_resetBypassed) return oReset(dev, pp);   // (the listener keeps the state in sync)
	logf("Reset (vtable): Windowed=%d %ux%u", pp ? pp->Windowed : -1,
	     pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
	beforeReset(dev, pp);
	D3DPRESENT_PARAMETERS local, *use = pp;   // our changes go into a copy, never the game's struct
	if (pp) { local = *pp; use = &local; }
	// g_windowFs still says what the window was last set up for: fullscreen -> windowed predicts the window size
	// the post-Reset setup will give it; windowed -> windowed (our resize Reset, device-lost recovery) takes the
	// client area as it is.
	applyFullscreenParams(use, g_windowFs, g_spawnPending);
	HRESULT hr = callWithFallback("Reset", [dev](D3DPRESENT_PARAMETERS *p) { return oReset(dev, p); }, pp, use);
	if (pp) syncPresentParams(pp, &local);
	afterReset(dev, SUCCEEDED(hr), false);
	return hr;
}

// The wrapper's entry. The wrapper always resets with the global struct (its `push 0x8A0F68`, which call-site
// hooks keep), so DM's params go into that struct for the duration of the call only: the game's values are put
// back right after (plus whatever the runtime resolved), so the game and other mods never see DM's override
// outside the Reset - writing it permanently once broke leaving borderless with Alt+Enter. A failed Reset is
// retried through the wrapper (callWithFallback), as the game's own device-lost loop would.
static GameReset_t oGameReset = nullptr;   // trampoline: the wrapper's original prologue, then the rest of it

// Enter/leave the game's render lock (its CRITICAL_SECTION, recursive: the wrapper's own enter/leave nest) with
// kernel32 directly - NOT the way the wrapper calls it. SokuDirectXOptimizations redirects the wrapper's lock calls
// (0x415156 / 0x4151BC / 0x41520C) to functions that, with use_original_lock=0, also lock a std::mutex
// (_Mtx_try, not recursive: a second lock by the same thread returns busy and it throws - verified in-game, it
// crashed the Reset). Its present thread then waits on that mutex instead of this lock, so in that mode (default 1)
// it isn't held off between a failed attempt and the retry - the same exposure as the game's own device-lost retry
// loop has with it, so nothing DM can or need fix here.
static void renderLock(bool enter) {
	if (enter) EnterCriticalSection(GAME_RENDER_LOCK);
	else       LeaveCriticalSection(GAME_RENDER_LOCK);
}

static bool __cdecl myGameReset() {
	dpiThread();
	IDirect3DDevice9 *dev = GAME_DEVICE;
	// DM not attached (yet), standing down, or one of the wrapper's own early-outs: nothing to override.
	if (!g_enabled || !g_deviceHooked || !dev || !*reinterpret_cast<void **>(ADDR_GAME_D3D) ||
	    *reinterpret_cast<HRESULT *>(ADDR_GAME_TCL) == D3DERR_DEVICELOST)
		return oGameReset();
	D3DPRESENT_PARAMETERS *gp = GAME_PP;
	logf("Reset: Windowed=%d %ux%u", gp->Windowed, gp->BackBufferWidth, gp->BackBufferHeight);
	D3DPRESENT_PARAMETERS game = *gp;             // the game's values, as the wrapper would pass them
	game.BackBufferFormat = wrapperFormat(game.Windowed);
	beforeReset(dev, &game);        // (window bookkeeping, may SetWindowPos: outside the lock)
	// Hold the render lock from here to the end: the render thread must not composite a frame with the new output
	// geometry (computeOutput) into the old backbuffer, nor run one between a failed attempt and its retry - a
	// failed wrapper call returns with the lock released, the swapchain global (0x8A0E34) NULL and the game's
	// textures released, and Present reads that global unchecked (0x401078 / 0x4081CD). It also keeps DM's params in
	// the global invisible to anything that takes the lock. See renderLock for SokuDirectXOptimizations. Every attempt re-runs
	// the whole wrapper body: each owner's "lost" slot (ours: release + the overlays' reset) and ReplayInputView+'s
	// trampolines (0x415186 / 0x4151DD) - all safe to repeat, as the game's own device-lost loop does that too.
	renderLock(true);
	D3DPRESENT_PARAMETERS local = game;
	applyFullscreenParams(&local, g_windowFs, g_spawnPending);
	local.BackBufferFormat = wrapperFormat(local.Windowed);   // the wrapper re-derives it from OUR Windowed
	if (g_active) g_bbFormat = local.BackBufferFormat;
	D3DPRESENT_PARAMETERS used = local;
	HRESULT hr = callWithFallback("Reset", [&](D3DPRESENT_PARAMETERS *p) -> HRESULT {
		*gp = *p;
		bool ok = oGameReset();
		used = *gp;                               // what the runtime resolved / filled in
		*gp = game;
		if (ok) return S_OK;
		// The wrapper returns only a bool: tell a lost device (no fallback, the game retries) from a rejected mode.
		HRESULT tcl = dev->TestCooperativeLevel();
		return tcl == D3DERR_DEVICELOST ? D3DERR_DEVICELOST : FAILED(tcl) ? tcl : E_FAIL;
	}, &game, &local);
	syncPresentParams(&game, &used);
	*gp = game;
	// Our owner "reset" call didn't run (still g_resetSeen): DM got registered only while this Reset was under way
	// (device-watch attach), after the wrapper had walked the owner list. Recreate here, still under the lock.
	if (SUCCEEDED(hr) && g_resetSeen && g_active) {
		createCapture(dev);
		setGameViewport(dev);
	}
	renderLock(false);
	afterReset(dev, SUCCEEDED(hr), true);
	if (FAILED(hr)) logf("Reset failed (0x%08lx)", (long)hr);
	return SUCCEEDED(hr);
}

// Jump from the wrapper's entry to myGameReset. Only over the exact original bytes: if another mod already patched
// the entry, DM keeps to the vtable hook instead. Installed at startup, before the first Reset.
static void installGameResetHook() {
	BYTE *p = reinterpret_cast<BYTE *>(ADDR_GAME_RESET);
	if (memcmp(p, GAME_RESET_PROLOGUE, sizeof GAME_RESET_PROLOGUE) != 0) {
		logf("Reset wrapper entry 0x%08lx already patched (%02x %02x %02x %02x %02x %02x) - using the Reset vtable "
		     "hook", ADDR_GAME_RESET, p[0], p[1], p[2], p[3], p[4], p[5]);
		return;
	}
	BYTE *t = static_cast<BYTE *>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
	if (!t) return;
	memcpy(t, p, sizeof GAME_RESET_PROLOGUE);
	t[6] = 0xE9;   // jmp back to the instruction after the copied prologue
	*reinterpret_cast<DWORD *>(t + 7) = (ADDR_GAME_RESET + 6) - reinterpret_cast<DWORD>(t + 11);
	FlushInstructionCache(GetCurrentProcess(), t, 16);
	DWORD old;
	if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old)) {
		logf("Reset wrapper entry not writable (VirtualProtect error %lu) - using the Reset vtable hook",
		     GetLastError());
		VirtualFree(t, 0, MEM_RELEASE);
		return;
	}
	oGameReset = reinterpret_cast<GameReset_t>(t);
	p[0] = 0xE9;
	*reinterpret_cast<DWORD *>(p + 1) = reinterpret_cast<DWORD>(&myGameReset) - (ADDR_GAME_RESET + 5);
	p[5] = 0x90;
	VirtualProtect(p, 6, old, &old);
	FlushInstructionCache(GetCurrentProcess(), p, 6);
	g_gameResetHook = true;
	logf("Reset wrapper entry hooked");
}

// Hook the swapchain's Present. The swapchain vtable lives in d3d9.dll and is shared by every swapchain
// the game (re)creates, so patching it once survives device resets (which make a fresh swapchain).
static void hookSwapChain(IDirect3DDevice9 *dev) {
	IDirect3DSwapChain9 *sc = nullptr;
	if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
		void **vt = *(void ***)sc;
		hookSlot(vt, VT_SC_PRESENT, (void *)mySCPresent, (void **)&oSCPresent);
		sc->Release();
		logf("swapchain Present hooked");
	} else {
		logf("GetSwapChain failed - cannot hook Present");
	}
}

// Remember the runtime's own BeginScene/EndScene for drawSharp. They are trusted only if they live in the same
// module as the device's QueryInterface (which mods practically never hook): that is the D3D runtime itself
// (system d3d9, DXVK's d3d9_custom.dll, Wine), not another mod's hook. If a mod patched the slots before us,
// fall back to calling through the vtable with the stage render target (the old, redraw-safe path).
static HMODULE moduleOf(void *p) {
	HMODULE m = nullptr;
	if (p) GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                          (LPCSTR)p, &m);
	return m;
}
static HMODULE g_runtime = nullptr;   // the D3D runtime's module (where the device's QueryInterface lives)
static void captureSceneFns(void **vt) {
	HMODULE rt = moduleOf(vt[VT_UNK_QUERYINTERFACE]);
	g_runtime = rt;
	HMODULE mb = moduleOf(vt[VT_DEV_BEGINSCENE]), me = moduleOf(vt[VT_DEV_ENDSCENE]);
	g_sceneDirect = rt && mb == rt && me == rt;
	if (g_sceneDirect) {
		g_origBeginScene = (Scene_t)vt[VT_DEV_BEGINSCENE];
		g_origEndScene   = (Scene_t)vt[VT_DEV_ENDSCENE];
	}
	char name[MAX_PATH] = "?";
	if (rt) GetModuleFileNameA(rt, name, sizeof(name));
	logf("scene calls: %s (runtime %s)", g_sceneDirect ? "direct" : "via vtable + stage (BeginScene/EndScene already hooked)",
	     name);
}

// Single-shot: the CreateDevice hook and the device-watch thread can both get here (th123 passes &0x8A0E30
// as ppDevice, so the global is already set inside oCreateDevice, before this runs). Returns true only for
// the caller that actually hooked.
static volatile LONG g_deviceHookClaim = 0;
static bool hookDevice(IDirect3DDevice9 *dev) {
	if (!dev || InterlockedCompareExchange(&g_deviceHookClaim, 1, 0) != 0) return false;
	void **vt = *(void ***)dev;
	captureSceneFns(vt);
	if (!g_gameResetHook) hookSlot(vt, VT_DEV_RESET, (void *)myReset, (void **)&oReset);
	hookSlot(vt, VT_DEV_SETRT, (void *)mySetRenderTarget, (void **)&oSetRenderTarget);
	hookSwapChain(dev);
	installSharpSprites(dev);
	g_deviceHooked = true;     // before registering: a Reset that calls our owner must also find DM attached
	registerDeviceListener();
	logf("device vtable hooked (%sSetRenderTarget) + swapchain Present", g_gameResetHook ? "" : "Reset, ");
	return true;
}

// Re-hook SetRenderTarget (the mid-frame viewport pin) and, with sharp sprites, DrawPrimitiveUP when a vtable reset
// (see "device Reset") put the slot back to the runtime's own function. Only then: if another mod's hook sits there,
// it is left alone - two mods that each re-hook over the other would end up calling each other forever. Render
// thread, every Present; `seen` caches the last foreign value checked, so moduleOf only runs when the slot changes.
// The Reset slot is deliberately NOT re-hooked in fallback mode: in-game, every Reset after such a re-hook failed
// (D3DERR_INVALIDCALL, even straight into the runtime with DM's targets released; not reproducible outside th123),
// while without it the bypass is detected and DM stands down cleanly.
struct HealedSlot { int index; void *hook; void **orig; const char *name; void *seen; unsigned count; };
static void healSlot(void **vt, HealedSlot &s) {
	void *cur = vt[s.index];
	if (cur == s.hook || cur == s.seen) return;
	if (!g_runtime || moduleOf(cur) != g_runtime) { s.seen = cur; return; }
	hookSlot(vt, s.index, s.hook, s.orig);
	s.seen = nullptr;
	s.count++;
	if (s.count <= 3 || s.count % 100 == 0)   // the count shows whether it happens every frame; can't flood the log
		logf("%s hook was dropped (device vtable reset by the runtime) - hooked again (%u so far)", s.name, s.count);
}
static void healSlots(IDirect3DDevice9 *dev) {
	static HealedSlot setRT = { VT_DEV_SETRT, (void *)mySetRenderTarget, (void **)&oSetRenderTarget, "SetRenderTarget" };
	static HealedSlot drawUP = { VT_DEV_DRAWPRIMUP, (void *)myDrawPrimitiveUP, (void **)&oDrawPrimitiveUP,
	                             "DrawPrimitiveUP" };
	void **vt = *(void ***)dev;
	healSlot(vt, setRT);
	if (g_sprWanted) healSlot(vt, drawUP);
}

// WindowResizer and the older IntegerFullscreen / ExclusiveFullscreen do the same job; together with them DM
// would process every frame twice and fight over the window. They may load after our Initialize, so this is
// checked when the game's device is created (or found by the device watch). If one is loaded, DM stands down
// completely (clears g_enabled: no hooks, override, post-process, window management, hotkeys or ini writes).
static const char *const CONFLICTING_MODS[] = { "WindowResizer.dll", "IntegerFullscreen.dll",
                                                "ExclusiveFullscreen.dll" };
static bool conflictingModLoaded() {
	for (const char *name : CONFLICTING_MODS) {
		if (GetModuleHandleA(name)) {
			logf("%s is loaded - DisplayManager is passing everything through (disabled). Use only one of "
			     "them.", name);
			g_enabled = false;
			return true;
		}
	}
	return false;
}

// Does `fn` point into executable code of a loaded module? Used to confirm the device global holds a
// finished device with a sane vtable before we patch it. (Deliberately not "inside d3d9.dll": in the usual
// setup GetModuleHandle("d3d9.dll") is the SokuModLoader proxy in the game folder, while the real device
// comes from System32's d3d9, a d3d9_custom.dll, or DXVK.)
static bool isExecutableImage(void *fn) {
	MEMORY_BASIC_INFORMATION mbi;
	if (!fn || !VirtualQuery(fn, &mbi, sizeof(mbi))) return false;
	return mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE &&
	       (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

// Fallback path when our Direct3DCreate9 hook never fires - e.g. SokuDirectXOptimizations creates a
// Direct3D9Ex device (via Direct3DCreate9Ex, a different export) or otherwise intercepts creation. We
// poll the game's device global (0x8A0E30) and hook the device once it exists, regardless of who made it.
// Best-effort only: it stands down as soon as the normal CreateDevice hook is in place. Resets are caught at
// the wrapper's entry (hooked at startup), so a late attach no longer misses them.
static DWORD WINAPI deviceWatchThread(LPVOID) {
	dpiThread();
	for (int i = 0; i < 1200 && !g_deviceHooked && !g_createDeviceHooked; i++) {   // ~60s
		IDirect3DDevice9 *dev = GAME_DEVICE;
		void **vt = dev ? *(void ***)dev : nullptr;
		if (vt && isExecutableImage(vt[VT_DEV_RESET]) && isExecutableImage(vt[VT_DEV_PRESENT])) {
			if (!g_enabled || conflictingModLoaded()) return 0;
			IDirect3DSwapChain9 *sc = nullptr;
			BOOL windowed = TRUE;
			if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
				D3DPRESENT_PARAMETERS pp = {0};
				if (SUCCEEDED(sc->GetPresentParameters(&pp))) {
					if (pp.hDeviceWindow) g_hwnd = pp.hDeviceWindow;
					windowed = pp.Windowed;
				}
				sc->Release();
			}
			if (!g_hwnd) {
				D3DDEVICE_CREATION_PARAMETERS cp = {0};
				if (SUCCEEDED(dev->GetCreationParameters(&cp))) g_hwnd = cp.hFocusWindow;
			}
			if (!hookDevice(dev)) return 0;        // the CreateDevice path got there first
			logf("device watch: found device %p (hwnd %p) - hooked without CreateDevice", dev, g_hwnd);
			installWndProc();
			installKeyboardHook();
			g_wantFullscreen = g_windowFs = !windowed;   // attached mid-fullscreen: wait for the next Reset
			if (windowed) postWindowApply(true);
			return 0;
		}
		Sleep(50);
	}
	if (!g_deviceHooked && !g_createDeviceHooked)
		logf("device watch: no hookable d3d9 device found. If SokuDirectXOptimizations is enabled, set "
		     "use_d3d9ex=0 - its 9Ex path wraps the device and DisplayManager can't attach to it.");
	return 0;
}

// ---- IDirect3D9::CreateDevice hook ---------------------------------------------------------------
static HRESULT WINAPI myCreateDevice(IDirect3D9 *self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                     DWORD behavior, D3DPRESENT_PARAMETERS *pp,
                                     IDirect3DDevice9 **out) {
	dpiThread();
	// Only the game's own call (it passes &GAME_DEVICE) is ours; a device another mod or overlay creates
	// must not touch the game window, monitor or present params.
	if (out != &GAME_DEVICE) {
		logf("CreateDevice from another caller (ppDevice=%p) - passed through", (void *)out);
		return oCreateDevice(self, adapter, type, focus, behavior, pp, out);
	}
	if (!g_enabled || conflictingModLoaded())
		return oCreateDevice(self, adapter, type, focus, behavior, pp, out);
	logf("CreateDevice: Windowed=%d %ux%u", pp ? pp->Windowed : -1,
	     pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
	if (pp) logf("  game pp: fmt=%d count=%u ms=%d swap=%d autoDepth=%d depthFmt=%d flags=0x%lx interval=0x%x",
	             (int)pp->BackBufferFormat, pp->BackBufferCount, (int)pp->MultiSampleType, (int)pp->SwapEffect,
	             pp->EnableAutoDepthStencil, (int)pp->AutoDepthStencilFormat, (unsigned long)pp->Flags,
	             pp->PresentationInterval);
	g_adapterMon = self->GetAdapterMonitor(adapter);
	g_d3d = self; g_adapter = adapter;
	D3DPRESENT_PARAMETERS local, *use = pp;   // our changes go into a copy, never the game's struct
	if (pp) { local = *pp; use = &local; }
	if (pp && !g_hwnd) g_hwnd = pp->hDeviceWindow ? pp->hDeviceWindow : focus;   // for the window-size prediction
	applyFullscreenParams(use, true, true);   // sets g_hwnd from pp->hDeviceWindow; first spawn = the saved size
	if (!g_hwnd && focus) g_hwnd = focus;
	installKeyboardHook();              // hooks the WINDOW's thread (not necessarily this one)
	HRESULT hr = callWithFallback("CreateDevice", [=](D3DPRESENT_PARAMETERS *p) {
		return oCreateDevice(self, adapter, type, focus, behavior, p, out);
	}, pp, use);
	if (pp) syncPresentParams(pp, &local);
	if (SUCCEEDED(hr) && *out) {           // out == &GAME_DEVICE here, never null
		hookDevice(*out);
		if (g_active) {
			createCapture(*out);
			setGameViewport(*out);
		}
		installWndProc();               // subclass the window for drag-resize aspect locking
		// First spawn (th123 always creates windowed): saved scale + spawn position. Deferred like the
		// Reset path, so it lands after the game's own window setup instead of racing it.
		g_windowFs = g_wantFullscreen;
		postWindowApply(true);
	}
	return hr;
}

// ---- Direct3DCreate9 hook (installs the CreateDevice hook once the D3D object exists) -------------
static IDirect3D9 *WINAPI myDirect3DCreate9(UINT sdkVersion) {
	IDirect3D9 *d3d = oDirect3DCreate9(sdkVersion);
	if (d3d && !g_createDeviceHooked) {
		void **vt = *(void ***)d3d;
		hookSlot(vt, VT_D3D9_CREATEDEVICE, (void *)myCreateDevice, (void **)&oCreateDevice);
		g_createDeviceHooked = true;
		logf("IDirect3D9 created, CreateDevice hooked");
	}
	return d3d;
}

// ---- setup ---------------------------------------------------------------------------------------
// Parse an integer scale written as "x2", "X3", or plain "2".
static int parseScale(const char *s) {
	while (*s == ' ' || *s == '\t') s++;
	if (*s == 'x' || *s == 'X') s++;
	int n = atoi(s);
	return n < 1 ? 1 : n;
}

// Parse an "RRGGBB" hex color (optionally prefixed with '#' or "0x") into a D3DCOLOR.
static D3DCOLOR parseColor(const char *s, D3DCOLOR fallback) {
	while (*s == ' ' || *s == '\t') s++;
	if (*s == '#') s++;
	else if ((s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) s += 2;
	char *end = nullptr;
	unsigned long v = strtoul(s, &end, 16);
	if (end == s) return fallback;
	return D3DCOLOR_XRGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

// Parse a hotkey key (a single letter or digit) into a virtual-key code. A-Z / 0-9 have VK == ASCII of
// the uppercase character. Empty = 0 = disabled.
static int parseKey(const char *s) {
	while (*s == ' ' || *s == '\t') s++;
	char c = *s;
	if (c >= 'a' && c <= 'z') c -= 32;
	if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (unsigned char)c;
	return 0;
}

// Old (<= 1.0.3) name of the FullscreenScale key.
static const char *LEGACY_FS_SCALE_KEY = "IntegerScaling";

static bool iniHasKey(const char *key) {
	char v[8] = {0};
	GetPrivateProfileStringA("Display", key, "\x01", v, sizeof(v), g_iniPath);   // sentinel = missing
	return lstrcmpA(v, "\x01") != 0;
}

// ---- ini upgrade -------------------------------------------------------------------------------------
// The shipped DisplayManager.ini is embedded in the DLL (DisplayManager.rc) and IniVersion in the ini records the
// version that last wrote it. When that is older than this build, the ini is rebuilt from the embedded one with the
// user's settings carried over, so an upgraded ini has exactly the keys, order and comments of a fresh install. For
// each key in the template:
//   - set in the user's ini           -> Key=<their value> (also over a template line that ships commented out)
//   - commented out in the user's ini -> their commented line (a commented-out hotkey stays disabled)
//   - absent                          -> the template line (a key their version didn't have yet)
// Keys the template doesn't have (e.g. MultiSample, which is added by hand) are kept at the end of their section and
// unknown sections at the end of the file; the user's own comments are not kept. Values whose meaning changed are
// fixed up first (fixupIniValues). An ini from a newer version is left alone.

// "a.b.c[.d]" as one comparable number (0 for a missing / blank version: older than any).
static ULONGLONG parseVersion(const char *s) {
	ULONGLONG v = 0;
	for (int i = 0; i < 4; i++) {
		v = (v << 16) | (WORD)StrToIntA(s);
		while (*s >= '0' && *s <= '9') s++;
		if (*s == '.') s++;
	}
	return v;
}
#define INI_VERSION(a, b, c) (((ULONGLONG)(a) << 48) | ((ULONGLONG)(b) << 32) | ((ULONGLONG)(c) << 16))

static ULONGLONG iniVersion() {
	char v[32] = {0};
	GetPrivateProfileStringA("Display", "IniVersion", "", v, sizeof(v), g_iniPath);
	return parseVersion(v);
}

// Before 1.2.0, Sharpness and the Xbr* knobs had fixed defaults (shipped in every ini). Those exact values in an older
// ini mean "the default", which is Auto (the value picked for the output size) since 1.2.0; anything else was chosen.
static const char *const PRE_AUTO_KEYS[] = { "Sharpness", "XbrStrength", "XbrCorner", "XbrSlopes", "XbrWidth" };
static bool isPreAutoDefault(const char *key, const char *val) {
	if (!lstrcmpiA(key, "XbrCorner")) return !lstrcmpiA(val, "B");
	if (!val[0] || !lstrcmpiA(val, "Auto")) return false;
	const float v = (float)atof(val);
	const float def = !lstrcmpiA(key, "Sharpness") ? 1.5f : !lstrcmpiA(key, "XbrStrength") ? 0.65f :
	                  !lstrcmpiA(key, "XbrWidth") ? 2.0f : 0.0f;   // XbrSlopes: 0
	return fabsf(v - def) < 0.001f;
}

// One ini line. Keys: `active` = Key=value, otherwise a commented-out key (";Key=value", no blank after the ';').
struct IniLine {
	enum Kind { OTHER, SECTION, KEY, COMMENTED_KEY } kind;
	std::string section, key, value, raw;
	bool used;
};

static std::string trimmed(const std::string &s) {
	size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
	return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

static std::vector<IniLine> parseIni(const char *text) {
	std::vector<IniLine> out;
	std::string section;
	for (const char *p = text; *p; ) {
		const char *nl = strchr(p, '\n');
		std::string raw(p, nl ? nl - p : strlen(p));
		p = nl ? nl + 1 : p + raw.size();
		if (!raw.empty() && raw.back() == '\r') raw.pop_back();
		IniLine l = { IniLine::OTHER, section, "", "", raw, false };
		std::string t = trimmed(raw);
		if (!t.empty() && t[0] == '[') {
			size_t close = t.find(']');
			section = l.section = t.substr(1, close == std::string::npos ? std::string::npos : close - 1);
			l.kind = IniLine::SECTION;
		} else if (!t.empty()) {
			bool commented = t[0] == ';';
			size_t k = commented ? 1 : 0, eq = t.find('=');
			std::string key = eq == std::string::npos ? "" : trimmed(t.substr(k, eq - k));
			bool ident = !key.empty() && t[k] != ' ' && t[k] != '\t';
			for (char c : key) ident = ident && (IsCharAlphaNumericA(c) || c == '_');
			if (ident) {
				l.kind = commented ? IniLine::COMMENTED_KEY : IniLine::KEY;
				l.key = key;
				l.value = trimmed(t.substr(eq + 1));
			}
		}
		out.push_back(l);
	}
	if (!out.empty() && out.back().raw.empty()) out.pop_back();   // the file's final newline
	return out;
}

// The first line of `kind` for section/key in `lines` (case-insensitive, like GetPrivateProfileString), or nullptr.
static IniLine *findIniKey(std::vector<IniLine> &lines, IniLine::Kind kind, const std::string &section,
                           const std::string &key) {
	for (IniLine &l : lines)
		if (l.kind == kind && !lstrcmpiA(l.section.c_str(), section.c_str()) && !lstrcmpiA(l.key.c_str(), key.c_str()))
			return &l;
	return nullptr;
}

// Mark every line of section/key (any kind) as carried over, so duplicates aren't appended as unknown keys.
static void markIniKeyUsed(std::vector<IniLine> &lines, const std::string &section, const std::string &key) {
	for (IniLine &l : lines)
		if (l.kind != IniLine::OTHER && l.kind != IniLine::SECTION &&
		    !lstrcmpiA(l.section.c_str(), section.c_str()) && !lstrcmpiA(l.key.c_str(), key.c_str()))
			l.used = true;
}

// Values whose name or meaning changed, rewritten into what this version's template means by them.
static void fixupIniValues(std::vector<IniLine> &user, ULONGLONG from) {
	// FullscreenScale was called IntegerScaling up to 1.0.3 (if both are there, FullscreenScale wins, as in loadConfig).
	if (IniLine *old = findIniKey(user, IniLine::KEY, "Display", LEGACY_FS_SCALE_KEY)) {
		if (findIniKey(user, IniLine::KEY, "Display", "FullscreenScale")) markIniKeyUsed(user, "Display", LEGACY_FS_SCALE_KEY);
		else old->key = "FullscreenScale";
	}
	// SpriteRestStrength (and its hotkeys), from unreleased 1.1.3 builds, is SpriteNearStrength (the new name wins, as
	// in loadConfig).
	static const char *const renamed[][3] = { { "Display", "SpriteRestStrength", "SpriteNearStrength" },
	                                          { "Hotkeys", "SpriteRestStrengthDown", "SpriteNearStrengthDown" },
	                                          { "Hotkeys", "SpriteRestStrengthUp", "SpriteNearStrengthUp" } };
	for (const auto &r : renamed)
		if (IniLine *old = findIniKey(user, IniLine::KEY, r[0], r[1])) {
			if (findIniKey(user, IniLine::KEY, r[0], r[2])) markIniKeyUsed(user, r[0], r[1]);
			else old->key = r[2];
		}
	// The old fixed defaults of Sharpness / Xbr* become Auto (see isPreAutoDefault). Filter is left as it is.
	if (from < INI_VERSION(1, 2, 0))
		for (const char *k : PRE_AUTO_KEYS)
			if (IniLine *l = findIniKey(user, IniLine::KEY, "Display", k))
				if (isPreAutoDefault(k, l->value.c_str())) l->value = "Auto";
	// A missing WindowScale (before 1.0.3) means the FullscreenScale value (loadConfig's fallback).
	IniLine *fs = findIniKey(user, IniLine::KEY, "Display", "FullscreenScale");
	if (fs && !findIniKey(user, IniLine::KEY, "Display", "WindowScale")) {
		IniLine ws = *fs;
		ws.key = "WindowScale";
		user.push_back(ws);
	}
	// Up to 1.1.2 a negative (or missing) PositionX/Y meant "don't move the window", which is spelled blank now.
	if (from < INI_VERSION(1, 1, 3)) {
		IniLine *x = findIniKey(user, IniLine::KEY, "Display", "PositionX");
		IniLine *y = findIniKey(user, IniLine::KEY, "Display", "PositionY");
		if (!x || !y || StrToIntA(x->value.c_str()) < 0 || StrToIntA(y->value.c_str()) < 0) {
			if (x) x->value.clear();
			if (y) y->value.clear();
		}
	}
}

// Append the user's not yet carried over keys of `section` to `out`, before the blank lines that end the section.
static void appendLeftoverKeys(std::vector<std::string> &out, std::vector<IniLine> &user, const std::string &section) {
	if (section.empty()) return;                                // keys before any section are never read
	size_t at = out.size();
	while (at > 0 && trimmed(out[at - 1]).empty()) at--;
	std::vector<std::string> add;
	for (IniLine &l : user)
		if (l.kind == IniLine::KEY && !l.used && !lstrcmpiA(l.section.c_str(), section.c_str())) {
			add.push_back(l.key + "=" + l.value);
			markIniKeyUsed(user, l.section, l.key);
		}
	out.insert(out.begin() + at, add.begin(), add.end());
}

// The template merged with the user's ini (see above), as the lines of the new file.
static std::vector<std::string> mergeIni(std::vector<IniLine> &tmpl, std::vector<IniLine> &user) {
	std::vector<std::string> out;
	std::string section;
	for (IniLine &t : tmpl) {
		if (t.kind == IniLine::SECTION) {
			appendLeftoverKeys(out, user, section);
			section = t.section;
			out.push_back(t.raw);
		} else if (t.kind == IniLine::KEY && !lstrcmpiA(t.key.c_str(), "IniVersion")) {
			out.push_back(t.key + "=" + DM_VERSION);
			markIniKeyUsed(user, section, t.key);
		} else if (t.kind == IniLine::KEY || t.kind == IniLine::COMMENTED_KEY) {
			IniLine *u = findIniKey(user, IniLine::KEY, section, t.key);
			IniLine *c = t.kind == IniLine::KEY && !u ? findIniKey(user, IniLine::COMMENTED_KEY, section, t.key) : nullptr;
			out.push_back(u ? t.key + "=" + u->value : c ? c->raw : t.raw);
			markIniKeyUsed(user, section, t.key);
		} else {
			out.push_back(t.raw);
		}
	}
	appendLeftoverKeys(out, user, section);
	for (IniLine &s : user) {                                   // sections the template doesn't have
		if (s.kind != IniLine::SECTION || s.used) continue;
		bool known = false;
		for (IniLine &t : tmpl) known = known || (t.kind == IniLine::SECTION && !lstrcmpiA(t.section.c_str(), s.section.c_str()));
		for (IniLine &o : user) if (o.kind == IniLine::SECTION && !lstrcmpiA(o.section.c_str(), s.section.c_str())) o.used = true;
		if (known) continue;
		out.push_back("");
		out.push_back("[" + s.section + "]");
		appendLeftoverKeys(out, user, s.section);
	}
	return out;
}

// The ini as one NUL-terminated buffer (nullptr if it's missing, unreadable, over 1 MB or UTF-16); free() it.
static char *readIniFile() {
	HANDLE f = CreateFileA(g_iniPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (f == INVALID_HANDLE_VALUE) return nullptr;
	DWORD n = GetFileSize(f, nullptr), got = 0;
	char *buf = (n != INVALID_FILE_SIZE && n < (1u << 20)) ? (char *)malloc(n + 1) : nullptr;
	bool ok = buf && ReadFile(f, buf, n, &got, nullptr) && got == n;
	CloseHandle(f);
	if (!ok || (n >= 2 && (BYTE)buf[0] == 0xFF && (BYTE)buf[1] == 0xFE)) { free(buf); return nullptr; }
	buf[n] = 0;
	return buf;
}

// Replace the ini with `text`, through a temp file. False (ini untouched) on any I/O error.
static bool writeIniFile(const std::string &text) {
	char tmp[1024 + MAX_PATH + 8];
	wsprintfA(tmp, "%s.tmp", g_iniPath);
	HANDLE o = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (o == INVALID_HANDLE_VALUE) return false;
	DWORD w = 0;
	bool wrote = WriteFile(o, text.data(), (DWORD)text.size(), &w, nullptr) && w == text.size();
	CloseHandle(o);
	if (wrote && MoveFileExA(tmp, g_iniPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
	DeleteFileA(tmp);
	return false;
}

// Rebuild an ini older than this build from the embedded template (see above). Only when the user lets us write
// the ini (PersistState or PersistPosition); skipped for UTF-16 inis and on any error, which leaves the file as it
// is - loadConfig still reads old inis correctly.
static void upgradeIni() {
	const ULONGLONG from = iniVersion();
	if (from >= parseVersion(DM_VERSION) || !(g_persist || g_persistPos)) return;
	HRSRC res = FindResourceA(g_module, "DM_DEFAULT_INI", MAKEINTRESOURCEA(10));   // RT_RCDATA
	HGLOBAL h = res ? LoadResource(g_module, res) : nullptr;
	const char *data = h ? (const char *)LockResource(h) : nullptr;
	char *cur = data ? readIniFile() : nullptr;
	if (!cur) return;
	std::vector<IniLine> tmpl = parseIni(std::string(data, SizeofResource(g_module, res)).c_str());
	std::vector<IniLine> user = parseIni(cur);
	free(cur);
	fixupIniValues(user, from);
	std::string text;
	for (const std::string &l : mergeIni(tmpl, user)) text += l + "\r\n";
	char old[32] = {0};
	GetPrivateProfileStringA("Display", "IniVersion", "", old, sizeof(old), g_iniPath);
	if (writeIniFile(text))
		logf("ini upgraded from %s to %s", old[0] ? old : "an unversioned ini (1.1.2 or older)", DM_VERSION);
}

static void loadConfig() {
	GetModuleFileNameA(g_module, g_iniPath, 1024);
	PathRemoveFileSpecA(g_iniPath);
	PathAppendA(g_iniPath, "DisplayManager.ini");
	g_enabled = GetPrivateProfileIntA("Display", "Enabled", 1, g_iniPath) != 0;
	g_log = GetPrivateProfileIntA("Display", "Log", 0, g_iniPath) != 0;   // early, so upgradeIni can log
	g_persist = GetPrivateProfileIntA("Display", "PersistState", 1, g_iniPath) != 0;
	g_persistPos = GetPrivateProfileIntA("Display", "PersistPosition", 1, g_iniPath) != 0;
	upgradeIni();
	const ULONGLONG iniVer = iniVersion();   // still older than this build if the ini couldn't be upgraded

	char mode[64] = {0};
	GetPrivateProfileStringA("Display", "Mode", "FitToScreen", mode, sizeof(mode), g_iniPath);
	if (StrCmpIA(mode, "IntegerScaling") == 0)        g_mode = MODE_INTEGER;
	else if (StrCmpIA(mode, "CustomResolution") == 0) g_mode = MODE_CUSTOM;
	else                                              g_mode = MODE_FIT;

	// FullscreenScale was called IntegerScaling up to 1.0.3: read the old name when the new one is missing
	// (upgradeIni renames it in the file).
	char scale[32] = {0};
	GetPrivateProfileStringA("Display", "FullscreenScale", "", scale, sizeof(scale), g_iniPath);
	if (!scale[0]) GetPrivateProfileStringA("Display", LEGACY_FS_SCALE_KEY, "x2", scale, sizeof(scale), g_iniPath);
	g_intScale = parseScale(scale);
	// The window scale defaults to the fullscreen scale, which versions before 1.0.3 used for the window too.
	char wscale[32] = {0};
	GetPrivateProfileStringA("Display", "WindowScale", scale, wscale, sizeof(wscale), g_iniPath);
	g_winScale = parseScale(wscale);

	g_customW = GetPrivateProfileIntA("Display", "CustomWidth", 1280, g_iniPath);
	g_customH = GetPrivateProfileIntA("Display", "CustomHeight", 960, g_iniPath);

	char color[32] = {0};
	GetPrivateProfileStringA("Display", "BackgroundColor", "000000", color, sizeof(color), g_iniPath);
	g_bgColor = parseColor(color, D3DCOLOR_XRGB(0, 0, 0));

	char filt[32] = {0};
	GetPrivateProfileStringA("Display", "Filter", "Point", filt, sizeof(filt), g_iniPath);
	if      (StrCmpIA(filt, "Sharp") == 0)  g_filterCfg = FILTER_SHARP;
	else if (StrCmpIA(filt, "Linear") == 0) g_filterCfg = FILTER_LINEAR;
	else if (StrCmpIA(filt, "xBR") == 0)    g_filterCfg = FILTER_XBR;
	else if (StrCmpIA(filt, "Auto") == 0)   g_filterCfg = FILTER_AUTO;
	else                                    g_filterCfg = FILTER_POINT;   // the default

	// Sharpness and the Xbr* knobs: Auto (also blank or missing) = the output scale's value (SCALE_DEFAULTS).
	char sharp[32] = {0};
	GetPrivateProfileStringA("Display", "Sharpness", "", sharp, sizeof(sharp), g_iniPath);
	// An older ini that couldn't be upgraded (PersistState=0 and PersistPosition=0) still means Auto by its old default.
	const bool preAuto = iniVersion() < INI_VERSION(1, 2, 0);
	g_auto[AUTO_SHARP] = !sharp[0] || StrCmpIA(sharp, "Auto") == 0 || (preAuto && isPreAutoDefault("Sharpness", sharp));
	if (!g_auto[AUTO_SHARP]) {
		g_sharpness = (float)atof(sharp);
		clampSharpness();
		g_cfgSharp = g_sharpness;
	}

	g_resizable = GetPrivateProfileIntA("Display", "Resizable", 1, g_iniPath) != 0;
	{   // PositionX/Y: blank (or missing) = leave the window where the game puts it; any number is a coordinate
		char px[16] = {0}, py[16] = {0};
		GetPrivateProfileStringA("Display", "PositionX", "", px, sizeof(px), g_iniPath);
		GetPrivateProfileStringA("Display", "PositionY", "", py, sizeof(py), g_iniPath);
		g_posX = StrToIntA(px); g_posY = StrToIntA(py);
		g_havePos = px[0] && py[0];
		if (iniVer < INI_VERSION(1, 1, 3) && (g_posX < 0 || g_posY < 0)) g_havePos = false;   // the old "don't move"
	}
	g_borderless   = GetPrivateProfileIntA("Display", "Borderless", 0, g_iniPath) != 0;
	g_winFilter    = GetPrivateProfileIntA("Display", "WindowedFilter", 1, g_iniPath) != 0;
	g_fsW          = GetPrivateProfileIntA("Display", "FullscreenWidth", 0, g_iniPath);
	g_fsH          = GetPrivateProfileIntA("Display", "FullscreenHeight", 0, g_iniPath);
	g_fsRefresh    = GetPrivateProfileIntA("Display", "FullscreenRefresh", 0, g_iniPath);
	g_vsync        = GetPrivateProfileIntA("Display", "VSync", -1, g_iniPath);
	g_allowWinKey  = GetPrivateProfileIntA("Input", "AllowWinKey", 0, g_iniPath) != 0;
	g_dpiAware     = GetPrivateProfileIntA("Display", "DpiAware", 1, g_iniPath) != 0;
	g_latinPending = GetPrivateProfileIntA("Input", "StartInLatinInput", 0, g_iniPath) != 0;
	g_msaaCfg      = GetPrivateProfileIntA("Display", "MultiSample", 0, g_iniPath);
	g_msaaIni      = g_msaaCfg;
	{   // xBR knobs
		static const char *const xbrKeys[4] = { "XbrStrength", "XbrCorner", "XbrSlopes", "XbrWidth" };
		char v[4][32] = {};
		for (int i = 0; i < 4; i++) {
			GetPrivateProfileStringA("Display", xbrKeys[i], "", v[i], sizeof(v[i]), g_iniPath);
			g_auto[AUTO_XBR_STRENGTH + i] = !v[i][0] || StrCmpIA(v[i], "Auto") == 0 ||
			                                (preAuto && isPreAutoDefault(xbrKeys[i], v[i]));
		}
		if (!g_auto[AUTO_XBR_STRENGTH]) {
			g_xbrStrength = (float)atof(v[0]);
			if (g_xbrStrength < 0.0f) g_xbrStrength = 0.0f;
			if (g_xbrStrength > 1.0f) g_xbrStrength = 1.0f;
		}
		if (!g_auto[AUTO_XBR_CORNER]) {
			char cc = v[1][0] >= 'a' ? v[1][0] - 32 : v[1][0];
			g_xbrCorner = (cc >= 'A' && cc <= 'D') ? cc - 'A' : 1;
		}
		if (!g_auto[AUTO_XBR_SLOPES]) g_xbrSlopes = StrToIntA(v[2]) != 0;
		if (!g_auto[AUTO_XBR_WIDTH]) {
			g_xbrWidth = (float)atof(v[3]);
			if (g_xbrWidth < 0.05f) g_xbrWidth = 0.05f;
			if (g_xbrWidth > 4.0f) g_xbrWidth = 4.0f;
		}
	}
	{   // sharp sprites (experimental): commented out, blank or 0 = off
		static const char *const keys[SPR_LAYERS] = { "SpriteSharpness", "BackgroundSharpness" };
		for (int l = 0; l < SPR_LAYERS; l++) {
			char v[32] = {0};
			GetPrivateProfileStringA("Display", keys[l], "", v, sizeof(v), g_iniPath);
			float k = (float)atof(v);
			if (k <= 0.0f) continue;
			if (k < SPR_K_MIN) k = SPR_K_MIN;
			if (k > SPR_K_MAX) k = SPR_K_MAX;
			g_cfgSprK[l] = g_sprLastK[l] = k;
			g_sprLastKSet[l] = true;
		}
		// Strengths, 0..1; commented out / blank = the default. SpriteNearStrength was SpriteRestStrength in unreleased
		// 1.1.3 builds.
		static const char *const restKeys[] = { "SpriteNearStrength", "SpriteFarStrength", "SpriteRestStrength" };
		float *const restVals[] = { &g_cfgSprNear, &g_cfgSprFar, &g_cfgSprNear };
		bool haveNear = false;
		for (int i = 0; i < 3; i++) {
			if (i == 2 && haveNear) break;
			char v[32] = {0};
			GetPrivateProfileStringA("Display", restKeys[i], "", v, sizeof(v), g_iniPath);
			if (!v[0]) continue;
			float r = (float)atof(v);
			*restVals[i] = r < 0.0f ? 0.0f : r > 1.0f ? 1.0f : r;
			if (i == 0) haveNear = true;
		}
		// BackgroundRestStrength / SpriteFarZoom: Auto (also blank or missing) = the output scale's value.
		char v[32] = {0};
		GetPrivateProfileStringA("Display", "BackgroundRestStrength", "", v, sizeof(v), g_iniPath);
		g_auto[AUTO_BGREST] = !v[0] || StrCmpIA(v, "Auto") == 0;
		if (!g_auto[AUTO_BGREST]) {
			float r = (float)atof(v);
			g_cfgBgRest = r < 0.0f ? 0.0f : r > 1.0f ? 1.0f : r;
		}
		GetPrivateProfileStringA("Display", "SpriteFarZoom", "", v, sizeof(v), g_iniPath);
		g_auto[AUTO_FARZOOM] = !v[0] || StrCmpIA(v, "Auto") == 0;
		if (!g_auto[AUTO_FARZOOM]) {
			float z = (float)atof(v);
			g_cfgFarZoom = z < SPR_FARZOOM_MIN ? SPR_FARZOOM_MIN : z > SPR_FARZOOM_MAX ? SPR_FARZOOM_MAX : z;
		}
	}

	// [Hotkeys]: a missing/commented/blank key line disables that hotkey.
	char modn[32] = {0};
	GetPrivateProfileStringA("Hotkeys", "Modifier", "Alt", modn, sizeof(modn), g_iniPath);
	if      (StrCmpIA(modn, "Ctrl") == 0 || StrCmpIA(modn, "Control") == 0) g_modifier = MODK_CTRL;
	else if (StrCmpIA(modn, "Shift") == 0)                                  g_modifier = MODK_SHIFT;
	else if (StrCmpIA(modn, "Win") == 0)                                    g_modifier = MODK_WIN;
	else if (StrCmpIA(modn, "None") == 0)                                   g_modifier = MODK_NONE;
	else                                                                    g_modifier = MODK_ALT;

	const char *names[ACT_COUNT] = { "FitToScreen", "Scale1", "Scale2", "Scale3",
	                                 "Scale4", "Scale5", "Scale6", "AlwaysOnTop", "CycleFilter",
	                                 "SharpnessDown", "SharpnessUp", "ToggleMSAA", "XbrStrength", "XbrCorner",
	                                 "XbrSlopes", "XbrWidth", "SharpSprites", "SpriteSharpnessDown",
	                                 "SpriteSharpnessUp", "SharpBackground", "BackgroundSharpnessDown",
	                                 "BackgroundSharpnessUp", "SpriteNearStrengthDown", "SpriteNearStrengthUp",
	                                 "BackgroundRestStrengthDown", "BackgroundRestStrengthUp", "SpriteFarStrengthDown",
	                                 "SpriteFarStrengthUp", "SpriteFarZoomDown", "SpriteFarZoomUp", "Menu" };
	for (int a = 0; a < ACT_COUNT; a++) {
		char k[16] = {0};
		GetPrivateProfileStringA("Hotkeys", names[a], "", k, sizeof(k), g_iniPath);
		g_hotkeyVk[a] = parseKey(k);
	}
	for (int a = 0; a < ACT_COUNT; a++)     // the keyboard hook runs the first match: a later duplicate never fires
		for (int b = a + 1; b < ACT_COUNT; b++)
			if (g_hotkeyVk[a] && g_hotkeyVk[a] == g_hotkeyVk[b])
				logf("hotkeys: %s and %s are both %c - only %s works", names[a], names[b], g_hotkeyVk[a], names[a]);
	// The game's draws are hooked only if sharp sprites can come on: an ini value, Filter=Auto, or a hotkey that turns
	// them on (theirs, or CycleFilter, which reaches Auto).
	g_sprWanted = g_cfgSprK[SPR_CHARS] > 0.0f || g_cfgSprK[SPR_STAGE] > 0.0f || g_filterCfg == FILTER_AUTO ||
	              g_hotkeyVk[ACT_FILTER];
	for (int a = ACT_SPR_TOGGLE; a <= ACT_SPR_FARZOOM_UP; a++)
		if (g_hotkeyVk[a]) g_sprWanted = true;
	if (g_hotkeyVk[ACT_MENU]) g_sprWanted = true;   // the menu can turn the sharp sprites on
	{   // [Menu] Scale: Auto (also blank or missing) = by screen height, else a factor 0.75..3
		char v[32] = {0};
		GetPrivateProfileStringA("Menu", "Scale", "", v, sizeof(v), g_iniPath);
		float f = (float)atof(v);
		g_menuScaleCfg = f < 0.75f ? 0.0f : f > 3.0f ? 3.0f : f;
	}
	resolveSettings();
}

// Write "Display"/key = val only if it differs from what's already in the ini, so an unchanged session
// doesn't re-serialize the file (which would change its timestamp and prompt editors to reload it).
static void writeIniIfChanged(const char *key, const char *val) {
	char cur[64] = {0};
	GetPrivateProfileStringA("Display", key, "\x01", cur, sizeof(cur), g_iniPath);  // sentinel default
	if (lstrcmpA(cur, val) != 0)
		WritePrivateProfileStringA("Display", key, val, g_iniPath);
}

// PersistPosition: write the window's last normal position (g_lastPos) as PositionX/Y, the next spawn position.
static void persistPosition() {
	if (!g_persistPos || !g_haveLastPos) return;
	char v[16];
	wsprintfA(v, "%d", g_lastPos.x);
	writeIniIfChanged("PositionX", v);
	wsprintfA(v, "%d", g_lastPos.y);
	writeIniIfChanged("PositionY", v);
}

// Save Mode, FullscreenScale, WindowScale, Filter and Sharpness (incl. live hotkey changes) with PersistState, and the
// window position with PersistPosition, to the ini at exit. Unchanged keys are not rewritten.
static void persistState() {
	if (!g_enabled) return;                 // (g_enabled is cleared when standing down for another mod)
	persistPosition();
	if (!g_persist) return;
	persistSettings();
	if (g_menuTouched) menuPersist();       // the rest of what the menu sets (DisplayManagerMenu.h)
}

// Mode, FullscreenScale, WindowScale, Filter and Sharpness: PersistState's part, also what the menu's Save button writes
// (whatever PersistState says).
static void persistSettings() {
	char scale[16];
	wsprintfA(scale, "x%d", g_intScale);
	writeIniIfChanged("Mode", modeName());
	writeIniIfChanged("FullscreenScale", scale);
	if (iniHasKey(LEGACY_FS_SCALE_KEY))                       // leftover pre-1.0.4 key (migration skipped)
		WritePrivateProfileStringA("Display", LEGACY_FS_SCALE_KEY, nullptr, g_iniPath);
	wsprintfA(scale, "x%d", g_winScale);
	writeIniIfChanged("WindowScale", scale);

	writeIniIfChanged("Filter", filterName());
	char sh[16] = "Auto";
	if (!g_auto[AUTO_SHARP]) formatHundredths(g_cfgSharp, sh);
	writeIniIfChanged("Sharpness", sh);
}

// ---- hotkeys (WindowResizer-style): Alt+0 = FitToScreen, Alt+1..6 = x1..x6, Alt+P/F/K/L ------------
// A WH_KEYBOARD hook on the game's UI thread (as WindowResizer does): it sees the game's own key messages,
// so it works in exclusive fullscreen, where a GetAsyncKeyState poll did not. Changes apply live (they only
// affect the post-process), no device reset - except a windowed Scale key with WindowedFilter, whose window
// resize ends in one (onWindowResized).
static HHOOK g_kbHook = nullptr;

// lParam bit 30 = previous key state, bit 31 = transition. Both 0 means a fresh key-down (not a repeat
// or a release).
#define IS_FRESH_KEYDOWN(lp) (((lp) & (1 << 30)) == 0 && ((lp) & (1 << 31)) == 0)

// Total non-client border size (width, height) of the game window while windowed. Its style is taken as it
// will be then, so the window-size prediction for a new backbuffer (windowedBackbufferSize) can run before the
// window is set up: the saved normal style while borderless, and our drag-resize border before installWndProc.
// With DpiAware, the frame is sized for the DPI of the window's monitor (`dpi`, 0 = the window's current one).
static void windowBordersForDpi(UINT dpi, int *bx, int *by) {
	LONG style = g_styleSaved ? g_savedStyle : GetWindowLongA(g_hwnd, GWL_STYLE);
	LONG ex = g_styleSaved ? g_savedExStyle : GetWindowLongA(g_hwnd, GWL_EXSTYLE);
	if (g_resizable) style |= WS_THICKFRAME;
	RECT r = { 0, 0, 0, 0 };
	if (g_dpiOn && !dpi) dpi = pGetDpiForWindow(g_hwnd);
	if (g_dpiOn && dpi) pAdjustWindowRectExForDpi(&r, style, GetMenu(g_hwnd) != nullptr, ex, dpi);
	else                AdjustWindowRectEx(&r, style, GetMenu(g_hwnd) != nullptr, ex);
	*bx = r.right - r.left; *by = r.bottom - r.top;
}
static void windowBorders(int *bx, int *by) { windowBordersForDpi(0, bx, by); }

// Where setWindowScaled(n, pos) puts the window: returns the scale it can use (n, or the largest one that fits
// the work area of the monitor it lands on) and the window's top-left in *px/*py (pos, or where the window is,
// moved back on-screen). The slack allows for Windows 10's invisible resize borders.
static int placeWindowScaled(int n, const POINT *pos, int *px, int *py) {
	int bx, by; windowBorders(&bx, &by);
	RECT wr;
	if (!GetWindowRect(g_hwnd, &wr)) return 0;
	int x = pos ? pos->x : wr.left, y = pos ? pos->y : wr.top, use = n;
	POINT at = { x, y };
	HMONITOR mon = pos ? MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST)
	                   : MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi = { sizeof(mi) };
	if (mon && GetMonitorInfoA(mon, &mi)) {
		const RECT &wa = mi.rcWork;
		int slack = bx / 2;                                   // one side border
		int maxN = min((wa.right - wa.left + 2 * slack - bx) / g_srcW, (wa.bottom - wa.top + slack - by) / g_srcH);
		if (maxN < 1) maxN = 1;
		if (use > maxN) use = maxN;
		int w = g_srcW * use + bx, h = g_srcH * use + by;
		if (x + w > wa.right + slack)   x = wa.right + slack - w;
		if (y + h > wa.bottom + slack)  y = wa.bottom + slack - h;
		if (x < wa.left - slack)        x = wa.left - slack;
		if (y < wa.top)                 y = wa.top;
	}
	*px = x; *py = y;
	return use;
}

// Resize the game's window to a (srcW*n) x (srcH*n) client, optionally moving its top-left to `pos`. Mirrors
// WindowResizer. Clamped to the work area of the monitor it lands on (placeWindowScaled). Windowed without our
// backbuffer, D3D9's present stretches the 640x480 one over the new size; with WindowedFilter the resulting
// WM_SIZE resizes the backbuffer to match (onWindowResized).
static void setWindowScaled(int n, const POINT *pos) {
	if (!g_hwnd || n < 1) return;
	int bx, by; windowBorders(&bx, &by);
	RECT wr;
	if (!GetWindowRect(g_hwnd, &wr)) return;
	int x, y, use = placeWindowScaled(n, pos, &x, &y);
	if (use < 1) return;
	UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
	if (x == wr.left && y == wr.top) flags |= SWP_NOMOVE;
	SetWindowPos(g_hwnd, nullptr, x, y, g_srcW * use + bx, g_srcH * use + by, flags);
	logf("window -> client %dx%d (x%d%s)%s", g_srcW * use, g_srcH * use, use,
	     use != n ? ", clamped to the work area" : "", (flags & SWP_NOMOVE) ? "" : " +pos");
}

static void applyTopmost() {
	if (g_hwnd)
		SetWindowPos(g_hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
		             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Apply the remembered window scale on entering (real) windowed mode. On the first spawn, move to the
// configured spawn position (if any); when coming back from fullscreen, to where the window was before
// (the game's post-toggle SetWindowPos re-centers it on the primary monitor). If we were in borderless
// fullscreen, restore the normal window frame first.
// Where onWindowedEntry moves the window: the configured spawn position on the first spawn, else where it was
// before fullscreen (nullptr = leave it where it is).
static const POINT *windowedEntryPos(bool firstTime, POINT *buf) {
	if (firstTime) {
		if (!g_havePos) return nullptr;
		buf->x = g_posX; buf->y = g_posY;
		return buf;
	}
	return g_haveWinPos ? &g_winPos : nullptr;
}

static void onWindowedEntry(bool firstTime) {
	if (g_styleSaved && g_hwnd) {
		SetWindowLongA(g_hwnd, GWL_STYLE, g_savedStyle);
		SetWindowLongA(g_hwnd, GWL_EXSTYLE, g_savedExStyle);
		SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
		             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
		g_styleSaved = false;
	}
	g_borderlessActive = false;
	POINT buf;
	setWindowScaled(g_winScale, windowedEntryPos(firstTime, &buf));
	applyTopmost();
}

// The windowed backbuffer size for WindowedFilter: the window's client area - the one onWindowedEntry is about to
// give it when `entering` (so the first spawn and a return from fullscreen need no second Reset), else the current
// one. At least 640x480 in each direction, since the game always draws its frame 1:1 into the top-left; a smaller
// client (only possible outside our 4:3 drag lock, e.g. snapping) gets a 640x480 backbuffer that D3D9 shrinks.
// Minimized: the last size. Any wrong guess is fixed by the next WM_SIZE (onWindowResized).
static void windowedBackbufferSize(bool entering, bool firstTime, UINT *w, UINT *h) {
	int cw = 0, ch = 0;
	RECT c;
	if (g_hwnd && entering) {
		POINT buf; int x, y;
		int use = placeWindowScaled(g_winScale, windowedEntryPos(firstTime, &buf), &x, &y);
		if (use < 1) use = g_winScale;
		cw = g_srcW * use; ch = g_srcH * use;
	} else if (g_hwnd && IsIconic(g_hwnd) && g_bbW && g_bbH) {
		cw = (int)g_bbW; ch = (int)g_bbH;
	} else if (g_hwnd && GetClientRect(g_hwnd, &c)) {
		cw = c.right; ch = c.bottom;
	}
	*w = (UINT)(cw > g_srcW ? cw : g_srcW);
	*h = (UINT)(ch > g_srcH ? ch : g_srcH);
}

// WindowedFilter: after the window's client area changed (posted from wndProc, so it runs from the game's message
// loop, between frames, like Alt+Enter), Reset to a backbuffer of the new size if it differs. The Reset goes through
// the game's own wrapper, which releases and recreates its D3DPOOL_DEFAULT resources around it; myGameReset sizes it.
// Not during a drag (the old backbuffer is stretched until WM_EXITSIZEMOVE) or while a window setup is still queued
// (that sets the final size and posts again).
static void onWindowResized() {
	if (!g_enabled || !g_winFilter || g_winFilterFailed || g_wantFullscreen || !g_deviceHooked || !g_hwnd ||
	    g_inSizeMove || g_applyPending || IsIconic(g_hwnd) || !GAME_DEVICE)
		return;
	UINT w, h;
	windowedBackbufferSize(false, false, &w, &h);
	if (g_active && w == g_bbW && h == g_bbH) return;
	logf("window resized: backbuffer %ux%u -> %ux%u, resetting", g_active ? g_bbW : 0, g_active ? g_bbH : 0, w, h);
	bool ok = reinterpret_cast<GameReset_t>(ADDR_GAME_RESET)();
	if (!ok) logf("window resized: the game's Reset returned false (device lost, or Reset failed)");
}

static void postWindowResized() {
	if (g_resizeMsg && g_hwnd) PostMessageA(g_hwnd, g_resizeMsg, 0, 0);
}

// Set the window up for the current state: windowed (onWindowedEntry; the first time with the spawn
// setup) or the borderless popup. Exclusive fullscreen needs nothing - D3D9 owns the screen, and neither
// does the game's own fullscreen when the forced mode failed and we fell back to it (g_active false).
static void applyWindowState() {
	bool firstTime = g_spawnPending;
	g_spawnPending = false;
	g_applyPending = false;
	if (!g_wantFullscreen)             onWindowedEntry(firstTime);
	else if (g_borderless && g_active) enterBorderlessFullscreen();
	if (!g_wantFullscreen) postWindowResized();   // check the backbuffer against the final size (no-op if it matches)
}

static WNDPROC g_origWndProc = nullptr;

// [Input] StartInLatinInput=1: open the game on a Latin keyboard instead of a CJK input method. CJK players usually
// have an IME (e.g. Microsoft Pinyin) next to an English keyboard, and the game window starts with whichever is active,
// often the IME in its native (Chinese) mode, which then composes from the game's keys (its box keeps popping up).
// Once, when the window is first active, if the active input method is a Chinese / Japanese / Korean IME that is on in
// its native mode: switch to an installed non-CJK keyboard layout (English US first), or, with none, put the IME where
// its own toggle key would (off / English mode; e.g. Shift in Pinyin turns it back on). It is never disabled, so
// players can switch back to chat. It has to wait until the window is the foreground one: with Windows' default "same
// input method for all apps", activating a window gives it the desktop's current input method, which would undo an
// earlier switch (and our switch becomes the desktop's, as when the player does it by hand). Activation messages can't
// be relied on to see that (NoFocusNoBgm, for one, keeps them from our wndProc), so until then a thread timer checks
// every 200 ms; its callback is called directly by the window thread's message loop. Runs on the window thread.
static UINT     g_latinMsg   = 0;   // private registered message: start startInLatinInput on the window thread
static UINT_PTR g_latinTimer = 0;
static DWORD    g_latinSince = 0;   // GetTickCount when first seen as the foreground window (0 = not yet)

static DWORD hklId(HKL hkl) { return (DWORD)(DWORD_PTR)hkl; }   // low word = language, high word = device

static bool isCjkLanguage(HKL hkl) {
	WORD lang = PRIMARYLANGID(LOWORD(hklId(hkl)));
	return lang == LANG_CHINESE || lang == LANG_JAPANESE || lang == LANG_KOREAN;
}

// A plain keyboard layout of a non-CJK language: not a CJK language, not a legacy IMM IME (device 0xE0xx). ImmIsIME
// can't tell: with TSF on it reports every layout as an IME.
static bool isLatinLayout(HKL hkl) {
	return !isCjkLanguage(hkl) && (HIWORD(hklId(hkl)) & 0xF000) != 0xE000;
}

static void startInLatinInput(HWND h) {
	if (!g_latinPending || GetForegroundWindow() != h) return;   // not the foreground window yet: the timer retries
	DWORD now = GetTickCount();
	if (!g_latinSince) g_latinSince = now | 1;   // never 0 (= not yet)
	HKL cur = GetKeyboardLayout(0);
	if (!isCjkLanguage(cur)) {
		g_latinPending = false;
		logf("StartInLatinInput: input method %08lx is not Chinese / Japanese / Korean - left alone", hklId(cur));
		return;
	}
	// Only an IME that is on in its native mode composes. A new window's IME takes a moment to start (it reports
	// itself off until then), so keep checking for a few seconds before taking "off" as the answer - which it is
	// for IMEs that start in direct input, like Microsoft's Japanese IME.
	HIMC imc = ImmGetContext(h);
	DWORD conv = 0, sentence = 0;
	bool native = imc && ImmGetOpenStatus(imc) && ImmGetConversionStatus(imc, &conv, &sentence) &&
	              (conv & IME_CMODE_NATIVE);
	if (!native) {
		if (now - g_latinSince > 5000) {
			g_latinPending = false;
			logf("StartInLatinInput: IME %08lx is off or not in native mode - left alone", hklId(cur));
		}
		if (imc) ImmReleaseContext(h, imc);
		return;
	}
	g_latinPending = false;
	HKL list[64], latin = nullptr;
	int n = GetKeyboardLayoutList(64, list);
	for (int i = 0; i < n; i++)
		if (isLatinLayout(list[i]) && (!latin || LOWORD(hklId(list[i])) == 0x0409)) latin = list[i];   // en-US first
	if (latin) {
		ActivateKeyboardLayout(latin, 0);
		logf("StartInLatinInput: IME %08lx -> keyboard layout %08lx (now %08lx)", hklId(cur), hklId(latin),
		     hklId(GetKeyboardLayout(0)));
	} else {
		// No Latin keyboard: put the IME where its own toggle key would, which differs by language (tested with
		// Microsoft's IMEs). Korean: English mode, i.e. clear the Hangul (native) bit, as the Han/Eng key does - it
		// keeps composing Hangul when merely turned off. Chinese / Japanese: turn the IME off (direct input) - Pinyin
		// reports a cleared native bit but keeps composing; Shift turns it back on in Chinese mode.
		bool korean = PRIMARYLANGID(LOWORD(hklId(cur))) == LANG_KOREAN;
		BOOL ok = korean ? ImmSetConversionStatus(imc, conv & ~IME_CMODE_NATIVE, sentence)
		                 : ImmSetOpenStatus(imc, FALSE);
		logf("StartInLatinInput: no non-CJK keyboard installed; IME %08lx (conversion 0x%lx) %s%s", hklId(cur), conv,
		     korean ? "set to English mode" : "turned off", ok ? "" : " - FAILED");
	}
	ImmReleaseContext(h, imc);
}

static void CALLBACK latinTimerProc(HWND, UINT, UINT_PTR, DWORD) {
	if (g_hwnd) startInLatinInput(g_hwnd);
	if (!g_latinPending && g_latinTimer) { KillTimer(nullptr, g_latinTimer); g_latinTimer = 0; }
}

// Subclassed window procedure: runs the deferred apply posted by postWindowApply, while windowed and resizable
// locks a drag-resize to 4:3 (at least 640x480) so the stretched image never gets squashed, and with
// WindowedFilter queues a backbuffer resize once a resize is over (onWindowResized): at the end of a drag, or
// right away for one that isn't a drag (Scale hotkeys, snapping, the game's own SetWindowPos).
static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
	if (g_applyMsg && msg == g_applyMsg) {
		applyWindowState();
		return 0;
	}
	if (g_resizeMsg && msg == g_resizeMsg) {
		onWindowResized();
		return 0;
	}
	if (g_menuMsg && msg == g_menuMsg) {     // a command from the menu (the render thread): wp = command, lp = value
		menuCommand((int)wp, lp);
		return 0;
	}
	{   // the settings menu takes the mouse while it is shown
		LRESULT mres = 0;
		if (menuWndProc(h, msg, wp, lp, &mres)) return mres;
	}
	if (g_latinMsg && msg == g_latinMsg) {   // StartInLatinInput: now, or from the timer once the window is foreground
		startInLatinInput(h);
		if (g_latinPending && !g_latinTimer) g_latinTimer = SetTimer(nullptr, 0, 200, latinTimerProc);
		return 0;
	}
	if (msg == 0x02E0 /* WM_DPICHANGED */ && g_dpiOn) {
		// DpiAware, moved to a monitor with another scale: keep the game area's pixel size - only the frame changes -
		// and the window where it is. Windows' suggested rect (lParam) is placed for a scaled window and would move it:
		// away from where DM (spawn position, PersistPosition) or the player's drag just put it. (Fullscreen: the
		// window covers the monitor already; nothing to do.)
		if (!g_wantFullscreen && !g_borderlessActive) {
			RECT cr, wr;
			GetClientRect(h, &cr);
			GetWindowRect(h, &wr);
			int bx, by; windowBordersForDpi(HIWORD(wp), &bx, &by);
			SetWindowPos(h, nullptr, wr.left, wr.top, cr.right - cr.left + bx, cr.bottom - cr.top + by,
			             SWP_NOZORDER | SWP_NOACTIVATE);
			logf("DPI changed to %u: client %ldx%ld kept at (%ld,%ld)", HIWORD(wp), cr.right - cr.left,
			     cr.bottom - cr.top, wr.left, wr.top);
		}
		return 0;
	}
	switch (msg) {
	case WM_ENTERSIZEMOVE:
		g_inSizeMove = true;
		break;
	case WM_EXITSIZEMOVE:
		g_inSizeMove = false;
		postWindowResized();
		break;
	case WM_SIZE:
		if (wp != SIZE_MINIMIZED && !g_inSizeMove) postWindowResized();
		break;
	case WM_WINDOWPOSCHANGED: {   // PersistPosition: track the last normal-window position (see g_lastPos)
		RECT r;
		if (g_persistPos && !g_wantFullscreen && !g_windowFs && !g_borderlessActive && !g_applyPending &&
		    !IsIconic(h) && !IsZoomed(h) && GetWindowRect(h, &r)) {
			g_lastPos.x = r.left; g_lastPos.y = r.top; g_haveLastPos = true;
		}
		break;
	}
	}
	if (msg == WM_SIZING && g_resizable && !g_wantFullscreen) {
		RECT *wr = (RECT *)lp;
		int bx, by; windowBorders(&bx, &by);
		int cw = (wr->right - wr->left) - bx;
		int ch = (wr->bottom - wr->top) - by;
		if (cw < g_srcW) cw = g_srcW;
		if (ch < g_srcH) ch = g_srcH;
		if (wp == WMSZ_TOP || wp == WMSZ_BOTTOM) cw = ch * g_srcW / g_srcH; // dragging a horizontal edge
		else                                     ch = cw * g_srcH / g_srcW; // vertical edge or a corner
		if (wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT) wr->left = wr->right - (cw + bx);
		else                                                                wr->right = wr->left + (cw + bx);
		if (wp == WMSZ_TOP || wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT)    wr->top = wr->bottom - (ch + by);
		else                                                                wr->bottom = wr->top + (ch + by);
		return TRUE;
	}
	return CallWindowProcA(g_origWndProc, h, msg, wp, lp);
}

static volatile LONG g_wndProcClaim = 0;
static void installWndProc() {
	if (!g_hwnd || InterlockedCompareExchange(&g_wndProcClaim, 1, 0) != 0) return;   // single-shot
	if (g_resizable) {
		LONG style = GetWindowLongA(g_hwnd, GWL_STYLE);
		SetWindowLongA(g_hwnd, GWL_STYLE, style | WS_THICKFRAME);   // add a drag-resize border
		SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
		             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
	}
	if (!g_applyMsg)  g_applyMsg  = RegisterWindowMessageA("DisplayManager.ApplyWindowState");
	if (!g_resizeMsg) g_resizeMsg = RegisterWindowMessageA("DisplayManager.WindowResized");
	if (!g_latinMsg)  g_latinMsg  = RegisterWindowMessageA("DisplayManager.StartInLatinInput");
	if (!g_menuMsg)   g_menuMsg   = RegisterWindowMessageA("DisplayManager.MenuCommand");
	// Record the original before swapping, so a message dispatched in between (the install can run off the
	// window thread, from the device watch) never finds it null.
	g_origWndProc = (WNDPROC)GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC);
	WNDPROC prev = (WNDPROC)SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)wndProc);
	if (prev) g_origWndProc = prev;
	logf("wndproc subclassed (resizable=%d)", g_resizable);
	if (g_latinPending) PostMessageA(g_hwnd, g_latinMsg, 0, 0);   // StartInLatinInput, on the window thread
}

// Apply the window state (applyWindowState) once the game has finished handling the current message.
// On Alt+Enter th123 calls Reset from its window procedure (0x4082DE -> 0x415220) and, AFTER Reset
// returns, does its own SetWindowPos: windowed = HWND_NOTOPMOST, re-centered on the primary monitor, sized
// with fixed-frame metrics (wrong for our WS_THICKFRAME, giving a non-4:3 client); fullscreen = move the
// client to the primary's origin. Anything done inside the Reset is undone by that, so we post a private
// message to the game window and do the work in wndProc, after the game's own window code has run.
// Falls back to applying immediately if the window isn't subclassed.
static void postWindowApply(bool firstTime) {
	if (firstTime) g_spawnPending = true;
	if (!g_applyMsg) g_applyMsg = RegisterWindowMessageA("DisplayManager.ApplyWindowState");
	g_applyPending = true;   // before posting: a Reset can run off the window thread, which may handle it at once
	if (g_hwnd && g_origWndProc && g_applyMsg && PostMessageA(g_hwnd, g_applyMsg, 0, 0))
		return;
	applyWindowState();
}

// Turn the game's window into a borderless popup covering its monitor.
static void enterBorderlessFullscreen() {
	if (!g_hwnd) return;
	if (!g_styleSaved) {
		g_savedStyle   = GetWindowLongA(g_hwnd, GWL_STYLE);
		g_savedExStyle = GetWindowLongA(g_hwnd, GWL_EXSTYLE);
		g_styleSaved = true;
	}
	LONG style = (g_savedStyle & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
	                               WS_SYSMENU | WS_BORDER | WS_DLGFRAME)) | WS_POPUP;
	LONG ex = g_savedExStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
	SetWindowLongA(g_hwnd, GWL_STYLE, style);
	SetWindowLongA(g_hwnd, GWL_EXSTYLE, ex);

	// Cover the monitor the window was on when fullscreen was requested (g_fsMon): by now the game's
	// post-toggle SetWindowPos has moved the window to the primary monitor's origin.
	MONITORINFO mi = { sizeof(mi) };
	if (!g_fsMon || !GetMonitorInfo(g_fsMon, &mi))
		GetMonitorInfo(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
	int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
	SetWindowPos(g_hwnd, g_topmost ? HWND_TOPMOST : HWND_TOP,
	             mi.rcMonitor.left, mi.rcMonitor.top, mw, mh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
	g_borderlessActive = true;
	logf("borderless window %dx%d at (%d,%d) topmost=%d",
	     mw, mh, mi.rcMonitor.left, mi.rcMonitor.top, g_topmost);
}

// Shift held together with a tuning hotkey: finer steps (Sharpness, sharp sprites) or the reverse direction (the
// xBR Strength / Width cycles). Not when Shift is the hotkey modifier itself.
static bool fineStep() {
	return g_modifier != MODK_SHIFT && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

// After a filter change: resolve the output's filter and the live values now (see resolveSettings).
static void filterChanged() {
	if (g_active) computeOutput();
	else resolveSettings();
}

// Run a hotkey action. "Scale N" applies to the current state: fullscreen sets Mode=IntegerScaling xN;
// windowed only resizes the window (g_winScale), so it never changes the fullscreen mode.
static void doAction(int act) {
	switch (act) {
	case ACT_FIT:
		g_mode = MODE_FIT;
		if (g_active) computeOutput();
		showOsd("FITTOSCREEN");
		logf("hotkey: FitToScreen");
		break;
	case ACT_S1: case ACT_S2: case ACT_S3: case ACT_S4: case ACT_S5: case ACT_S6: {
		int n = act - ACT_S1 + 1;
		if (g_wantFullscreen) {                    // fullscreen: re-scale the centered output live
			g_mode = MODE_INTEGER; g_intScale = n;
			if (g_active) computeOutput();
		} else {                                   // windowed: resize the window to N x
			g_winScale = n;
			setWindowScaled(n, nullptr);
		}
		char msg[8]; wsprintfA(msg, "X%d", n); showOsd(msg);
		logf("hotkey: x%d", n);
		break;
	}
	case ACT_TOP:
		g_topmost = !g_topmost;
		applyTopmost();
		logf("hotkey: always-on-top=%d", g_topmost);
		break;
	case ACT_FILTER:
		g_filterCfg = (g_filterCfg + 1) % FILTER_COUNT;   // Point -> Linear -> Sharp -> xBR -> Auto -> Point
		filterChanged();                                  // re-resolve g_filter now; next frame's present uses it
		if (g_filterCfg == FILTER_SHARP) showSharpnessOsd();
		else showOsd(g_filterCfg == FILTER_POINT ? "POINT" : g_filterCfg == FILTER_XBR ? "XBR" :
		             g_filterCfg == FILTER_AUTO ? "AUTO" : "LINEAR");
		logf("hotkey: filter -> %s", filterName());
		break;
	case ACT_SHARP_DOWN:
	case ACT_SHARP_UP: {
		const bool af = g_filterCfg == FILTER_AUTO;
		if (!af) {                              // sharpness only affects Sharp (and Auto), so switch to it
			g_filterCfg = FILTER_SHARP;
			filterChanged();
		}
		float step = fineStep() ? 0.05f : 0.25f;                     // Shift: fine steps
		g_sharpness += (act == ACT_SHARP_UP) ? step : -step;         // live; the shader reads it each frame
		g_sharpness = floorf(g_sharpness / 0.05f + 0.5f) * 0.05f;
		clampSharpness();
		if (!af) { g_cfgSharp = g_sharpness; g_auto[AUTO_SHARP] = false; }   // Auto: this session's Auto only
		showSharpnessOsd();
		logf("hotkey: filter=%s sharpness -> %.2f", filterName(), g_sharpness);
		break;
	}
	case ACT_XBR_STRENGTH: case ACT_XBR_CORNER: case ACT_XBR_SLOPES: case ACT_XBR_WIDTH: {
		// Development knobs: each cycles one xBR setting and switches to xBR. Session only (not written to the ini);
		// the log gets the resulting values so they can be copied into the ini. With Shift: the other direction.
		char msg[32], num[16];
		bool back = fineStep();
		g_filterCfg = FILTER_XBR;
		filterChanged();
		g_auto[AUTO_XBR_STRENGTH + (act - ACT_XBR_STRENGTH)] = false;   // a fixed value from now on
		if (act == ACT_XBR_STRENGTH) {
			// 0.05 steps down, wrapping 0.05 -> 1.0 (Shift: up, wrapping 1.0 -> 0.05)
			int s = (int)floorf(g_xbrStrength * 20.0f + 0.5f) + (back ? 1 : -1);
			if (s < 1) s = 20; else if (s > 20) s = 1;
			g_xbrStrength = s / 20.0f;
			formatHundredths(g_xbrStrength, num);
			wsprintfA(msg, "XBR STR %s", num);
		} else if (act == ACT_XBR_CORNER) {
			g_xbrCorner = (g_xbrCorner + 1) % 4;                    // A -> B -> C -> D
			wsprintfA(msg, "XBR CORNER %c", 'A' + g_xbrCorner);
		} else if (act == ACT_XBR_SLOPES) {
			g_xbrSlopes = !g_xbrSlopes;
			wsprintfA(msg, "XBR 30 60 %s", g_xbrSlopes ? "ON" : "OFF");
		} else {
			// 0.25 steps up, wrapping 4.0 -> 0.25 (Shift: down, wrapping 0.25 -> 4.0)
			int w = (int)floorf(g_xbrWidth * 4.0f + 0.5f) + (back ? -1 : 1);
			if (w < 1) w = 16; else if (w > 16) w = 1;
			g_xbrWidth = w / 4.0f;
			formatHundredths(g_xbrWidth, num);
			wsprintfA(msg, "XBR WIDTH %s", num);
		}
		showOsd(msg);
		char s[16], w[16];
		formatHundredths(g_xbrStrength, s);
		formatHundredths(g_xbrWidth, w);
		logf("hotkey: xBR XbrStrength=%s XbrCorner=%c XbrSlopes=%d XbrWidth=%s", s, 'A' + g_xbrCorner, g_xbrSlopes, w);
		break;
	}
	case ACT_SPR_TOGGLE: case ACT_SPR_DOWN: case ACT_SPR_UP:
	case ACT_BG_TOGGLE: case ACT_BG_DOWN: case ACT_BG_UP: {
	// Sharp sprites, session only (not written to the ini). Toggle: off <-> the last value (the ini's, else the
	// output scale's default). Down / Up: step k; from off they first turn it back on at the last value.
		static const float steps[] = { 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f, 4.0f, 6.0f, 8.0f,
		                               12.0f, 16.0f };
		const int n = sizeof(steps) / sizeof(steps[0]);
		int layer = act <= ACT_SPR_UP ? SPR_CHARS : SPR_STAGE;
		int op = act - (layer == SPR_CHARS ? ACT_SPR_TOGGLE : ACT_BG_TOGGLE);   // 0 toggle, 1 down, 2 up
		float k = g_sprK[layer];
		if (op == 0 || k <= 0.0f) {
			k = k > 0.0f ? 0.0f : g_sprLastK[layer];
		} else if (fineStep()) {                                        // Shift: 0.1 steps
			k = floorf(k * 10.0f + 0.5f) / 10.0f + (op == 1 ? -0.1f : 0.1f);
			k = k < steps[0] ? steps[0] : k > steps[n - 1] ? steps[n - 1] : k;
		} else {
			int j = 0;
			while (j < n - 1 && steps[j] < k - 0.001f) j++;            // the step at or above k
			if (op == 1) { if (j > 0) j--; }
			else if (steps[j] <= k + 0.001f && j < n - 1) j++;
			k = steps[j];
		}
		if (g_sprFailed) k = 0.0f;
		g_sprK[layer] = k;
		if (g_filterCfg != FILTER_AUTO) {       // Auto: this session's Auto only; leaving it brings the chosen value back
			g_cfgSprK[layer] = k;
			if (k > 0.0f) { g_sprLastK[layer] = k; g_sprLastKSet[layer] = true; }
		}
		char msg[32], num[16];
		formatHundredths(k, num);
		wsprintfA(msg, "%s %s", layer == SPR_CHARS ? "SPRITES" : "BG", k > 0.0f ? num : "OFF");
		showOsd(msg);
		logf("hotkey: sharp %s -> %.2f", layer == SPR_CHARS ? "sprites" : "background", (double)k);
		break;
	}
	case ACT_SPR_NEAR_DOWN: case ACT_SPR_NEAR_UP: case ACT_BG_REST_DOWN: case ACT_BG_REST_UP:
	case ACT_SPR_FAR_DOWN: case ACT_SPR_FAR_UP: {
		// The strengths in 0.1 steps, session only: the characters' near / far, the stage's rest.
		bool up = act == ACT_SPR_NEAR_UP || act == ACT_BG_REST_UP || act == ACT_SPR_FAR_UP;
		volatile float *p = act <= ACT_SPR_NEAR_UP ? &g_sprRest[SPR_CHARS]
		                  : act <= ACT_BG_REST_UP  ? &g_sprRest[SPR_STAGE] : &g_sprFar;
		const char *name = act <= ACT_SPR_NEAR_UP ? "SPRITES NEAR" : act <= ACT_BG_REST_UP ? "BG REST" : "SPRITES FAR";
		float r = floorf(*p * 10.0f + 0.5f) / 10.0f + (up ? 0.1f : -0.1f);
		r = r < 0.0f ? 0.0f : r > 1.0f ? 1.0f : r;
		*p = r;
		if (g_filterCfg != FILTER_AUTO) {       // Auto: this session's Auto only; leaving it brings the chosen value back
			if (p == &g_sprRest[SPR_STAGE]) { g_cfgBgRest = r; g_auto[AUTO_BGREST] = false; }
			else if (p == &g_sprFar)        g_cfgSprFar = r;
			else                            g_cfgSprNear = r;
		}
		char msg[32], num[16];
		formatHundredths(r, num);
		wsprintfA(msg, "%s %s", name, num);
		showOsd(msg);
		logf("hotkey: %s strength -> %.2f", name, (double)r);
		break;
	}
	case ACT_SPR_FARZOOM_DOWN: case ACT_SPR_FARZOOM_UP: {
		// The camera zoom where the characters reach the far strength, in 0.05 steps, session only.
		float z = floorf(g_sprFarZoom * 20.0f + 0.5f) / 20.0f + (act == ACT_SPR_FARZOOM_UP ? 0.05f : -0.05f);
		z = z < SPR_FARZOOM_MIN ? SPR_FARZOOM_MIN : z > SPR_FARZOOM_MAX ? SPR_FARZOOM_MAX : z;
		g_sprFarZoom = z;
		if (g_filterCfg != FILTER_AUTO) { g_cfgFarZoom = z; g_auto[AUTO_FARZOOM] = false; }
		char msg[32], num[16];
		formatHundredths(z, num);
		wsprintfA(msg, "SPRITES FAR AT %s", num);
		showOsd(msg);
		logf("hotkey: SPRITES FAR AT zoom -> %.2f", (double)z);
		break;
	}
	case ACT_MSAA: {
		// Off <-> MultiSample from the ini (x8 when that is 0). Session only: not written to the ini. Applied on the
		// render thread after the next Present; windowed without WindowedFilter, it takes effect when going fullscreen.
		int on = g_msaaIni >= 2 ? g_msaaIni : 8;
		g_msaaCfg = g_msaaCfg >= 2 ? 0 : on;
		g_msaaApply = true;
		char msg[32];
		if (g_msaaCfg) wsprintfA(msg, "MSAA X%d", g_msaaCfg); else lstrcpyA(msg, "MSAA OFF");
		showOsd(msg);
		logf("hotkey: MSAA -> %d", (int)g_msaaCfg);
		break;
	}
	case ACT_MENU:
		menuToggle();
		break;
	}
}

// The settings menu. After doAction: it reads and writes the same settings the hotkeys do.
#include "DisplayManagerMenu.h"

static bool modifierDown() {
	switch (g_modifier) {
	case MODK_CTRL:  return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
	case MODK_SHIFT: return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
	case MODK_WIN:   return ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
	case MODK_NONE:  return true;
	case MODK_ALT:
	default:         return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
	}
}

static LRESULT CALLBACK keyboardHook(int code, WPARAM wParam, LPARAM lParam) {
	if (code == HC_ACTION && IS_FRESH_KEYDOWN(lParam) && modifierDown()) {
		for (int a = 0; a < ACT_COUNT; a++) {
			if (g_hotkeyVk[a] && (int)wParam == g_hotkeyVk[a]) {
				doAction(a);
				return 1; // eat the key so it doesn't leak to the game / system menu
			}
		}
	}
	return CallNextHookEx(nullptr, code, wParam, lParam);
}

// Install the keyboard hook on the GAME WINDOW's thread, not the current one: SokuDirectXOptimizations
// moves rendering/present (and so CreateDevice) onto a separate thread that receives no key input.
static void installKeyboardHook() {
	if (g_kbHook || !g_hwnd) return;
	DWORD tid = GetWindowThreadProcessId(g_hwnd, nullptr);
	if (!tid) return;
	g_kbHook = SetWindowsHookExA(WH_KEYBOARD, keyboardHook, g_module, tid);
	logf("keyboard hook on window thread %lu (current %lu) %s",
	     tid, GetCurrentThreadId(), g_kbHook ? "installed" : "FAILED");
}

static void setupHooks() {
	// The IAT slot holds the Direct3DCreate9 pointer directly (the game does `jmp [0x8572A0]`); swap in
	// our wrapper and keep the original to call through.
	DWORD *slot = reinterpret_cast<DWORD *>(ADDR_D3DCREATE9_IAT);
	DWORD old;
	VirtualProtect(slot, sizeof(DWORD), PAGE_READWRITE, &old);
	oDirect3DCreate9 = reinterpret_cast<Direct3DCreate9_t>(*slot);
	*slot = reinterpret_cast<DWORD>(&myDirect3DCreate9);
	VirtualProtect(slot, sizeof(DWORD), old, &old);
}

// [Input] AllowWinKey=1: let the Windows key through while the game has focus (Win+Shift+S screenshots,
// virtual-desktop switching, ...). Vanilla th123 blocks it itself: its DirectInput keyboard is set up with
// DISCL_NOWINKEY (see ADDR_KB_COOPLEVEL_PUSH), which makes DirectInput swallow the Win keys while the device
// is acquired. We clear just that bit in the immediate; the keyboard stays non-exclusive + foreground, so game
// input is unchanged. Must run before the game creates its keyboard (Initialize runs before the game's init).
// Checks the exact bytes first and leaves anything unexpected (another patch) alone.
static void applyAllowWinKey() {
	BYTE *p = reinterpret_cast<BYTE *>(ADDR_KB_COOPLEVEL_PUSH);
	const BYTE want = DI_NONEXCLUSIVE | DI_FOREGROUND;
	if (p[0] != 0x6A || (p[1] & ~DI_NOWINKEY) != want) {
		logf("AllowWinKey: unexpected code at 0x%08lx (%02x %02x) - not patched", ADDR_KB_COOPLEVEL_PUSH, p[0], p[1]);
		return;
	}
	if (p[1] & DI_NOWINKEY) {
		DWORD old;
		VirtualProtect(p + 1, 1, PAGE_EXECUTE_READWRITE, &old);
		p[1] = want;
		VirtualProtect(p + 1, 1, old, &old);
		FlushInstructionCache(GetCurrentProcess(), p, 2);
	}
	bool late = *reinterpret_cast<void **>(ADDR_KB_DEVICE) != nullptr;
	logf("AllowWinKey: keyboard cooperative level 0x16 -> 0x%02x (Windows key allowed)%s", want,
	     late ? " - but the keyboard already exists; takes effect next launch" : "");
}

// For other mods (e.g. an overlay that maps mouse positions onto the game): where the game's 640x480 image is
// inside the game window's CLIENT area, in client pixels (the space of WM_MOUSEMOVE / ScreenToClient). Windowed,
// that is the whole client (our upscale or D3D9's stretch fills it). In fullscreen (exclusive or
// borderless) the client covers the monitor and the image is the centered, scaled rect DM draws (pillar/
// letterboxed; smaller with IntegerScaling/CustomResolution). Returns FALSE when DM is off or standing down, or
// the window isn't known yet - the caller should then assume a 4:3 image centered in the client.
// Look it up with GetProcAddress(GetModuleHandleA("DisplayManager.dll"), "DisplayManager_GetGameRect").
extern "C" __declspec(dllexport) BOOL DisplayManager_GetGameRect(RECT *out) {
	if (!out || !g_enabled || !g_hwnd) return FALSE;
	RECT c;
	if (!GetClientRect(g_hwnd, &c) || c.right <= 0 || c.bottom <= 0) return FALSE;
	UINT bbW = g_bbW, bbH = g_bbH;
	if (!g_active || !bbW || !bbH) { *out = c; return TRUE; }
	int w = g_scaleW, h = g_scaleH;
	int x = ((int)bbW - w) / 2, y = ((int)bbH - h) / 2;
	// Backbuffer -> client pixels (the same size in practice; scaled in case the client differs).
	out->left   = MulDiv(x,     c.right,  (int)bbW);
	out->top    = MulDiv(y,     c.bottom, (int)bbH);
	out->right  = MulDiv(x + w, c.right,  (int)bbW);
	out->bottom = MulDiv(y + h, c.bottom, (int)bbH);
	return TRUE;
}

// Overlay API: see DisplayManagerOverlay.h.
extern "C" __declspec(dllexport) BOOL __cdecl DisplayManager_AddOverlay(DisplayManager_OverlayProc proc, void *user) {
	if (!proc || !g_enabled) return FALSE;
	BOOL ok = FALSE;
	AcquireSRWLockExclusive(&g_overlayLock);
	for (int i = 0; i < g_overlayCount; i++)
		if (g_overlays[i].proc == proc && g_overlays[i].user == user) ok = TRUE;
	if (!ok && g_overlayCount < MAX_OVERLAYS) {
		g_overlays[g_overlayCount++] = { proc, user };
		ok = TRUE;
	}
	ReleaseSRWLockExclusive(&g_overlayLock);
	logf("overlay %p registered: %d", proc, ok);
	return ok;
}

extern "C" __declspec(dllexport) BOOL __cdecl DisplayManager_RemoveOverlay(DisplayManager_OverlayProc proc, void *user) {
	BOOL found = FALSE;
	AcquireSRWLockExclusive(&g_overlayLock);
	for (int i = 0; i < g_overlayCount; i++)
		if (g_overlays[i].proc == proc && g_overlays[i].user == user) {
			g_overlays[i] = g_overlays[--g_overlayCount];
			found = TRUE;
			break;
		}
	ReleaseSRWLockExclusive(&g_overlayLock);
	return found;
}

extern "C" __declspec(dllexport) bool CheckVersion(const BYTE hash[16]) {
	return ::memcmp(TARGET_HASH, hash, sizeof TARGET_HASH) == 0;
}

extern "C" __declspec(dllexport) bool Initialize(HMODULE hMyModule, HMODULE hParentModule) {
	g_module = hMyModule;
	loadConfig();
	if (g_enabled) {
		setupHooks();       // the keyboard hook + wndproc are installed later, from CreateDevice (UI thread)
		if (g_dpiAware) applyDpiAwareness();   // before the game creates its window (Initialize runs before WinMain)
		installGameResetHook();
		installSharpSpriteDetours();
		if (g_allowWinKey) applyAllowWinKey();   // input-only; independent of the display handling
		atexit(persistState);
		CloseHandle(CreateThread(nullptr, 0, deviceWatchThread, nullptr, 0, nullptr));
	}
	logf("DisplayManager initialized: enabled=%d filter=%s sharpness=%s mode=%s fsScale=x%d winScale=x%d "
	     "custom=%dx%d src=%dx%d resizable=%d persist=%d pos=%s(%d,%d) persistPos=%d borderless=%d vsync=%d "
	     "allowWinKey=%d latinInput=%d multiSample=%d windowedFilter=%d spriteSharpness=%.2f "
	     "backgroundSharpness=%.2f spriteStrength=%.2f..%.2f@zoom%.2f bgRest=%.2f dpiAware=%d",
	     g_enabled, filterName(), g_auto[AUTO_SHARP] ? "Auto" : "fixed", modeName(), g_intScale, g_winScale,
	     g_customW, g_customH, g_srcW, g_srcH, g_resizable, g_persist, g_havePos ? "" : "unset ", g_posX, g_posY,
	     g_persistPos, g_borderless, g_vsync, g_allowWinKey, g_latinPending, g_msaaCfg,
	     g_winFilter, (double)g_sprK[SPR_CHARS], (double)g_sprK[SPR_STAGE], (double)g_sprRest[SPR_CHARS],
	     (double)g_sprFar, (double)g_sprFarZoom, (double)g_sprRest[SPR_STAGE], g_dpiOn);
	return TRUE;
}

extern "C" int APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved) {
	return TRUE;
}
