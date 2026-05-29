#include "mesh.h"

#include <assimp/Importer.hpp>   // GLB/GLTF/FBX/OBJ mesh loading
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "stb_image.h"           // decode embedded model textures

#include <cstdio>

// ---- Cube geometry (pos + normal per vertex) -----------------------------
static const float kCubeVertices[] = {
    // back face (-Z)
    -0.5f,-0.5f,-0.5f, 0,0,-1,  0.5f,-0.5f,-0.5f, 0,0,-1,  0.5f, 0.5f,-0.5f, 0,0,-1,
     0.5f, 0.5f,-0.5f, 0,0,-1, -0.5f, 0.5f,-0.5f, 0,0,-1, -0.5f,-0.5f,-0.5f, 0,0,-1,
    // front face (+Z)
    -0.5f,-0.5f, 0.5f, 0,0,1,   0.5f,-0.5f, 0.5f, 0,0,1,   0.5f, 0.5f, 0.5f, 0,0,1,
     0.5f, 0.5f, 0.5f, 0,0,1,  -0.5f, 0.5f, 0.5f, 0,0,1,  -0.5f,-0.5f, 0.5f, 0,0,1,
    // left face (-X)
    -0.5f, 0.5f, 0.5f, -1,0,0, -0.5f, 0.5f,-0.5f, -1,0,0, -0.5f,-0.5f,-0.5f, -1,0,0,
    -0.5f,-0.5f,-0.5f, -1,0,0, -0.5f,-0.5f, 0.5f, -1,0,0, -0.5f, 0.5f, 0.5f, -1,0,0,
    // right face (+X)
     0.5f, 0.5f, 0.5f, 1,0,0,   0.5f, 0.5f,-0.5f, 1,0,0,   0.5f,-0.5f,-0.5f, 1,0,0,
     0.5f,-0.5f,-0.5f, 1,0,0,   0.5f,-0.5f, 0.5f, 1,0,0,   0.5f, 0.5f, 0.5f, 1,0,0,
    // bottom face (-Y)
    -0.5f,-0.5f,-0.5f, 0,-1,0,  0.5f,-0.5f,-0.5f, 0,-1,0,  0.5f,-0.5f, 0.5f, 0,-1,0,
     0.5f,-0.5f, 0.5f, 0,-1,0, -0.5f,-0.5f, 0.5f, 0,-1,0, -0.5f,-0.5f,-0.5f, 0,-1,0,
    // top face (+Y)
    -0.5f, 0.5f,-0.5f, 0,1,0,   0.5f, 0.5f,-0.5f, 0,1,0,   0.5f, 0.5f, 0.5f, 0,1,0,
     0.5f, 0.5f, 0.5f, 0,1,0,  -0.5f, 0.5f, 0.5f, 0,1,0,  -0.5f, 0.5f,-0.5f, 0,1,0,
};

AABB computeBounds(const std::vector<SubMesh>& subs) {
    AABB b{ glm::vec3(1e30f), glm::vec3(-1e30f) };
    bool any = false;
    for (const SubMesh& s : subs)
        for (const Vertex& v : s.vertices) {
            b.min = glm::min(b.min, v.pos);
            b.max = glm::max(b.max, v.pos);
            any = true;
        }
    if (!any) { b.min = glm::vec3(-0.5f); b.max = glm::vec3(0.5f); }
    return b;
}

MeshAsset buildCubeMesh() {
    MeshAsset m;
    m.name = "Cube";
    SubMesh s;
    const int vertCount = (int)(sizeof(kCubeVertices) / sizeof(float)) / 6;
    for (int i = 0; i < vertCount; ++i) {
        const float* p = &kCubeVertices[i * 6];
        Vertex v;
        v.pos    = { p[0], p[1], p[2] };
        v.normal = { p[3], p[4], p[5] };
        v.uv     = { 0.0f, 0.0f };
        s.vertices.push_back(v);
        s.indices.push_back((uint32_t)i);
    }
    s.baseColor = glm::vec3(1.0f);   // white: object tint colour shows through
    m.subs.push_back(std::move(s));
    m.bounds = computeBounds(m.subs);
    return m;
}

std::string fileStem(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    size_t start = (slash == std::string::npos) ? 0 : slash + 1;
    size_t dot = path.find_last_of('.');
    size_t end = (dot == std::string::npos || dot < start) ? path.size() : dot;
    return path.substr(start, end - start);
}

