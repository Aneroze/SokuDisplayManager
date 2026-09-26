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
// Alt+1..6 scale, spawn position, always-on-top).
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
//      device at native size instead). These changes go into a COPY of the game's struct, which is
//      never modified, so the game (and other mods) always see its real windowed/fullscreen state.
//      Windowed requests pass through untouched.
//   3. Hook the swapchain's Present: grab the 640x480 frame from the backbuffer's top-left into a
//      render-target texture, fill the borders and upscale it centered into the backbuffer (Sharp =
//      sharp-bilinear pixel shader quad, falling back to StretchRect; Point/Linear = StretchRect). The
//      viewport is pinned to 640x480 (the game relies on the default one).
// Windowed, the device is left exactly as the game made it (DM only sizes/positions the window), so
// Alt+Enter still toggles windowed <-> crisp fullscreen.
//
// If WindowResizer, IntegerFullscreen or ExclusiveFullscreen is loaded, DM detects it at device creation
// and passes everything through.
//
// Self-contained: Windows SDK headers only, no d3d9.lib import (Direct3DCreate9 is hooked via the IAT).

#include <windows.h>
#include <Shlwapi.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include "sharpbilinear.h"   // compiled ps_2_0 bytecode: g_sharpBilinearPS[]

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

// ---- vtable indices (verified against d3d9.h) ----------------------------------------------------
static const int VT_D3D9_CREATEDEVICE = 16;   // IDirect3D9::CreateDevice        (+0x40)
static const int VT_DEV_RESET         = 16;   // IDirect3DDevice9::Reset          (+0x40)
static const int VT_DEV_PRESENT       = 17;   // IDirect3DDevice9::Present         (+0x44)
static const int VT_DEV_SETRT         = 37;   // IDirect3DDevice9::SetRenderTarget (+0x94)
static const int VT_DEV_BEGINSCENE    = 41;   // IDirect3DDevice9::BeginScene      (+0xA4)
static const int VT_DEV_ENDSCENE      = 42;   // IDirect3DDevice9::EndScene        (+0xA8)
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
enum { FILTER_POINT, FILTER_LINEAR, FILTER_SHARP, FILTER_COUNT };
static int     g_filterCfg = FILTER_SHARP;
static D3DCOLOR g_bgColor  = D3DCOLOR_XRGB(0, 0, 0);  // fullscreen border/letterbox color
static bool    g_resizable = true;     // add a drag-resize border to the window (hotkeys work regardless)
static bool    g_persist   = true;     // save the current scaling settings to the ini on exit
static int     g_posX      = -1;       // spawn position (-1 = don't move the window)
static int     g_posY      = -1;
static bool    g_borderless = false;   // fullscreen as a borderless window instead of exclusive (higher latency)
static int     g_fsW       = 0;        // manual fullscreen display-mode override (0 = auto / native)
static int     g_fsH       = 0;
static int     g_fsRefresh = 0;        // manual refresh override (0 = keep the native refresh)
static int     g_vsync     = -1;       // exclusive PresentationInterval: -1 = the game's own, 0 = immediate, 1 = vsync
static bool    g_allowWinKey = false;  // [Input] AllowWinKey: drop the game's DISCL_NOWINKEY (vanilla blocks the Win key)
static bool    g_log       = false;
static FILE   *g_logFile   = nullptr;

// Hotkeys: the configured modifier + a per-action key. VK code 0 = that hotkey is disabled (which is
// also what a commented-out / missing ini line produces).
enum Action { ACT_FIT = 0, ACT_S1, ACT_S2, ACT_S3, ACT_S4, ACT_S5, ACT_S6, ACT_TOP, ACT_FILTER,
              ACT_SHARP_DOWN, ACT_SHARP_UP, ACT_COUNT };
enum ModKey { MODK_ALT = 0, MODK_CTRL, MODK_SHIFT, MODK_WIN, MODK_NONE }; // MOD_* are taken by winuser.h
static int g_hotkeyVk[ACT_COUNT];      // filled by loadConfig
static int g_modifier = MODK_ALT;

