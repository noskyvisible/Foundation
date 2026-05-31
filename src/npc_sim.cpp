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

// Per-NPC xorshift PRNG (avoids global rand(); deterministic from the seed).
static float frand(unsigned& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (s & 0xFFFFFF) / (float)0x1000000;   // [0,1)
}

const char* npcStateName(NPCState s) {
    switch (s) {
        case NPCState::Idle:   return "Idle";
        case NPCState::Wander: return "Wander";
        case NPCState::Work:   return "Work";
        case NPCState::Eat:    return "Eat";
        case NPCState::Drink:  return "Drink";
        case NPCState::Sleep:  return "Sleep";
        case NPCState::Goto:   return "Goto";
    }
    return "?";
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
        if (o.npcTemplate >= 0)           continue;   // NPCs aren't obstacles
        if (isDest(o.name))               continue;   // schedule destinations aren't obstacles
        if (o.resourceType != RES_NONE)   continue;   // food/water/bed are walkable goals
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

        // Preserve the authored facing: the model's forward is +Z (after importFix),
        // so the placed yaw is atan2 of the transform's Z basis. Without this the NPC
        // would snap to a fixed direction on Play, ignoring how it was rotated.
        n.facing = std::atan2(o.transform[2].x, o.transform[2].z);

        // Depart only ~15 game-minutes before the scheduled hour (small per-NPC
        // jitter so they don't all set off on the same frame). Until then the NPC
        // keeps doing its current activity instead of leaving an hour early.
        n.leadHours = 0.20f + hash01(i) * 0.10f;  // ~12-18 game-min (about 15)
        n.targetName.clear();
        n.state       = NPCState::Idle;
        n.decideTimer = hash01(i) * 0.4f;         // stagger the first decision
        n.hasGoal     = false;
        n.pauseTimer  = 0.0f;
        n.rng         = (unsigned)(i * 2654435761u) | 1u;   // non-zero seed
        out.push_back(n);
    }
}

// ---- per-frame simulation -------------------------------------------------

// Pick the schedule entry the NPC should be acting on right now:
//   * the upcoming entry, but only once it's within the (short) lead window — so
//     the NPC sets off ~15 min before, not hours early;
//   * otherwise the entry that is genuinely *ongoing* — the most recently passed
//     one, but only if it started within the last 12h. This guards the
//     wrap-around trap: a lone 10:00 entry, seen at 09:00, has elapsed≈23h and
//     would otherwise masquerade as the "current" activity, making the NPC leave
//     an hour early. Beyond that there is no appointment (free time) and the NPC
//     waits / wanders until its next entry's lead window opens.
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
    if (upIdx >= 0 && bestUntil <= lead)        return &t.schedule[upIdx];
    if (activeIdx >= 0 && bestElapsed <= 12.0f) return &t.schedule[activeIdx];
    return nullptr;
}

// ---- behaviour arbiter (utility-lite FSM) --------------------------------

struct StateChoice { NPCState state; std::string loc; float score; };

// Need pressure: 0 above 60, ramping to 1.0 at empty. Drives Eat/Drink/Sleep.
static float needUrgency(float need) {
    return glm::clamp((60.0f - need) / 60.0f, 0.0f, 1.0f);
}

// First schedule location whose activity contains either keyword (lowercased).
// This is how an NPC knows *where* to eat/sleep/work; the FSM decides *when*.
static std::string schedLocFor(const NPCTemplate& t, const char* k1, const char* k2) {
    for (const ScheduleEntry& se : t.schedule) {
        if (se.location.empty()) continue;
        std::string a = toLower(se.activity);
        if (a.find(k1) != std::string::npos || (k2 && a.find(k2) != std::string::npos))
            return se.location;
    }
    return {};
}

