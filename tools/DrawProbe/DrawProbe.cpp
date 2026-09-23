// DrawProbe - passive diagnostic for Touhou Hisoutensoku (th123 1.10a).
//
// Sampler states proved identical between a 1280-start and a 640-start, yet the two 640 renders
// differ (1280-start smooth, 640-start aliased). So the difference is in DRAW-TIME rendering. This
// probe captures the two remaining candidates, read-only (hooks forward every call unchanged):
//
//   - SetTexture (vtable 65): for each DISTINCT texture bound, log its width x height, mip LEVEL
//     COUNT and format. Answers "do the character portraits have mipmaps?" (needed for a mip-LOD
//     explanation) and reveals the source texture resolution.
//   - SetTransform (vtable 44): log each DISTINCT (state, matrix). If the game uses screen-space
//     (XYZRHW) verts these stay identity and the difference is in vertex coords; if it uses real
//     projection/world matrices, a per-startup scale difference would show here.
//
// Compare the DrawProbe.log from a 1280-start vs a 640-start.

#include <windows.h>
#include <Shlwapi.h>
#include <d3d9.h>
#include <cstdio>

static const BYTE TARGET_HASH[16] = {
	0xdf, 0x35, 0xd1, 0xfb, 0xc7, 0xb5, 0x83, 0x31,
	0x7a, 0xda, 0xbe, 0x8c, 0xd9, 0xf5, 0x3b, 0x2e,
};
#define GAME_DEVICE (*reinterpret_cast<IDirect3DDevice9 **>(0x008A0E30))
static const DWORD ADDR_PRESENT_PARAMS = 0x008A0F68;

static const int VT_SET_TRANSFORM = 44;   // IDirect3DDevice9::SetTransform
static const int VT_SET_TEXTURE   = 65;   // IDirect3DDevice9::SetTexture

typedef HRESULT (WINAPI *SetTransform_t)(IDirect3DDevice9 *, D3DTRANSFORMSTATETYPE, const D3DMATRIX *);
typedef HRESULT (WINAPI *SetTexture_t)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
static SetTransform_t g_origSetTransform = nullptr;
static SetTexture_t   g_origSetTexture   = nullptr;

static HMODULE          g_module;
static char             g_logPath[1024 + MAX_PATH];
static CRITICAL_SECTION g_cs;
static volatile LONG    g_dirty = 0;

static void *g_seenTex[512]; static int g_seenTexN = 0;              // distinct texture ptrs
static char  g_pending[512][128]; static int g_pendN = 0;           // lines to flush from log thread

// distinct transform matrices already logged (state<<0 + a hash of the matrix)
static unsigned g_seenXform[256]; static int g_seenXformN = 0;

static void logline(const char *s) {
	FILE *f = fopen(g_logPath, "a"); if (!f) return; fputs(s, f); fputc('\n', f); fclose(f);
}
static void pend(const char *s) {                                    // add a line (under g_cs)
	if (g_pendN < 512) { lstrcpynA(g_pending[g_pendN], s, 128); g_pendN++; }
	InterlockedExchange(&g_dirty, 1);
}

static HRESULT WINAPI mySetTexture(IDirect3DDevice9 *dev, DWORD stage, IDirect3DBaseTexture9 *tex) {
	if (tex) {
		EnterCriticalSection(&g_cs);
		bool seen = false;
		for (int i = 0; i < g_seenTexN; i++) if (g_seenTex[i] == tex) { seen = true; break; }
		if (!seen && g_seenTexN < 512) {
			g_seenTex[g_seenTexN++] = tex;
			DWORD levels = tex->GetLevelCount();          // on IDirect3DBaseTexture9 - no IID needed
			D3DRESOURCETYPE rt = tex->GetType();          // on IDirect3DResource9 base
			char line[128];
			if (rt == D3DRTYPE_TEXTURE) {
				// GetType confirms a 2D texture, so the base ptr IS an IDirect3DTexture9 (single-inheritance COM)
				IDirect3DTexture9 *t2 = (IDirect3DTexture9 *)tex;
				D3DSURFACE_DESC d;
				if (SUCCEEDED(t2->GetLevelDesc(0, &d)))
					wsprintfA(line, "TEX stage=%u %ux%u levels=%u fmt=%d", stage, d.Width, d.Height, levels, d.Format);
				else
					wsprintfA(line, "TEX stage=%u (GetLevelDesc failed) levels=%u", stage, levels);
			} else {
				wsprintfA(line, "TEX stage=%u type=%d levels=%u (not a 2D texture)", stage, rt, levels);
			}
			pend(line);
		}
		LeaveCriticalSection(&g_cs);
	}
	return g_origSetTexture(dev, stage, tex);
}

static unsigned hashMatrix(D3DTRANSFORMSTATETYPE st, const D3DMATRIX *m) {
	unsigned h = 2166136261u ^ (unsigned)st;
	const unsigned char *p = (const unsigned char *)m;
	for (int i = 0; i < (int)sizeof(D3DMATRIX); i++) { h ^= p[i]; h *= 16777619u; }
	return h;
}
static int fx(float f) { return (int)(f * 1000.0f); }               // wsprintf has no %f

