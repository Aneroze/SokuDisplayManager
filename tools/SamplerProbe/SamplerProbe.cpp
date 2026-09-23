// SamplerProbe - passive diagnostic for Touhou Hisoutensoku (th123 1.10a).
//
// Purpose: find out what texture-filter sampler states the GAME sets when it renders,
// and whether they differ between a "started at 1280" run and a "started at 640" run.
// This is what determines whether the game's internal 640x480 render is smooth (LINEAR)
// or aliased (POINT) - the real cause of the WindowResizer "1280-start looks better" effect.
//
// It hooks IDirect3DDevice9::SetSamplerState on the shared d3d9.dll vtable and only
// OBSERVES: it records every MAG/MIN/MIP filter the game sets, then always forwards the
// call unchanged. It changes nothing about rendering. Safe to run alongside (upstream)
// WindowResizer, which sets no sampler state itself.
//
// Output (next to this DLL, SamplerProbe.log):
//   - a one-time context line (backbuffer/viewport/client) so you can tell which start it was
//   - the first ~200 filter-setting calls verbatim, in order (shows the init sequence)
//   - a per-second histogram (on change) of how many times each filter value was set,
//     per sampler, for MAG / MIN / MIP. Compare the 1280-start log vs the 640-start log.

#include <windows.h>
#include <Shlwapi.h>
#include <d3d9.h>
#include <cstdio>

static const BYTE TARGET_HASH[16] = {
	0xdf, 0x35, 0xd1, 0xfb, 0xc7, 0xb5, 0x83, 0x31,
	0x7a, 0xda, 0xbe, 0x8c, 0xd9, 0xf5, 0x3b, 0x2e,
};

#define GAME_DEVICE (*reinterpret_cast<IDirect3DDevice9 **>(0x008A0E30))
static const DWORD ADDR_PRESENT_PARAMS = 0x008A0F68;  // W+0, H+4, hWnd+0x1C, Windowed+0x20

// IDirect3DDevice9 vtable index of SetSamplerState: 3 IUnknown + methods in d3d9.h order.
// (QueryInterface/AddRef/Release=0..2, ... GetSamplerState=68, SetSamplerState=69).
static const int VT_SET_SAMPLER_STATE = 69;

typedef HRESULT (WINAPI *SetSamplerState_t)(IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE, DWORD);
static SetSamplerState_t g_orig = nullptr;

static HMODULE          g_module;
static char             g_logPath[1024 + MAX_PATH];
static CRITICAL_SECTION g_cs;

// counts[sampler 0..7][typeIndex 0=MAG,1=MIN,2=MIP][value 0..3 = NONE/POINT/LINEAR/ANISO]
static volatile LONG g_counts[8][3][4];
static volatile LONG g_dirty = 0;

// Full per-(sampler,type) last-value tracking so we can log EVERY distinct sampler state the
// game settles into (not just filters) - to diff a 1280-start vs a 640-start and find what
// actually differs. Types 1..13 (D3DSAMP_*). 0xFFFFFFFF = "never seen".
static const int  MAXTYPE = 16;
static DWORD g_last[8][MAXTYPE];
static char  g_changeBuf[512][80];   // pending change lines to flush from the log thread
static int   g_changeN = 0;

static void logline(const char *s) {
	FILE *f = fopen(g_logPath, "a");
	if (!f) return;
	fputs(s, f); fputc('\n', f);
	fclose(f);
}

