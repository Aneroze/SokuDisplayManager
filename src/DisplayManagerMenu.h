// DisplayManager's in-game settings menu (Dear ImGui): the mouse-driven way to change the settings the hotkeys used to.
//
// Included once, by DisplayManager.cpp (one translation unit, like the rest), after doAction: it reads and writes the
// same settings the hotkeys do. The entry points are declared near g_menuOpen in DisplayManager.cpp.
//
// Threads
// -------
//  - window thread: the Menu hotkey (menuToggle), the mouse messages (menuWndProc: queued for the render thread, and
//    kept from the game while the menu is shown) and the few commands that have to run on it (menuCommand: resizing the
//    window, always-on-top, closing - the mouse capture belongs to this thread).
//  - render thread: everything ImGui (menuRender, called from the swapchain Present once per composited frame): the
//    queued input goes into ImGui's io, the UI is built - its changes are made to the settings right there, as the
//    Present reads them on this same thread - and drawn onto the finished frame, borders included.
// ImGui and its DX9 backend are only called from the render thread, except menuInvalidate (a device Reset, under the
// game's render lock, so no frame is in progress).
//
// The menu is only created on first use: with it never opened, ImGui does nothing at all.
//
// Input: the game reads its keys through DirectInput, so they still reach it while the menu is open - the menu is
// mouse-only on purpose (no keyboard navigation or text entry), and the mouse is swallowed from the game while shown.
// The cursor is ImGui's own (drawn in the frame): the Windows cursor is hidden in exclusive fullscreen anyway.
#pragma once

// ---- input from the window thread -------------------------------------------------------------------
enum { MEV_POS, MEV_BUTTON, MEV_WHEEL, MEV_RELEASE_ALL };
struct MenuEvent { int type; float a, b; };
static const int MENU_QUEUE = 256;
static MenuEvent g_menuQ[MENU_QUEUE];
static int       g_menuQN = 0;
static SRWLOCK   g_menuQLock = SRWLOCK_INIT;
static int       g_menuBtnMask = 0;      // mouse buttons held while the menu is shown (window thread)
static bool      g_menuTracking = false; // TrackMouseEvent asked for the WM_MOUSELEAVE (window thread)

static void menuPush(int type, float a = 0.0f, float b = 0.0f) {
	AcquireSRWLockExclusive(&g_menuQLock);
	if (type == MEV_POS && g_menuQN > 0 && g_menuQ[g_menuQN - 1].type == MEV_POS) {
		g_menuQ[g_menuQN - 1].a = a;     // consecutive moves: keep the last
		g_menuQ[g_menuQN - 1].b = b;
	} else if (g_menuQN < MENU_QUEUE) {
		MenuEvent e = { type, a, b };
		g_menuQ[g_menuQN++] = e;
	}
	ReleaseSRWLockExclusive(&g_menuQLock);
}

// A client-area mouse position (the lParam of a mouse message) in backbuffer pixels. They are the same size in
// fullscreen and windowed with WindowedFilter; scaled anyway in case Windows is stretching the window (DpiAware=0).
static void menuPushPos(HWND h, LPARAM lp) {
	float x = (float)(short)LOWORD(lp), y = (float)(short)HIWORD(lp);
	RECT cr;
	if (GetClientRect(h, &cr) && cr.right > 0 && cr.bottom > 0 && g_bbW && g_bbH) {
		x = x * (float)g_bbW / (float)cr.right;
		y = y * (float)g_bbH / (float)cr.bottom;
	}
	menuPush(MEV_POS, x, y);
}

static bool menuShown() {
	return g_menuOpen && g_active && !g_resetBypassed;
}

