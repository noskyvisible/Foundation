#include "renderer.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "shaders.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

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

    r.skyProgram    = createProgram(kSkyVS, kSkyFS);
    r.skyInvVPLoc   = glGetUniformLocation(r.skyProgram, "uInvViewProj");
    r.skyCamLoc     = glGetUniformLocation(r.skyProgram, "uCamPos");
    r.skySunLoc     = glGetUniformLocation(r.skyProgram, "uSunDir");
    r.skySunColLoc  = glGetUniformLocation(r.skyProgram, "uSunColor");
    r.skyMoonLoc    = glGetUniformLocation(r.skyProgram, "uMoonDir");
    r.skyTimeLoc    = glGetUniformLocation(r.skyProgram, "uTime");
    r.skyCloudLoc   = glGetUniformLocation(r.skyProgram, "uCloudCover");
    r.skyExposureLoc= glGetUniformLocation(r.skyProgram, "uExposure");
    glGenVertexArrays(1, &r.skyVao);   // empty VAO for the no-VBO fullscreen triangle

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
        glUniform1f(r.skyExposureLoc, env.exposure);
        glBindVertexArray(r.skyVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
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