static HRESULT WINAPI mySetTransform(IDirect3DDevice9 *dev, D3DTRANSFORMSTATETYPE st, const D3DMATRIX *m) {
	if (m) {
		unsigned h = hashMatrix(st, m);
		EnterCriticalSection(&g_cs);
		bool seen = false;
		for (int i = 0; i < g_seenXformN; i++) if (g_seenXform[i] == h) { seen = true; break; }
		if (!seen && g_seenXformN < 256) {
			g_seenXform[g_seenXformN++] = h;
			const char *sn = st == D3DTS_VIEW ? "VIEW" : st == D3DTS_PROJECTION ? "PROJ" :
			                 st == D3DTS_WORLD ? "WORLD" : "OTHER";
			char line[128];
			// log the scale/translate-relevant cells: m11 m22 (scale x/y), m41 m42 (translate x/y), m44
			wsprintfA(line, "XFORM %s(%u) m11=%d.%03d m22=%d.%03d m41=%d.%03d m42=%d.%03d m44=%d.%03d (x1000)",
			          sn, (unsigned)st,
			          fx(m->_11)/1000, (fx(m->_11)<0?-fx(m->_11):fx(m->_11))%1000,
			          fx(m->_22)/1000, (fx(m->_22)<0?-fx(m->_22):fx(m->_22))%1000,
			          fx(m->_41)/1000, (fx(m->_41)<0?-fx(m->_41):fx(m->_41))%1000,
			          fx(m->_42)/1000, (fx(m->_42)<0?-fx(m->_42):fx(m->_42))%1000,
			          fx(m->_44)/1000, (fx(m->_44)<0?-fx(m->_44):fx(m->_44))%1000);
			pend(line);
		}
		LeaveCriticalSection(&g_cs);
	}
	return g_origSetTransform(dev, st, m);
}

static bool moduleNameOf(void *p, char *out, DWORD n) {
	HMODULE h = nullptr;
	if (p && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &h) && h) {
		char path[MAX_PATH]; GetModuleFileNameA(h, path, MAX_PATH);
		const char *b = strrchr(path, '\\'); lstrcpynA(out, b ? b + 1 : path, n); return true;
	}
	return false;
}
static void *hookSlot(IDirect3DDevice9 *dev, int idx, void *replacement) {
	void **vt = *(void ***)dev;
	void *target = vt[idx];
	char owner[MAX_PATH];
	if (!moduleNameOf(target, owner, sizeof owner)) { logline("ERROR: vtable slot not in any module"); return nullptr; }
	DWORD op; VirtualProtect(&vt[idx], sizeof(void *), PAGE_READWRITE, &op);
	vt[idx] = replacement;
	VirtualProtect(&vt[idx], sizeof(void *), op, &op);
	return target;
}

static DWORD WINAPI probeThread(LPVOID) {
	IDirect3DDevice9 *dev = nullptr;
	for (int i = 0; i < 600 && !dev; i++) { dev = GAME_DEVICE; if (!dev) Sleep(100); }
	if (!dev) { logline("device never appeared"); return 0; }

	const DWORD *pp = (const DWORD *)ADDR_PRESENT_PARAMS;
	HWND hwnd = *(HWND *)(ADDR_PRESENT_PARAMS + 0x1C);
	RECT cr = {0,0,0,0}; if (hwnd) GetClientRect(hwnd, &cr);
	{ char line[256]; wsprintfA(line, "CONTEXT ppGlobal=%ux%u windowed=%u | client=%ldx%ld",
	                            pp[0], pp[1], pp[8], cr.right - cr.left, cr.bottom - cr.top); logline(line); }

	g_origSetTexture   = (SetTexture_t)  hookSlot(dev, VT_SET_TEXTURE,   (void *)mySetTexture);
	g_origSetTransform = (SetTransform_t)hookSlot(dev, VT_SET_TRANSFORM, (void *)mySetTransform);
	if (!g_origSetTexture || !g_origSetTransform) { logline("hook failed"); return 0; }
	logline("hooks installed (SetTexture, SetTransform) - passive");

	int last = 0;
	for (int i = 0; i < 600; i++) {
		Sleep(1000);
		EnterCriticalSection(&g_cs); int n = g_pendN; LeaveCriticalSection(&g_cs);
		while (last < n && last < 512) { logline(g_pending[last]); last++; }
	}
	return 0;
}

extern "C" __declspec(dllexport) bool CheckVersion(const BYTE hash[16]) {
	return ::memcmp(TARGET_HASH, hash, sizeof TARGET_HASH) == 0;
}
extern "C" __declspec(dllexport) bool Initialize(HMODULE hMyModule, HMODULE hParentModule) {
	g_module = hMyModule;
	InitializeCriticalSection(&g_cs);
	GetModuleFileNameA(g_module, g_logPath, 1024);
	PathRemoveFileSpecA(g_logPath); PathAppendA(g_logPath, "DrawProbe.log");
	logline("--- DrawProbe start (read-only; logs distinct textures + transforms) ---");
	CloseHandle(CreateThread(nullptr, 0, probeThread, nullptr, 0, nullptr));
	return TRUE;
}
extern "C" int APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved) { return TRUE; }
