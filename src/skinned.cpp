#include "skinned.h"

#include <glm/gtc/matrix_transform.hpp>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "stb_image.h"

#include <cmath>
#include <cstdio>
#include <functional>

// Assimp matrices are row-major; glm is column-major, so transpose on convert.
static glm::mat4 aiToGlm(const aiMatrix4x4& m) {
    return glm::mat4(
        m.a1, m.b1, m.c1, m.d1,
        m.a2, m.b2, m.c2, m.d2,
        m.a3, m.b3, m.c3, m.d3,
        m.a4, m.b4, m.c4, m.d4);
}

// Recursively copy the Assimp node tree into our flat array; returns node index.
static int copySkelNodes(const aiNode* n, SkinnedMesh& sm) {
    int idx = (int)sm.nodes.size();
    sm.nodes.push_back(SkelNode{});            // reserve our slot before recursing
    SkelNode node;
    node.name      = n->mName.C_Str();
    node.localBind = aiToGlm(n->mTransformation);
    auto it = sm.boneIndex.find(node.name);
    node.boneIndex = (it != sm.boneIndex.end()) ? it->second : -1;
    for (unsigned c = 0; c < n->mNumChildren; ++c)
        node.children.push_back(copySkelNodes(n->mChildren[c], sm));
    sm.nodes[idx] = std::move(node);
    return idx;
}

