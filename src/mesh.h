#pragma once
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

// ---- Mesh assets (CPU side) ----------------------------------------------
// A mesh asset is a list of submeshes (one per material), each a vertex/index
// buffer + a base colour. The built-in cube is asset #0; loaded models append.
struct Vertex { glm::vec3 pos; glm::vec3 normal; glm::vec2 uv; };
struct AABB    { glm::vec3 min{0.0f}; glm::vec3 max{0.0f}; };
struct SubMesh {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    glm::vec3 baseColor{1.0f};
    std::vector<uint8_t>  albedo;        // RGBA8 pixels, empty if untextured
    int albedoW = 0, albedoH = 0;
};
struct MeshAsset {
    std::string name;
    std::string sourcePath;          // file it was loaded from ("" = built-in)
    std::vector<SubMesh> subs;
    AABB bounds;
};

// Axis-aligned bounds over all submesh vertices (a unit box if there are none).
AABB computeBounds(const std::vector<SubMesh>& subs);

// Build the built-in unit cube as a MeshAsset (pos + normal per vertex).
MeshAsset buildCubeMesh();

// Filename without directory or extension, e.g. "C:/x/teapot.glb" -> "teapot".
std::string fileStem(const std::string& path);

// Load a model file via Assimp into a MeshAsset (one submesh per Assimp mesh,
// with its material base colour + embedded albedo texture). Returns false on
// failure / empty geometry.
bool loadModel(const std::string& path, MeshAsset& out);
