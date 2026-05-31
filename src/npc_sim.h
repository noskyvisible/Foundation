#pragma once
#include <glm/glm.hpp>

#include <string>
#include <vector>

#include "environment.h"
#include "mesh.h"
#include "navigation.h"
#include "scene.h"
#include "skinned.h"
#include "terrain.h"

// ---- Layer 3: scheduled NPC simulation -----------------------------------
// Drives placed NPC instances (SceneObjects with npcTemplate >= 0) during play.
//
// The AI is a *priority-arbitrated state machine* (utility-lite): each decision
// tick every state is scored from the NPC's needs, schedule and role, and the
// NPC switches to the highest-scoring valid state. Transitions are emergent
// (no hand-wired edges), so adding a state is purely additive. The chosen state
// emits a goal; a shared locomotion layer paths to it via grid A*, walks, plays
// the right animation, depletes needs, and refills them on arrival.

// AI states. The set grows over time; roles will gate/weight which apply.
// (Phase 1 = the Townsfolk baseline.)
enum class NPCState {
    Idle,     // stand still (fallback)
    Wander,   // stroll to a random reachable point, pause, repeat
    Work,     // go to the scheduled work location during work hours
    Eat,      // go to a food location and restore hunger
    Drink,    // go to a water location and restore thirst
    Sleep,    // go to a bed/home location and restore energy
    Goto,     // go to a scheduled location and wait there (generic appointment)
};
const char* npcStateName(NPCState s);

// Live, per-instance state for one NPC while playing.
struct NPCRuntime {
    int   sceneIndex  = -1;        // index into the scene vector
    int   templateId  = -1;        // index into npcTemplates
    // Live needs (seeded from the template, deplete over game time).
    float health = 100.0f;
    float hunger = 100.0f;
    float thirst = 100.0f;
    float energy = 100.0f;
    // Movement state.
    glm::vec3 pos{0.0f};           // current world position
    float     baseY  = 0.0f;       // authored ground height (kept while walking)
    glm::vec3 scale{1.0f};         // authored scale (reapplied each frame)
    float     facing = 0.0f;       // yaw radians, faces the walk direction
    float     speed  = 1.5f;       // world units / second
    std::vector<glm::vec2> path;   // remaining world-XZ waypoints
    size_t    waypoint = 0;        // index of the next waypoint in `path`
    bool      moving = false;
    // AI / behaviour state machine.
    NPCState  state = NPCState::Idle;  // current behaviour
    float     decideTimer = 0.0f;      // counts down to the next arbiter re-score
    glm::vec2 goalXZ{0.0f};            // current navigation goal (world XZ)
    bool      hasGoal = false;         // is goalXZ valid / are we pathing to it
    float     pauseTimer = 0.0f;       // dwell time after reaching a wander point
    unsigned  rng = 1u;                // per-NPC PRNG state (wander point picks)
    int       consumingObj = -1;       // resource source we've taken a portion from this visit
    float     sleepTimer = 0.0f;       // remaining game-hours of the current sleep session (>0 = asleep)
    float     restedTimer = 0.0f;      // remaining game-hours of the post-sleep slow-drain "rested" buff
    // Schedule tracking / display.
    std::string targetName;        // scene object we're currently heading to ("" = none)
    std::string activity = "idle"; // current activity label (for the inspector)
    float       leadHours = 1.0f;  // depart this many game-hours early
};

// Build a navigation grid from the scene. Every solid object that is NOT an NPC
// and NOT used as a schedule destination becomes a blocked footprint. The grid
// spans all object positions plus a margin.
void buildNavFromScene(NavGrid& grid,
                       const std::vector<SceneObject>& scene,
                       const std::vector<NPCTemplate>& templates,
                       const std::vector<MeshAsset>& meshLib,
                       const std::vector<SkinnedMesh>& skinnedLib,
                       float cellSize = 0.5f, float agentRadius = 0.35f);

// Seed runtime state for every NPC instance in the scene. Call on Play start.
void seedNPCs(std::vector<NPCRuntime>& out,
              const std::vector<SceneObject>& scene,
              const std::vector<NPCTemplate>& templates);

// Advance the simulation by `dt` real seconds. Picks each NPC's scheduled
// target, paths/walks to it, depletes needs by elapsed game-time, and refills
// on arrival. Writes updated position + facing back into the scene transforms.
void simulateNPCs(std::vector<NPCRuntime>& npcs,
                  std::vector<SceneObject>& scene,
                  const std::vector<NPCTemplate>& templates,
                  const NavGrid& grid,
                  const Environment& env,
                  const Terrain& terrain,
                  float dt);
