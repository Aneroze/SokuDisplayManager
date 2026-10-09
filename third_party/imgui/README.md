# Dear ImGui (vendored)

The in-game settings menu (`src/DisplayManagerMenu.h`) is built on [Dear ImGui](https://github.com/ocornut/imgui) (MIT),
compiled into `DisplayManager.dll`.

Copy these files from a release of the repository into this folder, keeping its layout:

```
imgui.h  imgui.cpp  imgui_draw.cpp  imgui_tables.cpp  imgui_widgets.cpp
imgui_internal.h  imconfig.h  imstb_rectpack.h  imstb_textedit.h  imstb_truetype.h
backends/imgui_impl_dx9.h  backends/imgui_impl_dx9.cpp
```

Version: the menu uses `ImGui::BeginDisabled`, `io.AddMousePosEvent` and `io.FontGlobalScale`, so it needs 1.87 or newer.
Use the latest 1.91.x release: `io.FontGlobalScale` is replaced by `style.FontScaleMain` in 1.92 (`menuRender` would need the
one-line change). `imgui_demo.cpp` is not needed. Keep `LICENSE.txt` next to the files.
