#include "scene.h"

#include <glm/gtc/type_ptr.hpp>

#include "environment.h"   // Environment + Weather (full defs for save/load)
#include "terrain.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

// Path written for a library slot with no backing file (built-in cube, or a
// skinned slot that failed to load). Chosen to avoid clashing with real paths.
static const char* kNoPath = "-";

// ---- terrain binary sidecar ----------------------------------------------
// Compact binary next to the scene file: magic, res, worldSize, splatRes, then
// the raw height (res*res floats) and splat (splatRes*splatRes*4 bytes) arrays.
static void saveTerrainBin(const Terrain& t, const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return;
    f.write("FTERR1", 6);
    f.write((const char*)&t.res, sizeof(int));
    f.write((const char*)&t.worldSize, sizeof(float));
    f.write((const char*)&t.splatRes, sizeof(int));
    f.write((const char*)t.height.data(), (std::streamsize)(t.height.size() * sizeof(float)));
    f.write((const char*)t.splat.data(), (std::streamsize)t.splat.size());
}

static bool loadTerrainBin(Terrain& t, const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[6] = {0};
    f.read(magic, 6);
    if (std::memcmp(magic, "FTERR1", 6) != 0) return false;
    int res = 0, splatRes = 0; float worldSize = 0.0f;
    f.read((char*)&res, sizeof(int));
    f.read((char*)&worldSize, sizeof(float));
    f.read((char*)&splatRes, sizeof(int));
    if (!f || res <= 1 || res > 4097 || splatRes <= 0 || splatRes > 8192) return false;
    Terrain nt;
    nt.res = res; nt.worldSize = worldSize; nt.splatRes = splatRes;
    nt.height.assign((size_t)res * res, 0.0f);
    nt.splat.assign((size_t)splatRes * splatRes * 4, 0);
    f.read((char*)nt.height.data(), (std::streamsize)(nt.height.size() * sizeof(float)));
    f.read((char*)nt.splat.data(), (std::streamsize)nt.splat.size());
    if (!f) return false;
    nt.version = t.version + 1;     // force renderers to re-upload
    nt.enabled = t.enabled;
    t = std::move(nt);
    return true;
}

bool saveScene(const std::vector<SceneObject>& scene,
               const std::vector<MeshAsset>& meshLib,
               const std::vector<SkinnedMesh>& skinnedLib,
               const std::vector<NPCTemplate>& npcTemplates,
               const Environment& env,
               const Terrain& terrain,
               const PlayerStart& player,
               const char* path) {
    std::ofstream f(path);
    if (!f) return false;
    f << "FOUNDATION_SCENE 4\n";

    // Library manifests: one source path per slot (paths may contain spaces, so
    // each gets its own line). Index order is preserved so object refs stay valid.
    f << "MESHLIB " << meshLib.size() << "\n";
    for (const MeshAsset& m : meshLib)
        f << (m.sourcePath.empty() ? kNoPath : m.sourcePath) << "\n";
    f << "SKINLIB " << skinnedLib.size() << "\n";
    for (const SkinnedMesh& s : skinnedLib)
        f << (s.sourcePath.empty() ? kNoPath : s.sourcePath) << "\n";

    f << "OBJECTS " << scene.size() << "\n";
    for (const SceneObject& o : scene) {
        f << o.name << "\n";
        const float* m = glm::value_ptr(o.transform);
        for (int i = 0; i < 16; ++i) f << m[i] << (i == 15 ? '\n' : ' ');
        f << o.color.r << ' ' << o.color.g << ' ' << o.color.b << '\n';
        f << o.meshId << ' ' << o.skinnedId << ' ' << o.animClip << ' '
          << o.npcTemplate << ' ' << o.resourceType << ' ' << o.portions << '\n';
    }

    // NPC templates: attributes, clip refs, custom fields, daily schedule.
    f << "NPCTEMPLATES " << npcTemplates.size() << "\n";
    for (const NPCTemplate& t : npcTemplates) {
        f << t.name << "\n";
        f << t.meshId << ' ' << t.skinnedId << ' ' << t.clipIdle << ' '
          << t.clipWalk << ' ' << t.clipRun << ' ' << t.runSpeed << "\n";
        const NPCAttributes& a = t.attr;
        f << a.health << ' ' << a.hunger << ' ' << a.hungerRate << ' '
          << a.thirst << ' ' << a.thirstRate << ' ' << a.energy << ' '
          << a.energyRate << ' ' << a.gold << ' ' << a.moveSpeed << "\n";
        f << "CUSTOM " << t.custom.size() << "\n";
        for (const auto& c : t.custom) f << c.first << "\n" << c.second << "\n";
        f << "SCHEDULE " << t.schedule.size() << "\n";
        for (const ScheduleEntry& s : t.schedule)
            f << s.hour << "\n" << s.activity << "\n" << s.location << "\n";
    }

    // Environment / weather / fog (authored fields only; derived state recomputes).
    f << "ENVIRONMENT\n";
    f << "TIME " << env.timeOfDay << ' ' << env.day << ' ' << env.dayLengthSec
      << ' ' << (env.running ? 1 : 0) << "\n";
    f << "EXPOSURE " << env.exposure << "\n";
    f << "FOG " << env.fogDensity << ' ' << env.fogFalloff << ' '
      << env.fogTint.r << ' ' << env.fogTint.g << ' ' << env.fogTint.b << "\n";
    f << "WEATHER " << (int)env.weather.current << ' '
      << (env.weather.autoCycle ? 1 : 0) << ' ' << env.weather.blendSpeed << "\n";

    // Player start + FPS controller tuning.
    f << "PLAYERSTART " << player.pos.x << ' ' << player.pos.y << ' ' << player.pos.z
      << ' ' << player.yaw << "\n";
    f << "PLAYERCFG " << player.eyeHeight << ' ' << player.radius << ' '
      << player.walkSpeed << ' ' << player.runSpeed << ' ' << player.jumpSpeed << ' '
      << player.gravity << ' ' << player.mouseSens << ' ' << (player.fps ? 1 : 0) << "\n";

    bool ok = (bool)f;
    f.close();
    saveTerrainBin(terrain, std::string(path) + ".terrain");
    return ok;
}

