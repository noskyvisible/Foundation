#pragma once
#include <glm/glm.hpp>

#include "imgui.h"      // ImGuizmo.h depends on ImGui types (ImVec2, ImU32, ...)
#include "ImGuizmo.h"

#include "camera.h"
#include "environment.h"
#include "mesh.h"
#include "renderer.h"
#include "scene.h"
#include "skinned.h"
#include "terrain.h"

#include <vector>

// Editing context for the active viewport: the scene + current selection index.
struct EditContext {
    std::vector<SceneObject>* scene = nullptr;
    const std::vector<MeshAsset>* lib = nullptr;          // static meshes (AABB picking)
    const std::vector<SkinnedMesh>* skinnedLib = nullptr; // skinned meshes (AABB picking)
    int* selected = nullptr;          // index into scene, -1 = none
    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE mode = ImGuizmo::WORLD;
    bool  snap = false;               // snap gizmo drags to increments
    float snapTranslate = 1.0f;       // world units (== grid size)
    float snapRotate = 15.0f;         // degrees
    float snapScale = 0.25f;
};

// One dockable panel that renders `cam`'s view of the scene into `rt`, with the
// image filling the panel edge-to-edge and the view label in the top-left. If
// `edit` is supplied, draws a gizmo for the selected object and handles
// click-to-select. Returns true if the viewport image was hovered this frame.
bool drawViewportPanel(const char* name, RenderTarget& rt, const Camera& cam,
                       const Renderer& r, const Environment& env, bool wireframe3D, bool showGrid,
                       const std::vector<glm::mat4>& models, const std::vector<glm::vec3>& colors,
                       const std::vector<int>& meshIds,
                       int selectedIndex, float gridSpacing,
                       const std::vector<SkinnedDrawItem>& skins = {},
                       EditContext* edit = nullptr,
                       Terrain* terrain = nullptr, TerrainBrush* brush = nullptr);
