// DisplayManager - crisp, integer-scaled exclusive fullscreen for Touhou Hisoutensoku (th123 1.10a)
//
// Problem this solves
// -------------------
// With WindowResizer off, the base game's fullscreen switches to a resolution that fills the monitor
// height at 4:3 and pillarboxes the sides, so 480 logical pixels are scaled by a non-integer factor
// (e.g. 1080/480 = 2.25x) and everything looks blurry. This mod instead keeps the desktop at its
// native resolution and renders the game centered with black borders. Three modes (Mode in the ini,
// and Alt+0..4 hotkeys): FitToScreen (default - largest aspect-correct size that fills the screen),
// IntegerScaling (exact x1/x2/x3..., crisp point-sampled), and CustomResolution. Because it is true
// exclusive fullscreen it also gets the low-latency direct-flip ("Independent Flip") present path -
// which a legacy Direct3D9 / DISCARD game like this one cannot get in a borderless window.
//
// How it works
// ------------
// The game builds one global D3DPRESENT_PARAMETERS (0x8A0F68) and creates a plain Direct3D9 device
// (Direct3DCreate9, not Ex; SwapEffect DISCARD). Its "fullscreen" state (0x8998B0) is literally
// (present.Windowed == 0). Crucially, the game always draws its 640x480 surface at 1:1 into the
// top-left of the backbuffer and relies on the fullscreen *display mode* to upscale the whole
// framebuffer - it does not scale its scene to the backbuffer. So we:
//   1. Intercept Direct3DCreate9 (IAT thunk at 0x8572A0) -> hook IDirect3D9::CreateDevice.
//   2. In CreateDevice/Reset, when the game asks for fullscreen (Windowed == FALSE), force the
//      backbuffer to the *native* desktop mode so the monitor never rescales. Windowed requests are
//      restored to the game's own windowed size (we must undo the large size we wrote into the shared
//      present-params struct, or it leaks into the windowed path).
//   3. Hook the swapchain's Present: the game has drawn its 640x480 frame into the
//      backbuffer's top-left. Grab it into an offscreen render target, clear the whole backbuffer
//      black, then StretchRect it back scaled to WidthxHeight, centered, with POINT filtering -> crisp
//      integer scaling with black borders, independent of how the game maps its coordinates.
// Windowed mode is passed through (restored) so Alt+Enter still toggles windowed <-> crisp fullscreen.
//
// Use either this or WindowResizer, but never both at the same time.
//
// Self-contained: it only needs the Windows SDK (windows.h / d3d9.h / shlwapi.h). It does not import
// d3d9.lib - it hooks the game's Direct3DCreate9 through the import table and drives the device the game
// itself creates, so there are no external runtime dependencies.

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
// The game's IDirect3DDevice9* global.
#define GAME_DEVICE (*reinterpret_cast<IDirect3DDevice9 **>(0x008A0E30))
// D3DDISPLAYMODE the game fetched via GetAdapterDisplayMode at startup (Width+0, Height+4, Refresh+8, Format+0xC).
static const DWORD ADDR_DESKTOP_MODE = 0x008A0FA0;

// ---- vtable indices (verified against d3d9.h) ----------------------------------------------------
static const int VT_D3D9_CREATEDEVICE = 16;   // IDirect3D9::CreateDevice        (+0x40)
static const int VT_DEV_RESET         = 16;   // IDirect3DDevice9::Reset          (+0x40)
static const int VT_SC_PRESENT        = 3;    // IDirect3DSwapChain9::Present     (+0x0C)
// NOTE: the game presents via the SWAPCHAIN (0x8A0E34)->Present, not the device, so we hook that.

// ---- config --------------------------------------------------------------------------------------
enum Mode { MODE_FIT = 0, MODE_INTEGER = 1, MODE_CUSTOM = 2 };

static HMODULE g_module;
static char    g_iniPath[1024 + MAX_PATH];
static bool    g_enabled   = true;
static int     g_mode      = MODE_FIT; // FitToScreen (default) / IntegerScaling / CustomResolution
static int     g_intScale  = 2;        // used when g_mode == MODE_INTEGER (x1, x2, ...)
static int     g_customW   = 1280;     // used when g_mode == MODE_CUSTOM
static int     g_customH   = 960;
static int     g_scaleW    = 1280;     // resolved output width  (computed from mode + native res)
static int     g_scaleH    = 960;      // resolved output height
static int     g_srcW      = 640;      // th123's fixed render size (grab region / pinned viewport); const
static int     g_srcH      = 480;      // - th123 always renders 640x480, so this is not configurable
static DWORD   g_filter    = D3DTEXF_POINT;  // resolved upscale filter for this frame
static int     g_filterCfg = 0;              // 0 = Auto (point at integer scales, linear otherwise), 1 = Point, 2 = Linear
static D3DCOLOR g_bgColor  = D3DCOLOR_XRGB(0, 0, 0);  // fullscreen border/letterbox color
static bool    g_resizable = true;     // add a drag-resize border to the window (hotkeys work regardless)
static bool    g_persist   = true;     // save the current scaling settings to the ini on exit
static int     g_posX      = -1;       // spawn position (-1 = don't move the window, the mod's old behavior)
static int     g_posY      = -1;
static bool    g_borderless = false;   // fullscreen as a borderless window instead of exclusive (higher latency)
static int     g_fsW       = 0;        // manual fullscreen display-mode override (0 = auto / native)
static int     g_fsH       = 0;
static int     g_fsRefresh = 0;        // manual refresh override (0 = keep the native refresh)
static bool    g_log       = false;
static FILE   *g_logFile   = nullptr;

