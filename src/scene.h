#pragma once
#include <glm/glm.hpp>

#include "mesh.h"
#include "skinned.h"

#include <string>
#include <utility>
#include <vector>

// Consumable resource kind for a placed source object. Food/water sources are
// what need-driven NPCs path to and consume from (each visit drains a portion).
enum ResourceType { RES_NONE = 0, RES_FOOD = 1, RES_WATER = 2 };

// A scene object: a named transform + colour (tint) + which mesh asset it uses.
// npcTemplate >= 0 marks this object as a placed NPC instance of that template.
struct SceneObject {
    std::string name;
    glm::mat4 transform{1.0f};
    glm::vec3 color{0.75f, 0.75f, 0.8f};
    int meshId = 0;        // index into the static mesh library (0 = built-in cube)
    int skinnedId = -1;    // index into the skinned mesh library, or -1 if static
    int animClip  = 0;     // which animation clip to play (skinned objects only)
    int npcTemplate = -1;  // index into npcTemplates, or -1 if a plain object
    int resourceType = RES_NONE;  // food/water source kind (RES_NONE = ordinary prop)
    int portions = 0;             // servings remaining (resource sources only)
};

// ---- NPC definition (template) -------------------------------------------
// One schedule entry: at `hour` the NPC goes to `location` (a scene object's
// name) and performs `activity`. Used by the simulation layer.
struct ScheduleEntry {
    float       hour = 8.0f;       // 0-24
    std::string activity = "work";
    std::string location = "";     // name of a scene object to go to
};

// Curated survival/RPG stats. Needs are 0-100 and deplete at a per-game-hour
// rate; the simulation refills them by eating/drinking/resting.
struct NPCAttributes {
    float health = 100.0f;
    float hunger = 100.0f, hungerRate = 4.0f;
    float thirst = 100.0f, thirstRate = 6.0f;
    float energy = 100.0f, energyRate = 4.0f;
    int   gold = 50;
    float moveSpeed = 1.5f;        // world units / second
};

// A reusable NPC type: mesh + attributes + custom fields + a daily schedule.
struct NPCTemplate {
    std::string name = "NPC";
    int meshId = 0;                // which static mesh asset this NPC uses
    int skinnedId = -1;            // or which skinned (animated) mesh, -1 if static
    NPCAttributes attr;
    // Animation clips driven by movement state (indices into the skinned mesh's
    // clip list; -1 falls back to the idle clip). The sim swaps these as the NPC
    // starts/stops walking.
    int clipIdle = 0;
    int clipWalk = -1;
    int clipRun  = -1;
    float runSpeed = 3.0f;         // moveSpeed at/above this uses the run clip
    std::vector<std::pair<std::string, float>> custom;  // user-defined attributes
    std::vector<ScheduleEntry> schedule;
};

// Where the FPS player spawns, plus the controller's tuning. Edited in the
// Properties panel, shown as a marker in the editor, and used by play mode's
// first-person controller. Persisted with the scene.
struct PlayerStart {
    glm::vec3 pos{0.0f, 0.0f, 0.0f};  // spawn position (feet, world space)
    float yaw       = 0.0f;           // spawn facing (radians)
    float eyeHeight = 1.7f;           // camera height above the feet
    float radius    = 0.4f;           // capsule radius (marker; collision is ground-only)
    float walkSpeed = 5.0f;           // m/s
    float runSpeed  = 9.0f;           // m/s while holding Shift
    float jumpSpeed = 6.0f;           // m/s launch velocity
    float gravity   = 18.0f;          // m/s^2
    float mouseSens = 0.0025f;        // radians per pixel
    bool  fps       = true;           // play uses the FPS controller (else free-fly)
};

struct Environment;   // environment.h (passed by ref; full include in scene.cpp)
struct Terrain;       // terrain.h

// Plain-text scene format (v4): header + library manifests (source paths for the
// static & skinned mesh libraries) + per-object records + NPC templates +
// environment/weather/fog. The heightmap terrain is written to a binary sidecar
// next to the scene file ("<path>.terrain"). Saving captures everything needed to
// fully restore the world; loading re-imports the meshes and restores the rest.
// Older files still load: v2 (transforms + colour only) and v3 (adds library
// manifests + object refs). Missing v4 sections fall back to the caller's state.
bool saveScene(const std::vector<SceneObject>& scene,
               const std::vector<MeshAsset>& meshLib,
               const std::vector<SkinnedMesh>& skinnedLib,
               const std::vector<NPCTemplate>& npcTemplates,
               const Environment& env,
               const Terrain& terrain,
               const PlayerStart& player,
               const char* path);
bool loadScene(std::vector<SceneObject>& outScene,
               std::vector<MeshAsset>& meshLib,
               std::vector<SkinnedMesh>& skinnedLib,
               std::vector<NPCTemplate>& npcTemplates,
               Environment& env,
               Terrain& terrain,
               PlayerStart& player,
               const char* path);
