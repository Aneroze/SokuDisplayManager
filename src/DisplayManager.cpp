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
//   3. Hook the swapchain's Present: the game has drawn its SourceWidth x SourceHeight frame into the
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
static int     g_srcW      = 640;      // the game's own render size (grabbed from the backbuffer top-left)
static int     g_srcH      = 480;
static DWORD   g_filter    = D3DTEXF_POINT;  // upscale filter (point for integer, linear otherwise)
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
enum Action { ACT_FIT = 0, ACT_S1, ACT_S2, ACT_S3, ACT_S4, ACT_S5, ACT_S6, ACT_TOP, ACT_COUNT };
enum ModKey { MODK_ALT = 0, MODK_CTRL, MODK_SHIFT, MODK_WIN, MODK_NONE }; // MOD_* are taken by winuser.h
static int g_hotkeyVk[ACT_COUNT];      // filled by loadConfig
static int g_modifier = MODK_ALT;      // the modifier held with each hotkey key

// ---- runtime state -------------------------------------------------------------------------------
static bool      g_createDeviceHooked = false;
static bool      g_deviceHooked       = false;
static bool      g_active             = false;  // currently forcing exclusive fullscreen?
static UINT      g_bbW = 0, g_bbH = 0;          // forced backbuffer size (= native desktop)
static D3DFORMAT g_bbFormat = D3DFMT_X8R8G8B8;  // backbuffer format (for the capture RT)
static IDirect3DSurface9 *g_capture = nullptr;  // offscreen RT holding the grabbed game frame
// The game's canonical windowed backbuffer size, captured from the first (windowed) CreateDevice, so a
// return to windowed restores it instead of inheriting the huge fullscreen size we wrote into the
// game's shared present-params struct.
static bool g_haveWin = false;
static UINT g_winW = 640, g_winH = 480;
static HWND g_hwnd = nullptr;   // the game's window (from present params), for windowed resizing
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
	// Point-sample only when it's an exact integer multiple (crisp); otherwise linear avoids the uneven
	// doubled/tripled pixels of non-integer point scaling.
	g_filter = (outW % g_srcW == 0 && outH % g_srcH == 0) ? D3DTEXF_POINT : D3DTEXF_LINEAR;
	logf("output -> %dx%d centered at (%d,%d), filter=%s", g_scaleW, g_scaleH,
	     ((int)g_bbW - g_scaleW) / 2, ((int)g_bbH - g_scaleH) / 2,
	     g_filter == D3DTEXF_POINT ? "point" : "linear");
}