// Hotkeys: the configured modifier + a per-action key. VK code 0 = that hotkey is disabled (which is
// also what a commented-out / missing ini line produces).
enum Action { ACT_FIT = 0, ACT_S1, ACT_S2, ACT_S3, ACT_S4, ACT_S5, ACT_S6, ACT_TOP, ACT_FILTER,
              ACT_SHARP_DOWN, ACT_SHARP_UP, ACT_COUNT };
enum ModKey { MODK_ALT = 0, MODK_CTRL, MODK_SHIFT, MODK_WIN, MODK_NONE }; // MOD_* are taken by winuser.h
static int g_hotkeyVk[ACT_COUNT];      // filled by loadConfig
static int g_modifier = MODK_ALT;      // the modifier held with each hotkey key

// ---- runtime state -------------------------------------------------------------------------------
static bool      g_createDeviceHooked = false;
static bool      g_deviceHooked       = false;
static bool      g_active             = false;  // currently forcing exclusive fullscreen?
static UINT      g_bbW = 0, g_bbH = 0;          // forced backbuffer size (= native desktop)
static D3DFORMAT g_bbFormat = D3DFMT_X8R8G8B8;  // backbuffer format (for the capture RT)
// Grabbed game frame: a render-target TEXTURE (so the Sharp shader can sample it) plus its level-0
// surface (the StretchRect grab target, and the source for Point/Linear StretchRect upscales).
static IDirect3DTexture9      *g_captureTex  = nullptr;
static IDirect3DSurface9      *g_captureSurf = nullptr;
// Offscreen backbuffer-sized render target the upscale is composed into, then copied 1:1 to the backbuffer.
// Keeps the upscaled frame out of reach of the redraw some mods do during our EndScene (see drawSharp).
static IDirect3DSurface9      *g_stageSurf   = nullptr;
static IDirect3DPixelShader9  *g_ps          = nullptr;  // sharp-bilinear upscale shader (Filter=Sharp)
static IDirect3DStateBlock9   *g_stateBlock  = nullptr;  // save/restore device state around the shader draw
static float                   g_sharpness   = 1.50f;    // 1 = aligned bilinear; higher = crisper toward point
static const float             SHARP_MIN     = 1.0f;     // clamp: 1.0 = bilinear
static const float             SHARP_MAX     = 4.0f;     // clamp: ~4.0 is already visually point (shader
                                                         // interp band = 0.5/sharp), so no point going higher
static volatile DWORD          g_osdUntil = 0;          // GetTickCount() deadline for the OSD toast
static char                    g_osdText[32] = {0};     // current OSD message (uppercase; see OSD_CHARS)
// The game's canonical windowed backbuffer size, captured from the first (windowed) CreateDevice, so a
// return to windowed restores it instead of inheriting the huge fullscreen size we wrote into the
// game's shared present-params struct.
static bool g_haveWin = false;
static UINT g_winW = 640, g_winH = 480;
static HWND g_hwnd = nullptr;   // the game's window (from present params), for windowed resizing
// Monitor of the adapter the game's device was created on. Exclusive fullscreen always takes over THIS
// monitor (the adapter is fixed at CreateDevice), wherever the window has been dragged.
static HMONITOR g_adapterMon = nullptr;
static bool g_topmost = false;          // always-on-top toggle (Alt+P)
// Borderless-mode state: g_wantFullscreen tracks the game's real intent (from pp.Windowed before we
// override it) so we can tell a forced-windowed borderless-fullscreen apart from a genuine windowed
// request. Saved styles restore the normal window when leaving borderless fullscreen.
static bool g_wantFullscreen = false;
static bool g_borderlessActive = false;
static bool g_styleSaved = false;
static LONG g_savedStyle = 0, g_savedExStyle = 0;

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

// forward declarations (definitions live further down)
static void installKeyboardHook();
static void installWndProc();
static void setWindowScaled(int n, bool applyPos);
static void onWindowedEntry(bool firstTime);
static void enterBorderlessFullscreen();
static void applyTopmost();

// Overwrite `len` bytes at `addr` with NOPs (used only in borderless mode to stop the game deriving its
// fullscreen flag from present.Windowed, so we can own that flag ourselves).
static void patchNop(DWORD addr, int len) {
	DWORD old;
	VirtualProtect((void *)addr, len, PAGE_EXECUTE_READWRITE, &old);
	for (int i = 0; i < len; i++) ((BYTE *)addr)[i] = 0x90;
	VirtualProtect((void *)addr, len, old, &old);
	FlushInstructionCache(GetCurrentProcess(), (void *)addr, len);
}

// In borderless mode we present a windowed device, but the game must still believe it is fullscreen so
// its Alt+Enter toggle flips the right way. We write the flag (0x8998B0) ourselves; the game's own write
// to it is NOP'd (see setupHooks).
static void writeFsFlag(bool fs) {
	if (g_borderless) *(BYTE *)0x008998B0 = fs ? 1 : 0;
}

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

