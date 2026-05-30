#include "renderer.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "shaders.h"

#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

// Load an image file into a REPEAT-wrapped, mipmapped GL texture. Tries a few
// cwd-relative candidates (the exe may run from the project root or build/).
static GLuint loadTextureFile(const char* const* candidates, int n) {
    int w = 0, h = 0, ch = 0;
    unsigned char* px = nullptr;
    for (int i = 0; i < n && !px; ++i) px = stbi_load(candidates[i], &w, &h, &ch, 4);
    if (!px) { std::fprintf(stderr, "Could not load ground texture.\n"); return 0; }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    stbi_image_free(px);
    return tex;
}

void resizeTarget(RenderTarget& rt, int w, int h) {
    if (rt.fbo && w == rt.width && h == rt.height) return;
    rt.width = w;
    rt.height = h;

    if (!rt.fbo) glGenFramebuffers(1, &rt.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);

    if (!rt.color) glGenTextures(1, &rt.color);
    glBindTexture(GL_TEXTURE_2D, rt.color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt.color, 0);

    if (!rt.depth) glGenRenderbuffers(1, &rt.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, rt.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rt.depth);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void destroyTarget(RenderTarget& rt) {
    if (rt.fbo)   glDeleteFramebuffers(1, &rt.fbo);
    if (rt.color) glDeleteTextures(1, &rt.color);
    if (rt.depth) glDeleteRenderbuffers(1, &rt.depth);
    rt = RenderTarget{};
}

// ---- Reference grid: a line mesh (pos + colour) in the XY plane (z=0). ----
// Per-camera gridModel rotates it into each view's plane. Minor lines are dim,
// every 5th line brighter, and the two centre axes are tinted red/green.
void makeGrid(GLuint& vao, GLuint& vbo, int& outVertexCount) {
    const int   half = 10;          // lines from -10..+10
    const float step = kGridStep;   // base world units between lines
    const float ext  = half * step;
    const glm::vec3 minor(0.18f, 0.18f, 0.21f);
    const glm::vec3 major(0.30f, 0.30f, 0.36f);
    const glm::vec3 axisX(0.55f, 0.22f, 0.22f);  // line along X (y=0)
    const glm::vec3 axisY(0.22f, 0.55f, 0.28f);  // line along Y (x=0)

    std::vector<float> v;
    auto push = [&](float x, float y, glm::vec3 c) {
        v.insert(v.end(), {x, y, 0.0f, c.r, c.g, c.b});
    };
    for (int i = -half; i <= half; ++i) {
        float p = i * step;
        glm::vec3 cv = (i == 0) ? axisY : (i % 5 == 0 ? major : minor);
        push(p, -ext, cv); push(p, ext, cv);     // vertical line (const x)
        glm::vec3 ch = (i == 0) ? axisX : (i % 5 == 0 ? major : minor);
        push(-ext, p, ch); push(ext, p, ch);     // horizontal line (const y)
    }

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    outVertexCount = (int)(v.size() / 6);
}

// The 12 edges of a slightly-enlarged cube, all in a bright highlight colour --
// drawn around the selected object as selection feedback (GL_LINES, 24 verts).
void makeSelectionBox(GLuint& vao, GLuint& vbo, int& outVertexCount) {
    const float s = 0.5f;                        // unit box; scaled to the mesh AABB at draw
    const glm::vec3 hl(1.0f, 0.6f, 0.1f);        // orange
    const glm::vec3 c[8] = {
        {-s,-s,-s}, { s,-s,-s}, { s,-s, s}, {-s,-s, s},
        {-s, s,-s}, { s, s,-s}, { s, s, s}, {-s, s, s},
    };
    const int e[12][2] = {
        {0,1},{1,2},{2,3},{3,0},   // bottom
        {4,5},{5,6},{6,7},{7,4},   // top
        {0,4},{1,5},{2,6},{3,7},   // verticals
    };
    std::vector<float> v;
    for (auto& edge : e)
        for (int k = 0; k < 2; ++k) {
            glm::vec3 p = c[edge[k]];
            v.insert(v.end(), {p.x, p.y, p.z, hl.r, hl.g, hl.b});
        }

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    outVertexCount = (int)(v.size() / 6);
}

GPUMesh uploadMesh(const MeshAsset& asset) {
    GPUMesh gm;
    gm.bounds = asset.bounds;
    for (const SubMesh& s : asset.subs) {
        GPUSubMesh g;
        g.baseColor = s.baseColor;
        g.count = (GLsizei)s.indices.size();
        glGenVertexArrays(1, &g.vao);
        glGenBuffers(1, &g.vbo);
        glGenBuffers(1, &g.ebo);
        glBindVertexArray(g.vao);
        glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
        glBufferData(GL_ARRAY_BUFFER, s.vertices.size() * sizeof(Vertex), s.vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, s.indices.size() * sizeof(uint32_t), s.indices.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, pos));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
        glEnableVertexAttribArray(2);

        if (s.albedoW > 0 && !s.albedo.empty()) {
            glGenTextures(1, &g.tex);
            glBindTexture(GL_TEXTURE_2D, g.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s.albedoW, s.albedoH, 0, GL_RGBA, GL_UNSIGNED_BYTE, s.albedo.data());
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        gm.subs.push_back(g);
    }
    return gm;
}

void syncMeshes(Renderer& r, const std::vector<MeshAsset>& lib) {
    for (size_t i = r.meshes.size(); i < lib.size(); ++i)
        r.meshes.push_back(uploadMesh(lib[i]));
}

GPUSkinned uploadSkinnedMesh(const SkinnedMesh& sm) {
    GPUSkinned g;
    g.count     = (GLsizei)sm.indices.size();
    g.baseColor = sm.baseColor;
    g.bounds    = sm.bounds;
    g.importFix = sm.importFix;
    g.numBones  = (int)sm.boneOffsets.size();
    glGenVertexArrays(1, &g.vao);
    glGenBuffers(1, &g.vbo);
    glGenBuffers(1, &g.ebo);
    glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER, sm.vertices.size() * sizeof(SkinnedVertex), sm.vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sm.indices.size() * sizeof(uint32_t), sm.indices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SkinnedVertex), (void*)offsetof(SkinnedVertex, pos));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(SkinnedVertex), (void*)offsetof(SkinnedVertex, normal));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(SkinnedVertex), (void*)offsetof(SkinnedVertex, uv));
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(3, 4, GL_INT, sizeof(SkinnedVertex), (void*)offsetof(SkinnedVertex, boneIds));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(SkinnedVertex), (void*)offsetof(SkinnedVertex, weights));
    glEnableVertexAttribArray(4);
    glBindVertexArray(0);

    if (sm.albedoW > 0 && !sm.albedo.empty()) {
        glGenTextures(1, &g.tex);
        glBindTexture(GL_TEXTURE_2D, g.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sm.albedoW, sm.albedoH, 0, GL_RGBA, GL_UNSIGNED_BYTE, sm.albedo.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    return g;
}

void syncSkinned(Renderer& r, const std::vector<SkinnedMesh>& lib) {
    for (size_t i = r.skinned.size(); i < lib.size(); ++i)
        r.skinned.push_back(uploadSkinnedMesh(lib[i]));
}

void syncTerrain(Renderer& r, const Terrain& t) {
    if (!t.enabled || t.version == 0) return;          // nothing built yet
    if (r.terrainVersion == t.version) return;         // already up to date

    std::vector<float> verts;
    std::vector<unsigned int> indices;
    buildTerrainMesh(t, verts, indices);
    r.terrainIndexCount = (GLsizei)indices.size();

    glBindVertexArray(r.terrainVao);
    glBindBuffer(GL_ARRAY_BUFFER, r.terrainVbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r.terrainEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_DYNAMIC_DRAW);
    const GLsizei stride = 8 * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, r.terrainSplatTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.splatRes, t.splatRes, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, t.splat.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    r.terrainVersion = t.version;
}

void invalidateGPUMeshes(Renderer& r) {
    for (GPUMesh& m : r.meshes)
        for (GPUSubMesh& s : m.subs) {
            glDeleteVertexArrays(1, &s.vao);
            glDeleteBuffers(1, &s.vbo);
            glDeleteBuffers(1, &s.ebo);
            if (s.tex) glDeleteTextures(1, &s.tex);
        }
    for (GPUSkinned& s : r.skinned) {
        glDeleteVertexArrays(1, &s.vao);
        glDeleteBuffers(1, &s.vbo);
        glDeleteBuffers(1, &s.ebo);
        if (s.tex) glDeleteTextures(1, &s.tex);
    }
    r.meshes.clear();
    r.skinned.clear();
}

Renderer createRenderer() {
    Renderer r;
    r.lineProgram = createProgram(kLineVS, kLineFS);
    r.lineMvpLoc  = glGetUniformLocation(r.lineProgram, "uMVP");
    r.litProgram  = createProgram(kLitVS, kLitFS);
    r.litMvpLoc   = glGetUniformLocation(r.litProgram, "uMVP");
    r.litModelLoc = glGetUniformLocation(r.litProgram, "uModel");
    r.litColorLoc = glGetUniformLocation(r.litProgram, "uColor");
    r.litHasTexLoc = glGetUniformLocation(r.litProgram, "uHasTexture");
    r.litAlbedoLoc = glGetUniformLocation(r.litProgram, "uAlbedo");
    r.litLightDirLoc = glGetUniformLocation(r.litProgram, "uLightDir");
    r.litLightColLoc = glGetUniformLocation(r.litProgram, "uLightColor");
    r.litAmbientLoc  = glGetUniformLocation(r.litProgram, "uAmbient");
    r.litCamLoc      = glGetUniformLocation(r.litProgram, "uCamPos");
    r.litFogColLoc   = glGetUniformLocation(r.litProgram, "uFogColor");
    r.litFogDenLoc   = glGetUniformLocation(r.litProgram, "uFogDensity");
    r.litFogFalLoc   = glGetUniformLocation(r.litProgram, "uFogFalloff");

    // Skinned program shares the lit fragment shader (same uniforms + uBones).
    r.skinProgram     = createProgram(kSkinVS, kLitFS);
    r.skinMvpLoc      = glGetUniformLocation(r.skinProgram, "uMVP");
    r.skinModelLoc    = glGetUniformLocation(r.skinProgram, "uModel");
    r.skinColorLoc    = glGetUniformLocation(r.skinProgram, "uColor");
    r.skinHasTexLoc   = glGetUniformLocation(r.skinProgram, "uHasTexture");
    r.skinAlbedoLoc   = glGetUniformLocation(r.skinProgram, "uAlbedo");
    r.skinLightDirLoc = glGetUniformLocation(r.skinProgram, "uLightDir");
    r.skinLightColLoc = glGetUniformLocation(r.skinProgram, "uLightColor");
    r.skinAmbientLoc  = glGetUniformLocation(r.skinProgram, "uAmbient");
    r.skinBonesLoc    = glGetUniformLocation(r.skinProgram, "uBones");
    r.skinCamLoc      = glGetUniformLocation(r.skinProgram, "uCamPos");
    r.skinFogColLoc   = glGetUniformLocation(r.skinProgram, "uFogColor");
    r.skinFogDenLoc   = glGetUniformLocation(r.skinProgram, "uFogDensity");
    r.skinFogFalLoc   = glGetUniformLocation(r.skinProgram, "uFogFalloff");

    r.skyProgram    = createProgram(kSkyVS, kSkyFS);
    r.skyInvVPLoc   = glGetUniformLocation(r.skyProgram, "uInvViewProj");
    r.skyCamLoc     = glGetUniformLocation(r.skyProgram, "uCamPos");
    r.skySunLoc     = glGetUniformLocation(r.skyProgram, "uSunDir");
    r.skySunColLoc  = glGetUniformLocation(r.skyProgram, "uSunColor");
    r.skyMoonLoc    = glGetUniformLocation(r.skyProgram, "uMoonDir");
    r.skyTimeLoc    = glGetUniformLocation(r.skyProgram, "uTime");
    r.skyCloudLoc   = glGetUniformLocation(r.skyProgram, "uCloudCover");
    r.skyExposureLoc= glGetUniformLocation(r.skyProgram, "uExposure");
    r.skyHazeColLoc = glGetUniformLocation(r.skyProgram, "uHazeColor");
    r.skyHazeLoc    = glGetUniformLocation(r.skyProgram, "uHaze");
    r.skyLightningLoc = glGetUniformLocation(r.skyProgram, "uLightning");
    r.skyBoltAzLoc    = glGetUniformLocation(r.skyProgram, "uBoltAz");
    r.skyBoltSeedLoc  = glGetUniformLocation(r.skyProgram, "uBoltSeed");
    glGenVertexArrays(1, &r.skyVao);   // empty VAO for the no-VBO fullscreen triangle

    // Ground plane: a big camera-centered quad, sand-textured + fogged.
    r.groundProgram     = createProgram(kGroundVS, kGroundFS);
    r.groundVPLoc       = glGetUniformLocation(r.groundProgram, "uViewProj");
    r.groundCamLoc      = glGetUniformLocation(r.groundProgram, "uCamPos");
    r.groundHalfLoc     = glGetUniformLocation(r.groundProgram, "uHalfSize");
    r.groundUvLoc       = glGetUniformLocation(r.groundProgram, "uUvScale");
    r.groundLightDirLoc = glGetUniformLocation(r.groundProgram, "uLightDir");
    r.groundLightColLoc = glGetUniformLocation(r.groundProgram, "uLightColor");
    r.groundAmbientLoc  = glGetUniformLocation(r.groundProgram, "uAmbient");
    r.groundAlbedoLoc   = glGetUniformLocation(r.groundProgram, "uAlbedo");
    r.groundFogColLoc   = glGetUniformLocation(r.groundProgram, "uFogColor");
    r.groundFogDenLoc   = glGetUniformLocation(r.groundProgram, "uFogDensity");
    r.groundFogFalLoc   = glGetUniformLocation(r.groundProgram, "uFogFalloff");
    {
        const float quad[8] = { -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f,  1.0f, 1.0f };  // tri-strip
        glGenVertexArrays(1, &r.groundVao);
        glBindVertexArray(r.groundVao);
        glGenBuffers(1, &r.groundVbo);
        glBindBuffer(GL_ARRAY_BUFFER, r.groundVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glBindVertexArray(0);
        const char* sand[] = { "materials/sand.png", "../materials/sand.png", "sand.png" };
        r.groundTex = loadTextureFile(sand, 3);
    }

    // Heightmap terrain: splat-blended, lit, fogged. Mesh + splatmap are uploaded
    // lazily by syncTerrain(); here we just build the program, layer textures, and
    // empty GPU buffers.
    r.terrainProgram  = createProgram(kTerrainVS, kTerrainFS);
    r.terVPLoc        = glGetUniformLocation(r.terrainProgram, "uViewProj");
    r.terLightDirLoc  = glGetUniformLocation(r.terrainProgram, "uLightDir");
    r.terLightColLoc  = glGetUniformLocation(r.terrainProgram, "uLightColor");
    r.terAmbientLoc   = glGetUniformLocation(r.terrainProgram, "uAmbient");
    r.terSplatLoc     = glGetUniformLocation(r.terrainProgram, "uSplat");
    r.terTexLoc[0]    = glGetUniformLocation(r.terrainProgram, "uTex0");
    r.terTexLoc[1]    = glGetUniformLocation(r.terrainProgram, "uTex1");
    r.terTexLoc[2]    = glGetUniformLocation(r.terrainProgram, "uTex2");
    r.terTexLoc[3]    = glGetUniformLocation(r.terrainProgram, "uTex3");
    r.terTileLoc      = glGetUniformLocation(r.terrainProgram, "uTileScale");
    r.terCamLoc       = glGetUniformLocation(r.terrainProgram, "uCamPos");
    r.terFogColLoc    = glGetUniformLocation(r.terrainProgram, "uFogColor");
    r.terFogDenLoc    = glGetUniformLocation(r.terrainProgram, "uFogDensity");
    r.terFogFalLoc    = glGetUniformLocation(r.terrainProgram, "uFogFalloff");
    {
        const char* sand[] = { "materials/sand.png", "../materials/sand.png", "sand.png" };
        const char* dirt[] = { "materials/dirt.png", "../materials/dirt.png", "dirt.png" };
        const char* rock[] = { "materials/rock.png", "../materials/rock.png", "rock.png" };
        const char* flor[] = { "materials/floor.png", "../materials/floor.png", "floor.png" };
        r.terrainLayerTex[0] = loadTextureFile(sand, 3);
        r.terrainLayerTex[1] = loadTextureFile(dirt, 3);
        r.terrainLayerTex[2] = loadTextureFile(rock, 3);
        r.terrainLayerTex[3] = loadTextureFile(flor, 3);
        glGenTextures(1, &r.terrainSplatTex);  // filled by syncTerrain
        glGenVertexArrays(1, &r.terrainVao);
        glGenBuffers(1, &r.terrainVbo);
        glGenBuffers(1, &r.terrainEbo);
    }

    // Rain: world-space instanced streaks. A static buffer of random drop
    // positions is animated (fall + wrap) in the vertex shader; the box follows
    // the camera so rain always surrounds the viewer with correct parallax.
    r.rainProgram  = createProgram(kRainVS, kRainFS);
    r.rainVPLoc    = glGetUniformLocation(r.rainProgram, "uViewProj");
    r.rainCamLoc   = glGetUniformLocation(r.rainProgram, "uCamPos");
    r.rainDispLoc  = glGetUniformLocation(r.rainProgram, "uRainDisp");
    r.rainDirLoc   = glGetUniformLocation(r.rainProgram, "uRainDir");
    r.rainBoxLoc   = glGetUniformLocation(r.rainProgram, "uBox");
    r.rainLenLoc   = glGetUniformLocation(r.rainProgram, "uStreakLen");
    r.rainWidthLoc = glGetUniformLocation(r.rainProgram, "uStreakWidth");
    r.rainColorLoc = glGetUniformLocation(r.rainProgram, "uColor");
    r.rainIntenLoc = glGetUniformLocation(r.rainProgram, "uIntensity");
    {
        const int   N = 6000;           // max drops; drawn count scales with intensity
        const float R = 28.0f, H = 36.0f;
        r.rainDrops = N;
        std::vector<glm::vec3> base(N);
        unsigned s = 1234567u;
        auto rnd = [&]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                           return float(s & 0xFFFFFFu) / float(0x1000000); };
        for (int i = 0; i < N; ++i)
            base[i] = glm::vec3((rnd() * 2.0f - 1.0f) * R, rnd() * H, (rnd() * 2.0f - 1.0f) * R);
        const float quad[8] = { -1.0f, 0.0f,  1.0f, 0.0f,  -1.0f, 1.0f,  1.0f, 1.0f };  // tri-strip
        glGenVertexArrays(1, &r.rainVao);
        glBindVertexArray(r.rainVao);
        glGenBuffers(1, &r.rainQuadVbo);
        glBindBuffer(GL_ARRAY_BUFFER, r.rainQuadVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glGenBuffers(1, &r.rainInstVbo);
        glBindBuffer(GL_ARRAY_BUFFER, r.rainInstVbo);
        glBufferData(GL_ARRAY_BUFFER, N * sizeof(glm::vec3), base.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
        glVertexAttribDivisor(1, 1);
        glBindVertexArray(0);
    }

    makeGrid(r.gridVao, r.gridVbo, r.gridVertexCount);
    makeSelectionBox(r.selBoxVao, r.selBoxVbo, r.selBoxVertexCount);
    return r;
}

void destroyRenderer(Renderer& r) {
    for (GPUMesh& m : r.meshes)
        for (GPUSubMesh& s : m.subs) {
            glDeleteVertexArrays(1, &s.vao);
            glDeleteBuffers(1, &s.vbo);
            glDeleteBuffers(1, &s.ebo);
            if (s.tex) glDeleteTextures(1, &s.tex);
        }
    for (GPUSkinned& s : r.skinned) {
        glDeleteVertexArrays(1, &s.vao);
        glDeleteBuffers(1, &s.vbo);
        glDeleteBuffers(1, &s.ebo);
        if (s.tex) glDeleteTextures(1, &s.tex);
    }
    glDeleteVertexArrays(1, &r.gridVao);
    glDeleteVertexArrays(1, &r.selBoxVao);
    glDeleteVertexArrays(1, &r.skyVao);
    glDeleteBuffers(1, &r.gridVbo);
    glDeleteBuffers(1, &r.selBoxVbo);
    glDeleteProgram(r.lineProgram);
    glDeleteProgram(r.litProgram);
    glDeleteProgram(r.skinProgram);
    glDeleteProgram(r.skyProgram);
    glDeleteVertexArrays(1, &r.groundVao);
    glDeleteBuffers(1, &r.groundVbo);
    if (r.groundTex) glDeleteTextures(1, &r.groundTex);
    glDeleteProgram(r.groundProgram);
    glDeleteVertexArrays(1, &r.rainVao);
    glDeleteBuffers(1, &r.rainQuadVbo);
    glDeleteBuffers(1, &r.rainInstVbo);
    glDeleteProgram(r.rainProgram);
    glDeleteVertexArrays(1, &r.terrainVao);
    glDeleteBuffers(1, &r.terrainVbo);
    glDeleteBuffers(1, &r.terrainEbo);
    if (r.terrainSplatTex) glDeleteTextures(1, &r.terrainSplatTex);
    for (int i = 0; i < 4; ++i) if (r.terrainLayerTex[i]) glDeleteTextures(1, &r.terrainLayerTex[i]);
    glDeleteProgram(r.terrainProgram);
    r = Renderer{};
}

void renderScene(GLuint fbo, int width, int height, const Camera& cam,
                 const glm::mat4& view, const glm::mat4& proj, const Renderer& r,
                 const Environment& env, bool wireframe3D, bool showGrid,
                 const std::vector<glm::mat4>& models,
                 const std::vector<glm::vec3>& colors, const std::vector<int>& meshIds,
                 int selectedIndex, float gridSpacing,
                 const std::vector<SkinnedDrawItem>& skins) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    glEnable(GL_DEPTH_TEST);
    if (cam.type == Projection::Ortho) glClearColor(0.06f, 0.06f, 0.07f, 1.0f);
    else                               glClearColor(0.11f, 0.11f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Procedural sky behind everything (perspective views only).
    if (cam.type == Projection::Perspective && r.skyProgram) {
        glDepthMask(GL_FALSE);
        glDisable(GL_DEPTH_TEST);
        glUseProgram(r.skyProgram);
        glUniformMatrix4fv(r.skyInvVPLoc, 1, GL_FALSE, glm::value_ptr(glm::inverse(proj * view)));
        glUniform3fv(r.skyCamLoc, 1, glm::value_ptr(cam.eye));
        glUniform3fv(r.skySunLoc, 1, glm::value_ptr(env.sunDir));
        glUniform3fv(r.skySunColLoc, 1, glm::value_ptr(env.sunColor));
        glUniform3fv(r.skyMoonLoc, 1, glm::value_ptr(env.moonDir));
        glUniform1f(r.skyTimeLoc, env.windTime);
        glUniform1f(r.skyCloudLoc, env.cloudCover);
        glUniform1f(r.skyExposureLoc, env.skyExposure);
        glUniform3fv(r.skyHazeColLoc, 1, glm::value_ptr(env.fogColor));
        glUniform1f(r.skyHazeLoc, glm::clamp((env.fogDensity + env.weather.fogDensity) * 18.0f, 0.25f, 0.9f));
        glUniform1f(r.skyLightningLoc, env.weather.lightning);
        glUniform1f(r.skyBoltAzLoc, env.weather.boltAz);
        glUniform1f(r.skyBoltSeedLoc, env.weather.boltSeed);
        glBindVertexArray(r.skyVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
    }

    // Textured ground plane (perspective only): a big camera-centered quad at
    // y=0, lit like the scene and fogged so its far edge melts into the horizon.
    if (cam.type == Projection::Perspective && r.groundProgram && r.groundTex) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glUseProgram(r.groundProgram);
        glUniformMatrix4fv(r.groundVPLoc, 1, GL_FALSE, glm::value_ptr(proj * view));
        glUniform3fv(r.groundCamLoc, 1, glm::value_ptr(cam.eye));
        glUniform1f(r.groundHalfLoc, 4000.0f);
        glUniform1f(r.groundUvLoc, 0.25f);                // tile every ~4 units
        glUniform3fv(r.groundLightDirLoc, 1, glm::value_ptr(env.sunDir));
        glUniform3fv(r.groundLightColLoc, 1, glm::value_ptr(env.lightColor));
        glUniform3fv(r.groundAmbientLoc, 1, glm::value_ptr(env.ambient));
        glUniform3fv(r.groundFogColLoc, 1, glm::value_ptr(env.fogColor));
        glUniform1f(r.groundFogDenLoc, env.fogDensity + env.weather.fogDensity);
        glUniform1f(r.groundFogFalLoc, env.fogFalloff);
        glUniform1i(r.groundAlbedoLoc, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, r.groundTex);
        glBindVertexArray(r.groundVao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // Heightmap terrain: splat-blended, lit, fogged (perspective views only, like
    // the ground). Sits on top of the flat ground within its block.
    if (cam.type == Projection::Perspective && r.terrainProgram && r.terrainIndexCount > 0) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glUseProgram(r.terrainProgram);
        glUniformMatrix4fv(r.terVPLoc, 1, GL_FALSE, glm::value_ptr(proj * view));
        glUniform3fv(r.terLightDirLoc, 1, glm::value_ptr(env.sunDir));
        glUniform3fv(r.terLightColLoc, 1, glm::value_ptr(env.lightColor));
        glUniform3fv(r.terAmbientLoc, 1, glm::value_ptr(env.ambient));
        glUniform1f(r.terTileLoc, 0.25f);                 // tile every ~4 units
        glUniform3fv(r.terCamLoc, 1, glm::value_ptr(cam.eye));
        glUniform3fv(r.terFogColLoc, 1, glm::value_ptr(env.fogColor));
        glUniform1f(r.terFogDenLoc, env.fogDensity + env.weather.fogDensity);
        glUniform1f(r.terFogFalLoc, env.fogFalloff);
        glUniform1i(r.terSplatLoc, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, r.terrainSplatTex);
        for (int i = 0; i < 4; ++i) {
            glUniform1i(r.terTexLoc[i], 1 + i);
            glActiveTexture(GL_TEXTURE1 + i);
            glBindTexture(GL_TEXTURE_2D, r.terrainLayerTex[i]);
        }
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(r.terrainVao);
        glDrawElements(GL_TRIANGLES, r.terrainIndexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }

    // Grid (unlit lines), oriented into this view's plane and scaled to the
    // requested grid size.
    if (showGrid && r.gridVao) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glUseProgram(r.lineProgram);
        glm::mat4 gridM = cam.gridModel * glm::scale(glm::mat4(1.0f), glm::vec3(gridSpacing / kGridStep));
        glm::mat4 gridMVP = proj * view * gridM;
        glUniformMatrix4fv(r.lineMvpLoc, 1, GL_FALSE, glm::value_ptr(gridMVP));
        glBindVertexArray(r.gridVao);
        glDrawArrays(GL_LINES, 0, r.gridVertexCount);
    }

    // Objects: lit, per-object colour (tint) * submesh base colour. Wireframe in ortho.
    bool wire = (cam.type == Projection::Ortho) || wireframe3D;
    glPolygonMode(GL_FRONT_AND_BACK, wire ? GL_LINE : GL_FILL);
    glUseProgram(r.litProgram);
    glUniform1i(r.litAlbedoLoc, 0);   // albedo sampler -> texture unit 0
    glUniform3fv(r.litLightDirLoc, 1, glm::value_ptr(env.sunDir));
    glUniform3fv(r.litLightColLoc, 1, glm::value_ptr(env.lightColor));
    glUniform3fv(r.litAmbientLoc, 1, glm::value_ptr(env.ambient));
    glUniform3fv(r.litCamLoc, 1, glm::value_ptr(cam.eye));
    glUniform3fv(r.litFogColLoc, 1, glm::value_ptr(env.fogColor));
    glUniform1f(r.litFogDenLoc, env.fogDensity + env.weather.fogDensity);
    glUniform1f(r.litFogFalLoc, env.fogFalloff);
    for (size_t i = 0; i < models.size(); ++i) {
        int mid = i < meshIds.size() ? meshIds[i] : 0;
        if (mid < 0 || mid >= (int)r.meshes.size()) continue;
        glm::vec3 tint = i < colors.size() ? colors[i] : glm::vec3(1.0f);
        glUniformMatrix4fv(r.litMvpLoc, 1, GL_FALSE, glm::value_ptr(proj * view * models[i]));
        glUniformMatrix4fv(r.litModelLoc, 1, GL_FALSE, glm::value_ptr(models[i]));
        for (const GPUSubMesh& s : r.meshes[mid].subs) {
            glm::vec3 c = s.baseColor * tint;
            glUniform3fv(r.litColorLoc, 1, glm::value_ptr(c));
            // Skip the texture in wireframe so the lines are plain, not textured.
            bool useTex = !wire && s.tex;
            glUniform1i(r.litHasTexLoc, useTex ? 1 : 0);
            if (useTex) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s.tex); }
            glBindVertexArray(s.vao);
            glDrawElements(GL_TRIANGLES, s.count, GL_UNSIGNED_INT, 0);
        }
    }

    // Skinned objects: same shading model, deformed by a per-object bone palette
    // uploaded as uBones[]. The vertex IDs/weights live in the VAO.
    if (!skins.empty() && r.skinProgram) {
        static const std::vector<glm::mat4> kIdentity(64, glm::mat4(1.0f));
        glPolygonMode(GL_FRONT_AND_BACK, wire ? GL_LINE : GL_FILL);
        glUseProgram(r.skinProgram);
        glUniform1i(r.skinAlbedoLoc, 0);
        glUniform3fv(r.skinLightDirLoc, 1, glm::value_ptr(env.sunDir));
        glUniform3fv(r.skinLightColLoc, 1, glm::value_ptr(env.lightColor));
        glUniform3fv(r.skinAmbientLoc, 1, glm::value_ptr(env.ambient));
        glUniform3fv(r.skinCamLoc, 1, glm::value_ptr(cam.eye));
        glUniform3fv(r.skinFogColLoc, 1, glm::value_ptr(env.fogColor));
        glUniform1f(r.skinFogDenLoc, env.fogDensity + env.weather.fogDensity);
        glUniform1f(r.skinFogFalLoc, env.fogFalloff);
        for (const SkinnedDrawItem& it : skins) {
            if (it.skinnedId < 0 || it.skinnedId >= (int)r.skinned.size()) continue;
            const GPUSkinned& g = r.skinned[it.skinnedId];
            glm::mat4 m = it.model * g.importFix;
            glUniformMatrix4fv(r.skinMvpLoc, 1, GL_FALSE, glm::value_ptr(proj * view * m));
            glUniformMatrix4fv(r.skinModelLoc, 1, GL_FALSE, glm::value_ptr(m));
            const std::vector<glm::mat4>& pal = (it.palette && !it.palette->empty()) ? *it.palette : kIdentity;
            int n = std::min((int)pal.size(), 64);
            glUniformMatrix4fv(r.skinBonesLoc, n, GL_FALSE, glm::value_ptr(pal[0]));
            glm::vec3 c = g.baseColor * it.color;
            glUniform3fv(r.skinColorLoc, 1, glm::value_ptr(c));
            bool useTex = !wire && g.tex;
            glUniform1i(r.skinHasTexLoc, useTex ? 1 : 0);
            if (useTex) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g.tex); }
            glBindVertexArray(g.vao);
            glDrawElements(GL_TRIANGLES, g.count, GL_UNSIGNED_INT, 0);
        }
    }

    // Selection highlight: an orange wireframe box scaled to the object's AABB.
    // The selected object may be static (meshIds) or skinned (skins).
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    if (selectedIndex >= 0 && r.selBoxVao) {
        bool found = false;
        AABB b{ glm::vec3(-0.5f), glm::vec3(0.5f) };
        glm::mat4 boxModel(1.0f);
        for (const SkinnedDrawItem& it : skins)
            if (it.sceneIndex == selectedIndex && it.skinnedId >= 0 && it.skinnedId < (int)r.skinned.size()) {
                b = r.skinned[it.skinnedId].bounds;
                boxModel = it.model * r.skinned[it.skinnedId].importFix;
                found = true; break;
            }
        if (!found && selectedIndex < (int)models.size()) {
            int mid = selectedIndex < (int)meshIds.size() ? meshIds[selectedIndex] : 0;
            if (mid >= 0 && mid < (int)r.meshes.size()) {
                b = r.meshes[mid].bounds; boxModel = models[selectedIndex]; found = true;
            }
        }
        if (found) {
            glm::vec3 center = 0.5f * (b.min + b.max);
            glm::vec3 size   = glm::max(b.max - b.min, glm::vec3(1e-3f)) * 1.03f;  // small margin
            glm::mat4 boxM = boxModel * glm::translate(glm::mat4(1.0f), center) * glm::scale(glm::mat4(1.0f), size);
            glUseProgram(r.lineProgram);
            glUniformMatrix4fv(r.lineMvpLoc, 1, GL_FALSE, glm::value_ptr(proj * view * boxM));
            glBindVertexArray(r.selBoxVao);
            glDrawArrays(GL_LINES, 0, r.selBoxVertexCount);
        }
    }

    // Rain (perspective only): world-space instanced streaks, depth-tested so
    // scene geometry occludes them, alpha-blended, no depth write. The drawn
    // instance count scales with intensity, and the pass is skipped when dry.
    if (cam.type == Projection::Perspective && r.rainProgram && env.weather.rain > 0.001f) {
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(r.rainProgram);
        glUniformMatrix4fv(r.rainVPLoc, 1, GL_FALSE, glm::value_ptr(proj * view));
        glUniform3fv(r.rainCamLoc, 1, glm::value_ptr(cam.eye));
        glUniform3fv(r.rainDispLoc, 1, glm::value_ptr(env.weather.rainDisp));  // integrated motion
        glUniform3fv(r.rainDirLoc, 1, glm::value_ptr(env.weather.rainDir));    // travel direction
        glUniform3f(r.rainBoxLoc, 28.0f, 36.0f, 28.0f);
        glUniform1f(r.rainLenLoc, 1.5f);
        glUniform1f(r.rainWidthLoc, 0.04f);  // streak half-width in world units
        glUniform3f(r.rainColorLoc, 0.80f, 0.84f, 0.92f);
        glUniform1f(r.rainIntenLoc, env.weather.rain);
        int count = (int)(r.rainDrops * glm::clamp(env.weather.rain, 0.0f, 1.0f));
        if (count > 0) {
            glBindVertexArray(r.rainVao);
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, count);
            glBindVertexArray(0);
        }
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

float rayAabbT(glm::vec3 ro, glm::vec3 rd, const glm::mat4& model,
               glm::vec3 bmin, glm::vec3 bmax) {
    glm::mat4 inv = glm::inverse(model);
    glm::vec3 o = glm::vec3(inv * glm::vec4(ro, 1.0f));
    glm::vec3 d = glm::vec3(inv * glm::vec4(rd, 0.0f));
    float tmin = -1e30f, tmax = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-8f) {
            if (o[i] < bmin[i] || o[i] > bmax[i]) return -1.0f;
        } else {
            float t1 = (bmin[i] - o[i]) / d[i];
            float t2 = (bmax[i] - o[i]) / d[i];
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            tmin = tmin > t1 ? tmin : t1;
            tmax = tmax < t2 ? tmax : t2;
            if (tmin > tmax) return -1.0f;
        }
    }
    if (tmax < 0.0f) return -1.0f;
    return tmin >= 0.0f ? tmin : 0.0f;
}
