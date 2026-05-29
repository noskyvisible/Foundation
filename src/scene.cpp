#include "scene.h"

#include <glm/gtc/type_ptr.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

// Path written for a library slot with no backing file (built-in cube, or a
// skinned slot that failed to load). Chosen to avoid clashing with real paths.
static const char* kNoPath = "-";

bool saveScene(const std::vector<SceneObject>& scene,
               const std::vector<MeshAsset>& meshLib,
               const std::vector<SkinnedMesh>& skinnedLib,
               const char* path) {
    std::ofstream f(path);
    if (!f) return false;
    f << "FOUNDATION_SCENE 3\n";

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
          << o.npcTemplate << '\n';
    }
    return (bool)f;
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
               const char* path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string header;
    if (!std::getline(f, header) || header.rfind("FOUNDATION_SCENE", 0) != 0) return false;

    int version = 2;
    { std::istringstream hs(header); std::string tag; hs >> tag >> version; }

    if (version < 3) {
        // Legacy file: only object transforms/colours. Leave the libraries as
        // the caller set them up (cube at index 0).
        return loadSceneV2(f, outScene);
    }

    // ---- v3: rebuild the libraries from their manifests ----
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
        { std::istringstream rs(refLine); rs >> o.meshId >> o.skinnedId >> o.animClip >> o.npcTemplate; }
        loaded.push_back(std::move(o));
    }

    // Commit everything only after a fully successful parse.
    meshLib    = std::move(newMesh);
    skinnedLib = std::move(newSkin);
    outScene   = std::move(loaded);
    return true;
}