// Overwrite one vtable slot, returning the previous entry. A single aligned pointer store, safe while
// the render thread may be calling through the table.
static void *hookSlot(void **vtable, int index, void *hook) {
	DWORD old;
	VirtualProtect(&vtable[index], sizeof(void *), PAGE_READWRITE, &old);
	void *prev = vtable[index];
	vtable[index] = hook;
	VirtualProtect(&vtable[index], sizeof(void *), old, &old);
	return prev;
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
			logf("IntegerScaling x%d doesn't fit %ux%u; using x%d", n, g_bbW, g_bbH, use);
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
	// Filter: Auto = point at exact integer multiples (crisp) and linear otherwise (avoids the uneven
	// doubled/tripled pixels of non-integer point scaling); or force one via the ini. Point keeps hard
	// pixels even at non-integer scales (sharper, slightly uneven - what WindowResizer's stretch does).
	if (g_filterCfg == 1)      g_filter = D3DTEXF_POINT;
	else if (g_filterCfg == 2) g_filter = D3DTEXF_LINEAR;
	else if (g_filterCfg == 3) g_filter = D3DTEXF_LINEAR;   // Sharp: shader does the work; StretchRect fallback
	else                       g_filter = (outW % g_srcW == 0 && outH % g_srcH == 0) ? D3DTEXF_POINT
	                                                                                 : D3DTEXF_LINEAR;
	logf("output -> %dx%d centered at (%d,%d), filter=%s", g_scaleW, g_scaleH,
	     ((int)g_bbW - g_scaleW) / 2, ((int)g_bbH - g_scaleH) / 2,
	     g_filter == D3DTEXF_POINT ? "point" : "linear");
}

// The native resolution/refresh of the monitor fullscreen will land on. We query this live (rather than
// trusting the game's cached GetAdapterDisplayMode global at 0x8A0FA0, which can be stale or the wrong
// monitor) so exclusive fullscreen always uses the true current mode - otherwise the desktop gets
// switched to a wrong (often small) resolution, which is blurry and shuffles the user's windows.
// Exclusive uses the device's ADAPTER monitor: the window's monitor is wrong once the window has been
// dragged to another screen (e.g. a 1080p second monitor's mode got applied to a 1440p primary).
// Borderless is a windowed device, so it can cover whichever monitor the window is on.
static void nativeMode(UINT *w, UINT *h, UINT *refresh) {
	*w = *h = *refresh = 0;
	HMONITOR mon = (!g_borderless && g_adapterMon) ? g_adapterMon
	             : g_hwnd ? MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY) : nullptr;
	if (mon) {
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
	// Manual override: force a specific fullscreen display mode (e.g. when auto-detection is wrong, or to
	// run the screen at a non-native resolution on purpose).
	if (g_fsW > 0 && g_fsH > 0) {
		*w = g_fsW; *h = g_fsH;
		if (g_fsRefresh > 0) *refresh = g_fsRefresh;
	}
}

// Decide how to shape the present parameters for this (Create)Device/Reset call. Fullscreen requests
// (Windowed == FALSE) are forced to the native desktop mode so the monitor is never rescaled; windowed
// requests are restored to the game's canonical windowed size (undoing the huge size we write into the
// game's shared present-params struct while fullscreen, which otherwise leaks into the windowed path).
static void applyFullscreenParams(D3DPRESENT_PARAMETERS *pp) {
	if (!g_enabled || !pp) { g_active = false; return; }
	if (pp->hDeviceWindow) g_hwnd = pp->hDeviceWindow;   // remember the game window for windowed resizing
	g_wantFullscreen = !pp->Windowed;                    // the game's real intent (before we override it)

	UINT w = 0, h = 0, refresh = 0;
	nativeMode(&w, &h, &refresh);

	// Border/letterbox format & backbuffer target used by both fullscreen paths.
	D3DFORMAT fmt = (pp->BackBufferFormat != D3DFMT_UNKNOWN) ? pp->BackBufferFormat : D3DFMT_X8R8G8B8;

	if (pp->Windowed) {
		// The game wants a normal window. Remember its canonical size the first time (before we meddle).
		if (!g_haveWin && pp->BackBufferWidth && pp->BackBufferHeight &&
		    pp->BackBufferWidth <= 4096 && pp->BackBufferHeight <= 4096) {
			g_winW = pp->BackBufferWidth; g_winH = pp->BackBufferHeight; g_haveWin = true;
		}
		if (g_haveWin) { pp->BackBufferWidth = g_winW; pp->BackBufferHeight = g_winH; }
		pp->FullScreen_RefreshRateInHz = 0;
		g_active = false;
		return;
	}

	// The game wants fullscreen.
	if (g_borderless) {
		// Borderless: a windowed device with a native-sized backbuffer; we cover the monitor with a
		// borderless window ourselves (done after the reset, in enterBorderlessFullscreen). No exclusive
		// mode-set, so no low-latency direct-flip - but it is friendlier to alt-tab / overlays.
		pp->Windowed = TRUE;
		pp->BackBufferWidth  = w;
		pp->BackBufferHeight = h;
		pp->FullScreen_RefreshRateInHz = 0;
		g_borderlessActive = true;
		logf("borderless fullscreen -> native %ux%u (windowed device)", w, h);
	} else {
		// Exclusive: true fullscreen at the native mode (no monitor rescale, gets Independent Flip).
		pp->BackBufferWidth  = w;
		pp->BackBufferHeight = h;
		pp->FullScreen_RefreshRateInHz = refresh;
		logf("exclusive fullscreen -> native %ux%u @%uHz", w, h, refresh);
	}
	g_bbW = w; g_bbH = h; g_bbFormat = fmt; g_active = true;
	computeOutput();
}