static int typeIndex(D3DSAMPLERSTATETYPE t) {
	if (t == D3DSAMP_MAGFILTER) return 0;
	if (t == D3DSAMP_MINFILTER) return 1;
	if (t == D3DSAMP_MIPFILTER) return 2;
	return -1;
}
static const char *filterName(DWORD v) {
	switch (v) {
	case 0: return "NONE"; case 1: return "POINT"; case 2: return "LINEAR"; case 3: return "ANISO";
	default: return "?";
	}
}
static const char *sampTypeName(DWORD t) {
	switch (t) {
	case 1: return "ADDRESSU"; case 2: return "ADDRESSV"; case 3: return "ADDRESSW";
	case 4: return "BORDERCOLOR"; case 5: return "MAGFILTER"; case 6: return "MINFILTER";
	case 7: return "MIPFILTER"; case 8: return "MIPMAPLODBIAS"; case 9: return "MAXMIPLEVEL";
	case 10: return "MAXANISOTROPY"; case 11: return "SRGBTEXTURE"; case 12: return "ELEMENTINDEX";
	case 13: return "DMAPOFFSET"; default: return "TYPE?";
	}
}

static HRESULT WINAPI mySetSamplerState(IDirect3DDevice9 *dev, DWORD sampler,
                                        D3DSAMPLERSTATETYPE type, DWORD value) {
	if (sampler < 8) {
		int ti = typeIndex(type);
		if (ti >= 0) InterlockedIncrement(&g_counts[sampler][ti][value & 3]);

		if ((DWORD)type < MAXTYPE) {
			EnterCriticalSection(&g_cs);
			if (g_last[sampler][type] != value) {          // log only distinct new states
				g_last[sampler][type] = value;
				if (g_changeN < 512) {
					char extra[24] = "";
					if (type == 5 || type == 6 || type == 7)          // filters: name it
						wsprintfA(extra, " (%s)", filterName(value));
					else if (type == 8) {                              // LOD bias: it's a float
						float f; memcpy(&f, &value, 4);
						int whole = (int)(f * 100.0f);                 // wsprintf has no %f
						wsprintfA(extra, " (=%d.%02d)", whole / 100, (whole < 0 ? -whole : whole) % 100);
					}
					wsprintfA(g_changeBuf[g_changeN], "STATE s=%u %s=0x%X%s",
					          sampler, sampTypeName(type), value, extra);
					g_changeN++;
				}
			}
			LeaveCriticalSection(&g_cs);
		}
		InterlockedExchange(&g_dirty, 1);
	}
	return g_orig(dev, sampler, type, value);
}

// Returns the base filename of the module that owns pointer p (e.g. "d3d9.dll"
// or a mod DLL that has wrapped the device vtable), or false if p is not inside
// any loaded module (which would indicate a wrong vtable index / garbage slot).
static bool moduleNameOf(void *p, char *out, DWORD n) {
	HMODULE h = nullptr;
	if (p && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                            (LPCSTR)p, &h) && h) {
		char path[MAX_PATH]; GetModuleFileNameA(h, path, MAX_PATH);
		const char *b = strrchr(path, '\\');
		lstrcpynA(out, b ? b + 1 : path, n);
		return true;
	}
	return false;
}

static bool installHook(IDirect3DDevice9 *dev) {
	void **vtable = *(void ***)dev;
	void *target = vtable[VT_SET_SAMPLER_STATE];
	char owner[MAX_PATH];
	if (!moduleNameOf(target, owner, sizeof owner)) {
		logline("ERROR: SetSamplerState slot points into no loaded module - wrong index, not hooking.");
		return false;
	}
	{ char line[MAX_PATH + 64]; wsprintfA(line, "SetSamplerState slot owned by: %s (hooking + forwarding)", owner); logline(line); }
	DWORD op;
	VirtualProtect(&vtable[VT_SET_SAMPLER_STATE], sizeof(void *), PAGE_READWRITE, &op);
	g_orig = (SetSamplerState_t)target;
	vtable[VT_SET_SAMPLER_STATE] = (void *)mySetSamplerState;   // single aligned pointer store
	VirtualProtect(&vtable[VT_SET_SAMPLER_STATE], sizeof(void *), op, &op);
	return true;
}