// Score every Townsfolk state and return the winner. Object-based states must
// have a valid destination to win; Wander/Idle need none. Need-driven states
// (Sleep/Eat/Drink) gain urgency as the matching need drops; Work wins during
// scheduled work hours when no need is pressing. Transitions are emergent: the
// NPC simply ends up in whichever state scores highest right now.
static StateChoice decideTownsfolk(const NPCRuntime& n, const NPCTemplate& t, float now,
                                   const std::string& foodLoc, const std::string& waterLoc,
                                   const std::string& bedLoc) {
    bool night = (now < 6.0f || now >= 21.0f);
    const ScheduleEntry* g = pickGoal(t, now, n.leadHours);
    std::string sa = g ? toLower(g->activity) : std::string();
    auto saHas = [&](const char* k) { return sa.find(k) != std::string::npos; };

    // Baseline: an NPC with a schedule WAITS in place between appointments (so it
    // doesn't appear to set off early), while a free agent with no schedule strolls
    // around for ambient life. Either is easily overridden below.
    StateChoice best{ NPCState::Idle, std::string(), 0.05f };
    if (t.schedule.empty() && 0.20f > best.score)
        best = { NPCState::Wander, std::string(), 0.20f };

    auto consider = [&](NPCState st, std::string loc, float score) {
        if (loc.empty()) return;                 // object-based states need a place
        if (score > best.score) best = { st, std::move(loc), score };
    };

    // (A) Scheduled appointment: attend the current/imminent entry's location.
    //     pickGoal only hands one back while it's genuinely ongoing or within the
    //     short lead window, so the NPC stays put until ~15 min before, then heads
    //     over. The activity keyword picks the matching state (so Eat/Sleep/Drink
    //     still restore their need); anything else is a generic Goto-and-wait.
    //     Duty is HIGH priority (0.75): a shopkeeper/barman sticks to their shift —
    //     only a *critical* need (urgency > 0.75, i.e. below ~15) pulls them away,
    //     and they head back the moment it's handled.
    bool hasAppt = (g && !g->location.empty());
    NPCState apptState = NPCState::Goto;
    if (hasAppt) {
        if      (saHas("sleep") || saHas("bed") || saHas("rest")) apptState = NPCState::Sleep;
        else if (saHas("eat")   || saHas("food") || saHas("cook")) apptState = NPCState::Eat;
        else if (saHas("drink") || saHas("water"))                apptState = NPCState::Drink;
        else if (saHas("work"))                                   apptState = NPCState::Work;
        consider(apptState, g->location, 0.75f);
    }
    // A non-sleep appointment (a work/duty shift) outranks sleeping entirely: the
    // NPC reports for the shift even mid-sleep, getting up ~15 min before it opens.
    bool dutyCalls = hasAppt && apptState != NPCState::Sleep;

    // (B) Survival needs. Below ~15 a need becomes urgent enough (urgency > 0.75)
    //     to override even a work shift; above that, duty wins. Once a restorative
    //     action is under way the "sticky" bonus (0.85) keeps the NPC finishing it
    //     to ~85% — beating the shift — so it tops up instead of bouncing back to
    //     work half-done, then returns. The ambient night-sleep nudge applies only
    //     when off-duty (no appointment).
    float eu = needUrgency(n.energy), hu = needUrgency(n.hunger), tu = needUrgency(n.thirst);
    auto sticky = [&](NPCState st, float need) { return (n.state == st && need < 85.0f) ? 0.85f : 0.0f; };

    float sleepScore;
    if (dutyCalls) {
        sleepScore = 0.0f;   // a work shift is imminent/active: get up and go, even mid-sleep
    } else {
        sleepScore = std::max(eu + ((night && !hasAppt) ? 0.30f : 0.0f), sticky(NPCState::Sleep, n.energy));
        if (n.sleepTimer > 0.0f) sleepScore = 2.0f;   // otherwise run the full random sleep session
    }
    consider(NPCState::Sleep, !bedLoc.empty() ? bedLoc : schedLocFor(t, "sleep", "bed"), sleepScore);
    consider(NPCState::Eat,   !foodLoc.empty()  ? foodLoc  : schedLocFor(t, "eat", "food"),
             std::max(hu, sticky(NPCState::Eat, n.hunger)));
    consider(NPCState::Drink, !waterLoc.empty() ? waterLoc : schedLocFor(t, "drink", "water"),
             std::max(tu, sticky(NPCState::Drink, n.thirst)));
    return best;
}