// ---- capture render target (holds the game's rendered frame so we can rescale it) ----------------
static void releaseCapture() {
	if (g_stateBlock)  { g_stateBlock->Release();  g_stateBlock = nullptr; }
	if (g_ps)          { g_ps->Release();          g_ps = nullptr; }
	if (g_stageSurf)   { g_stageSurf->Release();   g_stageSurf = nullptr; }
	if (g_captureSurf) { g_captureSurf->Release(); g_captureSurf = nullptr; }
	if (g_captureTex)  { g_captureTex->Release();  g_captureTex = nullptr; }
}

static void createCapture(IDirect3DDevice9 *dev) {
	releaseCapture();
	// A render-target texture (usable both as a StretchRect surface and a shader source).
	HRESULT hr = dev->CreateTexture((UINT)g_srcW, (UINT)g_srcH, 1, D3DUSAGE_RENDERTARGET, g_bbFormat,
	                                D3DPOOL_DEFAULT, &g_captureTex, nullptr);
	if (SUCCEEDED(hr) && g_captureTex)
		g_captureTex->GetSurfaceLevel(0, &g_captureSurf);
	HRESULT hrStage = dev->CreateRenderTarget(g_bbW, g_bbH, g_bbFormat, D3DMULTISAMPLE_NONE, 0, FALSE,
	                                          &g_stageSurf, nullptr);
	// Compile-once pixel shader for the Sharp filter (falls back to StretchRect if this fails).
	HRESULT hrPs = dev->CreatePixelShader((const DWORD *)g_sharpBilinearPS, &g_ps);
	dev->CreateStateBlock(D3DSBT_ALL, &g_stateBlock);
	logf("createCapture %dx%d fmt=%d -> tex=0x%08lx stage=0x%08lx ps=0x%08lx", g_srcW, g_srcH, (int)g_bbFormat,
	     (long)hr, (long)hrStage, (long)hrPs);
}

// ---- device / swapchain method hooks -------------------------------------------------------------
static bool g_presentLogged = false;

// Sharp-bilinear upscale: draw a full-screen quad over the (centered) destination rect, sampling the
// captured 640 texture through the sharp-bilinear shader. The quad is shifted by the D3D9 -0.5 half-pixel
// offset so an output pixel at screen x samples texel-space (x+0.5)/scale, i.e. output pixel centers land
// on (k+0.5)/N. WITHOUT the offset, at scale N every output pixel samples exactly k/N, which for integer N
// only ever hits texel positions where the sharp-bilinear math is a no-op (at 2x only s=0.0 -> fixed 50/50
// blend and s=0.5 -> exact texel), so Sharpness has NO visible effect. See
// docs/WindowResizer-rendering-research.md L166-173. Sharpness 1 = aligned bilinear; higher narrows the
// interpolation band toward point (~1.5 matches WR).
//
// The quad is drawn into `target` (the offscreen stage), but the backbuffer is the bound render target
// at BeginScene and again at EndScene: our Begin/EndScene re-triggers a mod redraw (PracticeEx's 640x480
// menu) into whatever is bound then, so it lands in the backbuffer - which the caller wipes and overwrites
// with the stage - instead of on the upscaled frame.
//
// Returns false if nothing was drawn (a resource is missing, or BeginScene / the draw failed), so the
// caller can fall back to a StretchRect upscale instead of presenting a stale or uninitialised stage.
static bool drawSharp(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb, IDirect3DSurface9 *target,
                      const RECT *dstRect) {
	if (!g_ps || !g_captureTex || !g_stateBlock) return false;
	g_stateBlock->Capture();                                  // save all device state
	dev->SetRenderTarget(0, bb);
	if (FAILED(dev->BeginScene())) { g_stateBlock->Apply(); return false; }
	dev->SetRenderTarget(0, target);
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
	dev->SetRenderTarget(0, bb);
	dev->EndScene();
	g_stateBlock->Apply();                                    // restore all device state
	return SUCCEEDED(hr);
}

// Pin the game's viewport to its 640x480 (g_srcW x g_srcH) frame. th123 never calls SetViewport: it
// relies on D3D9 setting the viewport to the whole render target at CreateDevice/Reset, which is 640x480
// in vanilla. With our native-sized backbuffer that default viewport is e.g. 2560x1440, and every draw
// that uses TRANSFORMED (non-RHW) vertices - the 3D stage and Okuu (Utsuho) - is mapped through it, i.e.
// scaled by backbuffer/640 from the top-left corner: the giant off-screen Okuu. Pre-transformed (XYZRHW)
// sprites ignore the viewport, which is why only she looked wrong.
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

// Build "SHARP X.XX" from the current sharpness into buf.
static void sharpOsdText(char *buf, int cap) {
	int hn = (int)(g_sharpness * 100.0f + 0.5f);
	wsprintfA(buf, "SHARP %d.%02d", hn / 100, hn % 100);
	(void)cap;
}

