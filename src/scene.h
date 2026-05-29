#pragma once
#include <glm/glm.hpp>

#include "mesh.h"
#include "skinned.h"

#include <string>
#include <utility>
#include <vector>

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

// Plain-text scene format (v3). Header + two library manifests (the source
// paths backing the static & skinned mesh libraries) + per-object records
// (name, 16-float transform, RGB colour, and the meshId/skinnedId/animClip/
// npcTemplate references). Saving captures the libraries so that loading can
// re-import the meshes and restore each object's references. Older v2 files
// (transform + colour only) still load. NPC templates themselves are not yet
// persisted, so npcTemplate indices are only meaningful within one session.
bool saveScene(const std::vector<SceneObject>& scene,
               const std::vector<MeshAsset>& meshLib,
               const std::vector<SkinnedMesh>& skinnedLib,
               const char* path);
bool loadScene(std::vector<SceneObject>& outScene,
               std::vector<MeshAsset>& meshLib,
               std::vector<SkinnedMesh>& skinnedLib,
               const char* path);
