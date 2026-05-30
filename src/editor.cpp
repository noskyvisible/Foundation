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
                       Terrain* terrain, TerrainBrush* brush) {
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
            // Gizmo for the selected object, clipped to this viewport's rect.
            if (sel >= 0 && sel < (int)scene.size()) {
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
                ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                                     edit->op, edit->mode, glm::value_ptr(scene[sel].transform),
                                     nullptr, snapPtr);
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
                *edit->selected = best;
            }
        }
    }
    ImGui::End();
    return imageHovered;
}