// The native resolution/refresh of the monitor the game window is on. We query this live (rather than
// trusting the game's cached GetAdapterDisplayMode global at 0x8A0FA0, which can be stale or the wrong
// monitor) so exclusive fullscreen always uses the true current mode - otherwise the desktop gets
// switched to a wrong (often small) resolution, which is blurry and shuffles the user's windows.
static void nativeMode(UINT *w, UINT *h, UINT *refresh) {
	*w = *h = *refresh = 0;
	if (g_hwnd) {
		MONITORINFOEXA mi; mi.cbSize = sizeof(mi);
		if (GetMonitorInfoA(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
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
	if (g_capture) { g_capture->Release(); g_capture = nullptr; }
}

static void createCapture(IDirect3DDevice9 *dev) {
	releaseCapture();
	HRESULT hr = dev->CreateRenderTarget((UINT)g_srcW, (UINT)g_srcH, g_bbFormat,
	                                     D3DMULTISAMPLE_NONE, 0, FALSE, &g_capture, nullptr);
	logf("createCapture %dx%d fmt=%d -> hr=0x%08lx", g_srcW, g_srcH, (int)g_bbFormat, (long)hr);
}

// ---- device / swapchain method hooks -------------------------------------------------------------
static bool g_presentLogged = false;

static HRESULT WINAPI mySCPresent(IDirect3DSwapChain9 *sc, const RECT *src, const RECT *dst,
                                  HWND wnd, const RGNDATA *dirty, DWORD flags) {
	// Post-process: the game has rendered its g_srcW x g_srcH surface into the top-left of a native-
	// resolution backbuffer. Grab that region, wipe the whole backbuffer black, then blit it back
	// scaled to an integer-multiple size, centered - point-filtered so it stays crisp.
	if (g_active && g_capture) {
		IDirect3DDevice9 *dev = GAME_DEVICE;
		IDirect3DSurface9 *bb = nullptr;
		if (dev && SUCCEEDED(sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
			RECT srcRect = { 0, 0, g_srcW, g_srcH };
			LONG x = ((LONG)g_bbW - g_scaleW) / 2, y = ((LONG)g_bbH - g_scaleH) / 2;
			RECT dstRect = { x, y, x + g_scaleW, y + g_scaleH };
			HRESULT a = dev->StretchRect(bb, &srcRect, g_capture, nullptr, D3DTEXF_NONE); // 1:1 copy out
			HRESULT b = dev->ColorFill(bb, nullptr, g_bgColor);                         // border color
			HRESULT c = dev->StretchRect(g_capture, nullptr, bb, &dstRect, (D3DTEXTUREFILTERTYPE)g_filter);
			if (!g_presentLogged) {
				logf("first present post-process: grab=0x%08lx fill=0x%08lx blit=0x%08lx",
				     (long)a, (long)b, (long)c);
				g_presentLogged = true;
			}
			bb->Release();
		}
	}
	return oSCPresent(sc, src, dst, wnd, dirty, flags);
}

static HRESULT WINAPI myReset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp) {
	logf("Reset: Windowed=%d %ux%u", pp ? pp->Windowed : -1,
	     pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
	releaseCapture();               // default-pool resources must be freed before Reset
	applyFullscreenParams(pp);
	HRESULT hr = oReset(dev, pp);
	if (SUCCEEDED(hr)) {
		if (g_active)
			createCapture(dev);
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
	applyFullscreenParams(pp);          // sets g_hwnd from pp->hDeviceWindow
	if (!g_hwnd && focus) g_hwnd = focus;
	installKeyboardHook();              // hooks the WINDOW's thread (not necessarily this one)
	HRESULT hr = oCreateDevice(self, adapter, type, focus, behavior, pp, out);
	if (SUCCEEDED(hr) && out && *out) {
		hookDevice(*out);
		if (g_active)
			createCapture(*out);
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

	g_resizable = GetPrivateProfileIntA("Display", "Resizable", 1, g_iniPath) != 0;
	g_persist   = GetPrivateProfileIntA("Display", "PersistState", 1, g_iniPath) != 0;
	g_posX      = GetPrivateProfileIntA("Display", "PositionX", -1, g_iniPath);
	g_posY      = GetPrivateProfileIntA("Display", "PositionY", -1, g_iniPath);
	g_borderless   = GetPrivateProfileIntA("Display", "Borderless", 0, g_iniPath) != 0;
	g_fsW          = GetPrivateProfileIntA("Display", "FullscreenWidth", 0, g_iniPath);
	g_fsH          = GetPrivateProfileIntA("Display", "FullscreenHeight", 0, g_iniPath);
	g_fsRefresh    = GetPrivateProfileIntA("Display", "FullscreenRefresh", 0, g_iniPath);

	g_srcW    = GetPrivateProfileIntA("Display", "SourceWidth", 640, g_iniPath);
	g_srcH    = GetPrivateProfileIntA("Display", "SourceHeight", 480, g_iniPath);
	g_log     = GetPrivateProfileIntA("Display", "Log", 0, g_iniPath) != 0;
	if (g_srcW < 1) g_srcW = 640;
	if (g_srcH < 1) g_srcH = 480;

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
	                                 "Scale4", "Scale5", "Scale6", "AlwaysOnTop" };
	for (int a = 0; a < ACT_COUNT; a++) {
		char k[16] = {0};
		GetPrivateProfileStringA("Hotkeys", names[a], "", k, sizeof(k), g_iniPath);  // "" = disabled
		g_hotkeyVk[a] = parseKey(k);
	}
}

// Persist the current scaling settings (Mode + IntegerScaling) to the ini so the next launch restores
// them. Window position is deliberately NOT saved. WritePrivateProfileString edits in place, keeping
// the other keys and comments.
static void persistState() {
	if (!g_persist) return;
	const char *m = g_mode == MODE_INTEGER ? "IntegerScaling"
	              : g_mode == MODE_CUSTOM  ? "CustomResolution" : "FitToScreen";
	char scale[16];
	wsprintfA(scale, "x%d", g_intScale);
	WritePrivateProfileStringA("Display", "Mode", m, g_iniPath);
	WritePrivateProfileStringA("Display", "IntegerScaling", scale, g_iniPath);
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
		logf("hotkey: FitToScreen");
		break;
	case ACT_S1: case ACT_S2: case ACT_S3: case ACT_S4: case ACT_S5: case ACT_S6: {
		int n = act - ACT_S1 + 1;
		g_mode = MODE_INTEGER; g_intScale = n;
		if (g_active) computeOutput();          // fullscreen: re-scale the centered output live
		else          setWindowScaled(n, false); // windowed: resize the window to N x
		logf("hotkey: x%d", n);
		break;
	}
	case ACT_TOP:
		g_topmost = !g_topmost;
		applyTopmost();
		logf("hotkey: always-on-top=%d", g_topmost);
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
