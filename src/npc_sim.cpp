#include "npc_sim.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

// ---- small helpers --------------------------------------------------------

// Cheap deterministic [0,1) hash so each NPC gets a stable, varied lead time.
static float hash01(int i) {
    float s = std::sin((float)i * 12.9898f) * 43758.5453f;
    return s - std::floor(s);
}

// Hours wrapped into [0, 24).
static float mod24(float h) {
    float r = std::fmod(h, 24.0f);
    return r < 0.0f ? r + 24.0f : r;
}

static std::string toLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Local AABB + the model matrix that places it in the world (mirrors the
// editor's picking logic so obstacle footprints match what's drawn).
static void objectBounds(const SceneObject& o,
                         const std::vector<MeshAsset>& meshLib,
                         const std::vector<SkinnedMesh>& skinnedLib,
                         glm::mat4& outModel, glm::vec3& bmin, glm::vec3& bmax) {
    bmin = glm::vec3(-0.5f); bmax = glm::vec3(0.5f);
    outModel = o.transform;
    if (o.skinnedId >= 0 && o.skinnedId < (int)skinnedLib.size()) {
        const SkinnedMesh& sm = skinnedLib[o.skinnedId];
        bmin = sm.bounds.min; bmax = sm.bounds.max;
        outModel = o.transform * sm.importFix;
    } else if (o.meshId >= 0 && o.meshId < (int)meshLib.size()) {
        bmin = meshLib[o.meshId].bounds.min;
        bmax = meshLib[o.meshId].bounds.max;
    }
}

// World-space XZ rectangle covering the object's transformed AABB.
static NavRect worldFootprint(const glm::mat4& m, glm::vec3 bmin, glm::vec3 bmax) {
    NavRect r;
    r.min = glm::vec2( 1e30f);
    r.max = glm::vec2(-1e30f);
    for (int i = 0; i < 8; ++i) {
        glm::vec3 corner((i & 1) ? bmax.x : bmin.x,
                         (i & 2) ? bmax.y : bmin.y,
                         (i & 4) ? bmax.z : bmin.z);
        glm::vec3 w = glm::vec3(m * glm::vec4(corner, 1.0f));
        r.min = glm::min(r.min, glm::vec2(w.x, w.z));
        r.max = glm::max(r.max, glm::vec2(w.x, w.z));
    }
    return r;
}

static int findObjectByName(const std::vector<SceneObject>& scene, const std::string& name) {
    if (name.empty()) return -1;
    for (int i = 0; i < (int)scene.size(); ++i)
        if (scene[i].name == name) return i;
    return -1;
}

// ---- grid from scene ------------------------------------------------------
void buildNavFromScene(NavGrid& grid,
                       const std::vector<SceneObject>& scene,
                       const std::vector<NPCTemplate>& templates,
                       const std::vector<MeshAsset>& meshLib,
                       const std::vector<SkinnedMesh>& skinnedLib,
                       float cellSize, float agentRadius) {
    // Names used as schedule destinations are never obstacles (an NPC must be
    // able to stand on its bed/workbench).
    std::vector<std::string> destNames;
    for (const NPCTemplate& t : templates)
        for (const ScheduleEntry& se : t.schedule)
            if (!se.location.empty()) destNames.push_back(se.location);

    auto isDest = [&](const std::string& n) {
        return std::find(destNames.begin(), destNames.end(), n) != destNames.end();
    };

    glm::vec2 worldMin( 1e30f), worldMax(-1e30f);
    std::vector<NavRect> obstacles;
    for (const SceneObject& o : scene) {
        glm::mat4 m; glm::vec3 bmin, bmax;
        objectBounds(o, meshLib, skinnedLib, m, bmin, bmax);
        NavRect fp = worldFootprint(m, bmin, bmax);
        worldMin = glm::min(worldMin, fp.min);
        worldMax = glm::max(worldMax, fp.max);
        if (o.npcTemplate >= 0) continue;     // NPCs aren't obstacles
        if (isDest(o.name))      continue;     // destinations aren't obstacles
        obstacles.push_back(fp);
    }

    // Fall back to a sane area if the scene is empty / degenerate.
    if (worldMin.x > worldMax.x) { worldMin = glm::vec2(-10.0f); worldMax = glm::vec2(10.0f); }
    const float margin = 5.0f;
    worldMin -= glm::vec2(margin);
    worldMax += glm::vec2(margin);

    buildNavGrid(grid, worldMin, worldMax, cellSize, obstacles, agentRadius);
}

// ---- seeding --------------------------------------------------------------
void seedNPCs(std::vector<NPCRuntime>& out,
              const std::vector<SceneObject>& scene,
              const std::vector<NPCTemplate>& templates) {
    out.clear();
    for (int i = 0; i < (int)scene.size(); ++i) {
        const SceneObject& o = scene[i];
        if (o.npcTemplate < 0 || o.npcTemplate >= (int)templates.size()) continue;
        const NPCTemplate& t = templates[o.npcTemplate];

        NPCRuntime n;
        n.sceneIndex = i;
        n.templateId = o.npcTemplate;
        n.health = t.attr.health;
        n.hunger = t.attr.hunger;
        n.thirst = t.attr.thirst;
        n.energy = t.attr.energy;
        n.speed  = t.attr.moveSpeed > 0.0f ? t.attr.moveSpeed : 1.5f;

        // Decompose the authored transform into position + scale (rotation is
        // replaced by the walk facing).
        glm::vec3 pos = glm::vec3(o.transform[3]);
        n.pos   = pos;
        n.baseY = pos.y;
        n.scale = glm::vec3(glm::length(glm::vec3(o.transform[0])),
                            glm::length(glm::vec3(o.transform[1])),
                            glm::length(glm::vec3(o.transform[2])));
        if (n.scale.x <= 0.0f) n.scale.x = 1.0f;
        if (n.scale.y <= 0.0f) n.scale.y = 1.0f;
        if (n.scale.z <= 0.0f) n.scale.z = 1.0f;

        n.leadHours = 0.5f + hash01(i) * 1.0f;   // 0.5 .. 1.5 game-hours early
        n.targetName.clear();
        out.push_back(n);
    }
}

// ---- per-frame simulation -------------------------------------------------

// Pick the schedule entry the NPC should be acting on right now: the upcoming
// entry if it's within lead time, otherwise the most recently passed one.
static const ScheduleEntry* pickGoal(const NPCTemplate& t, float now, float lead) {
    if (t.schedule.empty()) return nullptr;
    int    activeIdx = -1; float bestElapsed = 1e30f;
    int    upIdx     = -1; float bestUntil   = 1e30f;
    for (int e = 0; e < (int)t.schedule.size(); ++e) {
        float elapsed = mod24(now - t.schedule[e].hour);
        float until   = mod24(t.schedule[e].hour - now);
        if (elapsed < bestElapsed) { bestElapsed = elapsed; activeIdx = e; }
        if (until   < bestUntil)   { bestUntil   = until;   upIdx     = e; }
    }
    if (upIdx >= 0 && bestUntil <= lead) return &t.schedule[upIdx];
    if (activeIdx >= 0)                  return &t.schedule[activeIdx];
    return nullptr;
}

void simulateNPCs(std::vector<NPCRuntime>& npcs,
                  std::vector<SceneObject>& scene,
                  const std::vector<NPCTemplate>& templates,
                  const NavGrid& grid,
                  const Environment& env,
                  float dt) {
    const float now = env.timeOfDay;
    // Game-hours elapsed this frame (needs only deplete while the clock runs).
    const float gameHours = (env.running && env.dayLengthSec > 0.0f)
                            ? (dt / env.dayLengthSec) * 24.0f : 0.0f;

    for (NPCRuntime& n : npcs) {
        if (n.sceneIndex < 0 || n.sceneIndex >= (int)scene.size()) continue;
        if (n.templateId < 0 || n.templateId >= (int)templates.size()) continue;
        const NPCTemplate& t = templates[n.templateId];

        // 1) Decide where this NPC should be heading.
        const ScheduleEntry* goal = pickGoal(t, now, n.leadHours);
        std::string wantTarget = goal ? goal->location : std::string();
        n.activity = goal ? goal->activity : std::string("idle");

        // 2) (Re)plan a path when the target changes.
        if (wantTarget != n.targetName) {
            n.targetName = wantTarget;
            n.path.clear();
            n.waypoint = 0;
            n.moving   = false;
            int ti = findObjectByName(scene, wantTarget);
            if (ti >= 0 && ti != n.sceneIndex) {
                glm::vec3 gp = glm::vec3(scene[ti].transform[3]);
                std::vector<glm::vec2> path;
                if (findPath(grid, glm::vec2(n.pos.x, n.pos.z),
                             glm::vec2(gp.x, gp.z), path) && path.size() >= 1) {
                    n.path = std::move(path);
                    n.waypoint = (n.path.size() > 1) ? 1 : 0;  // skip the start point
                    n.moving = (n.waypoint < n.path.size());
                }
            }
        }

        // 3) Walk along the current path.
        if (n.moving && n.waypoint < n.path.size()) {
            glm::vec2 tgt = n.path[n.waypoint];
            glm::vec2 cur(n.pos.x, n.pos.z);
            glm::vec2 to = tgt - cur;
            float dist = glm::length(to);
            float stepLen = n.speed * dt;
            if (dist <= stepLen || dist < 1e-4f) {
                cur = tgt;
                ++n.waypoint;
                if (n.waypoint >= n.path.size()) n.moving = false;  // arrived
            } else {
                glm::vec2 dir = to / dist;
                cur += dir * stepLen;
                n.facing = std::atan2(dir.x, dir.y);   // yaw from +Z toward dir
            }
            n.pos.x = cur.x; n.pos.z = cur.y;
        }

        // 4) Needs deplete over game time; arriving at a restorative activity
        //    refills the matching need.
        n.hunger -= t.attr.hungerRate * gameHours;
        n.thirst -= t.attr.thirstRate * gameHours;
        n.energy -= t.attr.energyRate * gameHours;
        if (!n.moving && !n.targetName.empty()) {
            std::string act = toLower(n.activity);
            const float refill = 40.0f * gameHours;   // per game-hour
            if (act.find("eat") != std::string::npos || act.find("food") != std::string::npos ||
                act.find("cook") != std::string::npos)
                n.hunger += refill;
            if (act.find("drink") != std::string::npos || act.find("water") != std::string::npos)
                n.thirst += refill;
            if (act.find("sleep") != std::string::npos || act.find("rest") != std::string::npos ||
                act.find("bed")   != std::string::npos)
                n.energy += refill;
        }
        // Health: starving/dehydrated/exhausted hurts; otherwise slowly recovers.
        if (n.hunger <= 0.0f || n.thirst <= 0.0f || n.energy <= 0.0f)
            n.health -= 5.0f * gameHours;
        else
            n.health += 1.0f * gameHours;
        n.hunger = glm::clamp(n.hunger, 0.0f, 100.0f);
        n.thirst = glm::clamp(n.thirst, 0.0f, 100.0f);
        n.energy = glm::clamp(n.energy, 0.0f, 100.0f);
        n.health = glm::clamp(n.health, 0.0f, 100.0f);

        // 5) Drive the animation clip from movement state (skinned NPCs only).
        //    Idle when stopped, walk while moving, run when the NPC is fast.
        SceneObject& so = scene[n.sceneIndex];
        if (so.skinnedId >= 0) {
            int clip = t.clipIdle;
            if (n.moving) {
                bool runs = (n.speed >= t.runSpeed) && (t.clipRun >= 0);
                clip = runs ? t.clipRun : (t.clipWalk >= 0 ? t.clipWalk : t.clipIdle);
            }
            so.animClip = clip;   // render clamps to the mesh's clip range
        }

        // 6) Write position + facing back into the scene transform.
        n.pos.y = n.baseY;
        glm::mat4 m = glm::translate(glm::mat4(1.0f), n.pos);
        m = glm::rotate(m, n.facing, glm::vec3(0.0f, 1.0f, 0.0f));
        m = glm::scale(m, n.scale);
        so.transform = m;
    }
}
