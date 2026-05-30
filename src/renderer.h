#pragma once
#include <glad/gl.h>
#include <glm/glm.hpp>

#include "camera.h"
#include "environment.h"
#include "mesh.h"
#include "skinned.h"
#include "terrain.h"

#include <vector>

// ---- Offscreen render target (one per viewport) --------------------------
struct RenderTarget {
    GLuint fbo = 0;
    GLuint color = 0;   // texture we display in the UI
    GLuint depth = 0;   // depth-stencil renderbuffer
    int width = 0;
    int height = 0;
};

// (Re)allocate the FBO's attachments when the panel size changes.
void resizeTarget(RenderTarget& rt, int w, int h);
void destroyTarget(RenderTarget& rt);

// Reference grid: base mesh spacing; scaled at draw time.
inline constexpr float kGridStep = 0.5f;

void makeGrid(GLuint& vao, GLuint& vbo, int& outVertexCount);
void makeSelectionBox(GLuint& vao, GLuint& vbo, int& outVertexCount);

// GPU side of a mesh asset uploaded to the CURRENT GL context (one per submesh).
struct GPUSubMesh { GLuint vao = 0, vbo = 0, ebo = 0; GLsizei count = 0; glm::vec3 baseColor{1.0f}; GLuint tex = 0; };
struct GPUMesh    { std::vector<GPUSubMesh> subs; AABB bounds; };

// GPU side of a skinned mesh (one VAO; bone palette is a per-frame uniform).
struct GPUSkinned {
    GLuint vao = 0, vbo = 0, ebo = 0; GLsizei count = 0;
    GLuint tex = 0; glm::vec3 baseColor{1.0f};
    AABB bounds; glm::mat4 importFix{1.0f};
    int numBones = 0;
};

// One skinned object to draw this frame: which GPU mesh, its world transform,
// tint, and the bone palette computed for the current animation time.
struct SkinnedDrawItem {
    int skinnedId = -1;
    int sceneIndex = -1;                          // for selection-box matching
    glm::mat4 model{1.0f};
    glm::vec3 color{1.0f};
    const std::vector<glm::mat4>* palette = nullptr;
};