bool loadSkinnedModel(const std::string& path, SkinnedMesh& out) {
    Assimp::Importer imp;
    // NB: no aiProcess_PreTransformVertices -- that flattens the hierarchy and
    // discards the bones we need. LimitBoneWeights caps influences at 4.
    const aiScene* sc = imp.ReadFile(path,
        aiProcess_Triangulate | aiProcess_LimitBoneWeights |
        aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices);
    if (!sc || !sc->mRootNode || sc->mNumMeshes == 0) {
        std::fprintf(stderr, "Skinned load error: %s\n", imp.GetErrorString());
        return false;
    }
    out = SkinnedMesh{};
    out.name = fileStem(path);
    out.sourcePath = path;

    const aiMesh* m = sc->mMeshes[0];
    out.vertices.resize(m->mNumVertices);
    for (unsigned v = 0; v < m->mNumVertices; ++v) {
        SkinnedVertex sv;
        sv.pos    = { m->mVertices[v].x, m->mVertices[v].y, m->mVertices[v].z };
        sv.normal = m->HasNormals() ? glm::vec3(m->mNormals[v].x, m->mNormals[v].y, m->mNormals[v].z)
                                    : glm::vec3(0, 1, 0);
        sv.uv     = m->mTextureCoords[0] ? glm::vec2(m->mTextureCoords[0][v].x, m->mTextureCoords[0][v].y)
                                         : glm::vec2(0.0f);
        out.vertices[v] = sv;
    }
    for (unsigned f = 0; f < m->mNumFaces; ++f)
        for (unsigned k = 0; k < m->mFaces[f].mNumIndices; ++k)
            out.indices.push_back(m->mFaces[f].mIndices[k]);

    // Bones: each carries a name, an inverse-bind (offset) matrix, and the
    // vertices it influences. Bone slot = index into mBones.
    out.boneOffsets.resize(m->mNumBones);
    for (unsigned b = 0; b < m->mNumBones; ++b) {
        const aiBone* bone = m->mBones[b];
        out.boneIndex[bone->mName.C_Str()] = (int)b;
        out.boneOffsets[b] = aiToGlm(bone->mOffsetMatrix);
        for (unsigned w = 0; w < bone->mNumWeights; ++w) {
            const aiVertexWeight& vw = bone->mWeights[w];
            if (vw.mVertexId < out.vertices.size())
                out.vertices[vw.mVertexId].addWeight((int)b, vw.mWeight);
        }
    }
    for (SkinnedVertex& sv : out.vertices) sv.normalizeWeights();

    out.root = copySkelNodes(sc->mRootNode, out);
    out.globalInverse = glm::inverse(aiToGlm(sc->mRootNode->mTransformation));

    // Animation clips (one named clip per glTF animation).
    for (unsigned a = 0; a < sc->mNumAnimations; ++a) {
        const aiAnimation* anim = sc->mAnimations[a];
        Animation clip;
        clip.name        = anim->mName.C_Str();
        clip.duration    = (float)anim->mDuration;
        clip.ticksPerSec = anim->mTicksPerSecond > 0.0 ? (float)anim->mTicksPerSecond : 25.0f;
        for (unsigned c = 0; c < anim->mNumChannels; ++c) {
            const aiNodeAnim* ch = anim->mChannels[c];
            AnimChannel out_ch;
            for (unsigned k = 0; k < ch->mNumPositionKeys; ++k)
                out_ch.pos.push_back({ (float)ch->mPositionKeys[k].mTime,
                    glm::vec3(ch->mPositionKeys[k].mValue.x, ch->mPositionKeys[k].mValue.y, ch->mPositionKeys[k].mValue.z) });
            for (unsigned k = 0; k < ch->mNumRotationKeys; ++k) {
                const aiQuaternion& q = ch->mRotationKeys[k].mValue;
                out_ch.rot.push_back({ (float)ch->mRotationKeys[k].mTime, glm::quat(q.w, q.x, q.y, q.z) });
            }
            for (unsigned k = 0; k < ch->mNumScalingKeys; ++k)
                out_ch.scl.push_back({ (float)ch->mScalingKeys[k].mTime,
                    glm::vec3(ch->mScalingKeys[k].mValue.x, ch->mScalingKeys[k].mValue.y, ch->mScalingKeys[k].mValue.z) });
            clip.channels[ch->mNodeName.C_Str()] = std::move(out_ch);
        }
        out.animations.push_back(std::move(clip));
    }

    // Bounds from the BIND-POSE skinned geometry, not the raw vertices: glTF can
    // bake a root scale into the skeleton (this asset has an Armature node at
    // scale 0.01, so the skinned mesh comes out ~100x its metric size). Solving
    // the bind pose gives the size we actually draw, so the box/pick match.
    std::vector<glm::mat4> bindPal;
    computeBoneMatrices(out, Animation{}, 0.0f, bindPal);
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const SkinnedVertex& sv : out.vertices) {
        glm::mat4 skin =
            sv.weights[0] * bindPal[sv.boneIds[0]] + sv.weights[1] * bindPal[sv.boneIds[1]] +
            sv.weights[2] * bindPal[sv.boneIds[2]] + sv.weights[3] * bindPal[sv.boneIds[3]];
        glm::vec3 p = glm::vec3(skin * glm::vec4(sv.pos, 1.0f));
        lo = glm::min(lo, p); hi = glm::max(hi, p);
    }
    if (out.vertices.empty() || bindPal.empty()) { lo = glm::vec3(-0.5f); hi = glm::vec3(0.5f); }
    out.bounds = { lo, hi };   // kept in bind/model space; importFix orients it for display

    // Auto-fit display transform. Assimp imports this glTF skeleton Z-up with the
    // head at -Z (bind bounds: tall axis is Z, running 0 -> -190). So:
    //   1. orient: if the tall axis is Z, rotate +90deg about X (maps -Z -> +Y);
    //   2. scale uniformly to ~1.8u tall in the oriented frame;
    //   3. centre in X/Z and drop the feet (min Y) to the ground plane.
    // Applied as model * importFix, so authored object transforms still work.
    glm::vec3 ext = hi - lo;
    glm::mat4 orient(1.0f);
    if (ext.z > ext.y && ext.z > 1e-4f)   // Z is the tall axis -> stand it up (-Z to +Y)
        orient = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1, 0, 0));
    // Oriented bounds: transform the 8 corners and re-extract min/max.
    glm::vec3 olo(1e30f), ohi(-1e30f);
    for (int cx = 0; cx < 2; ++cx) for (int cy = 0; cy < 2; ++cy) for (int cz = 0; cz < 2; ++cz) {
        glm::vec3 corner(cx ? hi.x : lo.x, cy ? hi.y : lo.y, cz ? hi.z : lo.z);
        glm::vec3 p = glm::vec3(orient * glm::vec4(corner, 1.0f));
        olo = glm::min(olo, p); ohi = glm::max(ohi, p);
    }
    glm::vec3 oext = glm::max(ohi - olo, glm::vec3(1e-4f));
    float fit = 1.8f / oext.y;
    glm::vec3 octr = 0.5f * (olo + ohi);
    out.importFix = glm::translate(glm::mat4(1.0f), glm::vec3(-octr.x * fit, -olo.y * fit, -octr.z * fit))
                  * glm::scale(glm::mat4(1.0f), glm::vec3(fit))
                  * orient;

    // Embedded base-colour texture (same handling as static meshes).
    glm::vec3 col(0.8f); bool hasTex = false;
    if (m->mMaterialIndex < sc->mNumMaterials) {
        aiMaterial* mat = sc->mMaterials[m->mMaterialIndex];
        aiColor4D base; aiColor3D diff;
        if (mat->Get(AI_MATKEY_BASE_COLOR, base) == AI_SUCCESS)         col = { base.r, base.g, base.b };
        else if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, diff) == AI_SUCCESS) col = { diff.r, diff.g, diff.b };
        const aiTexture* t = nullptr; aiString texPath;
        if (mat->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_DIFFUSE,    0, &texPath) == AI_SUCCESS)
            t = sc->GetEmbeddedTexture(texPath.C_Str());
        if (!t && sc->mNumTextures > 0) t = sc->mTextures[0];
        if (t && t->mHeight == 0) {
            stbi_set_flip_vertically_on_load(true);
            int w, h, n;
            unsigned char* px = stbi_load_from_memory(
                reinterpret_cast<const unsigned char*>(t->pcData), (int)t->mWidth, &w, &h, &n, 4);
            if (px) { out.albedo.assign(px, px + (size_t)w * h * 4); out.albedoW = w; out.albedoH = h; hasTex = true; stbi_image_free(px); }
        } else if (t) {
            int w = (int)t->mWidth, h = (int)t->mHeight;
            out.albedo.resize((size_t)w * h * 4);
            for (int i = 0; i < w * h; ++i) {
                out.albedo[i*4+0] = t->pcData[i].r; out.albedo[i*4+1] = t->pcData[i].g;
                out.albedo[i*4+2] = t->pcData[i].b; out.albedo[i*4+3] = t->pcData[i].a;
            }
            out.albedoW = w; out.albedoH = h; hasTex = true;
        }
    }
    out.baseColor = hasTex ? glm::vec3(1.0f) : (glm::dot(col, col) < 0.0009f ? glm::vec3(0.8f) : col);
    return !out.vertices.empty() && !out.boneOffsets.empty();
}