// ---- runtime state -------------------------------------------------------------------------------
static volatile bool g_createDeviceHooked = false;   // (volatile: polled by the device-watch thread)
static volatile bool g_deviceHooked       = false;
static bool      g_active             = false;  // currently forcing exclusive fullscreen?
static UINT      g_bbW = 0, g_bbH = 0;          // forced backbuffer size (= native desktop)
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
// The D3D runtime's own BeginScene/EndScene, read from the device vtable right after CreateDevice (before
// other mods patch the shared slots). Calling them directly means our Sharp pass does not re-fire every other
// mod's per-scene hook once more per frame (double work, double-ticked mod logic, PracticeEx's menu dupe).
typedef HRESULT (WINAPI *Scene_t)(IDirect3DDevice9 *);
static Scene_t g_origBeginScene = nullptr;
static Scene_t g_origEndScene   = nullptr;
static bool    g_sceneDirect    = false;   // true when the two pointers above are trusted to be the runtime's
static IDirect3DPixelShader9  *g_ps          = nullptr;  // sharp-bilinear upscale shader (Filter=Sharp)
static IDirect3DStateBlock9   *g_stateBlock  = nullptr;  // save/restore device state around the shader draw
// The game swapchain's backbuffer surface, for identity checks in the SetRenderTarget hook only. NOT
// AddRef'd (the swapchain owns it); cached after CreateDevice/Reset and each Present, dropped before Reset.
static IDirect3DSurface9      *g_bbSurf      = nullptr;
static bool                    g_inPost      = false;    // inside our Present post-process (render thread)
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

// ini names of the current Mode / Filter.
static const char *modeName() {
	return g_mode == MODE_INTEGER ? "IntegerScaling" : g_mode == MODE_CUSTOM ? "CustomResolution" : "FitToScreen";
}
static const char *filterName() {
	return g_filterCfg == FILTER_POINT ? "Point" : g_filterCfg == FILTER_LINEAR ? "Linear" : "Sharp";
}

// g_sharpness as "X.XX" (wsprintf has no %f).
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

// Overwrite one vtable slot, storing the previous entry in *orig first (so a call that goes through the
// new slot immediately already finds the original). A single aligned pointer store, safe while the render
// thread may be calling through the table. No-op if the slot already holds our hook: recording our own
// hook as the "original" would make it call itself forever.
static void hookSlot(void **vtable, int index, void *hook, void **orig) {
	if (vtable[index] == hook) return;
	DWORD old;
	VirtualProtect(&vtable[index], sizeof(void *), PAGE_READWRITE, &old);
	*orig = vtable[index];
	vtable[index] = hook;
	VirtualProtect(&vtable[index], sizeof(void *), old, &old);
}

