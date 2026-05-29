#pragma once
#include <glm/glm.hpp>

// ---- Camera: bundles where we look from and how we project ---------------
enum class Projection { Perspective, Ortho };

struct Camera {
    Projection type = Projection::Perspective;
    glm::vec3 eye{0, 0, 3};
    glm::vec3 target{0, 0, 0};
    glm::vec3 up{0, 1, 0};
    float fovDeg = 45.0f;           // perspective only
    float orthoHalfHeight = 1.3f;   // ortho only: half view height in world units
    glm::mat4 gridModel{1.0f};      // orients the grid mesh into this view's plane
    const char* label = "";         // shown in the panel corner
    // Free-fly navigation state (perspective): eye/target are derived from these.
    glm::vec3 flyPos{0.0f};
    float yaw = 0.0f, pitch = 0.0f, flySpeed = 4.0f;
};

// FPS-style forward vector from yaw (around Y) and pitch (around X).
glm::vec3 forwardFromYawPitch(float yaw, float pitch);

glm::mat4 viewMatrix(const Camera& c);
glm::mat4 projMatrix(const Camera& c, float aspect);
