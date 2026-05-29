#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>   // quaternion keyframes

#include "mesh.h"                   // AABB

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// ---- Skeletal (skinned) mesh + animation ---------------------------------
// Loaded from a single GLB that contains the mesh, the skeleton (with explicit
// inverse-bind matrices), and one or more named animation clips.

struct SkinnedVertex {
    glm::vec3 pos{0.0f};
    glm::vec3 normal{0, 1, 0};
    glm::vec2 uv{0.0f};
    int   boneIds[4] = {0, 0, 0, 0};
    float weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    void addWeight(int id, float w) {
        if (w <= 0.0f) return;
        for (int k = 0; k < 4; ++k)
            if (weights[k] == 0.0f) { boneIds[k] = id; weights[k] = w; return; }
        // All 4 slots full (LimitBoneWeights should prevent this) -> replace
        // the smallest if this one is larger.
        int sm = 0;
        for (int k = 1; k < 4; ++k) if (weights[k] < weights[sm]) sm = k;
        if (w > weights[sm]) { boneIds[sm] = id; weights[sm] = w; }
    }
    void normalizeWeights() {
        float s = weights[0] + weights[1] + weights[2] + weights[3];
        if (s > 1e-6f) for (float& w : weights) w /= s;
        else weights[0] = 1.0f;   // unweighted vertex -> pin to bone 0
    }
};

template <class T> struct Key { float t; T v; };
struct AnimChannel {
    std::vector<Key<glm::vec3>> pos;
    std::vector<Key<glm::quat>> rot;
    std::vector<Key<glm::vec3>> scl;
};
struct Animation {
    std::string name;
    float duration   = 0.0f;     // in ticks
    float ticksPerSec = 25.0f;
    std::unordered_map<std::string, AnimChannel> channels;   // keyed by node name
    float seconds() const { return ticksPerSec > 0.0f ? duration / ticksPerSec : 0.0f; }
};

struct SkelNode {
    std::string name;
    glm::mat4 localBind{1.0f};   // node's own transform in the bind pose
    int boneIndex = -1;          // -1 if this node isn't a skinning bone
    std::vector<int> children;
};

struct SkinnedMesh {
    std::string name;
    std::string sourcePath;                   // file it was loaded from
    std::vector<SkinnedVertex> vertices;
    std::vector<uint32_t>      indices;
    std::vector<glm::mat4>     boneOffsets;   // inverse-bind per bone (from glTF)
    std::unordered_map<std::string, int> boneIndex;   // node name -> bone slot
    std::vector<SkelNode> nodes;              // flattened hierarchy
    int       root = -1;
    glm::mat4 globalInverse{1.0f};
    AABB      bounds;
    glm::mat4 importFix{1.0f};                // applied after the object transform
    std::vector<Animation> animations;
    std::vector<uint8_t> albedo; int albedoW = 0, albedoH = 0;
    glm::vec3 baseColor{1.0f};
    int animIndex(const std::string& n) const {
        for (size_t i = 0; i < animations.size(); ++i) if (animations[i].name == n) return (int)i;
        return -1;
    }
};

// Load a rigged GLB/FBX (mesh + skeleton + clips) into `out`. Computes the
// bind-pose bounds and an auto-fit importFix (stand up / scale / ground).
bool loadSkinnedModel(const std::string& path, SkinnedMesh& out);

// Walk the hierarchy at `timeSec`, sampling `clip` per node (an empty clip gives
// the bind pose), and fill out[bone] = globalInverse * animGlobal * offset.
void computeBoneMatrices(const SkinnedMesh& sm, const Animation& clip,
                         float timeSec, std::vector<glm::mat4>& out);