// Resolve the centered output size (g_scaleW/H) and upscale filter from the current mode and the native
// backbuffer size (g_bbW/g_bbH). Safe to call any time the native size is known (e.g. from a hotkey).
static void computeOutput() {
	if (g_bbW == 0 || g_bbH == 0) return;
	int outW, outH;
	switch (g_mode) {
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
	// Sharp draws with its shader; linear is only its StretchRect fallback.
	g_filter = g_filterCfg == FILTER_POINT ? D3DTEXF_POINT : D3DTEXF_LINEAR;
	logf("output -> %dx%d centered at (%d,%d), filter=%s", g_scaleW, g_scaleH,
	     ((int)g_bbW - g_scaleW) / 2, ((int)g_bbH - g_scaleH) / 2,
	     g_filter == D3DTEXF_POINT ? "point" : "linear");
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
// desktop mode so the monitor is never rescaled; windowed requests pass through untouched. `pp` is always
// OUR COPY of the game's struct: the global (0x8A0F68) must keep its real values - Alt+Enter just flips its
// Windowed, the post-toggle window code, device-lost recovery and other mods read it back. Writing
// Windowed=TRUE (borderless) or the native size into it made Alt+Enter unable to leave borderless.
static void applyFullscreenParams(D3DPRESENT_PARAMETERS *pp) {
	if (!g_enabled || !pp) { g_active = false; return; }
	if (pp->hDeviceWindow) g_hwnd = pp->hDeviceWindow;   // remember the game window for windowed resizing
	g_wantFullscreen = !pp->Windowed;                    // the game's real intent (before we override it)

	if (pp->Windowed) {
		g_active = false;
		return;
	}

	UINT w = 0, h = 0, refresh = 0;
	nativeMode(&w, &h, &refresh);
	D3DFORMAT fmt = (pp->BackBufferFormat != D3DFMT_UNKNOWN) ? pp->BackBufferFormat : D3DFMT_X8R8G8B8;

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
// the runtime itself fills in. An untouched (windowed) copy goes back whole, exactly as if the game had
// passed its own struct; for a fullscreen copy we never write back our size / Windowed overrides - only
// the defaults the runtime resolves (th123 passes explicit values for both, so this is normally a no-op).
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
	g_active = false;
	g_borderlessActive = false;
	hr = call(local);
	logf("%s retry with the game's own params (DisplayManager inactive until the next Reset) -> 0x%08lx",
	     what, (long)hr);
	return hr;
}

// ---- capture render target (holds the game's rendered frame so we can rescale it) ----------------
static void releaseCapture() {
	g_bbSurf = nullptr;
	if (g_stateBlock)  { g_stateBlock->Release();  g_stateBlock = nullptr; }
	if (g_ps)          { g_ps->Release();          g_ps = nullptr; }
	if (g_stageSurf)   { g_stageSurf->Release();   g_stageSurf = nullptr; }
	if (g_captureSurf) { g_captureSurf->Release(); g_captureSurf = nullptr; }
	if (g_captureTex)  { g_captureTex->Release();  g_captureTex = nullptr; }
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
	// Sharp filter shader (falls back to StretchRect if this fails).
	HRESULT hrPs = dev->CreatePixelShader((const DWORD *)g_sharpBilinearPS, &g_ps);
	dev->CreateStateBlock(D3DSBT_ALL, &g_stateBlock);
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
// Normally (g_sceneDirect) the runtime's own BeginScene/EndScene are called, so no other mod's scene hook runs
// and the quad goes straight into the backbuffer (`target` == bb). Otherwise they go through the vtable and
// the quad is drawn into the offscreen stage while the backbuffer is bound at BeginScene/EndScene: a mod may
// redraw its 640x480 menu (PracticeEx) into whatever is bound then, and the caller overwrites that.
//
// Returns false if nothing was drawn, so the caller can fall back to StretchRect instead of presenting a
// stale or uninitialised stage.
static bool drawSharp(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb, IDirect3DSurface9 *target,
                      const RECT *dstRect) {
	if (!g_ps || !g_captureTex || !g_stateBlock) return false;
	g_stateBlock->Capture();
	dev->SetRenderTarget(0, bb);
	HRESULT hrScene = g_sceneDirect ? g_origBeginScene(dev) : dev->BeginScene();
	if (FAILED(hrScene)) { g_stateBlock->Apply(); return false; }
	if (target != bb) dev->SetRenderTarget(0, target);
	dev->SetPixelShader(g_ps);
	dev->SetVertexShader(nullptr);
	dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
	dev->SetTexture(0, g_captureTex);
	dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
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
	float c0[4] = { (float)g_srcW, (float)g_srcH, g_sharpness, 0.0f };
	dev->SetPixelShaderConstantF(0, c0, 1);
	float L = dstRect->left - 0.5f, T = dstRect->top - 0.5f, R = dstRect->right - 0.5f, B = dstRect->bottom - 0.5f;
	struct V { float x, y, z, rhw, u, v; } q[4] = {
		{ L, T, 0.0f, 1.0f, 0.0f, 0.0f },
		{ R, T, 0.0f, 1.0f, 1.0f, 0.0f },
		{ L, B, 0.0f, 1.0f, 0.0f, 1.0f },
		{ R, B, 0.0f, 1.0f, 1.0f, 1.0f },
	};
	HRESULT hr = dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
	if (target != bb) dev->SetRenderTarget(0, bb);
	if (g_sceneDirect) g_origEndScene(dev); else dev->EndScene();
	g_stateBlock->Apply();
	return SUCCEEDED(hr);
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
static const char OSD_CHARS[] = "0123456789. SHARPCEFILNOTUX";
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
};
static int osdGlyph(char c) {
	for (int i = 0; OSD_CHARS[i]; i++) if (OSD_CHARS[i] == c) return i;
	return 11;   // space
}

// Post a message to the on-screen readout for ~1.5s (drawn by drawOsd in the fullscreen present hook).
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
			HRESULT a = dev->StretchRect(bb, &srcRect, g_captureSurf, nullptr, D3DTEXF_NONE); // 1:1 grab
			// Every output pixel comes from this one upscale pass - never re-blit part of the frame with a
			// different filter (that blurred the corner overlapping the grab region at non-integer scales,
			// KNOWN-BUGS Bug 4). Without direct scene calls the upscale goes into the stage and the backbuffer
			// is filled + copied AFTER it, which also wipes PracticeEx's menu dupe from drawSharp's scene.
			IDirect3DSurface9 *target = g_stageSurf ? g_stageSurf : bb;
			HRESULT b = S_OK, c = S_OK;
			if (!g_stageSurf) fillBorders(dev, bb, dstRect);
			// Sharp falls back to StretchRect (g_filter resolves to linear for Sharp) if the shader pass
			// couldn't draw - otherwise a stale / uninitialised frame would be shown.
			if (g_filterCfg != FILTER_SHARP || !drawSharp(dev, bb, target, &dstRect))
				c = dev->StretchRect(g_captureSurf, nullptr, target, &dstRect, (D3DTEXTUREFILTERTYPE)g_filter);
			if (g_stageSurf) {
				b = dev->ColorFill(bb, nullptr, g_bgColor);                                  // borders
				c = dev->StretchRect(g_stageSurf, &dstRect, bb, &dstRect, D3DTEXF_NONE);   // 1:1 copy
			}
			if (!g_presentLogged) {
				logf("first present: grab=0x%08lx fill=0x%08lx blit=0x%08lx filterCfg=%d sharp=%.2f scene=%s",
				     (long)a, (long)b, (long)c, g_filterCfg, g_sharpness, g_sceneDirect ? "direct" : "vtable+stage");
				g_presentLogged = true;
			}
			if (GetTickCount() < g_osdUntil) drawOsd(dev, bb, &dstRect);   // hotkey readout (Alt+K/L/F/0-6)
			g_inPost = false;
			bb->Release();
			g_composited = true;
		}
		// Re-pin every frame on the render thread so nothing (Reset, SetRenderTarget, other mods) undoes it.
		if (dev) setGameViewport(dev);
	}
	HRESULT hr = oSCPresent(sc, src, dst, wnd, dirty, flags);
	if (hr != D3DERR_WASSTILLDRAWING) g_composited = false;   // presented (or dropped): next call is a new frame
	return hr;
}

