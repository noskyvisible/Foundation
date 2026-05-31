#include "editor.h"

#include <glm/gtc/type_ptr.hpp>

#include "imgui.h"

bool drawViewportPanel(const char* name, RenderTarget& rt, const Camera& cam,
                       const Renderer& r, const Environment& env, bool wireframe3D, bool showGrid,
                       const std::vector<glm::mat4>& models, const std::vector<glm::vec3>& colors,
                       const std::vector<int>& meshIds,
                       int selectedIndex, float gridSpacing,
                       const std::vector<SkinnedDrawItem>& skins,
                       EditContext* edit,
                       Terrain* terrain, TerrainBrush* brush,
                       PlayerStart* playerStart) {
    bool imageHovered = false;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(name);
    ImGui::PopStyleVar();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    int w = (int)avail.x, h = (int)avail.y;
    if (w > 0 && h > 0) {
        float aspect = h == 0 ? 1.0f : (float)w / (float)h;
        glm::mat4 view = viewMatrix(cam);
        glm::mat4 proj = projMatrix(cam, aspect);

        resizeTarget(rt, w, h);
        renderScene(rt.fbo, rt.width, rt.height, cam, view, proj, r, env, wireframe3D, showGrid, models, colors, meshIds, selectedIndex, gridSpacing, skins);
        // Flip V (uv 0,1 -> 1,0): GL textures have origin at bottom-left.
        ImGui::Image((ImTextureID)(intptr_t)rt.color, avail, ImVec2(0, 1), ImVec2(1, 0));
        imageHovered = ImGui::IsItemHovered();

        ImVec2 p0 = ImGui::GetItemRectMin();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(p0.x + 6, p0.y + 4), IM_COL32(210, 210, 220, 255), cam.label);

        // Terrain sculpting (perspective view, brush active): show a brush cursor
        // ring draped on the terrain under the mouse, and on LMB-drag apply the
        // brush. Takes over from object picking/selection while active.
        bool sculpting = (brush && brush->active && terrain && cam.type == Projection::Perspective);
        if (sculpting && imageHovered && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()) {
            ImVec2 m = ImGui::GetIO().MousePos;
            float nx = 2.0f * (m.x - p0.x) / (float)w - 1.0f;
            float ny = 1.0f - 2.0f * (m.y - p0.y) / (float)h;
            glm::mat4 invVP = glm::inverse(proj * view);
            glm::vec4 pn = invVP * glm::vec4(nx, ny, -1.0f, 1.0f); pn /= pn.w;
            glm::vec4 pf = invVP * glm::vec4(nx, ny,  1.0f, 1.0f); pf /= pf.w;
            glm::vec3 ro = glm::vec3(pn);
            glm::vec3 rd = glm::normalize(glm::vec3(pf) - glm::vec3(pn));
            glm::vec3 hit;
            if (raycastTerrain(*terrain, ro, rd, hit)) {
                // Project a world point to viewport pixels (image V is flipped).
                auto toScreen = [&](glm::vec3 wp, ImVec2& out) -> bool {
                    glm::vec4 c = proj * view * glm::vec4(wp, 1.0f);
                    if (c.w <= 1e-4f) return false;
                    out = ImVec2(p0.x + (c.x / c.w * 0.5f + 0.5f) * (float)w,
                                 p0.y + (1.0f - (c.y / c.w * 0.5f + 0.5f)) * (float)h);
                    return true;
                };
                // Brush ring, draped on the heightfield so size + location read true.
                const int SEG = 48;
                ImVec2 prev; bool havePrev = false;
                ImU32 col = IM_COL32(255, 220, 80, 220);
                for (int s = 0; s <= SEG; ++s) {
                    float a = (float)s / SEG * 6.2831853f;
                    float px = hit.x + cosf(a) * brush->radius;
                    float pz = hit.z + sinf(a) * brush->radius;
                    float py = terrain->heightAt(px, pz) + 0.1f;
                    ImVec2 sp;
                    if (toScreen(glm::vec3(px, py, pz), sp)) {
                        if (havePrev) dl->AddLine(prev, sp, col, 2.0f);
                        prev = sp; havePrev = true;
                    } else havePrev = false;
                }
                ImVec2 cc;
                if (toScreen(hit + glm::vec3(0.0f, 0.1f, 0.0f), cc))
                    dl->AddCircleFilled(cc, 3.5f, col);

                if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                    applyTerrainBrush(*terrain, hit, *brush, ImGui::GetIO().DeltaTime);
            }
        }

        if (edit && edit->scene && edit->selected && !sculpting) {
            std::vector<SceneObject>& scene = *edit->scene;
            int sel = *edit->selected;
            // Gizmo for the selected object (or the player start, sel == -2),
            // clipped to this viewport's rect.
            bool gizmoObj    = (sel >= 0 && sel < (int)scene.size());
            bool gizmoPlayer = (sel == -2 && playerStart);
            if (gizmoObj || gizmoPlayer) {
                ImGuizmo::SetOrthographic(cam.type == Projection::Ortho);
                ImGuizmo::SetDrawlist();
                ImGuizmo::SetRect(p0.x, p0.y, (float)w, (float)h);
                float snapVal[3] = {0, 0, 0};
                const float* snapPtr = nullptr;
                if (edit->snap) {
                    if (edit->op == ImGuizmo::TRANSLATE)   { snapVal[0] = snapVal[1] = snapVal[2] = edit->snapTranslate; }
                    else if (edit->op == ImGuizmo::ROTATE) { snapVal[0] = edit->snapRotate; }
                    else                                   { snapVal[0] = edit->snapScale; }
                    snapPtr = snapVal;
                }
                if (gizmoObj) {
                    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                                         edit->op, edit->mode, glm::value_ptr(scene[sel].transform),
                                         nullptr, snapPtr);
                } else {
                    // Player start: gizmo a translate+yaw matrix, then read it back.
                    glm::mat4 pm = glm::translate(glm::mat4(1.0f), playerStart->pos)
                                 * glm::rotate(glm::mat4(1.0f), playerStart->yaw, glm::vec3(0, 1, 0));
                    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                                         edit->op, edit->mode, glm::value_ptr(pm), nullptr, snapPtr);
                    float t[3], r[3], s[3];
                    ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(pm), t, r, s);
                    playerStart->pos = glm::vec3(t[0], t[1], t[2]);
                    playerStart->yaw = glm::radians(r[1]);
                }
            }
            // Click-to-select: nearest object hit by the ray, unless over the gizmo.
            if (imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()) {
                ImVec2 m = ImGui::GetIO().MousePos;
                float nx = 2.0f * (m.x - p0.x) / (float)w - 1.0f;
                float ny = 1.0f - 2.0f * (m.y - p0.y) / (float)h;
                glm::mat4 invVP = glm::inverse(proj * view);
                glm::vec4 pn = invVP * glm::vec4(nx, ny, -1.0f, 1.0f); pn /= pn.w;
                glm::vec4 pf = invVP * glm::vec4(nx, ny,  1.0f, 1.0f); pf /= pf.w;
                glm::vec3 ro = glm::vec3(pn);
                glm::vec3 rd = glm::normalize(glm::vec3(pf) - glm::vec3(pn));
                int best = -1; float bestT = 1e30f;
                for (int i = 0; i < (int)scene.size(); ++i) {
                    glm::vec3 bmin(-0.5f), bmax(0.5f);
                    glm::mat4 pickModel = scene[i].transform;
                    if (scene[i].skinnedId >= 0 && edit->skinnedLib
                        && scene[i].skinnedId < (int)edit->skinnedLib->size()) {
                        // Skinned: box is in bind space, displayed via importFix.
                        const SkinnedMesh& sm = (*edit->skinnedLib)[scene[i].skinnedId];
                        bmin = sm.bounds.min; bmax = sm.bounds.max;
                        pickModel = scene[i].transform * sm.importFix;
                    } else if (edit->lib) {
                        int mid = scene[i].meshId;
                        if (mid >= 0 && mid < (int)edit->lib->size()) {
                            bmin = (*edit->lib)[mid].bounds.min;
                            bmax = (*edit->lib)[mid].bounds.max;
                        }
                    }
                    float t = rayAabbT(ro, rd, pickModel, bmin, bmax);
                    if (t >= 0.0f && t < bestT) { bestT = t; best = i; }
                }
                // The player start is also pickable (selection index -2).
                if (playerStart) {
                    glm::vec3 pmin = playerStart->pos + glm::vec3(-playerStart->radius, 0.0f, -playerStart->radius);
                    glm::vec3 pmax = playerStart->pos + glm::vec3(playerStart->radius, playerStart->eyeHeight + 0.2f, playerStart->radius);
                    float t = rayAabbT(ro, rd, glm::mat4(1.0f), pmin, pmax);
                    if (t >= 0.0f && t < bestT) { bestT = t; best = -2; }
                }
                *edit->selected = best;
            }
        }

        // Capsule colliders: a wireframe capsule (two rings + a rounded hemisphere
        // cap at each end + body lines) drawn around the player start and every
        // NPC, so colliders read as capsules rather than boxes.
        auto toS = [&](glm::vec3 wp, ImVec2& out) -> bool {
            glm::vec4 c = proj * view * glm::vec4(wp, 1.0f);
            if (c.w <= 1e-4f) return false;
            out = ImVec2(p0.x + (c.x / c.w * 0.5f + 0.5f) * (float)w,
                         p0.y + (1.0f - (c.y / c.w * 0.5f + 0.5f)) * (float)h);
            return true;
        };
        auto drawCapsule = [&](glm::vec3 base, float r, float total, ImU32 col) {
            r = std::max(r, 0.02f);
            total = std::max(total, 2.0f * r);
            float yBot = r, yTop = total - r;            // cap centres
            auto ring = [&](float cy) {
                ImVec2 prev; bool have = false;
                for (int i = 0; i <= 24; ++i) {
                    float a = i / 24.0f * 6.2831853f;
                    ImVec2 s; bool ok = toS(base + glm::vec3(cosf(a) * r, cy, sinf(a) * r), s);
                    if (ok && have) dl->AddLine(prev, s, col, 1.5f);
                    prev = s; have = ok;
                }
            };
            auto arc = [&](glm::vec3 c, glm::vec3 u, glm::vec3 v) {   // semicircle u->-u, bulge v
                ImVec2 prev; bool have = false;
                for (int i = 0; i <= 16; ++i) {
                    float a = i / 16.0f * 3.14159265f;
                    ImVec2 s; bool ok = toS(c + u * (cosf(a) * r) + v * (sinf(a) * r), s);
                    if (ok && have) dl->AddLine(prev, s, col, 1.5f);
                    prev = s; have = ok;
                }
            };
            ring(yBot); ring(yTop);
            glm::vec3 cb = base + glm::vec3(0, yBot, 0), ct = base + glm::vec3(0, yTop, 0);
            arc(cb, glm::vec3(1, 0, 0), glm::vec3(0, -1, 0));
            arc(cb, glm::vec3(0, 0, 1), glm::vec3(0, -1, 0));
            arc(ct, glm::vec3(1, 0, 0), glm::vec3(0,  1, 0));
            arc(ct, glm::vec3(0, 0, 1), glm::vec3(0,  1, 0));
            const glm::vec3 card[4] = {{r,0,0},{-r,0,0},{0,0,r},{0,0,-r}};
            for (const glm::vec3& d : card) {
                ImVec2 a, b;
                if (toS(base + glm::vec3(d.x, yBot, d.z), a) && toS(base + glm::vec3(d.x, yTop, d.z), b))
                    dl->AddLine(a, b, col, 1.5f);
            }
        };

        // NPC capsules, fitted to each NPC's bounds (feet on the ground).
        if (edit && edit->scene) {
            const std::vector<SceneObject>& sc = *edit->scene;
            for (int i = 0; i < (int)sc.size(); ++i) {
                const SceneObject& o = sc[i];
                if (o.npcTemplate < 0) continue;
                glm::vec3 bmin(-0.5f), bmax(0.5f);
                glm::mat4 model = o.transform;
                if (o.skinnedId >= 0 && edit->skinnedLib && o.skinnedId < (int)edit->skinnedLib->size()) {
                    const SkinnedMesh& sm = (*edit->skinnedLib)[o.skinnedId];
                    bmin = sm.bounds.min; bmax = sm.bounds.max; model = o.transform * sm.importFix;
                } else if (edit->lib && o.meshId >= 0 && o.meshId < (int)edit->lib->size()) {
                    bmin = (*edit->lib)[o.meshId].bounds.min; bmax = (*edit->lib)[o.meshId].bounds.max;
                }
                glm::vec3 mn(1e30f), mx(-1e30f);
                for (int c = 0; c < 8; ++c) {
                    glm::vec3 cor((c & 1) ? bmax.x : bmin.x, (c & 2) ? bmax.y : bmin.y, (c & 4) ? bmax.z : bmin.z);
                    glm::vec3 wp = glm::vec3(model * glm::vec4(cor, 1.0f));
                    mn = glm::min(mn, wp); mx = glm::max(mx, wp);
                }
                float hgt = mx.y - mn.y;
                float rad = std::min(0.5f * std::max(mx.x - mn.x, mx.z - mn.z), 0.30f * hgt);
                glm::vec3 feet((mn.x + mx.x) * 0.5f, mn.y, (mn.z + mx.z) * 0.5f);
                ImU32 ncol = (selectedIndex == i) ? IM_COL32(255, 210, 60, 255)   // selected
                                                  : IM_COL32(120, 235, 140, 210);
                drawCapsule(feet, rad, hgt, ncol);
            }
        }

        // Player-start capsule + forward arrow.
        if (playerStart) {
            float r   = std::max(playerStart->radius, 0.1f);
            float top = std::max(playerStart->eyeHeight + 0.2f, 2.0f * r);
            ImU32 col = (selectedIndex == -2) ? IM_COL32(255, 210, 60, 255)
                                              : IM_COL32(80, 220, 255, 235);
            drawCapsule(playerStart->pos, r, top, col);
            glm::vec3 fwd = forwardFromYawPitch(playerStart->yaw, 0.0f);
            glm::vec3 rt  = glm::normalize(glm::cross(fwd, glm::vec3(0, 1, 0)));
            glm::vec3 a0 = playerStart->pos + glm::vec3(0, top * 0.5f, 0), a1 = a0 + fwd * (r + 0.8f);
            ImVec2 s0, s1, hh;
            if (toS(a0, s0) && toS(a1, s1)) {
                dl->AddLine(s0, s1, col, 2.0f);
                if (toS(a1 - fwd * 0.3f + rt * 0.18f, hh)) dl->AddLine(s1, hh, col, 2.0f);
                if (toS(a1 - fwd * 0.3f - rt * 0.18f, hh)) dl->AddLine(s1, hh, col, 2.0f);
            }
        }
    }
    ImGui::End();
    return imageHovered;
}
