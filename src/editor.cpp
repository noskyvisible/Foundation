#include "editor.h"

#include <glm/gtc/type_ptr.hpp>

#include "imgui.h"

bool drawViewportPanel(const char* name, RenderTarget& rt, const Camera& cam,
                       const Renderer& r, const Environment& env, bool wireframe3D, bool showGrid,
                       const std::vector<glm::mat4>& models, const std::vector<glm::vec3>& colors,
                       const std::vector<int>& meshIds,
                       int selectedIndex, float gridSpacing,
                       const std::vector<SkinnedDrawItem>& skins,
                       EditContext* edit) {
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

        if (edit && edit->scene && edit->selected) {
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