// Keep the 640x480 viewport pinned across a mid-frame SetRenderTarget: D3D9 resets the viewport to the
// whole new render target, so binding the (native-sized) backbuffer again after drawing into a texture would
// bring back the giant-Okuu scaling (see setGameViewport) for the rest of that frame. Only for the game
// device's backbuffer at index 0 while we force fullscreen, and not during our own post-process. Other mods
// may hook this slot too; hookSlot chains to whatever was there.
static HRESULT WINAPI mySetRenderTarget(IDirect3DDevice9 *dev, DWORD index, IDirect3DSurface9 *rt) {
	HRESULT hr = oSetRenderTarget(dev, index, rt);
	if (index == 0 && g_active && !g_inPost && rt && rt == g_bbSurf && dev == GAME_DEVICE && SUCCEEDED(hr))
		setGameViewport(dev);
	return hr;
}

static HRESULT WINAPI myReset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp) {
	if (dev != GAME_DEVICE) return oReset(dev, pp);   // someone else's device (the vtable is shared)
	logf("Reset: Windowed=%d %ux%u", pp ? pp->Windowed : -1,
	     pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
	releaseCapture();               // default-pool resources must be freed before Reset
	g_composited = false;           // Reset discards the backbuffer contents
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
	D3DPRESENT_PARAMETERS local, *use = pp;   // our changes go into a copy, never the game's struct
	if (pp) { local = *pp; use = &local; }
	applyFullscreenParams(use);
	HRESULT hr = callWithFallback("Reset", [dev](D3DPRESENT_PARAMETERS *p) { return oReset(dev, p); }, pp, use);
	if (pp) syncPresentParams(pp, &local);
	if (SUCCEEDED(hr)) {
		if (g_active) {
			createCapture(dev);
			setGameViewport(dev);
		}
		// A real windowed <-> fullscreen switch: set the window up for the new state (borderless popup, or
		// the restored frame + remembered scale/position + topmost). Deferred via postWindowApply, because
		// the game's own SetWindowPos runs after this Reset returns and would undo it.
		if (g_wantFullscreen != g_windowFs) {
			g_windowFs = g_wantFullscreen;
			postWindowApply(false);
		}
	}
	g_presentLogged = false;
	return hr;
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
static void captureSceneFns(void **vt) {
	HMODULE rt = moduleOf(vt[VT_UNK_QUERYINTERFACE]);
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
	hookSlot(vt, VT_DEV_RESET, (void *)myReset, (void **)&oReset);
	hookSlot(vt, VT_DEV_SETRT, (void *)mySetRenderTarget, (void **)&oSetRenderTarget);
	hookSwapChain(dev);
	g_deviceHooked = true;
	logf("device vtable hooked (Reset, SetRenderTarget) + swapchain Present");
	return true;
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
// Best-effort only: it stands down as soon as the normal CreateDevice hook is in place, and a late attach
// can still miss Resets if another mod has already redirected the game's Reset call site (0x4151AC) to a
// saved copy of the original Reset.
static DWORD WINAPI deviceWatchThread(LPVOID) {
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
	g_adapterMon = self->GetAdapterMonitor(adapter);
	g_d3d = self; g_adapter = adapter;
	D3DPRESENT_PARAMETERS local, *use = pp;   // our changes go into a copy, never the game's struct
	if (pp) { local = *pp; use = &local; }
	applyFullscreenParams(use);         // sets g_hwnd from pp->hDeviceWindow
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

// Line starting (after blanks) with `key`, optional blanks, '='? Case-insensitive, like GetPrivateProfileString.
static bool isKeyLine(const char *p, const char *key) {
	int n = lstrlenA(key);
	if (StrCmpNIA(p, key, n) != 0) return false;
	p += n;
	while (*p == ' ' || *p == '\t') p++;
	return *p == '=';
}

// One-time ini upgrades for inis from older versions (only with PersistState=1, i.e. when the user lets us
// write the ini), done in place so values, comments and key order are kept (WritePrivateProfileString would
// append new keys at the end of the section instead):
//   - rename IntegerScaling (<= 1.0.3) to FullscreenScale;
//   - add WindowScale (1.0.3+) with its comment right after it, set to the value loadConfig falls back to.
// Skipped for UTF-16 inis and on any I/O error - the fallback reads in loadConfig keep old files working.
static void migrateIni() {
	const bool rename = iniHasKey(LEGACY_FS_SCALE_KEY) && !iniHasKey("FullscreenScale");
	const bool addWin = !iniHasKey("WindowScale");
	if (!rename && !addWin) return;
	HANDLE f = CreateFileA(g_iniPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (f == INVALID_HANDLE_VALUE) return;
	DWORD size = GetFileSize(f, nullptr), got = 0;
	char *buf = (size != INVALID_FILE_SIZE && size < (1u << 20)) ? (char *)malloc(size + 1) : nullptr;
	bool ok = buf && ReadFile(f, buf, size, &got, nullptr) && got == size;
	CloseHandle(f);
	if (!ok || (size >= 2 && (BYTE)buf[0] == 0xFF && (BYTE)buf[1] == 0xFE)) { free(buf); return; }
	buf[size] = 0;
	// The fullscreen scale line in [Display], under the name it has now.
	const char *key = rename ? LEGACY_FS_SCALE_KEY : "FullscreenScale";
	bool inDisplay = false;
	char *hit = nullptr;
	for (char *line = buf; *line && !hit; ) {
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '[') inDisplay = StrCmpNIA(p, "[Display]", 9) == 0;
		else if (inDisplay && isKeyLine(p, key)) hit = p;
		char *nl = strchr(line, '\n');
		line = nl ? nl + 1 : line + lstrlenA(line);
	}
	if (!hit) { free(buf); return; }

	const char *eolStr = strstr(buf, "\r\n") ? "\r\n" : "\n";   // match the file's line endings
	char *eol = strchr(hit, '\n');
	char *after = eol ? eol + 1 : buf + size;                    // start of the line after the scale line
	char ins[640] = "";
	if (addWin) {                                                // same text as the shipped ini
		char val[32] = {0};
		GetPrivateProfileStringA("Display", key, "x2", val, sizeof(val), g_iniPath);
		const char *e = eolStr;
		wsprintfA(ins, "%s%s"
		          "; WINDOW size (windowed mode), as a whole-number scale of 640x480%s"
		          "; (x2 = a 1280x960 client area). The Scale hotkeys change it while windowed;%s"
		          "; it is separate from the fullscreen Mode above. Reduced to the largest scale%s"
		          "; that fits your monitor's work area, and the window is kept on-screen. If%s"
		          "; missing, the FullscreenScale value is used.%s"
		          "WindowScale=%s%s",
		          eol ? "" : e, e, e, e, e, e, e, val, e);
	}
	const char *keyEnd = hit + lstrlenA(key);
	char tmp[1024 + MAX_PATH + 8];
	wsprintfA(tmp, "%s.tmp", g_iniPath);
	HANDLE o = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (o != INVALID_HANDLE_VALUE) {
		struct { const char *p; DWORD n; } parts[] = {
			{ buf,               (DWORD)(hit - buf) },            // up to the scale key
			{ "FullscreenScale", 15 },                            // its (new) name
			{ keyEnd,            (DWORD)(after - keyEnd) },       // the rest of that line
			{ ins,               (DWORD)lstrlenA(ins) },          // the WindowScale block, if added
			{ after,             (DWORD)(buf + size - after) },   // the rest of the file
		};
		bool wrote = true;
		for (auto &pt : parts) {
			DWORD w = 0;
			if (pt.n && !(WriteFile(o, pt.p, pt.n, &w, nullptr) && w == pt.n)) { wrote = false; break; }
		}
		CloseHandle(o);
		if (wrote && MoveFileExA(tmp, g_iniPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			logf("ini migrated:%s%s", rename ? " IntegerScaling -> FullscreenScale" : "", addWin ? " added WindowScale" : "");
		else
			DeleteFileA(tmp);
	}
	free(buf);
}

static void loadConfig() {
	GetModuleFileNameA(g_module, g_iniPath, 1024);
	PathRemoveFileSpecA(g_iniPath);
	PathAppendA(g_iniPath, "DisplayManager.ini");
	g_enabled = GetPrivateProfileIntA("Display", "Enabled", 1, g_iniPath) != 0;
	g_log = GetPrivateProfileIntA("Display", "Log", 0, g_iniPath) != 0;   // early, so migrateIni can log
	g_persist = GetPrivateProfileIntA("Display", "PersistState", 1, g_iniPath) != 0;
	if (g_persist) migrateIni();

	char mode[64] = {0};
	GetPrivateProfileStringA("Display", "Mode", "FitToScreen", mode, sizeof(mode), g_iniPath);
	if (StrCmpIA(mode, "IntegerScaling") == 0)        g_mode = MODE_INTEGER;
	else if (StrCmpIA(mode, "CustomResolution") == 0) g_mode = MODE_CUSTOM;
	else                                              g_mode = MODE_FIT;

	// FullscreenScale was called IntegerScaling up to 1.0.3: read the old name when the new one is missing
	// (migrateIni renames it in the file when PersistState is on).
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
	GetPrivateProfileStringA("Display", "Filter", "Sharp", filt, sizeof(filt), g_iniPath);
	if      (StrCmpIA(filt, "Point") == 0)  g_filterCfg = FILTER_POINT;
	else if (StrCmpIA(filt, "Linear") == 0) g_filterCfg = FILTER_LINEAR;
	else                                    g_filterCfg = FILTER_SHARP;   // the default; also the removed "Auto"

	char sharp[32] = {0};
	GetPrivateProfileStringA("Display", "Sharpness", "1.50", sharp, sizeof(sharp), g_iniPath);
	g_sharpness = (float)atof(sharp);
	clampSharpness();

	g_resizable = GetPrivateProfileIntA("Display", "Resizable", 1, g_iniPath) != 0;
	g_posX      = GetPrivateProfileIntA("Display", "PositionX", -1, g_iniPath);
	g_posY      = GetPrivateProfileIntA("Display", "PositionY", -1, g_iniPath);
	g_borderless   = GetPrivateProfileIntA("Display", "Borderless", 0, g_iniPath) != 0;
	g_fsW          = GetPrivateProfileIntA("Display", "FullscreenWidth", 0, g_iniPath);
	g_fsH          = GetPrivateProfileIntA("Display", "FullscreenHeight", 0, g_iniPath);
	g_fsRefresh    = GetPrivateProfileIntA("Display", "FullscreenRefresh", 0, g_iniPath);
	g_vsync        = GetPrivateProfileIntA("Display", "VSync", -1, g_iniPath);
	g_allowWinKey  = GetPrivateProfileIntA("Input", "AllowWinKey", 0, g_iniPath) != 0;

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
	                                 "SharpnessDown", "SharpnessUp" };
	for (int a = 0; a < ACT_COUNT; a++) {
		char k[16] = {0};
		GetPrivateProfileStringA("Hotkeys", names[a], "", k, sizeof(k), g_iniPath);
		g_hotkeyVk[a] = parseKey(k);
	}
}

// Write "Display"/key = val only if it differs from what's already in the ini, so an unchanged session
// doesn't re-serialize the file (which would change its timestamp and prompt editors to reload it).
static void writeIniIfChanged(const char *key, const char *val) {
	char cur[64] = {0};
	GetPrivateProfileStringA("Display", key, "\x01", cur, sizeof(cur), g_iniPath);  // sentinel default
	if (lstrcmpA(cur, val) != 0)
		WritePrivateProfileStringA("Display", key, val, g_iniPath);
}

// Save Mode, FullscreenScale, WindowScale, Filter and Sharpness (incl. live hotkey changes) to the ini at
// exit. Window position is deliberately NOT saved; unchanged keys are not rewritten.
static void persistState() {
	if (!g_persist || !g_enabled) return;   // (g_enabled is cleared when standing down for another mod)
	char scale[16];
	wsprintfA(scale, "x%d", g_intScale);
	writeIniIfChanged("Mode", modeName());
	writeIniIfChanged("FullscreenScale", scale);
	if (iniHasKey(LEGACY_FS_SCALE_KEY))                       // leftover pre-1.0.4 key (migration skipped)
		WritePrivateProfileStringA("Display", LEGACY_FS_SCALE_KEY, nullptr, g_iniPath);
	wsprintfA(scale, "x%d", g_winScale);
	writeIniIfChanged("WindowScale", scale);

	writeIniIfChanged("Filter", filterName());
	char sh[16];
	formatSharpness(sh);
	writeIniIfChanged("Sharpness", sh);
}

// ---- hotkeys (WindowResizer-style): Alt+0 = FitToScreen, Alt+1..6 = x1..x6, Alt+P/F/K/L ------------
// A WH_KEYBOARD hook on the game's UI thread (as WindowResizer does): it sees the game's own key messages,
// so it works in exclusive fullscreen, where a GetAsyncKeyState poll did not. Changes apply live (they only
// affect the post-process), no device reset.
static HHOOK g_kbHook = nullptr;

// lParam bit 30 = previous key state, bit 31 = transition. Both 0 means a fresh key-down (not a repeat
// or a release).
#define IS_FRESH_KEYDOWN(lp) (((lp) & (1 << 30)) == 0 && ((lp) & (1 << 31)) == 0)

// Total non-client border size (width, height) for the game window's current style.
static void windowBorders(int *bx, int *by) {
	RECT r = { 0, 0, 0, 0 };
	AdjustWindowRectEx(&r, GetWindowLongA(g_hwnd, GWL_STYLE), GetMenu(g_hwnd) != nullptr,
	                   GetWindowLongA(g_hwnd, GWL_EXSTYLE));
	*bx = r.right - r.left; *by = r.bottom - r.top;
}

// Resize the game's window to a (srcW*n) x (srcH*n) client, optionally moving its top-left to `pos`. Only the
// window changes: D3D9's windowed present stretches the existing backbuffer, so no (unsafe, external) device
// reset is needed. Mirrors WindowResizer. Clamped to the work area of the monitor it lands on (largest
// integer scale that fits, moved back on-screen); the slack allows for Windows 10's invisible resize borders.
static void setWindowScaled(int n, const POINT *pos) {
	if (!g_hwnd || n < 1) return;
	int bx, by; windowBorders(&bx, &by);
	RECT wr;
	if (!GetWindowRect(g_hwnd, &wr)) return;
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
static void onWindowedEntry(bool firstTime) {
	if (g_styleSaved && g_hwnd) {
		SetWindowLongA(g_hwnd, GWL_STYLE, g_savedStyle);
		SetWindowLongA(g_hwnd, GWL_EXSTYLE, g_savedExStyle);
		SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
		             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
		g_styleSaved = false;
	}
	g_borderlessActive = false;
	POINT spawn = { g_posX, g_posY };
	const POINT *pos = firstTime ? ((g_posX >= 0 && g_posY >= 0) ? &spawn : nullptr)
	                             : (g_haveWinPos ? &g_winPos : nullptr);
	setWindowScaled(g_winScale, pos);
	applyTopmost();
}

// Set the window up for the current state: windowed (onWindowedEntry; the first time with the spawn
// setup) or the borderless popup. Exclusive fullscreen needs nothing - D3D9 owns the screen, and neither
// does the game's own fullscreen when the forced mode failed and we fell back to it (g_active false).
static void applyWindowState() {
	bool firstTime = g_spawnPending;
	g_spawnPending = false;
	if (!g_wantFullscreen)             onWindowedEntry(firstTime);
	else if (g_borderless && g_active) enterBorderlessFullscreen();
}

static WNDPROC g_origWndProc = nullptr;

// Subclassed window procedure: runs the deferred apply posted by postWindowApply, and while windowed and
// resizable locks a drag-resize to 4:3 (at least 640x480) so the stretched image never gets squashed.
static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
	if (g_applyMsg && msg == g_applyMsg) {
		applyWindowState();
		return 0;
	}
	if (msg == WM_SIZING && g_resizable && !g_active) {
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
	// Record the original before swapping, so a message dispatched in between (the install can run off the
	// window thread, from the device watch) never finds it null.
	g_origWndProc = (WNDPROC)GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC);
	WNDPROC prev = (WNDPROC)SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)wndProc);
	if (prev) g_origWndProc = prev;
	logf("wndproc subclassed (resizable=%d)", g_resizable);
}

// Apply the window state (applyWindowState) once the game has finished handling the current message.
// On Alt+Enter th123 calls Reset from its window procedure (0x4082DE -> 0x415220) and, AFTER Reset
// returns, does its own SetWindowPos: windowed = HWND_NOTOPMOST, re-centered on the primary monitor, sized
// with fixed-frame metrics (wrong for our WS_THICKFRAME, giving a non-4:3 client); fullscreen = move the
// client to the primary's origin. Anything done inside myReset is undone by that, so we post a private
// message to the game window and do the work in wndProc, after the game's own window code has run.
// Falls back to applying immediately if the window isn't subclassed.
static void postWindowApply(bool firstTime) {
	if (firstTime) g_spawnPending = true;
	if (!g_applyMsg) g_applyMsg = RegisterWindowMessageA("DisplayManager.ApplyWindowState");
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
		g_filterCfg = (g_filterCfg + 1) % FILTER_COUNT;   // Point -> Linear -> Sharp -> Point
		if (g_active) computeOutput();                    // re-resolve g_filter now; next frame's present uses it
		if (g_filterCfg == FILTER_SHARP) showSharpnessOsd();
		else showOsd(g_filterCfg == FILTER_POINT ? "POINT" : "LINEAR");
		logf("hotkey: filter -> %s", filterName());
		break;
	case ACT_SHARP_DOWN:
	case ACT_SHARP_UP:
		g_filterCfg = FILTER_SHARP;             // sharpness only affects Sharp, so switch to it
		if (g_active) computeOutput();
		g_sharpness += (act == ACT_SHARP_UP) ? 0.25f : -0.25f;   // live; the shader reads it each frame
		clampSharpness();
		showSharpnessOsd();
		logf("hotkey: filter=Sharp sharpness -> %.2f", g_sharpness);
		break;
	}
}

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
// that is the whole client (D3D9 stretches the 640x480 backbuffer over it). In fullscreen (exclusive or
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

extern "C" __declspec(dllexport) bool CheckVersion(const BYTE hash[16]) {
	return ::memcmp(TARGET_HASH, hash, sizeof TARGET_HASH) == 0;
}

extern "C" __declspec(dllexport) bool Initialize(HMODULE hMyModule, HMODULE hParentModule) {
	g_module = hMyModule;
	loadConfig();
	if (g_enabled) {
		setupHooks();       // the keyboard hook + wndproc are installed later, from CreateDevice (UI thread)
		if (g_allowWinKey) applyAllowWinKey();   // input-only; independent of the display handling
		atexit(persistState);
		CloseHandle(CreateThread(nullptr, 0, deviceWatchThread, nullptr, 0, nullptr));
	}
	logf("DisplayManager initialized: enabled=%d mode=%s fsScale=x%d winScale=x%d custom=%dx%d src=%dx%d "
	     "resizable=%d persist=%d pos=(%d,%d) borderless=%d vsync=%d allowWinKey=%d",
	     g_enabled, modeName(), g_intScale, g_winScale, g_customW, g_customH, g_srcW, g_srcH,
	     g_resizable, g_persist, g_posX, g_posY, g_borderless, g_vsync, g_allowWinKey);
	return TRUE;
}

extern "C" int APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved) {
	return TRUE;
}
