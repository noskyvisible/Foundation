#include "camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

glm::vec3 forwardFromYawPitch(float yaw, float pitch) {
    return glm::vec3(std::cos(pitch) * std::sin(yaw),
                     std::sin(pitch),
                    -std::cos(pitch) * std::cos(yaw));
}

glm::mat4 viewMatrix(const Camera& c) {
    return glm::lookAt(c.eye, c.target, c.up);
}

glm::mat4 projMatrix(const Camera& c, float aspect) {
    if (c.type == Projection::Perspective)
        return glm::perspective(glm::radians(c.fovDeg), aspect, 0.1f, 100.0f);
    float h = c.orthoHalfHeight;
    float w = h * aspect;
    return glm::ortho(-w, w, -h, h, -100.0f, 100.0f);
}