// ---- Keyframe sampling ----------------------------------------------------
static glm::vec3 sampleVec(const std::vector<Key<glm::vec3>>& keys, float t, glm::vec3 fallback) {
    if (keys.empty()) return fallback;
    if (keys.size() == 1 || t <= keys.front().t) return keys.front().v;
    if (t >= keys.back().t) return keys.back().v;
    for (size_t i = 0; i + 1 < keys.size(); ++i)
        if (t < keys[i + 1].t) {
            float span = keys[i + 1].t - keys[i].t;
            float f = span > 1e-6f ? (t - keys[i].t) / span : 0.0f;
            return glm::mix(keys[i].v, keys[i + 1].v, f);
        }
    return keys.back().v;
}
static glm::quat sampleQuat(const std::vector<Key<glm::quat>>& keys, float t) {
    if (keys.empty()) return glm::quat(1, 0, 0, 0);
    if (keys.size() == 1 || t <= keys.front().t) return keys.front().v;
    if (t >= keys.back().t) return keys.back().v;
    for (size_t i = 0; i + 1 < keys.size(); ++i)
        if (t < keys[i + 1].t) {
            float span = keys[i + 1].t - keys[i].t;
            float f = span > 1e-6f ? (t - keys[i].t) / span : 0.0f;
            return glm::normalize(glm::slerp(keys[i].v, keys[i + 1].v, f));
        }
    return keys.back().v;
}

void computeBoneMatrices(const SkinnedMesh& sm, const Animation& clip,
                         float timeSec, std::vector<glm::mat4>& out) {
    out.assign(sm.boneOffsets.size(), glm::mat4(1.0f));
    if (sm.root < 0) return;
    float tick = 0.0f;
    if (clip.duration > 0.0f) {
        float tps = clip.ticksPerSec > 0.0f ? clip.ticksPerSec : 25.0f;
        tick = std::fmod(timeSec * tps, clip.duration);
        if (tick < 0.0f) tick += clip.duration;
    }
    std::function<void(int, const glm::mat4&)> rec = [&](int ni, const glm::mat4& parent) {
        const SkelNode& n = sm.nodes[ni];
        glm::mat4 local = n.localBind;
        auto it = clip.channels.find(n.name);
        if (it != clip.channels.end()) {
            glm::vec3 p = sampleVec(it->second.pos, tick, glm::vec3(n.localBind[3]));
            glm::quat q = sampleQuat(it->second.rot, tick);
            glm::vec3 s = sampleVec(it->second.scl, tick, glm::vec3(1.0f));
            local = glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(q)
                  * glm::scale(glm::mat4(1.0f), s);
        }
        glm::mat4 global = parent * local;
        if (n.boneIndex >= 0 && n.boneIndex < (int)out.size())
            out[n.boneIndex] = sm.globalInverse * global * sm.boneOffsets[n.boneIndex];
        for (int c : n.children) rec(c, global);
    };
    rec(sm.root, glm::mat4(1.0f));
}