// Read "TAG <count>" off one line; returns the count (-1 on tag mismatch).
static int readCount(std::ifstream& f, const char* tag) {
    std::string line;
    if (!std::getline(f, line)) return -1;
    std::istringstream ss(line);
    std::string got; int n = -1;
    ss >> got >> n;
    return got == tag ? n : -1;
}

// v2 (legacy): header already consumed; the next line is the object count, then
// per object a name line, 16 floats, and an RGB line. No library refs.
static bool loadSceneV2(std::ifstream& f, std::vector<SceneObject>& out) {
    std::string countLine;
    if (!std::getline(f, countLine)) return false;
    int count = std::atoi(countLine.c_str());
    std::vector<SceneObject> loaded;
    for (int i = 0; i < count; ++i) {
        SceneObject o;
        if (!std::getline(f, o.name)) return false;
        std::string nums;
        if (!std::getline(f, nums)) return false;
        std::istringstream ss(nums);
        float* m = glm::value_ptr(o.transform);
        for (int k = 0; k < 16; ++k) ss >> m[k];
        std::string colLine;
        if (std::getline(f, colLine)) {
            std::istringstream cs(colLine);
            cs >> o.color.r >> o.color.g >> o.color.b;
        }
        loaded.push_back(o);
    }
    out = std::move(loaded);
    return true;
}