bool loadModel(const std::string& path, MeshAsset& out) {
    Assimp::Importer imp;
    const aiScene* sc = imp.ReadFile(path,
        aiProcess_Triangulate | aiProcess_GenSmoothNormals |
        aiProcess_PreTransformVertices | aiProcess_JoinIdenticalVertices);
    if (!sc || (sc->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !sc->mRootNode) {
        std::fprintf(stderr, "Assimp load error: %s\n", imp.GetErrorString());
        return false;
    }
    out = MeshAsset{};
    out.name = fileStem(path);
    out.sourcePath = path;
    for (unsigned mi = 0; mi < sc->mNumMeshes; ++mi) {
        const aiMesh* m = sc->mMeshes[mi];
        SubMesh s;
        s.vertices.reserve(m->mNumVertices);
        for (unsigned v = 0; v < m->mNumVertices; ++v) {
            Vertex vert;
            vert.pos = { m->mVertices[v].x, m->mVertices[v].y, m->mVertices[v].z };
            vert.normal = m->HasNormals()
                ? glm::vec3(m->mNormals[v].x, m->mNormals[v].y, m->mNormals[v].z)
                : glm::vec3(0, 1, 0);
            vert.uv = m->mTextureCoords[0]
                ? glm::vec2(m->mTextureCoords[0][v].x, m->mTextureCoords[0][v].y)
                : glm::vec2(0.0f);
            s.vertices.push_back(vert);
        }
        for (unsigned f = 0; f < m->mNumFaces; ++f) {
            const aiFace& face = m->mFaces[f];
            for (unsigned k = 0; k < face.mNumIndices; ++k)
                s.indices.push_back(face.mIndices[k]);
        }
        glm::vec3 col(0.8f);
        bool hasTex = false;
        if (m->mMaterialIndex < sc->mNumMaterials) {
            aiMaterial* mat = sc->mMaterials[m->mMaterialIndex];
            aiColor4D base; aiColor3D diff;
            if (mat->Get(AI_MATKEY_BASE_COLOR, base) == AI_SUCCESS)        col = { base.r, base.g, base.b };
            else if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, diff) == AI_SUCCESS) col = { diff.r, diff.g, diff.b };

            // Base-colour / diffuse texture (embedded in the GLB). Some exporters
            // don't link it through the standard slots, so fall back to the
            // scene's single embedded texture when the material lookup is empty.
            const aiTexture* t = nullptr;
            aiString texPath;
            if (mat->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath) == AI_SUCCESS ||
                mat->GetTexture(aiTextureType_DIFFUSE,    0, &texPath) == AI_SUCCESS)
                t = sc->GetEmbeddedTexture(texPath.C_Str());
            if (!t && sc->mNumTextures > 0) t = sc->mTextures[0];

            if (t && t->mHeight == 0) {                // compressed (PNG/JPG) bytes
                stbi_set_flip_vertically_on_load(true);
                int w, h, n;
                unsigned char* px = stbi_load_from_memory(
                    reinterpret_cast<const unsigned char*>(t->pcData), (int)t->mWidth, &w, &h, &n, 4);
                if (px) {
                    s.albedo.assign(px, px + (size_t)w * h * 4);
                    s.albedoW = w; s.albedoH = h; hasTex = true;
                    stbi_image_free(px);
                }
            } else if (t) {                            // raw aiTexel (BGRA)
                int w = (int)t->mWidth, h = (int)t->mHeight;
                s.albedo.resize((size_t)w * h * 4);
                for (int i = 0; i < w * h; ++i) {
                    s.albedo[i*4+0] = t->pcData[i].r;
                    s.albedo[i*4+1] = t->pcData[i].g;
                    s.albedo[i*4+2] = t->pcData[i].b;
                    s.albedo[i*4+3] = t->pcData[i].a;
                }
                s.albedoW = w; s.albedoH = h; hasTex = true;
            }
        }
        if (hasTex)                              col = glm::vec3(1.0f);   // show texture fully
        else if (glm::dot(col, col) < 0.0009f)   col = glm::vec3(0.8f);   // black factor -> gray
        s.baseColor = col;
        out.subs.push_back(std::move(s));
    }
    out.bounds = computeBounds(out.subs);
    return !out.subs.empty();
}
