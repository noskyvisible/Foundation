// Editor shell: Hammer-style quad-view + play window + gizmos. main.cpp now
// holds only window/ImGui setup and the per-frame loop; the engine systems live
// in their own modules (mesh, skinned, camera, environment, scene, renderer,
// editor, navigation, npc_sim).
//
// WebGL parallels:
//   - An FBO is like an offscreen WebGL framebuffer we render into, then draw
//     as a texture into a UI region. We have one per viewport (see renderer.*).
//   - A Camera bundles the view + projection matrices (see camera.*).
//   - ImGui is "immediate mode": we rebuild the entire UI every frame inside
//     the render loop instead of creating persistent panel objects.

#include <glad/gl.h>      // before GLFW: defines the GL types/functions.
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder API for the default layout
#include "imgui_stdlib.h"     // ImGui::InputText with std::string
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "ImGuizmo.h"         // translate/rotate/scale gizmos

#include "camera.h"
#include "editor.h"
#include "environment.h"
#include "mesh.h"
#include "navigation.h"
#include "npc_sim.h"
#include "renderer.h"
#include "scene.h"
#include "skinned.h"

#include "platform_file.h"    // native file-open dialog

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

int main() {
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to init GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Foundation - Editor", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);   // vsync

    if (!gladLoadGL(glfwGetProcAddress)) {
        std::fprintf(stderr, "Failed to load OpenGL via GLAD\n");
        glfwTerminate();
        return 1;
    }

    // ---- ImGui setup ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;   // enable dockable panels
    // Don't load/save imgui.ini while we're still changing the panel set: a
    // stale layout file silently overrides our DockBuilder default and leaves
    // newly-renamed panels floating/collapsed. Build a fresh layout each launch.
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    // Squared-off, darker Hammer-ish theme.
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = style.FrameRounding = style.TabRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.Colors[ImGuiCol_WindowBg]       = ImVec4(0.10f, 0.10f, 0.11f, 1.0f);
    style.Colors[ImGuiCol_TitleBg]        = ImVec4(0.08f, 0.08f, 0.09f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive]  = ImVec4(0.16f, 0.16f, 0.19f, 1.0f);
    style.Colors[ImGuiCol_Tab]            = ImVec4(0.10f, 0.10f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_TabActive]      = ImVec4(0.20f, 0.20f, 0.24f, 1.0f);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Editor's own scene resources (built in the editor GL context).
    Renderer renderer = createRenderer();

    bool  wireframe3D = false;  // 3D view solid by default; ortho always wireframe
    bool  showGrid    = true;
    bool  snapEnabled = false;  // snap gizmo drags to the grid
    float gridSize    = 1.0f;   // grid spacing AND translate snap increment

    Environment env;            // game clock + day/night sky + sun light

    // Mesh library: asset #0 is the built-in cube; loaded models append. Each
    // Renderer uploads these into its own GL context lazily (syncMeshes).
    std::vector<MeshAsset> meshLibrary;
    meshLibrary.push_back(buildCubeMesh());

    // Skinned (animated) mesh library: parallel to meshLibrary but for rigged
    // GLB characters. A SceneObject is skinned when skinnedId >= 0.
    std::vector<SkinnedMesh> skinnedLibrary;

    // NPC templates (reusable types). Instances are SceneObjects whose
    // npcTemplate indexes into this list.
    std::vector<NPCTemplate> npcTemplates;
    int selectedTemplate = -1;

    // The scene is a list of objects referencing mesh assets. The gizmo edits
    // the selected object's transform.
    std::vector<SceneObject> scene;
    {
        // Start by loading models/testModel.glb (try a few cwd-relative paths).
        const char* candidates[] = { "models/testModel.glb", "../models/testModel.glb", "testModel.glb" };
        MeshAsset asset;
        bool loaded = false;
        for (const char* c : candidates) if (loadModel(c, asset)) { loaded = true; break; }
        if (loaded) {
            meshLibrary.push_back(std::move(asset));
            SceneObject o;
            o.name   = meshLibrary.back().name;
            o.meshId = (int)meshLibrary.size() - 1;
            o.color  = glm::vec3(1.0f);
            scene.push_back(o);
        } else {
            std::fprintf(stderr, "Could not load testModel.glb; starting with a cube.\n");
            scene.push_back({"Cube", glm::mat4(1.0f)});
        }
    }

    // Load the rigged demo character (Scarlet Heigns) and drop one instance next
    // to the test model so she's visible/animating on launch.
    {
        const char* candidates[] = {
            "models/Scarlet Heigns/scarletHeigns.glb",
            "../models/Scarlet Heigns/scarletHeigns.glb",
            "scarletHeigns.glb",
        };
        SkinnedMesh sm;
        bool loaded = false;
        for (const char* c : candidates) if (loadSkinnedModel(c, sm)) { loaded = true; break; }
        if (loaded) {
            std::fprintf(stderr, "Loaded skinned '%s': %zu bones, %zu clips, bounds min(%.1f,%.1f,%.1f) max(%.1f,%.1f,%.1f)\n",
                         sm.name.c_str(), sm.boneOffsets.size(), sm.animations.size(),
                         sm.bounds.min.x, sm.bounds.min.y, sm.bounds.min.z,
                         sm.bounds.max.x, sm.bounds.max.y, sm.bounds.max.z);
            std::fflush(stderr);
            skinnedLibrary.push_back(std::move(sm));
            SceneObject o;
            o.name      = skinnedLibrary.back().name;
            o.skinnedId = (int)skinnedLibrary.size() - 1;
            o.meshId    = -1;
            o.color     = glm::vec3(1.0f);
            o.transform = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f));
            scene.push_back(o);
        } else {
            std::fprintf(stderr, "Could not load scarletHeigns.glb (no skinned demo).\n");
        }
    }
    int selected = scene.empty() ? -1 : 0;   // index into scene, -1 = none
    ImGuizmo::OPERATION gizmoOp = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE      gizmoMode = ImGuizmo::WORLD;

    // Undo/redo: snapshot the whole (tiny) scene before each change. Copy/paste
    // holds one object on a clipboard.
    std::vector<std::vector<SceneObject>> undoStack, redoStack;
    SceneObject clipboard;
    bool hasClipboard = false;
    bool prevUsingGizmo = false;   // edge-detect gizmo drag start for undo

    const char* kScenePath = "scene.fdn";
    std::string sceneStatus;       // last save/load result, shown in Outliner

    // Play-mode state. Play opens a SEPARATE OS window with its own GL context
    // running the game; the editor stays static.
    GLFWwindow* playWindow   = nullptr;
    Renderer    playRenderer{};
    // NPC simulation lives only while playing. The scene is snapshotted on Play
    // and restored on Stop, so NPC movement doesn't disturb the authored scene.
    std::vector<SceneObject> sceneBackup;
    std::vector<NPCRuntime>  npcRuntimes;
    NavGrid                  navGrid;
    bool        prevF5       = false;  // edge-detect the F5 toggle (editor window)
    bool        prevPlayF11  = false;  // edge-detect F11 (play window fullscreen)
    bool        playFullscreen = false;
    bool        playLooking  = false;  // RMB-held mouselook in the play window
    double      playLastMouseX = 0.0, playLastMouseY = 0.0;
    int         playWinX = 0, playWinY = 0, playWinW = 1280, playWinH = 720;
    double      lastTime = glfwGetTime();
    float       animClock = 0.0f;      // always-running clock that drives skinned playback

    // Game camera used by the play window: a simple perspective view.
    Camera gameCam;
    gameCam.type = Projection::Perspective;
    gameCam.eye  = {0.0f, 1.2f, 3.5f};
    gameCam.fovDeg = 60.0f;
    gameCam.gridModel = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1, 0, 0));

    // Open/close the play window. createRenderer/destroyRenderer must run with
    // the play window's context current (VAOs are per-context).
    auto openPlay = [&]() {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        playWindow = glfwCreateWindow(playWinW, playWinH, "Foundation - Play", nullptr, nullptr);
        if (!playWindow) { std::fprintf(stderr, "Failed to create play window\n"); return; }
        glfwMakeContextCurrent(playWindow);
        glfwSwapInterval(1);
        playRenderer = createRenderer();
        glfwMakeContextCurrent(window);   // restore editor context
        playFullscreen = false;

        // Seed the free-fly state from the authored eye/target so WASD + RMB
        // mouselook start where the camera is pointing.
        gameCam.flyPos = gameCam.eye;
        glm::vec3 d0 = glm::normalize(gameCam.target - gameCam.eye);
        gameCam.yaw   = std::atan2(d0.x, -d0.z);
        gameCam.pitch = std::asin(glm::clamp(d0.y, -1.0f, 1.0f));
        playLooking = false;

        // Snapshot the scene, build a nav grid from the static objects, and seed
        // live NPC state. Stop restores the snapshot.
        sceneBackup = scene;
        buildNavFromScene(navGrid, scene, npcTemplates, meshLibrary, skinnedLibrary);
        seedNPCs(npcRuntimes, scene, npcTemplates);
    };
    auto closePlay = [&]() {
        glfwMakeContextCurrent(playWindow);
        destroyRenderer(playRenderer);
        glfwMakeContextCurrent(window);
        glfwDestroyWindow(playWindow);
        playWindow = nullptr;
        playLooking = false;
        // Restore the authored scene; discard runtime NPC movement.
        if (!sceneBackup.empty()) scene = sceneBackup;
        sceneBackup.clear();
        npcRuntimes.clear();
    };

    // Four viewports: a perspective 3/4 view plus three orthographic views
    // looking straight down each world axis. gridModel rotates the XY grid mesh
    // into each view's plane (perspective uses the XZ ground plane).
    RenderTarget rtPersp, rtTop, rtFront, rtRight;
    glm::mat4 toXZ = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1, 0, 0));
    glm::mat4 toYZ = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 1, 0));

    Camera camPersp;
    camPersp.type = Projection::Perspective;
    camPersp.eye = {2.5f, 2.0f, 3.0f};
    camPersp.gridModel = toXZ;          // ground plane
    camPersp.label = "3D camera";
    // Seed fly state so it initially looks at the origin.
    {
        camPersp.flyPos = camPersp.eye;
        glm::vec3 d0 = glm::normalize(camPersp.target - camPersp.eye);
        camPersp.yaw   = std::atan2(d0.x, -d0.z);
        camPersp.pitch = std::asin(d0.y);
    }
    bool flying = false;   // true while right-mouse fly is active in Perspective

    Camera camTop;   // looking straight down -Y; "up" can't be Y, so use -Z
    camTop.type = Projection::Ortho;
    camTop.eye = {0, 3, 0};
    camTop.up  = {0, 0, -1};
    camTop.gridModel = toXZ;
    camTop.label = "top (x/z)";

    Camera camFront; // looking down -Z from +Z
    camFront.type = Projection::Ortho;
    camFront.eye = {0, 0, 3};
    camFront.label = "front (x/y)";     // grid stays in XY plane (identity)

    Camera camRight; // looking down -X from +X
    camRight.type = Projection::Ortho;
    camRight.eye = {3, 0, 0};
    camRight.gridModel = toYZ;
    camRight.label = "side (z/y)";

    // Central-area layout: either the 2x2 quad grid, or one viewport maximized
    // to fill the area (NOT OS fullscreen -- the Controls panel still shows).
    // Engine starts with the Perspective view maximized.
    const char* viewName[4] = {"Perspective", "Top", "Front", "Right"};
    bool layoutSingle = true;     // true = one maximized view, false = quad grid
    int  activeView   = 0;        // index into viewName when maximized
    bool layoutDirty  = true;     // rebuild the dock layout next frame

    auto rebuildLayout = [&](ImGuiID dockId) {
        ImGui::DockBuilderRemoveNode(dockId);
        ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->Size);
        ImGuiID mainId = dockId;
        ImGuiID leftId  = ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Left, 0.18f, nullptr, &mainId);
        ImGuiID rightId = ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Right, 0.22f, nullptr, &mainId);
        ImGuiID controlsId;
        ImGuiID outlinerId = ImGui::DockBuilderSplitNode(leftId, ImGuiDir_Up, 0.4f, nullptr, &controlsId);
        ImGui::DockBuilderDockWindow("Outliner", outlinerId);
        ImGui::DockBuilderDockWindow("Controls", controlsId);
        ImGui::DockBuilderDockWindow("Environment", controlsId);   // tabbed with Controls
        ImGui::DockBuilderDockWindow("Properties", rightId);
        ImGui::DockBuilderDockWindow("NPC Editor", rightId);   // tabbed with Properties
        if (layoutSingle) {
            ImGui::DockBuilderDockWindow(viewName[activeView], mainId);
        } else {
            ImGuiID bottomId;
            ImGuiID topId = ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Up, 0.5f, nullptr, &bottomId);
            ImGuiID trId, brId;
            ImGuiID tlId = ImGui::DockBuilderSplitNode(topId, ImGuiDir_Left, 0.5f, nullptr, &trId);
            ImGuiID blId = ImGui::DockBuilderSplitNode(bottomId, ImGuiDir_Left, 0.5f, nullptr, &brId);
            ImGui::DockBuilderDockWindow("Perspective", tlId);
            ImGui::DockBuilderDockWindow("Top", trId);
            ImGui::DockBuilderDockWindow("Front", blId);
            ImGui::DockBuilderDockWindow("Right", brId);
        }
        ImGui::DockBuilderFinish(dockId);
    };

    auto clampSelected = [&]() {
        if (selected >= (int)scene.size()) selected = (int)scene.size() - 1;
    };
    auto snapshot = [&]() {            // record current scene for undo
        undoStack.push_back(scene);
        redoStack.clear();
        if (undoStack.size() > 64) undoStack.erase(undoStack.begin());
    };
    auto doUndo = [&]() {
        if (undoStack.empty()) return;
        redoStack.push_back(scene);
        scene = undoStack.back(); undoStack.pop_back();
        clampSelected();
    };
    auto doRedo = [&]() {
        if (redoStack.empty()) return;
        undoStack.push_back(scene);
        scene = redoStack.back(); redoStack.pop_back();
        clampSelected();
    };
    auto doCopy = [&]() {
        if (selected >= 0 && selected < (int)scene.size()) { clipboard = scene[selected]; hasClipboard = true; }
    };
    auto doPaste = [&]() {
        if (!hasClipboard) return;
        snapshot();
        SceneObject o = clipboard;
        float off = gridSize > 0.0f ? gridSize : 1.0f;
        o.transform = glm::translate(glm::mat4(1.0f), glm::vec3(off, 0.0f, off)) * o.transform;
        o.name += " copy";
        scene.push_back(o);
        selected = (int)scene.size() - 1;
    };
    auto doAddCube = [&]() {
        snapshot();
        static const glm::vec3 palette[6] = {
            {0.85f,0.35f,0.35f}, {0.4f,0.7f,0.4f}, {0.4f,0.55f,0.9f},
            {0.9f,0.8f,0.35f},   {0.7f,0.45f,0.85f}, {0.4f,0.8f,0.8f},
        };
        SceneObject o;
        o.name = "Cube " + std::to_string(scene.size());
        o.transform = glm::translate(glm::mat4(1.0f), glm::vec3((float)scene.size() * 1.5f, 0.0f, 0.0f));
        o.color = palette[scene.size() % 6];
        scene.push_back(o);
        selected = (int)scene.size() - 1;
    };
    auto doDelete = [&]() {
        if (selected < 0 || selected >= (int)scene.size()) return;
        snapshot();
        scene.erase(scene.begin() + selected);
        if (selected >= (int)scene.size()) selected = (int)scene.size() - 1;
    };
    auto doSave = [&]() {
        sceneStatus = saveScene(scene, meshLibrary, skinnedLibrary, kScenePath)
                          ? "Saved scene.fdn" : "Save FAILED";
    };
    auto doLoad = [&]() {
        std::vector<SceneObject> loaded;
        std::vector<MeshAsset>   loadedMesh;
        std::vector<SkinnedMesh> loadedSkin;
        if (loadScene(loaded, loadedMesh, loadedSkin, kScenePath)) {
            snapshot();
            scene = std::move(loaded);
            // v3 files rebuild the libraries; v2 leaves them empty (keep ours).
            if (!loadedMesh.empty()) meshLibrary    = std::move(loadedMesh);
            if (!loadedSkin.empty()) skinnedLibrary = std::move(loadedSkin);
            invalidateGPUMeshes(renderer);   // editor context is current here
            selected = scene.empty() ? -1 : 0;
            sceneStatus = "Loaded scene.fdn";
        } else sceneStatus = "Load FAILED (no scene.fdn?)";
    };
    auto doLoadMesh = [&]() {
        std::string path = openModelFileDialog("models");
        if (path.empty()) return;
        MeshAsset asset;
        if (loadModel(path, asset)) {
            snapshot();
            meshLibrary.push_back(std::move(asset));
            SceneObject o;
            o.name   = meshLibrary.back().name;
            o.meshId = (int)meshLibrary.size() - 1;
            o.color  = glm::vec3(1.0f);
            scene.push_back(o);
            selected = (int)scene.size() - 1;
            sceneStatus = "Loaded " + o.name;
        } else sceneStatus = "Mesh load FAILED";
    };
    // Place an instance of an already-loaded skinned mesh into the scene.
    auto doPlaceSkinned = [&](int sid) {
        if (sid < 0 || sid >= (int)skinnedLibrary.size()) return;
        snapshot();
        SceneObject o;
        o.name      = skinnedLibrary[sid].name;
        o.skinnedId = sid;
        o.meshId    = -1;
        o.color     = glm::vec3(1.0f);
        scene.push_back(o);
        selected = (int)scene.size() - 1;
    };
    // Import a rigged GLB/FBX into the skinned library and place one instance.
    auto doLoadSkinned = [&]() {
        std::string path = openModelFileDialog("models");
        if (path.empty()) return;
        SkinnedMesh sm;
        if (loadSkinnedModel(path, sm)) {
            skinnedLibrary.push_back(std::move(sm));
            sceneStatus = "Loaded skinned " + skinnedLibrary.back().name;
            doPlaceSkinned((int)skinnedLibrary.size() - 1);   // snapshots + selects
        } else sceneStatus = "Skinned load FAILED";
    };

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, true);

        // The play window can be closed via its own [X]; treat that as Stop.
        if (playWindow && glfwWindowShouldClose(playWindow)) closePlay();

        // F5 (editor) toggles the play window open/closed (edge-detected).
        bool f5 = glfwGetKey(window, GLFW_KEY_F5) == GLFW_PRESS;
        if (f5 && !prevF5) { if (playWindow) closePlay(); else openPlay(); }
        prevF5 = f5;

        // Frame timing.
        double now = glfwGetTime();
        float  dt  = (float)(now - lastTime);
        lastTime   = now;
        animClock += dt;                                     // skinned preview always plays
        updateEnvironment(env, dt, playWindow != nullptr);   // clock ticks only in play mode

        // Layer 3: scheduled NPC simulation (play mode only). Walks placed NPCs
        // along A* paths to their scheduled locations and updates their needs.
        if (playWindow)
            simulateNPCs(npcRuntimes, scene, npcTemplates, navGrid, env, dt);

        // ---- Editor render (editor GL context) ----
        glfwMakeContextCurrent(window);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();

        // W/E/R switch gizmo operation (Unity-style) -- but not while flying,
        // where WASD drives the camera instead.
        if (!io.WantTextInput && !flying) {
            if (ImGui::IsKeyPressed(ImGuiKey_W, false)) gizmoOp = ImGuizmo::TRANSLATE;
            if (ImGui::IsKeyPressed(ImGuiKey_E, false)) gizmoOp = ImGuizmo::ROTATE;
            if (ImGui::IsKeyPressed(ImGuiKey_R, false)) gizmoOp = ImGuizmo::SCALE;
        }

        // Ctrl shortcuts: Z undo, Y redo, C copy, V paste, S save.
        if (io.KeyCtrl && !io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) doUndo();
            if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) doRedo();
            if (ImGui::IsKeyPressed(ImGuiKey_C, false)) doCopy();
            if (ImGui::IsKeyPressed(ImGuiKey_V, false)) doPaste();
            if (ImGui::IsKeyPressed(ImGuiKey_S, false)) doSave();
        }

        // Derive the perspective camera's eye/target from its fly state.
        {
            glm::vec3 f = forwardFromYawPitch(camPersp.yaw, camPersp.pitch);
            camPersp.eye = camPersp.flyPos;
            camPersp.target = camPersp.flyPos + f;
            camPersp.up = glm::vec3(0, 1, 0);
        }

        // ---- Top menu bar ----
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Save", "Ctrl+S"))            doSave();
                if (ImGui::MenuItem("Load"))                      doLoad();
                if (ImGui::MenuItem("Load Mesh (GLB/FBX)..."))    doLoadMesh();
                if (ImGui::MenuItem("Load Skinned Mesh (GLB)...")) doLoadSkinned();
                ImGui::Separator();
                if (ImGui::MenuItem("Exit"))                      glfwSetWindowShouldClose(window, true);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undoStack.empty())) doUndo();
                if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redoStack.empty())) doRedo();
                ImGui::Separator();
                bool hasSel = selected >= 0 && selected < (int)scene.size();
                if (ImGui::MenuItem("Copy",   "Ctrl+C", false, hasSel))        doCopy();
                if (ImGui::MenuItem("Paste",  "Ctrl+V", false, hasClipboard))  doPaste();
                if (ImGui::MenuItem("Delete", nullptr,  false, hasSel))        doDelete();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Create")) {
                if (ImGui::MenuItem("Cube"))                   doAddCube();
                if (ImGui::MenuItem("Mesh (GLB/FBX)..."))      doLoadMesh();
                if (ImGui::MenuItem("Skinned Mesh (GLB)..."))  doLoadSkinned();
                if (!skinnedLibrary.empty() && ImGui::BeginMenu("Place Skinned")) {
                    for (int s = 0; s < (int)skinnedLibrary.size(); ++s) {
                        ImGui::PushID(s);
                        if (ImGui::MenuItem(skinnedLibrary[s].name.c_str())) doPlaceSkinned(s);
                        ImGui::PopID();
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Layout")) {
                if (ImGui::MenuItem("Quad view", "Space", !layoutSingle)) { layoutSingle = false; layoutDirty = true; }
                ImGui::Separator();
                for (int i = 0; i < 4; ++i)
                    if (ImGui::MenuItem(viewName[i], nullptr, layoutSingle && activeView == i))
                        { activeView = i; layoutSingle = true; layoutDirty = true; }
                ImGui::Separator();
                ImGui::MenuItem("Show grid",         nullptr, &showGrid);
                ImGui::MenuItem("Wireframe 3D view", nullptr, &wireframe3D);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Play")) {
                if (ImGui::MenuItem(playWindow ? "Stop" : "Play", "F5")) { if (playWindow) closePlay(); else openPlay(); }
                ImGui::EndMenu();
            }
            // Right-aligned clock + mode indicator.
            int hh = (int)env.timeOfDay;
            int mm = (int)((env.timeOfDay - hh) * 60.0f);
            ImGui::SameLine(ImGui::GetWindowWidth() - 230.0f);
            ImGui::Text("Day %d  %02d:%02d", env.day, hh, mm);
            ImGui::SameLine();
            ImGui::TextColored(playWindow ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                               playWindow ? "  PLAYING" : "  EDIT");
            ImGui::EndMainMenuBar();
        }

        // Full-window dockspace so panels can be docked/tabbed/resized.
        ImGuiID dockId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        if (layoutDirty) { rebuildLayout(dockId); layoutDirty = false; }

        // Editor views show each object at its authored transform (the gizmo
        // edits the selected one); the simulation spin runs only in the play
        // window. The gizmo + click-select live in the Perspective view.
        EditContext edit;
        edit.scene = &scene;
        edit.lib = &meshLibrary;
        edit.skinnedLib = &skinnedLibrary;
        edit.selected = &selected;
        edit.op = gizmoOp;
        edit.mode = gizmoMode;
        edit.snap = snapEnabled;
        edit.snapTranslate = gridSize;

        syncMeshes(renderer, meshLibrary);       // upload any newly-loaded meshes (editor context)
        syncSkinned(renderer, skinnedLibrary);   // ...and skinned meshes
        std::vector<glm::mat4> editorModels;
        std::vector<glm::vec3> editorColors;
        std::vector<int>       editorMeshIds;
        editorModels.reserve(scene.size());
        editorColors.reserve(scene.size());
        editorMeshIds.reserve(scene.size());
        for (const SceneObject& o : scene) {
            editorModels.push_back(o.transform);
            editorColors.push_back(o.color);
            // Skinned objects draw via the skin path; -1 makes the static loop skip them.
            editorMeshIds.push_back(o.skinnedId >= 0 ? -1 : o.meshId);
        }

        // Build the skinned draw list: one item per skinned scene object, each with
        // a bone palette solved from its clip at the current preview time. Palettes
        // live in editorPalettes (reserved up-front so its storage never moves while
        // we take pointers into it).
        std::vector<SkinnedDrawItem>          editorSkins;
        std::vector<std::vector<glm::mat4>>   editorPalettes;
        editorPalettes.reserve(scene.size());
        for (int i = 0; i < (int)scene.size(); ++i) {
            const SceneObject& o = scene[i];
            if (o.skinnedId < 0 || o.skinnedId >= (int)skinnedLibrary.size()) continue;
            const SkinnedMesh& sm = skinnedLibrary[o.skinnedId];
            editorPalettes.emplace_back();
            if (!sm.animations.empty()) {
                int ci = glm::clamp(o.animClip, 0, (int)sm.animations.size() - 1);
                computeBoneMatrices(sm, sm.animations[ci], animClock, editorPalettes.back());
            } else {
                computeBoneMatrices(sm, Animation{}, 0.0f, editorPalettes.back());  // bind pose
            }
            SkinnedDrawItem it;
            it.skinnedId  = o.skinnedId;
            it.sceneIndex = i;
            it.model      = o.transform;
            it.color      = o.color;
            editorSkins.push_back(it);
        }
        for (size_t k = 0; k < editorSkins.size(); ++k)
            editorSkins[k].palette = &editorPalettes[k];

        // Snapshot before any gizmo edit this frame, so a drag that starts now
        // can be undone back to this pre-drag state.
        std::vector<SceneObject> frameStartScene = scene;

        // Submit only the viewports the current layout shows; track which one
        // the mouse is over so Space can maximize/restore it.
        int hoveredView = -1;
        auto drawView = [&](int i) -> bool {
            switch (i) {
                case 0: return drawViewportPanel("Perspective", rtPersp, camPersp, renderer, env, wireframe3D, showGrid, editorModels, editorColors, editorMeshIds, selected, gridSize, editorSkins, &edit);
                case 1: return drawViewportPanel("Top",   rtTop,   camTop,   renderer, env, wireframe3D, showGrid, editorModels, editorColors, editorMeshIds, selected, gridSize, editorSkins);
                case 2: return drawViewportPanel("Front", rtFront, camFront, renderer, env, wireframe3D, showGrid, editorModels, editorColors, editorMeshIds, selected, gridSize, editorSkins);
                default:return drawViewportPanel("Right", rtRight, camRight, renderer, env, wireframe3D, showGrid, editorModels, editorColors, editorMeshIds, selected, gridSize, editorSkins);
            }
        };
        if (layoutSingle) {
            if (drawView(activeView)) hoveredView = activeView;
        } else {
            for (int i = 0; i < 4; ++i) if (drawView(i)) hoveredView = i;
        }
        // One undo entry per gizmo drag: record the pre-drag scene on the frame
        // the manipulation starts.
        bool usingGizmo = ImGuizmo::IsUsing();
        if (usingGizmo && !prevUsingGizmo) {
            undoStack.push_back(frameStartScene);
            redoStack.clear();
            if (undoStack.size() > 64) undoStack.erase(undoStack.begin());
        }
        prevUsingGizmo = usingGizmo;

        // Space toggles maximize: in quad, maximize the hovered view; in single,
        // return to the quad grid.
        if (!io.WantTextInput && !flying && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            if (layoutSingle)            { layoutSingle = false; layoutDirty = true; }
            else if (hoveredView >= 0)   { activeView = hoveredView; layoutSingle = true; layoutDirty = true; }
        }

        // ---- Camera navigation ----
        // Right-mouse over the Perspective view = Unreal-style flythrough; the
        // mouse wheel zooms whichever view is hovered (ortho zoom / persp dolly).
        {
            Camera* cams[4] = {&camPersp, &camTop, &camFront, &camRight};
            bool rmb = ImGui::IsMouseDown(ImGuiMouseButton_Right);
            if (!flying && rmb && hoveredView == 0) {
                flying = true;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);  // lock + hide
            } else if (flying && !rmb) {
                flying = false;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }

            if (flying) {
                camPersp.yaw   += io.MouseDelta.x * 0.0025f;
                camPersp.pitch -= io.MouseDelta.y * 0.0025f;          // invert Y
                camPersp.pitch  = glm::clamp(camPersp.pitch, -1.54f, 1.54f);
                if (io.MouseWheel != 0.0f)                            // wheel = fly speed
                    camPersp.flySpeed = glm::clamp(camPersp.flySpeed * (1.0f + 0.1f * io.MouseWheel), 0.25f, 60.0f);
                glm::vec3 f   = forwardFromYawPitch(camPersp.yaw, camPersp.pitch);
                glm::vec3 rgt = glm::normalize(glm::cross(f, glm::vec3(0, 1, 0)));
                glm::vec3 wup(0, 1, 0);
                float v = camPersp.flySpeed * dt;
                if (ImGui::IsKeyDown(ImGuiKey_W)) camPersp.flyPos += f   * v;
                if (ImGui::IsKeyDown(ImGuiKey_S)) camPersp.flyPos -= f   * v;
                if (ImGui::IsKeyDown(ImGuiKey_D)) camPersp.flyPos += rgt * v;
                if (ImGui::IsKeyDown(ImGuiKey_A)) camPersp.flyPos -= rgt * v;
                if (ImGui::IsKeyDown(ImGuiKey_E)) camPersp.flyPos += wup * v;
                if (ImGui::IsKeyDown(ImGuiKey_Q)) camPersp.flyPos -= wup * v;
            } else if (io.MouseWheel != 0.0f && hoveredView >= 0) {
                Camera* c = cams[hoveredView];
                if (c->type == Projection::Ortho) {
                    c->orthoHalfHeight = glm::clamp(c->orthoHalfHeight * (1.0f - 0.1f * io.MouseWheel), 0.05f, 50.0f);
                } else {
                    glm::vec3 f = forwardFromYawPitch(c->yaw, c->pitch);  // dolly = zoom
                    c->flyPos += f * (io.MouseWheel * 0.4f);
                }
            }
        }

        // --- Outliner: scene object list + add/delete ---
        ImGui::Begin("Outliner");
        if (ImGui::Button("Add Cube"))   doAddCube();
        ImGui::SameLine();
        ImGui::BeginDisabled(selected < 0 || selected >= (int)scene.size());
        if (ImGui::Button("Delete"))     doDelete();
        ImGui::EndDisabled();
        ImGui::Separator();
        for (int i = 0; i < (int)scene.size(); ++i) {
            if (ImGui::Selectable(scene[i].name.c_str(), selected == i))
                selected = i;
        }
        if (!sceneStatus.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", sceneStatus.c_str());
        }
        ImGui::End();

        // --- Controls: tools + display settings + help ---
        ImGui::Begin("Controls");
        ImGui::SeparatorText("Gizmo");
        if (ImGui::RadioButton("Move (W)",   gizmoOp == ImGuizmo::TRANSLATE)) gizmoOp = ImGuizmo::TRANSLATE;
        if (ImGui::RadioButton("Rotate (E)", gizmoOp == ImGuizmo::ROTATE))    gizmoOp = ImGuizmo::ROTATE;
        if (ImGui::RadioButton("Scale (R)",  gizmoOp == ImGuizmo::SCALE))     gizmoOp = ImGuizmo::SCALE;
        bool worldMode = (gizmoMode == ImGuizmo::WORLD);
        if (ImGui::Checkbox("World space", &worldMode))
            gizmoMode = worldMode ? ImGuizmo::WORLD : ImGuizmo::LOCAL;

        ImGui::SeparatorText("Grid");
        ImGui::Checkbox("Show grid", &showGrid);
        ImGui::Checkbox("Snap to grid", &snapEnabled);
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::DragFloat("Grid size", &gridSize, 0.05f, 0.05f, 16.0f, "%.2f"))
            gridSize = glm::clamp(gridSize, 0.05f, 16.0f);

        ImGui::SeparatorText("Stats");
        ImGui::Text("%.1f FPS  (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);

        ImGui::SeparatorText("Help");
        ImGui::TextWrapped("Wheel zooms the hovered view. In Perspective hold RIGHT mouse to fly (WASD, E/Q up/down, wheel = speed). Space maximizes the hovered view.");
        ImGui::End();

        // --- Environment: time of day + day/night ---
        ImGui::Begin("Environment");
        int hh = (int)env.timeOfDay;
        int mm = (int)((env.timeOfDay - hh) * 60.0f);
        ImGui::Text("Day %d   %02d:%02d", env.day, hh, mm);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##time", &env.timeOfDay, 0.0f, 24.0f, "%.2f h");
        ImGui::Checkbox("Clock runs in Play", &env.running);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("Day length (s)", &env.dayLengthSec, 10.0f, 3600.0f, "%.0f");
        ImGui::TextDisabled("Real seconds per full 24h day.");

        ImGui::SeparatorText("Sky");
        ImGui::SliderFloat("Cloud cover", &env.cloudCover, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Exposure",    &env.exposure,   0.2f, 3.0f, "%.2f");
        ImGui::TextDisabled("Physically-based atmosphere; clouds drift on their own.");

        ImGui::Separator();
        ImGui::TextWrapped("Time only advances in Play mode (F5). In the editor, drag the time slider to preview the sky. NPC schedules use this clock.");
        ImGui::End();

        // --- Properties: inspector for the selected object (right side) ---
        ImGui::Begin("Properties");
        if (selected >= 0 && selected < (int)scene.size()) {
            SceneObject& o = scene[selected];
            bool propActivated = false;   // snapshot for undo on the first edit

            ImGui::InputText("Name", &o.name);
            propActivated |= ImGui::IsItemActivated();

            ImGui::SeparatorText("Transform");
            float t[3], rot[3], s[3];
            ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(o.transform), t, rot, s);
            bool changed = false;
            changed |= ImGui::DragFloat3("Position", t,   0.05f);
            propActivated |= ImGui::IsItemActivated();
            changed |= ImGui::DragFloat3("Rotation", rot, 0.5f);
            propActivated |= ImGui::IsItemActivated();
            changed |= ImGui::DragFloat3("Scale",    s,   0.05f);
            propActivated |= ImGui::IsItemActivated();
            if (changed)
                ImGuizmo::RecomposeMatrixFromComponents(t, rot, s, glm::value_ptr(o.transform));

            ImGui::SeparatorText("Material");
            ImGui::ColorEdit3("Color", glm::value_ptr(o.color));
            propActivated |= ImGui::IsItemActivated();
            ImGui::TextDisabled("Texture: drop files in materials/ (coming soon)");

            if (o.skinnedId >= 0 && o.skinnedId < (int)skinnedLibrary.size()) {
                const SkinnedMesh& sm = skinnedLibrary[o.skinnedId];
                ImGui::SeparatorText("Animation");
                if (!sm.animations.empty()) {
                    o.animClip = glm::clamp(o.animClip, 0, (int)sm.animations.size() - 1);
                    if (ImGui::BeginCombo("Clip", sm.animations[o.animClip].name.c_str())) {
                        for (int a = 0; a < (int)sm.animations.size(); ++a) {
                            ImGui::PushID(a);
                            if (ImGui::Selectable(sm.animations[a].name.c_str(), o.animClip == a)) o.animClip = a;
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TextDisabled("%.2fs clip, playing on preview clock.", sm.animations[o.animClip].seconds());
                } else {
                    ImGui::TextDisabled("No animation clips in this mesh.");
                }
            }

            if (o.npcTemplate >= 0 && o.npcTemplate < (int)npcTemplates.size()) {
                ImGui::SeparatorText("NPC");
                ImGui::Text("Template: %s", npcTemplates[o.npcTemplate].name.c_str());
                // Live runtime state while playing: needs bars + current task.
                const NPCRuntime* rt = nullptr;
                for (const NPCRuntime& n : npcRuntimes)
                    if (n.sceneIndex == selected) { rt = &n; break; }
                if (rt) {
                    ImGui::ProgressBar(rt->health / 100.0f, ImVec2(-FLT_MIN, 0), "Health");
                    ImGui::ProgressBar(rt->hunger / 100.0f, ImVec2(-FLT_MIN, 0), "Hunger");
                    ImGui::ProgressBar(rt->thirst / 100.0f, ImVec2(-FLT_MIN, 0), "Thirst");
                    ImGui::ProgressBar(rt->energy / 100.0f, ImVec2(-FLT_MIN, 0), "Energy");
                    ImGui::Text("Activity: %s", rt->activity.c_str());
                    if (!rt->targetName.empty())
                        ImGui::Text("Heading to: %s%s", rt->targetName.c_str(),
                                    rt->moving ? " (walking)" : " (arrived)");
                    else
                        ImGui::TextDisabled("No scheduled target.");
                } else {
                    ImGui::TextDisabled("Press Play (F5) to simulate. Edit the type in the NPC Editor tab.");
                }
            }

            if (propActivated) snapshot();
        } else {
            ImGui::TextDisabled("No object selected.");
            ImGui::TextWrapped("Click an object in a viewport or the Outliner to edit it here.");
        }
        ImGui::End();

        // --- NPC Editor: author reusable NPC templates ---
        ImGui::Begin("NPC Editor");
        if (ImGui::Button("New Template")) {
            NPCTemplate t;
            t.name = "NPC " + std::to_string(npcTemplates.size());
            npcTemplates.push_back(t);
            selectedTemplate = (int)npcTemplates.size() - 1;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(selectedTemplate < 0 || selectedTemplate >= (int)npcTemplates.size());
        if (ImGui::Button("Delete Template")) {
            npcTemplates.erase(npcTemplates.begin() + selectedTemplate);
            for (SceneObject& so : scene) {     // keep instance links valid
                if (so.npcTemplate == selectedTemplate)      so.npcTemplate = -1;
                else if (so.npcTemplate > selectedTemplate)  so.npcTemplate--;
            }
            if (selectedTemplate >= (int)npcTemplates.size()) selectedTemplate = (int)npcTemplates.size() - 1;
        }
        ImGui::EndDisabled();

        ImGui::BeginChild("##tpllist", ImVec2(0, 90), true);
        for (int i = 0; i < (int)npcTemplates.size(); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(npcTemplates[i].name.c_str(), selectedTemplate == i)) selectedTemplate = i;
            ImGui::PopID();
        }
        ImGui::EndChild();

        if (selectedTemplate >= 0 && selectedTemplate < (int)npcTemplates.size()) {
            NPCTemplate& t = npcTemplates[selectedTemplate];
            ImGui::InputText("Name", &t.name);
            // Mesh picker lists static meshes first, then skinned (animated) ones.
            // Choosing a skinned mesh sets skinnedId (and clears meshId, -1).
            std::string curMesh;
            if (t.skinnedId >= 0 && t.skinnedId < (int)skinnedLibrary.size())
                curMesh = "[anim] " + skinnedLibrary[t.skinnedId].name;
            else if (t.meshId >= 0 && t.meshId < (int)meshLibrary.size())
                curMesh = meshLibrary[t.meshId].name;
            else
                curMesh = "(none)";
            if (ImGui::BeginCombo("Mesh", curMesh.c_str())) {
                for (int m = 0; m < (int)meshLibrary.size(); ++m) {
                    ImGui::PushID(m);
                    bool sel = (t.skinnedId < 0 && t.meshId == m);
                    if (ImGui::Selectable(meshLibrary[m].name.c_str(), sel)) { t.meshId = m; t.skinnedId = -1; }
                    ImGui::PopID();
                }
                for (int s = 0; s < (int)skinnedLibrary.size(); ++s) {
                    ImGui::PushID(1000 + s);
                    std::string label = "[anim] " + skinnedLibrary[s].name;
                    if (ImGui::Selectable(label.c_str(), t.skinnedId == s)) { t.skinnedId = s; t.meshId = -1; }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }

            // Animation clips by movement state (only for skinned NPCs that
            // actually carry clips). -1 = "use idle".
            if (t.skinnedId >= 0 && t.skinnedId < (int)skinnedLibrary.size()
                && !skinnedLibrary[t.skinnedId].animations.empty()) {
                const SkinnedMesh& sm = skinnedLibrary[t.skinnedId];
                ImGui::SeparatorText("Animation states");
                auto clipCombo = [&](const char* label, int& clip, bool allowNone) {
                    std::string cur = (clip >= 0 && clip < (int)sm.animations.size())
                                          ? sm.animations[clip].name : "(idle)";
                    if (ImGui::BeginCombo(label, cur.c_str())) {
                        if (allowNone && ImGui::Selectable("(idle)", clip < 0)) clip = -1;
                        for (int a = 0; a < (int)sm.animations.size(); ++a) {
                            ImGui::PushID(a);
                            if (ImGui::Selectable(sm.animations[a].name.c_str(), clip == a)) clip = a;
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                };
                clipCombo("Idle", t.clipIdle, false);
                clipCombo("Walk", t.clipWalk, true);
                clipCombo("Run",  t.clipRun,  true);
                ImGui::DragFloat("Run at speed >=", &t.runSpeed, 0.1f, 0.0f, 20.0f);
            }

            ImGui::SeparatorText("Attributes");
            ImGui::DragFloat("Health", &t.attr.health, 1.0f, 0.0f, 100.0f);
            ImGui::DragFloat("Hunger", &t.attr.hunger, 1.0f, 0.0f, 100.0f);
            ImGui::SameLine(); ImGui::SetNextItemWidth(60); ImGui::DragFloat("/h##hu", &t.attr.hungerRate, 0.1f, 0.0f, 50.0f);
            ImGui::DragFloat("Thirst", &t.attr.thirst, 1.0f, 0.0f, 100.0f);
            ImGui::SameLine(); ImGui::SetNextItemWidth(60); ImGui::DragFloat("/h##th", &t.attr.thirstRate, 0.1f, 0.0f, 50.0f);
            ImGui::DragFloat("Energy", &t.attr.energy, 1.0f, 0.0f, 100.0f);
            ImGui::SameLine(); ImGui::SetNextItemWidth(60); ImGui::DragFloat("/h##en", &t.attr.energyRate, 0.1f, 0.0f, 50.0f);
            ImGui::DragInt("Gold", &t.attr.gold, 1.0f, 0, 1000000);
            ImGui::DragFloat("Move speed", &t.attr.moveSpeed, 0.1f, 0.0f, 20.0f);

            ImGui::SeparatorText("Custom attributes");
            int removeC = -1;
            for (int c = 0; c < (int)t.custom.size(); ++c) {
                ImGui::PushID(2000 + c);
                ImGui::SetNextItemWidth(120); ImGui::InputText("##cn", &t.custom[c].first);
                ImGui::SameLine(); ImGui::SetNextItemWidth(70); ImGui::DragFloat("##cv", &t.custom[c].second, 0.1f);
                ImGui::SameLine(); if (ImGui::SmallButton("x")) removeC = c;
                ImGui::PopID();
            }
            if (removeC >= 0) t.custom.erase(t.custom.begin() + removeC);
            if (ImGui::Button("Add attribute")) t.custom.push_back({"attribute", 0.0f});

            ImGui::SeparatorText("Schedule (hour / activity / location)");
            int removeS = -1;
            for (int e = 0; e < (int)t.schedule.size(); ++e) {
                ImGui::PushID(3000 + e);
                ScheduleEntry& se = t.schedule[e];
                ImGui::SetNextItemWidth(64); ImGui::DragFloat("##hr", &se.hour, 0.25f, 0.0f, 24.0f, "%.2f");
                ImGui::SameLine(); ImGui::SetNextItemWidth(80); ImGui::InputText("##act", &se.activity);
                ImGui::SameLine(); ImGui::SetNextItemWidth(90);
                if (ImGui::BeginCombo("##loc", se.location.empty() ? "(where)" : se.location.c_str())) {
                    for (int o = 0; o < (int)scene.size(); ++o) {
                        ImGui::PushID(o);
                        if (ImGui::Selectable(scene[o].name.c_str(), se.location == scene[o].name)) se.location = scene[o].name;
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine(); if (ImGui::SmallButton("x")) removeS = e;
                ImGui::PopID();
            }
            if (removeS >= 0) t.schedule.erase(t.schedule.begin() + removeS);
            if (ImGui::Button("Add schedule entry")) t.schedule.push_back({8.0f, "work", ""});

            ImGui::Separator();
            if (ImGui::Button("Place NPC in world")) {
                snapshot();
                SceneObject o;
                o.name = t.name;
                o.meshId = t.skinnedId >= 0 ? -1 : t.meshId;
                o.skinnedId = t.skinnedId;
                o.npcTemplate = selectedTemplate;
                o.color = glm::vec3(1.0f);
                scene.push_back(o);
                selected = (int)scene.size() - 1;
            }
        } else {
            ImGui::TextDisabled("No template selected. Click 'New Template'.");
        }
        ImGui::End();

        // --- Present: render ImGui to the real window ---
        ImGui::Render();
        int dw, dh;
        glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);

        // ---- Play render (separate window, its own GL context) ----
        if (playWindow) {
            glfwMakeContextCurrent(playWindow);

            // Esc stops play (back to editor); F11 toggles borderless fullscreen.
            if (glfwGetKey(playWindow, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
                closePlay();
            } else {
                bool f11 = glfwGetKey(playWindow, GLFW_KEY_F11) == GLFW_PRESS;
                if (f11 && !prevPlayF11) {
                    if (!playFullscreen) {
                        glfwGetWindowPos(playWindow, &playWinX, &playWinY);
                        glfwGetWindowSize(playWindow, &playWinW, &playWinH);
                        GLFWmonitor* mon = glfwGetPrimaryMonitor();
                        const GLFWvidmode* vm = glfwGetVideoMode(mon);
                        glfwSetWindowMonitor(playWindow, mon, 0, 0, vm->width, vm->height, vm->refreshRate);
                    } else {
                        glfwSetWindowMonitor(playWindow, nullptr, playWinX, playWinY, playWinW, playWinH, 0);
                    }
                    playFullscreen = !playFullscreen;
                }
                prevPlayF11 = f11;

                // --- Free-fly camera: WASD/QE move, hold RMB to mouselook.
                // Raw GLFW input (the play window has no ImGui context). Gated
                // on focus so editor typing never drives the game camera.
                bool focused = glfwGetWindowAttrib(playWindow, GLFW_FOCUSED) != 0;
                bool rmb = focused && glfwGetMouseButton(playWindow, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
                if (rmb && !playLooking) {
                    playLooking = true;
                    glfwGetCursorPos(playWindow, &playLastMouseX, &playLastMouseY);
                    glfwSetInputMode(playWindow, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                } else if (playLooking && !rmb) {
                    playLooking = false;
                    glfwSetInputMode(playWindow, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                }
                if (playLooking) {
                    double mx, my; glfwGetCursorPos(playWindow, &mx, &my);
                    gameCam.yaw   += (float)(mx - playLastMouseX) * 0.0025f;
                    gameCam.pitch -= (float)(my - playLastMouseY) * 0.0025f;   // invert Y
                    gameCam.pitch  = glm::clamp(gameCam.pitch, -1.54f, 1.54f);
                    playLastMouseX = mx; playLastMouseY = my;
                }
                glm::vec3 gf  = forwardFromYawPitch(gameCam.yaw, gameCam.pitch);
                glm::vec3 grt = glm::normalize(glm::cross(gf, glm::vec3(0, 1, 0)));
                glm::vec3 gwup(0, 1, 0);
                if (focused) {
                    float v = gameCam.flySpeed * dt;
                    if (glfwGetKey(playWindow, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) v *= 3.0f;
                    if (glfwGetKey(playWindow, GLFW_KEY_W) == GLFW_PRESS) gameCam.flyPos += gf  * v;
                    if (glfwGetKey(playWindow, GLFW_KEY_S) == GLFW_PRESS) gameCam.flyPos -= gf  * v;
                    if (glfwGetKey(playWindow, GLFW_KEY_D) == GLFW_PRESS) gameCam.flyPos += grt * v;
                    if (glfwGetKey(playWindow, GLFW_KEY_A) == GLFW_PRESS) gameCam.flyPos -= grt * v;
                    if (glfwGetKey(playWindow, GLFW_KEY_E) == GLFW_PRESS) gameCam.flyPos += gwup * v;
                    if (glfwGetKey(playWindow, GLFW_KEY_Q) == GLFW_PRESS) gameCam.flyPos -= gwup * v;
                }
                gameCam.eye    = gameCam.flyPos;
                gameCam.target = gameCam.flyPos + gf;

                int gw, gh;
                glfwGetFramebufferSize(playWindow, &gw, &gh);
                float gAspect = gh == 0 ? 1.0f : (float)gw / (float)gh;
                glm::mat4 gView = viewMatrix(gameCam);
                glm::mat4 gProj = projMatrix(gameCam, gAspect);
                syncMeshes(playRenderer, meshLibrary);       // upload meshes into the play context
                syncSkinned(playRenderer, skinnedLibrary);   // ...and skinned meshes
                std::vector<glm::mat4> playModels;
                std::vector<glm::vec3> playColors;
                std::vector<int>       playMeshIds;
                playModels.reserve(scene.size());
                playColors.reserve(scene.size());
                playMeshIds.reserve(scene.size());
                for (const SceneObject& o : scene) {
                    playModels.push_back(o.transform);
                    playColors.push_back(o.color);
                    playMeshIds.push_back(o.skinnedId >= 0 ? -1 : o.meshId);
                }
                std::vector<SkinnedDrawItem>        playSkins;
                std::vector<std::vector<glm::mat4>> playPalettes;
                playPalettes.reserve(scene.size());
                for (int i = 0; i < (int)scene.size(); ++i) {
                    const SceneObject& o = scene[i];
                    if (o.skinnedId < 0 || o.skinnedId >= (int)skinnedLibrary.size()) continue;
                    const SkinnedMesh& sm = skinnedLibrary[o.skinnedId];
                    playPalettes.emplace_back();
                    if (!sm.animations.empty()) {
                        int ci = glm::clamp(o.animClip, 0, (int)sm.animations.size() - 1);
                        computeBoneMatrices(sm, sm.animations[ci], animClock, playPalettes.back());
                    } else {
                        computeBoneMatrices(sm, Animation{}, 0.0f, playPalettes.back());
                    }
                    SkinnedDrawItem it;
                    it.skinnedId = o.skinnedId; it.sceneIndex = i;
                    it.model = o.transform; it.color = o.color;
                    playSkins.push_back(it);
                }
                for (size_t k = 0; k < playSkins.size(); ++k) playSkins[k].palette = &playPalettes[k];
                renderScene(0, gw, gh, gameCam, gView, gProj, playRenderer, env, false, true, playModels, playColors, playMeshIds, -1, gridSize, playSkins);
                glfwSwapBuffers(playWindow);
            }
        }
    }

    if (playWindow) closePlay();
    destroyRenderer(renderer);
    destroyTarget(rtPersp);
    destroyTarget(rtTop);
    destroyTarget(rtFront);
    destroyTarget(rtRight);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
