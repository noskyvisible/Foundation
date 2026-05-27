# Foundation

A small C++/OpenGL scene editor — a Hammer/Unity-style quad-view editor with a
separate play window, gizmos, an outliner/inspector, and GLB/FBX model loading.

## Features

- **Quad-view editor** — Perspective + Top/Front/Side orthographic viewports,
  each rendered to its own framebuffer; maximize one to fill the area (Space)
  or show the 2x2 grid.
- **Hammer-style visuals** — reference grid, wireframe ortho views / solid 3D,
  per-view labels, dark theme.
- **Play mode** — F5 opens a separate, fullscreen-able (F11) game window with
  its own GL context running a game camera; the editor stays static.
- **Gizmos** (ImGuizmo) — translate / rotate / scale (W/E/R), world/local,
  click-to-select with ray picking, an orange selection box.
- **Snap to grid** with an adjustable grid size (drives both the visible grid
  and the snap increment).
- **Scene model** — an outliner (add/delete/select objects) and a properties
  inspector (name, numeric transform, material colour).
- **Undo/redo** (Ctrl+Z / Ctrl+Y) and **copy/paste** (Ctrl+C / Ctrl+V).
- **Save / load** scenes to a plain-text `.fdn` file (Ctrl+S).
- **Cameras** — mouse-wheel zoom in any view; Unreal-style fly in Perspective
  (hold right mouse: WASD move, E/Q up/down, wheel = speed).
- **Lighting + materials** — directional light over surface normals, per-object
  colour.
- **Mesh loading** — import GLB / GLTF / FBX / OBJ via Assimp, with embedded
  base-colour textures (decoded with stb_image). Loads at the origin and is
  editable like any other object.

## Dependencies

All fetched automatically by CMake (`FetchContent`) on first configure — nothing
to install by hand:

- [GLFW](https://github.com/glfw/glfw) — window + OpenGL context + input
- [GLAD](https://github.com/Dav1dde/glad) — OpenGL 3.3 Core loader
- [GLM](https://github.com/g-truc/glm) — vector/matrix math
- [Dear ImGui](https://github.com/ocornut/imgui) (docking) — editor UI
- [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) — transform gizmos
- [Assimp](https://github.com/assimp/assimp) — model import (GLTF/FBX/OBJ)
- [stb_image](https://github.com/nothings/stb) — image decoding

## Building (Windows / MinGW)

Requires: a C++17 compiler (MinGW-w64 g++), CMake 3.16+, Python 3 (used by GLAD
to generate its loader), and git.

```sh
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
.\build\Foundation.exe
```

> The first configure/build downloads and compiles all dependencies, including
> Assimp, so it takes a few minutes. Subsequent builds are fast.

## Controls

| Action | Input |
| --- | --- |
| Select object | Left-click it (or pick in the Outliner) |
| Move / Rotate / Scale | `W` / `E` / `R`, then drag the gizmo |
| Maximize / restore the hovered view | `Space` |
| Fly (Perspective) | Hold **right mouse**: `WASD`, `E`/`Q` up/down, wheel = speed |
| Zoom a view | Mouse wheel |
| Undo / Redo | `Ctrl+Z` / `Ctrl+Y` |
| Copy / Paste | `Ctrl+C` / `Ctrl+V` |
| Save scene | `Ctrl+S` |
| Play / Stop | `F5` (in play window: `F11` fullscreen, `Esc` stop) |

Use **Load Mesh (GLB/FBX)** in the Outliner to import a model; put source assets
in `models/` and textures in `materials/`.

## Layout

```
src/            engine + editor source (main.cpp, platform_file, stb impl)
models/         model files (.glb/.fbx/.obj)
materials/      textures
CMakeLists.txt  build + dependency fetch
```
