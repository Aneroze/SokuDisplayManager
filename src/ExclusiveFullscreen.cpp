// ExclusiveFullscreen - crisp, integer-scaled exclusive fullscreen for Touhou Hisoutensoku (th123 1.10a)
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
static HWND g_hwnd = nullptr;   // the game's window (from present params), for windowed resizing

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

// Decide how to shape the present parameters for this (Create)Device/Reset call. Fullscreen requests
// (Windowed == FALSE) are forced to the native desktop mode so the monitor is never rescaled; windowed
// requests are restored to the game's canonical windowed size (undoing the huge size we write into the
// game's shared present-params struct while fullscreen, which otherwise leaks into the windowed path).
static void applyFullscreenParams(D3DPRESENT_PARAMETERS *pp) {
	if (!g_enabled || !pp) { g_active = false; return; }
	if (pp->hDeviceWindow) g_hwnd = pp->hDeviceWindow;   // remember the game window for windowed resizing

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
	pp->BackBufferWidth  = w;
	pp->BackBufferHeight = h;
	pp->FullScreen_RefreshRateInHz = dm->RefreshRate;
	g_bbW = w; g_bbH = h;
	g_bbFormat = (pp->BackBufferFormat != D3DFMT_UNKNOWN) ? pp->BackBufferFormat
	                                                      : (D3DFORMAT)dm->Format;
	g_active = true;
	logf("fullscreen -> native %ux%u @%uHz fmt=%d", w, h, dm->RefreshRate, (int)g_bbFormat);
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
			HRESULT b = dev->ColorFill(bb, nullptr, D3DCOLOR_XRGB(0, 0, 0));             // black borders
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
	installKeyboardHook();   // we're on the game's UI thread here - the right thread to hook
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
// Parse an integer scale written as "x2", "X3", or plain "2".
static int parseScale(const char *s) {
	while (*s == ' ' || *s == '\t') s++;
	if (*s == 'x' || *s == 'X') s++;
	int n = atoi(s);
	return n < 1 ? 1 : n;
}

static void loadConfig() {
	GetModuleFileNameA(g_module, g_iniPath, 1024);
	PathRemoveFileSpecA(g_iniPath);
	PathAppendA(g_iniPath, "ExclusiveFullscreen.ini");
	g_enabled = GetPrivateProfileIntA("Fullscreen", "Enabled", 1, g_iniPath) != 0;

	char mode[64] = {0};
	GetPrivateProfileStringA("Fullscreen", "Mode", "FitToScreen", mode, sizeof(mode), g_iniPath);
	if (StrCmpIA(mode, "IntegerScaling") == 0)        g_mode = MODE_INTEGER;
	else if (StrCmpIA(mode, "CustomResolution") == 0) g_mode = MODE_CUSTOM;
	else                                              g_mode = MODE_FIT;

	char scale[32] = {0};
	GetPrivateProfileStringA("Fullscreen", "IntegerScaling", "x2", scale, sizeof(scale), g_iniPath);
	g_intScale = parseScale(scale);

	g_customW = GetPrivateProfileIntA("Fullscreen", "CustomWidth", 1280, g_iniPath);
	g_customH = GetPrivateProfileIntA("Fullscreen", "CustomHeight", 960, g_iniPath);
	g_srcW    = GetPrivateProfileIntA("Fullscreen", "SourceWidth", 640, g_iniPath);
	g_srcH    = GetPrivateProfileIntA("Fullscreen", "SourceHeight", 480, g_iniPath);
	g_log     = GetPrivateProfileIntA("Fullscreen", "Log", 0, g_iniPath) != 0;
	if (g_srcW < 1) g_srcW = 640;
	if (g_srcH < 1) g_srcH = 480;
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

// Resize the game's window so its client area is exactly (srcW*n) x (srcH*n). We only move the window's
// borders - the game keeps rendering to its existing backbuffer and D3D9's windowed present stretches it
// to the new client, so no (unsafe, external) device reset is needed. Mirrors WindowResizer.
static void resizeWindowToScale(int n) {
	if (!g_hwnd || n < 1) return;
	RECT r = { 0, 0, g_srcW * n, g_srcH * n };
	LONG style = GetWindowLongA(g_hwnd, GWL_STYLE);
	LONG ex    = GetWindowLongA(g_hwnd, GWL_EXSTYLE);
	AdjustWindowRectEx(&r, style, GetMenu(g_hwnd) != nullptr, ex);
	SetWindowPos(g_hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
	             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	logf("windowed resize -> client %dx%d (x%d)", g_srcW * n, g_srcH * n, n);
}

// "Alt+N" means "N x" in both contexts: it sets the scaling choice (so it carries between modes), then
// applies it to whichever mode is active right now - the fullscreen output size, or the window size.
static void applyHotkey(int digit) {
	if (digit == 0) { g_mode = MODE_FIT;     logf("hotkey: Alt+0 -> FitToScreen"); }
	else            { g_mode = MODE_INTEGER; g_intScale = digit; logf("hotkey: Alt+%d -> x%d", digit, digit); }
	if (g_active)
		computeOutput();            // fullscreen: re-scale the centered output live
	else if (digit >= 1)
		resizeWindowToScale(digit); // windowed: resize the window to N x
}

static LRESULT CALLBACK keyboardHook(int code, WPARAM wParam, LPARAM lParam) {
	if (code == HC_ACTION && wParam >= '0' && wParam <= '4' &&
	    IS_FRESH_KEYDOWN(lParam) && (GetAsyncKeyState(VK_MENU) & 0x8000)) {
		applyHotkey((int)(wParam - '0'));
		return 1; // eat the key so it doesn't leak to the game / system menu
	}
	return CallNextHookEx(nullptr, code, wParam, lParam);
}

// Install the keyboard hook on whatever thread calls this. CreateDevice runs on the game's UI thread, so
// installing from there targets the right thread (as WindowResizer installs from CreateWindowExA).
static void installKeyboardHook() {
	if (g_kbHook) return;
	g_kbHook = SetWindowsHookExA(WH_KEYBOARD, keyboardHook, g_module, GetCurrentThreadId());
	logf("keyboard hook %s", g_kbHook ? "installed" : "FAILED");
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
		setupHooks();   // the keyboard hook is installed later, from CreateDevice (on the UI thread)
	const char *modeName = g_mode == MODE_INTEGER ? "IntegerScaling"
	                     : g_mode == MODE_CUSTOM  ? "CustomResolution" : "FitToScreen";
	logf("ExclusiveFullscreen initialized: enabled=%d mode=%s intScale=x%d custom=%dx%d src=%dx%d",
	     g_enabled, modeName, g_intScale, g_customW, g_customH, g_srcW, g_srcH);
	return TRUE;
}

extern "C" int APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved) {
	return TRUE;
}
