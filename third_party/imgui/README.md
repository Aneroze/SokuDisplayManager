# Dear ImGui (vendored)

The in-game settings menu (`src/DisplayManagerMenu.h`) is built on [Dear ImGui](https://github.com/ocornut/imgui) (MIT,
`LICENSE.txt`), compiled into `DisplayManager.dll`.

**Version: v1.91.9b**, unmodified: only the files the build needs, copied from the release's source archive.

```
imgui.h  imgui.cpp  imgui_draw.cpp  imgui_tables.cpp  imgui_widgets.cpp
imgui_internal.h  imconfig.h  imstb_rectpack.h  imstb_textedit.h  imstb_truetype.h
backends/imgui_impl_dx9.h  backends/imgui_impl_dx9.cpp  LICENSE.txt
```

The builds (`build.bat`, `build.sh`, `CMakeLists.txt`) configure it with defines instead of edits to `imconfig.h`:

- `IMGUI_DISABLE_WIN32_FUNCTIONS`: no default clipboard / IME handlers (the menu takes no text).
- `IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS`: no `ShellExecute`, so no `shell32.dll` import.
- `IMGUI_DISABLE_DEMO_WINDOWS`, `IMGUI_DISABLE_DEBUG_TOOLS`: a smaller DLL.
- `NDEBUG` (CMake's Release has it already): `IM_ASSERT` compiles out, so a failed check can't abort the game.

To update: replace the files above with a newer release's and check `menuBuildFont` / `menuRender`. 1.92 reworks the
font atlas and the backend's texture handling (fonts are then rasterized at any size on demand).