bool loadScene(std::vector<SceneObject>& outScene,
               std::vector<MeshAsset>& meshLib,
               std::vector<SkinnedMesh>& skinnedLib,
               std::vector<NPCTemplate>& npcTemplates,
               Environment& env,
               Terrain& terrain,
               PlayerStart& player,
               const char* path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string header;
    if (!std::getline(f, header) || header.rfind("FOUNDATION_SCENE", 0) != 0) return false;

    int version = 2;
    { std::istringstream hs(header); std::string tag; hs >> tag >> version; }

    if (version < 3) {
        // Legacy file: only object transforms/colours. Leave the libraries as
        // the caller set them up (cube at index 0). Terrain sidecar isn't loaded.
        return loadSceneV2(f, outScene);
    }

    // ---- v3/v4: rebuild the libraries from their manifests ----
    int meshN = readCount(f, "MESHLIB");
    if (meshN < 0) return false;
    std::vector<MeshAsset> newMesh;
    newMesh.reserve(meshN);
    for (int i = 0; i < meshN; ++i) {
        std::string p;
        if (!std::getline(f, p)) return false;
        MeshAsset asset;
        if (p == kNoPath) {
            asset = buildCubeMesh();               // built-in slot
        } else if (!loadModel(p, asset)) {
            std::fprintf(stderr, "loadScene: mesh '%s' failed; using a cube placeholder.\n", p.c_str());
            asset = buildCubeMesh();
        }
        newMesh.push_back(std::move(asset));
    }

    int skinN = readCount(f, "SKINLIB");
    if (skinN < 0) return false;
    std::vector<SkinnedMesh> newSkin;
    newSkin.reserve(skinN);
    for (int i = 0; i < skinN; ++i) {
        std::string p;
        if (!std::getline(f, p)) return false;
        SkinnedMesh sm;
        if (p != kNoPath && !loadSkinnedModel(p, sm))
            std::fprintf(stderr, "loadScene: skinned '%s' failed; slot left empty.\n", p.c_str());
        newSkin.push_back(std::move(sm));          // empty slot keeps indices aligned
    }

    int count = readCount(f, "OBJECTS");
    if (count < 0) return false;
    std::vector<SceneObject> loaded;
    loaded.reserve(count);
    for (int i = 0; i < count; ++i) {
        SceneObject o;
        if (!std::getline(f, o.name)) return false;
        std::string nums;
        if (!std::getline(f, nums)) return false;
        std::istringstream ss(nums);
        float* m = glm::value_ptr(o.transform);
        for (int k = 0; k < 16; ++k) ss >> m[k];
        std::string colLine;
        if (!std::getline(f, colLine)) return false;
        { std::istringstream cs(colLine); cs >> o.color.r >> o.color.g >> o.color.b; }
        std::string refLine;
        if (!std::getline(f, refLine)) return false;
        { std::istringstream rs(refLine);
          rs >> o.meshId >> o.skinnedId >> o.animClip >> o.npcTemplate;
          rs >> o.resourceType >> o.portions; }   // optional (older saves: stays 0/0)
        loaded.push_back(std::move(o));
    }

    // ---- v4 extras: NPC templates + environment (tolerant of absence) ----
    std::vector<NPCTemplate> newTemplates;
    bool haveTemplates = false;
    if (version >= 4) {
        int tN = readCount(f, "NPCTEMPLATES");
        if (tN >= 0) {
            haveTemplates = true;
            for (int i = 0; i < tN; ++i) {
                NPCTemplate t;
                if (!std::getline(f, t.name)) break;
                { std::string l; std::getline(f, l); std::istringstream s(l);
                  s >> t.meshId >> t.skinnedId >> t.clipIdle >> t.clipWalk >> t.clipRun >> t.runSpeed; }
                { std::string l; std::getline(f, l); std::istringstream s(l); NPCAttributes& a = t.attr;
                  s >> a.health >> a.hunger >> a.hungerRate >> a.thirst >> a.thirstRate
                    >> a.energy >> a.energyRate >> a.gold >> a.moveSpeed; }
                int ck = readCount(f, "CUSTOM");
                for (int c = 0; c < ck; ++c) {
                    std::string k, v; std::getline(f, k); std::getline(f, v);
                    t.custom.push_back({ k, (float)std::atof(v.c_str()) });
                }
                int sk = readCount(f, "SCHEDULE");
                for (int s = 0; s < sk; ++s) {
                    std::string h, act, loc;
                    std::getline(f, h); std::getline(f, act); std::getline(f, loc);
                    ScheduleEntry e; e.hour = (float)std::atof(h.c_str());
                    e.activity = act; e.location = loc;
                    t.schedule.push_back(std::move(e));
                }
                newTemplates.push_back(std::move(t));
            }
        }
        // Environment: labelled lines to EOF; unknown tags (incl. "ENVIRONMENT")
        // are ignored, and any missing field keeps the caller's current value.
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream s(line); std::string tag; s >> tag;
            if (tag == "TIME") { int run = 1; s >> env.timeOfDay >> env.day >> env.dayLengthSec >> run; env.running = run != 0; }
            else if (tag == "EXPOSURE") s >> env.exposure;
            else if (tag == "FOG") s >> env.fogDensity >> env.fogFalloff >> env.fogTint.x >> env.fogTint.y >> env.fogTint.z;
            else if (tag == "WEATHER") {
                int c = 1, ac = 1; float bs = 0.4f; s >> c >> ac >> bs;
                if (c < 0) c = 0; if (c >= kWeatherCount) c = kWeatherCount - 1;
                env.weather.current = (WeatherType)c; env.weather.autoCycle = ac != 0; env.weather.blendSpeed = bs;
            }
            else if (tag == "PLAYERSTART") s >> player.pos.x >> player.pos.y >> player.pos.z >> player.yaw;
            else if (tag == "PLAYERCFG") {
                int fps = 1;
                s >> player.eyeHeight >> player.radius >> player.walkSpeed >> player.runSpeed
                  >> player.jumpSpeed >> player.gravity >> player.mouseSens >> fps;
                player.fps = fps != 0;
            }
        }
    }

    // Commit everything only after a fully successful parse.
    meshLib    = std::move(newMesh);
    skinnedLib = std::move(newSkin);
    outScene   = std::move(loaded);
    if (haveTemplates) npcTemplates = std::move(newTemplates);

    // Terrain sidecar (independent of version; absent for v2/v3 -> leave as is).
    loadTerrainBin(terrain, std::string(path) + ".terrain");
    return true;
}