static HRESULT WINAPI mySCPresent(IDirect3DSwapChain9 *sc, const RECT *src, const RECT *dst,
                                  HWND wnd, const RGNDATA *dirty, DWORD flags) {
	// Post-process: the game has rendered its g_srcW x g_srcH surface into the top-left of a native-
	// resolution backbuffer. Grab that region, wipe the whole backbuffer black, then blit it back
	// scaled + centered. Point/Linear go through StretchRect; Sharp goes through the shader quad.
	if (g_active && g_captureSurf) {
		IDirect3DDevice9 *dev = GAME_DEVICE;
		IDirect3DSurface9 *bb = nullptr;
		if (dev && SUCCEEDED(sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
			RECT srcRect = { 0, 0, g_srcW, g_srcH };
			LONG x = ((LONG)g_bbW - g_scaleW) / 2, y = ((LONG)g_bbH - g_scaleH) / 2;
			RECT dstRect = { x, y, x + g_scaleW, y + g_scaleH };
			HRESULT a = dev->StretchRect(bb, &srcRect, g_captureSurf, nullptr, D3DTEXF_NONE); // 1:1 grab
			// Compose the upscale into the stage (or straight into the backbuffer if the stage couldn't be
			// created), then clear the backbuffer and copy the finished frame over 1:1. Clearing AFTER the
			// upscale also wipes the un-upscaled menu dupe PracticeEx redraws into the backbuffer's top-left
			// during drawSharp's scene. Every output pixel comes from this one upscale pass - never re-blit part
			// of the frame with a different filter (that made the corner overlapping the grab region blurry at
			// non-integer FitToScreen scales, e.g. 2.25x on a 1080p screen).
			IDirect3DSurface9 *target = g_stageSurf ? g_stageSurf : bb;
			HRESULT b = S_OK, c = S_OK;
			if (!g_stageSurf) b = dev->ColorFill(bb, nullptr, g_bgColor);
			// Sharp falls back to StretchRect (g_filter resolves to linear for Sharp) if the shader pass
			// couldn't draw - otherwise the stale / uninitialised stage would be copied to the screen.
			if (g_filterCfg != 3 || !drawSharp(dev, bb, target, &dstRect))
				c = dev->StretchRect(g_captureSurf, nullptr, target, &dstRect, (D3DTEXTUREFILTERTYPE)g_filter);
			if (g_stageSurf) {
				b = dev->ColorFill(bb, nullptr, g_bgColor);                                  // borders
				c = dev->StretchRect(g_stageSurf, &dstRect, bb, &dstRect, D3DTEXF_NONE);   // 1:1 copy
			}
			if (!g_presentLogged) {
				logf("first present: grab=0x%08lx fill=0x%08lx blit=0x%08lx filterCfg=%d sharp=%.2f",
				     (long)a, (long)b, (long)c, g_filterCfg, g_sharpness);
				g_presentLogged = true;
			}
			if (GetTickCount() < g_osdUntil) drawOsd(dev, bb, &dstRect);   // hotkey readout (Alt+K/L/F/0-6)
			bb->Release();
		}
		// Re-pin every frame on the render thread so nothing (Reset, SetRenderTarget, other mods) undoes it.
		if (dev) setGameViewport(dev);
	}
	return oSCPresent(sc, src, dst, wnd, dirty, flags);
}

static HRESULT WINAPI myReset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp) {
	logf("Reset: Windowed=%d %ux%u", pp ? pp->Windowed : -1,
	     pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
	releaseCapture();               // default-pool resources must be freed before Reset
	if (!g_adapterMon) {            // device hooked without our CreateDevice (device-watch fallback)
		D3DDEVICE_CREATION_PARAMETERS cp; IDirect3D9 *d3d = nullptr;
		if (SUCCEEDED(dev->GetCreationParameters(&cp)) && SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d) {
			g_adapterMon = d3d->GetAdapterMonitor(cp.AdapterOrdinal);
			d3d->Release();
		}
	}
	applyFullscreenParams(pp);
	HRESULT hr = oReset(dev, pp);
	if (SUCCEEDED(hr)) {
		if (g_active) {
			createCapture(dev);
			setGameViewport(dev);
		}
		if (g_wantFullscreen) {
			if (g_borderless) enterBorderlessFullscreen();
		} else {
			onWindowedEntry(false); // returning to windowed: restore frame + remembered window scale
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
		oSCPresent = (SCPresent_t)hookSlot(vt, VT_SC_PRESENT, (void *)mySCPresent);
		sc->Release();
		logf("swapchain Present hooked");
	} else {
		logf("GetSwapChain failed - cannot hook Present");
	}
}

static void hookDevice(IDirect3DDevice9 *dev) {
	if (g_deviceHooked || !dev) return;
	void **vt = *(void ***)dev;
	oReset = (Reset_t)hookSlot(vt, VT_DEV_RESET, (void *)myReset);
	hookSwapChain(dev);
	g_deviceHooked = true;
	logf("device vtable hooked (Reset) + swapchain Present");
}

// Is `fn` inside d3d9.dll? Used to confirm the device global holds a real Direct3D(9/9Ex) device with a
// standard vtable before we patch it (a wrapper device would point elsewhere - we skip those safely).
static bool isInsideD3D9(void *fn) {
	HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
	if (!d3d9 || !fn) return false;
	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)d3d9;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)d3d9 + dos->e_lfanew);
	BYTE *base = (BYTE *)d3d9, *end = base + nt->OptionalHeader.SizeOfImage;
	return (BYTE *)fn >= base && (BYTE *)fn < end;
}