// Set the NPC's path to a world-XZ goal. Returns true if a path was found.
static bool tryPathTo(const NavGrid& grid, NPCRuntime& n, glm::vec2 goal) {
    std::vector<glm::vec2> path;
    if (findPath(grid, glm::vec2(n.pos.x, n.pos.z), goal, path) && path.size() >= 1) {
        n.path = std::move(path);
        n.waypoint = (n.path.size() > 1) ? 1 : 0;   // skip the start cell
        n.moving   = (n.waypoint < n.path.size());
        n.goalXZ   = goal;
        n.hasGoal  = true;
        return true;
    }
    return false;
}

// Pick a random reachable point a short stroll away and path to it.
static bool pickWanderGoal(const NavGrid& grid, NPCRuntime& n) {
    for (int attempt = 0; attempt < 6; ++attempt) {
        float ang = frand(n.rng) * 6.2831853f;
        float rad = 3.0f + frand(n.rng) * 7.0f;       // 3..10 units away
        glm::vec2 goal(n.pos.x + std::cos(ang) * rad, n.pos.z + std::sin(ang) * rad);
        if (tryPathTo(grid, n, goal)) return true;
    }
    return false;
}

void simulateNPCs(std::vector<NPCRuntime>& npcs,
                  std::vector<SceneObject>& scene,
                  const std::vector<NPCTemplate>& templates,
                  const NavGrid& grid,
                  const Environment& env,
                  const Terrain& terrain,
                  float dt) {
    const float now = env.timeOfDay;
    // Game-hours elapsed this frame (needs only deplete while the clock runs).
    const float gameHours = (env.running && env.dayLengthSec > 0.0f)
                            ? (dt / env.dayLengthSec) * 24.0f : 0.0f;

    for (NPCRuntime& n : npcs) {
        if (n.sceneIndex < 0 || n.sceneIndex >= (int)scene.size()) continue;
        if (n.templateId < 0 || n.templateId >= (int)templates.size()) continue;
        const NPCTemplate& t = templates[n.templateId];

        // Nearest food/water/bed source (so a no-schedule NPC can still find where
        // to eat/drink/sleep). Scanned each frame against the live scene so depleted
        // food/water drop out immediately; beds are reusable and never deplete.
        std::string foodLoc, waterLoc, bedLoc;
        float bestF = 1e30f, bestW = 1e30f, bestB = 1e30f;
        for (const SceneObject& so : scene) {
            if (so.resourceType == RES_NONE) continue;
            glm::vec3 sp = glm::vec3(so.transform[3]);
            float d2 = (sp.x - n.pos.x) * (sp.x - n.pos.x) + (sp.z - n.pos.z) * (sp.z - n.pos.z);
            if (so.resourceType == RES_FOOD  && so.portions > 0 && d2 < bestF) { bestF = d2; foodLoc  = so.name; }
            if (so.resourceType == RES_WATER && so.portions > 0 && d2 < bestW) { bestW = d2; waterLoc = so.name; }
            if (so.resourceType == RES_BED   && d2 < bestB)                    { bestB = d2; bedLoc   = so.name; }
        }

        // 1) DECIDE: re-score the states on a throttle and adopt the winner.
        //    Transitions are emergent — we just move to the highest scorer.
        n.decideTimer -= dt;
        if (n.decideTimer <= 0.0f) {
            n.decideTimer = 0.4f;
            StateChoice c = decideTownsfolk(n, t, now, foodLoc, waterLoc, bedLoc);
            if (c.state != n.state) {
                n.state      = c.state;
                n.hasGoal    = false;       // new state wants a fresh goal
                n.pauseTimer = 0.0f;
            }
            n.targetName = c.loc;           // "" for Wander/Idle
            n.activity   = toLower(npcStateName(n.state));
        }

        // 2) GOAL: resolve the current state's destination and (re)path to it.
        if (n.state == NPCState::Wander) {
            if (n.pauseTimer > 0.0f) {          // dwell at the last stroll point
                n.pauseTimer -= dt;
                n.moving = false;
            } else if (!n.hasGoal) {
                if (!pickWanderGoal(grid, n)) n.pauseTimer = 0.8f;   // boxed in; wait
            }
        } else if (n.state == NPCState::Idle) {
            n.moving = false; n.hasGoal = false;
        } else {
            int ti = findObjectByName(scene, n.targetName);
            if (ti >= 0 && ti != n.sceneIndex) {
                glm::vec3 gp = glm::vec3(scene[ti].transform[3]);
                glm::vec2 g(gp.x, gp.z);
                if (!n.hasGoal || glm::distance(g, n.goalXZ) > 0.5f) {
                    if (!tryPathTo(grid, n, g)) { n.moving = false; n.hasGoal = false; }
                }
            } else { n.moving = false; n.hasGoal = false; }
        }

        // 3) Walk along the current path (shared locomotion layer).
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

        // 4) Needs deplete over game time; the current state restores its need
        //    once the NPC has arrived at (and is standing on) its goal.
        bool arrivedAtGoal = !n.moving && n.hasGoal;
        bool asleep = (n.state == NPCState::Sleep && arrivedAtGoal);
        if (n.state != NPCState::Sleep) n.sleepTimer = 0.0f;   // sleep interrupted -> drop the session

        // Rate multipliers: needs barely move while asleep; energy doesn't drain
        // while asleep and drains at half-rate during the post-sleep "rested" window
        // (so they don't nod off again every few hours).
        float needMul   = asleep ? 0.15f : 1.0f;
        float energyMul = asleep ? 0.0f  : (n.restedTimer > 0.0f ? 0.5f : 1.0f);
        n.hunger -= t.attr.hungerRate * gameHours * needMul;
        n.thirst -= t.attr.thirstRate * gameHours * needMul;
        n.energy -= t.attr.energyRate * gameHours * energyMul;
        if (n.restedTimer > 0.0f && !asleep) n.restedTimer = std::max(0.0f, n.restedTimer - gameHours);

        if (arrivedAtGoal) {
            const float refill = 40.0f * gameHours;   // per game-hour
            if (n.state == NPCState::Sleep) {
                if (n.sleepTimer <= 0.0f)             // begin a sleep session of random length
                    n.sleepTimer = 5.0f + frand(n.rng) * 4.0f;        // 5..9 game-hours
                n.energy += refill;                                   // restore energy while asleep
                n.sleepTimer -= gameHours;
                if (n.sleepTimer <= 0.0f) {           // wake rested: energy drains slower for a while
                    n.sleepTimer  = 0.0f;
                    n.restedTimer = 8.0f + frand(n.rng) * 4.0f;       // 8..12 game-hours
                }
            } else if (n.state == NPCState::Eat || n.state == NPCState::Drink) {
                int ri = findObjectByName(scene, n.targetName);
                int want = (n.state == NPCState::Eat) ? RES_FOOD : RES_WATER;
                if (ri >= 0 && scene[ri].resourceType != RES_NONE) {
                    // A consumable source: take one portion per visit, then feed
                    // while standing here. An empty source provides nothing.
                    SceneObject& src = scene[ri];
                    if (src.resourceType == want && src.portions > 0) {
                        if (n.consumingObj != ri) { src.portions -= 1; n.consumingObj = ri; }
                        if (n.state == NPCState::Eat) n.hunger += refill; else n.thirst += refill;
                    }
                } else if (ri >= 0) {
                    // A plain schedule location (no resource): refill as before.
                    if (n.state == NPCState::Eat) n.hunger += refill; else n.thirst += refill;
                }
            }
        }
        // Release the portion claim once we stop eating/drinking or move on, so the
        // next visit consumes a fresh portion.
        if (n.moving || (n.state != NPCState::Eat && n.state != NPCState::Drink))
            n.consumingObj = -1;
        // Wander: reached the stroll point -> dwell a beat, then pick a new one.
        if (n.state == NPCState::Wander && n.hasGoal && !n.moving) {
            n.hasGoal    = false;
            n.pauseTimer = 1.0f + frand(n.rng) * 2.0f;
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

        // 6) Write position + facing back into the scene transform. The NPC's feet
        //    follow the terrain surface (like the FPS player), so it walks over the
        //    dunes rather than floating at the authored ground height.
        n.pos.y = terrain.enabled ? terrain.heightAt(n.pos.x, n.pos.z) : n.baseY;
        glm::mat4 m = glm::translate(glm::mat4(1.0f), n.pos);
        m = glm::rotate(m, n.facing, glm::vec3(0.0f, 1.0f, 0.0f));
        m = glm::scale(m, n.scale);
        so.transform = m;
    }
}
