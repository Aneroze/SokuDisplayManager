// DMProbe - passive, read-only diagnostic for Touhou Hisoutensoku (th123 1.10a).
//
// Purpose: determine what resolution the game actually RENDERS at under a given setup (e.g. WindowResizer)
// - specifically whether the backbuffer / render viewport is 640x480 (present-time stretch) or larger
// (draw-time magnification). It hooks NOTHING and modifies nothing, so it can run safely alongside
// WindowResizer (or anything else). It just reads the game's device + globals and logs them.
//
// It logs, once per second (deduped), to DMProbe.log next to this DLL:
//   - present-params global 0x8A0F68: BackBufferWidth/Height, Windowed
//   - the real backbuffer surface desc (device->GetSwapChain->GetBackBuffer->GetDesc)
//   - the current render viewport (device->GetViewport)
//   - the game window client rect

#include <windows.h>
#include <Shlwapi.h>
#include <d3d9.h>
#include <cstdio>

#define SWRS_USES_HASH_LOCAL
static const BYTE TARGET_HASH[16] = {
	0xdf, 0x35, 0xd1, 0xfb, 0xc7, 0xb5, 0x83, 0x31,
	0x7a, 0xda, 0xbe, 0x8c, 0xd9, 0xf5, 0x3b, 0x2e,
};
#define GAME_DEVICE (*reinterpret_cast<IDirect3DDevice9 **>(0x008A0E30))
static const DWORD ADDR_PRESENT_PARAMS = 0x008A0F68;  // D3DPRESENT_PARAMETERS (W+0, H+4, hWnd+0x1C, Windowed+0x20)

static HMODULE g_module;
static char    g_logPath[1024 + MAX_PATH];

static void logline(const char *s) {
	FILE *f = fopen(g_logPath, "a");
	if (!f) return;
	fputs(s, f);
	fputc('\n', f);
	fclose(f);
}

static DWORD WINAPI probeThread(LPVOID) {
	char last[512] = {0};
	for (int i = 0; i < 600; i++) {                 // ~10 min
		IDirect3DDevice9 *dev = GAME_DEVICE;
		if (dev) {
			const DWORD *pp = reinterpret_cast<const DWORD *>(ADDR_PRESENT_PARAMS);
			UINT ppW = pp[0], ppH = pp[1], ppWindowed = pp[8]; // +0, +4, +0x20
			HWND hwnd = *reinterpret_cast<HWND *>(ADDR_PRESENT_PARAMS + 0x1C);

			UINT bbW = 0, bbH = 0; int bbFmt = -1;
			IDirect3DSurface9 *bb = nullptr;
			if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
				D3DSURFACE_DESC d; if (SUCCEEDED(bb->GetDesc(&d))) { bbW = d.Width; bbH = d.Height; bbFmt = d.Format; }
				bb->Release();
			}
			D3DVIEWPORT9 vp; ZeroMemory(&vp, sizeof(vp));
			dev->GetViewport(&vp);

			RECT cr = {0,0,0,0};
			if (hwnd) GetClientRect(hwnd, &cr);

			char line[512];
			wsprintfA(line,
			          "ppGlobal=%ux%u windowed=%u | backbuffer=%ux%u fmt=%d | viewport=%u,%u %ux%u | client=%ldx%ld",
			          ppW, ppH, ppWindowed, bbW, bbH, bbFmt,
			          vp.X, vp.Y, vp.Width, vp.Height, cr.right - cr.left, cr.bottom - cr.top);
			if (lstrcmpA(line, last) != 0) {          // only log on change
				lstrcpynA(last, line, sizeof(last));
				logline(line);
			}
		}
		Sleep(1000);
	}
	return 0;
}

extern "C" __declspec(dllexport) bool CheckVersion(const BYTE hash[16]) {
	return ::memcmp(TARGET_HASH, hash, sizeof TARGET_HASH) == 0;
}

extern "C" __declspec(dllexport) bool Initialize(HMODULE hMyModule, HMODULE hParentModule) {
	g_module = hMyModule;
	GetModuleFileNameA(g_module, g_logPath, 1024);
	PathRemoveFileSpecA(g_logPath);
	PathAppendA(g_logPath, "DMProbe.log");
	logline("--- DMProbe start (read-only; logs backbuffer/viewport/client each second on change) ---");
	CloseHandle(CreateThread(nullptr, 0, probeThread, nullptr, 0, nullptr));
	return TRUE;
}

extern "C" int APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved) {
	return TRUE;
}