static void logContext(IDirect3DDevice9 *dev) {
	const DWORD *pp = (const DWORD *)ADDR_PRESENT_PARAMS;
	UINT ppW = pp[0], ppH = pp[1], ppWindowed = pp[8];
	HWND hwnd = *(HWND *)(ADDR_PRESENT_PARAMS + 0x1C);
	UINT bbW = 0, bbH = 0;
	IDirect3DSurface9 *bb = nullptr;
	if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
		D3DSURFACE_DESC d; if (SUCCEEDED(bb->GetDesc(&d))) { bbW = d.Width; bbH = d.Height; }
		bb->Release();
	}
	D3DVIEWPORT9 vp; ZeroMemory(&vp, sizeof(vp)); dev->GetViewport(&vp);
	RECT cr = {0,0,0,0}; if (hwnd) GetClientRect(hwnd, &cr);
	char line[512];
	wsprintfA(line, "CONTEXT ppGlobal=%ux%u windowed=%u | backbuffer=%ux%u | viewport=%ux%u | client=%ldx%ld",
	          ppW, ppH, ppWindowed, bbW, bbH, vp.Width, vp.Height, cr.right - cr.left, cr.bottom - cr.top);
	logline(line);
}

static DWORD WINAPI probeThread(LPVOID) {
	IDirect3DDevice9 *dev = nullptr;
	for (int i = 0; i < 600 && !dev; i++) { dev = GAME_DEVICE; if (!dev) Sleep(100); }
	if (!dev) { logline("device never appeared within 60s"); return 0; }

	logContext(dev);
	if (!installHook(dev)) return 0;
	logline("hook installed on SetSamplerState (passive, forwards all calls)");

	int lastChange = 0;
	for (int i = 0; i < 600; i++) {          // ~10 min
		Sleep(1000);

		EnterCriticalSection(&g_cs);
		int cn = g_changeN;
		LeaveCriticalSection(&g_cs);
		while (lastChange < cn && lastChange < 512) { logline(g_changeBuf[lastChange]); lastChange++; }

		if (InterlockedExchange(&g_dirty, 0)) {
			logline("-- filter histogram (counts of each value set; N=None P=Point L=Linear A=Aniso) --");
			for (int s = 0; s < 8; s++) {
				LONG tot = 0;
				for (int t = 0; t < 3; t++) for (int v = 0; v < 4; v++) tot += g_counts[s][t][v];
				if (!tot) continue;
				char line[256];
				wsprintfA(line,
				  "  s=%d MAG{N%ld P%ld L%ld A%ld} MIN{N%ld P%ld L%ld A%ld} MIP{N%ld P%ld L%ld A%ld}",
				  s,
				  g_counts[s][0][0], g_counts[s][0][1], g_counts[s][0][2], g_counts[s][0][3],
				  g_counts[s][1][0], g_counts[s][1][1], g_counts[s][1][2], g_counts[s][1][3],
				  g_counts[s][2][0], g_counts[s][2][1], g_counts[s][2][2], g_counts[s][2][3]);
				logline(line);
			}
		}
	}
	return 0;
}

extern "C" __declspec(dllexport) bool CheckVersion(const BYTE hash[16]) {
	return ::memcmp(TARGET_HASH, hash, sizeof TARGET_HASH) == 0;
}

extern "C" __declspec(dllexport) bool Initialize(HMODULE hMyModule, HMODULE hParentModule) {
	g_module = hMyModule;
	InitializeCriticalSection(&g_cs);
	memset(g_last, 0xFF, sizeof g_last);   // 0xFFFFFFFF = never-seen sentinel
	GetModuleFileNameA(g_module, g_logPath, 1024);
	PathRemoveFileSpecA(g_logPath);
	PathAppendA(g_logPath, "SamplerProbe.log");
	logline("--- SamplerProbe start (read-only; observes the game's texture-filter sampler states) ---");
	CloseHandle(CreateThread(nullptr, 0, probeThread, nullptr, 0, nullptr));
	return TRUE;
}

extern "C" int APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved) {
	return TRUE;
}