// Fallback path when our Direct3DCreate9 hook never fires - e.g. SokuDirectXOptimizations creates a
// Direct3D9Ex device (via Direct3DCreate9Ex, a different export) or otherwise intercepts creation. We
// poll the game's device global (0x8A0E30) and hook the device once it exists, regardless of who made it.
static DWORD WINAPI deviceWatchThread(LPVOID) {
	for (int i = 0; i < 1200 && !g_deviceHooked; i++) {   // ~60s
		IDirect3DDevice9 *dev = GAME_DEVICE;
		void **vt = dev ? *(void ***)dev : nullptr;
		if (vt && isInsideD3D9(vt[17])) {                 // 17 = Present: a real d3d9 vtable
			IDirect3DSwapChain9 *sc = nullptr;
			if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
				D3DPRESENT_PARAMETERS pp = {0};
				if (SUCCEEDED(sc->GetPresentParameters(&pp)) && pp.hDeviceWindow)
					g_hwnd = pp.hDeviceWindow;
				sc->Release();
			}
			if (!g_hwnd) {
				D3DDEVICE_CREATION_PARAMETERS cp = {0};
				if (SUCCEEDED(dev->GetCreationParameters(&cp))) g_hwnd = cp.hFocusWindow;
			}
			logf("device watch: found device %p (hwnd %p) - hooking without CreateDevice", dev, g_hwnd);
			hookDevice(dev);
			installWndProc();
			installKeyboardHook();
			onWindowedEntry(true);
			return 0;
		}
		Sleep(50);
	}
	if (!g_deviceHooked)
		logf("device watch: no hookable d3d9 device found. If SokuDirectXOptimizations is enabled, set "
		     "use_d3d9ex=0 - its 9Ex path wraps the device and DisplayManager can't attach to it.");
	return 0;
}