// Bundles the GPU resources a window needs to draw. VAOs are NOT shared between
// GL contexts, so each window (editor / play) builds its own Renderer.
struct Renderer {
    GLuint lineProgram = 0;            // grid + selection box (pos+colour, uMVP)
    GLint  lineMvpLoc = -1;
    GLuint litProgram = 0;             // objects (pos+normal+uv, lit)
    GLint  litMvpLoc = -1, litModelLoc = -1, litColorLoc = -1, litHasTexLoc = -1, litAlbedoLoc = -1;
    GLint  litLightDirLoc = -1, litLightColLoc = -1, litAmbientLoc = -1;
    GLint  litCamLoc = -1, litFogColLoc = -1, litFogDenLoc = -1, litFogFalLoc = -1;
    GLuint skinProgram = 0;            // skinned objects (adds bone palette)
    GLint  skinMvpLoc = -1, skinModelLoc = -1, skinColorLoc = -1, skinHasTexLoc = -1, skinAlbedoLoc = -1;
    GLint  skinLightDirLoc = -1, skinLightColLoc = -1, skinAmbientLoc = -1, skinBonesLoc = -1;
    GLint  skinCamLoc = -1, skinFogColLoc = -1, skinFogDenLoc = -1, skinFogFalLoc = -1;
    GLuint groundProgram = 0, groundVao = 0, groundVbo = 0, groundTex = 0;  // textured ground plane
    GLint  groundVPLoc = -1, groundCamLoc = -1, groundHalfLoc = -1, groundUvLoc = -1;
    GLint  groundLightDirLoc = -1, groundLightColLoc = -1, groundAmbientLoc = -1;
    GLint  groundAlbedoLoc = -1, groundFogColLoc = -1, groundFogDenLoc = -1, groundFogFalLoc = -1;
    GLuint terrainProgram = 0, terrainVao = 0, terrainVbo = 0, terrainEbo = 0;  // heightmap terrain
    GLuint terrainSplatTex = 0, terrainLayerTex[4] = {0,0,0,0};
    GLsizei terrainIndexCount = 0;
    unsigned terrainVersion = 0;       // last-uploaded Terrain version (re-upload when it differs)
    GLint  terVPLoc = -1, terLightDirLoc = -1, terLightColLoc = -1, terAmbientLoc = -1;
    GLint  terSplatLoc = -1, terTexLoc[4] = {-1,-1,-1,-1}, terTileLoc = -1;
    GLint  terCamLoc = -1, terFogColLoc = -1, terFogDenLoc = -1, terFogFalLoc = -1;
    GLuint skyProgram = 0, skyVao = 0; // procedural sky (fullscreen triangle)
    GLint  skyInvVPLoc = -1, skyCamLoc = -1, skySunLoc = -1, skySunColLoc = -1;
    GLint  skyMoonLoc = -1, skyTimeLoc = -1, skyCloudLoc = -1, skyExposureLoc = -1;
    GLint  skyHazeColLoc = -1, skyHazeLoc = -1;
    GLint  skyLightningLoc = -1, skyBoltAzLoc = -1, skyBoltSeedLoc = -1;
    GLuint rainProgram = 0, rainVao = 0, rainQuadVbo = 0, rainInstVbo = 0;  // instanced rain
    int    rainDrops = 0;              // number of drop instances in the buffer
    GLint  rainVPLoc = -1, rainCamLoc = -1, rainDispLoc = -1, rainDirLoc = -1;
    GLint  rainBoxLoc = -1, rainLenLoc = -1;
    GLint  rainWidthLoc = -1, rainColorLoc = -1, rainIntenLoc = -1;
    GLuint gridVao = 0, gridVbo = 0;   // pos+colour
    GLuint selBoxVao = 0, selBoxVbo = 0;
    int    gridVertexCount = 0;
    int    selBoxVertexCount = 0;
    std::vector<GPUMesh> meshes;       // parallel to the CPU mesh library
    std::vector<GPUSkinned> skinned;   // parallel to the CPU skinned library
};

GPUMesh uploadMesh(const MeshAsset& asset);
void syncMeshes(Renderer& r, const std::vector<MeshAsset>& lib);
GPUSkinned uploadSkinnedMesh(const SkinnedMesh& sm);
void syncSkinned(Renderer& r, const std::vector<SkinnedMesh>& lib);

// (Re)upload the terrain mesh + splatmap to this renderer's GL context when the
// terrain's version differs from what we last uploaded. Cheap no-op otherwise.
void syncTerrain(Renderer& r, const Terrain& t);

// Free all cached GPU mesh/skinned uploads (must run with this renderer's GL
// context current). Call after the CPU library is replaced wholesale, e.g. on
// scene load; the next syncMeshes/syncSkinned re-uploads from scratch.
void invalidateGPUMeshes(Renderer& r);

// Build per-context shared resources (programs, grid, selection box).
Renderer createRenderer();
void destroyRenderer(Renderer& r);

// Draw the grid + objects into the target, seen through the given camera.
// `models`, `colors`, `meshIds` are parallel per-object arrays. The renderer's
// meshes must already be synced to the library (see syncMeshes).
void renderScene(GLuint fbo, int width, int height, const Camera& cam,
                 const glm::mat4& view, const glm::mat4& proj, const Renderer& r,
                 const Environment& env, bool wireframe3D, bool showGrid,
                 const std::vector<glm::mat4>& models,
                 const std::vector<glm::vec3>& colors, const std::vector<int>& meshIds,
                 int selectedIndex = -1, float gridSpacing = kGridStep,
                 const std::vector<SkinnedDrawItem>& skins = {});

// Ray vs an axis-aligned box [bmin,bmax] in the object's local space. Returns
// the entry distance (0 if inside), or -1 on a miss. The ray parameter t is the
// same in world and local space, so values compare across objects for picking.
float rayAabbT(glm::vec3 ro, glm::vec3 rd, const glm::mat4& model,
               glm::vec3 bmin, glm::vec3 bmax);