// Window thread. True = handled, *res is the message's result; false = the game gets it.
static bool menuWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, LRESULT *res) {
	if (msg == WM_MOUSELEAVE) g_menuTracking = false;   // (one-shot, whether or not the menu is still shown)
	if (!menuShown()) {
		if (g_menuBtnMask) {                       // the menu went away with a button down: let go of the mouse
			g_menuBtnMask = 0;
			if (GetCapture() == h) ReleaseCapture();
		}
		return false;
	}
	switch (msg) {
	case WM_SETCURSOR:                             // ImGui draws the cursor
		if (LOWORD(lp) != HTCLIENT) return false;
		SetCursor(nullptr);
		*res = TRUE;
		return true;
	case WM_MOUSEMOVE:
		if (!g_menuTracking) {                     // windowed: hear when the cursor leaves the client area
			TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 };
			g_menuTracking = TrackMouseEvent(&t) != FALSE;
		}
		menuPushPos(h, lp);
		*res = 0;
		return true;
	case WM_MOUSELEAVE:                            // hide ImGui's cursor, or it stays drawn at the edge
		menuPush(MEV_POS, -FLT_MAX, -FLT_MAX);
		*res = 0;
		return true;
	case WM_CAPTURECHANGED:                        // lost the capture mid-drag (e.g. another window took it)
		if (g_menuBtnMask && (HWND)lp != h) { g_menuBtnMask = 0; menuPush(MEV_RELEASE_ALL); }
		return false;
	case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
	case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: {
		int b = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) ? 0 :
		        (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK) ? 1 : 2;
		menuPushPos(h, lp);
		if (!g_menuBtnMask) SetCapture(h);         // so the release is seen even outside the window
		g_menuBtnMask |= 1 << b;
		menuPush(MEV_BUTTON, (float)b, 1.0f);
		*res = 0;
		return true;
	}
	case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
		int b = msg == WM_LBUTTONUP ? 0 : msg == WM_RBUTTONUP ? 1 : 2;
		menuPushPos(h, lp);
		g_menuBtnMask &= ~(1 << b);
		menuPush(MEV_BUTTON, (float)b, 0.0f);
		if (!g_menuBtnMask && GetCapture() == h) ReleaseCapture();
		*res = 0;
		return true;
	}
	case WM_MOUSEWHEEL:
		menuPush(MEV_WHEEL, (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA);
		*res = 0;
		return true;
	case WM_KILLFOCUS:                             // (the game still gets it)
		if (g_menuBtnMask) { g_menuBtnMask = 0; menuPush(MEV_RELEASE_ALL); }
		return false;
	}
	return false;
}

// ---- window-thread side -----------------------------------------------------------------------------
enum { MCMD_WINSCALE = 1, MCMD_TOPMOST, MCMD_CLOSE };

// Window thread: switch the menu on or off (the Menu hotkey).
static void menuToggle() {
	if (!g_menuOpen) {
		if (!g_active || g_resetBypassed) {        // nothing of ours is drawn on this frame: no menu to show
			logf("menu: not available (windowed without WindowedFilter, or DisplayManager is standing down)");
			return;
		}
		AcquireSRWLockExclusive(&g_menuQLock);
		g_menuQN = 0;
		ReleaseSRWLockExclusive(&g_menuQLock);
		menuPush(MEV_RELEASE_ALL);                 // (a drag cut short by closing with the key left a button down)
		g_menuBtnMask = 0;
		POINT p;                                   // the cursor is drawn from the first frame
		if (g_hwnd && GetCursorPos(&p) && ScreenToClient(g_hwnd, &p)) menuPushPos(g_hwnd, MAKELPARAM((short)p.x, (short)p.y));
		g_menuOpen = true;
	} else {
		g_menuOpen = false;
		g_menuBtnMask = 0;
		if (g_hwnd && GetCapture() == g_hwnd) ReleaseCapture();
	}
	logf("menu: %s", g_menuOpen ? "open" : "closed");
}

static void menuCommand(int cmd, LPARAM value) {
	switch (cmd) {
	case MCMD_WINSCALE:
		if (!g_wantFullscreen) setWindowScaled((int)value, nullptr);
		break;
	case MCMD_TOPMOST:
		applyTopmost();
		break;
	case MCMD_CLOSE:
		if (g_menuOpen) menuToggle();
		break;
	}
}

// Render thread -> window thread. (Without the subclassed window procedure there is no message to send: run it here.)
static void menuPost(int cmd, LPARAM value) {
	if (g_hwnd && g_menuMsg) PostMessageA(g_hwnd, g_menuMsg, (WPARAM)cmd, value);
	else menuCommand(cmd, value);
}

// ---- ImGui set-up (render thread) -------------------------------------------------------------------
static bool             g_imguiCtx     = false;     // the ImGui context exists
static IDirect3DDevice9 *g_imguiDev    = nullptr;   // the device the backend is initialised for (null = not)
static LARGE_INTEGER    g_imguiLast    = {};
static bool             g_menuFailed   = false;     // the backend couldn't be initialised: no menu this session
static float            g_menuScale    = 0.0f;      // the size factor in effect
static ImGuiStyle       g_menuBaseStyle;            // the style at scale 1
static DWORD            g_menuSavedAt  = 0;         // GetTickCount() of the last "Save settings"

// The menu's size when [Menu] Scale is Auto: the screen height decides (1.0 below 900 px up to 2.0 from 2000 px).
static float menuAutoScale() {
	return g_bbH >= 2000 ? 2.0f : g_bbH >= 1300 ? 1.5f : g_bbH >= 900 ? 1.25f : 1.0f;
}