// ---- IDirect3D9::CreateDevice hook ---------------------------------------------------------------
static HRESULT WINAPI myCreateDevice(IDirect3D9 *self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                     DWORD behavior, D3DPRESENT_PARAMETERS *pp,
                                     IDirect3DDevice9 **out) {
	logf("CreateDevice: Windowed=%d %ux%u", pp ? pp->Windowed : -1,
	     pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
	g_adapterMon = self->GetAdapterMonitor(adapter);
	applyFullscreenParams(pp);          // sets g_hwnd from pp->hDeviceWindow
	if (!g_hwnd && focus) g_hwnd = focus;
	installKeyboardHook();              // hooks the WINDOW's thread (not necessarily this one)
	HRESULT hr = oCreateDevice(self, adapter, type, focus, behavior, pp, out);
	if (SUCCEEDED(hr) && out && *out) {
		hookDevice(*out);
		if (g_active) {
			createCapture(*out);
			setGameViewport(*out);
		}
		installWndProc();               // subclass the window for drag-resize aspect locking
		if (g_wantFullscreen) {
			if (g_borderless) enterBorderlessFullscreen();
		} else {
			onWindowedEntry(true);      // first windowed spawn: apply saved scale + spawn position
		}
	}
	return hr;
}

// ---- Direct3DCreate9 hook (installs the CreateDevice hook once the D3D object exists) -------------
static IDirect3D9 *WINAPI myDirect3DCreate9(UINT sdkVersion) {
	IDirect3D9 *d3d = oDirect3DCreate9(sdkVersion);
	if (d3d && !g_createDeviceHooked) {
		void **vt = *(void ***)d3d;
		oCreateDevice = (CreateDevice_t)hookSlot(vt, VT_D3D9_CREATEDEVICE, (void *)myCreateDevice);
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

static void loadConfig() {
	GetModuleFileNameA(g_module, g_iniPath, 1024);
	PathRemoveFileSpecA(g_iniPath);
	PathAppendA(g_iniPath, "DisplayManager.ini");
	g_enabled = GetPrivateProfileIntA("Display", "Enabled", 1, g_iniPath) != 0;

	char mode[64] = {0};
	GetPrivateProfileStringA("Display", "Mode", "FitToScreen", mode, sizeof(mode), g_iniPath);
	if (StrCmpIA(mode, "IntegerScaling") == 0)        g_mode = MODE_INTEGER;
	else if (StrCmpIA(mode, "CustomResolution") == 0) g_mode = MODE_CUSTOM;
	else                                              g_mode = MODE_FIT;

	char scale[32] = {0};
	GetPrivateProfileStringA("Display", "IntegerScaling", "x2", scale, sizeof(scale), g_iniPath);
	g_intScale = parseScale(scale);

	g_customW = GetPrivateProfileIntA("Display", "CustomWidth", 1280, g_iniPath);
	g_customH = GetPrivateProfileIntA("Display", "CustomHeight", 960, g_iniPath);

	char color[32] = {0};
	GetPrivateProfileStringA("Display", "BackgroundColor", "000000", color, sizeof(color), g_iniPath);
	g_bgColor = parseColor(color, D3DCOLOR_XRGB(0, 0, 0));

	char filt[32] = {0};
	GetPrivateProfileStringA("Display", "Filter", "Auto", filt, sizeof(filt), g_iniPath);
	if      (StrCmpIA(filt, "Point") == 0)  g_filterCfg = 1;
	else if (StrCmpIA(filt, "Linear") == 0) g_filterCfg = 2;
	else if (StrCmpIA(filt, "Sharp") == 0)  g_filterCfg = 3;
	else                                    g_filterCfg = 0;

	char sharp[32] = {0};
	GetPrivateProfileStringA("Display", "Sharpness", "1.50", sharp, sizeof(sharp), g_iniPath);
	g_sharpness = (float)atof(sharp);
	if (g_sharpness < SHARP_MIN) g_sharpness = SHARP_MIN;
	if (g_sharpness > SHARP_MAX) g_sharpness = SHARP_MAX;

	g_resizable = GetPrivateProfileIntA("Display", "Resizable", 1, g_iniPath) != 0;
	g_persist   = GetPrivateProfileIntA("Display", "PersistState", 1, g_iniPath) != 0;
	g_posX      = GetPrivateProfileIntA("Display", "PositionX", -1, g_iniPath);
	g_posY      = GetPrivateProfileIntA("Display", "PositionY", -1, g_iniPath);
	g_borderless   = GetPrivateProfileIntA("Display", "Borderless", 0, g_iniPath) != 0;
	g_fsW          = GetPrivateProfileIntA("Display", "FullscreenWidth", 0, g_iniPath);
	g_fsH          = GetPrivateProfileIntA("Display", "FullscreenHeight", 0, g_iniPath);
	g_fsRefresh    = GetPrivateProfileIntA("Display", "FullscreenRefresh", 0, g_iniPath);

	// g_srcW/g_srcH are fixed at 640x480: th123 always renders its scene at that size, so the grab
	// region, the pinned viewport, and the post-upscale re-clear must all be exactly 640x480 - there is
	// no useful reason to make it configurable (a wrong value can only clip the game or grab garbage).
	g_log     = GetPrivateProfileIntA("Display", "Log", 0, g_iniPath) != 0;

	// [Hotkeys] - the modifier plus a per-action key (single letter/digit). A missing/commented/blank
	// line disables that hotkey (default is empty, so commenting a line out turns it off).
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
		GetPrivateProfileStringA("Hotkeys", names[a], "", k, sizeof(k), g_iniPath);  // "" = disabled
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

// Persist the current scaling settings (Mode + IntegerScaling + Filter + Sharpness) to the ini so the
// next launch restores them - including live Alt+F / Alt+K / Alt+L tuning. Window position is deliberately
// NOT saved. Only keys that actually changed are written (see writeIniIfChanged), so if the user changed
// nothing the file is left untouched.
static void persistState() {
	if (!g_persist) return;
	const char *m = g_mode == MODE_INTEGER ? "IntegerScaling"
	              : g_mode == MODE_CUSTOM  ? "CustomResolution" : "FitToScreen";
	char scale[16];
	wsprintfA(scale, "x%d", g_intScale);
	writeIniIfChanged("Mode", m);
	writeIniIfChanged("IntegerScaling", scale);

	const char *f = g_filterCfg == 1 ? "Point" : g_filterCfg == 2 ? "Linear"
	              : g_filterCfg == 3 ? "Sharp" : "Auto";
	writeIniIfChanged("Filter", f);
	// wsprintf has no %f; format the sharpness manually (2 decimals).
	int hundredths = (int)(g_sharpness * 100.0f + 0.5f);
	char sh[32];
	wsprintfA(sh, "%d.%02d", hundredths / 100, hundredths % 100);
	writeIniIfChanged("Sharpness", sh);
}

// ---- hotkeys (WindowResizer-style): Alt+0 = FitToScreen, Alt+1..4 = IntegerScaling x1..x4 ---------
// A WH_KEYBOARD hook on the game's UI thread (the same technique WindowResizer uses for its Alt+number
// hotkeys). This fires for the game's own key messages, so it works in exclusive fullscreen - unlike a
// GetAsyncKeyState poll, which the exclusive-fullscreen input path doesn't cooperate with. The change is
// applied live: it only affects the post-process output size, so no device reset is needed.
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

// Resize the game's window so its client area is exactly (srcW*n) x (srcH*n). We only move the window's
// borders - the game keeps rendering to its existing backbuffer and D3D9's windowed present stretches it
// to the new client, so no (unsafe, external) device reset is needed. Mirrors WindowResizer. Moves to
// the configured spawn position only when applyPos is set (and a position is configured).
static void setWindowScaled(int n, bool applyPos) {
	if (!g_hwnd || n < 1) return;
	int bx, by; windowBorders(&bx, &by);
	UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
	int x = 0, y = 0;
	if (applyPos && g_posX >= 0 && g_posY >= 0) { x = g_posX; y = g_posY; }
	else flags |= SWP_NOMOVE;
	SetWindowPos(g_hwnd, nullptr, x, y, g_srcW * n + bx, g_srcH * n + by, flags);
	logf("window -> client %dx%d (x%d)%s", g_srcW * n, g_srcH * n, n,
	     (flags & SWP_NOMOVE) ? "" : " +spawn-pos");
}

static void applyTopmost() {
	if (g_hwnd)
		SetWindowPos(g_hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
		             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Apply the remembered window scale on entering (real) windowed mode; on the first spawn also honor the
// configured spawn position. If we were in borderless fullscreen, restore the normal window frame first.
static void onWindowedEntry(bool firstTime) {
	if (g_styleSaved && g_hwnd) {
		SetWindowLongA(g_hwnd, GWL_STYLE, g_savedStyle);
		SetWindowLongA(g_hwnd, GWL_EXSTYLE, g_savedExStyle);
		SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
		             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
		g_styleSaved = false;
	}
	g_borderlessActive = false;
	writeFsFlag(false);
	setWindowScaled(g_intScale, firstTime);
	applyTopmost();
}

// Subclassed window procedure: while windowed and resizable, lock a drag-resize to the source aspect
// ratio (and a minimum of one source-size) so the stretched image never gets squashed.
static WNDPROC g_origWndProc = nullptr;

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
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

static void installWndProc() {
	if (g_origWndProc || !g_hwnd) return;
	if (g_resizable) {
		LONG style = GetWindowLongA(g_hwnd, GWL_STYLE);
		SetWindowLongA(g_hwnd, GWL_STYLE, style | WS_THICKFRAME);   // add a drag-resize border
		SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
		             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
	}
	g_origWndProc = (WNDPROC)SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)wndProc);
	logf("wndproc subclassed (resizable=%d)", g_resizable);
}

// Turn the game's window into a borderless popup covering its monitor (used in borderless-fullscreen
// mode).
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

	MONITORINFO mi = { sizeof(mi) };
	GetMonitorInfo(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
	int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
	SetWindowPos(g_hwnd, g_topmost ? HWND_TOPMOST : HWND_TOP,
	             mi.rcMonitor.left, mi.rcMonitor.top, mw, mh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
	g_borderlessActive = true;
	writeFsFlag(true);
	logf("borderless window %dx%d at (%d,%d) topmost=%d",
	     mw, mh, mi.rcMonitor.left, mi.rcMonitor.top, g_topmost);
}

// Run a hotkey action. "Scale N" means "N x": it sets the scaling choice (so it carries between modes),
// then applies it to whichever mode is active - the fullscreen output, or the window size.
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
		g_mode = MODE_INTEGER; g_intScale = n;
		if (g_active) computeOutput();          // fullscreen: re-scale the centered output live
		else          setWindowScaled(n, false); // windowed: resize the window to N x
		char msg[8]; wsprintfA(msg, "X%d", n); showOsd(msg);
		logf("hotkey: x%d", n);
		break;
	}
	case ACT_TOP:
		g_topmost = !g_topmost;
		applyTopmost();
		logf("hotkey: always-on-top=%d", g_topmost);
		break;
	case ACT_FILTER: {
		g_filterCfg = (g_filterCfg + 1) % 4;   // Auto -> Point -> Linear -> Sharp -> Auto
		if (g_active) computeOutput();         // re-resolve g_filter now; next frame's present uses it
		const char *n = g_filterCfg == 1 ? "Point" : g_filterCfg == 2 ? "Linear"
		              : g_filterCfg == 3 ? "Sharp" : "Auto";
		if (g_filterCfg == 3) { char msg[32]; sharpOsdText(msg, sizeof(msg)); showOsd(msg); }  // Sharp: show value
		else showOsd(g_filterCfg == 1 ? "POINT" : g_filterCfg == 2 ? "LINEAR" : "AUTO");
		logf("hotkey: filter -> %s", n);
		break;
	}
	case ACT_SHARP_DOWN:
	case ACT_SHARP_UP: {
		g_filterCfg = 3;                        // sharpness only affects Sharp, so switch to it
		if (g_active) computeOutput();
		g_sharpness += (act == ACT_SHARP_UP) ? 0.25f : -0.25f;   // live; the shader reads it each frame
		if (g_sharpness < SHARP_MIN) g_sharpness = SHARP_MIN;
		if (g_sharpness > SHARP_MAX) g_sharpness = SHARP_MAX;
		char msg[32]; sharpOsdText(msg, sizeof(msg)); showOsd(msg);
		logf("hotkey: filter=Sharp sharpness -> %.2f", g_sharpness);
		break;
	}
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

// Install the keyboard hook on the GAME WINDOW's thread. We must target that thread explicitly (rather
// than the current one): SokuDirectXOptimizations moves rendering/present onto a separate thread, so the
// thread that calls CreateDevice/Present is not the window's message thread that receives key input.
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

	// Borderless mode presents a windowed device, so stop the game deriving its fullscreen flag
	// (0x8998B0) from present.Windowed - we own that flag instead (writeFsFlag), which keeps its
	// Alt+Enter toggle flipping the right way. `mov [0x8998B0], al` at 0x004405BC is 5 bytes.
	if (g_borderless)
		patchNop(0x004405BC, 5);
}

extern "C" __declspec(dllexport) bool CheckVersion(const BYTE hash[16]) {
	return ::memcmp(TARGET_HASH, hash, sizeof TARGET_HASH) == 0;
}

extern "C" __declspec(dllexport) bool Initialize(HMODULE hMyModule, HMODULE hParentModule) {
	g_module = hMyModule;
	loadConfig();
	if (g_enabled) {
		setupHooks();       // the keyboard hook + wndproc are installed later, from CreateDevice (UI thread)
		atexit(persistState);
		// Fallback for when our Direct3DCreate9 hook never fires (e.g. SokuDirectXOptimizations with
		// use_d3d9ex=1 creates a Direct3D9Ex device): watch for the device global and hook it directly.
		CloseHandle(CreateThread(nullptr, 0, deviceWatchThread, nullptr, 0, nullptr));
	}
	const char *modeName = g_mode == MODE_INTEGER ? "IntegerScaling"
	                     : g_mode == MODE_CUSTOM  ? "CustomResolution" : "FitToScreen";
	logf("DisplayManager initialized: enabled=%d mode=%s intScale=x%d custom=%dx%d src=%dx%d "
	     "resizable=%d persist=%d pos=(%d,%d) borderless=%d",
	     g_enabled, modeName, g_intScale, g_customW, g_customH, g_srcW, g_srcH,
	     g_resizable, g_persist, g_posX, g_posY, g_borderless);
	return TRUE;
}

extern "C" int APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved) {
	return TRUE;
}
