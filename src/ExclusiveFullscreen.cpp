// ExclusiveFullscreen - crisp, integer-scaled exclusive fullscreen for Touhou Hisoutensoku (th123 1.10a)
//
// Problem this solves
// -------------------
// With WindowResizer off, the base game's fullscreen switches to a resolution that fills the monitor
// height at 4:3 and pillarboxes the sides, so 480 logical pixels are scaled by a non-integer factor
// (e.g. 1080/480 = 2.25x) and everything looks blurry. This mod instead keeps the desktop at its
// native resolution and renders the game centered at an integer scale (1280x960 = 2x by default) with
// black borders all around. Because it is true exclusive fullscreen it also gets the low-latency
// direct-flip ("Independent Flip") present path - which a legacy Direct3D9 / DISCARD game like this one
// cannot get in a borderless window.
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
// This mod expects WindowResizer to be DISABLED (they both manage the window/fullscreen path).
//
// Self-contained: it only needs the Windows SDK (windows.h / d3d9.h / shlwapi.h). It does not import
// d3d9.lib - it hooks the game's Direct3DCreate9 through the import table and drives the device the game
// itself creates, so there are no external runtime dependencies.

#include <windows.h>
#include <Shlwapi.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdarg>

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
static HMODULE g_module;
static char    g_iniPath[1024 + MAX_PATH];
static bool    g_enabled   = true;
static int     g_maxScale  = 2;      // cap on the auto integer scale (2 = at most 2x, even if 3x fits)
static int     g_ovrW      = 0;      // exact output override from the ini (0 = use MaxScale auto-fit)
static int     g_ovrH      = 0;
static int     g_scaleW    = 1280;   // resolved output width  (set per-fullscreen from override or auto)
static int     g_scaleH    = 960;    // resolved output height
static int     g_srcW      = 640;    // the game's own render size (grabbed from the backbuffer top-left)
static int     g_srcH      = 480;
static bool    g_log       = false;
static FILE   *g_logFile   = nullptr;

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

