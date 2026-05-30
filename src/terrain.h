#pragma once
#include <glm/glm.hpp>
#include <vector>

// ---- Heightmap terrain: a single level-sized block ------------------------
// A res x res grid of heights spanning `worldSize` metres, centred on the origin
// at y=0. A splatmap (RGBA = weights for 4 texture layers) drives multi-texture
// blending. Brushes edit height/splat in place; `version` bumps on any edit so
// each renderer knows to re-upload its GPU copy (like syncMeshes).
//
// Layers: R = sand, G = dirt, B = rock, A = 4th (unused for now).
struct Terrain {
    int   res       = 257;          // vertices per side (res-1 quads)
    float worldSize = 300.0f;       // metres across (X and Z)
    std::vector<float> height;      // res*res world-Y values

    int   splatRes  = 256;          // splatmap resolution
    std::vector<unsigned char> splat;  // splatRes*splatRes*4 (RGBA layer weights)

    unsigned version = 0;           // bumped on any edit (height or splat)
    bool  enabled = true;

    float spacing() const { return worldSize / float(res - 1); }
    float originX() const { return -worldSize * 0.5f; }
    float originZ() const { return -worldSize * 0.5f; }

    // Bilinear height at world (x,z). Returns 0 outside the block.
    float heightAt(float wx, float wz) const;
};

// Allocate and start FLAT at y=0 with an all-sand splat (you sculpt up from here).
void initTerrain(Terrain& t);

// Optional: fill the heightmap with a tapered dune field (for a "reseed" button).
void seedDunes(Terrain& t, float amplitude);

// Build the render mesh from the heightmap: interleaved [pos.xyz, normal.xyz,
// uv.xy] per vertex (8 floats), and triangle indices.
void buildTerrainMesh(const Terrain& t, std::vector<float>& verts,
                      std::vector<unsigned int>& indices);

// ---- Sculpting --------------------------------------------------------------
enum class TerrainTool { Raise, Lower, Smooth, Flatten };

struct TerrainBrush {
    bool        active   = false;          // terrain edit mode on (LMB sculpts)
    TerrainTool tool     = TerrainTool::Raise;
    float       radius   = 12.0f;          // world units
    float       strength = 10.0f;          // raise/lower units per second held
};

// March the ray against the heightfield; returns the world hit point (true) or
// false if it misses. Used to place the brush from a viewport click.
bool raycastTerrain(const Terrain& t, glm::vec3 ro, glm::vec3 rd, glm::vec3& outHit);

// Apply one frame of the brush at `center` (world). Modifies the heightmap in a
// radius and bumps version so renderers re-upload. `dt` scales the strength.
void applyTerrainBrush(Terrain& t, glm::vec3 center, const TerrainBrush& b, float dt);