// The menu's font, rasterized at its size on screen (stretching a 1x atlas with io.FontGlobalScale blurs it): Segoe UI
// (every Windows since Vista), else - Wine, usually - ImGui's built-in pixel font at the nearest whole multiple of its
// 13 px, the only sizes where it stays crisp. Render thread, outside NewFrame..Render; the backend's font texture is
// dropped so the next NewFrame uploads the new atlas.
static void menuBuildFont(float scale) {
	ImFontAtlas *fonts = ImGui::GetIO().Fonts;
	fonts->Clear();
	ImFontConfig cfg;
	cfg.PixelSnapH = true;                         // glyphs on whole pixels: no 1x atlas is ever stretched
	cfg.OversampleH = cfg.OversampleV = 1;         // (oversampling only helps sub-pixel positions)
	const float size = floorf(15.0f * scale + 0.5f), builtinSize = 13.0f * floorf(scale + 0.5f);   // (scale >= 0.75)
	char path[MAX_PATH * 3] = {0};
	wchar_t dir[MAX_PATH + 32];
	UINT n = GetWindowsDirectoryW(dir, MAX_PATH);  // ImGui opens files by UTF-8 path
	if (n && n < MAX_PATH) {
		lstrcatW(dir, L"\\Fonts\\segoeui.ttf");
		if (GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES)
			WideCharToMultiByte(CP_UTF8, 0, dir, -1, path, sizeof path, nullptr, nullptr);
	}
	const ImFont *f = path[0] ? fonts->AddFontFromFileTTF(path, size, &cfg) : nullptr;
	if (!f) {
		ImFontConfig def;
		def.SizePixels = builtinSize;
		fonts->AddFontDefault(&def);
	}
	ImGui_ImplDX9_InvalidateDeviceObjects();
	logf("menu: font %s %d px (scale %.2f)", f ? "Segoe UI" : "built-in", (int)(f ? size : builtinSize), (double)scale);
}

static bool menuInit(IDirect3DDevice9 *dev) {
	if (g_imguiDev == dev) return true;
	if (g_imguiDev) {                              // another device than before (not expected): start the backend again
		ImGui_ImplDX9_Shutdown();
		g_imguiDev = nullptr;
	}
	if (!g_imguiCtx) {
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO &io = ImGui::GetIO();
		io.IniFilename = nullptr;                  // no imgui.ini / imgui.log next to the game
		io.LogFilename = nullptr;
		io.MouseDrawCursor = true;
		io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
		ImGui::StyleColorsDark();
		ImGuiStyle &st = ImGui::GetStyle();
		st.WindowRounding = 6.0f;
		st.FrameRounding = 3.0f;
		st.GrabRounding = 3.0f;
		st.WindowBorderSize = 1.0f;
		st.Colors[ImGuiCol_WindowBg].w = 0.94f;
		g_menuBaseStyle = st;
		g_menuScale = 0.0f;
		g_imguiCtx = true;
	}
	if (!ImGui_ImplDX9_Init(dev)) return false;
	g_imguiDev = dev;
	QueryPerformanceCounter(&g_imguiLast);
	return true;
}

// Before a device Reset (releaseCapture): the backend's vertex / index buffers and font texture are D3DPOOL_DEFAULT.
// The next frame creates them again.
static void menuInvalidate() {
	if (g_imguiDev) ImGui_ImplDX9_InvalidateDeviceObjects();
}

// ---- the UI -----------------------------------------------------------------------------------------
static void menuTouch() { g_menuTouched = true; }

// "Auto" box + slider for a setting that can follow the output scale's picked value (SCALE_DEFAULTS). `chosen` is where
// the player's value lives, `live` the value in effect (what Auto resolves to). Leaving Auto keeps the live value.
// `locked` (Filter=Auto, which uses its own values): shows the live value and nothing can be changed.
static bool menuAutoSlider(const char *label, bool *autoFlag, float *chosen, float live, float lo, float hi,
                           const char *fmt, bool locked) {
	bool changed = false;
	ImGui::PushID(label);
	bool a = *autoFlag;
	if (ImGui::Checkbox("Auto", &a)) {
		if (!a) *chosen = live;
		*autoFlag = a;
		changed = true;
	}
	ImGui::SameLine();
	float v = (a || locked) ? live : *chosen;
	ImGui::BeginDisabled(a);
	if (ImGui::SliderFloat(label, &v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp)) {
		*chosen = v;
		*autoFlag = false;
		changed = true;
	}
	ImGui::EndDisabled();
	ImGui::PopID();
	return changed;
}