// Decide how to shape the present parameters for this (Create)Device/Reset call. Fullscreen requests
// (Windowed == FALSE) are forced to the native desktop mode so the monitor is never rescaled; windowed
// requests are restored to the game's canonical windowed size (undoing the huge size we write into the
// game's shared present-params struct while fullscreen, which otherwise leaks into the windowed path).
static void applyFullscreenParams(D3DPRESENT_PARAMETERS *pp) {
	if (!g_enabled || !pp) { g_active = false; return; }

	if (pp->Windowed) {
		// Remember the game's own windowed size the first time we see it (before we ever meddle).
		if (!g_haveWin && pp->BackBufferWidth && pp->BackBufferHeight &&
		    pp->BackBufferWidth <= 4096 && pp->BackBufferHeight <= 4096) {
			g_winW = pp->BackBufferWidth; g_winH = pp->BackBufferHeight; g_haveWin = true;
		}
		if (g_haveWin) { pp->BackBufferWidth = g_winW; pp->BackBufferHeight = g_winH; }
		pp->FullScreen_RefreshRateInHz = 0;
		g_active = false;
		return;
	}

	const D3DDISPLAYMODE *dm = reinterpret_cast<const D3DDISPLAYMODE *>(ADDR_DESKTOP_MODE);
	UINT w = dm->Width, h = dm->Height;

	// Resolve the output size: exact override if given, else the largest integer multiple of the source
	// that fits the native desktop, capped at MaxScale (computed live, so it needs no configuration).
	if (g_ovrW > 0 && g_ovrH > 0) {
		g_scaleW = g_ovrW; g_scaleH = g_ovrH;
	} else {
		int fit = (int)min(w / (UINT)g_srcW, h / (UINT)g_srcH);  // largest scale that physically fits
		int n = fit < g_maxScale ? fit : g_maxScale;             // cap at MaxScale
		if (n < 1) n = 1;
		g_scaleW = g_srcW * n; g_scaleH = g_srcH * n;
	}

	if (w < (UINT)g_scaleW || h < (UINT)g_scaleH) {
		logf("desktop %ux%u too small for %dx%d output - passthrough", w, h, g_scaleW, g_scaleH);
		g_active = false;
		return;
	}
	pp->BackBufferWidth  = w;
	pp->BackBufferHeight = h;
	pp->FullScreen_RefreshRateInHz = dm->RefreshRate;
	g_bbW = w; g_bbH = h;
	g_bbFormat = (pp->BackBufferFormat != D3DFMT_UNKNOWN) ? pp->BackBufferFormat
	                                                      : (D3DFORMAT)dm->Format;
	g_active = true;
	logf("fullscreen -> native %ux%u @%uHz fmt=%d; grab %dx%d -> %dx%d centered at (%d,%d)",
	     w, h, dm->RefreshRate, (int)g_bbFormat, g_srcW, g_srcH, g_scaleW, g_scaleH,
	     (int)(w - g_scaleW) / 2, (int)(h - g_scaleH) / 2);
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
			HRESULT a = dev->StretchRect(bb, &srcRect, g_capture, nullptr, D3DTEXF_POINT);
			HRESULT b = dev->ColorFill(bb, nullptr, D3DCOLOR_XRGB(0, 0, 0));
			HRESULT c = dev->StretchRect(g_capture, nullptr, bb, &dstRect, D3DTEXF_POINT);
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
	if (SUCCEEDED(hr) && g_active)
		createCapture(dev);
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

// ---- IDirect3D9::CreateDevice hook ---------------------------------------------------------------
static HRESULT WINAPI myCreateDevice(IDirect3D9 *self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                     DWORD behavior, D3DPRESENT_PARAMETERS *pp,
                                     IDirect3DDevice9 **out) {
	logf("CreateDevice: Windowed=%d %ux%u", pp ? pp->Windowed : -1,
	     pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
	applyFullscreenParams(pp);
	HRESULT hr = oCreateDevice(self, adapter, type, focus, behavior, pp, out);
	if (SUCCEEDED(hr) && out && *out) {
		hookDevice(*out);
		if (g_active)
			createCapture(*out);
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
static void loadConfig() {
	GetModuleFileNameA(g_module, g_iniPath, 1024);
	PathRemoveFileSpecA(g_iniPath);
	PathAppendA(g_iniPath, "ExclusiveFullscreen.ini");
	g_enabled  = GetPrivateProfileIntA("Fullscreen", "Enabled", 1, g_iniPath) != 0;
	g_maxScale = GetPrivateProfileIntA("Fullscreen", "MaxScale", 2, g_iniPath);
	g_ovrW     = GetPrivateProfileIntA("Fullscreen", "WidthOverride", 0, g_iniPath);
	g_ovrH     = GetPrivateProfileIntA("Fullscreen", "HeightOverride", 0, g_iniPath);
	g_srcW     = GetPrivateProfileIntA("Fullscreen", "SourceWidth", 640, g_iniPath);
	g_srcH     = GetPrivateProfileIntA("Fullscreen", "SourceHeight", 480, g_iniPath);
	g_log      = GetPrivateProfileIntA("Fullscreen", "Log", 0, g_iniPath) != 0;
	if (g_maxScale < 1) g_maxScale = 1;
	if (g_srcW < 1) g_srcW = 640;
	if (g_srcH < 1) g_srcH = 480;
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

extern "C" __declspec(dllexport) bool CheckVersion(const BYTE hash[16]) {
	return ::memcmp(TARGET_HASH, hash, sizeof TARGET_HASH) == 0;
}

extern "C" __declspec(dllexport) bool Initialize(HMODULE hMyModule, HMODULE hParentModule) {
	g_module = hMyModule;
	loadConfig();
	if (g_enabled)
		setupHooks();
	if (g_ovrW > 0 && g_ovrH > 0)
		logf("ExclusiveFullscreen initialized: enabled=%d src=%dx%d out=%dx%d (override)",
		     g_enabled, g_srcW, g_srcH, g_ovrW, g_ovrH);
	else
		logf("ExclusiveFullscreen initialized: enabled=%d src=%dx%d out=auto (<= %dx)",
		     g_enabled, g_srcW, g_srcH, g_maxScale);
	return TRUE;
}

extern "C" int APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved) {
	return TRUE;
}
