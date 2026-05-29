#pragma once
#include <glm/glm.hpp>

#include <string>
#include <vector>

#include "environment.h"
#include "mesh.h"
#include "navigation.h"
#include "scene.h"
#include "skinned.h"

// ---- Layer 3: scheduled NPC simulation -----------------------------------
// Drives placed NPC instances (SceneObjects with npcTemplate >= 0) during play:
// reads each NPC's daily schedule against the game clock, paths to the target
// scene object via grid A*, walks there, depletes survival needs over game
// time, and refills them on arrival. Mutates the scene objects' transforms.

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
    // Schedule tracking.
    std::string targetName;        // scene object we're currently heading to
    std::string activity = "idle";
    float       leadHours = 1.0f;  // depart this many game-hours early
    bool        moving = false;
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
                  float dt);
