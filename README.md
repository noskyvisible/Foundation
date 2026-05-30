# Foundation

A from-scratch **C++/OpenGL game engine** with a Hammer/Unity-style editor:
quad-view editing and a separate play window, transform gizmos, model and
skeletal-animation import, scripted NPCs with pathfinding, a dynamic day/night
sky with weather, and a sculptable heightmap terrain — all built on a GL 3.3
core renderer with no engine middleware.

## Download & run (Windows)

Grab the latest prebuilt build from the
[**Releases**](https://github.com/noskyvisible/Foundation/releases/latest) page —
no build tools required:

1. Download `Foundation-vX.Y.Z-win64.zip` and extract it.
2. Open the extracted `Foundation/` folder and double-click **`Foundation.exe`**.

The executable is statically linked (it uses only standard Windows system
libraries), so there's nothing to install — just keep `Foundation.exe` next to
the `materials/` and `models/` folders it ships with. To build from source
instead, see [Building](#building-windows--mingw).

## Features

### Editor
- **Quad-view** — Perspective + Top/Front/Side orthographic viewports, each
  rendered to its own framebuffer; maximize one (Space) or show the 2×2 grid.
- **Play mode** — `F5` opens a separate, fullscreen-able (`F11`) game window with
  its own GL context and a free-fly game camera; the editor keeps running.
- **Gizmos** (ImGuizmo) — translate / rotate / scale (`W`/`E`/`R`), world/local,
  click-to-select via ray picking, an orange selection box.
- **Snap to grid** with an adjustable grid size (drives the grid *and* the snap).
- **Outliner + inspector** — add/delete/select objects; edit name, transform,
  material colour, and (for NPCs) attributes and schedule.
- **Undo/redo** (`Ctrl+Z`/`Ctrl+Y`), **copy/paste** (`Ctrl+C`/`Ctrl+V`), and
  **save/load** scenes to a plain-text `.fdn` file (`Ctrl+S`).
- **Cameras** — wheel zoom in any view; Unreal-style fly in Perspective.

### Models & animation
- **Mesh import** — GLB / GLTF / FBX / OBJ via Assimp, with embedded base-colour
  textures (decoded with stb_image).
- **Skinned meshes** — GPU skinning with a per-frame bone palette; **skeletal
  animation** clips selected by movement state (idle / walk / run).

### NPCs & navigation
- **NPC templates** — attributes and needs (health/hunger/thirst/energy with
  per-hour rates), plus a daily **schedule** (HH:MM → activity → location).
- **Grid A\*** pathfinding; in play mode NPCs walk/run to their scheduled targets
  on the game clock, with the animation clip driven by their speed.

### Environment & sky
- **Game clock** with an adjustable day length and a full day/night cycle.
- **Physically-based atmosphere** — Rayleigh + Mie single-scattering raymarched
  per pixel (blue day, warm sunrise/sunset, dark night).
- **Volumetric clouds** — raymarched, wind-drifted, domain-warped with altitude
  variation; **night stars + moon**; sun-driven directional light + ambient.

### Weather
- States **Clear / Fair / Overcast / Rain / Thunderstorm** with smoothly-eased
  transitions and a natural, mean-reverting **auto-cycle** (or manual override).
- **World-anchored instanced rain** you actually move through (not glued to the
  camera), with per-drop variation, distance fade, and wind-driven slant.
- **Forked lightning** rendered procedurally in the sky during storms, plus a
  scene-lighting flash.
- **Exponential height fog** with a colour picker (darkens at night, warms at
  dawn/dusk) and density that scales with the weather; a matching sky horizon
  haze keeps ground and sky blended.

### Terrain
- Level-sized **heightmap** landscape with a **multi-texture splat blend**
  (sand / dirt / rock), lit and fogged like the rest of the scene.
- In-editor **sculpt brushes** — raise / lower / smooth / flatten — driven by a
  ray cast into the heightfield, with a brush cursor draped on the terrain.

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
| Sculpt terrain | **Terrain** tab → *Edit terrain*, then drag **left mouse** in Perspective |
| Undo / Redo | `Ctrl+Z` / `Ctrl+Y` |
| Copy / Paste | `Ctrl+C` / `Ctrl+V` |
| Save scene | `Ctrl+S` |
| Play / Stop | `F5` (in play window: `F11` fullscreen, `Esc` stop) |

Panels: **Outliner** (objects), **Controls / Environment / Terrain** (tabbed —
gizmo & grid, sky/weather/fog, terrain brushes), **Properties** and **NPC
Editor**. Import models with **Load Mesh** in the Outliner; put source models in
`models/` and textures in `materials/`.

## Layout

```
src/
  main.cpp                window/ImGui setup + per-frame loop
  camera / environment    view + projection; game clock, day/night, lighting
  weather / terrain       weather state machine; heightmap + sculpt + splat
  mesh / skinned / scene  static + skinned model loading; scene model + save/load
  navigation / npc_sim    grid A* pathfinding; scheduled NPC simulation
  renderer / shaders      GL 3.3 renderer; sky/cloud/rain/terrain/fog shaders
  editor                  dockable viewport panel (gizmos, picking, brushes)
  platform_file / stb_impl native file dialog; stb_image implementation
models/                   model files (.glb/.fbx/.obj)
materials/                terrain + object textures
CMakeLists.txt            build + dependency fetch
```
