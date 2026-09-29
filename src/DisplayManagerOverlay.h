// DisplayManager overlay API - lets another mod draw on DisplayManager's final fullscreen frame, e.g. into
// the borders around the scaled game image.
//
// In fullscreen, DisplayManager rebuilds the whole backbuffer in the swapchain's Present: it fills the borders
// and scales the game's 640x480 frame into the centered game rect. Anything another mod draws into the
// backbuffer before that is overwritten, and a mod can't reliably hook Present *after* DisplayManager (the
// hook order follows the mod load order). A registered overlay is called at the right point instead: after
// the frame is composited, right before it is presented.
//
// Usage (resolve at runtime; DisplayManager may load after your mod, so retry until the module exists):
//
//   HMODULE dm = GetModuleHandleA("DisplayManager.dll");
//   auto add = (DisplayManager_AddOverlay_t)GetProcAddress(dm, "DisplayManager_AddOverlay");
//   if (add) add(myOverlay, myUserData);
//
// The callback runs on the game's render thread, only while DisplayManager is compositing (fullscreen, exclusive
// or borderless, and windowed with WindowedFilter=1, where the backbuffer is the window's client area). No scene
// is open: draw with ColorFill / StretchRect / UpdateSurface. If you need BeginScene/EndScene, restore every
// device state you touch, and keep in mind that other mods hook BeginScene/EndScene (calling them re-runs their
// per-scene code). Release your D3DPOOL_DEFAULT resources on DM_OVERLAY_RESET: the device is about to be Reset
// (e.g. Alt+Enter, or a windowed resize).
#pragma once
#include <windows.h>
#include <d3d9.h>

#define DM_OVERLAY_DRAW  0   // the composited frame is in info->backbuffer; draw on it
#define DM_OVERLAY_RESET 1   // the device is about to be Reset: release D3DPOOL_DEFAULT resources

typedef struct DisplayManager_OverlayInfo {
	UINT               size;          // sizeof(DisplayManager_OverlayInfo); newer versions may append fields
	IDirect3DDevice9  *device;
	IDirect3DSurface9 *backbuffer;    // DRAW only (null for RESET); not AddRef'd, valid during the call
	UINT               bbWidth;       // backbuffer size (the fullscreen resolution)
	UINT               bbHeight;
	D3DFORMAT          bbFormat;
	RECT               gameRect;      // where the scaled game image is, in backbuffer pixels
	D3DCOLOR           borderColor;   // the color DisplayManager fills the borders with (BackgroundColor)
} DisplayManager_OverlayInfo;

typedef void (__cdecl *DisplayManager_OverlayProc)(int event, const DisplayManager_OverlayInfo *info, void *user);

// Register / unregister a callback (thread-safe; the same proc+user pair is only registered once). Add returns
// FALSE when DisplayManager is disabled or standing down, or the table (8 entries) is full.
typedef BOOL (__cdecl *DisplayManager_AddOverlay_t)(DisplayManager_OverlayProc proc, void *user);
typedef BOOL (__cdecl *DisplayManager_RemoveOverlay_t)(DisplayManager_OverlayProc proc, void *user);