// Sharp sprites for the characters / the stage: a checkbox (off = 0, on = the last value) and the sharpness.
static void menuSpriteRow(const char *label, int layer, bool locked) {
	ImGui::PushID(label);
	const float cur = locked ? g_sprK[layer] : g_cfgSprK[layer];
	bool on = cur > 0.0f;
	if (ImGui::Checkbox("On", &on)) {
		g_cfgSprK[layer] = on ? g_sprLastK[layer] : 0.0f;
		menuTouch();
		resolveSettings();
	}
	ImGui::SameLine();
	float k = on ? cur : g_sprLastK[layer];
	ImGui::BeginDisabled(!on);
	if (ImGui::SliderFloat(label, &k, SPR_K_MIN, SPR_K_MAX, "%.2f",
	                       ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) {
		g_cfgSprK[layer] = g_sprLastK[layer] = k;
		g_sprLastKSet[layer] = true;
		menuTouch();
		resolveSettings();
	}
	ImGui::EndDisabled();
	ImGui::PopID();
}

// A 0..1 strength, in the player's value (cfg) or, with Filter=Auto, the live one.
static void menuStrength(const char *label, float *cfg, float live, bool locked) {
	float v = locked ? live : *cfg;
	if (ImGui::SliderFloat(label, &v, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
		*cfg = v;
		menuTouch();
		resolveSettings();
	}
}

static const char *const MENU_MODES[]   = { "FitToScreen", "IntegerScaling", "CustomResolution" };   // MODE_*
static const char *const MENU_FILTERS[] = { "Point", "Linear", "Sharp", "xBR", "Auto" };               // FILTER_*
static const int         MENU_MSAA[]    = { 0, 2, 4, 8 };

static void menuBody() {
	ImGui::TextDisabled("%s: the game is drawn at %dx%d on %ux%u",
	                    g_wantFullscreen ? "Fullscreen" : "Windowed", g_scaleW, g_scaleH, g_bbW, g_bbH);

	if (ImGui::CollapsingHeader("Size", ImGuiTreeNodeFlags_DefaultOpen)) {
		int mode = g_mode;
		if (ImGui::Combo("Fullscreen size", &mode, MENU_MODES, 3)) {
			g_mode = mode;
			menuTouch();
			filterChanged();
		}
		if (g_mode == MODE_INTEGER) {
			int n = g_intScale < 1 ? 1 : g_intScale > 8 ? 8 : g_intScale;
			if (ImGui::SliderInt("Fullscreen scale", &n, 1, 8, "x%d", ImGuiSliderFlags_AlwaysClamp)) {
				g_intScale = n;
				menuTouch();
				filterChanged();
			}
		} else if (g_mode == MODE_CUSTOM) {
			int w = g_customW, h = g_customH;
			bool c = ImGui::SliderInt("Width", &w, 640, 7680, "%d px", ImGuiSliderFlags_AlwaysClamp);
			c |= ImGui::SliderInt("Height", &h, 480, 4320, "%d px", ImGuiSliderFlags_AlwaysClamp);
			if (c) {
				g_customW = w;
				g_customH = h;
				menuTouch();
				filterChanged();
			}
		}
		int ws = g_winScale < 1 ? 1 : g_winScale > 8 ? 8 : g_winScale;
		if (ImGui::SliderInt("Window scale", &ws, 1, 8, "x%d", ImGuiSliderFlags_AlwaysClamp)) {
			g_winScale = ws;
			menuTouch();
			if (!g_wantFullscreen) menuPost(MCMD_WINSCALE, (LPARAM)ws);   // windowed: resize the window now
		}
		bool top = g_topmost;
		if (ImGui::Checkbox("Always on top", &top)) {
			g_topmost = top;
			menuPost(MCMD_TOPMOST, 0);
		}
		float col[3] = { (float)((g_bgColor >> 16) & 0xFF) / 255.0f, (float)((g_bgColor >> 8) & 0xFF) / 255.0f,
		                 (float)(g_bgColor & 0xFF) / 255.0f };
		if (ImGui::ColorEdit3("Border color", col, ImGuiColorEditFlags_NoInputs)) {
			g_bgColor = D3DCOLOR_XRGB((int)(col[0] * 255.0f + 0.5f), (int)(col[1] * 255.0f + 0.5f),
			                          (int)(col[2] * 255.0f + 0.5f));
			menuTouch();
		}
	}

	const bool af = g_filterCfg == FILTER_AUTO;
	if (ImGui::CollapsingHeader("Filter", ImGuiTreeNodeFlags_DefaultOpen)) {
		int f = g_filterCfg;
		if (ImGui::Combo("Upscale filter", &f, MENU_FILTERS, FILTER_COUNT)) {
			g_filterCfg = f;
			menuTouch();
			filterChanged();
		}
		if (af) {
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextDisabled("Auto is Sharp plus filtered sprites, with values picked for the output size. "
			                    "Pick another filter to set them yourself.");
			ImGui::PopTextWrapPos();
		}
		ImGui::BeginDisabled(af);
		if (menuAutoSlider("Sharpness (Sharp)", &g_auto[AUTO_SHARP], &g_cfgSharp, g_sharpness, SHARP_MIN, SHARP_MAX,
		                   "%.2f", af)) {
			menuTouch();
			resolveSettings();
		}
		ImGui::EndDisabled();
	}

	if (ImGui::CollapsingHeader("xBR settings")) {
		if (menuAutoSlider("Strength", &g_auto[AUTO_XBR_STRENGTH], &g_xbrStrength, g_xbrStrength, 0.0f, 1.0f, "%.2f",
		                   false)) {
			menuTouch();
			resolveSettings();
		}
		{
			ImGui::PushID("corner");
			bool a = g_auto[AUTO_XBR_CORNER];
			if (ImGui::Checkbox("Auto", &a)) { g_auto[AUTO_XBR_CORNER] = a; menuTouch(); resolveSettings(); }
			ImGui::SameLine();
			ImGui::BeginDisabled(a);
			int c = g_xbrCorner;
			if (ImGui::Combo("Corner", &c, "A (roundest)\0B\0C\0D (keeps most corners)\0")) {
				g_xbrCorner = c;
				g_auto[AUTO_XBR_CORNER] = false;
				menuTouch();
			}
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		{
			ImGui::PushID("slopes");
			bool a = g_auto[AUTO_XBR_SLOPES];
			if (ImGui::Checkbox("Auto", &a)) { g_auto[AUTO_XBR_SLOPES] = a; menuTouch(); resolveSettings(); }
			ImGui::SameLine();
			ImGui::BeginDisabled(a);
			bool sl = g_xbrSlopes;
			if (ImGui::Checkbox("Smooth 30/60 degree edges", &sl)) {
				g_xbrSlopes = sl;
				g_auto[AUTO_XBR_SLOPES] = false;
				menuTouch();
			}
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		if (menuAutoSlider("Width", &g_auto[AUTO_XBR_WIDTH], &g_xbrWidth, g_xbrWidth, 0.25f, 4.0f, "%.2f", false)) {
			menuTouch();
			resolveSettings();
		}
	}

	if (ImGui::CollapsingHeader("Filtered sprites (experimental)")) {
		ImGui::PushTextWrapPos(0.0f);
		if (g_sprFailed)
			ImGui::TextDisabled("Not available: the sprite shader could not be created.");
		else if (af)
			ImGui::TextDisabled("Filter is Auto: it sets these. Pick another filter to change them.");
		else
			ImGui::TextDisabled("Filters the characters and the stage (sharp-bilinear) instead of drawing the game's "
			                    "hard, uneven pixels.");
		ImGui::PopTextWrapPos();
		ImGui::BeginDisabled(af || g_sprFailed);
		menuSpriteRow("Characters", SPR_CHARS, af);
		menuSpriteRow("Stage", SPR_STAGE, af);
		menuStrength("Characters: strength up close", &g_cfgSprNear, g_sprRest[SPR_CHARS], af);
		menuStrength("Characters: strength far apart", &g_cfgSprFar, g_sprFar, af);
		if (menuAutoSlider("Far apart from zoom", &g_auto[AUTO_FARZOOM], &g_cfgFarZoom, g_sprFarZoom, SPR_FARZOOM_MIN,
		                   SPR_FARZOOM_MAX, "%.2f", af)) {
			menuTouch();
			resolveSettings();
		}
		if (menuAutoSlider("Stage strength at rest", &g_auto[AUTO_BGREST], &g_cfgBgRest, g_sprRest[SPR_STAGE], 0.0f,
		                   1.0f, "%.2f", af)) {
			menuTouch();
			resolveSettings();
		}
		ImGui::EndDisabled();
	}

	if (ImGui::CollapsingHeader("Anti-aliasing (presently, mostly does nothing)")) {
		int cur = 0;
		for (int i = 0; i < 4; i++) if (MENU_MSAA[i] == (int)g_msaaCfg) cur = i;
		if (ImGui::Combo("MSAA", &cur, "Off\0x2\0x4\0x8\0")) {
			g_msaaCfg = MENU_MSAA[cur];
			g_msaaApply = true;                    // the render thread recreates its targets after this Present
			menuTouch();
		}
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("Only smooths polygon edges; the game's sprites and stages are not noticeably affected.");
		ImGui::PopTextWrapPos();
	}

	if (ImGui::CollapsingHeader("Menu")) {
		bool a = g_menuScaleCfg <= 0.0f;
		ImGui::PushID("menuScale");
		if (ImGui::Checkbox("Auto", &a)) g_menuScaleCfg = a ? 0.0f : g_menuScale;
		ImGui::SameLine();
		ImGui::BeginDisabled(a);
		// Applied on release: rescaling mid-drag would move the slider under the mouse and rebuild the font each frame.
		static float drag = 0.0f;
		static bool  dragging = false;
		float v = dragging ? drag : a ? g_menuScale : g_menuScaleCfg;
		ImGui::SliderFloat("Menu size", &v, 0.75f, 3.0f, "x%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (ImGui::IsItemActive()) { drag = v; dragging = true; }
		if (ImGui::IsItemDeactivated()) {
			dragging = false;
			if (ImGui::IsItemDeactivatedAfterEdit()) g_menuScaleCfg = drag;
		}
		ImGui::EndDisabled();
		ImGui::PopID();
		ImGui::TextDisabled("For a size at startup: [Menu] Scale in the ini.");
	}

	ImGui::Separator();
	// PersistState / PersistPosition: written to the ini at once (they decide what happens at exit, so they can't wait
	// for it). Turning PersistState off asks first: from then on the menu's changes are only kept with Save settings.
	bool ps = g_persist;
	if (ImGui::Checkbox("Save settings on exit (PersistState)", &ps)) {
		if (ps) {
			g_persist = true;
			writeIniIfChanged("PersistState", "1");
		} else {
			ImGui::OpenPopup("Turn off PersistState?");
		}
	}
	bool pp = g_persistPos;
	if (ImGui::Checkbox("Remember the window position (PersistPosition)", &pp)) {
		g_persistPos = pp;
		writeIniIfChanged("PersistPosition", pp ? "1" : "0");
	}
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	if (ImGui::BeginPopupModal("Turn off PersistState?", nullptr,
	                           ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
		ImGui::TextUnformatted("Changes made in this menu (or with the hotkeys) will no longer be saved when the game "
		                       "exits. They will only be kept if you press Save settings before quitting.");
		ImGui::PopTextWrapPos();
		if (ImGui::Button("Turn off")) {
			g_persist = false;
			writeIniIfChanged("PersistState", "0");
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	if (ImGui::Button("Save settings")) {
		persistSettings();                         // Mode, scales, Filter, Sharpness...
		menuPersist();                             // ...and the rest of what is set here
		g_menuSavedAt = GetTickCount() | 1;
	}
	ImGui::SameLine();
	if (ImGui::Button("Close")) menuPost(MCMD_CLOSE, 0);
	if (g_menuSavedAt && GetTickCount() - g_menuSavedAt < 2500) {
		ImGui::SameLine();
		ImGui::TextDisabled("Saved.");
	}
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("%s", g_persist ? "The settings are saved when the game exits."
	                                    : "The settings are only saved with Save settings.");
	ImGui::TextDisabled("Borderless, VSync, DPI handling and the other options are in DisplayManager.ini.");
	if (g_hotkeyVk[ACT_MENU]) {
		static const char *const mods[] = { "Alt", "Ctrl", "Shift", "Win", "" };
		ImGui::TextDisabled("%s%s%c opens and closes this menu.", mods[g_modifier], g_modifier == MODK_NONE ? "" : "+",
		                    (char)g_hotkeyVk[ACT_MENU]);
	}
	ImGui::PopTextWrapPos();
}

// The menu window as of the last frame, and the display size / menu size it was placed for (0 = never shown).
static ImVec2 g_menuWinPos, g_menuWinSize, g_menuLastDisp;
static float  g_menuLastScale = 0.0f;
static bool   g_menuWinCollapsed = false;

// Where a window edge goes when the free space beside it changes from oldFree to newFree: the same fraction of it, so
// a window against the right / bottom edge stays there.
static float menuKeepPlace(float pos, float oldFree, float newFree) {
	if (newFree <= 0.0f) return 0.0f;
	return oldFree > 0.0f ? pos * newFree / oldFree : 0.0f;
}

static void menuBuild() {
	const float s = g_menuScale;
	const ImVec2 disp = ImGui::GetIO().DisplaySize;
	float h = disp.y - 48.0f * s;
	if (h > 640.0f * s) h = 640.0f * s;
	ImGui::SetNextWindowPos(ImVec2(24.0f * s, 24.0f * s), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(480.0f * s, h), ImGuiCond_FirstUseEver);
	// The display (windowed Alt+1..6, fullscreen <-> windowed) or the menu size changed since the last frame shown: the
	// window keeps its place relative to the screen and its size relative to the menu's. ImGui would keep its absolute
	// position, which can leave it mostly off a smaller screen.
	if (g_menuLastScale > 0.0f &&
	    (disp.x != g_menuLastDisp.x || disp.y != g_menuLastDisp.y || s != g_menuLastScale)) {
		ImVec2 size = g_menuWinSize;
		if (!g_menuWinCollapsed) {                 // (collapsed, its size is the title bar's: leave the full size alone)
			size = ImVec2(size.x * s / g_menuLastScale, size.y * s / g_menuLastScale);
			if (size.x > disp.x) size.x = disp.x;
			if (size.y > disp.y) size.y = disp.y;
			ImGui::SetNextWindowSize(size, ImGuiCond_Always);
		}
		ImGui::SetNextWindowPos(ImVec2(menuKeepPlace(g_menuWinPos.x, g_menuLastDisp.x - g_menuWinSize.x, disp.x - size.x),
		                               menuKeepPlace(g_menuWinPos.y, g_menuLastDisp.y - g_menuWinSize.y, disp.y - size.y)),
		                        ImGuiCond_Always);
	}
	ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f * s, 160.0f * s), disp);
	bool open = true;
	if (ImGui::Begin("DisplayManager " DM_VERSION "###DisplayManagerMenu", &open, ImGuiWindowFlags_NoSavedSettings))
		menuBody();
	{   // Always entirely on screen, so it can't be dragged (or left) out of reach.
		const ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
		const float maxX = disp.x - sz.x > 0.0f ? disp.x - sz.x : 0.0f, maxY = disp.y - sz.y > 0.0f ? disp.y - sz.y : 0.0f;
		const ImVec2 c(p.x < 0.0f ? 0.0f : p.x > maxX ? maxX : p.x, p.y < 0.0f ? 0.0f : p.y > maxY ? maxY : p.y);
		if (c.x != p.x || c.y != p.y) ImGui::SetWindowPos(c);
		g_menuWinPos = c;
		g_menuWinSize = sz;
		g_menuWinCollapsed = ImGui::IsWindowCollapsed();
	}
	ImGui::End();
	g_menuLastDisp = disp;
	g_menuLastScale = s;
	if (!open) menuPost(MCMD_CLOSE, 0);
}

// ---- one frame (render thread) ----------------------------------------------------------------------
// Called from the swapchain Present, on a frame DisplayManager has composited into `bb` (the backbuffer, the render
// target at this point), before it is presented.
static void menuRender(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb) {
	if (g_menuFailed || !dev || !bb || !g_stateBlock || !g_bbW || !g_bbH) return;
	if (!menuInit(dev)) {
		g_menuFailed = true;
		logf("menu: ImGui_ImplDX9_Init failed - no menu this session");
		menuPost(MCMD_CLOSE, 0);
		return;
	}
	ImGuiIO &io = ImGui::GetIO();

	// The mouse, queued by the window thread.
	MenuEvent ev[MENU_QUEUE];
	AcquireSRWLockExclusive(&g_menuQLock);
	const int n = g_menuQN;
	memcpy(ev, g_menuQ, n * sizeof(MenuEvent));
	g_menuQN = 0;
	ReleaseSRWLockExclusive(&g_menuQLock);
	for (int i = 0; i < n; i++) {
		switch (ev[i].type) {
		case MEV_POS:         io.AddMousePosEvent(ev[i].a, ev[i].b); break;
		case MEV_BUTTON:      io.AddMouseButtonEvent((int)ev[i].a, ev[i].b != 0.0f); break;
		case MEV_WHEEL:       io.AddMouseWheelEvent(0.0f, ev[i].a); break;
		case MEV_RELEASE_ALL: for (int b = 0; b < 3; b++) io.AddMouseButtonEvent(b, false); break;
		}
	}

	LARGE_INTEGER now, freq;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);
	float dt = freq.QuadPart ? (float)((double)(now.QuadPart - g_imguiLast.QuadPart) / (double)freq.QuadPart) : 0.0f;
	g_imguiLast = now;
	io.DeltaTime = dt < 0.001f ? 0.001f : dt > 0.25f ? 0.25f : dt;
	io.DisplaySize = ImVec2((float)g_bbW, (float)g_bbH);

	const float want = g_menuScaleCfg > 0.0f ? g_menuScaleCfg : menuAutoScale();
	if (fabsf(want - g_menuScale) > 0.001f) {
		g_menuScale = want;
		ImGuiStyle &st = ImGui::GetStyle();
		st = g_menuBaseStyle;
		st.ScaleAllSizes(want);
		menuBuildFont(want);
	}

	ImGui_ImplDX9_NewFrame();                      // (re)creates the backend's buffers / font texture after a Reset
	if (!io.Fonts->TexID) return;                  // couldn't (device lost?): try again next frame
	ImGui::NewFrame();
	menuBuild();
	ImGui::Render();

	// Drawn like DisplayManager's own shader quad (see drawShaderQuad): the scene calls run with the guard surface
	// bound, so another mod's inline EndScene hook draws its 640x480 menu there and not on our finished frame.
	g_stateBlock->Capture();
	IDirect3DSurface9 *guard = g_sceneGuard ? g_sceneGuard : bb;
	dev->SetRenderTarget(0, guard);
	const HRESULT hrScene = g_sceneDirect ? g_origBeginScene(dev) : dev->BeginScene();
	if (SUCCEEDED(hrScene)) {
		dev->SetRenderTarget(0, bb);
		dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
		dev->SetRenderTarget(0, guard);
		if (g_sceneDirect) g_origEndScene(dev); else dev->EndScene();
	}
	dev->SetRenderTarget(0, bb);
	g_stateBlock->Apply();
}

// ---- saving -----------------------------------------------------------------------------------------
// The settings persistState doesn't write (it has Mode, the scales, Filter and Sharpness): the border color, the
// custom size, the xBR knobs, the sharp sprites' values and MSAA. Only called once the menu changed something (or by
// its Save button), so an ini the menu never touched stays as it is. Auto values are written as Auto.
// A key the ini doesn't have (commented out, as most of these ship) is only added when its value isn't what the
// missing key already means (`asMissing`): WritePrivateProfileString would append it at the end of [Display].
static void menuWriteKey(const char *key, const char *val, bool asMissing) {
	if (asMissing && !iniHasKey(key)) return;
	writeIniIfChanged(key, val);
}

static void menuPersist() {
	char v[32];
	wsprintfA(v, "%02X%02X%02X", (UINT)((g_bgColor >> 16) & 0xFF), (UINT)((g_bgColor >> 8) & 0xFF), (UINT)(g_bgColor & 0xFF));
	writeIniIfChanged("BackgroundColor", v);
	wsprintfA(v, "%d", g_customW);
	writeIniIfChanged("CustomWidth", v);
	wsprintfA(v, "%d", g_customH);
	writeIniIfChanged("CustomHeight", v);

	// Missing = Auto, or the fixed value `missing` (asAuto false).
	struct Num { const char *key; bool autoVal; float val; bool asAuto; float missing; };
	const Num nums[] = {
		{ "XbrStrength",            g_auto[AUTO_XBR_STRENGTH], g_xbrStrength, true,  0.0f },
		{ "XbrWidth",               g_auto[AUTO_XBR_WIDTH],    g_xbrWidth,    true,  0.0f },
		{ "SpriteFarZoom",          g_auto[AUTO_FARZOOM],      g_cfgFarZoom,  true,  0.0f },
		{ "BackgroundRestStrength", g_auto[AUTO_BGREST],       g_cfgBgRest,   true,  0.0f },
		{ "SpriteNearStrength",     false,                     g_cfgSprNear,  false, 0.0f },
		{ "SpriteFarStrength",      false,                     g_cfgSprFar,   false, 1.0f },
	};
	for (const Num &n : nums) {
		if (n.autoVal) lstrcpyA(v, "Auto"); else formatHundredths(n.val, v);
		menuWriteKey(n.key, v, n.asAuto ? n.autoVal : fabsf(n.val - n.missing) < 0.005f);
	}
	if (g_auto[AUTO_XBR_CORNER]) lstrcpyA(v, "Auto"); else { v[0] = (char)('A' + g_xbrCorner); v[1] = 0; }
	menuWriteKey("XbrCorner", v, g_auto[AUTO_XBR_CORNER]);
	if (g_auto[AUTO_XBR_SLOPES]) lstrcpyA(v, "Auto"); else lstrcpyA(v, g_xbrSlopes ? "1" : "0");
	menuWriteKey("XbrSlopes", v, g_auto[AUTO_XBR_SLOPES]);

	const char *const sprKeys[SPR_LAYERS] = { "SpriteSharpness", "BackgroundSharpness" };   // 0 / missing = off
	for (int l = 0; l < SPR_LAYERS; l++) {
		if (g_cfgSprK[l] > 0.0f) formatHundredths(g_cfgSprK[l], v); else lstrcpyA(v, "0");
		menuWriteKey(sprKeys[l], v, g_cfgSprK[l] <= 0.0f);
	}
	wsprintfA(v, "%d", (int)g_msaaCfg);
	menuWriteKey("MultiSample", v, g_msaaCfg == 0);
}
